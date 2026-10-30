// INFRA VR - OpenXR backend, D3D side: eye images and panels as shared textures for the helper.
//
// Stock DXVK on Windows cannot hand out D3D9 shared textures that D3D11 can open (CreateTexture
// with pSharedHandle gives a Vulkan memory handle, DXVK logs "Failed to write shared resource info"
// and the helper's OpenSharedResource fails with E_INVALIDARG). So it goes the other way round:
// a small D3D11 device in the game process creates the shared textures, they are imported into
// DXVK's Vulkan device (VK_KHR_external_memory_win32, D3D11 KMT handles), and the eye images are
// copied into them with our own command buffer on DXVK's queue.
#include "d3d_vr.h"
#include "config.h"
#include "flags.h"
#include "openxr_rt.h"
#include "overlay_api.h"

#include <d3d11.h>
#include <dxgi.h>

#include <cstring>

namespace ivr {

namespace {

constexpr const char *kFlag = "openxr_interop";

// From vulkan_win32.h (not in third_party): VK_KHR_external_memory_win32.
constexpr VkStructureType kImportMemoryWin32HandleInfo = (VkStructureType)1000073000;
struct ImportMemoryWin32HandleInfo
{
    VkStructureType sType;
    const void *pNext;
    VkExternalMemoryHandleTypeFlagBits handleType;
    HANDLE handle;
    LPCWSTR name;
};
struct MemoryWin32HandleProperties
{
    VkStructureType sType;
    void *pNext;
    uint32_t memoryTypeBits;
};
constexpr VkStructureType kMemoryWin32HandleProperties = (VkStructureType)1000073002;
constexpr VkExternalMemoryHandleTypeFlagBits kKmt = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

struct Vk
{
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2 = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements imageRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    VkResult(VKAPI_PTR *handleProperties)(VkDevice, VkExternalMemoryHandleTypeFlagBits, HANDLE,
                                          MemoryWin32HandleProperties *) = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
} g_vk;

template <typename T> bool DeviceFn(VkDevice dev, T &fn, const char *name)
{
    fn = reinterpret_cast<T>(g_vk.getDeviceProcAddr(dev, name));
    if (!fn)
        IVR_ERR("OpenXR: Vulkan function %s is missing", name);
    return fn != nullptr;
}

bool LoadVulkan(VkDevice dev)
{
    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk)
    {
        IVR_ERR("OpenXR: vulkan-1.dll is not loaded");
        return false;
    }
    g_vk.getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vk, "vkGetDeviceProcAddr"));
    g_vk.memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        GetProcAddress(vk, "vkGetPhysicalDeviceMemoryProperties"));
    g_vk.properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(GetProcAddress(vk, "vkGetPhysicalDeviceProperties2"));
    if (!g_vk.getDeviceProcAddr || !g_vk.memoryProperties)
    {
        IVR_ERR("OpenXR: vulkan-1.dll has no vkGetDeviceProcAddr");
        return false;
    }
    return DeviceFn(dev, g_vk.createImage, "vkCreateImage") && DeviceFn(dev, g_vk.destroyImage, "vkDestroyImage") &&
           DeviceFn(dev, g_vk.imageRequirements, "vkGetImageMemoryRequirements") &&
           DeviceFn(dev, g_vk.allocateMemory, "vkAllocateMemory") && DeviceFn(dev, g_vk.freeMemory, "vkFreeMemory") &&
           DeviceFn(dev, g_vk.bindImageMemory, "vkBindImageMemory") &&
           DeviceFn(dev, g_vk.handleProperties, "vkGetMemoryWin32HandlePropertiesKHR") &&
           DeviceFn(dev, g_vk.createCommandPool, "vkCreateCommandPool") &&
           DeviceFn(dev, g_vk.allocateCommandBuffers, "vkAllocateCommandBuffers") &&
           DeviceFn(dev, g_vk.freeCommandBuffers, "vkFreeCommandBuffers") &&
           DeviceFn(dev, g_vk.resetCommandBuffer, "vkResetCommandBuffer") &&
           DeviceFn(dev, g_vk.beginCommandBuffer, "vkBeginCommandBuffer") &&
           DeviceFn(dev, g_vk.endCommandBuffer, "vkEndCommandBuffer") &&
           DeviceFn(dev, g_vk.cmdPipelineBarrier, "vkCmdPipelineBarrier") &&
           DeviceFn(dev, g_vk.cmdCopyImage, "vkCmdCopyImage") && DeviceFn(dev, g_vk.queueSubmit, "vkQueueSubmit") &&
           DeviceFn(dev, g_vk.createFence, "vkCreateFence") && DeviceFn(dev, g_vk.destroyFence, "vkDestroyFence") &&
           DeviceFn(dev, g_vk.resetFences, "vkResetFences") &&
           DeviceFn(dev, g_vk.getFenceStatus, "vkGetFenceStatus") &&
           DeviceFn(dev, g_vk.waitForFences, "vkWaitForFences");
}

