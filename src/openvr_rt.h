// INFRA VR - OpenVR runtime (SteamVR) access.
//
// openvr_api.dll (32-bit, next to d3d9.dll in INFRA\bin) is loaded dynamically and
// initialised BEFORE DXVK creates its Vulkan instance: DXVK's built-in OpenVR support
// then finds the loaded openvr_api.dll and enables the Vulkan instance/device
// extensions the SteamVR compositor needs (same approach as L4D2VR/Portal2VR).
#pragma once
#include "common.h"
#include "mathlib.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include "openvr.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace ivr {

enum Hand
{
    HAND_LEFT = 0,
    HAND_RIGHT = 1
};

struct TrackedPose
{
    bool valid = false;
    Vector pos;          // metres, OpenVR standing space converted to Source axes (x fwd, y left, z up)
    Mat3 rot;            // Source-axis rotation
    Vector velocity;     // m/s, Source axes
    vr::HmdMatrix34_t raw{}; // original OpenVR matrix (standing universe)
};

struct DigitalAction
{
    vr::VRActionHandle_t handle = vr::k_ulInvalidActionHandle;
    bool state = false, pressed = false, released = false;
};

struct AnalogAction
{
    vr::VRActionHandle_t handle = vr::k_ulInvalidActionHandle;
    float x = 0, y = 0;
};

struct InputState
{
    AnalogAction move, turn;
    DigitalAction jump, crouch, sprint, useRight, useLeft, gripLeft, gripRight, flashlight, reload, pause,
        recenter, phone;
};

class OpenVR
{
public:
    bool Init();          // loads openvr_api.dll and calls VR_Init(Scene)
    bool Ok() const { return m_system && m_compositor; }
    // True once WaitGetPoses has run: the compositor rejects frames (error 101) before that.
    bool HavePoses() const { return m_havePoses; }

    // Per frame -------------------------------------------------------------------------
    void WaitPoses();     // blocks until the compositor wants the next frame
    void UpdateInput();

    const TrackedPose &Head() const { return m_head; }
    const TrackedPose &HandPose(int hand) const { return m_hands[hand]; }
    const InputState &Input() const { return m_input; }
    // SteamVR finger tracking (skeletal summary): curl 0 = straight .. 1 = curled; thumb, index,
    // middle, ring, pinky. skeletonValid is false when the controller reports no skeleton.
    bool skeletonValid[2] = {false, false};
    float fingerCurl[2][5]{};
    void Haptic(int hand, float seconds, float frequency, float amplitude);

    // Rendering parameters (symmetric frustum shared by both eyes + per-eye UV bounds)
    uint32_t renderWidth = 0, renderHeight = 0;
    float fovX = 90.0f;                 // degrees, horizontal
    float aspect = 1.0f;                // width / height of the frustum
    vr::VRTextureBounds_t bounds[2]{};  // which part of the eye texture each eye sees
    // Size the 3D view is actually drawn at inside the eye texture (auto_fit can shrink it when the
    // game only fills part of the texture); 0 = the whole texture.
    uint32_t viewWidth = 0, viewHeight = 0;
    uint32_t ViewW() const { return viewWidth ? viewWidth : renderWidth; }
    uint32_t ViewH() const { return viewHeight ? viewHeight : renderHeight; }
    // bounds[e] mapped onto the part of the texture the view occupies
    vr::VRTextureBounds_t ViewBounds(int e) const
    {
        float su = renderWidth ? ViewW() / (float)renderWidth : 1.0f;
        float sv = renderHeight ? ViewH() / (float)renderHeight : 1.0f;
        return {bounds[e].uMin * su, bounds[e].vMin * sv, bounds[e].uMax * su, bounds[e].vMax * sv};
    }
    Vector eyeOffset[2];                // metres, Source axes, relative to the head

    vr::IVRSystem *m_system = nullptr;
    vr::IVRCompositor *m_compositor = nullptr;
    vr::IVROverlay *m_overlay = nullptr;
    vr::IVRInput *m_vrInput = nullptr;

private:
    void ReadAction(DigitalAction &a);
    void ReadAction(AnalogAction &a);
    void ReadHandPose(int hand);
    void *Interface(const char *version);

    HMODULE m_dll = nullptr;
    void *(__cdecl *m_getGenericInterface)(const char *, vr::EVRInitError *) = nullptr;

    vr::TrackedDevicePose_t m_poses[vr::k_unMaxTrackedDeviceCount]{};
    TrackedPose m_head, m_hands[2];
    InputState m_input;
    vr::VRActionSetHandle_t m_actionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t m_poseAction[2]{vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t m_hapticAction[2]{vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t m_skeletonAction[2]{vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};
    double m_skeletonLogAt = 0;
    int m_skeletonLogs = 0;
    void ReadSkeleton(int hand);
    bool m_inputOk = false;
    bool m_havePoses = false;
};

extern OpenVR g_VR;

// OpenVR (x right, y up, z back; metres) -> Source axes (x forward, y left, z up).
TrackedPose ConvertPose(const vr::HmdMatrix34_t &m);

} // namespace ivr
