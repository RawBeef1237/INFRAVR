// INFRA VR - D3D9 (DXVK) side: textures handed to SteamVR through DXVK's public
// D3D9 <-> Vulkan interop interfaces (works with the stock DXVK release).
#pragma once
#include "common.h"
#include "runtime.h"
#include "openxr_bridge.h"

#include <d3d9.h>

struct ID3D11Device;
struct ID3D11Texture2D;

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace ivr {

// {d56344f5-8d35-46fd-806d-94c351b472c1}
static const GUID IID_ID3D9VkInteropTexture = {
    0xd56344f5, 0x8d35, 0x46fd, {0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1}};
// {2eaa4b89-0107-4bdb-87f7-0f541c493ce0}
static const GUID IID_ID3D9VkInteropDevice = {
    0x2eaa4b89, 0x0107, 0x4bdb, {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};

struct ID3D9VkInteropTexture : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage *pHandle, VkImageLayout *pLayout,
                                                         VkImageCreateInfo *pInfo) = 0;
};

struct ID3D9VkInteropDevice : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *pInstance, VkPhysicalDevice *pPhysDev,
                                                    VkDevice *pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *pQueue, uint32_t *pQueueIndex,
                                                      uint32_t *pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture *pTexture,
                                                           const VkImageSubresourceRange *pSubresources,
                                                           VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9 *pResource, DWORD MapFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateImage(const void *desc, IDirect3DResource9 **ppResult) = 0;
};

// A D3D9 texture whose Vulkan image can be given to SteamVR.
struct VrTexture
{
    IDirect3DTexture9 *d3d = nullptr;         // not owned when captured from the engine
    ID3D9VkInteropTexture *interop = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool owned = false;

    bool Attach(IDirect3DTexture9 *tex, bool takeOwnership);
    void Reset();
    bool Valid() const { return image != VK_NULL_HANDLE; }
};

class D3DVR
{
public:
    void OnDeviceCreated(IDirect3DDevice9 *device);
    void OnBeforeReset();
    void OnPresent(); // called right before the real Present
    void OnAfterPresent(); // called right after it

    IDirect3DDevice9 *device = nullptr;
    bool interopOk = false;

    // Engine-owned eye/HUD textures (captured when the engine creates them).
    VrTexture eye[2];
    VrTexture hud;
    // Our own textures.
    VrTexture blank, menu;
    // Subtitle panel: subsRaw is the subtitle region copied out of the HUD texture while the game
    // paints (render queue), subs the same picture with its alpha rebuilt, sent to the headset.
    VrTexture subsRaw, subs;
    void CopySubtitles(); // on the render queue, right after the in-game HUD was painted

    // Set while the engine creates one of our render targets (see CreateTexture hook).
    int capturing = -1; // 0/1 eye, 2 hud
    // One-frame trace of render target switches/copies while the game draws the left eye
    // (diagnostics for the stretched-lines problem).
    bool trace = false;
    // The camera weapon's screen texture (_rt_CameraWeapon): DrawViewModels copies the frame into
    // it. Learned during our lens view; while blockCameraCopy is set the eye views may not overwrite it.
    bool learnCameraCopy = false;
    LONG learnW = 0, learnH = 0; // the lens view size: the camera copy's destination rect
    bool blockCameraCopy = false;
    IDirect3DSurface9 *cameraSurface = nullptr;
    IDirect3DSurface9 *cameraTemp = nullptr; // lens view, resolved, before it is stretched into cameraSurface
    int traceDraws = 0;
    void TraceSurface(const char *what, IDirect3DSurface9 *s, const RECT *r = nullptr);
    // Saves the bound render target 0 (or the texture on stage 0) as infravr\<name>.bmp, half size.
    void DumpBound(bool renderTarget, const char *name);
    void OnTextureCreated(IDirect3DTexture9 *tex, UINT w, UINT h, DWORD usage);
    // Fallback: identify the texture currently bound as render target 0.
    bool CaptureBoundRenderTarget(VrTexture &out, UINT width, UINT height);
    // Once per eye: checks that the texture the game drew into is the one sent to SteamVR.
    void CheckBoundRenderTarget(int which);
    bool BackBufferBound() const; // the game window is the current render target