// The system's d3d11.dll/dxgi.dll, never a copy next to infra.exe.
HMODULE LoadSystemDll(const char *name)
{
    char dir[MAX_PATH];
    UINT n = GetSystemDirectoryA(dir, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return nullptr;
    std::string path = std::string(dir) + "\\" + name;
    return LoadLibraryA(path.c_str());
}

// The DXGI adapter with this LUID, or nullptr (the default adapter).
IDXGIAdapter1 *FindAdapter(const LUID &want, bool haveLuid)
{
    HMODULE dxgi = LoadSystemDll("dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
    auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1 *factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return nullptr;
    IDXGIAdapter1 *found = nullptr;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; haveLuid && !found && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.LowPart == want.LowPart &&
            d.AdapterLuid.HighPart == want.HighPart)
            found = a;
        else
            a->Release();
    }
    factory->Release();
    return found;
}

uint32_t PickMemoryType(VkPhysicalDevice phys, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties p{};
    g_vk.memoryProperties(phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
        if (bits & (1u << i))
            return i;
    return UINT32_MAX;
}

ivrxr::Shared *Shm()
{
    xr::Link *l = g_VR.XrLink();
    return l ? l->shm : nullptr;
}

const VkImageSubresourceLayers kColorLayers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
const VkImageSubresourceRange kColor = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

} // namespace

bool D3DVR::XrInitInterop()
{
    if (m_xrInit)
        return m_xrInit > 0;
    m_xrInit = -1;
    if (!flags::Begin(kFlag))
    {
        IVR_ERR("OpenXR: the texture handoff is skipped (infravr_skip.txt) - nothing can be shown in the headset; "
                "set vr.backend = openvr");
        return false;
    }
    if (!LoadVulkan(m_vkDevice))
        return false;

    // The D3D11 device must sit on DXVK's GPU, or the import fails.
    LUID luid{};
    bool haveLuid = false;
    if (g_vk.properties2)
    {
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 p{};
        p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p.pNext = &id;
        g_vk.properties2(m_physDev, &p);
        if (id.deviceLUIDValid)
        {
            std::memcpy(&luid, id.deviceLUID, sizeof(luid));
            haveLuid = true;
        }
    }
    IDXGIAdapter1 *adapter = FindAdapter(luid, haveLuid);
    if (!adapter)
        IVR_LOG("OpenXR: no DXGI adapter matches DXVK's GPU (LUID %s) - using the default adapter",
                haveLuid ? "not found" : "unknown");
    HMODULE d3d11 = LoadSystemDll("d3d11.dll");
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"))
                        : nullptr;
    HRESULT hr = E_FAIL;
    if (create)
        hr = create(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                    D3D11_SDK_VERSION, &m_xrD3D11, nullptr, nullptr);
    if (adapter)
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
        IVR_LOG("OpenXR: shared textures are made on %s", name);
        adapter->Release();
    }
    if (FAILED(hr) || !m_xrD3D11)
    {
        IVR_ERR("OpenXR: could not create a D3D11 device for the shared textures (0x%08lx)", (unsigned long)hr);
        m_xrD3D11 = nullptr;
        return false;
    }

    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = m_queueFamily;
    VkResult r = g_vk.createCommandPool(m_vkDevice, &pi, nullptr, &m_xrPool);
    if (r != VK_SUCCESS)
    {
        IVR_ERR("OpenXR: vkCreateCommandPool failed (%d)", (int)r);
        return false;
    }
    m_xrInit = 1;
    return true;
}

