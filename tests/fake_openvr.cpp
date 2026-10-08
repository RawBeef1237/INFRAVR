// Fake openvr_api.dll for testing INFRA VR under Wine: no SteamVR, but every texture the
// mod submits is read back through Vulkan (on DXVK's own device/queue) and logged.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <type_traits>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "openvr.h"
using namespace vr;
#include "fake_openvr_gen.h"

static FILE *g_log;
static void L(const char *fmt, ...)
{
    if (!g_log)
        g_log = fopen("fake_openvr.log", "w");
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

// Read one pixel (centre of the given uv rect) of a VkImage in TRANSFER_SRC layout.
static bool ReadPixel(const VRVulkanTextureData_t *d, float u, float v, uint8_t out[4])
{
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk)
        return false;
    auto gdpa = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
    auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    VkDevice dev = (VkDevice)d->m_pDevice;
    VkInstance inst = (VkInstance)d->m_pInstance;
#define DF(n) auto n = (PFN_##n)gdpa(dev, #n)
    DF(vkCreateCommandPool); DF(vkAllocateCommandBuffers); DF(vkBeginCommandBuffer); DF(vkCmdCopyImageToBuffer);
    DF(vkEndCommandBuffer); DF(vkQueueSubmit); DF(vkQueueWaitIdle); DF(vkCreateBuffer); DF(vkGetBufferMemoryRequirements);
    DF(vkAllocateMemory); DF(vkBindBufferMemory); DF(vkMapMemory); DF(vkDestroyBuffer); DF(vkFreeMemory);
    DF(vkDestroyCommandPool);
    auto gpmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = d->m_nQueueFamilyIndex;
    VkCommandPool pool;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool))
        return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, nullptr, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    gpmp((VkPhysicalDevice)d->m_pPhysicalDevice, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            type = i;
            break;
        }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    vkAllocateMemory(dev, &mai, nullptr, &mem);
    vkBindBufferMemory(dev, buf, mem, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageOffset = {(int32_t)(u * (d->m_nWidth - 1)), (int32_t)(v * (d->m_nHeight - 1)), 0};
    r.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(cb, (VkImage)d->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult res = vkQueueSubmit((VkQueue)d->m_pQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle((VkQueue)d->m_pQueue);
    void *p = nullptr;
    vkMapMemory(dev, mem, 0, 256, 0, &p);
    memcpy(out, p, 4);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    return res == VK_SUCCESS;
}

static void LogTexture(const char *what, const Texture_t *t, const VRTextureBounds_t *b)
{
    if (!t || t->eType != TextureType_Vulkan || !t->handle)
    {
        L("%s: not a Vulkan texture", what);
        return;
    }
    auto *d = (const VRVulkanTextureData_t *)t->handle;
    float u = b ? (b->uMin + b->uMax) * 0.5f : 0.5f, v = b ? (b->vMin + b->vMax) * 0.5f : 0.5f;
    uint8_t px[4] = {};
    bool ok = ReadPixel(d, u, v, px);
    L("%s: image %llx %ux%u fmt %u queue %p family %u -> pixel(%.2f,%.2f) = %u %u %u %u (%s)", what,
      (unsigned long long)d->m_nImage, d->m_nWidth, d->m_nHeight, d->m_nFormat, (void *)d->m_pQueue,
      d->m_nQueueFamilyIndex, u, v, px[0], px[1], px[2], px[3], ok ? "ok" : "readback failed");
}

static HmdMatrix34_t Pose(float x, float y, float z)
{
    HmdMatrix34_t m = {{{1, 0, 0, x}, {0, 1, 0, y}, {0, 0, 1, z}}};
    return m;
}

struct FakeSystem : Base_IVRSystem
{
    void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override { *w = 1440, *h = 1600; }
    void GetProjectionRaw(EVREye e, float *l, float *r, float *t, float *b) override
    {
        *l = e == Eye_Left ? -1.39f : -1.24f;
        *r = e == Eye_Left ? 1.24f : 1.39f;
        *t = -1.47f;
        *b = 1.45f;
    }
    void GetDXGIOutputInfo(int32_t *i) override { *i = 0; }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye e) override { return Pose(e == Eye_Left ? -0.032f : 0.032f, 0, 0); }
    TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole r) override
    {
        return r == TrackedControllerRole_LeftHand ? 1 : 2;
    }
};

struct FakeCompositor : Base_IVRCompositor
{
    int frames = 0;
    EVRCompositorError WaitGetPoses(TrackedDevicePose_t *p, uint32_t n, TrackedDevicePose_t *, uint32_t) override
    {
        for (uint32_t i = 0; i < n; ++i)
            p[i] = {};
        p[0].bPoseIsValid = p[1].bPoseIsValid = p[2].bPoseIsValid = true;
        p[0].mDeviceToAbsoluteTracking = Pose(0, 1.7f, 0);
        p[1].mDeviceToAbsoluteTracking = Pose(-0.2f, 1.2f, -0.3f);
        p[2].mDeviceToAbsoluteTracking = Pose(0.2f, 1.2f, -0.3f);
        Sleep(5);
        return VRCompositorError_None;
    }
    EVRCompositorError Submit(EVREye e, const Texture_t *t, const VRTextureBounds_t *b, EVRSubmitFlags) override
    {
        if (frames < 8 || frames == 12)
            LogTexture(e == Eye_Left ? "Submit L" : "Submit R", t, b);
        if (e == Eye_Right)
            ++frames;
        return VRCompositorError_None;
    }
    void PostPresentHandoff() override {}
    ETrackingUniverseOrigin GetTrackingSpace() override { return TrackingUniverseStanding; }
};

