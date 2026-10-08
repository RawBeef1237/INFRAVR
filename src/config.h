// INFRA VR - user settings (INFRA\bin\infravr\infravr.ini).
#pragma once
#include "common.h"
#include "mathlib.h"

#include <map>

namespace ivr {

class Ini
{
public:
    bool Load(const std::string &path);
    std::string GetString(const std::string &key, const std::string &def) const;
    float GetFloat(const std::string &key, float def) const;
    int GetInt(const std::string &key, int def) const;
    bool GetBool(const std::string &key, bool def) const;
    Vector GetVector(const std::string &key, const Vector &def) const;

private:
    std::map<std::string, std::string> m_values; // "section.key" (lower case) -> value
};

enum class HudMode
{
    Overlay, // HUD painted into its own texture, shown as a floating panel
    Eyes,    // HUD painted into both eyes like the flat game (head-locked, at infinity)
    Off
};

struct Config
{
    // [vr]
    float worldScale = 1.0f;   // multiplies 39.37 game units per metre
    float renderScale = 1.0f;  // multiplies SteamVR's recommended per-eye resolution
    float zNear = 3.0f;
    bool leftHanded = false;
    bool mirrorToDesktop = true;
    bool mirrorFill = true; // crop the eye to the window shape (16:9) instead of letterboxing
    HudMode hudMode = HudMode::Overlay;
    float hudDistance = 1.2f, hudWidth = 1.0f, hudDown = 0.10f;
    float menuDistance = 2.2f, menuWidth = 2.4f;
    // The engine's full-screen post effects (bloom, colour correction, vignette, depth of field)
    // assume the picture is window-sized; on the larger eye images they smear into lines.
    bool postProcessing = false;
    // Render the eyes no larger than the game window (blurrier, but every effect lines up).
    bool renderFitWindow = false;
    // If the game only fills part of the eye images (the rest stretched lines), draw the view
    // into just that part and show only that part in the headset.
    bool autoFit = true;
    // In-game pause menu (INFRA's phone): held in the hand, controlled with sticks/buttons.
    bool phoneMenu = true;
    // INFRA's main menu is a live 3D level; false = show it on the flat virtual screen.
    bool mainMenuInVR = false;
    float phoneMenuWidth = 0.32f;
    // true = the phone menu is a panel standing in the world in front of you (the phone model is
    // hidden meanwhile); false = a small screen above the phone in your off hand.
    bool phoneMenuInWorld = true;
    float phoneMenuWorldWidth = 0.8f, phoneMenuWorldDistance = 0.9f;

    // [movement]
    bool snapTurn = true;
    float snapAngle = 45.0f;
    float smoothTurnSpeed = 150.0f;
    bool moveRelativeToHand = false;
    bool roomscale = true;
    float stickDeadzone = 0.15f;

    // [aim]
    float aimPitch = 0.0f;   // extra pitch applied to the controller aim ray (degrees, + = down)

    // [holsters]
    bool holsters = true;
    float hipHeight = 0.55f; // hip height as a fraction of your standing eye height
    float hipSide = 0.20f;   // metres left/right of the body centre
    float hipForward = 0.02f;
    float hipRadius = 0.17f;
    float backRadius = 0.26f;
    bool haptics = true;
    bool gripToCarry = true; // physics objects are only carried while a grip is held

    // [camera]
    bool cameraScreenView = true; // draw the camera screen from the lens in your hand
    int cameraScreenEvery = 2;    // ... every Nth frame (the extra view costs GPU time)

    // [viewmodel]  offsets in game units: forward, left, up (in the hand's frame)
    Vector vmOffsetCamera{-14.0f, 2.0f, 5.0f};
    Vector vmOffsetPhone{-12.0f, 3.0f, 4.0f};
    Vector vmOffsetDefault{-12.0f, 2.0f, 4.0f};
    Vector vmAngles{0.0f, 0.0f, 0.0f}; // pitch yaw roll added to the hand orientation
    Vector lensOffset{3.0f, 0.0f, 1.5f};
    Vector flashlightOffset{2.0f, 0.0f, 0.0f};

    // [hands]
    bool handsTracked = true;  // move the viewmodel's other arm to the other controller
    bool handsHideUpperArm = true; // collapse shoulders, upper arms and spine of the viewmodel

    // [commands]
    std::string cmdFlashlightDown = "+flashlight";
    std::string cmdFlashlightUp = "-flashlight";
    std::string cmdPhone = "use infra_phone";
    std::string cmdCamera = "use infra_camera";
    std::string cmdHands = "use infra_hands";
    std::string cmdPause = "gameui_activate";

    // [debug]
    bool logInput = false;
    bool probe = true; // log what the headset images contain every few seconds
};

extern Config g_Config;
void LoadConfig();

} // namespace ivr
