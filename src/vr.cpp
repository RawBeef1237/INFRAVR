#include "vr.h"
#include "config.h"
#include "game.h"
#include "hooks.h"

#include <algorithm>
#include <cmath>

namespace ivr {

VRMod g_Mod;

namespace {

constexpr float kUnitsPerMetre = 39.37f;
constexpr float kMaxResidual = 30.0f;  // game units the view may lead the body (roomscale)
constexpr float kTeleportDist = 40.0f; // body moves further than this per frame = teleport

Vector Flat(const Vector &v) { return {v.x, v.y, 0.0f}; }

Vector YawForward(float yaw) { return {std::cos(Deg2Rad(yaw)), std::sin(Deg2Rad(yaw)), 0.0f}; }
Vector YawRight(float yaw) { return {std::sin(Deg2Rad(yaw)), -std::cos(Deg2Rad(yaw)), 0.0f}; }

QAngle ClampForCmd(QAngle a)
{
    a.x = std::max(-89.0f, std::min(89.0f, a.x));
    a.y = NormalizeYaw(a.y);
    a.z = 0.0f;
    return a;
}

} // namespace

bool VRMod::Init()
{
    m_scale = kUnitsPerMetre * g_Config.worldScale;
    if (!g_VR.Ok())
        return false;
    if (vr::IVROverlay *ov = g_VR.m_overlay)
    {
        if (ov->CreateOverlay("infravr.menu", "INFRA", &menuOverlay) != vr::VROverlayError_None)
            IVR_ERR("CreateOverlay(menu) failed");
        else
        {
            // Flat screen; clicks come from our own pointer (see ProcessMenuInput), not from
            // SteamVR's overlay laser, which takes the controllers away from the game.
            ov->SetOverlayWidthInMeters(menuOverlay, g_Config.menuWidth);
        }
        if (ov->CreateOverlay("infravr.cursor", "INFRA pointer", &cursorOverlay) == vr::VROverlayError_None)
        {
            static uint8_t dot[32 * 32 * 4];
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                {
                    float r = std::sqrt((x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f));
                    uint8_t *px = dot + (y * 32 + x) * 4;
                    uint8_t c = r < 9.0f ? 255 : 25;
                    px[0] = px[1] = px[2] = c;
                    px[3] = r < 13.0f ? 255 : 0;
                }
            ov->SetOverlayRaw(cursorOverlay, dot, 32, 32, 4);
            ov->SetOverlayWidthInMeters(cursorOverlay, 0.035f);
            ov->SetOverlaySortOrder(cursorOverlay, 3);
        }
        if (ov->CreateOverlay("infravr.beam", "INFRA pointer beam", &beamOverlay) == vr::VROverlayError_None)
        {
            static uint8_t beam[4 * 256 * 4];
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    uint8_t *px = beam + (y * 4 + x) * 4;
                    px[0] = 255;
                    px[1] = 230;
                    px[2] = 200;
                    px[3] = (uint8_t)(40 + 200 * y / 255); // row 255 (beam start at the hand) is brightest
                }
            ov->SetOverlayRaw(beamOverlay, beam, 4, 256, 4);
            ov->SetOverlayWidthInMeters(beamOverlay, 0.006f); // 4x256 texels -> 0.38 m long
            ov->SetOverlaySortOrder(beamOverlay, 2);
        }
        if (ov->CreateOverlay("infravr.hud", "INFRA HUD", &hudOverlay) != vr::VROverlayError_None)
            IVR_ERR("CreateOverlay(hud) failed");
        else
        {
            ov->SetOverlayWidthInMeters(hudOverlay, g_Config.hudWidth);
            vr::HmdMatrix34_t m = {{{1, 0, 0, 0}, {0, 1, 0, -g_Config.hudDown}, {0, 0, 1, -g_Config.hudDistance}}};
            ov->SetOverlayTransformTrackedDeviceRelative(hudOverlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
            ov->SetOverlaySortOrder(hudOverlay, 1);
        }
    }
    m_active = true;
    return true;
}

int VRMod::Dominant() const { return g_Config.leftHanded ? HAND_LEFT : HAND_RIGHT; }

bool VRMod::IsHoldingObject() const { return lastGrabUpdate > 0 && NowSeconds() - lastGrabUpdate < 0.25; }

// --- tracking -> world -----------------------------------------------------------------
Pose VRMod::HeadWorld() const
{
    const TrackedPose &h = g_VR.Head();
    Mat3 R = RotationZ(m_yawOffset);
    Pose p;
    p.rot = R * h.rot;
    p.pos = m_anchor + m_residual;
    if (m_standingHeight > 0)
        p.pos.z += (h.pos.z - m_standingHeight) * m_scale;
    return p;
}

Pose VRMod::EyeWorld(int eye) const
{
    Pose head = HeadWorld();
    head.pos += head.rot * (g_VR.eyeOffset[eye] * m_scale);
    return head;
}

Pose VRMod::HandWorld(int hand) const
{
    const TrackedPose &h = g_VR.Head();
    const TrackedPose &c = g_VR.HandPose(hand);
    Mat3 R = RotationZ(m_yawOffset);
    Pose head = HeadWorld();
    Pose p;
    p.pos = head.pos + R * ((c.pos - h.pos) * m_scale);
    p.rot = R * c.rot;
    return p;
}

Pose VRMod::AimWorld(int hand) const
{
    Pose p = HandWorld(hand);
    if (g_Config.aimPitch != 0.0f)
        p.rot = p.rot * AnglesToMatrix(QAngle(g_Config.aimPitch, 0, 0));
    return p;
}

Pose VRMod::LensWorld() const
{
    Pose p = AimWorld(itemHand);
    p.pos += p.rot * g_Config.lensOffset;
    return p;
}

// --- engine hooks ----------------------------------------------------------------------
void VRMod::OnRenderViewBegin(const CViewSetup &view)
{
    Vector origin = view.origin;
    if (m_haveAnchor)
    {
        Vector moved = Flat(origin - m_prevAnchor);
        float dist = moved.Length();
        if (dist > kTeleportDist)
        {
            m_residual = Vector(0, 0, 0); // level change / teleport: re-centre the body
        }
        else if (m_correcting && m_residual.Length() > 0.01f)
        {
            // Remove the part of the body's movement that went toward the head.
            Vector dir = m_residual.Normalized();
            float along = Dot(moved, dir);
            if (along > 0)
                m_residual -= dir * std::min(along, m_residual.Length());
        }
    }
    m_correcting = false;
    m_prevAnchor = origin;
    m_anchor = origin;
    m_haveAnchor = true;
}

void VRMod::OnCreateMove(CUserCmd *cmd)
{
    if (!m_active || !cmd || !g_VR.Head().valid)
        return;
    double now = NowSeconds();
    RunCommands(now);
    if (menuMode)
        return;

    const TrackedPose &head = g_VR.Head();
    float trackYaw = YawOf(head.rot);

    // The game (or the mouse) turned the view since we last set it: follow it.
    if (m_needYawAlign)
    {
        m_yawOffset = NormalizeYaw(cmd->viewangles.y - trackYaw);
        m_needYawAlign = false;
    }
    else if (m_haveLastSetAngles)
    {
        // Only a real turn by the game counts: while the phone is up INFRA keeps handing back the
        // same angles, ignoring ours, and following that undid every head turn (the world moved
        // with your head).
        float d = NormalizeYaw(cmd->viewangles.y - m_lastSetAngles.y);
        bool gameHeldStill = m_haveLastGameYaw && std::fabs(NormalizeYaw(cmd->viewangles.y - m_lastGameYaw)) < 0.01f;
        if (std::fabs(d) > 0.5f && !gameHeldStill && activeItem != Item::Phone)
            m_yawOffset = NormalizeYaw(m_yawOffset + d);
    }
    m_lastGameYaw = cmd->viewangles.y;
    m_haveLastGameYaw = true;
    m_yawOffset = NormalizeYaw(m_yawOffset + m_turnThisFrame);
    m_turnThisFrame = 0.0f;

    Pose headW = HeadWorld();
    QAngle headAngles = ClampForCmd(headW.Angles());

    // Where the player "looks" for the game's logic this tick.
    QAngle aim = headAngles;
    if (activeItem == Item::Camera && HandValid(itemHand))
        aim = ClampForCmd(LensWorld().Angles()); // photos and camera targets use the lens
    if (m_aimTicks > 0)
    {
        aim = ClampForCmd(m_aimSource == 1 ? LensWorld().Angles() : AimWorld(interactHand).Angles());
        --m_aimTicks;
    }

    const InputState &in = g_VR.Input();

    // Buttons
    bool triggerUse = (in.useRight.state && !(activeItem == Item::Camera && itemHand == HAND_RIGHT)) ||
                      (in.useLeft.state && !(activeItem == Item::Camera && itemHand == HAND_LEFT));
    if (m_useTicks > 0 || triggerUse)
    {
        cmd->buttons |= IN_USE;
        if (m_useTicks == 0 && triggerUse)
            aim = ClampForCmd(AimWorld(interactHand).Angles());
    }
    if (m_useTicks > 0)
        --m_useTicks;
    if (g_Config.gripToCarry)
    {
        bool held = IsHoldingObject();
        if (held && m_grabbingHand >= 0 && m_useTicks == 0)
            cmd->buttons &= ~IN_USE; // a trigger press must not drop what the grip carries
        if (held && m_grabbingHand < 0 && m_freeDropTicks == 0 && m_useTicks == 0 && m_dropCheckAt < 0 &&
            now > m_freeDropUntil)
        {
            m_freeDropTicks = 3; // let go of "use", then press it again: drops the object
            m_freeDropUntil = now + 0.6;
            static int s_logs = 0;
            if (s_logs++ < 5)
                IVR_LOG("Grip to carry: dropping an object that no grip is holding");
        }
        if (m_freeDropTicks > 0)
        {
            if (m_freeDropTicks == 3)
                cmd->buttons &= ~IN_USE;
            else
                cmd->buttons |= IN_USE;
            --m_freeDropTicks;
        }
    }
    if (m_attackTicks > 0)
    {
        cmd->buttons |= IN_ATTACK;
        --m_attackTicks;
    }
    if (m_reloadTicks > 0)
    {
        cmd->buttons |= IN_RELOAD;
        --m_reloadTicks;
    }
    if (m_jumpTicks > 0 || in.jump.state)
    {
        cmd->buttons |= IN_JUMP;
        if (m_jumpTicks > 0)
            --m_jumpTicks;
    }
    if (m_crouchToggled)
        cmd->buttons |= IN_DUCK;
    if (m_sprintToggled)
        cmd->buttons |= IN_SPEED;

    // Movement (world space), then expressed relative to the command's yaw.
    Vector wish(0, 0, 0);
    float sx = in.move.x, sy = in.move.y;
    float mag = std::sqrt(sx * sx + sy * sy);
    if (mag > g_Config.stickDeadzone)
    {
        float basisYaw = headAngles.y;
        if (g_Config.moveRelativeToHand && HandValid(Offhand()))
            basisYaw = AimWorld(Offhand()).Angles().y;
        float scale = std::min(1.0f, (mag - g_Config.stickDeadzone) / (1.0f - g_Config.stickDeadzone)) / mag;
        wish = (YawForward(basisYaw) * sy + YawRight(basisYaw) * sx) * (450.0f * scale);
    }
    else if (g_Config.roomscale && m_residual.Length() > 1.0f)
    {
        // Walk the body toward where you physically stand.
        float speed = std::min(m_residual.Length() * 8.0f, 200.0f);
        wish = m_residual.Normalized() * speed;
        m_correcting = true;
    }
    if (wish.Length2D() > 0.1f)
    {
        float fwd = Dot(wish, YawForward(aim.y));
        float side = Dot(wish, YawRight(aim.y));
        cmd->forwardmove = std::max(-450.0f, std::min(450.0f, cmd->forwardmove + fwd));
        cmd->sidemove = std::max(-450.0f, std::min(450.0f, cmd->sidemove + side));
        if (fwd > 1)
            cmd->buttons |= IN_FORWARD;
        else if (fwd < -1)
            cmd->buttons |= IN_BACK;
        if (side > 1)
            cmd->buttons |= IN_MOVERIGHT;
        else if (side < -1)
            cmd->buttons |= IN_MOVELEFT;
    }

    cmd->viewangles = aim;
    g_Game.SetViewAngles(headAngles);
    m_lastSetAngles = headAngles;
    m_haveLastSetAngles = true;

    // Poses used by the server-side grab hooks during this tick.
    if (HandValid(interactHand))
    {
        Pose hand = AimWorld(interactHand);
        grabHandPos = hand.pos;
        grabHandAngles = hand.Angles();
    }
    else
    {
        grabHandPos = headW.pos;
        grabHandAngles = headAngles;
    }
    if (g_Config.logInput && (cmd->buttons & (IN_USE | IN_ATTACK)))
        IVR_LOG("cmd %d buttons %x aim %.1f %.1f move %.0f %.0f", cmd->command_number, cmd->buttons, aim.x, aim.y,
                cmd->forwardmove, cmd->sidemove);
}

// --- per frame ---------------------------------------------------------------------------
void VRMod::OnFrame()
{
    if (!m_active)
        return;
    double now = NowSeconds();
    double dt = m_lastFrame > 0 ? std::min(0.1, now - m_lastFrame) : 0.0;
    m_lastFrame = now;

    g_VR.WaitPoses();
    g_VR.UpdateInput();

    const TrackedPose &head = g_VR.Head();
    if (!head.valid)
        return;
    if (m_standingHeight <= 0.0f)
        m_standingHeight = std::max(1.0f, std::min(2.2f, head.pos.z));

    // Roomscale: physical head movement moves the view; the body catches up in CreateMove.
    if (m_havePrevHead)
    {
        Vector d = RotationZ(m_yawOffset) * (head.pos - m_prevHeadTrack);
        if (g_Config.roomscale)
            m_residual += Flat(d) * m_scale;
        float len = m_residual.Length();
        if (len > kMaxResidual)
            m_residual = m_residual * (kMaxResidual / len);
    }
    m_prevHeadTrack = head.pos;
    m_havePrevHead = true;

    // Pause button: tap = pause menu / back, hold 1 s = switch the flat "screen mode" on or off
    // (the flat game on the virtual screen, e.g. for menus that hide the mouse cursor).
    const InputState &in = g_VR.Input();
    m_pauseTap = false;
    if (in.pause.pressed)
        m_pauseHeldSince = now;
    if (in.pause.state && m_pauseHeldSince >= 0 && now - m_pauseHeldSince > 1.0)
    {
        m_forceScreen = !m_forceScreen;
        m_pauseHeldSince = -2.0;
        IVR_LOG("Screen mode %s", m_forceScreen ? "on" : "off");
        g_VR.Haptic(HAND_LEFT, 0.1f, 80, 0.6f);
    }
    if (in.pause.released)
    {
        m_pauseTap = m_pauseHeldSince >= 0 && now - m_pauseHeldSince < 0.6;
        m_pauseHeldSince = -1.0;
    }

    bool inGame = g_Game.ready && g_Game.IsInGame();
    bool cursor = g_Game.IsCursorVisible();
    static bool wasLevel = false;
    if (inGame && !wasLevel)
        OnLevelLoaded();
    wasLevel = inGame;
    if (!inGame)
        mainMenu = false;
    inGame = inGame && !(mainMenu && !g_Config.mainMenuInVR); // main menu: flat screen like other menus
    bool wasPhone = phoneMenu;
    // In game, a visible cursor means INFRA's pause menu (on the phone): keep the 3D view and put
    // that menu on a panel in the hand instead of switching to the flat screen.
    phoneMenu = g_Config.phoneMenu && inGame && cursor && !m_forceScreen;
    menuMode = !inGame || (cursor && !phoneMenu) || m_forceScreen;
    if (phoneMenu != wasPhone)
    {
        IVR_LOG("Phone menu %s", phoneMenu ? "open" : "closed");
        if (g_Config.phoneMenuInWorld)
        {
            // The menu stands in the world; the phone model would only get in the way.
            g_Game.ExecuteCommand(phoneMenu ? "r_drawviewmodel 0" : "r_drawviewmodel 1");
            m_phoneWorldPlaced = false;
        }
        else if (phoneMenu)
        {
            m_savedItemHand = itemHand;
            itemHand = Offhand(); // phone in the off hand, point and click with the other one
        }
        else if (m_savedItemHand >= 0)
        {
            itemHand = m_savedItemHand;
            m_savedItemHand = -1;
        }
        m_navDir = 0;
    }
    static bool wasInGame = false;
    if (inGame && !wasInGame)
    {
        // config.cfg may have re-enabled the render thread / effects; the stereo pass needs them off.
        g_Game.ApplyVRSettings();
        activeItem = Item::Unknown;
    }
    wasInGame = inGame;

    // Right-stick click (Crouch action, as on INFRA's gamepad layout): tap = crouch toggle,
    // hold 1 s = recenter.
    m_crouchTap = false;
    if (in.crouch.pressed)
        m_backHeldSince = now;
    if (in.crouch.state && m_backHeldSince >= 0 && now - m_backHeldSince > 1.0)
    {
        Recenter();
        m_backHeldSince = -2.0; // consumed until released
    }
    if (in.crouch.released)
    {
        m_crouchTap = m_backHeldSince >= 0 && now - m_backHeldSince < 0.6;
        m_backHeldSince = -1.0;
    }

    if (phoneMenu)
    {
        RunCommands(now); // the game is paused: CreateMove does not run
        ProcessPhoneMenuInput(now);
    }
    else if (menuMode)
    {
        m_residual = Vector(0, 0, 0);
        RunCommands(now); // CreateMove does not run while paused / in menus
        if (m_pauseTap)
        {
            if (inGame && !m_forceScreen)
                QueueCommand("gameui_hide");
            else
                SendKey(VK_ESCAPE);
        }
        // Like INFRA's gamepad support: left stick moves between menu items, A selects, B goes back.
        // (The right stick scrolls, and pointing + trigger clicks.)
        NavigateWithStick(now, in.move);
        if (in.jump.pressed)
        {
            FocusGameWindow();
            SendKey(VK_RETURN);
        }
        if (in.reload.pressed) // B: back out of a menu page
            SendKey(VK_ESCAPE);
        if (in.recenter.pressed)
            Recenter();
    }
    else
        ProcessGameInput(now, dt);
}

Item VRMod::DetectItem()
{
    void *weapon = g_Game.ActiveWeapon();
    if (!weapon)
        return Item::Hands;
    const char *name = RttiName(weapon);
    if (strstr(name, "INFRACamera"))
        return Item::Camera;
    if (strstr(name, "INFRAPhone"))
        return Item::Phone;
    if (strstr(name, "INFRAHands"))
        return Item::Hands;
    return Item::Other;
}

// INFRA shows its main menu over a live background level. Tell it apart from real levels by name:
// Source background maps are called "background..."; otherwise the first level after start-up
// (the game was not launched straight into a map) is the menu's.
void VRMod::OnLevelLoaded()
{
    std::string map = g_Game.ConVarString("host_map");
    ++m_levelLoads;
    std::string lower = map;
    for (char &c : lower)
        c = (char)tolower((unsigned char)c);
    bool launchedIntoMap = strstr(GetCommandLineA(), "+map") != nullptr;
    if (m_levelLoads == 1 && !launchedIntoMap)
        m_firstMap = lower;
    mainMenu = lower.find("background") != std::string::npos || lower.find("menu") != std::string::npos ||
               (!m_firstMap.empty() && lower == m_firstMap) || (map.empty() && m_levelLoads == 1 && !launchedIntoMap);
    IVR_LOG("Level loaded: \"%s\"%s", map.c_str(),
            mainMenu ? (g_Config.mainMenuInVR ? " (main menu, shown in VR)" : " (main menu, shown on the flat screen)")
                     : "");
}

void VRMod::Recenter()
{
    const TrackedPose &head = g_VR.Head();
    if (head.valid)
        m_standingHeight = std::max(1.0f, std::min(2.2f, head.pos.z));
    m_residual = Vector(0, 0, 0);
    m_needYawAlign = true;
    m_replaceMenu = true;
    IVR_LOG("Recentered (standing eye height %.2f m)", m_standingHeight);
    g_VR.Haptic(HAND_LEFT, 0.1f, 80, 0.5f);
    g_VR.Haptic(HAND_RIGHT, 0.1f, 80, 0.5f);
}

void VRMod::QueueCommand(const std::string &cmd, double delay)
{
    m_commands.push_back({NowSeconds() + delay, cmd});
}

void VRMod::RunCommands(double now)
{
    while (!m_commands.empty() && m_commands.front().at <= now)
    {
        std::string cmd = m_commands.front().cmd;
        m_commands.pop_front();
        g_Game.ExecuteCommand(cmd.c_str());
    }
}

void VRMod::ProcessGameInput(double now, double dt)
{
    const InputState &in = g_VR.Input();
    Item previous = activeItem;
    activeItem = DetectItem();
    if (activeItem != previous)
        IVR_LOG("Active item: %d", (int)activeItem);

    // Turning
    float tx = in.turn.x;
    if (g_Config.snapTurn)
    {
        if (std::fabs(tx) > 0.7f && m_snapArmed)
        {
            m_turnThisFrame -= (tx > 0 ? 1.0f : -1.0f) * g_Config.snapAngle;
            m_snapArmed = false;
        }
        else if (std::fabs(tx) < 0.3f)
            m_snapArmed = true;
    }
    else if (std::fabs(tx) > g_Config.stickDeadzone)
        m_turnThisFrame -= tx * g_Config.smoothTurnSpeed * (float)dt;

    // Recenter: hold for 1 second
    if (in.recenter.state)
    {
        if (m_recenterHeldSince < 0)
            m_recenterHeldSince = now;
        else if (now - m_recenterHeldSince > 1.0)
        {
            Recenter();
            m_recenterHeldSince = 1e30;
        }
    }
    else
        m_recenterHeldSince = -1.0;

    if (in.jump.pressed)
        m_jumpTicks = 2;
    if (m_crouchTap)
        m_crouchToggled = !m_crouchToggled;
    // Sprint toggles like on INFRA's gamepad (left stick click) and ends when you stop moving.
    if (in.sprint.pressed)
        m_sprintToggled = !m_sprintToggled;
    else if (std::sqrt(in.move.x * in.move.x + in.move.y * in.move.y) < g_Config.stickDeadzone)
        m_sprintToggled = false;
    if (in.reload.pressed)
        m_reloadTicks = 2;
    if (m_pauseTap)
        QueueCommand(g_Config.cmdPause);
    if (in.flashlight.pressed)
    {
        flashlightInHand = !flashlightInHand;
        flashlightHand = Offhand();
        QueueCommand(g_Config.cmdFlashlightDown);
        QueueCommand(g_Config.cmdFlashlightUp, 0.1);
    }
    if (in.phone.pressed)
    {
        itemHand = Dominant();
        QueueCommand(activeItem == Item::Phone ? g_Config.cmdHands : g_Config.cmdPhone);
    }

    // Holsters first; a grip that starts in a holster zone does not grab.
    bool consumed[2] = {false, false};
    const DigitalAction *grips[2] = {&in.gripLeft, &in.gripRight};
    if (g_Config.holsters && g_VR.Head().valid)
    {
        const TrackedPose &head = g_VR.Head();
        float yaw = YawOf(head.rot);
        Vector f = YawForward(yaw), l = {-f.y, f.x, 0.0f};
        Vector hipBase(head.pos.x, head.pos.y, m_standingHeight * g_Config.hipHeight);
        Vector centres[3] = {
            hipBase + f * g_Config.hipForward + l * g_Config.hipSide, // 0: left hip  - flashlight
            hipBase + f * g_Config.hipForward - l * g_Config.hipSide, // 1: right hip - phone
            head.pos - f * 0.18f + Vector(0, 0, -0.08f),              // 2: behind the head / shoulder - camera
        };
        float radii[3] = {g_Config.hipRadius, g_Config.hipRadius, g_Config.backRadius};
        for (int h = 0; h < 2; ++h)
        {
            const TrackedPose &c = g_VR.HandPose(h);
            if (!c.valid)
                continue;
            for (int z = 0; z < 3; ++z)
            {
                bool inside = (c.pos - centres[z]).Length() < radii[z];
                if (z == 2)
                    inside = inside && Dot(c.pos - head.pos, f) < 0.03f;
                if (inside && !m_inZone[h][z])
                    g_VR.Haptic(h, 0.04f, 120, 0.35f);
                m_inZone[h][z] = inside;
                if (!inside || !grips[h]->pressed || consumed[h])
                    continue;
                consumed[h] = true;
                g_VR.Haptic(h, 0.08f, 160, 0.8f);
                if (z == 0)
                {
                    flashlightInHand = !flashlightInHand;
                    flashlightHand = h;
                    QueueCommand(g_Config.cmdFlashlightDown);
                    QueueCommand(g_Config.cmdFlashlightUp, 0.1);
                    IVR_LOG("Holster: flashlight %s (hand %d)", flashlightInHand ? "out" : "away", h);
                }
                else if (z == 1)
                {
                    itemHand = h;
                    QueueCommand(activeItem == Item::Phone ? g_Config.cmdHands : g_Config.cmdPhone);
                    IVR_LOG("Holster: phone (hand %d)", h);
                }
                else
                {
                    itemHand = h;
                    QueueCommand(activeItem == Item::Camera ? g_Config.cmdHands : g_Config.cmdCamera);
                    IVR_LOG("Holster: camera (hand %d)", h);
                }
            }
        }
    }

    // Grip = grab / hold: press "use" aimed from that hand; releasing drops (or throws).
    for (int h = 0; h < 2; ++h)
    {
        if (grips[h]->pressed && !consumed[h] && HandValid(h))
        {
            interactHand = h;
            m_grabbingHand = h;
            m_useTicks = 2;
            m_aimTicks = 2;
            m_aimSource = 0;
        }
        else if (grips[h]->released && m_grabbingHand == h)
        {
            m_grabbingHand = -1;
            if (IsHoldingObject())
            {
                interactHand = h;
                float speed = g_VR.HandPose(h).velocity.Length();
                if (speed > 2.5f)
                {
                    m_attackTicks = 2; // throw
                    m_dropCheckAt = now + 0.3;
                }
                else
                    m_useTicks = 2;    // drop
                m_freeDropUntil = now + 0.6; // already letting go: grip_to_carry must not press "use" again
                m_aimTicks = 2;
                m_aimSource = 0;
            }
        }
    }
    if (m_grabbingHand >= 0)
        interactHand = m_grabbingHand;
    if (m_dropCheckAt > 0 && now >= m_dropCheckAt)
    {
        m_dropCheckAt = -1.0;
        if (m_grabbingHand < 0 && IsHoldingObject())
        {
            m_useTicks = 2; // attack did not throw it: just let go
            m_freeDropUntil = now + 0.6;
        }
    }

    // Triggers = "use" (buttons, doors, pick-ups) from that hand, or the camera shutter.
    const DigitalAction *triggers[2] = {&in.useLeft, &in.useRight};
    for (int h = 0; h < 2; ++h)
    {
        if (!triggers[h]->pressed)
            continue;
        if (activeItem == Item::Camera && h == itemHand)
        {
            m_attackTicks = 2;
            m_aimTicks = 2;
            m_aimSource = 1;
        }
        else if (m_grabbingHand < 0)
        {
            interactHand = h;
            m_aimTicks = 2;
            m_aimSource = 0;
        }
    }
}

// --- overlays / menus ----------------------------------------------------------------------
void VRMod::UpdateOverlays(bool showMenu, bool showHud, int menuWidth, int menuHeight)
{
    vr::IVROverlay *ov = g_VR.m_overlay;
    if (!ov)
        return;
    if (hudOverlay != vr::k_ulOverlayHandleInvalid)
    {
        bool atHand = phoneMenu && showHud;
        float aspect = menuWidth > 0 ? (float)menuHeight / (float)menuWidth : 9.0f / 16.0f;
        if (atHand && g_Config.phoneMenuInWorld)
        {
            if (!m_phoneWorldPlaced)
                PlacePhoneMenuInWorld(aspect);
        }
        else if (atHand || m_hudAtHand)
            PlaceHud(atHand, aspect);
        if (showHud)
            ov->ShowOverlay(hudOverlay);
        else
            ov->HideOverlay(hudOverlay);
        if (atHand)
            ProcessPointer(m_phoneXf, g_Config.phoneMenuInWorld ? g_Config.phoneMenuWorldWidth : g_Config.phoneMenuWidth,
                           aspect, g_Config.phoneMenuInWorld ? -1 : 1 - itemHand, false);
        else if (!showMenu && m_pointerShown)
        {
            HidePointer();
            if (m_mouseDown)
                SendMouseButton(false);
        }
    }
    if (menuOverlay == vr::k_ulOverlayHandleInvalid)
        return;
    if (showMenu && (!m_menuVisible || m_replaceMenu) && g_VR.Head().valid)
    {
        // Place the screen in front of the player, level with their eyes.
        const vr::HmdMatrix34_t &hm = g_VR.Head().raw;
        float fx = -hm.m[0][2], fz = -hm.m[2][2];
        float len = std::sqrt(fx * fx + fz * fz);
        if (len < 1e-3f)
        {
            fx = 0;
            fz = -1;
            len = 1;
        }
        fx /= len;
        fz /= len;
        float d = g_Config.menuDistance;
        // columns: X = right, Y = up, Z = toward the viewer (= -forward)
        m_menuXf = {{{-fz, 0, -fx, hm.m[0][3] + fx * d},
                     {0, 1, 0, hm.m[1][3] - 0.05f},
                     {fx, 0, -fz, hm.m[2][3] + fz * d}}};
        m_menuAspect = menuWidth > 0 ? (float)menuHeight / (float)menuWidth : 9.0f / 16.0f;
        ov->SetOverlayTransformAbsolute(menuOverlay, vr::TrackingUniverseStanding, &m_menuXf);
        ov->ShowOverlay(menuOverlay);
        m_menuVisible = true;
        m_replaceMenu = false;
    }
    else if (!showMenu && m_menuVisible)
    {
        ov->HideOverlay(menuOverlay);
        HidePointer();
        if (m_mouseDown)
            SendMouseButton(false);
        m_menuVisible = false;
    }
    if (m_menuVisible)
        ProcessPointer(m_menuXf, g_Config.menuWidth, m_menuAspect, -1, true);
}

// Phone menu as a panel standing in the world: placed once when the menu opens, in front of the
// player a little below eye level and tilted toward them, like a lectern.
void VRMod::PlacePhoneMenuInWorld(float aspect)
{
    vr::IVROverlay *ov = g_VR.m_overlay;
    if (!g_VR.Head().valid)
        return;
    const vr::HmdMatrix34_t &hm = g_VR.Head().raw;
    float fx = -hm.m[0][2], fz = -hm.m[2][2];
    float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-3f)
    {
        fx = 0;
        fz = -1;
        len = 1;
    }
    fx /= len;
    fz /= len;
    const float tilt = 0.35f; // radians, top leaning away from the player
    float c = std::cos(tilt), sn = std::sin(tilt);
    float d = g_Config.phoneMenuWorldDistance;
    // X = right (horizontal), Y = up tilted away, Z = toward the player tilted up
    float X[3] = {-fz, 0, fx};
    float Y[3] = {fx * sn, c, fz * sn};
    float Z[3] = {-fx * c, sn, -fz * c};
    float P[3] = {hm.m[0][3] + fx * d, hm.m[1][3] - 0.25f, hm.m[2][3] + fz * d};
    m_phoneXf = {{{X[0], Y[0], Z[0], P[0]}, {X[1], Y[1], Z[1], P[1]}, {X[2], Y[2], Z[2], P[2]}}};
    ov->SetOverlayWidthInMeters(hudOverlay, g_Config.phoneMenuWorldWidth);
    ov->SetOverlaySortOrder(hudOverlay, 1);
    ov->SetOverlayTransformAbsolute(hudOverlay, vr::TrackingUniverseStanding, &m_phoneXf);
    m_hudAtHand = true; // restore the head-locked HUD when the menu closes
    m_phoneWorldPlaced = true;
    (void)aspect;
}

