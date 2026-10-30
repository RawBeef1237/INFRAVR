#include "runtime.h"
#include "config.h"
#include "openxr_rt.h"
#include "overlay_api.h"

#include <algorithm>
#include <cmath>

namespace ivr {

VrRuntime g_VR;

static Mat3 ConvertRotation(const float r[3][3])
{
    // Source = C * R * C^T with C = [[0,0,-1],[-1,0,0],[0,1,0]]
    // -> S[i][j] = sum C[i][a] R[a][b] C[j][b]
    static const float C[3][3] = {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}};
    Mat3 out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            float s = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    s += C[i][a] * r[a][b] * C[j][b];
            out.m[i][j] = s;
        }
    return out;
}

Vector ConvertVector(float x, float y, float z) { return {-z, -x, y}; }

TrackedPose ConvertPose(const vr::HmdMatrix34_t &m)
{
    TrackedPose p;
    float r[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i][j] = m.m[i][j];
    p.rot = ConvertRotation(r);
    p.pos = ConvertVector(m.m[0][3], m.m[1][3], m.m[2][3]);
    p.raw = m;
    p.valid = true;
    return p;
}

void *VrRuntime::Interface(const char *version)
{
    vr::EVRInitError err = vr::VRInitError_None;
    void *p = m_getGenericInterface ? m_getGenericInterface(version, &err) : nullptr;
    if (!p || err != vr::VRInitError_None)
    {
        IVR_ERR("OpenVR interface %s unavailable (error %d)", version, (int)err);
        return nullptr;
    }
    return p;
}

bool VrRuntime::Init()
{
    if (g_Config.backend == "openxr")
    {
        if (InitOpenXR())
            return true;
        IVR_ERR("OpenXR is not available - trying OpenVR (SteamVR / OpenOpenVR) instead");
    }
    else if (g_Config.backend != "openvr")
        IVR_ERR("Unknown vr.backend \"%s\" - using openvr", g_Config.backend.c_str());
    return InitOpenVR();
}

bool VrRuntime::Ok() const
{
    switch (m_backend)
    {
    case Backend::OpenVR:
        return m_system && m_compositor;
    case Backend::OpenXR:
        return m_xr && m_xr->Alive();
    default:
        return false;
    }
}

const char *VrRuntime::BackendName() const
{
    switch (m_backend)
    {
    case Backend::OpenVR:
        return "OpenVR";
    case Backend::OpenXR:
        return "OpenXR (helper process)";
    default:
        return "none";
    }
}

bool VrRuntime::AdapterLuid(LUID &out) const
{
    if (m_backend != Backend::OpenXR || !m_xr || !m_xr->shm)
        return false;
    out.LowPart = m_xr->shm->adapterLuidLow;
    out.HighPart = m_xr->shm->adapterLuidHigh;
    return out.LowPart || out.HighPart;
}

void VrRuntime::SetProjection(const float l[2], const float r[2], const float t[2], const float b[2], uint32_t recW,
                              uint32_t recH)
{
    float tanX = std::max({-l[0], r[0], -l[1], r[1]});
    float tanY = std::max({-t[0], b[0], -t[1], b[1]});
    for (int e = 0; e < 2; ++e)
    {
        bounds[e].uMin = 0.5f + 0.5f * l[e] / tanX;
        bounds[e].uMax = 0.5f + 0.5f * r[e] / tanX;
        // OpenVR's raw "top" is the downward half-angle and "bottom" the upward one (SteamVR builds
        // GetProjectionMatrix from them that way; Valve's Unity plugin maps them the same). Our
        // texture's first row is up, so it starts at "bottom". On headsets that see as far up as
        // down (PSVR2, Index) this changes nothing; on Quest it put the picture off vertically.
        bounds[e].vMin = 0.5f - 0.5f * b[e] / tanY;
        bounds[e].vMax = 0.5f - 0.5f * t[e] / tanY;
    }
    fovX = Rad2Deg(2.0f * std::atan(tanX));
    aspect = tanX / tanY;
    // Keep the headset's pixel density across the (larger) symmetric frustum.
    float growX = (2.0f * tanX) / std::max(0.1f, (r[0] - l[0]));
    float growY = (2.0f * tanY) / std::max(0.1f, (b[0] - t[0]));
    renderWidth = (uint32_t)std::lround(recW * growX * g_Config.renderScale) & ~1u;
    renderHeight = (uint32_t)std::lround(recH * growY * g_Config.renderScale) & ~1u;
    renderWidth = std::min<uint32_t>(std::max<uint32_t>(renderWidth, 256), 8192);
    renderHeight = std::min<uint32_t>(std::max<uint32_t>(renderHeight, 256), 8192);

    IVR_LOG("Projection: recommended %ux%u, rendering %ux%u, fov %.1f, aspect %.3f, IPD %.1f mm", recW, recH,
            renderWidth, renderHeight, fovX, aspect, (eyeOffset[1] - eyeOffset[0]).Length() * 1000.0f);
    IVR_LOG("Projection: raw L %.3f %.3f %.3f %.3f  R %.3f %.3f %.3f %.3f", l[0], r[0], t[0], b[0], l[1],
            r[1], t[1], b[1]);
    IVR_LOG("Projection: bounds L u[%.3f %.3f] v[%.3f %.3f]  R u[%.3f %.3f] v[%.3f %.3f]", bounds[0].uMin,
            bounds[0].uMax, bounds[0].vMin, bounds[0].vMax, bounds[1].uMin, bounds[1].uMax, bounds[1].vMin,
            bounds[1].vMax);
}

