// INFRA VR - overlay panels (menu screen, HUD/phone panel, pointer dot and beam) for either VR
// backend. The calls mirror the IVROverlay subset the mod used before, so gameplay code reads the
// same: OpenVR forwards them to IVROverlay, OpenXR turns them into quad layers that the helper
// process composes (openxr_bridge.h).
#pragma once
#include "runtime.h"

namespace ivr {

class OverlayApi
{
public:
    virtual ~OverlayApi() = default;
    virtual vr::EVROverlayError CreateOverlay(const char *key, const char *name, vr::VROverlayHandle_t *out) = 0;
    virtual void SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float meters) = 0;
    virtual void SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t order) = 0;
    // RGBA8 pixels, copied.
    virtual void SetOverlayRaw(vr::VROverlayHandle_t h, const void *pixels, uint32_t w, uint32_t hgt,
                               uint32_t bytesPerPixel) = 0;
    // Placement in the standing space (OpenVR overlay convention: the panel is the X/Y plane, facing +Z).
    virtual void SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, const vr::HmdMatrix34_t &xf) = 0;
    // Placement relative to the headset.
    virtual void SetOverlayTransformHeadRelative(vr::VROverlayHandle_t h, const vr::HmdMatrix34_t &xf) = 0;
    virtual void ShowOverlay(vr::VROverlayHandle_t h) = 0;
    virtual void HideOverlay(vr::VROverlayHandle_t h) = 0;
    // Picture of the panel. OpenVR takes a Vulkan texture, OpenXR a shared D3D texture handle.
    virtual void SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t &tex) = 0;
    virtual void SetOverlaySharedTexture(vr::VROverlayHandle_t h, uint64_t sharedHandle, uint32_t w, uint32_t hgt) = 0;
};

OverlayApi *MakeOpenVROverlays(vr::IVROverlay *ov);
OverlayApi *MakeOpenXROverlays(xr::Link *link);

} // namespace ivr
