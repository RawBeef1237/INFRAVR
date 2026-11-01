// INFRA VR - VR runtime layer.
//
// One class, two backends, picked by vr.backend in infravr.ini:
//   openvr (default) - openvr_api.dll (32-bit, next to d3d9.dll in INFRA\bin) is loaded
//     dynamically and initialised BEFORE DXVK creates its Vulkan instance: DXVK's built-in OpenVR
//     support then finds the loaded openvr_api.dll and enables the Vulkan extensions the SteamVR
//     compositor needs (same approach as L4D2VR/Portal2VR). This also covers OpenXR-only headsets
//     through OpenOpenVR / OpenComposite, which is the recommended route for them.
//   openxr - OpenXR runtimes are 64-bit only, so the session lives in a helper process
//     (bin\infravr\openxr\infravr_openxr.exe) and the game talks to it through shared memory
//     (openxr_bridge.h). Falls back to openvr when the helper does not come up.
// The rest of the mod sees the same poses, input, finger curls and overlay calls either way.
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

class OverlayApi;
namespace xr { struct Link; }

enum class Backend
{
    None,
    OpenVR,
    OpenXR
};

class VrRuntime
{
public:
    bool Init();          // picks the backend (vr.backend), falls back to OpenVR
    bool Ok() const;
    Backend GetBackend() const { return m_backend; }
    const char *BackendName() const;
    bool UsesSteamVR() const { return m_backend == Backend::OpenVR; } // IVRCompositor etc. are there
    bool IsOpenXR() const { return m_backend == Backend::OpenXR; }
    // Overlay panels (menu, HUD, pointer) for whichever backend runs; never null after Init.
    OverlayApi *Overlays() const { return m_overlays; }
    // GPU the headset runtime wants the game to render on.
    bool AdapterLuid(LUID &out) const;
    xr::Link *XrLink() const { return m_xr; }
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
    // Size the 3D view is drawn at: the whole eye texture, or with off-center eyes (OpenXR,
    // vr.offcenter_eyes) the size of each eye's own part of the shared frustum (bounds[e]), drawn
    // as an off-center projection. Same picture, about a third fewer pixels.
    bool offCenter = false;
    float eyeFracW = 1.0f, eyeFracH = 1.0f; // the largest eye's share of the shared frustum
    uint32_t ViewW() const
    {
        uint32_t e = (uint32_t)std::lround(renderWidth * eyeFracW) & ~1u;
        return offCenter && e && e < renderWidth ? e : renderWidth;
    }
    uint32_t ViewH() const
    {
        uint32_t e = (uint32_t)std::lround(renderHeight * eyeFracH) & ~1u;
        return offCenter && e && e < renderHeight ? e : renderHeight;
    }
    vr::VRTextureBounds_t ViewBounds(int e) const
    {
        if (!offCenter)
            return bounds[e];
        // the eye fills its whole view, in the corner of the texture
        float su = renderWidth ? ViewW() / (float)renderWidth : 1.0f;
        float sv = renderHeight ? ViewH() / (float)renderHeight : 1.0f;
        return {0.0f, 0.0f, su, sv};
    }
    // Eye e's part of the shared frustum in Source's off-center terms (0..1 across it, left to
    // right and bottom to top), and as tangents: left, right, up, down (left and down negative).
    void EyeOffCenter(int e, float &left, float &right, float &bottom, float &top) const
    {
        left = bounds[e].uMin;
        right = bounds[e].uMax;
        top = 1.0f - bounds[e].vMin; // texture rows run top to bottom
        bottom = 1.0f - bounds[e].vMax;
    }
    void EyeTangents(int e, float out[4]) const
    {
        float tanX = std::tan(fovX * 0.00872664626f), tanY = tanX / aspect; // fovX/2 in radians
        float l, r, b, t;
        EyeOffCenter(e, l, r, b, t);
        out[0] = -tanX + 2.0f * tanX * l;
        out[1] = -tanX + 2.0f * tanX * r;
        out[2] = -tanY + 2.0f * tanY * t;
        out[3] = -tanY + 2.0f * tanY * b;
    }
    Vector eyeOffset[2];                // metres, Source axes, relative to the head

    vr::IVRSystem *m_system = nullptr;
    vr::IVRCompositor *m_compositor = nullptr;
    vr::IVROverlay *m_overlay = nullptr;
    vr::IVRInput *m_vrInput = nullptr;

    // Backend internals, shared with openxr_rt.cpp.
    TrackedPose m_head, m_hands[2];
    InputState m_input;
    bool m_havePoses = false;

private:
    bool InitOpenVR();
    bool InitOpenXR();
    void WaitPosesOpenXR();
    void UpdateInputOpenXR();
    void HapticOpenXR(int hand, float seconds, float frequency, float amplitude);
    void SetProjection(const float l[2], const float r[2], const float t[2], const float b[2], uint32_t recW,
                       uint32_t recH);
    Backend m_backend = Backend::None;
    OverlayApi *m_overlays = nullptr;
    xr::Link *m_xr = nullptr;

    void ReadAction(DigitalAction &a);
    void ReadAction(AnalogAction &a);
    void ReadHandPose(int hand);
    void *Interface(const char *version);

    HMODULE m_dll = nullptr;
    void *(__cdecl *m_getGenericInterface)(const char *, vr::EVRInitError *) = nullptr;

    vr::TrackedDevicePose_t m_poses[vr::k_unMaxTrackedDeviceCount]{};
    vr::VRActionSetHandle_t m_actionSet = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t m_poseAction[2]{vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t m_hapticAction[2]{vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};
    vr::VRActionHandle_t m_skeletonAction[2]{vr::k_ulInvalidActionHandle, vr::k_ulInvalidActionHandle};
    double m_skeletonLogAt = 0;
    int m_skeletonLogs = 0;
    void ReadSkeleton(int hand);
    bool m_inputOk = false;
};

extern VrRuntime g_VR;

// OpenVR (x right, y up, z back; metres) -> Source axes (x forward, y left, z up).
TrackedPose ConvertPose(const vr::HmdMatrix34_t &m);
Vector ConvertVector(float x, float y, float z);

} // namespace ivr