bool VrRuntime::InitOpenVR()
{
    std::string path = ModuleDir() + "openvr_api.dll";
    m_dll = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_dll)
    {
        IVR_ERR("Could not load %s (error %lu) - VR disabled", path.c_str(), GetLastError());
        return false;
    }
    auto initInternal2 = reinterpret_cast<uint32_t(__cdecl *)(vr::EVRInitError *, vr::EVRApplicationType, const char *)>(
        GetProcAddress(m_dll, "VR_InitInternal2"));
    auto isHmdPresent = reinterpret_cast<bool(__cdecl *)()>(GetProcAddress(m_dll, "VR_IsHmdPresent"));
    auto errorString = reinterpret_cast<const char *(__cdecl *)(vr::EVRInitError)>(
        GetProcAddress(m_dll, "VR_GetVRInitErrorAsEnglishDescription"));
    m_getGenericInterface = reinterpret_cast<void *(__cdecl *)(const char *, vr::EVRInitError *)>(
        GetProcAddress(m_dll, "VR_GetGenericInterface"));
    if (!initInternal2 || !m_getGenericInterface)
    {
        IVR_ERR("openvr_api.dll is missing VR_InitInternal2/VR_GetGenericInterface");
        return false;
    }
    if (isHmdPresent && !isHmdPresent())
        IVR_LOG("VR_IsHmdPresent() = false (SteamVR may still start it)");

    vr::EVRInitError err = vr::VRInitError_None;
    initInternal2(&err, vr::VRApplication_Scene, nullptr);
    if (err != vr::VRInitError_None)
    {
        IVR_ERR("VR_Init failed: %s (%d)", errorString ? errorString(err) : "?", (int)err);
        return false;
    }
    m_system = static_cast<vr::IVRSystem *>(Interface(vr::IVRSystem_Version));
    m_compositor = static_cast<vr::IVRCompositor *>(Interface(vr::IVRCompositor_Version));
    m_overlay = static_cast<vr::IVROverlay *>(Interface(vr::IVROverlay_Version));
    m_vrInput = static_cast<vr::IVRInput *>(Interface(vr::IVRInput_Version));
    if (!m_system || !m_compositor)
        return false;

    // --- projection -------------------------------------------------------------------
    uint32_t recW = 0, recH = 0;
    m_system->GetRecommendedRenderTargetSize(&recW, &recH);
    float l[2], r[2], t[2], b[2];
    for (int e = 0; e < 2; ++e)
        m_system->GetProjectionRaw(static_cast<vr::EVREye>(e), &l[e], &r[e], &t[e], &b[e]);
    // GetEyeToHeadTransform returns a struct by value; call it with MSVC's hidden-pointer
    // convention explicitly instead of relying on the compiler's struct-return ABI.
    using EyeToHeadFn = vr::HmdMatrix34_t *(__thiscall *)(void *, vr::HmdMatrix34_t *, vr::EVREye);
    auto eyeToHead = reinterpret_cast<EyeToHeadFn>((*reinterpret_cast<void ***>(m_system))[4]);
    for (int e = 0; e < 2; ++e)
    {
        vr::HmdMatrix34_t m{};
        eyeToHead(m_system, &m, static_cast<vr::EVREye>(e));
        eyeOffset[e] = ConvertVector(m.m[0][3], m.m[1][3], m.m[2][3]);
    }
    SetProjection(l, r, t, b, recW, recH);
    m_backend = Backend::OpenVR;
    m_overlays = MakeOpenVROverlays(m_overlay);

    // --- input ------------------------------------------------------------------------
    if (m_vrInput)
    {
        std::string manifest = ModuleDir() + "infravr\\actions\\action_manifest.json";
        vr::EVRInputError ie = m_vrInput->SetActionManifestPath(manifest.c_str());
        if (ie != vr::VRInputError_None)
            IVR_ERR("SetActionManifestPath(%s) failed: %d", manifest.c_str(), (int)ie);
        else
        {
            m_inputOk = true;
            auto A = [&](const char *name, vr::VRActionHandle_t &h) {
                if (m_vrInput->GetActionHandle(name, &h) != vr::VRInputError_None)
                    IVR_ERR("GetActionHandle(%s) failed", name);
            };
            m_vrInput->GetActionSetHandle("/actions/main", &m_actionSet);
            A("/actions/main/in/Move", m_input.move.handle);
            A("/actions/main/in/Turn", m_input.turn.handle);
            A("/actions/main/in/Jump", m_input.jump.handle);
            A("/actions/main/in/Crouch", m_input.crouch.handle);
            A("/actions/main/in/Sprint", m_input.sprint.handle);
            A("/actions/main/in/UseRight", m_input.useRight.handle);
            A("/actions/main/in/UseLeft", m_input.useLeft.handle);
            A("/actions/main/in/GripLeft", m_input.gripLeft.handle);
            A("/actions/main/in/GripRight", m_input.gripRight.handle);
            A("/actions/main/in/Flashlight", m_input.flashlight.handle);
            A("/actions/main/in/Phone", m_input.phone.handle);
            A("/actions/main/in/Reload", m_input.reload.handle);
            A("/actions/main/in/Pause", m_input.pause.handle);
            A("/actions/main/in/Recenter", m_input.recenter.handle);
            A("/actions/main/in/PoseLeft", m_poseAction[0]);
            A("/actions/main/in/PoseRight", m_poseAction[1]);
            A("/actions/main/out/HapticLeft", m_hapticAction[0]);
            A("/actions/main/out/HapticRight", m_hapticAction[1]);
            A("/actions/main/in/SkeletonLeft", m_skeletonAction[0]);
            A("/actions/main/in/SkeletonRight", m_skeletonAction[1]);
            IVR_LOG("OpenVR input: action manifest %s", manifest.c_str());
        }
    }
    return true;
}