    // Set when the HUD panel turned out to be opaque (it would cover the view): HUD overlay off.
    bool hudBroken = false;

    // Antialiasing: while the game draws into an eye texture it really draws into a multisampled
    // stand-in (with a multisampled depth buffer); the stand-in is resolved into the eye texture
    // before the frame goes to the headset, and copies the game makes from the eye read it too.
    IDirect3DSurface9 *MapRenderTarget(IDirect3DSurface9 *s);
    IDirect3DSurface9 *MapDepth(IDirect3DSurface9 *s);
    IDirect3DSurface9 *MapCopySource(IDirect3DSurface9 *s);
    IDirect3DSurface9 *MapCopyDest(IDirect3DSurface9 *s);
    void ResolveEyes();
    // Head pose the eyes about to be presented were drawn for (sent with them, so SteamVR
    // reprojects correctly even when the frame is presented later, as with multicore rendering).
    void SetRenderPose(const vr::HmdMatrix34_t &p)
    {
        m_renderPose = p;
        m_haveRenderPose = true;
    }
    bool resolving = false; // our own resolve copies: no mapping

private:
    struct ProbeStats
    {
        bool ok = false;
        float avg = 0, max = 0; // brightness 0..255
        float opaque = 0;       // fraction of pixels with alpha > 200
        int rowsSame = 0, colsSame = 0; // neighbouring rows/columns that are identical (smeared lines)
        uint32_t hash = 0;              // of the downscaled picture: same hash = same picture
    };
    ProbeStats Probe(const VrTexture &t, const vr::VRTextureBounds_t *bounds);
    void ProbeFrame(bool stereo, bool hudOn);
    void LogHudContent();
    bool MeasureSmear(const VrTexture &t, const char *what, UINT &picW, UINT &picH);
    void MakeOpaque(const VrTexture &t);
    bool BuildSubtitlePanel(); // subsRaw -> subs at Present
    void ReleaseSubtitles();
    IDirect3DPixelShader9 *m_subsPS = nullptr;
    bool m_subsPSFailed = false;
    bool m_subsShown = false; // the subtitle panel went to the headset this frame (probe)
    int m_subsLogs = 0;
    void ReleaseHelpers();
    bool EnsureOwnTextures();
    void RefreshImages(VrTexture *const *textures, int count); // DXVK may move images
    void BeginVulkanAccess(VrTexture *const *textures, int count);
    void EndVulkanAccess(VrTexture *const *textures, int count);
    vr::VRVulkanTextureData_t VulkanData(const VrTexture &t) const;
    void CopyBackBufferToMenu();
    IDirect3DSurface9 *EyeSurface(int e);
    IDirect3DSurface9 *BoundRenderTarget(); // render target 0 (AddRef'd), stand-ins mapped back to the eye
    void ReleaseMsaa();
    struct MsaaEye
    {
        IDirect3DTexture9 *tex = nullptr;  // eye texture the cached surface belongs to
        IDirect3DSurface9 *surf = nullptr; // its level 0 (not owned)
        IDirect3DSurface9 *ms = nullptr;   // multisampled stand-in (owned)
        bool dirty = false;                // drawn into since the last resolve
    } m_msaa[2];
    struct MsaaDepth
    {
        IDirect3DSurface9 *src = nullptr; // the game's depth buffer (not owned)
        IDirect3DSurface9 *ms = nullptr;  // owned
    } m_msaaDepth[4];
    bool m_msaaFailed = false;
    bool m_msaaLogged = false;
    void MirrorEyeToBackBuffer();

