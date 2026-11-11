// INFRA VR - d3d9.dll proxy (INFRA\bin\d3d9.dll).
//
// shaderapidx9.dll loads "d3d9.dll" from INFRA\bin. That is this DLL. It
//   1. loads openvr_api.dll and initialises SteamVR,
//   2. chain-loads stock DXVK (INFRA\bin\dxvk_d3d9.dll) - DXVK sees the loaded OpenVR
//      runtime and creates its Vulkan device with the extensions SteamVR needs,
//   3. hooks the D3D9 device (Present/Reset/CreateTexture) and, once client.dll is up,
//      the Source engine (see hooks_game.cpp).
#include "common.h"
#include "config.h"
#include "flags.h"
#include "world_size.h"
#include "hands_renderer.h"
#include "gamepad.h"
#include "d3d_vr.h"
#include "game.h"
#include "hooks.h"
#include "runtime.h"
#include "mod.h"

#include <d3d9.h>
#include <dxgi.h>
#include <vulkan/vulkan_core.h>
#include <cstring>
#include <vector>
#include <cstdio>

using namespace ivr;

namespace ivr {
bool InstallClientHooks();
void ResetStereoGuard();
bool InstallServerHooks();
} // namespace ivr

namespace {

HMODULE g_realD3D9 = nullptr;
bool g_vrInitDone = false;
// False in a second copy of this DLL loaded into the same process: that copy only forwards to
// DXVK (two VR sessions in one game wedge SteamVR - Black Mesa VR saw the same).
bool g_active = true;
HANDLE g_instanceMutex = nullptr;

using Direct3DCreate9Fn = IDirect3D9 *(WINAPI *)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
using CreateDeviceExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                      D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *,
                                                      IDirect3DDevice9Ex **);
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using PresentExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND,
                                                 const RGNDATA *, DWORD);
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using ResetExFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9Ex *, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *);
using CreateTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                     IDirect3DTexture9 **, HANDLE *);

Hook<CreateDeviceFn> g_hkCreateDevice;
Hook<CreateDeviceExFn> g_hkCreateDeviceEx;
Hook<PresentFn> g_hkPresent;
Hook<PresentExFn> g_hkPresentEx;
Hook<ResetFn> g_hkReset;
Hook<ResetExFn> g_hkResetEx;
Hook<CreateTextureFn> g_hkCreateTexture;

// Trace-only device hooks (active for one eye render, see D3DVR::trace)
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
using SetViewportFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
using StretchRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
                                                   IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
using SetDepthStencilFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *);
using DrawIndexedFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
Hook<SetRenderTargetFn> g_hkSetRenderTarget;
Hook<SetViewportFn> g_hkSetViewport;
Hook<StretchRectFn> g_hkStretchRect;
Hook<SetDepthStencilFn> g_hkSetDepthStencil;
Hook<DrawIndexedFn> g_hkDrawIndexed;
Hook<DrawFn> g_hkDraw;

void TraceDraws()
{
    if (g_D3D.traceDraws > 0)
        IVR_LOG("  trace:   ... %d draws", g_D3D.traceDraws);
    g_D3D.traceDraws = 0;
}

// The depth buffer the game asked for, and whether render target 0 is a multisampled stand-in.
// Its multisampled depth buffer is used only with a multisampled render target: an eye-sized plain
// target (the selection glow's _rt_FullFrameFB) with a multisampled depth buffer draws nothing.
IDirect3DSurface9 *g_appDepth = nullptr; // referenced
IDirect3DSurface9 *g_boundDepth = nullptr;
bool g_rt0Multisampled = false;

HRESULT ApplyDepth(IDirect3DDevice9 *self, bool force)
{
    IDirect3DSurface9 *want = g_rt0Multisampled ? g_D3D.MapDepth(g_appDepth) : g_appDepth;
    if (!force && want == g_boundDepth)
        return D3D_OK;
    g_boundDepth = want;
    return g_hkSetDepthStencil.original(self, want);
}

void ForgetDepth()
{
    if (g_appDepth)
        g_appDepth->Release();
    g_appDepth = g_boundDepth = nullptr;
    g_rt0Multisampled = false;
}

HRESULT STDMETHODCALLTYPE hkSetRenderTarget(IDirect3DDevice9 *self, DWORD index, IDirect3DSurface9 *surf)
{
    if (g_D3D.trace)
    {
        TraceDraws();
        char what[32];
        sprintf(what, "SetRenderTarget[%lu]", (unsigned long)index);
        g_D3D.TraceSurface(what, surf);
    }
    if (index != 0)
        return g_hkSetRenderTarget.original(self, index, surf);
    IDirect3DSurface9 *mapped = g_D3D.MapRenderTarget(surf);
    g_rt0Multisampled = mapped != surf;
    HRESULT hr = g_hkSetRenderTarget.original(self, index, mapped);
    ApplyDepth(self, false);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkSetDepthStencil(IDirect3DDevice9 *self, IDirect3DSurface9 *surf)
{
    if (g_D3D.trace)
        g_D3D.TraceSurface("SetDepthStencil", surf);
    if (surf != g_appDepth)
    {
        if (surf)
            surf->AddRef();
        if (g_appDepth)
            g_appDepth->Release();
        g_appDepth = surf;
    }
    return ApplyDepth(self, true);
}

} // namespace

