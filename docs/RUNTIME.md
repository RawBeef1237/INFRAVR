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
| Eye image size (auto_fit removed) | unverified | the view always fills the whole eye image; the maintainer saw it look fine with auto_fit off. Builds |
| Stereo once per Present | unverified | logs "A second main view before Present" when it acts |
| Present handoff only when needed | unverified | |
| Console commands at the RenderView safe point | unverified | fallback from the frame update while no 3D view is drawn |
| GLB hand models load and bend | wine | `tests/glb_test` (19 finger joints, fist moves the tips 0.16 m) |
| GLB hands placement on the controllers | unverified | orientation derived offline from the models, tune with `[hands] offset/angles` |
| GLB hands drawing in the eyes, depth test | unverified | log line "GLB hands: eye target ... depth test on/off" |
| INFRA's arms hidden in glb mode | unverified | log line "GLB hands: ... bones hidden" |
| OpenXR helper | unverified | 64-bit build compiles; not run (no 64-bit Wine here, no headset) |
| DXVK D3D9 shared textures for OpenXR | unverified | the main open question: if stock DXVK refuses `pSharedHandle`, the log says so and `backend = openvr` is the way |

## Logs

- `INFRA\infravr.log`: the mod. Look for `Flags:`, `world_eye_size:`, `GLB hands:`, `OpenXR`.
- `INFRA\infravr_openxr.log`: the OpenXR helper.
- `INFRA\infravr_skip.txt`: mechanisms switched off after a crash, one per line with the reason.
