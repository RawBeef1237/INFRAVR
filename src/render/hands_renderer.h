// INFRA VR - the mod's own VR hands: the GLB models from bin\infravr\hands drawn on both
// controllers in each eye, fingers bent from SteamVR finger tracking (or grip/trigger), INFRA's
// viewmodel arms hidden ([hands] model = glb). Same split as Black Mesa VR (custom hand models
// drawn by the mod) with Half-Life 2 VR's behaviour (always visible, finger curl).
#pragma once
#include "common.h"

namespace ivr {
struct CViewSetup;

namespace hands {

// True when [hands] model = glb and both models loaded (loads them on first call).
bool Enabled();
// Main thread, right after the engine drew eye `eye` with view `v` (queued for the render thread).
void DrawEye(int eye, const CViewSetup &v);
// Before a D3D9 device reset.
void OnDeviceReset();

} // namespace hands
} // namespace ivr