bool D3DVR::XrCreateImage(UINT w, UINT h, XrImage &out)
{
    out = XrImage();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // DXVK's A8R8G8B8, and the helper's swapchain family
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = m_xrD3D11->CreateTexture2D(&d, nullptr, &out.d3d11);
    IDXGIResource *res = nullptr;
    if (SUCCEEDED(hr))
        hr = out.d3d11->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&res));
    if (SUCCEEDED(hr))
    {
        hr = res->GetSharedHandle(&out.handle);
        res->Release();
    }
    if (FAILED(hr) || !out.handle)
    {
        IVR_ERR("OpenXR: could not create a shared %ux%u D3D11 texture (0x%08lx)", w, h, (unsigned long)hr);
        XrDestroyImage(out);
        return false;
    }

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = kKmt;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.pNext = &ext;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = g_vk.createImage(m_vkDevice, &ii, nullptr, &out.image);
    const char *step = "vkCreateImage";
    VkMemoryRequirements req{};
    MemoryWin32HandleProperties hp{kMemoryWin32HandleProperties, nullptr, 0};
    if (r == VK_SUCCESS)
    {
        g_vk.imageRequirements(m_vkDevice, out.image, &req);
        step = "vkGetMemoryWin32HandlePropertiesKHR";
        r = g_vk.handleProperties(m_vkDevice, kKmt, out.handle, &hp);
    }
    uint32_t type = UINT32_MAX;
    if (r == VK_SUCCESS)
    {
        uint32_t bits = req.memoryTypeBits & (hp.memoryTypeBits ? hp.memoryTypeBits : ~0u);
        type = PickMemoryType(m_physDev, bits);
        step = "memory type";
        if (type == UINT32_MAX)
            r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (r == VK_SUCCESS)
    {
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = out.image;
        ImportMemoryWin32HandleInfo import{kImportMemoryWin32HandleInfo, &dedicated, kKmt, out.handle, nullptr};
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.pNext = &import;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        step = "vkAllocateMemory (import)";
        r = g_vk.allocateMemory(m_vkDevice, &ai, nullptr, &out.memory);
    }
    if (r == VK_SUCCESS)
    {
        step = "vkBindImageMemory";
        r = g_vk.bindImageMemory(m_vkDevice, out.image, out.memory, 0);
    }
    if (r != VK_SUCCESS)
    {
        IVR_ERR("OpenXR: importing a %ux%u shared texture into Vulkan failed at %s (%d)", w, h, step, (int)r);
        XrDestroyImage(out);
        return false;
    }
    return true;
}

void D3DVR::XrDestroyImage(XrImage &img)
{
    if (img.image)
        g_vk.destroyImage(m_vkDevice, img.image, nullptr);
    if (img.memory)
        g_vk.freeMemory(m_vkDevice, img.memory, nullptr);
    if (img.d3d11)
        img.d3d11->Release();
    img = XrImage();
}

bool D3DVR::XrCreateCopier(XrCopier &c)
{
    if (c.cmd)
        return true;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_xrPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g_vk.allocateCommandBuffers(m_vkDevice, &ai, &c.cmd) != VK_SUCCESS)
    {
        c.cmd = VK_NULL_HANDLE;
        return false;
    }
    if (g_vk.createFence(m_vkDevice, &fi, nullptr, &c.fence) != VK_SUCCESS)
    {
        c.fence = VK_NULL_HANDLE;
        XrDestroyCopier(c);
        return false;
    }
    c.submitted = false;
    return true;
}

void D3DVR::XrDestroyCopier(XrCopier &c)
{
    if (c.fence && c.submitted)
        g_vk.waitForFences(m_vkDevice, 1, &c.fence, VK_TRUE, 1000000000ull);
    if (c.fence)
        g_vk.destroyFence(m_vkDevice, c.fence, nullptr);
    if (c.cmd)
        g_vk.freeCommandBuffers(m_vkDevice, m_xrPool, 1, &c.cmd);
    c = XrCopier();
}

