// INFRA VR - the mod's per-frame state: tracking -> game world, input, holsters, grabbing.
#pragma once
#include "common.h"
#include "mathlib.h"
#include "runtime.h"

#include <deque>

namespace ivr {

struct CUserCmd;
struct CViewSetup;

enum class Item
{
    Unknown,
    Hands,
    Camera,
    Phone,
    Other
};

class VRMod
{
public:
    bool Init();                  // after VR_Init: overlays etc.
    bool Active() const { return m_active; }

    // Called after the real Present (poses for the next frame + input processing).
    void OnFrame();
    // Engine hooks
    void OnRenderViewBegin(const CViewSetup &playerView); // anchor = the engine's eye position
    void OnCreateMove(CUserCmd *cmd);

    // World-space poses (valid after OnRenderViewBegin / OnCreateMove)
    Pose HeadWorld() const;
    Pose EyeWorld(int eye) const;
    Pose HandWorld(int hand) const;   // controller grip pose
    Pose AimWorld(int hand) const;    // pointing ray (grip pose + aim pitch)
    Pose LensWorld() const;           // camera lens, in the hand holding the camera
    QAngle UseAimAngles(int hand) const; // "use" aim: from the eye through where the hand points
    bool HandValid(int hand) const { return g_VR.HandPose(hand).valid; }
    float UnitsPerMetre() const { return m_scale; }

    // Console commands run at a safe point: the start of each frame's RenderView on the main thread
    // (never from Present, which Black Mesa VR found crashes). While the game draws no 3D view
    // (paused, loading) they run from the frame update instead, after a short wait.
    void QueueCommand(const std::string &cmd, double delay = 0.0);
    void OnSafePoint();

    // State queried by hooks
    bool menuMode = true;        // flat menu on a virtual screen, eyes blank
    bool eyesRendered = false;   // stereo views rendered this frame
    bool hudPainted = false;
    bool subsPainted = false;    // the subtitle region was copied out of the HUD this frame
    bool phoneMenu = false;
    double firstLevelAt = -1; // when the first map (normally INFRA's main menu) finished loading
    bool mainMenu = false;        // the level loaded is INFRA's main menu background
    std::string m_firstMap;
    int m_levelLoads = 0;
    void OnLevelLoaded();      // in-game pause menu (INFRA's phone) shown in the hand, game keeps rendering
    Item activeItem = Item::Unknown;
    int itemHand = HAND_RIGHT;   // hand holding the viewmodel item (camera / phone)
    int flashlightHand = HAND_LEFT;
    bool flashlightInHand = false;

