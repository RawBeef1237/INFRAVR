#include "d3d_vr.h"
#include "config.h"
#include "mod.h"
#include "overlay_api.h"
#include "world_size.h"
#include "hands_renderer.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

namespace ivr {

D3DVR g_D3D;

// --- VrTexture ---------------------------------------------------------------------------------
bool VrTexture::Attach(IDirect3DTexture9 *tex, bool takeOwnership)
{
    Reset();
    if (!tex)
        return false;
    ID3D9VkInteropTexture *it = nullptr;
    if (FAILED(tex->QueryInterface(IID_ID3D9VkInteropTexture, reinterpret_cast<void **>(&it))) || !it)
    {
        IVR_ERR("Texture %p has no DXVK interop interface (is dxvk_d3d9.dll installed?)", tex);
        return false;
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    VkImage img = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (FAILED(it->GetVulkanImageInfo(&img, &layout, &info)) || !img)
    {
        IVR_ERR("GetVulkanImageInfo failed for %p", tex);
        it->Release();
        return false;
    }
    d3d = tex;
    interop = it;
    image = img;
    this->layout = layout;
    format = info.format;
    width = info.extent.width;
    height = info.extent.height;
    owned = takeOwnership;
    if (!owned)
        tex->AddRef();
    IVR_LOG("VR texture %p: VkImage %llx %ux%u format %d layout %d samples %d usage 0x%x", tex,
            (unsigned long long)(uintptr_t)img, width, height, (int)format, (int)layout, (int)info.samples,
            (unsigned)info.usage);
    return true;
}

void VrTexture::Reset()
{
    if (interop)
        interop->Release();
    if (d3d)
        d3d->Release();
    *this = VrTexture();
}

// --- device -------------------------------------------------------------------------------------
void D3DVR::OnDeviceCreated(IDirect3DDevice9 *dev)
{
    if (device && device != dev)
        XrForgetDevice();
    device = dev;
    interopOk = false;
    if (m_interop)
    {
        m_interop->Release();
        m_interop = nullptr;
    }
    if (FAILED(dev->QueryInterface(IID_ID3D9VkInteropDevice, reinterpret_cast<void **>(&m_interop))) || !m_interop)
    {
        IVR_ERR("The D3D9 device is not DXVK (no ID3D9VkInteropDevice) - nothing can be shown in the headset. "
                "Check that INFRA\\bin\\dxvk_d3d9.dll exists.");
        return;
    }
    m_interop->GetVulkanHandles(&m_instance, &m_physDev, &m_vkDevice);
    uint32_t queueIndex = 0;
    m_interop->GetSubmissionQueue(&m_queue, &queueIndex, &m_queueFamily);
    interopOk = true;
    IVR_LOG("DXVK interop: VkInstance %p, VkDevice %p, queue %p (family %u)", (void *)m_instance,
            (void *)m_vkDevice, (void *)m_queue, m_queueFamily);
    if (HMODULE vk = GetModuleHandleA("vulkan-1.dll"))
    {
        using PropsFn = void(VKAPI_PTR *)(VkPhysicalDevice, VkPhysicalDeviceProperties *);
        auto props = reinterpret_cast<PropsFn>(GetProcAddress(vk, "vkGetPhysicalDeviceProperties"));
        if (props)
        {
            VkPhysicalDeviceProperties p{};
            props(m_physDev, &p);
            IVR_LOG("DXVK renders on: %s", p.deviceName);
        }
    }
    if (g_VR.Ok() && g_VR.UsesSteamVR())
    {
        uint64_t want = 0;
        g_VR.m_system->GetOutputDevice(&want, vr::TextureType_Vulkan, reinterpret_cast<VkInstance_T *>(m_instance));
        if (want && want != (uint64_t)(uintptr_t)m_physDev)
            IVR_ERR("DXVK is rendering on a different GPU than SteamVR - this can crash or show nothing. Put the "
                    "headset GPU's name in INFRA\\infravr\\cfg\\dxvk.conf as dxvk.deviceFilter");
    }
}

void D3DVR::ReleaseHelpers()
{
    if (m_stateBlock)
        m_stateBlock->Release();
    if (m_probeRT)
        m_probeRT->Release();
    if (m_probeSys)
        m_probeSys->Release();
    m_stateBlock = nullptr;
    m_probeRT = m_probeSys = nullptr;
}

void D3DVR::OnBeforeReset()
{
    IVR_LOG("D3D9 device reset");
    ReleaseHelpers();
    ReleaseMsaa();
    XrRelease();
    hands::OnDeviceReset();
    if (cameraTemp)
    {
        cameraTemp->Release();
        cameraTemp = nullptr;
    }
    // Our own textures are D3DPOOL_DEFAULT and must go before a reset. Engine textures are
    // released by the engine itself (it recreates its render targets).
    blank.Reset();
    menu.Reset();
    ReleaseSubtitles();
    for (auto &e : eye)
        e.Reset();
    hud.Reset();
}

void D3DVR::TraceSurface(const char *what, IDirect3DSurface9 *surf, const RECT *r)
{
    D3DSURFACE_DESC d{};
    if (surf)
        surf->GetDesc(&d);
    const char *tag = "";
    IDirect3DTexture9 *tex = nullptr;
    if (surf && SUCCEEDED(surf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&tex))) && tex)
    {
        tag = tex == eye[0].d3d ? " (left eye)" : tex == eye[1].d3d ? " (right eye)" : tex == hud.d3d ? " (hud)" : "";
        tex->Release();
    }
    else if (surf)
        tag = " (not a texture: back buffer or plain surface)";
    if (r)
        IVR_LOG("  trace: %s %p %ux%u ms%d rect %ld,%ld-%ld,%ld%s", what, (void *)surf, d.Width, d.Height,
                (int)d.MultiSampleType, r->left, r->top, r->right, r->bottom, tag);
    else
        IVR_LOG("  trace: %s %p %ux%u ms%d%s", what, (void *)surf, d.Width, d.Height, (int)d.MultiSampleType, tag);
}