// The stencil buffer bound now, as a picture (white = not 0): the selection glow is added only where
// it is 0, so this shows what hides the glow. Drawn with a full-screen quad into a multisampled
// target the size of the bound depth buffer, then saved like the other pictures.
void D3DVR::DumpStencil(const char *name)
{
    IDirect3DDevice9 *dev = device;
    IDirect3DSurface9 *ds = nullptr, *rt0 = nullptr;
    if (!dev || FAILED(dev->GetDepthStencilSurface(&ds)) || !ds)
    {
        IVR_LOG("dump %s: no depth buffer bound", name);
        return;
    }
    dev->GetRenderTarget(0, &rt0);
    D3DSURFACE_DESC dd{};
    ds->GetDesc(&dd);
    IDirect3DSurface9 *tmp = nullptr;
    IDirect3DStateBlock9 *sb = nullptr;
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    if (SUCCEEDED(dev->CreateRenderTarget(dd.Width, dd.Height, D3DFMT_A8R8G8B8, dd.MultiSampleType,
                                          dd.MultiSampleQuality, FALSE, &tmp, nullptr)) &&
        SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb)))
    {
        g_hkSetRenderTarget.original(dev, 0, tmp);
        g_hkSetDepthStencil.original(dev, ds);
        D3DVIEWPORT9 full = {0, 0, dd.Width, dd.Height, 0.0f, 1.0f};
        dev->SetViewport(&full);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        dev->SetTexture(0, nullptr);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
        dev->SetRenderState(D3DRS_STENCILREF, 0);
        dev->SetRenderState(D3DRS_STENCILMASK, 0xff);
        dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
        dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
        dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        struct V
        {
            float x, y, z, w;
            DWORD c;
        };
        const float W = (float)dd.Width, H = (float)dd.Height;
        const DWORD white = D3DCOLOR_ARGB(255, 255, 255, 255);
        V q[4] = {{-0.5f, -0.5f, 0, 1, white}, {W - 0.5f, -0.5f, 0, 1, white}, {-0.5f, H - 0.5f, 0, 1, white},
                  {W - 0.5f, H - 0.5f, 0, 1, white}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
        DumpSurface(tmp, name);
        sb->Apply();
        g_hkSetRenderTarget.original(dev, 0, rt0);
        g_hkSetDepthStencil.original(dev, ds);
        dev->SetViewport(&vp);
    }
    else
        IVR_LOG("dump %s: could not make the picture", name);
    if (sb)
        sb->Release();
    if (tmp)
        tmp->Release();
    if (rt0)
        rt0->Release();
    ds->Release();
}

