# Runtime notes

What has been checked where, and the rules for writing it down.

## Status grades

- **static**: read from the binaries (disassembly, RTTI, vtables).
- **wine**: exercised by a test program under Wine against INFRA's real DLLs or the real files.
- **headset**: seen working in INFRA with a headset. Only the maintainer's test runs count.
- **unverified**: written and built, not yet run.
- **verified-fail**: checked and it does not exist or does not work. Keep these: they stop the
  next person from trying the same thing (Black Mesa VR's convention).

A change moves a line up only with evidence: a log excerpt, a test output or a screenshot.

## Current state

| Feature | Grade | Notes |
|---|---|---|
| Stereo rendering, OpenVR submit, overlays, MSAA | headset | unchanged from the previous build |
| Module layout, crash-sticky flags, skip file, crash logger, init mutex | unverified | builds; flag handling exercised by resolve_test |
| world_eye_size hook | wine | signature found once, hook installs; buffers at the eye size in game is unverified |
| Post-processing in VR | unverified | only on when world_eye_size reports resized buffers |
| Stereo once per Present | unverified | logs "A second main view before Present" when it acts |
| Present handoff only when needed | unverified | |
| Console commands at the RenderView safe point | unverified | fallback from the frame update while no 3D view is drawn |
| GLB hand models load and bend | wine | `tests/glb_test` (19 finger joints, fist moves the tips 0.16 m) |
| GLB hands placement on the controllers | unverified | orientation derived offline from the models, tune with `[hands] offset/angles` |
| GLB hands drawing in the eyes, depth test | unverified | log line "GLB hands: eye target ... depth test on/off" |
| INFRA's arms hidden in glb mode | unverified | log line "GLB hands: ... bones hidden" |
| Hands lit by the world (`[hands] lighting = world`): engine ambient cube + 4 local lights per hand, times the HDR exposure | unverified | log lines "Hand lighting at (...)" every 30 s and "Hand lighting: ... frames lit by the engine"; falls back to simple lighting when the engine call is missing |
| Hand shaders (per-pixel light, normal maps) | wine | `tests/shader_test` compiles both with INFRA's `bin\d3dcompiler_41.dll` under Wine; Wine's own d3dcompiler_47 cannot (vkd3d), so the mod tries 47, 43, then INFRA's 41. In game: log "Hand shaders compiled with ..." and "GLB hands: drawn with world lighting and normal maps" |
| Normal map green channel | unverified | `hands.normal_map_green = up/down`; if bumps look inverted, flip it |
| Camera model (`[camera] model`) on the VR hand, moved in the hand by v_camera's animated bones relative to INFRA's hand (the arm motion and the take-out animation are not used), all of v_camera hidden | unverified (first headset run had it at INFRA's viewmodel position instead; moved to the VR hand since) |
| Camera model skinned from v_camera's bones | wine | `tests/glb_test` skins it with game-style bones (hand around the camera, scale consistent); in game: "Camera model: v_camera has N of the model's 22 bones" |
| Camera model screen showing the lens view, lens view taken from the model's lens | unverified | screen cropped to the screen's aspect; lens pose is one frame behind the bones |
| Camera model held in the left hand (mirrored) | unverified | text on the camera body appears mirrored |
| Flashlight model (`[flashlight] model`, flashlight_hand.glb) on the flashlight's VR hand, moved by v_light's bones the same way | wine (loads, tangents; `tests/glb_test`) | in game: "Flashlight model: weapons/v_light.mdl has N of the model's 22 bones" |
| Flashlight beam from the flashlight model's lens (found at load: the item mesh's wider end, faces pointing out) | unverified | log "Flashlight model: 22 joints, beam from the lens on joint flashlight"; without the model drawn the beam comes from the hand with `flashlight_offset` |
| Phone model (`[phone] model`, phone_hand.glb) on the phone hand, moved by v_phone's bones, in the phone menu and INFRA's phone scenes | unverified | log "Phone model: weapons/v_phone.mdl has N of the model's 22 bones"; while the pause menu is up its last bones are kept |
| Phone menu panel on the phone model's screen (fitted inside the glass, across your view) | unverified | log "Phone model: 22 joints, screen found (...)"; without the phone model the panel stays at the hand as before |
| INFRA's screen panels (its menu on v_phone, the viewfinder on v_camera) moved onto the mod's phone / camera model: the bone carrying the `controlpanel0_ll` attachment (no vertices on it) is turned and placed so the panel lands centred on the model's screen | unverified | log "Screen panel: weapons/v_phone.mdl has INFRA's screen on bone 53 (2.27 x 3.84 units) - moved onto the mod's model". The panel keeps INFRA's size, so on the larger VR-sized model it covers about 80% of the glass. Before this, hiding the viewmodel collapsed that bone and the panel floated beside the model |
| Phone call texture (`[phone] call_texture`) while v_phone's material shows INFRA's call screen | unverified | INFRA's v_phone has one skin, so the call screen must come from a texture swap on its material; log "Phone model: INFRA's phone texture is ..." on every change shows what INFRA does. The swap is assumed to name a texture with "skin1" |
| Hidden viewmodel bones collapse to one point (each to its own point left dark slivers by the hand) | unverified | |
| Hands drawn with the viewmodels, before post-processing (`hands.post_processing`) | unverified | log "GLB hands: drawn with the viewmodels, before post-processing"; if the hands vanish or flicker, set it to false |
| OpenXR helper | unverified | 64-bit build compiles; not run (no 64-bit Wine here, no headset) |
| DXVK D3D9 shared textures for OpenXR | verified-fail | headset (Virtual Desktop OpenXR, RTX 4070 SUPER): DXVK warns "Failed to write shared resource info", the helper's `OpenSharedResource` fails with 0x80070057, the headset stays black |
| OpenXR off-center eyes (each eye drawn with its own field of view, `vr.offcenter_eyes`) | unverified | builds; log line "Off-center eyes: each eye drawn at ..."; if the picture looks stretched or shifted, the engine ignores the off-center fields: set `offcenter_eyes = false` |
| OpenXR handoff through D3D11 shared textures imported into Vulkan | unverified | builds; replaces the line above. Crash-sticky flag `openxr_interop`; log lines "OpenXR: shared textures are made on ..." and, in `infravr_openxr.log`, "Opened shared texture" |

## Logs

- `INFRA\infravr.log`: the mod. Look for `Flags:`, `world_eye_size:`, `GLB hands:`, `OpenXR`.
- `INFRA\infravr_openxr.log`: the OpenXR helper.
- `INFRA\infravr_skip.txt`: mechanisms switched off after a crash, one per line with the reason.
