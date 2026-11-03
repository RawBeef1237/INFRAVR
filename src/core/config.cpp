#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace ivr {

Config g_Config;

static std::string Trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string Lower(std::string s)
{
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

bool Ini::Load(const std::string &path)
{
    std::ifstream f(path);
    if (!f)
        return false;
    std::string line, section;
    while (std::getline(f, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        std::string value = Trim(line.substr(eq + 1));
        size_t c = value.find(" ;");
        if (c != std::string::npos)
            value = Trim(value.substr(0, c));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        m_values[section + "." + key] = value;
    }
    return true;
}

std::string Ini::GetString(const std::string &key, const std::string &def) const
{
    auto it = m_values.find(Lower(key));
    return it == m_values.end() ? def : it->second;
}

float Ini::GetFloat(const std::string &key, float def) const
{
    auto it = m_values.find(Lower(key));
    return it == m_values.end() ? def : (float)atof(it->second.c_str());
}

int Ini::GetInt(const std::string &key, int def) const
{
    auto it = m_values.find(Lower(key));
    return it == m_values.end() ? def : (int)strtol(it->second.c_str(), nullptr, 0);
}

bool Ini::GetBool(const std::string &key, bool def) const
{
    auto it = m_values.find(Lower(key));
    if (it == m_values.end())
        return def;
    std::string v = Lower(it->second);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

Vector Ini::GetVector(const std::string &key, const Vector &def) const
{
    auto it = m_values.find(Lower(key));
    if (it == m_values.end())
        return def;
    Vector v = def;
    if (sscanf(it->second.c_str(), "%f %f %f", &v.x, &v.y, &v.z) != 3)
        return def;
    return v;
}

void LoadConfig()
{
    Ini ini;
    std::string path = ModuleDir() + "infravr\\infravr.ini";
    if (!ini.Load(path))
    {
        IVR_LOG("No %s - using defaults", path.c_str());
        return;
    }
    IVR_LOG("Loaded %s", path.c_str());
    Config &c = g_Config;
    c.worldScale = ini.GetFloat("vr.world_scale", c.worldScale);
    c.renderScale = ini.GetFloat("vr.render_scale", c.renderScale);
    c.zNear = ini.GetFloat("vr.znear", c.zNear);
    c.leftHanded = ini.GetBool("vr.left_handed", c.leftHanded);
    c.mirrorToDesktop = ini.GetBool("vr.mirror_to_desktop", c.mirrorToDesktop);
    c.mirrorFill = ini.GetBool("vr.mirror_fill", c.mirrorFill);
    c.antialiasing = ini.GetInt("vr.antialiasing", c.antialiasing);
    c.offCenterEyes = ini.GetBool("vr.offcenter_eyes", c.offCenterEyes);
    if (c.antialiasing != 2 && c.antialiasing != 4 && c.antialiasing != 8)
        c.antialiasing = 0;
    c.multicore = ini.GetBool("vr.multicore", c.multicore);
    c.dxvk = Lower(ini.GetString("vr.dxvk", c.dxvk));
    c.backend = Lower(ini.GetString("vr.backend", c.backend));
    c.worldAtEyeSize = ini.GetBool("vr.world_at_eye_size", c.worldAtEyeSize);
    std::string hud = Lower(ini.GetString("vr.hud_mode", "overlay"));
    c.hudMode = hud == "eyes" ? HudMode::Eyes : hud == "off" ? HudMode::Off : HudMode::Overlay;
    c.hudDistance = ini.GetFloat("vr.hud_distance", c.hudDistance);
    c.hudWidth = ini.GetFloat("vr.hud_width", c.hudWidth);
    c.hudDown = ini.GetFloat("vr.hud_down", c.hudDown);
    c.menuDistance = ini.GetFloat("vr.menu_distance", c.menuDistance);
    c.menuWidth = ini.GetFloat("vr.menu_width", c.menuWidth);
    c.postProcessing = ini.GetBool("vr.post_processing", c.postProcessing);
    c.explicitHandoff = ini.GetBool("vr.explicit_handoff", c.explicitHandoff);
    c.renderFitWindow = ini.GetBool("vr.render_fit_window", c.renderFitWindow);
    c.phoneMenu = ini.GetBool("vr.phone_menu", c.phoneMenu);
    c.mainMenuInVR = ini.GetBool("vr.main_menu_in_vr", c.mainMenuInVR);
    c.phoneMenuWidth = ini.GetFloat("vr.phone_menu_width", c.phoneMenuWidth);
    c.phoneMenuInWorld = ini.GetBool("vr.phone_menu_in_world", c.phoneMenuInWorld);
    c.phoneMenuPointer = ini.GetBool("vr.phone_menu_pointer", c.phoneMenuPointer);
    c.phoneGamepad = ini.GetBool("vr.phone_gamepad", c.phoneGamepad);
    c.phoneMenuWorldWidth = ini.GetFloat("vr.phone_menu_world_width", c.phoneMenuWorldWidth);
    c.phoneMenuWorldDistance = ini.GetFloat("vr.phone_menu_world_distance", c.phoneMenuWorldDistance);

    c.snapTurn = ini.GetBool("movement.snap_turn", c.snapTurn);
    c.snapAngle = ini.GetFloat("movement.snap_angle", c.snapAngle);
    c.smoothTurnSpeed = ini.GetFloat("movement.smooth_turn_speed", c.smoothTurnSpeed);
    c.moveRelativeToHand = Lower(ini.GetString("movement.move_relative_to", "head")) == "hand";
    c.roomscale = ini.GetBool("movement.roomscale", c.roomscale);
    c.realCrouch = ini.GetBool("movement.real_crouch", c.realCrouch);
    c.crouchDrop = ini.GetFloat("movement.crouch_drop", c.crouchDrop);
    c.stickDeadzone = ini.GetFloat("movement.stick_deadzone", c.stickDeadzone);

    c.aimPitch = ini.GetFloat("aim.aim_pitch", c.aimPitch);

    c.holsters = ini.GetBool("holsters.enabled", c.holsters);
    c.hipHeight = ini.GetFloat("holsters.hip_height", c.hipHeight);
    c.hipSide = ini.GetFloat("holsters.hip_side", c.hipSide);
    c.hipForward = ini.GetFloat("holsters.hip_forward", c.hipForward);
    c.hipRadius = ini.GetFloat("holsters.hip_radius", c.hipRadius);
    c.backRadius = ini.GetFloat("holsters.back_radius", c.backRadius);
    c.haptics = ini.GetBool("holsters.haptics", c.haptics);
    c.gripToCarry = ini.GetBool("holsters.grip_to_carry", c.gripToCarry);
    c.phoneOpensMenu = ini.GetBool("holsters.phone_opens_menu", c.phoneOpensMenu);
    c.cameraScreenView = ini.GetBool("camera.screen_view", c.cameraScreenView);
    c.cameraScreenEvery = std::max(1, ini.GetInt("camera.screen_every", c.cameraScreenEvery));
    c.cameraModel = ini.GetString("camera.model", c.cameraModel);
    c.flashlightModel = ini.GetString("flashlight.model", c.flashlightModel);
    c.phoneModel = ini.GetString("phone.model", c.phoneModel);
    c.phoneCallTexture = ini.GetString("phone.call_texture", c.phoneCallTexture);
    {
        std::string uv = ini.GetString("phone.screen_uv", "");
        float r[4];
        if (!uv.empty() && std::sscanf(uv.c_str(), "%f %f %f %f", &r[0], &r[1], &r[2], &r[3]) == 4)
            std::copy(r, r + 4, c.phoneScreenUV);
    }
    {
        float r[4];
        std::string uv = ini.GetString("camera.screen_uv", "");
        if (sscanf(uv.c_str(), "%f %f %f %f", &r[0], &r[1], &r[2], &r[3]) == 4)
            std::copy(r, r + 4, c.cameraScreenUV);
        Vector l = ini.GetVector("camera.lens_uv", Vector(c.cameraLensUV[0], c.cameraLensUV[1], c.cameraLensUV[2]));
        c.cameraLensUV[0] = l.x, c.cameraLensUV[1] = l.y, c.cameraLensUV[2] = l.z;
    }
    c.cameraScreenBrightness = ini.GetFloat("camera.screen_brightness", c.cameraScreenBrightness);

    c.vmOffsetCamera = ini.GetVector("viewmodel.offset_camera", c.vmOffsetCamera);
    c.vmOffsetPhone = ini.GetVector("viewmodel.offset_phone", c.vmOffsetPhone);
    c.vmOffsetDefault = ini.GetVector("viewmodel.offset_default", c.vmOffsetDefault);
    c.vmAngles = ini.GetVector("viewmodel.angles", c.vmAngles);
    c.lensOffset = ini.GetVector("viewmodel.lens_offset", c.lensOffset);
    c.flashlightOffset = ini.GetVector("viewmodel.flashlight_offset", c.flashlightOffset);

    c.handsTracked = ini.GetBool("hands.tracked", c.handsTracked);
    c.handsHideUpperArm = ini.GetBool("hands.hide_upper_arm", c.handsHideUpperArm);
    c.handsModel = Lower(ini.GetString("hands.model", c.handsModel));
    c.handsLeftFile = ini.GetString("hands.left_model", c.handsLeftFile);
    c.handsRightFile = ini.GetString("hands.right_model", c.handsRightFile);
    c.handsOffset = ini.GetVector("hands.offset", c.handsOffset);
    c.handsAngles = ini.GetVector("hands.angles", c.handsAngles);
    c.handsScale = ini.GetFloat("hands.scale", c.handsScale);
    c.handsBrightness = ini.GetFloat("hands.brightness", c.handsBrightness);
    c.handsLighting = Lower(ini.GetString("hands.lighting", c.handsLighting));
    c.handsNormalMaps = ini.GetBool("hands.normal_maps", c.handsNormalMaps);
    c.handsNormalMapGreen = Lower(ini.GetString("hands.normal_map_green", "up")) == "down" ? -1.0f : 1.0f;
    c.handsDepthTest = ini.GetBool("hands.depth_test", c.handsDepthTest);
    c.handsPostProcessing = ini.GetBool("hands.post_processing", c.handsPostProcessing);
    c.handsFingers = ini.GetBool("hands.fingers", c.handsFingers);
    c.handsCurlMax = ini.GetFloat("hands.curl_max", c.handsCurlMax);

    c.cmdFlashlightDown = ini.GetString("commands.flashlight_press", c.cmdFlashlightDown);
    c.cmdFlashlightUp = ini.GetString("commands.flashlight_release", c.cmdFlashlightUp);
    c.cmdPhone = ini.GetString("commands.phone", c.cmdPhone);
    c.cmdCamera = ini.GetString("commands.camera", c.cmdCamera);
    c.cmdHands = ini.GetString("commands.hands", c.cmdHands);
    c.cmdPause = ini.GetString("commands.pause", c.cmdPause);

    c.logInput = ini.GetBool("debug.log_input", c.logInput);
    c.probe = ini.GetBool("debug.probe", c.probe);
    c.retrySkipped = ini.GetBool("debug.retry_skipped", c.retrySkipped);
}

} // namespace ivr