    // Grabbing (server side hooks)
    int interactHand = HAND_RIGHT;
    double lastGrabUpdate = -1.0;
    bool IsHoldingObject() const;
    Vector grabHandPos;           // world, written by OnCreateMove
    QAngle grabHandAngles;
    // Hand-based "use": the hooks on FindUseEntity (server: what "use" acts on; client: what INFRA's
    // selection outline marks) make it search from the hand instead of the eyes.
    bool handUseServer = false;   // server FindUseEntity + EyePosition hooks installed
    bool handUseClient = false;   // client FindUseEntity + EyePosition + EyeAngles hooks installed
    bool holdInHandReady = false; // server SetTargetPosition hook installed
    Vector useStart;              // server use search for interactHand (written by OnCreateMove)
    QAngle useAngles;
    Vector holdPos;               // where a held object's grab point goes (interactHand)
    Vector holdRel;               // holdPos relative to the engine's view origin (the server adds
                                  // it to the player's own eye position, so moving doesn't hitch)
    int heldEntIndex = -1;        // entity index of the object held in the hand (server handle & 0xFFFF)
    Vector handPos[2];            // each controller in the world, this tick
    Mat3 handRot[2];
    Mat3 holdRot;                 // that hand's rotation
    // Pointer finger: a hand with only the index finger out touches things with its tip.
    bool IsPointing(int hand) const;
    Vector PointerTip(int hand) const; // the index fingertip in the world
    // The drawn hand's index fingertip (hands renderer, each frame it is drawn); PointerTip uses
    // it while it is fresh, else an estimate from the controller.
    void SetFingerTip(int hand, const Vector &p);
    Vector fingerTip[2];
    double fingerTipAt[2] = {-1.0, -1.0};
    bool pointerTouching[2] = {false, false}; // the tip is on something usable (client outline search)
    bool pointerPress[2] = {false, false};    // just touched it: OnCreateMove presses "use"
    double pointerUseUntil = -1.0;            // the server's use search is the fingertip's until then
    int pointerHand = -1;
    bool PointerUseActive() const;
    bool HandUseActive() const;
    // The use search for one hand: starts a little behind the controller (so something the hand
    // is already touching is found too) and goes along where the hand points.
    void UseRay(int hand, Vector &start, QAngle &angles) const;
    // The hand that holds a physics object and is hidden for it, or -1.
    int HeldObjectHand() const;
    // Second object, carried in the other hand by the mod's own grab controller (server side,
    // SecondHoldFrame in hooks_game.cpp). The grip asks; the server picks up or lets go.
    Vector holdPosH[2], holdRelH[2]; // each hand's hold point (as holdPos / holdRel)
    Mat3 holdRotH[2];
    int secondRequest = -1;       // hand that just gripped while the other one carries something
    bool secondRelease = false;   // the second object's hand let go
    float secondReleaseSpeed = 0; // m/s, how fast that hand was moving (a throw)
    int secondHand = -1;          // the hand carrying the second object (set by the server), or -1
    int secondEntIndex = -1;      // its entity index
    int SecondHeldHand() const { return menuMode ? -1 : secondHand; }
    Vector lensPos;
    QAngle lensAngles;

    // Overlays
    vr::VROverlayHandle_t menuOverlay = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t hudOverlay = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t cursorOverlay = vr::k_ulOverlayHandleInvalid; // dot where the pointer hits the menu
    vr::VROverlayHandle_t beamOverlay = vr::k_ulOverlayHandleInvalid;   // short pointer beam from the hand
    void UpdateOverlays(bool showMenu, bool showHud, int menuWidth, int menuHeight);
    // Subtitles on their own head-locked panel (see Config::subtitles).
    vr::VROverlayHandle_t subsOverlay = vr::k_ulOverlayHandleInvalid;
    bool SubtitleLayer() const;
    void ShowSubtitles(bool on);
    HWND gameWindow = nullptr;

private:
    void ProcessGameInput(double now, double dt);
    // Pointer + trigger clicks on a screen (the menu screen or the phone panel). onlyHand >= 0
    // limits pointing to that hand; scroll = right stick scrolls.
    void ProcessPointer(const vr::HmdMatrix34_t &screen, float width, float aspect, int onlyHand, bool scroll);
    bool ScreenHit(int hand, const vr::HmdMatrix34_t &screen, float width, float aspect, float &u, float &v,
                   float hit[3]) const;
    void ShowPointer(int hand, bool onScreen, const float hit[3], const vr::HmdMatrix34_t &screen);
    void ProcessPhoneMenuInput(double now);
    void ProcessHolsters(bool consumed[2]);
    void TogglePhone();
    void PhoneButton(int padCode, WPARAM vk);
    float m_phoneAspect = 9.0f / 16.0f;
    float m_phoneWidth = 0.32f; // phone menu panel width now (metres)
    int m_lastPhoneButtonResult = 2;
    void NavigateWithStick(double now, const AnalogAction &a, bool pad = false); // stick = arrow keys, with repeat
    void FocusGameWindow();
    int m_clickLogs = 0;
    void PlaceHud(bool atHand, float aspect);
    void PlacePhoneMenuInWorld(float aspect);
    void HidePointer();
    void SendMouseButton(bool down);
    void SendKey(WPARAM vk);
    void Holsters();
    void Recenter();
    void RunCommands(double now);
    void RunCommandsFallback(double now);
    double m_lastSafePoint = -1;
    Item DetectItem();
    int Dominant() const;
    int Offhand() const { return 1 - Dominant(); }