namespace {

HRESULT STDMETHODCALLTYPE hkSetViewport(IDirect3DDevice9 *self, const D3DVIEWPORT9 *vp)
{
    if (g_D3D.trace && vp)
    {
        TraceDraws();
        IVR_LOG("  trace: SetViewport %lu,%lu %lux%lu", vp->X, vp->Y, vp->Width, vp->Height);
    }
    return g_hkSetViewport.original(self, vp);
}

// INFRA's viewfinder shows all of _rt_CameraWeapon, which world_eye_size made eye-sized, while the
// lens view only fills its top left: stretch it over the whole texture, or the rest keeps the eye
// view copied in when the camera came out. The eye image is multisampled, so it is resolved at its
// own size into a plain surface first, and stretched from there (a stretched copy straight from the
// multisampled image froze the game). Crash-sticky: a hang turns it off for the next launch.
bool StretchLensView(IDirect3DDevice9 *self, IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst,
                     const D3DSURFACE_DESC &dd)
{
    static const char *kFlag = "camera_stretch";
    static int s_state = 0; // 0 not tried, 1 on trial, 2 trusted, -1 off
    static int s_copies = 0;
    if (s_state < 0)
        return false;
    if (s_state == 0)
        s_state = flags::Begin(kFlag) ? 1 : -1;
    if (s_state < 0)
        return false;
    LONG w = g_D3D.learnW, h = g_D3D.learnH;
    D3DSURFACE_DESC td{};
    if (g_D3D.cameraTemp && (FAILED(g_D3D.cameraTemp->GetDesc(&td)) || (LONG)td.Width != w || (LONG)td.Height != h ||
                             td.Format != dd.Format))
    {
        g_D3D.cameraTemp->Release();
        g_D3D.cameraTemp = nullptr;
    }
    if (!g_D3D.cameraTemp &&
        FAILED(self->CreateRenderTarget((UINT)w, (UINT)h, dd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_D3D.cameraTemp,
                                        nullptr)))
        g_D3D.cameraTemp = nullptr;
    RECT whole = {0, 0, w, h};
    HRESULT hr = g_D3D.cameraTemp ? g_hkStretchRect.original(self, g_D3D.MapCopySource(src), sr ? sr : &whole,
                                                             g_D3D.cameraTemp, nullptr, D3DTEXF_NONE)
                                  : E_FAIL;
    if (SUCCEEDED(hr))
        hr = g_hkStretchRect.original(self, g_D3D.cameraTemp, nullptr, g_D3D.MapCopyDest(dst), nullptr, D3DTEXF_LINEAR);
    if (s_copies == 0)
        IVR_LOG("Camera screen: lens view %ldx%ld stretched over the whole %ux%u texture: %s", w, h, dd.Width, dd.Height,
                SUCCEEDED(hr) ? "ok" : "refused, copied as INFRA does");
    if (FAILED(hr))
    {
        flags::Skip(kFlag, "the device refused the copy");
        s_state = -1;
        return false;
    }
    if (++s_copies == 300 && s_state == 1)
    {
        flags::End(kFlag);
        s_state = 2;
    }
    return true;
}

HRESULT STDMETHODCALLTYPE hkStretchRect(IDirect3DDevice9 *self, IDirect3DSurface9 *src, const RECT *sr,
                                        IDirect3DSurface9 *dst, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
    if (g_D3D.trace)
    {
        TraceDraws();
        RECT none = {-1, -1, -1, -1};
        g_D3D.TraceSurface("StretchRect from", src, sr ? sr : &none);
        g_D3D.TraceSurface("              to", dst, dr ? dr : &none);
    }
    D3DSURFACE_DESC dd{};
    if (g_D3D.learnCameraCopy && dst && dr && dr->left == 0 && dr->top == 0 && dr->right == g_D3D.learnW &&
        dr->bottom == g_D3D.learnH && SUCCEEDED(dst->GetDesc(&dd)) && (LONG)dd.Width >= g_D3D.learnW &&
        (LONG)dd.Height >= g_D3D.learnH)
    {
        g_D3D.learnCameraCopy = false;
        if (g_D3D.cameraSurface != dst)
            IVR_LOG("Camera screen texture: %p %ux%u", (void *)dst, dd.Width, dd.Height);
        g_D3D.cameraSurface = dst;
        if (StretchLensView(self, src, sr, dst, dd))
            return D3D_OK;
    }
    else if (g_D3D.blockCameraCopy && dst && dst == g_D3D.cameraSurface)
        return D3D_OK; // keep the picture from the camera lens
    return g_hkStretchRect.original(self, g_D3D.MapCopySource(src), sr, g_D3D.MapCopyDest(dst), dr, f);
}

HRESULT STDMETHODCALLTYPE hkDrawIndexed(IDirect3DDevice9 *self, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si,
                                        UINT pc)
{
    if (g_D3D.trace)
        ++g_D3D.traceDraws;
    return g_hkDrawIndexed.original(self, t, bv, mi, nv, si, pc);
}

using SetRenderStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
using SetScissorFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *);
Hook<SetRenderStateFn> g_hkSetRenderState;
Hook<SetTextureFn> g_hkSetTexture;
Hook<SetScissorFn> g_hkSetScissor;
using ClearFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
Hook<ClearFn> g_hkClear;
HRESULT STDMETHODCALLTYPE hkClear(IDirect3DDevice9 *self, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z,
                                  DWORD st)
{
    if (g_D3D.trace)
    {
        TraceDraws();
        IVR_LOG("  trace: Clear%s%s%s %lu rects, stencil %lu", (flags & D3DCLEAR_TARGET) ? " colour" : "",
                (flags & D3DCLEAR_ZBUFFER) ? " depth" : "", (flags & D3DCLEAR_STENCIL) ? " stencil" : "",
                (unsigned long)n, (unsigned long)st);
    }
    return g_hkClear.original(self, n, r, flags, c, z, st);
}