void D3DVR::OnTextureCreated(IDirect3DTexture9 *tex, UINT w, UINT h, DWORD usage)
{
    if (capturing < 0 || !(usage & D3DUSAGE_RENDERTARGET))
        return;
    VrTexture &t = capturing == 2 ? hud : eye[capturing];
    if (t.Attach(tex, false))
        IVR_LOG("Captured %s texture %ux%u", capturing == 2 ? "HUD" : (capturing == 0 ? "left eye" : "right eye"), w,
                h);
    capturing = -1;
}

// Runs after every eye render. The engine can replace its render target textures without a device
// reset (it does on a level change: _rt_infravr_left gets a new D3D texture). Our reference keeps
// the old one alive, so without this the headset would keep showing the last frame drawn into it.
void D3DVR::CheckBoundRenderTarget(int which)
{
    static bool logged[3] = {false, false, false};
    if (!device || which < 0 || which > 2)
        return;
    VrTexture &t = which == 2 ? hud : eye[which];
    const char *name = which == 0 ? "left eye" : which == 1 ? "right eye" : "HUD";
    IDirect3DSurface9 *surf = nullptr;
    IDirect3DTexture9 *tex = nullptr;
    D3DSURFACE_DESC d{};
    if ((surf = BoundRenderTarget()) != nullptr)
    {
        surf->GetDesc(&d);
        surf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&tex));
        surf->Release();
    }
    if (!tex)
        return;
    bool sameSize = d.Width == t.width && d.Height == t.height;
    if (which < 2 && tex == eye[1 - which].d3d) // the other eye's texture: not a replacement
        sameSize = false;
    if (!logged[which])
    {
        logged[which] = true;
        IVR_LOG("%s: game texture %p (%ux%u) bound, headset gets %p%s", name, (void *)tex, d.Width, d.Height,
                (void *)t.d3d, tex == t.d3d ? "" : (sameSize ? "  <-- MISMATCH, switching" : "  <-- different target"));
    }
    else if (tex != t.d3d && sameSize)
        IVR_LOG("%s: the game replaced its texture (%p -> %p), following it", name, (void *)t.d3d, (void *)tex);
    if (tex != t.d3d && sameSize)
    {
        t.Attach(tex, false); // submit what the game actually drew into
        if (which < 2)
            m_sameEyeProbes = 0;
    }
    tex->Release();
}

bool D3DVR::BackBufferBound() const
{
    IDirect3DSurface9 *surf = nullptr;
    IDirect3DSwapChain9 *sc = nullptr;
    if (!device || FAILED(device->GetRenderTarget(0, &surf)) || !surf)
        return false;
    bool bb = SUCCEEDED(surf->GetContainer(__uuidof(IDirect3DSwapChain9), reinterpret_cast<void **>(&sc))) && sc;
    if (sc)
        sc->Release();
    surf->Release();
    return bb;
}

bool D3DVR::CaptureBoundRenderTarget(VrTexture &out, UINT width, UINT height)
{
    if (!device)
        return false;
    IDirect3DSurface9 *surf = nullptr;
    if ((surf = BoundRenderTarget()) == nullptr)
        return false;
    IDirect3DTexture9 *tex = nullptr;
    bool ok = false;
    if (SUCCEEDED(surf->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void **>(&tex))) && tex)
    {
        D3DSURFACE_DESC d{};
        surf->GetDesc(&d);
        if (d.Width == width && d.Height == height)
        {
            ok = out.Attach(tex, false);
            if (ok)
                IVR_LOG("Captured texture %ux%u from the bound render target", width, height);
        }
        tex->Release();
    }
    surf->Release();
    return ok;
}

// --- antialiasing ---------------------------------------------------------------------------
IDirect3DSurface9 *D3DVR::EyeSurface(int e)
{
    MsaaEye &m = m_msaa[e];
    if (m.tex != eye[e].d3d)
    {
        m.tex = eye[e].d3d;
        m.surf = nullptr;
        if (m.ms)
        {
            m.ms->Release(); // the game replaced its texture: make a new stand-in to match
            m.ms = nullptr;
        }
        m.dirty = false;
        IDirect3DSurface9 *s = nullptr;
        if (m.tex && SUCCEEDED(m.tex->GetSurfaceLevel(0, &s)) && s)
        {
            m.surf = s;
            s->Release(); // the texture keeps it alive
        }
    }
    return m.surf;
}

IDirect3DSurface9 *D3DVR::MapRenderTarget(IDirect3DSurface9 *s)
{
    if (!s || g_Config.antialiasing < 2 || m_msaaFailed || !device)
        return s;
    for (int e = 0; e < 2; ++e)
    {
        if (!eye[e].Valid() || s != EyeSurface(e))
            continue;
        MsaaEye &m = m_msaa[e];
        if (!m.ms)
        {
            D3DSURFACE_DESC d{};
            s->GetDesc(&d);
            HRESULT hr = device->CreateRenderTarget(d.Width, d.Height, d.Format,
                                                    (D3DMULTISAMPLE_TYPE)g_Config.antialiasing, 0, FALSE, &m.ms,
                                                    nullptr);
            if (FAILED(hr) || !m.ms)
            {
                m.ms = nullptr;
                m_msaaFailed = true;
                IVR_ERR("Antialiasing: could not create a %dx multisampled %ux%u eye image (0x%08lx) - off",
                        g_Config.antialiasing, d.Width, d.Height, (unsigned long)hr);
                return s;
            }
            if (!m_msaaLogged)
            {
                m_msaaLogged = true;
                IVR_LOG("Antialiasing: eyes drawn with %dx MSAA (%ux%u)", g_Config.antialiasing, d.Width, d.Height);
            }
        }
        m.dirty = true;
        return m.ms;
    }
    return s;
}