    bool m_active = false;

    // tracking -> world
    float m_scale = 39.37f;          // game units per metre
    float m_yawOffset = 0.0f;        // degrees added to the tracking yaw
    float m_standingHeight = 0.0f;   // metres, calibrated eye height
    Vector m_residual;               // world units, head position minus body (roomscale)
    Vector m_prevHeadTrack;
    bool m_havePrevHead = false;
    Vector m_anchor;                 // engine eye position of the local player
    bool m_haveAnchor = false;
    Vector m_prevAnchor;
    bool m_correcting = false;
    QAngle m_lastSetAngles;
    bool m_haveLastSetAngles = false;
    bool m_needYawAlign = true;
    float m_lastGameYaw = 0.0f;      // the yaw the game handed CreateMove last tick
    bool m_haveLastGameYaw = false;

    // input latches consumed by CreateMove
    int m_useTicks = 0, m_attackTicks = 0, m_reloadTicks = 0, m_jumpTicks = 0;
    int m_aimTicks = 0;
    int m_aimSource = 0;             // 0 = hand interactHand, 1 = camera lens
    bool m_crouchToggled = false;
    // Real-life crouching: the game lowers its eye when it ducks; the head already went down by
    // itself, so the game's part is added back (m_crouchComp, world units) to avoid going down twice.
    bool m_realCrouch = false;
    float m_eyeAboveFeet = -1.0f, m_standEye = -1.0f, m_crouchComp = 0.0f;
    double m_lastDuck = -10.0;
    bool m_feetFailed = false;
    bool m_snapArmed = true;
    float m_turnThisFrame = 0.0f;
    double m_lastFrame = 0.0;
    double m_recenterHeldSince = -1.0;
    double m_dropCheckAt = -1.0; // after a throw: drop instead if the object is still held
    bool m_inZone[2][3] = {};
    int m_grabbingHand = -1;     // hand whose grip is held after starting a grab
    int m_freeDropTicks = 0;     // dropping an object nobody holds a grip on
    double m_freeDropUntil = -1.0;

    struct Pending
    {
        double at;
        std::string cmd;
    };
    std::deque<Pending> m_commands;

    // menu overlay state
    bool m_menuVisible = false;
    bool m_replaceMenu = false;   // put the screen in front of the player again (recenter)
    bool m_mouseDown = false;
    vr::HmdMatrix34_t m_menuXf{}; // screen pose, standing universe (X right, Y up, Z toward the viewer)
    float m_menuAspect = 9.0f / 16.0f;
    int m_pointerHand = HAND_RIGHT;
    bool m_pointerOnScreen = false;
    bool m_pointerShown = false;
    int m_lastMouseX = -1, m_lastMouseY = -1;
    bool m_scrollArmed = true;
    double m_backHeldSince = -1.0;  // right-stick click: tap = crouch, hold = recenter
    bool m_sprintToggled = false;
    bool m_crouchTap = false;
    double m_pauseHeldSince = -1.0; // pause button: tap = pause menu, hold = screen mode on/off
    bool m_pauseTap = false;
    bool m_forceScreen = false;     // show the flat game on the virtual screen even in game
    bool m_subsShown = false;
    bool m_hudAtHand = false;     // the HUD panel is placed for the phone menu, not head-locked
    bool m_phoneWorldPlaced = false;
    bool PhoneScreenPanel(float aspect, vr::HmdMatrix34_t &xf, float &width) const;
    vr::HmdMatrix34_t m_phoneXf{};  // phone menu panel pose (standing universe)
    int m_savedItemHand = -1;       // item hand before the phone menu moved the phone to the off hand
    int m_navDir = 0;               // phone menu stick navigation: current direction key, 0 = none
    double m_navNext = 0;
};

extern VRMod g_Mod;

} // namespace ivr