// HUD panel: head-locked subtitles normally; during the phone menu it is the phone's screen,
// held above the phone hand and tilted toward your eyes.
void VRMod::PlaceHud(bool atHand, float aspect)
{
    vr::IVROverlay *ov = g_VR.m_overlay;
    if (atHand)
    {
        const TrackedPose &c = g_VR.HandPose(itemHand);
        if (!c.valid)
            return;
        const auto &r = c.raw.m;
        const float k = 0.70710678f;
        float X[3], Y[3], Z[3];
        for (int i = 0; i < 3; ++i)
        {
            X[i] = r[i][0];
            Y[i] = (r[i][1] - r[i][2]) * k; // screen up: up and forward from the hand
            Z[i] = (r[i][1] + r[i][2]) * k; // screen faces up and back toward you
        }
        float w = g_Config.phoneMenuWidth, h = w * aspect;
        float up = h * 0.5f + 0.03f;
        m_phoneXf = {{{X[0], Y[0], Z[0], r[0][3] + Y[0] * up + Z[0] * 0.02f},
                      {X[1], Y[1], Z[1], r[1][3] + Y[1] * up + Z[1] * 0.02f},
                      {X[2], Y[2], Z[2], r[2][3] + Y[2] * up + Z[2] * 0.02f}}};
        if (!m_hudAtHand)
        {
            ov->SetOverlayWidthInMeters(hudOverlay, w);
            ov->SetOverlaySortOrder(hudOverlay, 1);
        }
        ov->SetOverlayTransformAbsolute(hudOverlay, vr::TrackingUniverseStanding, &m_phoneXf);
        m_hudAtHand = true;
    }
    else
    {
        ov->SetOverlayWidthInMeters(hudOverlay, g_Config.hudWidth);
        vr::HmdMatrix34_t m = {{{1, 0, 0, 0}, {0, 1, 0, -g_Config.hudDown}, {0, 0, 1, -g_Config.hudDistance}}};
        ov->SetOverlayTransformTrackedDeviceRelative(hudOverlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
        m_hudAtHand = false;
    }
}

// Phone menu: sticks = arrow keys (with repeat), A = select, B = back, pause = close.
void VRMod::ProcessPhoneMenuInput(double now)
{
    const InputState &in = g_VR.Input();
    NavigateWithStick(now, std::fabs(in.move.x) + std::fabs(in.move.y) >= std::fabs(in.turn.x) + std::fabs(in.turn.y)
                               ? in.move
                               : in.turn);
    if (in.jump.pressed) // A
        SendKey(VK_RETURN);
    if (in.reload.pressed) // B
        SendKey(VK_ESCAPE);
    if (m_pauseTap)
        QueueCommand("gameui_hide");
}

void VRMod::NavigateWithStick(double now, const AnalogAction &a)
{
    int dir = 0;
    if (std::fabs(a.y) > 0.6f && std::fabs(a.y) >= std::fabs(a.x))
        dir = a.y > 0 ? VK_UP : VK_DOWN;
    else if (std::fabs(a.x) > 0.6f)
        dir = a.x > 0 ? VK_RIGHT : VK_LEFT;
    else if (std::fabs(a.x) > 0.3f || std::fabs(a.y) > 0.3f)
        dir = m_navDir; // between thresholds: keep the current direction (no flicker)
    if (dir == 0)
        m_navDir = 0;
    else if (dir != m_navDir)
    {
        m_navDir = dir;
        m_navNext = now + 0.4;
        SendKey(dir);
    }
    else if (now >= m_navNext)
    {
        m_navNext = now + 0.15;
        SendKey(dir);
    }
}

// Where the hand's pointing ray meets a screen: u, v in 0..1 from the top left.
bool VRMod::ScreenHit(int hand, const vr::HmdMatrix34_t &m, float w, float aspect, float &u, float &v,
                      float hit[3]) const
{
    const TrackedPose &c = g_VR.HandPose(hand);
    if (!c.valid)
        return false;
    float o[3] = {c.raw.m[0][3], c.raw.m[1][3], c.raw.m[2][3]};
    float d[3] = {-c.raw.m[0][2], -c.raw.m[1][2], -c.raw.m[2][2]};
    float X[3] = {m.m[0][0], m.m[1][0], m.m[2][0]};
    float Y[3] = {m.m[0][1], m.m[1][1], m.m[2][1]};
    float Z[3] = {m.m[0][2], m.m[1][2], m.m[2][2]};
    float C[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
    auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    float denom = dot(d, Z);
    if (denom > -1e-4f) // pointing away from (or along) the screen
        return false;
    float co[3] = {C[0] - o[0], C[1] - o[1], C[2] - o[2]};
    float t = dot(co, Z) / denom;
    if (t < 0.0f)
        return false;
    for (int i = 0; i < 3; ++i)
        hit[i] = o[i] + d[i] * t;
    float rel[3] = {hit[0] - C[0], hit[1] - C[1], hit[2] - C[2]};
    float h = w * aspect;
    u = dot(rel, X) / w + 0.5f;
    v = 0.5f - dot(rel, Y) / h;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

void VRMod::ShowPointer(int hand, bool onScreen, const float hit[3], const vr::HmdMatrix34_t &screen)
{
    vr::IVROverlay *ov = g_VR.m_overlay;
    const TrackedPose &c = g_VR.HandPose(hand);
    if (beamOverlay != vr::k_ulOverlayHandleInvalid && c.valid)
    {
        // Overlay plane X/Y; texture row 0 is +Y. Beam runs along the hand's -Z, faces its +Y.
        const auto &r = c.raw.m;
        const float half = 0.19f;
        vr::HmdMatrix34_t b = {{{r[0][0], -r[0][2], r[0][1], r[0][3] - r[0][2] * half},
                                {r[1][0], -r[1][2], r[1][1], r[1][3] - r[1][2] * half},
                                {r[2][0], -r[2][2], r[2][1], r[2][3] - r[2][2] * half}}};
        ov->SetOverlayTransformAbsolute(beamOverlay, vr::TrackingUniverseStanding, &b);
        ov->ShowOverlay(beamOverlay);
        m_pointerShown = true;
    }
    if (cursorOverlay == vr::k_ulOverlayHandleInvalid)
        return;
    if (!onScreen)
    {
        ov->HideOverlay(cursorOverlay);
        return;
    }
    vr::HmdMatrix34_t m = screen;
    for (int i = 0; i < 3; ++i)
        m.m[i][3] = hit[i] + screen.m[i][2] * 0.005f; // just in front of the screen
    ov->SetOverlayTransformAbsolute(cursorOverlay, vr::TrackingUniverseStanding, &m);
    ov->ShowOverlay(cursorOverlay);
}

void VRMod::HidePointer()
{
    vr::IVROverlay *ov = g_VR.m_overlay;
    if (!ov)
        return;
    if (cursorOverlay != vr::k_ulOverlayHandleInvalid)
        ov->HideOverlay(cursorOverlay);
    if (beamOverlay != vr::k_ulOverlayHandleInvalid)
        ov->HideOverlay(beamOverlay);
    m_pointerOnScreen = false;
    m_pointerShown = false;
}

// Source ignores mouse and keys while its window is in the background (e.g. after clicking the
// SteamVR window). Windows only lets the foreground app hand over focus, so first send a
// harmless Alt press, which unlocks SetForegroundWindow for this process.
void VRMod::FocusGameWindow()
{
    if (!gameWindow || GetForegroundWindow() == gameWindow)
        return;
    INPUT alt[2] = {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    if (IsIconic(gameWindow))
        ShowWindow(gameWindow, SW_RESTORE);
    SetForegroundWindow(gameWindow);
    SetFocus(gameWindow);
    // The engine turns its input off on WM_ACTIVATEAPP(FALSE); tell it it is active again.
    PostMessageA(gameWindow, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageA(gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
}

void VRMod::SendMouseButton(bool down)
{
    m_mouseDown = down;
    if (!gameWindow)
        return;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(gameWindow, &pt);
    PostMessageA(gameWindow, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, MAKELPARAM(pt.x, pt.y));
}

void VRMod::SendKey(WPARAM vk)
{
    if (!gameWindow)
        return;
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    PostMessageA(gameWindow, WM_KEYDOWN, vk, 1 | (scan << 16));
    PostMessageA(gameWindow, WM_KEYUP, vk, 1 | (scan << 16) | (1u << 30) | (1u << 31));
}

void VRMod::ProcessPointer(const vr::HmdMatrix34_t &screen, float width, float aspect, int onlyHand, bool scroll)
{
    const InputState &in = g_VR.Input();
    // The hand that last pulled its trigger points; the other hand takes over when only it hits.
    if (onlyHand >= 0)
        m_pointerHand = onlyHand;
    else if (in.useLeft.pressed)
        m_pointerHand = HAND_LEFT;
    else if (in.useRight.pressed)
        m_pointerHand = HAND_RIGHT;
    float u = 0, v = 0, hit[3] = {};
    bool onScreen = ScreenHit(m_pointerHand, screen, width, aspect, u, v, hit);
    if (!onScreen && !m_mouseDown && onlyHand < 0)
    {
        float u2, v2, hit2[3];
        if (ScreenHit(1 - m_pointerHand, screen, width, aspect, u2, v2, hit2))
        {
            m_pointerHand = 1 - m_pointerHand;
            onScreen = true;
            u = u2;
            v = v2;
            std::memcpy(hit, hit2, sizeof(hit));
        }
    }
    if (onScreen && !m_pointerOnScreen)
        g_VR.Haptic(m_pointerHand, 0.02f, 150, 0.25f);
    m_pointerOnScreen = onScreen;
    ShowPointer(m_pointerHand, onScreen, hit, screen);

    if (!gameWindow)
        return;
    RECT rc{};
    GetClientRect(gameWindow, &rc);
    int cw = std::max<LONG>(1, rc.right - rc.left), ch = std::max<LONG>(1, rc.bottom - rc.top);
    if (onScreen)
    {
        int x = std::min(cw - 1, (int)(u * cw)), y = std::min(ch - 1, (int)(v * ch));
        if (x != m_lastMouseX || y != m_lastMouseY)
        {
            m_lastMouseX = x;
            m_lastMouseY = y;
            POINT pt{x, y};
            ClientToScreen(gameWindow, &pt);
            SetCursorPos(pt.x, pt.y);
            PostMessageA(gameWindow, WM_MOUSEMOVE, m_mouseDown ? MK_LBUTTON : 0, MAKELPARAM(x, y));
        }
    }

    const DigitalAction &trigger = m_pointerHand == HAND_LEFT ? in.useLeft : in.useRight;
    if (trigger.pressed && onScreen && !m_mouseDown)
    {
        FocusGameWindow();
        if (m_clickLogs < 10)
        {
            ++m_clickLogs;
            IVR_LOG("click at %.2f %.2f (window %dx%d, game window %s)", u, v, cw, ch,
                    GetForegroundWindow() == gameWindow ? "in front" : "NOT in front - Windows refused to focus it");
        }
        SendMouseButton(true);
        g_VR.Haptic(m_pointerHand, 0.03f, 200, 0.5f);
    }
    else if (!trigger.state && m_mouseDown)
        SendMouseButton(false);

    if (!scroll)
        return;
    // Right stick up/down scrolls lists.
    float sy = in.turn.y;
    if (std::fabs(sy) > 0.7f && m_scrollArmed)
    {
        m_scrollArmed = false;
        POINT pt;
        GetCursorPos(&pt);
        PostMessageA(gameWindow, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)(sy > 0 ? WHEEL_DELTA : -WHEEL_DELTA)),
                     MAKELPARAM(pt.x, pt.y));
    }
    else if (std::fabs(sy) < 0.3f)
        m_scrollArmed = true;
}

} // namespace ivr