IDirect3DSurface9 *D3DVR::MapDepth(IDirect3DSurface9 *s)
{
    if (!s || g_Config.antialiasing < 2 || m_msaaFailed || !device || !eye[0].Valid())
        return s;
    for (auto &d : m_msaaDepth)
        if (d.src == s)
            return d.ms;
    D3DSURFACE_DESC desc{};
    if (FAILED(s->GetDesc(&desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE ||
        desc.Width != eye[0].width || desc.Height != eye[0].height)
        return s;
    for (auto &d : m_msaaDepth)
    {
        if (d.src)
            continue;
        HRESULT hr = device->CreateDepthStencilSurface(desc.Width, desc.Height, desc.Format,
                                                       (D3DMULTISAMPLE_TYPE)g_Config.antialiasing, 0, FALSE, &d.ms,
                                                       nullptr);
        if (FAILED(hr) || !d.ms)
        {
            d.ms = nullptr;
            m_msaaFailed = true;
            IVR_ERR("Antialiasing: could not create a multisampled depth buffer (0x%08lx) - off", (unsigned long)hr);
            return s;
        }
        d.src = s;
        return d.ms;
    }
    return s;
}

IDirect3DSurface9 *D3DVR::MapCopySource(IDirect3DSurface9 *s)
{
    if (!s || resolving)
        return s;
    for (auto &m : m_msaa)
        if (m.ms && m.dirty && s == m.surf)
            return m.ms; // the game copies from the eye while drawing it: read the stand-in
    return s;
}

IDirect3DSurface9 *D3DVR::MapCopyDest(IDirect3DSurface9 *s)
{
    if (!s || resolving)
        return s;
    for (auto &m : m_msaa)
        if (m.ms && s == m.surf)
        {
            m.dirty = true;
            return m.ms; // would be overwritten by the resolve otherwise
        }
    return s;
}

void D3DVR::ResolveEyes()
{
    if (!device)
        return;
    resolving = true;
    for (int e = 0; e < 2; ++e)
    {
        MsaaEye &m = m_msaa[e];
        if (!m.ms || !m.dirty || !m.surf || m.tex != eye[e].d3d)
            continue;
        HRESULT hr = device->StretchRect(m.ms, nullptr, m.surf, nullptr, D3DTEXF_NONE);
        static int s_errors = 0;
        if (FAILED(hr) && s_errors++ < 5)
            IVR_ERR("Antialiasing: resolving the %s eye failed (0x%08lx)", e == 0 ? "left" : "right",
                    (unsigned long)hr);
        m.dirty = false;
    }
    resolving = false;
}

void D3DVR::ReleaseMsaa()
{
    for (auto &m : m_msaa)
    {
        if (m.ms)
            m.ms->Release();
        m = MsaaEye();
    }
    for (auto &d : m_msaaDepth)
    {
        if (d.ms)
            d.ms->Release();
        d = MsaaDepth();
    }
}

IDirect3DSurface9 *D3DVR::BoundRenderTarget()
{
    IDirect3DSurface9 *surf = nullptr;
    if (!device || FAILED(device->GetRenderTarget(0, &surf)) || !surf)
        return nullptr;
    for (auto &m : m_msaa)
        if (m.ms && surf == m.ms && m.surf)
        {
            surf->Release();
            m.surf->AddRef();
            return m.surf;
        }
    return surf;
}

bool D3DVR::EnsureOwnTextures()
{
    if (!device || !interopOk)
        return false;
    IDirect3DSurface9 *bb = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return false;
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    bb->Release();
    if (menu.Valid() && (m_bbWidth != d.Width || m_bbHeight != d.Height))
        menu.Reset();
    m_bbWidth = d.Width;
    m_bbHeight = d.Height;

    auto make = [&](VrTexture &t, UINT w, UINT h, const char *what) {
        if (t.Valid())
            return true;
        IDirect3DTexture9 *tex = nullptr;
        HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex,
                                           nullptr);
        if (FAILED(hr) || !tex)
        {
            IVR_ERR("CreateTexture(%s %ux%u) failed: 0x%08lx", what, w, h, (unsigned long)hr);
            return false;
        }
        IDirect3DSurface9 *s = nullptr;
        if (SUCCEEDED(tex->GetSurfaceLevel(0, &s)) && s)
        {
            device->ColorFill(s, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
            s->Release();
        }
        return t.Attach(tex, true);
    };
    bool ok = make(blank, 64, 64, "blank");
    ok &= make(menu, m_bbWidth, m_bbHeight, "menu");
    return ok;
}

void D3DVR::CopyBackBufferToMenu()
{
    IDirect3DSurface9 *bb = nullptr, *dst = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb &&
        SUCCEEDED(menu.d3d->GetSurfaceLevel(0, &dst)) && dst)
        device->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_LINEAR);
    if (dst)
        dst->Release();
    if (bb)
        bb->Release();
}

void D3DVR::MirrorEyeToBackBuffer()
{
    IDirect3DSurface9 *bb = nullptr, *src = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb &&
        SUCCEEDED(eye[0].d3d->GetSurfaceLevel(0, &src)) && src)
    {
        D3DSURFACE_DESC bd{};
        bb->GetDesc(&bd);
        // DXVK can stretch into a multisampled back buffer too, so this also works with MSAA on.
        {
            // Show the part of the left eye the headset shows, letterboxed to the window.
            const vr::VRTextureBounds_t b = g_VR.ViewBounds(0);
            RECT sr = {(LONG)(b.uMin * eye[0].width), (LONG)(b.vMin * eye[0].height), (LONG)(b.uMax * eye[0].width),
                       (LONG)(b.vMax * eye[0].height)};
            float sw = float(sr.right - sr.left), sh = float(sr.bottom - sr.top);
            if (g_Config.mirrorFill && bd.Width && bd.Height)
            {
                // Crop to the window's shape, as near the middle of the view (straight ahead) as fits.
                float aspect = float(bd.Width) / float(bd.Height);
                float cw = std::min(sw, sh * aspect), ch = cw / aspect;
                float cx = std::clamp(0.5f * eye[0].width, sr.left + cw / 2, sr.right - cw / 2);
                float cy = std::clamp(0.5f * eye[0].height, sr.top + ch / 2, sr.bottom - ch / 2);
                sr = {(LONG)(cx - cw / 2), (LONG)(cy - ch / 2), (LONG)(cx + cw / 2), (LONG)(cy + ch / 2)};
                sw = float(sr.right - sr.left);
                sh = float(sr.bottom - sr.top);
            }
            float scale = std::min(bd.Width / sw, bd.Height / sh);
            LONG w = (LONG)(sw * scale), h = (LONG)(sh * scale);
            RECT dr = {(LONG)(bd.Width - w) / 2, (LONG)(bd.Height - h) / 2, 0, 0};
            dr.right = dr.left + w;
            dr.bottom = dr.top + h;
            device->ColorFill(bb, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
            device->StretchRect(src, &sr, bb, &dr, D3DTEXF_LINEAR);
        }
    }
    if (src)
        src->Release();
    if (bb)
        bb->Release();
}

