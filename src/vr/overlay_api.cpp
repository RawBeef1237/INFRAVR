#include "overlay_api.h"
#include "openxr_rt.h"

#include <cstring>

namespace ivr {

namespace {

// --- OpenVR: straight to IVROverlay ----------------------------------------------------------------
class OpenVROverlays final : public OverlayApi
{
public:
    explicit OpenVROverlays(vr::IVROverlay *ov) : m_ov(ov) {}

    vr::EVROverlayError CreateOverlay(const char *key, const char *name, vr::VROverlayHandle_t *out) override
    {
        if (!m_ov)
            return vr::VROverlayError_RequestFailed;
        return m_ov->CreateOverlay(key, name, out);
    }
    void SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float m) override
    {
        if (Valid(h))
            m_ov->SetOverlayWidthInMeters(h, m);
    }
    void SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t o) override
    {
        if (Valid(h))
            m_ov->SetOverlaySortOrder(h, o);
    }
    void SetOverlayRaw(vr::VROverlayHandle_t h, const void *px, uint32_t w, uint32_t hg, uint32_t bpp) override
    {
        if (Valid(h))
            m_ov->SetOverlayRaw(h, const_cast<void *>(px), w, hg, bpp);
    }
    void SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, const vr::HmdMatrix34_t &xf) override
    {
        if (Valid(h))
            m_ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &xf);
    }
    void SetOverlayTransformHeadRelative(vr::VROverlayHandle_t h, const vr::HmdMatrix34_t &xf) override
    {
        if (Valid(h))
            m_ov->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &xf);
    }
    void ShowOverlay(vr::VROverlayHandle_t h) override
    {
        if (Valid(h))
            m_ov->ShowOverlay(h);
    }
    void HideOverlay(vr::VROverlayHandle_t h) override
    {
        if (Valid(h))
            m_ov->HideOverlay(h);
    }
    void SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t &t) override
    {
        if (Valid(h))
            m_ov->SetOverlayTexture(h, &t);
    }
    void SetOverlaySharedTexture(vr::VROverlayHandle_t, uint64_t, uint32_t, uint32_t) override {}

private:
    bool Valid(vr::VROverlayHandle_t h) const { return m_ov && h != vr::k_ulOverlayHandleInvalid; }
    vr::IVROverlay *m_ov;
};

// --- OpenXR: quad layers in the shared memory ------------------------------------------------------
// Handles are layer index + 1.
class OpenXROverlays final : public OverlayApi
{
public:
    explicit OpenXROverlays(xr::Link *link) : m_link(link) {}

    vr::EVROverlayError CreateOverlay(const char *key, const char *, vr::VROverlayHandle_t *out) override
    {
        ivrxr::Shared *s = m_link ? m_link->shm : nullptr;
        if (!s || m_used >= ivrxr::kLayers)
            return vr::VROverlayError_RequestFailed;
        int i = m_used++;
        std::memset(&s->layers[i], 0, sizeof(s->layers[i]));
        s->layers[i].widthMeters = 1.0f;
        s->layers[i].xf[0][0] = s->layers[i].xf[1][1] = s->layers[i].xf[2][2] = 1.0f;
        *out = (vr::VROverlayHandle_t)(i + 1);
        IVR_LOG("OpenXR overlay \"%s\" is layer %d", key, i);
        return vr::VROverlayError_None;
    }
    void SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float m) override
    {
        if (ivrxr::Layer *l = L(h))
            l->widthMeters = m;
    }
    void SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t o) override
    {
        if (ivrxr::Layer *l = L(h))
            l->sortOrder = o;
    }
    void SetOverlayRaw(vr::VROverlayHandle_t h, const void *px, uint32_t w, uint32_t hg, uint32_t bpp) override
    {
        ivrxr::Layer *l = L(h);
        if (!l || bpp != 4 || (uint64_t)w * hg * 4 > ivrxr::kRawBytes)
            return;
        std::memcpy(m_link->shm->raw[h - 1], px, (size_t)w * hg * 4);
        l->kind = ivrxr::LAYER_RAW;
        l->width = w;
        l->height = hg;
        ++l->serial;
    }
    void SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, const vr::HmdMatrix34_t &xf) override
    {
        if (ivrxr::Layer *l = L(h))
        {
            std::memcpy(l->xf, xf.m, sizeof(l->xf));
            l->headLocked = 0;
        }
    }
    void SetOverlayTransformHeadRelative(vr::VROverlayHandle_t h, const vr::HmdMatrix34_t &xf) override
    {
        if (ivrxr::Layer *l = L(h))
        {
            std::memcpy(l->xf, xf.m, sizeof(l->xf));
            l->headLocked = 1;
        }
    }
    void ShowOverlay(vr::VROverlayHandle_t h) override
    {
        if (ivrxr::Layer *l = L(h))
            l->visible = 1;
    }
    void HideOverlay(vr::VROverlayHandle_t h) override
    {
        if (ivrxr::Layer *l = L(h))
            l->visible = 0;
    }
    void SetOverlayTexture(vr::VROverlayHandle_t, const vr::Texture_t &) override {}
    void SetOverlaySharedTexture(vr::VROverlayHandle_t h, uint64_t handle, uint32_t w, uint32_t hg) override
    {
        ivrxr::Layer *l = L(h);
        if (!l)
            return;
        if (l->kind != ivrxr::LAYER_SHARED || l->sharedHandle != handle || l->width != w || l->height != hg)
        {
            l->kind = ivrxr::LAYER_SHARED;
            l->sharedHandle = handle;
            l->width = w;
            l->height = hg;
            ++l->serial;
        }
    }

private:
    ivrxr::Layer *L(vr::VROverlayHandle_t h) const
    {
        if (!m_link || !m_link->shm || h == 0 || h > (vr::VROverlayHandle_t)m_used)
            return nullptr;
        return &m_link->shm->layers[h - 1];
    }
    xr::Link *m_link;
    int m_used = 0;
};

} // namespace

OverlayApi *MakeOpenVROverlays(vr::IVROverlay *ov) { return new OpenVROverlays(ov); }
OverlayApi *MakeOpenXROverlays(xr::Link *link) { return new OpenXROverlays(link); }

} // namespace ivr
