// INFRA VR - the mod's own VR hands: the GLB models from infravr\models\hands drawn on both
// controllers in each eye, fingers bent from SteamVR finger tracking (or grip/trigger), INFRA's
// viewmodel arms hidden ([hands] model = glb). Same split as Black Mesa VR (custom hand models
// drawn by the mod) with Half-Life 2 VR's behaviour (always visible, finger curl).
//
// Lighting ([hands] lighting = world): the engine is asked how it would light a model where each
// hand is (IVModelRender::ComputeLightingState: ambient cube + up to 4 local lights, times the HDR
// exposure), and a small vertex/pixel shader applies that per pixel with the models' normal maps.
// Without a shader compiler the same light is applied per vertex.
//
// The camera, the flashlight and the phone ([camera] / [flashlight] / [phone] model): a GLB of the item with the
// hand holding it, rigged to the same bones as INFRA's v_camera.mdl / v_light.mdl, drawn on the VR
// hand that holds the item. While INFRA shows the item, its animated bones are copied every frame
// (BuildTransformations) and move the item in the hand (photos, changing the battery); INFRA's own
// viewmodel is hidden. The camera's screen shows the lens view, taken from the model's lens.
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

// --- held-item models (camera, flashlight) ----------------------------------------------------
constexpr int ITEM_CAMERA = 0, ITEM_FLASHLIGHT = 1, ITEM_PHONE = 2, ITEM_COUNT = 3;
// True when the GLB hands are on and that item's model loaded.
bool ItemModelEnabled(int item);
// Skin joint names of the item's model, in skin order (bone names of INFRA's viewmodel).
const std::vector<std::string> &ItemJointNames(int item);
// From the viewmodel bone setup (any thread): world matrices of INFRA's viewmodel bones, one per
// ItemJointNames() entry, in game units.
void SetItemBones(int item, const std::vector<GlbMat34> &world);
// The camera model is being drawn (fresh bones from INFRA's camera).
bool CameraModelShown();
// The lens of the camera model in the world (x forward out of the lens, z up), when shown.
bool CameraLens(Pose &out);
// The flashlight model's beam in the world (x along the beam from the lens, z up), when shown.
bool FlashlightBeam(Pose &out);
// The phone model's screen in the world (x right, y up, z out of the glass) and its size in game
// units, when the phone model is drawn.
bool PhoneScreen(Pose &out, float &width, float &height);
// Where one of INFRA's screen panels on the item's viewmodel goes on the item model: the panel's
// centre and frame (x along its width, y up it, z out of it) in the space of INFRA's bone `bone`
// (game units) -> the world, on the model as it is drawn (mirrored back to be readable when the
// model is mirrored). Main thread, while the model is drawn.
// Width / height of the camera model's screen.
float CameraScreenAspect();
// The camera model's screen in the world as last drawn: upper left, upper right and lower left corners.
bool CameraScreenCorners(Vector &ul, Vector &ur, Vector &ll);
// `scale`: how much bigger the model is drawn than INFRA's own viewmodel.
bool ItemPanel(int item, const char *bone, const Vector &centre, const Mat3 &frame, Pose &out, float &scale);

} // namespace hands
} // namespace ivr