    // OpenXR backend: the eye images and panels go to the helper process as shared textures
    // (xr_publish.cpp). DXVK cannot export D3D9 shared textures that D3D11 can open on Windows, so
    // the textures are made by a small D3D11 device of our own (legacy shared handles, opened by the
    // helper with OpenSharedResource) and imported into DXVK's Vulkan device; our own command buffer
    // on DXVK's queue copies the eye images into them. An eye pair is copied into a free slot and
    // handed over at a later Present, once its fence says the GPU has finished the copy.
    struct XrImage
    {
        ID3D11Texture2D *d3d11 = nullptr;
        HANDLE handle = nullptr; // KMT shared handle for the helper
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    struct XrCopier // command buffer + fence for one destination
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool submitted = false;
    };
    struct XrSlot
    {
        XrImage img[2];
        XrCopier copier;
    } m_xrSlots[ivrxr::kEyeSlots];
    UINT m_xrW = 0, m_xrH = 0;
    int m_xrPending = -1; // slot copied, waiting for the GPU
    vr::HmdMatrix34_t m_xrPendingPose{};
    uint32_t m_xrSeq = 0;
    struct XrPanel
    {
        vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
        XrImage img;
        XrCopier copier;
        UINT w = 0, h = 0;
    } m_xrPanels[2];
    bool m_xrFailed = false;
    uint32_t m_xrBusy = 0, m_xrNoSlot = 0, m_xrLastSubmits = 0; // eye pairs not sent (pacing log)
    double m_xrStatsAt = 0;
    bool m_xrTrusted = false;               // the crash-sticky flag has been cleared
    int m_xrInit = 0;                       // 0 not tried, 1 ready, -1 failed
    ID3D11Device *m_xrD3D11 = nullptr;
    VkCommandPool m_xrPool = VK_NULL_HANDLE;
    bool XrInitInterop();
    bool XrCreateImage(UINT w, UINT h, XrImage &out);
    void XrDestroyImage(XrImage &img);
    bool XrCreateCopier(XrCopier &c);
    void XrDestroyCopier(XrCopier &c);
    bool XrCopy(XrCopier &c, const VrTexture *const *src, XrImage *const *dst, int count, UINT w, UINT h);
    bool XrEnsureSlots(UINT w, UINT h);
    void XrHandOver();
    void XrPublishEyes(const vr::HmdMatrix34_t &pose);
    void XrPublishPanel(vr::VROverlayHandle_t overlay, const VrTexture &src);
    void XrShowEyes(bool on);
    void XrRelease();
    void XrForgetDevice(); // a new D3D9 device: the old Vulkan objects went with the old one

    ID3D9VkInteropDevice *m_interop = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDev = VK_NULL_HANDLE;
    VkDevice m_vkDevice = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    UINT m_bbWidth = 0, m_bbHeight = 0;
    IDirect3DStateBlock9 *m_stateBlock = nullptr;
    IDirect3DSurface9 *m_probeRT = nullptr, *m_probeSys = nullptr;
    double m_nextProbe = 0;
    int m_probes = 0;
    int m_blackEyeProbes = 0;
    int m_smearChecks = 0;
    void LogCompositorStats();
    uint32_t m_lastEyeHash = 0;
    int m_sameEyeProbes = 0;
    uint32_t m_submits = 0;                 // stereo frames handed to SteamVR
    // Last stereo frame's head pose, so a frame can be shown again (with correct reprojection)
    // when the game skips the 3D view while its pause/phone menu is up.
    vr::HmdMatrix34_t m_lastEyePose{};
    vr::HmdMatrix34_t m_renderPose{};
    double m_firstPresent = -1;
    bool m_submitReady = false;
    bool m_haveRenderPose = false;
    bool m_haveLastEyes = false;
    int m_phoneFrames = 0, m_phoneReused = 0;
    uint32_t m_lastSubmits = 0, m_lastPresents = 0, m_lastDropped = 0, m_lastReprojected = 0, m_lastTimedOut = 0;
    bool m_haveStats = false;
};

extern D3DVR g_D3D;

} // namespace ivr