// The menu texture is a copy of the back buffer, whose alpha channel holds whatever the
// engine left there (Source writes depth/fog into destination alpha). SteamVR overlays
// are alpha blended, so that made the menu screen patchy and see-through. Force alpha to
// 255 by drawing one alpha-only quad; the device state is saved and restored around it.
void D3DVR::MakeOpaque(const VrTexture &t)
{
    if (!device || !t.d3d)
        return;
    IDirect3DSurface9 *dst = nullptr;
    if (FAILED(t.d3d->GetSurfaceLevel(0, &dst)) || !dst)
        return;
    if (!m_stateBlock && FAILED(device->CreateStateBlock(D3DSBT_ALL, &m_stateBlock)))
    {
        m_stateBlock = nullptr;
        dst->Release();
        return;
    }
    m_stateBlock->Capture();
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    device->GetRenderTarget(0, &oldRT);
    device->GetDepthStencilSurface(&oldDS);

    device->SetRenderTarget(0, dst);
    device->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, t.width, t.height, 0.0f, 1.0f};
    device->SetViewport(&vp);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTexture(0, nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    const DWORD rs[][2] = {
        {D3DRS_ZENABLE, FALSE},          {D3DRS_ZWRITEENABLE, FALSE},          {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE},  {D3DRS_SCISSORTESTENABLE, FALSE},     {D3DRS_FOGENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE},         {D3DRS_CLIPPLANEENABLE, 0},           {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA},
    };
    for (const auto &r : rs)
        device->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    struct Vtx
    {
        float x, y, z, rhw;
        DWORD color;
    };
    float w = (float)t.width - 0.5f, h = (float)t.height - 0.5f;
    const Vtx quad[4] = {{-0.5f, -0.5f, 0, 1, 0xFF000000},
                         {w, -0.5f, 0, 1, 0xFF000000},
                         {-0.5f, h, 0, 1, 0xFF000000},
                         {w, h, 0, 1, 0xFF000000}};
    if (SUCCEEDED(device->BeginScene()))
    {
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));
        device->EndScene();
    }

    device->SetRenderTarget(0, oldRT);
    device->SetDepthStencilSurface(oldDS);
    m_stateBlock->Apply();
    if (oldRT)
        oldRT->Release();
    if (oldDS)
        oldDS->Release();
    dst->Release();
}

// Reads a 32x32 down-scaled copy of a texture back to the CPU (diagnostics only).
D3DVR::ProbeStats D3DVR::Probe(const VrTexture &t, const vr::VRTextureBounds_t *b)
{
    ProbeStats st;
    if (!device || !t.d3d)
        return st;
    const UINT N = 32;
    if (!m_probeRT &&
        FAILED(device->CreateRenderTarget(N, N, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &m_probeRT, nullptr)))
        m_probeRT = nullptr;
    if (!m_probeSys &&
        FAILED(device->CreateOffscreenPlainSurface(N, N, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_probeSys, nullptr)))
        m_probeSys = nullptr;
    if (!m_probeRT || !m_probeSys)
        return st;
    IDirect3DSurface9 *src = nullptr;
    if (FAILED(t.d3d->GetSurfaceLevel(0, &src)) || !src)
        return st;
    RECT sr = {0, 0, (LONG)t.width, (LONG)t.height};
    if (b)
        sr = {(LONG)(b->uMin * t.width), (LONG)(b->vMin * t.height), (LONG)(b->uMax * t.width),
              (LONG)(b->vMax * t.height)};
    HRESULT hr = device->StretchRect(src, &sr, m_probeRT, nullptr, D3DTEXF_LINEAR);
    src->Release();
    if (FAILED(hr) || FAILED(device->GetRenderTargetData(m_probeRT, m_probeSys)))
        return st;
    D3DLOCKED_RECT lr{};
    if (FAILED(m_probeSys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        return st;
    double sum = 0;
    int opaque = 0;
    uint32_t hash = 2166136261u;
    static float lum[32][32];
    for (UINT y = 0; y < N; ++y)
    {
        const uint8_t *row = static_cast<const uint8_t *>(lr.pBits) + y * lr.Pitch;
        for (UINT x = 0; x < N; ++x)
        {
            const uint8_t *px = row + x * 4; // B G R A
            for (int c = 0; c < 3; ++c)
                hash = (hash ^ px[c]) * 16777619u;
            float l = 0.114f * px[0] + 0.587f * px[1] + 0.299f * px[2];
            lum[y][x] = l;
            sum += l;
            st.max = std::max(st.max, l);
            if (px[3] > 200)
                ++opaque;
        }
    }
    m_probeSys->UnlockRect();
    for (UINT i = 1; i < N; ++i)
    {
        float dr = 0, dc = 0, er = 0, ec = 0;
        for (UINT j = 0; j < N; ++j)
        {
            dr += std::fabs(lum[i][j] - lum[i - 1][j]);
            dc += std::fabs(lum[j][i] - lum[j][i - 1]);
            er += lum[i][j];
            ec += lum[j][i];
        }
        // identical to the neighbour but not just empty black
        if (dr < 0.5f * N && er > 4.0f * N)
            ++st.rowsSame;
        if (dc < 0.5f * N && ec > 4.0f * N)
            ++st.colsSame;
    }
    st.avg = float(sum / (N * N));
    st.opaque = float(opaque) / float(N * N);
    st.hash = hash;
    st.ok = true;
    return st;
}

// Full-resolution check: where does the real picture end and the smeared copies of its last
// row/column begin? Only looks at the part of the texture the view is drawn into.
bool D3DVR::MeasureSmear(const VrTexture &t, const char *what, UINT &picW, UINT &picH)
{
    bool ok = false;
    IDirect3DSurface9 *src = nullptr, *sys = nullptr;
    if (!t.d3d || FAILED(t.d3d->GetSurfaceLevel(0, &src)) || !src)
        return false;
    D3DSURFACE_DESC d{};
    src->GetDesc(&d);
    UINT areaW = std::min<UINT>(d.Width, g_VR.ViewW()), areaH = std::min<UINT>(d.Height, g_VR.ViewH());
    if (SUCCEEDED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(src, sys)))
    {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        {
            auto px = [&](UINT x, UINT y) {
                return *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(lr.pBits) + y * lr.Pitch +
                                                           x * 4) & 0x00FFFFFF;
            };
            // Identical neighbours that are pure black are a dark scene, not smearing.
            auto rowSame = [&](UINT y) { // row y identical to row y+1
                bool lit = false;
                for (UINT x = 0; x < areaW; x += 3)
                {
                    if (px(x, y) != px(x, y + 1))
                        return false;
                    lit |= px(x, y) != 0;
                }
                return lit;
            };
            auto colSame = [&](UINT x, UINT yEnd) {
                bool lit = false;
                for (UINT y = 0; y < yEnd; y += 3)
                {
                    if (px(x, y) != px(x + 1, y))
                        return false;
                    lit |= px(x, y) != 0;
                }
                return lit;
            };
            UINT h = areaH;
            while (h > 1 && rowSame(h - 2))
                --h;
            UINT w = areaW;
            while (w > 1 && colSame(w - 2, h))
                --w;
            sys->UnlockRect();
            picW = w;
            picH = h;
            ok = true;
            IVR_LOG("smear check %s: picture is %ux%u of the %ux%u area drawn%s", what, w, h, areaW, areaH,
                    (w + 8 < areaW || h + 8 < areaH) ? "  <-- the rest is stretched lines" : " (no stretching)");
        }
    }
    if (sys)
        sys->Release();
    src->Release();
    return ok;
}