// Traced: the stencil / colour-write / scissor states and the first texture of each draw, so the
// order of a multi-pass effect (INFRA's selection glow) can be followed in the log.
HRESULT STDMETHODCALLTYPE hkSetRenderState(IDirect3DDevice9 *self, D3DRENDERSTATETYPE st, DWORD v)
{
    if (g_D3D.trace)
    {
        const char *n = nullptr;
        switch (st)
        {
        case D3DRS_STENCILENABLE: n = "STENCILENABLE"; break;
        case D3DRS_STENCILFUNC: n = "STENCILFUNC"; break;
        case D3DRS_STENCILREF: n = "STENCILREF"; break;
        case D3DRS_STENCILPASS: n = "STENCILPASS"; break;
        case D3DRS_COLORWRITEENABLE: n = "COLORWRITE"; break;
        case D3DRS_SCISSORTESTENABLE: n = "SCISSORTEST"; break;
        case D3DRS_ZENABLE: n = "ZENABLE"; break;
        case D3DRS_ALPHABLENDENABLE: n = "BLEND"; break;
        default: break;
        }
        if (n)
        {
            TraceDraws();
            IVR_LOG("  trace: %s %lu", n, (unsigned long)v);
        }
    }
    return g_hkSetRenderState.original(self, st, v);
}

HRESULT STDMETHODCALLTYPE hkSetTexture(IDirect3DDevice9 *self, DWORD stage, IDirect3DBaseTexture9 *t)
{
    if (g_D3D.trace && stage == 0 && t && t->GetType() == D3DRTYPE_TEXTURE)
    {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DTexture9 *>(t)->GetLevelDesc(0, &d)) && (d.Usage & D3DUSAGE_RENDERTARGET))
        {
            TraceDraws();
            IVR_LOG("  trace: texture 0 = render target %p %ux%u", (void *)t, d.Width, d.Height);
        }
    }
    return g_hkSetTexture.original(self, stage, t);
}

