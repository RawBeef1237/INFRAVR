// INFRA VR OpenXR helper (infravr_openxr.exe, 64-bit).
//
// INFRA is a 32-bit game and OpenXR runtimes only ship 64-bit, so - like Black Mesa VR's
// OpenXRHelper64 - a small helper process owns the OpenXR session. It is started by the mod's
// d3d9.dll with the game's process id, and talks to it through shared memory (openxr_bridge.h):
//
//   - publishes the view FOV, head and controller poses and controller input every frame,
//   - opens the eye images the game shares from DXVK (D3D shared textures) and copies them into
//     the OpenXR swapchains, together with up to four quad layers (menu screen, HUD/phone panel,
//     pointer dot and beam),
//   - plays haptic pulses.
//
// It exits when the game exits. Everything is logged to INFRA\infravr_openxr.log.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "openxr_bridge.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

FILE *g_log = nullptr;

void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (g_log)
    {
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(g_log);
    }
}

ivrxr::Shared *g_shm = nullptr;

void Fail(const char *fmt, ...)
{
    char buf[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    Log("FAILED: %s", buf);
    if (g_shm)
    {
        snprintf(g_shm->error, sizeof(g_shm->error), "%s", buf);
        g_shm->helperState = 2;
    }
}

// ---- OpenXR function table (loaded from openxr_loader.dll next to this exe) ----------------
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_ = nullptr;
#define XR_FUNCS(X)                                                                                                    \
    X(xrDestroyInstance)                                                                                               \
    X(xrGetInstanceProperties)                                                                                         \
    X(xrGetSystem)                                                                                                     \
    X(xrEnumerateViewConfigurationViews)                                                                               \
    X(xrCreateSession)                                                                                                 \
    X(xrDestroySession)                                                                                                \
    X(xrBeginSession)                                                                                                  \
    X(xrEndSession)                                                                                                    \
    X(xrRequestExitSession)                                                                                            \
    X(xrPollEvent)                                                                                                     \
    X(xrEnumerateReferenceSpaces)                                                                                      \
    X(xrCreateReferenceSpace)                                                                                          \
    X(xrCreateActionSpace)                                                                                             \
    X(xrLocateSpace)                                                                                                   \
    X(xrLocateViews)                                                                                                   \
    X(xrEnumerateSwapchainFormats)                                                                                     \
    X(xrCreateSwapchain)                                                                                               \
    X(xrDestroySwapchain)                                                                                              \
    X(xrEnumerateSwapchainImages)                                                                                      \
    X(xrAcquireSwapchainImage)                                                                                         \
    X(xrWaitSwapchainImage)                                                                                            \
    X(xrReleaseSwapchainImage)                                                                                         \
    X(xrWaitFrame)                                                                                                     \
    X(xrBeginFrame)                                                                                                    \
    X(xrEndFrame)                                                                                                      \
    X(xrStringToPath)                                                                                                  \
    X(xrCreateActionSet)                                                                                               \
    X(xrCreateAction)                                                                                                  \
    X(xrSuggestInteractionProfileBindings)                                                                             \
    X(xrAttachSessionActionSets)                                                                                       \
    X(xrSyncActions)                                                                                                   \
    X(xrGetActionStateBoolean)                                                                                         \
    X(xrGetActionStateFloat)                                                                                           \
    X(xrGetActionStateVector2f)                                                                                        \
    X(xrGetActionStatePose)                                                                                            \
    X(xrApplyHapticFeedback)                                                                                           \
    X(xrGetD3D11GraphicsRequirementsKHR)
#define XR_DECLARE(name) PFN_##name name##_ = nullptr;
XR_FUNCS(XR_DECLARE)

bool XrOk(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r))
        return true;
    Log("%s failed: %d", what, (int)r);
    return false;
}

// ---- math ---------------------------------------------------------------------------------
void PoseToMatrix(const XrPosef &p, float m[3][4])
{
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
    m[0][3] = p.position.x;
    m[1][3] = p.position.y;
    m[2][3] = p.position.z;
}