void D3DVR::DumpBound(bool renderTarget, const char *name)
{
    if (!device)
        return;
    IDirect3DSurface9 *src = nullptr;
    if (renderTarget)
        device->GetRenderTarget(0, &src);
    else
    {
        IDirect3DBaseTexture9 *bt = nullptr;
        if (SUCCEEDED(device->GetTexture(0, &bt)) && bt)
        {
            if (bt->GetType() == D3DRTYPE_TEXTURE)
                static_cast<IDirect3DTexture9 *>(bt)->GetSurfaceLevel(0, &src);
            bt->Release();
        }
    }
    if (!src)
    {
        IVR_LOG("dump %s: nothing bound", name);
        return;
    }
    DumpSurface(src, name);
    src->Release();
}

void D3DVR::DumpSurface(IDirect3DSurface9 *src, const char *name)
{
    D3DSURFACE_DESC d{};
    src->GetDesc(&d);
    IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
    HRESULT hr = device->CreateRenderTarget(d.Width, d.Height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt,
                                            nullptr);
    if (SUCCEEDED(hr))
        hr = device->StretchRect(src, nullptr, rt, nullptr, D3DTEXF_NONE);
    if (SUCCEEDED(hr))
        hr = device->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr))
        hr = device->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr))
        hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr))
    {
        // Big pictures are halved; small ones (the glow) kept at full size.
        const UINT step = d.Width > 1200 ? 2 : 1;
        const UINT w = d.Width / step, h = d.Height / step;
        const UINT pitch = (w * 3 + 3) & ~3u;
        std::vector<uint8_t> img(pitch * h);
        float maxA = 0;
        for (UINT y = 0; y < h; ++y)
        {
            // BMP rows run bottom to top
            uint8_t *out = img.data() + (h - 1 - y) * pitch;
            const uint8_t *row = static_cast<const uint8_t *>(lr.pBits) + (y * step) * lr.Pitch;
            for (UINT x = 0; x < w; ++x)
            {
                const uint8_t *px = row + (x * step) * 4;
                out[x * 3 + 0] = px[0];
                out[x * 3 + 1] = px[1];
                out[x * 3 + 2] = px[2];
                maxA = std::max(maxA, (float)px[3]);
            }
        }
        sys->UnlockRect();
        std::string path = ModDir() + name + ".bmp";
        if (FILE *f = fopen(path.c_str(), "wb"))
        {
            uint8_t hdr[54] = {'B', 'M'};
            auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
            put32(2, 54 + (uint32_t)img.size());
            put32(10, 54);
            put32(14, 40);
            put32(18, w);
            put32(22, h);
            hdr[26] = 1;
            hdr[28] = 24;
            put32(34, (uint32_t)img.size());
            fwrite(hdr, 1, 54, f);
            fwrite(img.data(), 1, img.size(), f);
            fclose(f);
        }
        IVR_LOG("dump %s: %p %ux%u ms%d format %d saved as %s (%ux%u)", name, (void *)src, d.Width, d.Height,
                (int)d.MultiSampleType, (int)d.Format, path.c_str(), w, h);
    }
    else
        IVR_LOG("dump %s: %p %ux%u ms%d format %d could not be read (0x%08lx)", name, (void *)src, d.Width, d.Height,
                (int)d.MultiSampleType, (int)d.Format, (unsigned long)hr);
    if (sys)
        sys->Release();
    if (rt)
        rt->Release();
}