HRESULT STDMETHODCALLTYPE hkSetScissor(IDirect3DDevice9 *self, const RECT *r)
{
    if (g_D3D.trace && r)
    {
        TraceDraws();
        IVR_LOG("  trace: scissor %ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
    }
    return g_hkSetScissor.original(self, r);
}

HRESULT STDMETHODCALLTYPE hkDraw(IDirect3DDevice9 *self, D3DPRIMITIVETYPE t, UINT sv, UINT pc)
{
    if (g_D3D.trace)
        ++g_D3D.traceDraws;
    return g_hkDraw.original(self, t, sv, pc);
}

// ---- VR / DXVK bring-up ---------------------------------------------------------------------
// On PCs with two GPUs (e.g. a CPU with built-in graphics plus a graphics card) DXVK may pick a
// different GPU than the one SteamVR runs on; handing SteamVR a picture from the other GPU can crash
// inside the graphics driver. Tell DXVK to use SteamVR's GPU unless the user chose one themselves.
void PickHeadsetGpu(const std::string &conf)
{
    char existing[256];
    if (GetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", existing, sizeof(existing)))
    {
        IVR_LOG("GPU: DXVK_FILTER_DEVICE_NAME is set to \"%s\", leaving it", existing);
        return;
    }
    if (FILE *f = fopen(conf.c_str(), "r"))
    {
        char line[512];
        bool userFilter = false;
        while (fgets(line, sizeof(line), f))
        {
            const char *p = line;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (strncmp(p, "dxvk.deviceFilter", 17) == 0)
                userFilter = true;
        }
        fclose(f);
        if (userFilter)
        {
            IVR_LOG("GPU: dxvk.deviceFilter is set in dxvk.conf, leaving it");
            return;
        }
    }
    int32_t adapter = -1;
    LUID want{};
    bool byLuid = g_VR.IsOpenXR() && g_VR.AdapterLuid(want);
    if (g_VR.UsesSteamVR())
        g_VR.m_system->GetDXGIOutputInfo(&adapter);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
    auto create = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1 *factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
    {
        IVR_LOG("GPU: could not list the GPUs (DXGI unavailable)");
        return;
    }
    LUID luid{};
    bool haveLuid = false;
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        a->Release();
        char name[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        bool match = byLuid ? memcmp(&d.AdapterLuid, &want, sizeof(want)) == 0 : (int32_t)i == adapter;
        IVR_LOG("GPU %u: %s%s", i, name, match ? "  <-- the headset runtime's GPU" : "");
        if (match)
        {
            luid = d.AdapterLuid;
            haveLuid = true;
        }
    }
    factory->Release();
    if (!haveLuid)
        return;

    // DXVK filters by its Vulkan device name, which can differ from the Windows name: find the
    // Vulkan device with the same LUID and use exactly its name.
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    auto gipa = vk ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr")) : nullptr;
    if (!gipa)
        return;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_MAKE_VERSION(1, 1, 0);
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (!createInstance || createInstance(&ci, nullptr, &inst) != VK_SUCCESS)
        return;
    auto enumDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto props2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(inst, "vkGetPhysicalDeviceProperties2"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    std::string chosen;
    int sameName = 0;
    if (enumDevices && props2)
    {
        uint32_t n = 0;
        enumDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        enumDevices(inst, &n, devs.data());
        std::vector<std::string> names;
        for (VkPhysicalDevice dev : devs)
        {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p.pNext = &id;
            props2(dev, &p);
            names.push_back(p.properties.deviceName);
            if (id.deviceLUIDValid && memcmp(id.deviceLUID, &luid, sizeof(luid)) == 0)
                chosen = p.properties.deviceName;
        }
        for (const std::string &nm : names)
            if (!chosen.empty() && nm.find(chosen) != std::string::npos)
                ++sameName;
        IVR_LOG("GPU: %u Vulkan device(s)", n);
    }
    if (destroy)
        destroy(inst, nullptr);
    if (chosen.empty())
    {
        IVR_LOG("GPU: SteamVR's GPU not found among the Vulkan devices - leaving the choice to DXVK");
        return;
    }
    if (sameName > 1)
    {
        IVR_LOG("GPU: %d GPUs are called \"%s\" - leaving the choice to DXVK", sameName, chosen.c_str());
        return;
    }
    SetEnvironmentVariableA("DXVK_FILTER_DEVICE_NAME", chosen.c_str());
    IVR_LOG("GPU: DXVK will render on \"%s\", the GPU SteamVR uses", chosen.c_str());
}

// DXVK's memory defragmentation moves textures to new VkImages and destroys the old ones. The
// images handed to SteamVR (eyes, HUD, menu, subtitle panel) are remembered by handle, and SteamVR
// given a destroyed one crashes in the GPU driver - it happened right after loading a save from
// another map, when a lot of memory is freed at once. Turned off through DXVK_CONFIG (read after
// dxvk.conf, so it wins) unless dxvk.conf or DXVK_CONFIG already says what to do.
void KeepDxvkImagesInPlace()
{
    static const char kKey[] = "dxvk.enableMemoryDefrag";
    char env[1024] = {};
    DWORD n = GetEnvironmentVariableA("DXVK_CONFIG", env, sizeof(env));
    if (n >= sizeof(env))
        return; // too long to edit safely: leave it alone
    if (strstr(env, kKey))
    {
        IVR_LOG("DXVK: memory defragmentation left as DXVK_CONFIG says (%s)", env);
        return;
    }
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("DXVK_CONFIG_FILE", path, sizeof(path)) && path[0])
    {
        if (FILE *f = fopen(path, "r"))
        {
            char line[512];
            bool set = false;
            while (!set && fgets(line, sizeof(line), f))
            {
                const char *p = line;
                while (*p == ' ' || *p == '\t')
                    ++p;
                set = *p != '#' && strstr(p, kKey) != nullptr;
            }
            fclose(f);
            if (set)
            {
                IVR_LOG("DXVK: memory defragmentation left as %s says", path);
                return;
            }
        }
    }
    std::string value = std::string(kKey) + " = False";
    if (n)
        value = std::string(env) + ";" + value;
    SetEnvironmentVariableA("DXVK_CONFIG", value.c_str());
    IVR_LOG("DXVK: memory defragmentation off (textures shown in the headset must keep their VkImage)");
}

void InitVROnce()
{
    if (g_vrInitDone || !g_active)
        return;
    g_vrInitDone = true;
    InstallGamepadHooks(); // before the engine's input system looks for controllers

    // DXVK options for VR (no vsync against the monitor, low latency) unless the user has their own.
    std::string conf = ModDir() + "cfg\\dxvk.conf";
    char existing[8];
    if (!GetEnvironmentVariableA("DXVK_CONFIG_FILE", existing, sizeof(existing)) &&
        GetFileAttributesA(conf.c_str()) != INVALID_FILE_ATTRIBUTES)
        SetEnvironmentVariableA("DXVK_CONFIG_FILE", conf.c_str());
    KeepDxvkImagesInPlace();

    flags::SetStage("VR runtime start");
    if (g_VR.Init())
    {
        IVR_LOG("VR runtime initialised (%s)", g_VR.BackendName());
        PickHeadsetGpu(conf);
        // Before the engine creates its render targets (that happens after the device exists).
        if (g_Config.worldAtEyeSize && !g_Config.renderFitWindow)
            world::Install(GetInterface("materialsystem.dll", "VMaterialSystem080"), g_VR.renderWidth,
                           g_VR.renderHeight);
    }
    else
        IVR_ERR("No VR runtime is available - INFRA will run flat. Start SteamVR and your headset first.");
    flags::SetStage("engine start");
}

HMODULE RealD3D9()
{
    if (g_realD3D9)
        return g_realD3D9;
    std::string dxvk = ModuleDir() + "dxvk_d3d9.dll";
    if (g_Config.dxvk == "gplall")
    {
        // DXVK-GPLALL 2.6.1 (the build l4d2vr uses), optional since 0.8.1
        std::string alt = ModuleDir() + "dxvk_d3d9_gplall.dll";
        if (GetFileAttributesA(alt.c_str()) != INVALID_FILE_ATTRIBUTES)
            dxvk = alt;
        else
            IVR_ERR("%s not found - using the stock DXVK", alt.c_str());
    }
    g_realD3D9 = LoadLibraryExA(dxvk.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_realD3D9)
    {
        IVR_LOG("Loaded %s", dxvk.c_str());
        return g_realD3D9;
    }
    IVR_ERR("%s not found (error %lu) - using the system d3d9.dll, the headset will stay black", dxvk.c_str(),
            GetLastError());
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    g_realD3D9 = LoadLibraryA((std::string(sys) + "\\d3d9.dll").c_str());
    return g_realD3D9;
}

template <typename T>
T RealProc(const char *name)
{
    HMODULE m = RealD3D9();
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

// ---- engine bring-up (main thread, from Present) ------------------------------------------------
void TryInitEngine()
{
    static bool clientDone = false, serverDone = false;
    static double nextTry = 0;
    if (serverDone && clientDone)
        return;
    double now = NowSeconds();
    if (now < nextTry)
        return;
    nextTry = now + 2.0;
    if (!clientDone && GetModuleHandleA("client.dll") && GetModuleHandleA("engine.dll"))
    {
        clientDone = true;
        if (g_Game.Init() && InstallClientHooks())
        {
            IVR_LOG("Engine hooks installed");
            flags::InstallCrashLogger(); // ahead of the engine's own crash handler
            g_Game.ApplyVRSettings(); // convars only; console commands go through the queue
            g_Mod.QueueCommand("mat_queue_mode 0");
            g_Mod.QueueCommand("r_drawviewmodel 1");
            hands::Enabled(); // load the hand models now rather than in the middle of a frame
        }
        else
            IVR_ERR("Engine hooks could not be installed - see above");
    }
    if (clientDone && !serverDone && GetModuleHandleA("server.dll"))
    {
        serverDone = true;
        if (!InstallServerHooks())
            IVR_ERR("Server hooks unavailable - picked-up objects will be held in front of your eyes");
    }
}

void OnPresent()
{
    TryInitEngine();
    g_D3D.OnPresent();
}

void AfterPresent()
{
    if (!g_VR.Ok() || !g_Mod.Active())
        return;
    ResetStereoGuard();
    g_D3D.OnAfterPresent();
    // With multicore rendering Present runs on the engine's render thread; the VR logic (poses,
    // input) then runs on the main thread at the start of each RenderView instead.
    g_Game.presentThread = GetCurrentThreadId();
    // WaitGetPoses right after Present hands the frame over by itself; only when the poses are read
    // somewhere else (multicore) does SteamVR need the explicit handoff.
    bool posesLater = g_Game.mainThread && g_Game.presentThread != g_Game.mainThread;
    if (g_VR.UsesSteamVR() && (posesLater || g_Config.explicitHandoff))
        g_VR.m_compositor->PostPresentHandoff();
    if (!g_Game.mainThread || g_Game.presentThread == g_Game.mainThread)
        g_Mod.OnFrame();
}

void AdjustPresentParams(D3DPRESENT_PARAMETERS *pp)
{
    if (pp && g_VR.Ok())
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; // the headset paces the game, not the monitor
}

// ---- device hooks -----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9 *self, const RECT *src, const RECT *dst, HWND wnd,
                                    const RGNDATA *dirty)
{
    if (!g_active)
        return g_hkPresent.original(self, src, dst, wnd, dirty);
    OnPresent();
    HRESULT hr = g_hkPresent.original(self, src, dst, wnd, dirty);
    AfterPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE hkPresentEx(IDirect3DDevice9Ex *self, const RECT *src, const RECT *dst, HWND wnd,
                                      const RGNDATA *dirty, DWORD flags)
{
    if (!g_active)
        return g_hkPresentEx.original(self, src, dst, wnd, dirty, flags);
    OnPresent();
    HRESULT hr = g_hkPresentEx.original(self, src, dst, wnd, dirty, flags);
    AfterPresent();
    return hr;
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9 *self, D3DPRESENT_PARAMETERS *pp)
{
    ForgetDepth();
    g_D3D.OnBeforeReset();
    AdjustPresentParams(pp);
    return g_hkReset.original(self, pp);
}

HRESULT STDMETHODCALLTYPE hkResetEx(IDirect3DDevice9Ex *self, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode)
{
    ForgetDepth();
    g_D3D.OnBeforeReset();
    AdjustPresentParams(pp);
    return g_hkResetEx.original(self, pp, mode);
}

HRESULT STDMETHODCALLTYPE hkCreateTexture(IDirect3DDevice9 *self, UINT w, UINT h, UINT levels, DWORD usage,
                                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared)
{
    HRESULT hr = g_hkCreateTexture.original(self, w, h, levels, usage, fmt, pool, out, shared);
    if (SUCCEEDED(hr) && out && *out && g_D3D.capturing >= 0)
        g_D3D.OnTextureCreated(*out, w, h, usage);
    return hr;
}

void HookDevice(IDirect3DDevice9 *device, HWND window)
{
    void **vt = *reinterpret_cast<void ***>(device);
    if (!g_hkPresent.Installed())
        g_hkPresent.Install("IDirect3DDevice9::Present", vt[17], reinterpret_cast<void *>(&hkPresent));
    if (!g_hkReset.Installed())
        g_hkReset.Install("IDirect3DDevice9::Reset", vt[16], reinterpret_cast<void *>(&hkReset));
    if (!g_hkCreateTexture.Installed())
        g_hkCreateTexture.Install("IDirect3DDevice9::CreateTexture", vt[23],
                                  reinterpret_cast<void *>(&hkCreateTexture));
    if (!g_hkStretchRect.Installed())
    {
        g_hkStretchRect.Install("IDirect3DDevice9::StretchRect", vt[34], reinterpret_cast<void *>(&hkStretchRect));
        g_hkSetRenderTarget.Install("IDirect3DDevice9::SetRenderTarget", vt[37],
                                    reinterpret_cast<void *>(&hkSetRenderTarget));
        g_hkSetDepthStencil.Install("IDirect3DDevice9::SetDepthStencilSurface", vt[39],
                                    reinterpret_cast<void *>(&hkSetDepthStencil));
        g_hkSetViewport.Install("IDirect3DDevice9::SetViewport", vt[47], reinterpret_cast<void *>(&hkSetViewport));
        g_hkDraw.Install("IDirect3DDevice9::DrawPrimitive", vt[81], reinterpret_cast<void *>(&hkDraw));
        g_hkDrawIndexed.Install("IDirect3DDevice9::DrawIndexedPrimitive", vt[82],
                                reinterpret_cast<void *>(&hkDrawIndexed));
        g_hkSetRenderState.Install("IDirect3DDevice9::SetRenderState", vt[57],
                                   reinterpret_cast<void *>(&hkSetRenderState));
        g_hkSetTexture.Install("IDirect3DDevice9::SetTexture", vt[65], reinterpret_cast<void *>(&hkSetTexture));
        g_hkSetScissor.Install("IDirect3DDevice9::SetScissorRect", vt[75], reinterpret_cast<void *>(&hkSetScissor));
        g_hkClear.Install("IDirect3DDevice9::Clear", vt[43], reinterpret_cast<void *>(&hkClear));
    }
    IDirect3DDevice9Ex *ex = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void **>(&ex))) && ex)
    {
        void **vtx = *reinterpret_cast<void ***>(ex);
        if (!g_hkPresentEx.Installed())
            g_hkPresentEx.Install("IDirect3DDevice9Ex::PresentEx", vtx[121], reinterpret_cast<void *>(&hkPresentEx));
        if (!g_hkResetEx.Installed())
            g_hkResetEx.Install("IDirect3DDevice9Ex::ResetEx", vtx[132], reinterpret_cast<void *>(&hkResetEx));
        ex->Release();
    }
    g_D3D.OnDeviceCreated(device);
    g_Mod.gameWindow = window;
    if (g_VR.Ok() && !g_Mod.Active())
        g_Mod.Init();
}