XrPosef MatrixToPose(const float m[3][4])
{
    XrPosef p{};
    float tr = m[0][0] + m[1][1] + m[2][2];
    float x, y, z, w;
    if (tr > 0)
    {
        float s = std::sqrt(tr + 1.0f) * 2;
        w = 0.25f * s;
        x = (m[2][1] - m[1][2]) / s;
        y = (m[0][2] - m[2][0]) / s;
        z = (m[1][0] - m[0][1]) / s;
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2;
        w = (m[2][1] - m[1][2]) / s;
        x = 0.25f * s;
        y = (m[0][1] + m[1][0]) / s;
        z = (m[0][2] + m[2][0]) / s;
    }
    else if (m[1][1] > m[2][2])
    {
        float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2;
        w = (m[0][2] - m[2][0]) / s;
        x = (m[0][1] + m[1][0]) / s;
        y = 0.25f * s;
        z = (m[1][2] + m[2][1]) / s;
    }
    else
    {
        float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2;
        w = (m[1][0] - m[0][1]) / s;
        x = (m[0][2] + m[2][0]) / s;
        y = (m[1][2] + m[2][1]) / s;
        z = 0.25f * s;
    }
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    p.orientation = {x / n, y / n, z / n, w / n};
    p.position = {m[0][3], m[1][3], m[2][3]};
    return p;
}

// eye pose = head * (eye offset, no rotation)
XrPosef EyeFromHead(const float head[3][4], const float off[3])
{
    float m[3][4];
    std::memcpy(m, head, sizeof(m));
    for (int r = 0; r < 3; ++r)
        m[r][3] = head[r][3] + head[r][0] * off[0] + head[r][1] * off[1] + head[r][2] * off[2];
    return MatrixToPose(m);
}

// ---- D3D11 / swapchains ---------------------------------------------------------------------
ID3D11Device *g_dev = nullptr;
ID3D11DeviceContext *g_ctx = nullptr;

struct Swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
};

int64_t g_colorFormat = 0;

bool CreateSwapchain(XrSession session, uint32_t w, uint32_t h, Swapchain &out)
{
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = g_colorFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!XrOk(xrCreateSwapchain_(session, &ci, &out.handle), "xrCreateSwapchain"))
        return false;
    uint32_t n = 0;
    xrEnumerateSwapchainImages_(out.handle, 0, &n, nullptr);
    out.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages_(out.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(out.images.data()));
    out.width = w;
    out.height = h;
    return true;
}

void DestroySwapchain(Swapchain &s)
{
    if (s.handle)
        xrDestroySwapchain_(s.handle);
    s = Swapchain();
}

// Copies `src` (whole texture) into the next image of `sc`.
bool CopyToSwapchain(Swapchain &sc, ID3D11Texture2D *src)
{
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage_(sc.handle, &ai, &idx)))
        return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage_(sc.handle, &wi);
    D3D11_BOX box{0, 0, 0, sc.width, sc.height, 1};
    g_ctx->CopySubresourceRegion(sc.images[idx].texture, 0, 0, 0, 0, src, 0, &box);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage_(sc.handle, &ri);
    return true;
}

// Shared textures from the game, opened once per handle.
struct Opened
{
    uint64_t handle = 0;
    ID3D11Texture2D *tex = nullptr;
};
std::vector<Opened> g_opened;

ID3D11Texture2D *OpenShared(uint64_t handle)
{
    if (!handle)
        return nullptr;
    for (const Opened &o : g_opened)
        if (o.handle == handle)
            return o.tex;
    ID3D11Texture2D *tex = nullptr;
    HRESULT hr = g_dev->OpenSharedResource(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
                                           reinterpret_cast<void **>(&tex));
    if (FAILED(hr) || !tex)
    {
        Log("OpenSharedResource(%llx) failed: 0x%08lx", (unsigned long long)handle, (unsigned long)hr);
        g_opened.push_back({handle, nullptr});
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    Log("Opened shared texture %llx: %ux%u format %d", (unsigned long long)handle, d.Width, d.Height, (int)d.Format);
    g_opened.push_back({handle, tex});
    return tex;
}

// A small texture for raw-pixel layers (RGBA8 from the game, stored as BGRA like everything else).
struct RawTexture
{
    ID3D11Texture2D *tex = nullptr;
    uint32_t w = 0, h = 0, serial = 0;
};

ID3D11Texture2D *RawLayerTexture(RawTexture &rt, const ivrxr::Layer &l, const uint8_t *pixels)
{
    if (!rt.tex || rt.w != l.width || rt.h != l.height)
    {
        if (rt.tex)
            rt.tex->Release();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = l.width;
        d.Height = l.height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        rt.tex = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &rt.tex)))
            return nullptr;
        rt.w = l.width;
        rt.h = l.height;
        rt.serial = l.serial - 1;
    }
    if (rt.serial != l.serial)
    {
        std::vector<uint8_t> bgra(pixels, pixels + (size_t)l.width * l.height * 4);
        for (size_t i = 0; i < bgra.size(); i += 4)
            std::swap(bgra[i], bgra[i + 2]);
        g_ctx->UpdateSubresource(rt.tex, 0, nullptr, bgra.data(), l.width * 4, 0);
        rt.serial = l.serial;
    }
    return rt.tex;
}