void VrRuntime::WaitPoses()
{
    if (m_backend == Backend::OpenXR)
        return WaitPosesOpenXR();
    if (!Ok())
        return;
    m_compositor->WaitGetPoses(m_poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    m_havePoses = true;
    const vr::TrackedDevicePose_t &hmd = m_poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (hmd.bPoseIsValid)
        m_head = ConvertPose(hmd.mDeviceToAbsoluteTracking);
}

void VrRuntime::ReadAction(DigitalAction &a)
{
    vr::InputDigitalActionData_t d{};
    bool prev = a.state;
    if (a.handle != vr::k_ulInvalidActionHandle &&
        m_vrInput->GetDigitalActionData(a.handle, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) ==
            vr::VRInputError_None &&
        d.bActive)
        a.state = d.bState;
    else
        a.state = false;
    a.pressed = a.state && !prev;
    a.released = !a.state && prev;
}

void VrRuntime::ReadAction(AnalogAction &a)
{
    vr::InputAnalogActionData_t d{};
    if (a.handle != vr::k_ulInvalidActionHandle &&
        m_vrInput->GetAnalogActionData(a.handle, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) ==
            vr::VRInputError_None &&
        d.bActive)
    {
        a.x = d.x;
        a.y = d.y;
    }
    else
        a.x = a.y = 0;
}

void VrRuntime::ReadHandPose(int hand)
{
    TrackedPose &out = m_hands[hand];
    vr::InputPoseActionData_t pd{};
    if (m_inputOk && m_poseAction[hand] != vr::k_ulInvalidActionHandle &&
        m_vrInput->GetPoseActionDataForNextFrame(m_poseAction[hand], vr::TrackingUniverseStanding, &pd, sizeof(pd),
                                                 vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
        pd.bActive && pd.pose.bPoseIsValid)
    {
        out = ConvertPose(pd.pose.mDeviceToAbsoluteTracking);
        out.velocity = ConvertVector(pd.pose.vVelocity.v[0], pd.pose.vVelocity.v[1], pd.pose.vVelocity.v[2]);
        return;
    }
    // Fallback: raw device pose of the controller in that role.
    vr::TrackedDeviceIndex_t idx = m_system->GetTrackedDeviceIndexForControllerRole(
        hand == HAND_LEFT ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    if (idx < vr::k_unMaxTrackedDeviceCount && m_poses[idx].bPoseIsValid)
    {
        out = ConvertPose(m_poses[idx].mDeviceToAbsoluteTracking);
        const auto &v = m_poses[idx].vVelocity.v;
        out.velocity = ConvertVector(v[0], v[1], v[2]);
    }
    else
        out.valid = false;
}

void VrRuntime::ReadSkeleton(int hand)
{
    vr::InputSkeletalActionData_t sd{};
    vr::VRSkeletalSummaryData_t sum{};
    skeletonValid[hand] = m_skeletonAction[hand] != vr::k_ulInvalidActionHandle &&
                          m_vrInput->GetSkeletalActionData(m_skeletonAction[hand], &sd, sizeof(sd)) ==
                              vr::VRInputError_None &&
                          sd.bActive &&
                          m_vrInput->GetSkeletalSummaryData(m_skeletonAction[hand], vr::VRSummaryType_FromDevice,
                                                            &sum) == vr::VRInputError_None;
    for (int f = 0; f < 5; ++f)
        fingerCurl[hand][f] = skeletonValid[hand] ? sum.flFingerCurl[f] : 0.0f;
}

void VrRuntime::UpdateInput()
{
    if (m_backend == Backend::OpenXR)
        return UpdateInputOpenXR();
    if (!Ok())
        return;
    if (m_inputOk)
    {
        vr::VRActiveActionSet_t set{};
        set.ulActionSet = m_actionSet;
        m_vrInput->UpdateActionState(&set, sizeof(set), 1);
        ReadAction(m_input.move);
        ReadAction(m_input.turn);
        DigitalAction *all[] = {&m_input.jump,     &m_input.crouch,    &m_input.sprint,     &m_input.useRight,
                                &m_input.useLeft,  &m_input.gripLeft,  &m_input.gripRight,  &m_input.flashlight,
                                &m_input.phone,    &m_input.reload,    &m_input.pause,      &m_input.recenter};
        for (auto *a : all)
            ReadAction(*a);
        ReadSkeleton(HAND_LEFT);
        ReadSkeleton(HAND_RIGHT);
        // Finger tracking check: log the curls every 5 s for the first minute.
        double now = NowSeconds();
        if (m_skeletonLogs < 12 && now >= m_skeletonLogAt)
        {
            m_skeletonLogAt = now + 5.0;
            ++m_skeletonLogs;
            const float(*c)[5] = fingerCurl;
            IVR_LOG("Finger curl L %s %.2f %.2f %.2f %.2f %.2f | R %s %.2f %.2f %.2f %.2f %.2f",
                    skeletonValid[0] ? "ok" : "none", c[0][0], c[0][1], c[0][2], c[0][3], c[0][4],
                    skeletonValid[1] ? "ok" : "none", c[1][0], c[1][1], c[1][2], c[1][3], c[1][4]);
        }
    }
    ReadHandPose(HAND_LEFT);
    ReadHandPose(HAND_RIGHT);
}

void VrRuntime::Haptic(int hand, float seconds, float frequency, float amplitude)
{
    if (m_backend == Backend::OpenXR)
        return HapticOpenXR(hand, seconds, frequency, amplitude);
    if (!m_inputOk || !g_Config.haptics || m_hapticAction[hand] == vr::k_ulInvalidActionHandle)
        return;
    m_vrInput->TriggerHapticVibrationAction(m_hapticAction[hand], 0.0f, seconds, frequency, amplitude,
                                            vr::k_ulInvalidInputValueHandle);
}

} // namespace ivr