// The mod touches the device from the engine's thread and from Present, which is the material
// system's render thread whenever INFRA's own "multicore rendering" is on (even briefly at startup,
// before mat_queue_mode 0 takes effect), so let DXVK lock every call.
DWORD DeviceFlags(DWORD flags)
{
    IVR_LOG("D3D9 device flags 0x%lx%s", (unsigned long)flags,
            (flags & D3DCREATE_MULTITHREADED) ? "" : " (+ D3DCREATE_MULTITHREADED)");
    return flags | D3DCREATE_MULTITHREADED;
}

HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                         D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    AdjustPresentParams(pp);
    flags = DeviceFlags(flags);
    HRESULT hr = g_hkCreateDevice.original(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out)
    {
        IVR_LOG("D3D9 device created: %ux%u, windowed %d, MSAA %d", pp->BackBufferWidth, pp->BackBufferHeight,
                pp->Windowed, (int)pp->MultiSampleType);
        HookDevice(*out, pp->hDeviceWindow ? pp->hDeviceWindow : focus);
    }
    else
        IVR_ERR("CreateDevice failed: 0x%08lx", (unsigned long)hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(IDirect3D9Ex *self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                           DWORD flags, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode,
                                           IDirect3DDevice9Ex **out)
{
    AdjustPresentParams(pp);
    flags = DeviceFlags(flags);
    HRESULT hr = g_hkCreateDeviceEx.original(self, adapter, type, focus, flags, pp, mode, out);
    if (SUCCEEDED(hr) && out && *out)
    {
        IVR_LOG("D3D9Ex device created: %ux%u, windowed %d, MSAA %d", pp->BackBufferWidth, pp->BackBufferHeight,
                pp->Windowed, (int)pp->MultiSampleType);
        HookDevice(*out, pp->hDeviceWindow ? pp->hDeviceWindow : focus);
    }
    else
        IVR_ERR("CreateDeviceEx failed: 0x%08lx", (unsigned long)hr);
    return hr;
}

void HookInterface(IDirect3D9 *d3d)
{
    if (!d3d)
        return;
    void **vt = *reinterpret_cast<void ***>(d3d);
    if (!g_hkCreateDevice.Installed())
        g_hkCreateDevice.Install("IDirect3D9::CreateDevice", vt[16], reinterpret_cast<void *>(&hkCreateDevice));
    IDirect3D9Ex *ex = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void **>(&ex))) && ex)
    {
        void **vtx = *reinterpret_cast<void ***>(ex);
        if (!g_hkCreateDeviceEx.Installed())
            g_hkCreateDeviceEx.Install("IDirect3D9Ex::CreateDeviceEx", vtx[20],
                                       reinterpret_cast<void *>(&hkCreateDeviceEx));
        ex->Release();
    }
}

} // namespace

