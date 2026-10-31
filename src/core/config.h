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
    int antialiasing = 4;   // MSAA samples for the eye images: 0 (off), 2, 4 or 8
    bool multicore = false; // let the engine render on its own thread (mat_queue_mode 2)
    std::string dxvk = "stock"; // stock = dxvk_d3d9.dll (DXVK 2.7.1), gplall = dxvk_d3d9_gplall.dll
    // VR runtime: "openvr" (SteamVR, in this process) or "openxr" (the 64-bit infravr_openxr.exe
    // helper talks to the headset's own OpenXR runtime; falls back to OpenVR if it cannot start).
    std::string backend = "openvr";
    // Make the engine allocate its full-frame and post-processing buffers at the eye size (the
    // Black Mesa VR fix), so post-processing works in the headset. Crash-sticky ("world_eye_size").
    bool worldAtEyeSize = true;
    HudMode hudMode = HudMode::Overlay;
    float hudDistance = 1.2f, hudWidth = 1.0f, hudDown = 0.10f;
    float menuDistance = 2.2f, menuWidth = 2.4f;
    // The engine's full-screen post effects (bloom, colour correction, vignette, depth of field).
    // They need buffers at the eye size: on only when world_at_eye_size worked (or the eyes are
    // window-sized), otherwise they smear into lines and are switched off whatever this says.
    bool postProcessing = true;
    // Call IVRCompositor::PostPresentHandoff after every Present. Only needed when the poses are
    // not read right after Present (multicore rendering does that automatically).
    bool explicitHandoff = false;
    // Render the eyes no larger than the game window (blurrier, but every effect lines up).
    bool renderFitWindow = false;
    // If the game only fills part of the eye images (the rest stretched lines), draw the view
    // into just that part and show only that part in the headset.
    // In-game pause menu (INFRA's phone): held in the hand, controlled with sticks/buttons.
    bool phoneMenu = true;
    // INFRA's main menu is a live 3D level; false = show it on the flat virtual screen.
    bool mainMenuInVR = false;
    float phoneMenuWidth = 0.32f;
    // true = the phone menu is a panel standing in the world in front of you (the phone model is
    // hidden meanwhile); false = a small screen above the phone in your off hand.
    bool phoneMenuInWorld = false;
    bool phoneMenuPointer = false; // pointer beam in the phone menu
    bool phoneGamepad = true; // phone menu: the VR controllers act as an Xbox controller for INFRA
    float phoneMenuWorldWidth = 0.8f, phoneMenuWorldDistance = 0.9f;

    // [movement]
    bool snapTurn = true;
    float snapAngle = 45.0f;
    float smoothTurnSpeed = 150.0f;
    bool moveRelativeToHand = false;
    bool roomscale = true;
    bool realCrouch = true;  // crouching in real life crouches in game
    float crouchDrop = 0.30f; // metres below your standing eye height that count as crouching
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
    bool phoneOpensMenu = true; // the phone comes out as INFRA's phone (pause) menu
    bool gripToCarry = true; // physics objects are only carried while a grip is held
    bool triggerAttacks = true; // an empty-handed trigger also sends IN_ATTACK (scripted screens)

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
    // "glb" = the mod draws its own hand models (bin\infravr\hands\*.glb) on both controllers and
    // hides INFRA's arms; "infra" = INFRA's own viewmodel arms follow the controllers.
    std::string handsModel = "glb";
    std::string handsLeftFile = "hands\\vr_hand_left.glb", handsRightFile = "hands\\vr_hand_right.glb";
    // Placement of the right hand model (the left one is mirrored). The bundled models have the
    // fingers along +Z, palm down; these turn them to point forward with the palm inwards around
    // the grip, measured from the controller's aim/tip pose.
    Vector handsOffset{0.035f, -0.02f, -0.07f}; // metres in the controller's frame: right, up, back
    Vector handsAngles{0.0f, 180.0f, -90.0f};   // pitch yaw roll, degrees, in the controller's frame
    float handsScale = 1.0f;
    float handsBrightness = 1.0f;
    bool handsDepthTest = true; // hide the hands behind walls/props in front of them
    bool handsFingers = true;   // bend the fingers from SteamVR's finger tracking
    float handsCurlMax = 75.0f; // degrees each finger joint bends at full curl

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
    bool retrySkipped = false; // forget infravr_skip.txt and try every mechanism again
};

extern Config g_Config;
void LoadConfig();

} // namespace ivr