// Copies src[i] (w x h from the top left) into dst[i] on DXVK's queue; c.fence signals when done.
bool D3DVR::XrCopy(XrCopier &c, const VrTexture *const *src, XrImage *const *dst, int count, UINT w, UINT h)
{
    if (c.submitted)
    {
        // The last copy into these images must be finished before they are written again.
        if (g_vk.waitForFences(m_vkDevice, 1, &c.fence, VK_TRUE, 50000000ull) != VK_SUCCESS)
            return false;
        c.submitted = false;
    }
    g_vk.resetFences(m_vkDevice, 1, &c.fence);
    g_vk.resetCommandBuffer(c.cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_vk.beginCommandBuffer(c.cmd, &bi);

    // DXVK's rendering into the sources (submitted before ours on the same queue) must be done.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkImageMemoryBarrier ib[2]{};
    for (int i = 0; i < count; ++i)
    {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib[i].srcAccessMask = 0;
        ib[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // overwritten completely
        ib[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = dst[i]->image;
        ib[i].subresourceRange = kColor;
    }
    g_vk.cmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                            0, nullptr, (uint32_t)count, ib);
    for (int i = 0; i < count; ++i)
    {
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = kColorLayers;
        region.extent = {w < src[i]->width ? w : src[i]->width, h < src[i]->height ? h : src[i]->height, 1};
        g_vk.cmdCopyImage(c.cmd, src[i]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst[i]->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    for (int i = 0; i < count; ++i)
    {
        ib[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL; // what D3D11 sees
    }
    g_vk.cmdPipelineBarrier(c.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                            nullptr, 0, nullptr, (uint32_t)count, ib);
    if (g_vk.endCommandBuffer(c.cmd) != VK_SUCCESS)
        return false;

    VrTexture *srcs[2] = {const_cast<VrTexture *>(src[0]), count > 1 ? const_cast<VrTexture *>(src[1]) : nullptr};
    BeginVulkanAccess(srcs, count);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c.cmd;
    VkResult r = g_vk.queueSubmit(m_queue, 1, &si, c.fence);
    EndVulkanAccess(srcs, count);
    if (r != VK_SUCCESS)
    {
        static int s_errors = 0;
        if (s_errors++ < 5)
            IVR_ERR("OpenXR: vkQueueSubmit for the shared texture copy failed (%d)", (int)r);
        return false;
    }
    c.submitted = true;
    return true;
}

void D3DVR::XrRelease()
{
    if (ivrxr::Shared *s = Shm())
    {
        s->eyeSeq = 0; // the textures are going: the helper must not open them
        for (auto &l : s->layers)
            if (l.kind == ivrxr::LAYER_SHARED)
                l.visible = 0;
    }
    if (m_xrInit > 0)
    {
        for (auto &s : m_xrSlots)
        {
            XrDestroyCopier(s.copier); // waits for its copy
            for (auto &img : s.img)
                XrDestroyImage(img);
        }
        for (auto &p : m_xrPanels)
        {
            XrDestroyCopier(p.copier);
            XrDestroyImage(p.img);
            p = XrPanel();
        }
    }
    m_xrPending = -1;
    m_xrW = m_xrH = 0;
}

void D3DVR::XrForgetDevice()
{
    // The old VkDevice is gone with the old D3D9 device: drop the handles without destroying them.
    for (auto &s : m_xrSlots)
    {
        for (auto &img : s.img)
        {
            if (img.d3d11)
                img.d3d11->Release();
            img = XrImage();
        }
        s.copier = XrCopier();
    }
    for (auto &p : m_xrPanels)
    {
        if (p.img.d3d11)
            p.img.d3d11->Release();
        p = XrPanel();
    }
    if (m_xrD3D11)
        m_xrD3D11->Release();
    m_xrD3D11 = nullptr;
    m_xrPool = VK_NULL_HANDLE;
    m_xrInit = 0;
    m_xrFailed = false;
    m_xrPending = -1;
    m_xrW = m_xrH = 0;
}

bool D3DVR::XrEnsureSlots(UINT w, UINT h)
{
    if (m_xrFailed)
        return false;
    if (m_xrW == w && m_xrH == h && m_xrSlots[0].img[0].image)
        return true;
    if (!XrInitInterop())
    {
        m_xrFailed = true;
        return false;
    }
    XrRelease();
    for (auto &s : m_xrSlots)
    {
        bool ok = XrCreateCopier(s.copier);
        for (int e = 0; e < 2 && ok; ++e)
            ok = XrCreateImage(w, h, s.img[e]);
        if (!ok)
        {
            m_xrFailed = true;
            XrRelease();
            IVR_ERR("OpenXR: nothing can be shown in the headset - set vr.backend = openvr");
            return false;
        }
    }
    m_xrW = w;
    m_xrH = h;
    IVR_LOG("OpenXR: %d shared eye texture pairs, %ux%u", ivrxr::kEyeSlots, w, h);
    return true;
}

// Publishes the pending slot once the GPU is done with it.
void D3DVR::XrHandOver()
{
    ivrxr::Shared *s = Shm();
    if (!s || m_xrPending < 0)
        return;
    XrCopier &c = m_xrSlots[m_xrPending].copier;
    if (c.submitted && g_vk.getFenceStatus(m_vkDevice, c.fence) != VK_SUCCESS)
        return;
    int slot = m_xrPending;
    for (int e = 0; e < 2; ++e)
        s->eyeHandle[slot][e] = (uint64_t)(uintptr_t)m_xrSlots[slot].img[e].handle;
    s->eyeWidth = m_xrW;
    s->eyeHeight = m_xrH;
    std::memcpy(s->renderHead[slot].m, m_xrPendingPose.m, sizeof(s->renderHead[slot].m));
    s->renderHead[slot].valid = 1;
    // eyeSeq % kEyeSlots must name the slot.
    uint32_t seq = m_xrSeq + 1;
    while (seq % ivrxr::kEyeSlots != (uint32_t)slot)
        ++seq;
    m_xrSeq = seq;
    MemoryBarrier();
    s->eyeSeq = seq;
    m_xrPending = -1;
    // A few hundred eye pairs went through without a crash: trust the mechanism.
    if (!m_xrTrusted && seq > 600)
    {
        m_xrTrusted = true;
        flags::End(kFlag);
    }
}

void D3DVR::XrPublishEyes(const vr::HmdMatrix34_t &pose)
{
    ivrxr::Shared *s = Shm();
    UINT w = g_VR.ViewW(), h = g_VR.ViewH();
    if (!s || !XrEnsureSlots(w, h))
        return;
    XrHandOver();
    if (m_xrPending >= 0)
        return; // the GPU has not finished the last copy yet: skip this one
    // A slot the helper is not reading and that is not the published one.
    uint32_t published = s->eyeSeq ? s->eyeSeq % ivrxr::kEyeSlots : 0xFFFFFFFFu;
    int slot = -1;
    for (int i = 0; i < ivrxr::kEyeSlots && slot < 0; ++i)
        if ((uint32_t)i != published && (uint32_t)i != s->helperUsingSlot)
            slot = i;
    if (slot < 0)
        return;
    const VrTexture *src[2] = {&eye[0], &eye[1]};
    XrImage *dst[2] = {&m_xrSlots[slot].img[0], &m_xrSlots[slot].img[1]};
    if (!XrCopy(m_xrSlots[slot].copier, src, dst, 2, w, h))
        return;
    m_xrPending = slot;
    m_xrPendingPose = pose;
    ++m_submits;
}

void D3DVR::XrPublishPanel(vr::VROverlayHandle_t overlay, const VrTexture &src)
{
    if (overlay == vr::k_ulOverlayHandleInvalid || !src.Valid() || m_xrFailed)
        return;
    XrPanel *p = nullptr;
    for (auto &c : m_xrPanels)
        if (c.overlay == overlay || (!p && c.overlay == vr::k_ulOverlayHandleInvalid))
            p = &c;
    if (!p)
        return;
    if (!p->img.image || p->w != src.width || p->h != src.height)
    {
        if (!XrInitInterop())
        {
            m_xrFailed = true;
            return;
        }
        XrDestroyCopier(p->copier);
        XrDestroyImage(p->img);
        *p = XrPanel();
        if (!XrCreateCopier(p->copier) || !XrCreateImage(src.width, src.height, p->img))
        {
            XrDestroyCopier(p->copier);
            m_xrFailed = true;
            IVR_ERR("OpenXR: nothing can be shown in the headset - set vr.backend = openvr");
            return;
        }
        p->overlay = overlay;
        p->w = src.width;
        p->h = src.height;
        IVR_LOG("OpenXR: shared panel texture %ux%u", p->w, p->h);
    }
    const VrTexture *s[1] = {&src};
    XrImage *d[1] = {&p->img};
    if (!XrCopy(p->copier, s, d, 1, p->w, p->h))
        return;
    g_VR.Overlays()->SetOverlaySharedTexture(overlay, (uint64_t)(uintptr_t)p->img.handle, p->w, p->h);
}

void D3DVR::XrShowEyes(bool on)
{
    if (ivrxr::Shared *s = Shm())
        s->eyesVisible = on ? 1 : 0;
}

} // namespace ivr