// ---- exports ------------------------------------------------------------------------------------
extern "C" {

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    InitVROnce(); // must happen before DXVK creates its Vulkan instance
    auto fn = RealProc<Direct3DCreate9Fn>("Direct3DCreate9");
    IDirect3D9 *d3d = fn ? fn(sdk) : nullptr;
    if (g_active)
        HookInterface(d3d);
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    InitVROnce();
    auto fn = RealProc<Direct3DCreate9ExFn>("Direct3DCreate9Ex");
    if (!fn)
        return D3DERR_NOTAVAILABLE;
    HRESULT hr = fn(sdk, out);
    if (g_active && SUCCEEDED(hr) && out && *out)
        HookInterface(*out);
    return hr;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name)
{
    auto fn = RealProc<int(WINAPI *)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent");
    return fn ? fn(col, name) : 0;
}

int WINAPI D3DPERF_EndEvent()
{
    auto fn = RealProc<int(WINAPI *)()>("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name)
{
    if (auto fn = RealProc<void(WINAPI *)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"))
        fn(col, name);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name)
{
    if (auto fn = RealProc<void(WINAPI *)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"))
        fn(col, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame()
{
    auto fn = RealProc<BOOL(WINAPI *)()>("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD options)
{
    if (auto fn = RealProc<void(WINAPI *)(DWORD)>("D3DPERF_SetOptions"))
        fn(options);
}

DWORD WINAPI D3DPERF_GetStatus()
{
    auto fn = RealProc<DWORD(WINAPI *)()>("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

HRESULT WINAPI Direct3DShaderValidatorCreate9()
{
    auto fn = RealProc<HRESULT(WINAPI *)()>("Direct3DShaderValidatorCreate9");
    return fn ? fn() : E_NOTIMPL;
}

void WINAPI PSGPError() {}
void WINAPI PSGPSampleTexture() {}
void WINAPI DebugSetLevel() {}
void WINAPI DebugSetMute() {}

void WINAPI Direct3D9EnableMaximizedWindowedModeShim(BOOL enable)
{
    if (auto fn = RealProc<void(WINAPI *)(BOOL)>("Direct3D9EnableMaximizedWindowedModeShim"))
        fn(enable);
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    // Process exit (reserved != null): a normal quit, or a crash whose handler exited the process.
    // Not called when the process is killed. The crash filter tells the two apart.
    if (reason == DLL_PROCESS_DETACH && reserved && g_active)
        flags::OnProcessExit();
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        char name[64];
        snprintf(name, sizeof(name), "Local\\INFRAVR-%lu", (unsigned long)GetCurrentProcessId());
        g_instanceMutex = CreateMutexA(nullptr, FALSE, name);
        if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            g_active = false; // another copy runs VR; this one only forwards to DXVK
            return TRUE;
        }
        // Everything the mod writes goes in its own folder, INFRA\infravr\ (made here if it is missing).
        CreateDirectoryA(ModDir().c_str(), nullptr);
        LogInit((ModDir() + "infravr.log").c_str());
        // DXVK's own log (infra_d3d9.log) goes next to ours unless the user points it elsewhere.
        char dxvkLog[8];
        if (!GetEnvironmentVariableA("DXVK_LOG_PATH", dxvkLog, sizeof(dxvkLog)))
            SetEnvironmentVariableA("DXVK_LOG_PATH", ModDir().c_str());
        IVR_LOG("INFRA VR %s loaded from %s", INFRAVR_VERSION, ModuleDir().c_str());
        LoadConfig();
        flags::Init();
        flags::InstallCrashLogger();
        HooksInit();
    }
    return TRUE;
}
