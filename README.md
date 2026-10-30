# INFRA VR

A VR mod for **INFRA** (Loiste Interactive, 2016) with motion controls (still a work in progress). It works like
[L4D2VR](https://github.com/keyou91/l4d2vr) and [Portal 2 VR](https://github.com/Gistix/portal2vr): a `d3d9.dll`
in INFRA's `bin` folder hooks the Source engine, the game is rendered through DXVK (Vulkan), and
the frames go to SteamVR through OpenVR (or, experimentally, to the headset's own OpenXR runtime).

> **Work in progress:** the motion controls (hands, holsters, grabbing, the phone and camera in
> your hands) are still being worked on and may feel rough in places.

## Features

- **Full stereo 3D** in SteamVR at the headset's own resolution and field of view, with 4x MSAA and
  an adjustable render scale. Works with any SteamVR headset (Quest via Link/Air Link/Virtual
  Desktop/Steam Link, Index, Vive, WMR, Pico...).
- **Motion controls (work in progress):** stick movement, snap or smooth turning, room-scale
  movement and real crouching, and "use" aimed along your hand.
- **VR hands:** hand models on both controllers, always visible, with the fingers following
  finger tracking (Index) or the grip and trigger. INFRA's own arms can be used instead
  (`[hands] model = infra`).
- **Post-processing in VR:** INFRA's bloom, colour correction and vignette work in the headset
  (the engine's buffers are made at the eye size).
- **Crash-safe:** anything risky that ever crashed INFRA switches itself off on the next start
  (`infravr_skip.txt`), and the log names where a crash happened.
- **Grab and throw** props with the grip; they follow your hand.
- **Holsters:** flashlight on the left hip, phone on the right hip, camera over your shoulder.
- **Camera in your hand:** the viewfinder shows the view from the lens, and photos are aimed along
  it.
- **Flashlight** shines from the hand that holds it.
- **Phone menu in your hand:** the pause menu opens on the phone you are holding, and the VR
  controllers work it like an Xbox controller.
- **Menus on a big virtual screen** with a laser pointer (point and pull the trigger), plus stick
  navigation like INFRA's gamepad support. Hold pause for **screen mode** (the flat game on the
  virtual screen) at any time.
- **Floating HUD panel** for subtitles and hints.
- **Remappable controls** through SteamVR's binding editor, defaults for Index, Touch, Cosmos,
  Reverb G2 and Vive wands.
- **One-click install and uninstall**, with DXVK and OpenVR included.
- **Detailed log** (`infravr.log`) for troubleshooting.

Release notes for every version are on the [Releases](https://github.com/RawBeef1237/infravr/releases) page.

## Requirements

- INFRA on Steam (Windows, 32-bit `infra.exe`)
- SteamVR (any headset SteamVR supports: Index, Vive, Quest via Link/Air Link/Virtual Desktop/Steam Link, WMR, Pico...)
- A GPU with Vulkan 1.3 drivers

DXVK 2.7.1 (32-bit; DXVK-GPLALL 2.6.1-6, the build L4D2VR uses, is included as an option) and Valve's `openvr_api.dll` are included - nothing else to download.

## Install

1. Close INFRA.
2. Run `install.bat` (it finds `C:\Program Files (x86)\Steam\steamapps\common\infra` automatically;
   for another library drag the INFRA folder onto `install.bat`). It copies:
   - `bin\d3d9.dll` - the mod
   - `bin\dxvk_d3d9.dll` - DXVK
   - `bin\openvr_api.dll` - OpenVR
   - `bin\infravr\` - `infravr.ini`, `dxvk.conf`, SteamVR controller bindings
   - `bin\infravr\hands\` - the VR hand models
   - `bin\infravr\openxr\` - the OpenXR helper (only used with `backend = openxr`)

3. Steam -> INFRA -> Properties -> Launch options:

   `-windowed -novid +mat_queue_mode 0`

4. Start SteamVR, then INFRA. Menus and loading screens appear on a large screen in front of you:
   point a controller at it and pull the trigger to click.

`uninstall.bat` removes everything again.

## Controls (defaults, right-handed)

| | Index | Touch / Cosmos / Reverb G2 | Vive wands |
|---|---|---|---|
| Move | left stick | left stick | left pad |
| Sprint (toggle, like INFRA's gamepad) | left stick click | left stick click | left pad click |
| Turn (snap 45 deg) | right stick | right stick | right pad |
| Jump | A (right) | A | right pad click |
| Crouch (toggle, like INFRA's gamepad) | tap right stick click | tap right stick click | tap right menu |
| Use / press buttons | trigger | trigger | trigger |
| Grab & hold objects | grip (hold) | grip (hold) | grip (hold) |
| Take photo (holding camera) | trigger of the camera hand | same | same |
| Flashlight | A (left) or left-hip holster | X or left-hip holster | left-hip holster |
| Phone | left trackpad click or right-hip holster | right-hip holster | right-hip holster |
| Reload (camera battery) | B (right) or right trackpad click | B | - |
| Pause menu | tap B (left) | tap Y | tap left menu |
| Screen mode on/off | hold B (left) 1 s | hold Y 1 s | hold left menu 1 s |
| Recenter & measure height | hold right stick click 1 s | hold right stick click 1 s | hold right menu 1 s |

**Phone (pause) menu:** taking the phone out of its holster, the phone button or pause opens it on
the phone in your hand. The VR controllers work it like an Xbox controller (sticks, A, B,
triggers); putting the phone back or pause closes it.

**Menus:** the left stick moves between items, A selects (like INFRA's gamepad support). Or point at the screen with either controller (a short beam comes out of the hand and a
white dot shows where you point), pull the trigger to click, right stick up/down scrolls, B / the
right menu button goes back. **Screen mode** shows the flat game on the virtual screen even while
playing; use it if a menu shows up in 3D without a pointer.

You can remap everything in SteamVR -> Settings -> Controllers -> Manage controller bindings -> INFRA.

**Grabbing:** squeeze the grip while your hand points at a prop to pick it up; it is held in front
of that hand and follows it. Let go of the grip to drop it, or let go while swinging to throw it.
**Buttons/doors/levers:** point with a hand and pull the trigger ("use" is aimed along your hand).

### Holsters

Reach to the spot and squeeze **grip** - the controller buzzes when your hand is over a holster.

| Where | What |
|---|---|
| Left hip | Flashlight on/off. The beam comes out of the hand that took it |
| Right hip | Phone out / put away |
| Over your shoulder, behind your head | Camera out / put away (into that hand) |

Hip height is measured when you recenter - stand straight, hold the right stick click for a second.

### The camera

With the camera out, its viewfinder screen shows the view **from the camera lens** in your hand,
not from your head, and photos are aimed along the lens. If the camera model doesn't line up with
your controller, adjust `offset_camera`, `angles` and `lens_offset` in `bin\infravr\infravr.ini`.

## Settings

`INFRA\bin\infravr\infravr.ini` - render scale, snap/smooth turning, HUD placement (`hud_mode =
overlay | eyes | off`), holster sizes, viewmodel offsets, hand model placement (`[hands]`), the VR
runtime (`backend = openvr | openxr`), and the console commands the holsters run. `bin\infravr\dxvk.conf` - DXVK options (set `dxvk.deviceFilter` on multi-GPU PCs).

## Troubleshooting

Everything is logged to **`INFRA\infravr.log`** (DXVK writes `infra_d3d9.log` next to it).

| Symptom | What to check in infravr.log |
|---|---|
| Game starts flat, nothing in the headset | `SteamVR is not available` - start SteamVR first. `openvr_api.dll` missing -> reinstall |
| Headset shows only a grey/black void in menus | the menu screen is placed where you were looking when it opened - look around; `D3D9 device is not DXVK` = `bin\dxvk_d3d9.dll` missing |
| In-game view black | look at the `probe:` lines - `eye brightness avg 0 max 0` means the game drew nothing into the headset images; `HUD panel ... opaque` means the subtitle panel was covering the view (it switches itself off). The desktop window mirrors the left eye. Send the log |
| Stutters | lower `render_scale`; make sure `mat_queue_mode 0` is in the launch options; turn off MSAA in video options |
| Something stopped working after a crash | `Flags: ... stays off` - that mechanism crashed INFRA once and is listed in `infravr_skip.txt`; delete its line (or set `retry_skipped = true`) to try again |
| Hands float or point the wrong way | adjust `offset` / `angles` under `[hands]` (right hand; the left is mirrored) |
| Menu doesn't react to clicks | the mod brings the INFRA window to the front when you click; if Windows refuses, click the INFRA window once on the desktop |

## How it works

```
infra.exe -> shaderapidx9.dll -> bin\d3d9.dll (INFRA VR) -> bin\dxvk_d3d9.dll (DXVK) -> Vulkan
                                        |   openvr_api.dll is initialised first, so DXVK creates its
                                        |   Vulkan device with the extensions SteamVR needs
                                        v
             client.dll / engine.dll / server.dll / materialsystem.dll (hooks)
```

- **Rendering** - `CViewRender::RenderView` is run once per eye into two render targets (created by
  temporarily lifting the material system's "game started" guard), with the headset's field of
  view. At `Present`, the Vulkan images behind those D3D9 textures are handed to
  `IVRCompositor::Submit` through DXVK's public interop interface (layout transition + queue lock).
- **HUD** - the in-game VGUI (subtitles, hints) is painted into its own transparent texture and
  shown as a floating panel. Menus/loading screens are the desktop frame shown as a virtual screen
  with SteamVR's laser pointer.
- **Input** - `ClientModeShared::CreateMove`: stick locomotion, room-scale (your body follows your
  head), snap turn, and "use"/attack aimed along the controller or the camera lens.
- **Grab** - server `CGrabController::UpdateObject`: while a held object is updated, the player's
  eye angles/shoot position are replaced by the controller's, so objects follow your hand.
- **Viewmodel / flashlight / camera** - `C_BasePlayer::CalcViewModelView`,
  `CFlashlightEffect::UpdateLight` and INFRA's camera viewfinder pass are moved to your hands.

More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (modules, frame flow, backends),
[docs/OFFSETS.md](docs/OFFSETS.md) (every hooked function and how it was found) and
[docs/RUNTIME.md](docs/RUNTIME.md) (what has been tested where).

## Building

MinGW-w64 (i686) or Visual Studio (Win32):

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 tools/gen_bindings.py build/infravr/actions
```

The OpenXR helper is a separate 64-bit build (MinGW-w64 x86_64 or Visual Studio x64), plus
Khronos' loader next to it:

```
cmake -S helper/openxr -B build-xr -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-xr
tools/fetch_openxr_loader.sh build-xr
```

Package layout: `bin\infravr\hands\` gets `assets/hands/*.glb`, `bin\infravr\openxr\` gets
`infravr_openxr.exe` and `openxr_loader.dll`. `config/actions/` holds the generated SteamVR action
manifest and default bindings. The log's version string comes from `-DINFRAVR_VERSION=...`.

`tests/` contains the checks used during development (`-DINFRAVR_TESTS=ON`): `resolve_test.cpp`
loads INFRA's real DLLs and verifies every hook target, interface index and the eye-size
signature; `glb_test.cpp` loads the hand models and checks the fingers bend; `d3d_test.cpp` +
`fake_openvr.cpp` run the D3D9 -> DXVK -> OpenVR submission path (under Wine + lavapipe) and read
the submitted pixels back through Vulkan.

## Credits & licences

- Approach from [L4D2VR](https://github.com/keyou91/l4d2vr) and Gistix's
  [Portal 2 VR](https://github.com/Gistix/portal2vr); crash-sticky mechanisms, eye-size engine
  buffers, the OpenXR helper process and stereo-once-per-Present from
  [Black Mesa VR](https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr).
- [DXVK](https://github.com/doitsujin/dxvk) (zlib; DXVK 2.7.1 and [DXVK-GPLALL](https://github.com/Digger1955/dxvk-gplall) 2.6.1-6, commit 7927e64, d3d9 only),  [OpenVR](https://github.com/ValveSoftware/openvr) (BSD-3),
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0/MIT),
  [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK-Source) headers and loader (Apache-2.0),
  [cgltf](https://github.com/jkuhlmann/cgltf) (MIT), [stb_image](https://github.com/nothings/stb) (MIT / public domain).
- INFRA is (c) Loiste Interactive. This is an unofficial fan mod.
