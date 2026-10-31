// INFRA VR - the mod's own VR hands: the GLB models from bin\infravr\hands drawn on both
// controllers in each eye, fingers bent from SteamVR finger tracking (or grip/trigger), INFRA's
// viewmodel arms hidden ([hands] model = glb). Same split as Black Mesa VR (custom hand models
// drawn by the mod) with Half-Life 2 VR's behaviour (always visible, finger curl).
//
// Lighting ([hands] lighting = world): the engine is asked how it would light a model where each
// hand is (IVModelRender::ComputeLightingState: ambient cube + up to 4 local lights, times the HDR
// exposure), and a small vertex/pixel shader applies that per pixel with the models' normal maps.
// Without a shader compiler the same light is applied per vertex.
//
// The camera ([camera] model): a GLB of the camera with the hand holding it, rigged to the same
// bones as INFRA's v_camera.mdl. While INFRA shows its camera, its animated bones are copied every
// frame (BuildTransformations) and drive the GLB, so taking the camera out, taking photos and
// changing the battery animate it; INFRA's own camera model is hidden. Its screen shows the lens
// view and the lens view is taken from the model's lens.
#pragma once
#include "common.h"
#include "mathlib.h"

#include <string>
#include <vector>

namespace ivr {
struct CViewSetup;
struct GlbMat34;

namespace hands {

// True when [hands] model = glb and both models loaded (loads them on first call).
bool Enabled();
// Main thread, right after the engine drew eye `eye` with view `v` (queued for the render thread).
void DrawEye(int eye, const CViewSetup &v);
// Before a D3D9 device reset.
void OnDeviceReset();

// --- camera model ---------------------------------------------------------------------------
// True when the GLB hands are on and the camera model loaded.
bool CameraModelEnabled();
// Skin joint names of the camera model, in skin order (INFRA's v_camera bone names).
const std::vector<std::string> &CameraJointNames();
// From the viewmodel bone setup (any thread): world matrices of INFRA's v_camera bones, one per
// CameraJointNames() entry, in game units.
void SetCameraBones(const std::vector<GlbMat34> &world);
// The camera model is being drawn (fresh bones from INFRA's camera).
bool CameraModelShown();
// The lens of the camera model in the world (x forward out of the lens, z up), when shown.
bool CameraLens(Pose &out);

} // namespace hands
} // namespace ivr
