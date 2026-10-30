# Architecture

INFRA VR is a `d3d9.dll` proxy in `INFRA\bin`. INFRA's shader API loads it instead of the system
D3D9; it chain-loads stock DXVK (`dxvk_d3d9.dll`), hooks the Source engine with MinHook, draws
every main view twice (once per eye) and hands the eye images to the headset runtime.

```
infra.exe -> shaderapidx9.dll -> bin\d3d9.dll (INFRA VR) -> bin\dxvk_d3d9.dll (DXVK) -> Vulkan
                                    |
                                    +-- vr backend: openvr_api.dll (in process)
                                    |            or infravr_openxr.exe (64-bit helper, shared memory)
                                    v
           client.dll / engine.dll / server.dll / materialsystem.dll (hooks)
```

## Modules

| Folder | What lives there | May depend on |
|---|---|---|
| `src/core` | logging, `infravr.ini`, crash-sticky flags and the skip file, crash logger | nothing |
| `src/engine` | Source structures, interface/signature resolution, engine detours (`hooks_game.cpp`) | core, and the render/gameplay entry points it drives |
| `src/render` | the d3d9 proxy, DXVK interop and frame submission, eye-size render targets, OpenXR texture publishing, the GLB hand renderer | core, engine types, vr |
| `src/vr` | `VrRuntime` (OpenVR and OpenXR backends), the overlay API, the shared-memory bridge | core |
| `src/gameplay` | `VRMod`: locomotion, turning, holsters, grabbing, menus, the console command queue | core, engine, vr |
| `src/ui` | the phone-menu gamepad emulation | core, vr, gameplay |
| `helper/openxr` | `infravr_openxr.exe`, the 64-bit OpenXR session owner | `src/vr/openxr_bridge.h` only |

## Frame flow (single-threaded, `mat_queue_mode 0`)

1. `Present` (render/d3d9_proxy): `D3DVR::OnPresent` resolves MSAA, then submits the eye pair
   (OpenVR: Vulkan images via `IVRCompositor::Submit`; OpenXR: copies into shared textures for the
   helper) and the menu/HUD panels.
2. After `Present`: the stereo guard is reset, then `VRMod::OnFrame` waits for the next poses
   (`WaitGetPoses`, or the helper's frame event) and reads input. `PostPresentHandoff` is only
   called when the poses are read elsewhere (multicore) or `explicit_handoff = true`.
3. `CViewRender::RenderView` (engine/hooks_game): queued console commands run here (the safe
   point), then the main view is drawn per eye into the eye render targets with the eye's pose and
   the headset's field of view. Each eye render is bracketed by `world::BeginEye/EndEye` so
   post-processing works out its rectangles for the eye. The GLB hands are drawn into each eye
   right after it (queued on the render queue, so this also works with multicore).
4. A second main view before the next `Present` is drawn flat (stereo once per Present).

With multicore (`mat_queue_mode 2`), `Present` runs on the render thread: poses and input are
read at the start of `RenderView` on the main thread instead, and D3D work the hooks need is
queued with `Defer` so it runs in order with the engine's own commands.

## Eye-size engine buffers (`world_at_eye_size`)

INFRA sizes its full-frame buffers from `CMaterialSystem::GetBackBufferDimensions`, i.e. the
desktop window. The hook answers with the eye size only when the caller is
`CTexture::ApplyRenderTargetSizeMode` (found by signature, see OFFSETS.md) or while an eye is being
drawn on the main thread. Post-processing is switched on only when this worked; otherwise it is
forced off as before. No GPU fence is needed between the eyes: INFRA draws straight into separate
eye render targets and the GPU runs the commands in order.

## VR backends

`VrRuntime` hides the backend from the rest of the mod: poses (Source axes), input actions,
finger curls, haptics and overlays are the same either way.

- **openvr** (default): `openvr_api.dll` is initialised before DXVK creates its Vulkan instance,
  so DXVK enables the extensions SteamVR needs. This also covers OpenXR-only headsets through
  OpenOpenVR / OpenComposite, the recommended route for them.
- **openxr** (experimental): OpenXR runtimes are 64-bit only, so the session lives in
  `infravr_openxr.exe` (Black Mesa VR's design). The game creates the shared memory, starts the
  helper with its process id and waits until it reports the headset. The helper publishes FOV,
  head/aim poses and input with a seqlock and signals a frame event after `xrWaitFrame`. The game
  copies each eye pair into one of three shared D3D9 textures (DXVK exports them), hands the slot
  over once an event query shows the copy finished, and the helper copies it into its swapchains
  with the head pose the frame was drawn for. Menu/HUD panels and the pointer are quad layers.
  If the helper does not come up the mod falls back to OpenVR (both start before DXVK, so this
  is safe).

## Crash-sticky mechanisms

Risky mechanisms write `infravr_in_<name>.flag` next to `infra.exe` before they start and remove
it once they have run for a while. A flag left behind means INFRA stopped while that mechanism
was running: on the next start it is added to `infravr_skip.txt` and stays off. Current names:
`world_eye_size`, `glb_hands`, `openxr_start`. `[debug] retry_skipped = true` (or deleting the
line) tries again; the installer clears the skip file on upgrade. An unhandled-exception filter
logs the crash address as module + offset together with the mod's current stage.

Only one copy of the mod is active per process (a named mutex); a second load just forwards to
DXVK.

## Hands

`[hands] model = glb` (default): `GlbModel` loads the two `.glb` files with cgltf/stb_image,
recognises the biped finger joints and skins on the CPU each frame with the finger curls
(SteamVR skeletal summary, OpenXR trigger/squeeze, or the grip/trigger buttons eased in). The
renderer draws both hands with fixed-function D3D9, pre-lit, one draw call per hand, depth-tested
against the eye's depth buffer when it matches. INFRA's viewmodel arm bones are collapsed; the
camera, phone and flashlight bones are kept so the held item still shows. `model = infra` keeps
INFRA's own arms following the controllers.