struct FakeOverlay : Base_IVROverlay
{
    int n = 0, texLogs = 0;
    EVROverlayError CreateOverlay(const char *key, const char *name, VROverlayHandle_t *h) override
    {
        *h = ++n;
        L("CreateOverlay %s -> %llu", key, (unsigned long long)*h);
        return VROverlayError_None;
    }
    EVROverlayError SetOverlayTexture(VROverlayHandle_t h, const Texture_t *t) override
    {
        if (texLogs++ < 6)
        {
            char w[64];
            sprintf(w, "SetOverlayTexture %llu", (unsigned long long)h);
            LogTexture(w, t, nullptr);
        }
        return VROverlayError_None;
    }
    int xfLogs = 0;
    EVROverlayError SetOverlayTransformAbsolute(VROverlayHandle_t h, ETrackingUniverseOrigin,
                                                const HmdMatrix34_t *m) override
    {
        if (xfLogs++ < 12)
            L("SetOverlayTransformAbsolute %llu pos %.3f %.3f %.3f", (unsigned long long)h, m->m[0][3], m->m[1][3],
              m->m[2][3]);
        return VROverlayError_None;
    }
    EVROverlayError SetOverlayRaw(VROverlayHandle_t h, void *, uint32_t w, uint32_t ht, uint32_t bpp) override
    {
        L("SetOverlayRaw %llu %ux%u x%u", (unsigned long long)h, w, ht, bpp);
        return VROverlayError_None;
    }
    EVROverlayError ShowOverlay(VROverlayHandle_t h) override
    {
        L("ShowOverlay %llu", (unsigned long long)h);
        return VROverlayError_None;
    }
};

struct FakeInput : Base_IVRInput
{
    uint64_t next = 1;
    EVRInputError SetActionManifestPath(const char *p) override
    {
        DWORD a = GetFileAttributesA(p);
        L("SetActionManifestPath %s (%s)", p, a == INVALID_FILE_ATTRIBUTES ? "MISSING" : "exists");
        return a == INVALID_FILE_ATTRIBUTES ? VRInputError_InvalidParam : VRInputError_None;
    }
    EVRInputError GetActionSetHandle(const char *, VRActionSetHandle_t *h) override
    {
        *h = 100;
        return VRInputError_None;
    }
    EVRInputError GetActionHandle(const char *, VRActionHandle_t *h) override
    {
        *h = next++;
        return VRInputError_None;
    }
    EVRInputError GetSkeletalActionData(VRActionHandle_t, InputSkeletalActionData_t *d, uint32_t) override
    {
        d->bActive = true;
        return VRInputError_None;
    }
    EVRInputError GetSkeletalSummaryData(VRActionHandle_t h, EVRSummaryType, VRSkeletalSummaryData_t *d) override
    {
        for (int f = 0; f < 5; ++f)
            d->flFingerCurl[f] = 0.1f * f + 0.01f * (float)(h % 10);
        return VRInputError_None;
    }
};

static FakeSystem g_sys;
// MSVC member-function struct return: ECX = this, hidden result pointer first on the stack.
static HmdMatrix34_t *__fastcall EyeToHeadMsvc(void *, void *, HmdMatrix34_t *out, EVREye e)
{
    *out = Pose(e == Eye_Left ? -0.032f : 0.032f, 0, 0);
    return out;
}
static void *g_sysVt[64];
static struct PatchSys
{
    PatchSys()
    {
        void **vt = *(void ***)&g_sys;
        for (int i = 0; i < 64; ++i)
            g_sysVt[i] = vt[i];
        g_sysVt[4] = (void *)&EyeToHeadMsvc;
        *(void ***)&g_sys = g_sysVt;
    }
} g_patchSys;
static FakeCompositor g_comp;
static FakeOverlay g_ov;
static FakeInput g_in;

extern "C" __declspec(dllexport) uint32_t VR_InitInternal2(EVRInitError *e, EVRApplicationType t, const char *)
{
    L("VR_InitInternal2(type %d)", (int)t);
    *e = VRInitError_None;
    return 1;
}
extern "C" __declspec(dllexport) void *VR_InitInternal(EVRInitError *e, EVRApplicationType t)
{
    L("VR_InitInternal(type %d)", (int)t);
    *e = VRInitError_None;
    return &g_sys;
}
extern "C" __declspec(dllexport) void VR_ShutdownInternal() { L("VR_ShutdownInternal"); }
extern "C" __declspec(dllexport) bool VR_IsHmdPresent() { return true; }
extern "C" __declspec(dllexport) const char *VR_GetVRInitErrorAsEnglishDescription(EVRInitError) { return "?"; }
extern "C" __declspec(dllexport) void *VR_GetGenericInterface(const char *v, EVRInitError *e)
{
    *e = VRInitError_None;
    void *r = nullptr;
    if (!strcmp(v, IVRSystem_Version))
        r = &g_sys;
    else if (!strcmp(v, IVRCompositor_Version))
        r = &g_comp;
    else if (!strcmp(v, IVROverlay_Version))
        r = &g_ov;
    else if (!strcmp(v, IVRInput_Version))
        r = &g_in;
    if (!r)
        *e = VRInitError_Init_InterfaceNotFound;
    L("VR_GetGenericInterface(%s) -> %p", v, r);
    return r;
}