// ---- actions ----------------------------------------------------------------------------------
struct Actions
{
    XrActionSet set = XR_NULL_HANDLE;
    XrAction aim = XR_NULL_HANDLE, move = XR_NULL_HANDLE, turn = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE,
             squeeze = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    struct Btn
    {
        XrAction action;
        uint32_t bit;
    };
    std::vector<Btn> buttons;
    XrPath hand[2] = {};
    XrSpace aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
} g_act;

XrInstance g_instance = XR_NULL_HANDLE;

XrPath P(const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath_(g_instance, s, &p);
    return p;
}

XrAction MakeAction(const char *name, XrActionType type, bool hands)
{
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strncpy(ci.actionName, name, sizeof(ci.actionName) - 1);
    strncpy(ci.localizedActionName, name, sizeof(ci.localizedActionName) - 1);
    ci.actionType = type;
    if (hands)
    {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = g_act.hand;
    }
    XrAction a = XR_NULL_HANDLE;
    XrOk(xrCreateAction_(g_act.set, &ci, &a), name);
    return a;
}

struct Binding
{
    XrAction *action;
    const char *path;
};

void Suggest(const char *profile, const std::vector<std::pair<XrAction, const char *>> &list)
{
    std::vector<XrActionSuggestedBinding> b;
    for (auto &e : list)
        if (e.first)
            b.push_back({e.first, P(e.second)});
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = P(profile);
    sb.countSuggestedBindings = (uint32_t)b.size();
    sb.suggestedBindings = b.data();
    XrResult r = xrSuggestInteractionProfileBindings_(g_instance, &sb);
    Log("Bindings for %s: %s (%d)", profile, XR_SUCCEEDED(r) ? "ok" : "rejected", (int)r);
}

