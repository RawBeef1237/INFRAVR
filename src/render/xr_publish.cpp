// INFRA VR - OpenXR backend, D3D side: eye images and panels as shared textures for the helper.
#include "d3d_vr.h"
#include "config.h"
#include "openxr_rt.h"
#include "overlay_api.h"

#include <cstring>

namespace ivr {

namespace {

ivrxr::Shared *Shm()
{
    xr::Link *l = g_VR.XrLink();
    return l ? l->shm : nullptr;
}

// D3DPOOL_DEFAULT render target with a shared handle (DXVK creates it as an exportable Vulkan image).
bool CreateShared(IDirect3DDevice9 *dev, UINT w, UINT h, IDirect3DTexture9 **tex, HANDLE *handle)
{
    *tex = nullptr;
    *handle = nullptr;
    HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, tex, handle);
    if (FAILED(hr) || !*tex || !*handle)
    {
        IVR_ERR("OpenXR: could not create a shared %ux%u texture (0x%08lx) - this DXVK build may not support "
                "shared textures; set vr.backend = openvr",
                w, h, (unsigned long)hr);
        if (*tex)
            (*tex)->Release();
        *tex = nullptr;
        return false;
    }
    return true;
}

bool Copy(IDirect3DDevice9 *dev, IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DTexture9 *dst)
{
    IDirect3DSurface9 *d = nullptr;
    if (!src || FAILED(dst->GetSurfaceLevel(0, &d)) || !d)
        return false;
    HRESULT hr = dev->StretchRect(src, srcRect, d, nullptr, D3DTEXF_LINEAR);
    d->Release();
    return SUCCEEDED(hr);
}

} // namespace

void D3DVR::XrRelease()
{
    for (auto &s : m_xrSlots)
        for (int e = 0; e < 2; ++e)
        {
            if (s.tex[e])
                s.tex[e]->Release();
            s.tex[e] = nullptr;
            s.handle[e] = nullptr;
        }
    for (auto &p : m_xrPanels)
    {
        if (p.tex)
            p.tex->Release();
        p = XrPanel();
    }
    if (m_xrQuery)
        m_xrQuery->Release();
    m_xrQuery = nullptr;
    m_xrPending = -1;
    m_xrW = m_xrH = 0;
    if (ivrxr::Shared *s = Shm())
    {
        s->eyeSeq = 0; // the handles are gone: the helper must not open them
        for (auto &l : s->layers)
            if (l.kind == ivrxr::LAYER_SHARED)
                l.visible = 0;
    }
}

bool D3DVR::XrEnsureSlots(UINT w, UINT h)
{
    if (m_xrFailed)
        return false;
    if (m_xrW == w && m_xrH == h && m_xrSlots[0].tex[0])
        return true;
    XrRelease();
    for (auto &s : m_xrSlots)
        for (int e = 0; e < 2; ++e)
            if (!CreateShared(device, w, h, &s.tex[e], &s.handle[e]))
            {
                m_xrFailed = true;
                XrRelease();
                return false;
            }
    if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &m_xrQuery)))
        m_xrQuery = nullptr;
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
    if (m_xrQuery && m_xrQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
        return;
    int slot = m_xrPending;
    for (int e = 0; e < 2; ++e)
        s->eyeHandle[slot][e] = (uint64_t)(uintptr_t)m_xrSlots[slot].handle[e];
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
    RECT r = {0, 0, (LONG)w, (LONG)h};
    for (int e = 0; e < 2; ++e)
        if (!Copy(device, EyeSurface(e), &r, m_xrSlots[slot].tex[e]))
            return;
    if (m_xrQuery)
        m_xrQuery->Issue(D3DISSUE_END);
    m_xrPending = slot;
    m_xrPendingPose = pose;
    ++m_submits;
}

void D3DVR::XrPublishPanel(vr::VROverlayHandle_t overlay, const VrTexture &src)
{
    if (overlay == vr::k_ulOverlayHandleInvalid || !src.d3d || m_xrFailed)
        return;
    XrPanel *p = nullptr;
    for (auto &c : m_xrPanels)
        if (c.overlay == overlay || (!p && c.overlay == vr::k_ulOverlayHandleInvalid))
            p = &c;
    if (!p)
        return;
    if (!p->tex || p->w != src.width || p->h != src.height)
    {
        if (p->tex)
            p->tex->Release();
        *p = XrPanel();
        if (!CreateShared(device, src.width, src.height, &p->tex, &p->handle))
        {
            m_xrFailed = true;
            return;
        }
        p->overlay = overlay;
        p->w = src.width;
        p->h = src.height;
    }
    IDirect3DSurface9 *s = nullptr;
    if (FAILED(src.d3d->GetSurfaceLevel(0, &s)) || !s)
        return;
    Copy(device, s, nullptr, p->tex);
    s->Release();
    g_VR.Overlays()->SetOverlaySharedTexture(overlay, (uint64_t)(uintptr_t)p->handle, p->w, p->h);
}

void D3DVR::XrShowEyes(bool on)
{
    if (ivrxr::Shared *s = Shm())
        s->eyesVisible = on ? 1 : 0;
}

} // namespace ivr
