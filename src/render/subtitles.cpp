// INFRA VR - subtitles on a panel of their own.
//
// INFRA paints its close captions with the rest of the in-game HUD. That HUD used to reach the
// headset only as one big panel that was also the phone menu's screen, so the subtitles were tiny
// on the phone and, in normal play, did not show (the HUD texture's alpha does not follow what is
// drawn). Here the subtitle region is cut out of the HUD texture right after the HUD is painted
// (CopySubtitles, on the render queue) and, at Present, drawn once into a panel texture whose alpha
// is rebuilt from the picture (BuildSubtitlePanel, subtitle_shader.h): the HUD is painted onto
// black, so a pixel's brightness says how much was drawn there. The panel is a head-locked OpenVR
// overlay (VRMod::subsOverlay), sized by [subtitles] in infravr.ini.
#include "d3d_vr.h"
#include "config.h"
#include "mod.h"
#include "subtitle_shader.h"

#include <algorithm>
#include <cmath>

namespace ivr {

namespace {

bool CreateOwnTarget(IDirect3DDevice9 *device, VrTexture &t, UINT w, UINT h, const char *what)
{
    if (t.Valid() && t.width == w && t.height == h)
        return true;
    t.Reset();
    IDirect3DTexture9 *tex = nullptr;
    HRESULT hr =
        device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
    if (FAILED(hr) || !tex)
    {
        IVR_ERR("CreateTexture(%s %ux%u) failed: 0x%08lx", what, w, h, (unsigned long)hr);
        return false;
    }
    IDirect3DSurface9 *s = nullptr;
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &s)) && s)
    {
        device->ColorFill(s, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        s->Release();
    }
    return t.Attach(tex, true);
}

} // namespace

void D3DVR::ReleaseSubtitles()
{
    subsRaw.Reset();
    subs.Reset();
    if (m_subsPS)
        m_subsPS->Release();
    m_subsPS = nullptr;
}

void D3DVR::CopySubtitles()
{
    if (!device || !hud.d3d || !hud.width || !hud.height)
        return;
    RECT rc = subtitles::Region(g_Config.subtitlesRegion, hud.width, hud.height);
    if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8)
        return;
    UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    if (!CreateOwnTarget(device, subsRaw, w, h, "subtitles"))
        return;
    IDirect3DSurface9 *src = nullptr, *dst = nullptr;
    if (SUCCEEDED(hud.d3d->GetSurfaceLevel(0, &src)) && src && SUCCEEDED(subsRaw.d3d->GetSurfaceLevel(0, &dst)) &&
        dst)
    {
        resolving = true; // our own copy: no MSAA / camera mapping in the StretchRect hook
        HRESULT hr = device->StretchRect(src, &rc, dst, nullptr, D3DTEXF_POINT);
        resolving = false;
        if (m_subsLogs < 1)
        {
            ++m_subsLogs;
            IVR_LOG("Subtitles: region %ld,%ld-%ld,%ld of the %ux%u HUD -> panel %ux%u (%s)", rc.left, rc.top,
                    rc.right, rc.bottom, hud.width, hud.height, w, h, SUCCEEDED(hr) ? "ok" : "copy failed");
        }
    }
    if (dst)
        dst->Release();
    if (src)
        src->Release();
}

bool D3DVR::BuildSubtitlePanel()
{
    if (!device || !subsRaw.Valid() ||
        !CreateOwnTarget(device, subs, subsRaw.width, subsRaw.height, "subtitle panel"))
        return false;
    if (!m_subsPS && !m_subsPSFailed)
    {
        if (FAILED(device->CreatePixelShader(subtitles::kSubtitlePS, &m_subsPS)) || !m_subsPS)
        {
            m_subsPS = nullptr;
            m_subsPSFailed = true;
            IVR_ERR("Subtitles: the device refused the panel shader - the panel is copied as it is");
        }
    }
    IDirect3DSurface9 *dst = nullptr;
    if (FAILED(subs.d3d->GetSurfaceLevel(0, &dst)) || !dst)
        return false;
    if (!m_subsPS)
    {
        IDirect3DSurface9 *src = nullptr;
        bool ok = SUCCEEDED(subsRaw.d3d->GetSurfaceLevel(0, &src)) && src &&
                  SUCCEEDED(device->StretchRect(src, nullptr, dst, nullptr, D3DTEXF_POINT));
        if (src)
            src->Release();
        dst->Release();
        return ok;
    }
    if (!m_stateBlock && FAILED(device->CreateStateBlock(D3DSBT_ALL, &m_stateBlock)))
    {
        m_stateBlock = nullptr;
        dst->Release();
        return false;
    }
    m_stateBlock->Capture();
    IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr;
    device->GetRenderTarget(0, &oldRT);
    device->GetDepthStencilSurface(&oldDS);

    bool ok = subtitles::DrawPanel(device, subsRaw.d3d, dst, subs.width, subs.height, m_subsPS,
                                   g_Config.subtitlesGain, g_Config.subtitlesBackground);

    device->SetRenderTarget(0, oldRT);
    device->SetDepthStencilSurface(oldDS);
    m_stateBlock->Apply();
    if (oldRT)
        oldRT->Release();
    if (oldDS)
        oldDS->Release();
    dst->Release();
    return ok;
}

} // namespace ivr