void CreateActions(XrSession session)
{
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "main");
    strcpy(si.localizedActionSetName, "INFRA");
    XrOk(xrCreateActionSet_(g_instance, &si, &g_act.set), "xrCreateActionSet");
    g_act.hand[0] = P("/user/hand/left");
    g_act.hand[1] = P("/user/hand/right");
    g_act.aim = MakeAction("aim", XR_ACTION_TYPE_POSE_INPUT, true);
    g_act.move = MakeAction("move", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    g_act.turn = MakeAction("turn", XR_ACTION_TYPE_VECTOR2F_INPUT, false);
    g_act.trigger = MakeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    g_act.squeeze = MakeAction("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
    g_act.haptic = MakeAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    struct
    {
        const char *name;
        uint32_t bit;
    } names[] = {{"jump", ivrxr::BTN_JUMP},           {"crouch", ivrxr::BTN_CROUCH},
                 {"sprint", ivrxr::BTN_SPRINT},       {"use_right", ivrxr::BTN_USE_RIGHT},
                 {"use_left", ivrxr::BTN_USE_LEFT},   {"grip_left", ivrxr::BTN_GRIP_LEFT},
                 {"grip_right", ivrxr::BTN_GRIP_RIGHT}, {"flashlight", ivrxr::BTN_FLASHLIGHT},
                 {"phone", ivrxr::BTN_PHONE},         {"reload", ivrxr::BTN_RELOAD},
                 {"pause", ivrxr::BTN_PAUSE},         {"recenter", ivrxr::BTN_RECENTER}};
    for (auto &n : names)
        g_act.buttons.push_back({MakeAction(n.name, XR_ACTION_TYPE_BOOLEAN_INPUT, false), n.bit});
    auto B = [](uint32_t bit) {
        for (auto &b : g_act.buttons)
            if (b.bit == bit)
                return b.action;
        return (XrAction)XR_NULL_HANDLE;
    };
    using namespace ivrxr;
    // Same layout as the SteamVR default bindings (README: Touch / Index / Vive / WMR).
    Suggest("/interaction_profiles/oculus/touch_controller",
            {{g_act.aim, "/user/hand/left/input/aim/pose"},
             {g_act.aim, "/user/hand/right/input/aim/pose"},
             {g_act.move, "/user/hand/left/input/thumbstick"},
             {g_act.turn, "/user/hand/right/input/thumbstick"},
             {g_act.trigger, "/user/hand/left/input/trigger/value"},
             {g_act.trigger, "/user/hand/right/input/trigger/value"},
             {g_act.squeeze, "/user/hand/left/input/squeeze/value"},
             {g_act.squeeze, "/user/hand/right/input/squeeze/value"},
             {g_act.haptic, "/user/hand/left/output/haptic"},
             {g_act.haptic, "/user/hand/right/output/haptic"},
             {B(BTN_SPRINT), "/user/hand/left/input/thumbstick/click"},
             {B(BTN_CROUCH), "/user/hand/right/input/thumbstick/click"},
             {B(BTN_RECENTER), "/user/hand/right/input/thumbstick/click"},
             {B(BTN_JUMP), "/user/hand/right/input/a/click"},
             {B(BTN_RELOAD), "/user/hand/right/input/b/click"},
             {B(BTN_FLASHLIGHT), "/user/hand/left/input/x/click"},
             {B(BTN_PAUSE), "/user/hand/left/input/y/click"},
             {B(BTN_USE_LEFT), "/user/hand/left/input/trigger/value"},
             {B(BTN_USE_RIGHT), "/user/hand/right/input/trigger/value"},
             {B(BTN_GRIP_LEFT), "/user/hand/left/input/squeeze/value"},
             {B(BTN_GRIP_RIGHT), "/user/hand/right/input/squeeze/value"}});
    Suggest("/interaction_profiles/valve/index_controller",
            {{g_act.aim, "/user/hand/left/input/aim/pose"},
             {g_act.aim, "/user/hand/right/input/aim/pose"},
             {g_act.move, "/user/hand/left/input/thumbstick"},
             {g_act.turn, "/user/hand/right/input/thumbstick"},
             {g_act.trigger, "/user/hand/left/input/trigger/value"},
             {g_act.trigger, "/user/hand/right/input/trigger/value"},
             {g_act.squeeze, "/user/hand/left/input/squeeze/value"},
             {g_act.squeeze, "/user/hand/right/input/squeeze/value"},
             {g_act.haptic, "/user/hand/left/output/haptic"},
             {g_act.haptic, "/user/hand/right/output/haptic"},
             {B(BTN_SPRINT), "/user/hand/left/input/thumbstick/click"},
             {B(BTN_CROUCH), "/user/hand/right/input/thumbstick/click"},
             {B(BTN_RECENTER), "/user/hand/right/input/thumbstick/click"},
             {B(BTN_JUMP), "/user/hand/right/input/a/click"},
             {B(BTN_RELOAD), "/user/hand/right/input/b/click"},
             {B(BTN_FLASHLIGHT), "/user/hand/left/input/a/click"},
             {B(BTN_PAUSE), "/user/hand/left/input/b/click"},
             {B(BTN_PHONE), "/user/hand/left/input/trackpad/force"},
             {B(BTN_USE_LEFT), "/user/hand/left/input/trigger/click"},
             {B(BTN_USE_RIGHT), "/user/hand/right/input/trigger/click"},
             {B(BTN_GRIP_LEFT), "/user/hand/left/input/squeeze/value"},
             {B(BTN_GRIP_RIGHT), "/user/hand/right/input/squeeze/value"}});
    Suggest("/interaction_profiles/microsoft/motion_controller",
            {{g_act.aim, "/user/hand/left/input/aim/pose"},
             {g_act.aim, "/user/hand/right/input/aim/pose"},
             {g_act.move, "/user/hand/left/input/thumbstick"},
             {g_act.turn, "/user/hand/right/input/thumbstick"},
             {g_act.trigger, "/user/hand/left/input/trigger/value"},
             {g_act.trigger, "/user/hand/right/input/trigger/value"},
             {g_act.haptic, "/user/hand/left/output/haptic"},
             {g_act.haptic, "/user/hand/right/output/haptic"},
             {B(BTN_SPRINT), "/user/hand/left/input/thumbstick/click"},
             {B(BTN_CROUCH), "/user/hand/right/input/thumbstick/click"},
             {B(BTN_RECENTER), "/user/hand/right/input/thumbstick/click"},
             {B(BTN_JUMP), "/user/hand/right/input/trackpad/click"},
             {B(BTN_PAUSE), "/user/hand/left/input/menu/click"},
             {B(BTN_RELOAD), "/user/hand/right/input/menu/click"},
             {B(BTN_USE_LEFT), "/user/hand/left/input/trigger/value"},
             {B(BTN_USE_RIGHT), "/user/hand/right/input/trigger/value"},
             {B(BTN_GRIP_LEFT), "/user/hand/left/input/squeeze/click"},
             {B(BTN_GRIP_RIGHT), "/user/hand/right/input/squeeze/click"}});
    Suggest("/interaction_profiles/htc/vive_controller",
            {{g_act.aim, "/user/hand/left/input/aim/pose"},
             {g_act.aim, "/user/hand/right/input/aim/pose"},
             {g_act.move, "/user/hand/left/input/trackpad"},
             {g_act.turn, "/user/hand/right/input/trackpad"},
             {g_act.trigger, "/user/hand/left/input/trigger/value"},
             {g_act.trigger, "/user/hand/right/input/trigger/value"},
             {g_act.haptic, "/user/hand/left/output/haptic"},
             {g_act.haptic, "/user/hand/right/output/haptic"},
             {B(BTN_SPRINT), "/user/hand/left/input/trackpad/click"},
             {B(BTN_JUMP), "/user/hand/right/input/trackpad/click"},
             {B(BTN_CROUCH), "/user/hand/right/input/menu/click"},
             {B(BTN_RECENTER), "/user/hand/right/input/menu/click"},
             {B(BTN_PAUSE), "/user/hand/left/input/menu/click"},
             {B(BTN_USE_LEFT), "/user/hand/left/input/trigger/click"},
             {B(BTN_USE_RIGHT), "/user/hand/right/input/trigger/click"},
             {B(BTN_GRIP_LEFT), "/user/hand/left/input/squeeze/click"},
             {B(BTN_GRIP_RIGHT), "/user/hand/right/input/squeeze/click"}});
    XrSessionActionSetsAttachInfo att{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    att.countActionSets = 1;
    att.actionSets = &g_act.set;
    XrOk(xrAttachSessionActionSets_(session, &att), "xrAttachSessionActionSets");
    for (int h = 0; h < 2; ++h)
    {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = g_act.aim;
        ci.subactionPath = g_act.hand[h];
        ci.poseInActionSpace.orientation.w = 1;
        XrOk(xrCreateActionSpace_(session, &ci, &g_act.aimSpace[h]), "xrCreateActionSpace");
    }
}

void ReadInput(XrSession session, XrSpace stage, XrTime time)
{
    XrActiveActionSet active{g_act.set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions_(session, &sync);

    auto get2 = [&](XrAction a, float out[2]) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f_(session, &gi, &s)) && s.isActive)
        {
            out[0] = s.currentState.x;
            out[1] = s.currentState.y;
        }
        else
            out[0] = out[1] = 0;
    };
    auto getf = [&](XrAction a, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = g_act.hand[hand];
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        return XR_SUCCEEDED(xrGetActionStateFloat_(session, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
    };
    ivrxr::Shared &sh = *g_shm;
    get2(g_act.move, sh.move);
    get2(g_act.turn, sh.turn);
    uint32_t buttons = 0;
    for (auto &b : g_act.buttons)
    {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = b.action;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean_(session, &gi, &s)) && s.isActive && s.currentState)
            buttons |= b.bit;
    }
    sh.buttons = buttons;
    for (int h = 0; h < 2; ++h)
    {
        sh.trigger[h] = getf(g_act.trigger, h);
        sh.squeeze[h] = getf(g_act.squeeze, h);
        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        loc.next = &vel;
        ivrxr::Pose &p = sh.hands[h];
        if (XR_SUCCEEDED(xrLocateSpace_(g_act.aimSpace[h], stage, time, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            PoseToMatrix(loc.pose, p.m);
            p.valid = 1;
            bool v = vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
            p.velocity[0] = v ? vel.linearVelocity.x : 0;
            p.velocity[1] = v ? vel.linearVelocity.y : 0;
            p.velocity[2] = v ? vel.linearVelocity.z : 0;
        }
        else
            p.valid = 0;
    }
}

void Haptics(XrSession session)
{
    static uint32_t s_seen[2] = {0, 0};
    for (int h = 0; h < 2; ++h)
    {
        uint32_t seq = g_shm->hapticSeq[h];
        if (seq == s_seen[h])
            continue;
        s_seen[h] = seq;
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.duration = (XrDuration)(g_shm->hapticSeconds[h] * 1e9);
        v.frequency = g_shm->hapticFrequency[h] > 0 ? g_shm->hapticFrequency[h] : XR_FREQUENCY_UNSPECIFIED;
        v.amplitude = g_shm->hapticAmplitude[h];
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = g_act.haptic;
        hi.subactionPath = g_act.hand[h];
        xrApplyHapticFeedback_(session, &hi, reinterpret_cast<XrHapticBaseHeader *>(&v));
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int)
{
    DWORD gamePid = strtoul(cmdLine, nullptr, 10);
    char exeDir[MAX_PATH];
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    *strrchr(exeDir, '\\') = 0;
    std::string gameDir = exeDir;
    // exe lives in INFRA\bin\infravr\openxr: the log goes next to infra.exe
    for (int i = 0; i < 3; ++i)
        gameDir = gameDir.substr(0, gameDir.find_last_of('\\'));
    g_log = fopen((gameDir + "\\infravr_openxr.log").c_str(), "w");
    Log("INFRA VR OpenXR helper, game process %lu", (unsigned long)gamePid);

    char name[96];
    snprintf(name, sizeof(name), IVRXR_SHM_NAME "%lu", (unsigned long)gamePid);
    HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map)
    {
        Log("Shared memory %s not found", name);
        return 1;
    }
    g_shm = static_cast<ivrxr::Shared *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ivrxr::Shared)));
    if (!g_shm || g_shm->magic != ivrxr::kMagic || g_shm->version != ivrxr::kVersion)
    {
        Log("Shared memory has the wrong version");
        return 1;
    }
    g_shm->helperPid = GetCurrentProcessId();
    snprintf(name, sizeof(name), IVRXR_FRAME_EVENT "%lu", (unsigned long)gamePid);
    HANDLE frameEvent = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);

    std::string loaderPath = std::string(exeDir) + "\\openxr_loader.dll";
    HMODULE loader = LoadLibraryA(loaderPath.c_str());
    if (!loader || !(xrGetInstanceProcAddr_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
                         GetProcAddress(loader, "xrGetInstanceProcAddr"))))
    {
        Fail("openxr_loader.dll not found next to infravr_openxr.exe");
        return 1;
    }
    PFN_xrCreateInstance createInstance = nullptr;
    xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&createInstance));

    const char *exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "INFRA VR");
    strcpy(ici.applicationInfo.engineName, "Source");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    if (!createInstance || !XrOk(createInstance(&ici, &g_instance), "xrCreateInstance"))
    {
        Fail("No OpenXR runtime with Direct3D 11 support (xrCreateInstance failed)");
        return 1;
    }