// Where on the HUD panel something is drawn (subtitles, hints): the 32x32 probe misses small text.
// The first time something shows, the panel is also saved as infravr\hud_panel.bmp.
void D3DVR::LogHudContent()
{
    if (!device || !hud.d3d)
        return;
    IDirect3DSurface9 *src = nullptr;
    if (FAILED(hud.d3d->GetSurfaceLevel(0, &src)) || !src)
        return;
    D3DSURFACE_DESC d{};
    src->GetDesc(&d);
    IDirect3DSurface9 *sys = nullptr;
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(device->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(src, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
    {
        if (d.Format == D3DFMT_A8R8G8B8)
        {
            int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1, count = 0;
            for (UINT y = 0; y < d.Height; ++y)
            {
                const uint8_t *row = static_cast<const uint8_t *>(lr.pBits) + y * lr.Pitch;
                for (UINT x = 0; x < d.Width; ++x)
                    if (row[x * 4 + 3] > 16)
                    {
                        ++count;
                        x0 = std::min(x0, (int)x);
                        x1 = std::max(x1, (int)x);
                        y0 = std::min(y0, (int)y);
                        y1 = std::max(y1, (int)y);
                    }
            }
            if (count)
                IVR_LOG("probe: HUD panel has %d drawn pixels in %d,%d-%d,%d of %ux%u", count, x0, y0, x1, y1, d.Width,
                        d.Height);
            else
                IVR_LOG("probe: HUD panel is empty (%ux%u)", d.Width, d.Height);
            static bool s_saved = false;
            if (count && !s_saved)
            {
                s_saved = true;
                const UINT w = d.Width / 2, h = d.Height / 2, pitch = (w * 3 + 3) & ~3u;
                std::vector<uint8_t> img(pitch * h);
                for (UINT y = 0; y < h; ++y)
                {
                    uint8_t *out = img.data() + (h - 1 - y) * pitch;
                    const uint8_t *row = static_cast<const uint8_t *>(lr.pBits) + (y * 2) * lr.Pitch;
                    for (UINT x = 0; x < w; ++x)
                    {
                        const uint8_t *px = row + x * 8;
                        // over grey, so the panel's see-through parts show
                        for (int c = 0; c < 3; ++c)
                            out[x * 3 + c] = (uint8_t)((px[c] * px[3] + 64 * (255 - px[3])) / 255);
                    }
                }
                std::string path = ModDir() + "hud_panel.bmp";
                if (FILE *f = fopen(path.c_str(), "wb"))
                {
                    uint8_t hdr[54] = {'B', 'M'};
                    auto put32 = [&](int at, uint32_t v) { std::memcpy(hdr + at, &v, 4); };
                    put32(2, 54 + (uint32_t)img.size());
                    put32(10, 54);
                    put32(14, 40);
                    put32(18, w);
                    put32(22, h);
                    hdr[26] = 1;
                    hdr[28] = 24;
                    put32(34, (uint32_t)img.size());
                    fwrite(hdr, 1, 54, f);
                    fwrite(img.data(), 1, img.size(), f);
                    fclose(f);
                    IVR_LOG("probe: HUD panel saved as %s", path.c_str());
                }
            }
        }
        else
            IVR_LOG("probe: HUD panel format %d not checked", (int)d.Format);
        sys->UnlockRect();
    }
    if (sys)
        sys->Release();
    src->Release();
}

void D3DVR::ProbeFrame(bool stereo, bool hudOn)
{
    double now = NowSeconds();
    if (!g_Config.probe || now < m_nextProbe)
        return;
    m_nextProbe = now + (m_probes < 6 ? 5.0 : 30.0);
    ++m_probes;
    if (stereo)
    {
        vr::VRTextureBounds_t bl = g_VR.ViewBounds(0), br = g_VR.ViewBounds(1);
        ProbeStats l = Probe(eye[0], &bl), r = Probe(eye[1], &br);
        UINT pw = 0, ph = 0;
        if (m_smearChecks < 4 && l.max > 2.0f && MeasureSmear(eye[0], "left eye", pw, ph))
            ++m_smearChecks;
        IVR_LOG("probe: left eye brightness avg %.0f max %.0f, right eye avg %.0f max %.0f; repeated rows/cols "
                "L %d/%d R %d/%d (of 31)",
                l.avg, l.max, r.avg, r.max, l.rowsSame, l.colsSame, r.rowsSame, r.colsSame);
        if (l.ok && l.max > 2.0f)
        {
            // A picture that is bit-for-bit the same 5 s later means the game stopped drawing new frames.
            if (l.hash == m_lastEyeHash)
                IVR_LOG("probe: left eye picture is identical to the last probe (%d in a row) - the game's view "
                        "is not changing (normal only if the headset is lying still)", ++m_sameEyeProbes);
            else
                m_sameEyeProbes = 0;
            m_lastEyeHash = l.hash;
        }
        LogCompositorStats();
        if (l.ok && r.ok && l.max < 2.0f && r.max < 2.0f)
        {
            if (++m_blackEyeProbes == 2)
                IVR_ERR("The eye images are completely black: the game is not drawing into them");
        }
        else
            m_blackEyeProbes = 0;
        if (m_subsShown)
        {
            // raw = as the game left it, panel = what the headset gets (alpha rebuilt)
            ProbeStats r = Probe(subsRaw, nullptr), p = Probe(subs, nullptr);
            IVR_LOG("probe: subtitle panel brightness max %.0f; game alpha %.0f%% opaque, panel %.0f%% opaque", p.max,
                    r.opaque * 100.0f, p.opaque * 100.0f);
        }
        if (hudOn)
        {
            ProbeStats h = Probe(hud, nullptr);
            IVR_LOG("probe: HUD panel %.0f%% opaque", h.opaque * 100.0f);
            LogHudContent();
            if (h.ok && h.opaque > 0.6f && !hudBroken)
            {
                hudBroken = true;
                g_Config.hudMode = HudMode::Eyes; // draw subtitles/hints into the view instead
                IVR_ERR("The HUD panel is opaque and would cover your view - switched to hud_mode = eyes");
            }
        }
    }
    else
    {
        ProbeStats m = Probe(menu, nullptr);
        IVR_LOG("probe: menu screen brightness avg %.0f max %.0f, back buffer alpha %.0f%% opaque", m.avg, m.max,
                m.opaque * 100.0f);
    }
}

// What SteamVR did with our frames since the last probe: if it shows far fewer new frames than we
// submitted (or keeps timing out), the headset is not getting the game's pictures.
void D3DVR::LogCompositorStats()
{
    if (g_VR.IsOpenXR())
    {
        double now = NowSeconds();
        if (m_xrStatsAt > 0)
            IVR_LOG("OpenXR: %.1f eye pairs/s sent to the helper; skipped %u (copy still running), %u (no free slot)",
                    (m_submits - m_xrLastSubmits) / (now - m_xrStatsAt), m_xrBusy, m_xrNoSlot);
        m_xrStatsAt = now;
        m_xrLastSubmits = m_submits;
        m_xrBusy = m_xrNoSlot = 0;
        return;
    }
    if (!g_VR.UsesSteamVR())
        return;
    vr::Compositor_CumulativeStats st{};
    g_VR.m_compositor->GetCumulativeStats(&st, sizeof(st));
    if (m_haveStats)
        IVR_LOG("compositor: %u frames submitted by INFRA VR, SteamVR showed %u (%u repeated without "
                "reprojection, %u reprojected), app timeouts %u",
                m_submits - m_lastSubmits, st.m_nNumFramePresents - m_lastPresents,
                st.m_nNumDroppedFrames - m_lastDropped, st.m_nNumReprojectedFrames - m_lastReprojected,
                st.m_nNumTimedOut - m_lastTimedOut);
    m_haveStats = true;
    m_lastSubmits = m_submits;
    m_lastPresents = st.m_nNumFramePresents;
    m_lastDropped = st.m_nNumDroppedFrames;
    m_lastReprojected = st.m_nNumReprojectedFrames;
    m_lastTimedOut = st.m_nNumTimedOut;
}

vr::VRVulkanTextureData_t D3DVR::VulkanData(const VrTexture &t) const
{
    vr::VRVulkanTextureData_t d{};
    d.m_nImage = (uint64_t)(uintptr_t)t.image;
    d.m_pDevice = reinterpret_cast<VkDevice_T *>(m_vkDevice);
    d.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T *>(m_physDev);
    d.m_pInstance = reinterpret_cast<VkInstance_T *>(m_instance);
    d.m_pQueue = reinterpret_cast<VkQueue_T *>(m_queue);
    d.m_nQueueFamilyIndex = m_queueFamily;
    d.m_nWidth = t.width;
    d.m_nHeight = t.height;
    d.m_nFormat = (uint32_t)t.format;
    d.m_nSampleCount = 1;
    return d;
}

static const VkImageSubresourceRange kColorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

// DXVK (2.5 and later) defragments video memory: it may move a texture into new memory, which gives
// it a new VkImage and destroys the old one once the GPU is done with it. The D3D9 interop does not
// pin the images it hands out, so a VkImage remembered at Attach can be a dead one minutes later,
// typically right after a level load frees a lot of memory. SteamVR given a dead image crashes
// inside NVIDIA's Vulkan driver (crash dump: IVROverlay::SetOverlayTexture for the subtitle panel,
// null read in nvoglv32.dll). Defragmentation is turned off at startup (DXVK_CONFIG, d3d9_proxy.cpp);
// this asks DXVK for each image's current handle before every hand-over in case it moved anyway.
void D3DVR::RefreshImages(VrTexture *const *textures, int count)
{
    static int s_moves = 0;
    for (int i = 0; i < count; ++i)
    {
        VrTexture &t = *textures[i];
        if (!t.interop)
            continue;
        VkImage img = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (FAILED(t.interop->GetVulkanImageInfo(&img, &layout, nullptr)) || !img)
            continue;
        if (img != t.image)
        {
            if (s_moves < 20)
            {
                ++s_moves;
                IVR_LOG("VR texture %p: DXVK moved it, VkImage %llx -> %llx", t.d3d,
                        (unsigned long long)(uintptr_t)t.image, (unsigned long long)(uintptr_t)img);
            }
            t.image = img;
            t.layout = layout;
        }
    }
}

void D3DVR::BeginVulkanAccess(VrTexture *const *textures, int count)
{
    for (int i = 0; i < count; ++i)
        m_interop->TransitionTextureLayout(textures[i]->interop, &kColorRange, textures[i]->layout,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    m_interop->FlushRenderingCommands(); // also waits for DXVK's worker, so no move is half done
    RefreshImages(textures, count);
    m_interop->LockSubmissionQueue();
}

void D3DVR::EndVulkanAccess(VrTexture *const *textures, int count)
{
    m_interop->ReleaseSubmissionQueue();
    for (int i = 0; i < count; ++i)
        m_interop->TransitionTextureLayout(textures[i]->interop, &kColorRange, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                           textures[i]->layout);
}

void D3DVR::OnPresent()
{
    if (!g_VR.Ok() || !g_Mod.Active() || !device || !interopOk || !g_VR.HavePoses())
        return;
    // Every startup crash so far (NVIDIA's Vulkan driver, inside SteamVR's first Submit) came in the
    // first seconds while INFRA was still loading its main menu map: send nothing to SteamVR until
    // that map is in (or 20 s have passed), SteamVR keeps showing its own loading screen meanwhile.
    double now = NowSeconds();
    if (m_firstPresent < 0)
        m_firstPresent = now;
    if (!m_submitReady)
    {
        bool levelIn = g_Mod.firstLevelAt >= 0 && now - g_Mod.firstLevelAt > 1.0;
        if (!levelIn && now - m_firstPresent < 20.0)
            return;
        m_submitReady = true;
        IVR_LOG("Sending frames to the headset (%s, %.1f s after the first frame)",
                levelIn ? "main menu loaded" : "no map loaded yet", now - m_firstPresent);
    }
    if (!EnsureOwnTextures())
        return;
    ResolveEyes();

    bool stereo = g_Mod.eyesRendered && !g_Mod.menuMode && eye[0].Valid() && eye[1].Valid();
    bool hudOn = stereo && g_Mod.hudPainted && hud.Valid() && (!hudBroken || g_Mod.phoneMenu);
    bool subsOn = stereo && g_Mod.subsPainted && subsRaw.Valid() && !g_VR.IsOpenXR();
    // Phone (pause) menu but no new 3D view this frame: show the last one again, tagged with the
    // head pose it was drawn for, so SteamVR keeps it fixed in the world while you look around.
    bool reuse = !stereo && g_Mod.phoneMenu && !g_Mod.menuMode && m_haveLastEyes && eye[0].Valid() &&
                 eye[1].Valid();
    if (g_Mod.phoneMenu)
    {
        ++m_phoneFrames;
        m_phoneReused += reuse;
    }
    else if (m_phoneFrames)
    {
        IVR_LOG("Phone menu: %d frames, %d of them without a new 3D view from the game", m_phoneFrames,
                m_phoneReused);
        m_phoneFrames = m_phoneReused = 0;
    }
    if (stereo)
    {
        m_lastEyePose = m_haveRenderPose ? m_renderPose : g_VR.Head().raw;
        m_haveLastEyes = true;
    }
    if (g_Mod.menuMode)
        m_haveLastEyes = false;
    g_Mod.eyesRendered = false;
    g_Mod.hudPainted = false;
    g_Mod.subsPainted = false;
    if (g_VR.IsOpenXR())
        XrHandOver();
    if (reuse && g_VR.IsOpenXR())
    {
        // The helper keeps showing the last eye pair with the pose it was drawn for.
        XrShowEyes(true);
        g_Mod.UpdateOverlays(false, hud.Valid(), (int)m_bbWidth, (int)m_bbHeight);
        return;
    }
    if (reuse)
    {
        VrTexture *eyes[2] = {&eye[0], &eye[1]};
        BeginVulkanAccess(eyes, 2);
        vr::EVRCompositorError err[2];
        for (int e = 0; e < 2; ++e)
        {
            vr::VRVulkanTextureData_t d = VulkanData(eye[e]);
            vr::VRTextureWithPose_t t{};
            t.handle = &d;
            t.eType = vr::TextureType_Vulkan;
            t.eColorSpace = vr::ColorSpace_Auto;
            t.mDeviceToAbsoluteTracking = m_lastEyePose;
            vr::VRTextureBounds_t b = g_VR.ViewBounds(e);
            err[e] = g_VR.m_compositor->Submit(e == 0 ? vr::Eye_Left : vr::Eye_Right, &t, &b,
                                               vr::Submit_TextureWithPose);
        }
        EndVulkanAccess(eyes, 2);
        static int s_reuseErrors = 0;
        if ((err[0] != vr::VRCompositorError_None || err[1] != vr::VRCompositorError_None) && s_reuseErrors < 5)
        {
            ++s_reuseErrors;
            IVR_ERR("Submit (last frame again) failed: left %d, right %d", (int)err[0], (int)err[1]);
        }
        g_Mod.UpdateOverlays(false, hud.Valid(), (int)m_bbWidth, (int)m_bbHeight);
        return;
    }

    if (stereo)
    {
        if (g_Config.mirrorToDesktop)
            MirrorEyeToBackBuffer();
    }
    else
        CopyBackBufferToMenu();
    if (subsOn)
        subsOn = BuildSubtitlePanel();
    m_subsShown = subsOn;
    if (!stereo)
        g_Mod.ShowSubtitles(false);
    ProbeFrame(stereo, hudOn && !g_Mod.phoneMenu); // the pause menu may be opaque on purpose
    hudOn = hudOn && (!hudBroken || g_Mod.phoneMenu);
    if (!stereo)
        MakeOpaque(menu);

    if (g_VR.IsOpenXR())
    {
        XrShowEyes(stereo);
        if (stereo)
        {
            XrPublishEyes(m_lastEyePose);
            if (hudOn)
                XrPublishPanel(g_Mod.hudOverlay, hud);
        }
        else
            XrPublishPanel(g_Mod.menuOverlay, menu);
        g_Mod.UpdateOverlays(!stereo, hudOn, (int)m_bbWidth, (int)m_bbHeight);
        return;
    }

    VrTexture *list[4];
    int n = 0;
    if (stereo)
    {
        list[n++] = &eye[0];
        list[n++] = &eye[1];
        if (hudOn)
            list[n++] = &hud;
        if (subsOn)
            list[n++] = &subs;
    }
    else
    {
        list[n++] = &blank;
        list[n++] = &menu;
    }

    static int s_errors = 0;
    BeginVulkanAccess(list, n);
    vr::EVRCompositorError e0, e1;
    if (stereo)
    {
        vr::VRVulkanTextureData_t l = VulkanData(eye[0]), r = VulkanData(eye[1]);
        vr::VRTextureWithPose_t tl{}, tr{};
        tl.handle = &l;
        tr.handle = &r;
        tl.eType = tr.eType = vr::TextureType_Vulkan;
        tl.eColorSpace = tr.eColorSpace = vr::ColorSpace_Auto;
        tl.mDeviceToAbsoluteTracking = tr.mDeviceToAbsoluteTracking = m_lastEyePose;
        vr::EVRSubmitFlags flags = m_haveRenderPose ? vr::Submit_TextureWithPose : vr::Submit_Default;
        vr::VRTextureBounds_t bl = g_VR.ViewBounds(0), br = g_VR.ViewBounds(1);
        e0 = g_VR.m_compositor->Submit(vr::Eye_Left, &tl, &bl, flags);
        e1 = g_VR.m_compositor->Submit(vr::Eye_Right, &tr, &br, flags);
        ++m_submits;
        if (hudOn)
        {
            vr::VRVulkanTextureData_t h = VulkanData(hud);
            vr::Texture_t th = {&h, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
            g_VR.Overlays()->SetOverlayTexture(g_Mod.hudOverlay, th);
        }
        if (subsOn)
        {
            vr::VRVulkanTextureData_t sd = VulkanData(subs);
            vr::Texture_t ts = {&sd, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
            g_VR.Overlays()->SetOverlayTexture(g_Mod.subsOverlay, ts);
        }
        g_Mod.ShowSubtitles(subsOn);
    }
    else
    {
        vr::VRVulkanTextureData_t b = VulkanData(blank), m = VulkanData(menu);
        vr::Texture_t tb = {&b, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        vr::Texture_t tm = {&m, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        e0 = g_VR.m_compositor->Submit(vr::Eye_Left, &tb, nullptr, vr::Submit_Default);
        e1 = g_VR.m_compositor->Submit(vr::Eye_Right, &tb, nullptr, vr::Submit_Default);
        g_VR.Overlays()->SetOverlayTexture(g_Mod.menuOverlay, tm);
    }
    EndVulkanAccess(list, n);

    if ((e0 != vr::VRCompositorError_None || e1 != vr::VRCompositorError_None) && s_errors < 20)
    {
        ++s_errors;
        IVR_ERR("IVRCompositor::Submit failed: left %d, right %d (stereo %d)", (int)e0, (int)e1, (int)stereo);
    }
    g_Mod.UpdateOverlays(!stereo, hudOn, (int)m_bbWidth, (int)m_bbHeight);
}

} // namespace ivr
