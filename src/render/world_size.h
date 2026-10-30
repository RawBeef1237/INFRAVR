// INFRA VR - engine render targets at the eye size ("world_eye_size").
//
// INFRA sizes its full-frame buffers (_rt_FullFrameFB, the bloom/HDR scratch targets...) from
// CMaterialSystem::GetBackBufferDimensions, i.e. the desktop window. Drawn into the larger eye
// images, every post-processing pass then covered only part of the eye and smeared the rest into
// lines, which is why post-processing used to be switched off in VR.
//
// Black Mesa VR fixed the same problem by making the engine allocate those buffers at the eye
// size. INFRA has no override API for this (SetRenderTargetFrameBufferSizeOverrides is not in its
// material system), so this module hooks GetBackBufferDimensions and answers with the eye size
//   - when the caller is CTexture's size-mode code (ApplyRenderTargetSizeMode, found by
//     signature), so full-frame targets are created - and re-created after a device reset - at
//     the eye size, and
//   - on the main thread while an eye view is being drawn, so post-processing works out its
//     rectangles for the eye instead of the window.
// The desktop window, menus and the HUD keep seeing the real window size.
#pragma once
#include "common.h"

namespace ivr {
namespace world {

// Installs the hook (once). materials = VMaterialSystem080. Returns false if anything is missing;
// the mod then works as before (post-processing off).
bool Install(void *materials, uint32_t eyeWidth, uint32_t eyeHeight);

// True when the hook is in and full-frame targets were sized for the eyes at least once.
bool Active();
// How many render target sizes were answered with the eye size.
int ResizedTargets();

// Brackets the drawing of one eye view (main thread).
void BeginEye();
void EndEye();

// Called once per stereo frame; after enough frames the crash-sticky flag is cleared.
void OnStereoFrame();

} // namespace world
} // namespace ivr