#define XR_LOAD(fn) xrGetInstanceProcAddr_(g_instance, #fn, reinterpret_cast<PFN_xrVoidFunction *>(&fn##_));
    XR_FUNCS(XR_LOAD)
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties_(g_instance, &ip);
    snprintf(g_shm->runtimeName, sizeof(g_shm->runtimeName), "%s %u.%u.%u", ip.runtimeName,
             XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
             XR_VERSION_PATCH(ip.runtimeVersion));
    Log("OpenXR runtime: %s", g_shm->runtimeName);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!XrOk(xrGetSystem_(g_instance, &sgi, &system), "xrGetSystem"))
    {
        Fail("No headset found by the OpenXR runtime");
        return 1;
    }
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nViews = 0;
    xrEnumerateViewConfigurationViews_(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nViews,
                                       views);

    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    xrGetD3D11GraphicsRequirementsKHR_(g_instance, system, &req);
    g_shm->adapterLuidLow = req.adapterLuid.LowPart;
    g_shm->adapterLuidHigh = req.adapterLuid.HighPart;
    IDXGIFactory1 *factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (std::memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0)
            break;
        adapter->Release();
        adapter = nullptr;
    }
    D3D_FEATURE_LEVEL fl = req.minFeatureLevel > D3D_FEATURE_LEVEL_11_0 ? req.minFeatureLevel : D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 &fl, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)))
    {
        Fail("Could not create a Direct3D 11 device on the headset's GPU");
        return 1;
    }

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (!XrOk(xrCreateSession_(g_instance, &sci, &session), "xrCreateSession"))
    {
        Fail("xrCreateSession failed");
        return 1;
    }
    // Standing space: STAGE (floor, play area centre) like SteamVR's standing universe.
    uint32_t nSpaces = 0;
    xrEnumerateReferenceSpaces_(session, 0, &nSpaces, nullptr);
    std::vector<XrReferenceSpaceType> spaces(nSpaces);
    xrEnumerateReferenceSpaces_(session, nSpaces, &nSpaces, spaces.data());
    XrReferenceSpaceType standType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (auto t : spaces)
        if (t == XR_REFERENCE_SPACE_TYPE_STAGE)
            standType = t;
    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace.orientation.w = 1;
    rci.referenceSpaceType = standType;
    XrSpace stage = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
    xrCreateReferenceSpace_(session, &rci, &stage);
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace_(session, &rci, &viewSpace);
    Log("Reference space: %s", standType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : "local (no stage)");

    uint32_t nFormats = 0;
    xrEnumerateSwapchainFormats_(session, 0, &nFormats, nullptr);
    std::vector<int64_t> formats(nFormats);
    xrEnumerateSwapchainFormats_(session, nFormats, &nFormats, formats.data());
    // The game's shared textures are D3D9 A8R8G8B8 = DXGI B8G8R8A8, and CopySubresourceRegion needs
    // the same format family. The picture is already gamma-encoded, so the _SRGB view is the right
    // one: the runtime then reads it as sRGB without changing the bytes.
    for (int64_t want : {(int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM})
        for (int64_t f : formats)
            if (!g_colorFormat && f == want)
                g_colorFormat = f;
    if (!g_colorFormat)
    {
        snprintf(g_shm->error, sizeof(g_shm->error), "the OpenXR runtime offers no B8G8R8A8 swapchain format");
        Log("%s", g_shm->error);
        g_shm->helperState = 2;
        return 1;
    }
    Log("Swapchain format %lld", (long long)g_colorFormat);
    CreateActions(session);

    g_shm->recommendedWidth = views[0].recommendedImageRectWidth;
    g_shm->recommendedHeight = views[0].recommendedImageRectHeight;

    Swapchain eyeSc[2];
    Swapchain layerSc[ivrxr::kLayers];
    RawTexture raw[ivrxr::kLayers];
    bool running = false, haveFov = false;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    uint32_t submitted = 0;

    for (;;)
    {
        if (WaitForSingleObject(game, 0) == WAIT_OBJECT_0 || g_shm->gameQuit)
        {
            Log("The game has exited");
            if (running)
                xrRequestExitSession_(session);
            break;
        }
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent_(g_instance, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                state = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev)->state;
                Log("Session state %d", (int)state);
                if (state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = XrOk(xrBeginSession_(session, &bi), "xrBeginSession");
                }
                else if (state == XR_SESSION_STATE_STOPPING)
                {
                    xrEndSession_(session);
                    running = false;
                }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    g_shm->helperState = 3;
                    goto done;
                }
                g_shm->sessionFocused = state == XR_SESSION_STATE_FOCUSED;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running)
        {
            Sleep(10);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        if (!XR_SUCCEEDED(xrWaitFrame_(session, &fwi, &fs)))
            continue;
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame_(session, &fbi);

        // ---- poses and input for the game ----------------------------------------------------
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = viewSpace;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t nv = 0;
        g_shm->poseSeq = g_shm->poseSeq + 1; // odd: writing
        MemoryBarrier();
        if (XR_SUCCEEDED(xrLocateViews_(session, &vli, &vs, 2, &nv, xv)) && nv == 2)
        {
            for (int e = 0; e < 2; ++e)
            {
                g_shm->fov[e][0] = std::tan(xv[e].fov.angleLeft);
                g_shm->fov[e][1] = std::tan(xv[e].fov.angleRight);
                g_shm->fov[e][2] = std::tan(xv[e].fov.angleUp);
                g_shm->fov[e][3] = std::tan(xv[e].fov.angleDown);
                g_shm->eyeToHead[e][0] = xv[e].pose.position.x;
                g_shm->eyeToHead[e][1] = xv[e].pose.position.y;
                g_shm->eyeToHead[e][2] = xv[e].pose.position.z;
            }
            haveFov = true;
        }
        XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace_(viewSpace, stage, fs.predictedDisplayTime, &hl)) &&
            (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            PoseToMatrix(hl.pose, g_shm->head.m);
            g_shm->head.valid = 1;
        }
        ReadInput(session, stage, fs.predictedDisplayTime);
        MemoryBarrier();
        g_shm->poseSeq = g_shm->poseSeq + 1;
        if (haveFov && g_shm->helperState == 0)
        {
            g_shm->helperState = 1;
            Log("Ready: %ux%u per eye", g_shm->recommendedWidth, g_shm->recommendedHeight);
        }
        if (frameEvent)
            SetEvent(frameEvent);
        Haptics(session);

        // ---- layers from the game -----------------------------------------------------------
        std::vector<XrCompositionLayerBaseHeader *> layers;
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerQuad quads[ivrxr::kLayers];
        uint32_t seq = g_shm->eyeSeq;
        if (fs.shouldRender && seq && g_shm->eyesVisible)
        {
            uint32_t slot = seq % ivrxr::kEyeSlots;
            g_shm->helperUsingSlot = slot;
            MemoryBarrier();
            if (g_shm->eyeSeq == seq) // not overwritten meanwhile
            {
                bool ok = true;
                for (int e = 0; e < 2 && ok; ++e)
                {
                    ID3D11Texture2D *src = OpenShared(g_shm->eyeHandle[slot][e]);
                    if (!src)
                    {
                        ok = false;
                        break;
                    }
                    if (eyeSc[e].width != g_shm->eyeWidth || eyeSc[e].height != g_shm->eyeHeight)
                    {
                        DestroySwapchain(eyeSc[e]);
                        ok = CreateSwapchain(session, g_shm->eyeWidth, g_shm->eyeHeight, eyeSc[e]);
                    }
                    ok = ok && CopyToSwapchain(eyeSc[e], src);
                }
                if (ok)
                {
                    const float *f = g_shm->renderFov;
                    for (int e = 0; e < 2; ++e)
                    {
                        pv[e].pose = EyeFromHead(g_shm->renderHead[slot].m, g_shm->eyeToHead[e]);
                        pv[e].fov.angleLeft = std::atan(f[0]);
                        pv[e].fov.angleRight = std::atan(f[1]);
                        pv[e].fov.angleUp = std::atan(f[2]);
                        pv[e].fov.angleDown = std::atan(f[3]);
                        pv[e].subImage.swapchain = eyeSc[e].handle;
                        pv[e].subImage.imageRect = {{0, 0}, {(int32_t)eyeSc[e].width, (int32_t)eyeSc[e].height}};
                    }
                    proj.space = stage;
                    proj.viewCount = 2;
                    proj.views = pv;
                    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&proj));
                    if (++submitted == 1)
                        Log("First frame from the game: %ux%u", g_shm->eyeWidth, g_shm->eyeHeight);
                }
            }
            g_shm->helperUsingSlot = 0xFFFFFFFFu;
        }
        // Quad layers, lowest sort order first.
        int order[ivrxr::kLayers] = {0, 1, 2, 3};
        for (int i = 0; i < ivrxr::kLayers; ++i)
            for (int j = i + 1; j < ivrxr::kLayers; ++j)
                if (g_shm->layers[order[j]].sortOrder < g_shm->layers[order[i]].sortOrder)
                    std::swap(order[i], order[j]);
        for (int k = 0; k < ivrxr::kLayers && fs.shouldRender; ++k)
        {
            int i = order[k];
            const ivrxr::Layer l = g_shm->layers[i];
            if (!l.visible || l.kind == ivrxr::LAYER_NONE || !l.width || !l.height)
                continue;
            ID3D11Texture2D *src = l.kind == ivrxr::LAYER_SHARED ? OpenShared(l.sharedHandle)
                                                                 : RawLayerTexture(raw[i], l, g_shm->raw[i]);
            if (!src)
                continue;
            if (layerSc[i].width != l.width || layerSc[i].height != l.height)
            {
                DestroySwapchain(layerSc[i]);
                if (!CreateSwapchain(session, l.width, l.height, layerSc[i]))
                    continue;
            }
            if (!CopyToSwapchain(layerSc[i], src))
                continue;
            XrCompositionLayerQuad &q = quads[i];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = l.headLocked ? viewSpace : stage;
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = layerSc[i].handle;
            q.subImage.imageRect = {{0, 0}, {(int32_t)l.width, (int32_t)l.height}};
            q.pose = MatrixToPose(l.xf);
            q.size = {l.widthMeters, l.widthMeters * l.height / (float)l.width};
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&q));
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = (uint32_t)layers.size();
        fei.layers = layers.data();
        xrEndFrame_(session, &fei);
    }
done:
    for (auto &s : eyeSc)
        DestroySwapchain(s);
    for (auto &s : layerSc)
        DestroySwapchain(s);
    xrDestroySession_(session);
    xrDestroyInstance_(g_instance);
    Log("Helper finished");
    return 0;
}
