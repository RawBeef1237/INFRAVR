# INFRA VR

A VR mod for **INFRA** (Loiste Interactive, 2016) with motion controls. It works like
L4D2VR(https://github.com/keyou91/l4d2vr) and Portal 2 VR(https://github.com/Gistix/portal2vr): a `d3d9.dll`
in INFRA's `bin` folder hooks the Source engine, the game is rendered through DXVK (Vulkan), and
the frames go to SteamVR through OpenVR.

## What changed in 0.13

- **World following your head while holding the phone:** with the phone out, INFRA keeps handing
  back the same view angles and ignores the ones the mod sets. The mod read that as "the game turned
  you" and turned the world to match, cancelling every head turn. It now only follows a real turn by
  the game, and never while the phone is out.
- **Camera screen:** the camera's screen shows a copy of the frame that the game makes just before
  it draws the arms. The eye images are larger than that copy's texture, so the copy failed and the
  screen kept an old picture (the main menu). While the camera is out the mod now draws one extra
  view from the camera lens in your hand and lets that fill the screen. `[camera] screen_view` turns
  it off, and `screen_every` (default 2, every other frame) sets how often it is redrawn, since the
  extra view costs GPU time.
- **Hold grip to carry:** physics objects are only carried while you hold a grip. Letting go drops
  them (or throws them if your hand is moving fast), and an object picked up with the trigger is put
  down again straight away. A trigger press no longer drops what a grip is holding.
  `[holsters] grip_to_carry = false` brings back the old behaviour.

## What changed in 0.12

- **Flashlight beam in the left hand:** INFRA does not use Source's usual flashlight code; it has
  its own (the `infra_flashlight_*` settings). The mod now moves that one to the flashlight hand,
  which is the left hand by default. The log notes the first time it does ("INFRA flashlight: moved
  ...").
- **Looking around in the phone menu:** if the game stops drawing new 3D frames while its pause
  menu (the phone) is open, the last frame is shown again together with the head position it was
  drawn for. SteamVR then keeps it fixed in the world, so you can look around instead of the view
  following your head. When the menu closes the log says how many frames that happened for.

## What changed in 0.11

- **Arms in the corner of the view:** the pass that draws the first-person models (arms,
  flashlight, camera, phone) was being given the flat 1920x1080 desktop view, and only for the
  left eye. That is what put the arms in the top-left corner. Each eye now draws them with its own
  view.
- **Flashlight beam:** the beam now always comes from the flashlight hand (left by default), even
  if the holster and the game disagree about whether the flashlight is out. The log notes the
  first few flashlight updates ("Flashlight update ...").
- **16:9 desktop window:** the desktop window now shows a 16:9 crop of the left eye that fills the
  window. `mirror_fill = false` brings back the whole eye image with black bars.
- The camera's viewfinder no longer swaps to the lens view in that pass. It never drew the
  viewfinder there, only the arms.

## What changed in 0.10

- **Arms fixed after the first tracked-hands test:** the flashlight model is now held by the hand
  the flashlight beam comes from (in 0.9 it sat on the other controller), and its offsets are
  mirrored for the left hand. The shoulders, upper arms and body of the arm models are now hidden
  on both sides, so the big sleeve that filled the corner of the view is gone; only forearms and
  hands show.

## What changed in 0.9

- **Both hands tracked:** INFRA's camera, flashlight and phone models already contain both arms.
  Until now the whole model followed the hand holding the item, so the other arm stayed wherever
  the animation put it. Now that arm follows your other controller, holding the same grip mirrored,
  and its upper arm and shoulder are hidden so only the forearm and hand show. Turn it off with
  `tracked = false` under `[hands]` in `infravr.ini`.
- **Finger tracking read from SteamVR:** the controllers' finger curl is read (new skeleton actions
  in the bindings) and logged every 5 seconds for the first minute ("Finger curl ..."). Fingers do
  not move on the models yet; this checks what Quest 3 and PSVR2 report first.

## What changed in 0.8

- **Crash at start-up on PCs with two GPUs:** the crash reports from 0.7 showed SteamVR crashing
  inside the NVIDIA driver while taking the game's first picture, with an AMD graphics driver also
  loaded (typically a CPU with built-in graphics next to the graphics card). The game could end up
  rendering on one GPU while SteamVR runs on the other. The mod now asks SteamVR which GPU it uses
  and makes DXVK render on that one. The log lists the GPUs and says which was picked; a
  `dxvk.deviceFilter` line in `infravr\dxvk.conf` still overrides it.

## What changed in 0.7

- **Warped view on Quest 3:** the mod read SteamVR's up/down view angles the wrong way round. On
  headsets that see as far up as down (PSVR2, Index) that made no difference; on Quest 3, which
  sees further one way, the picture sat too high or low and warped as you looked around. Fixed.
- **Phone menu in the world:** the in-game pause menu is now a panel standing in front of you
  (a little below eye level, tilted toward you) instead of a screen on the phone in your hand,
  and the phone model is hidden while it is open. Point with either hand and pull the trigger,
  or use the sticks, A and B. `phone_menu_in_world = false` brings back the hand-held screen.

## What changed in 0.6

- **Frozen picture after loading a level, real fix:** on a level load INFRA frees every texture
  nobody holds on to, and that included the mod's eye images. From then on the game drew the view
  into its window and the headset kept the last frame. The mod now holds on to its images, and if
  the game ever draws the eyes into the window again it makes new ones (the log says so).
- **Main menu on the flat screen:** INFRA's main menu is a live 3D scene. It now shows on the flat
  virtual screen like the other menus (point and pull the trigger, or left stick and A). The pause
  menu in game stays on the phone in your hand. `main_menu_in_vr = true` brings back the 3D menu.

## What changed in 0.5

- **Frozen picture after loading a level:** when a level loads, INFRA swaps the eye images for new
  ones. The mod kept sending the old ones, so the headset and the window showed the last frame
  from before the load while the game carried on. It now follows the game to the new images
  (the log says "the game replaced its texture").

## What changed in 0.4

- **Menus with the controllers, gamepad style:** in the main menu and other flat menus the left
  stick moves between menu items, A selects and B goes back, like INFRA's own gamepad support.
  Pointing with the beam and pulling the trigger still clicks; the right stick scrolls.
- **Clicks reach the game:** if the INFRA window is not in front (for example after clicking the
  SteamVR window), the mod now brings it back before clicking. The log says when Windows refuses.
- **Frozen view diagnostics:** every probe now logs how many frames went to SteamVR and how many it
  showed, and warns when the eye picture stops changing.

## What changed in 0.3

- **Stretched lines, second round:** the screenshot from 0.2 showed the game only draws a
  16:9 picture in the top-left of each eye image (about 1568x882 of 2564x2634) and the rest is the
  last row and column smeared out. 0.3 forces `mat_viewportscale` back to 1 and, if the game
  still only fills part of the image, draws the view into just that part and shows only that part
  in the headset (`auto_fit = true` in infravr.ini). That costs some sharpness but no more lines.
- **Better log:** `smear check`, `auto_fit` and `trace:` lines say exactly where the picture ends
  and which render step does it, and changed console settings are logged with their old values.

## What changed in 0.2

- **Stretched lines in the headset:** INFRA's full-screen effects (bloom, colour correction,
  vignette, depth of field) are built for the window's size; on the larger eye images they smeared
  into lines. The mod now turns them off while in VR (`post_processing = false` in infravr.ini).
  If lines remain, set `render_fit_window = true` (sharpness drops to the window's resolution, but
  every effect lines up) and send the log - the `probe:` lines now also count smeared rows/columns.
- **Phone menu in your hand:** the in-game pause menu (INFRA's phone) no longer switches to the flat
  screen. The game stays in 3D, the phone moves to your off hand and its menu shows on a screen
  above it. Either stick moves through the menu, A selects, B goes back, the pause button closes
  it, and you can also point at it with your other hand and pull the trigger.
  `phone_menu = false` brings back the old flat screen.

## What changed in 0.1 (after the first headset test)

- **Menus:** the menu screen is now solid and flat. Before, it was copied from the game window with
  the engine's leftover alpha channel, so it showed up patchy and see-through in the headset.
- **Pointing and clicking with the controllers:** a short beam comes out of your hand and a white
  dot shows where it meets the screen; the trigger clicks. Version 0.0 relied on SteamVR's own
  overlay laser, which takes the controllers away from the game. The INFRA window is also brought
  to the front so clicks register.
- **Screen mode:** hold the pause button for a second to show the flat game on the virtual screen
  (for any menu that appears in 3D without a pointer). Hold again to go back.
- **Black in-game view:** the mod now checks its own output and writes the result to
  `infravr.log` (`probe:` lines). If the subtitle/HUD panel turns out to be opaque (it would sit in
  front of your face as a black panel), it switches to drawing subtitles into the view instead. It
  also checks that the texture the game drew into is the one sent to the headset, and switches if
  not. The desktop window now mirrors the left eye even with anti-aliasing on.
- **Controls follow INFRA's gamepad layout:** crouch is a tap of the right stick (hold it 1 s to
  recenter) and sprint is a toggle on the left stick click, like INFRA's own controller support.
  B (right) is reload and also "back" in menus.
- No more `IVRCompositor::Submit failed ... 101` at startup (frames are only sent once SteamVR has
  handed out the first poses).

## Version numbers

Versions are `0.MINOR.PATCH`. Small updates (bug fixes, tweaks to defaults) raise the third digit,
for example 0.13 to 0.13.1. Larger updates (new features or reworked systems) raise the second digit
and reset the third, for example 0.13.2 to 0.14.

## Requirements

- INFRA on Steam (Windows, 32-bit `infra.exe`)
- SteamVR (any headset SteamVR supports: Index, Vive, Quest via Link/Air Link/Virtual Desktop/Steam Link, WMR, Pico...)
- A GPU with Vulkan 1.3 drivers

DXVK 2.7.1 (32-bit) and Valve's `openvr_api.dll` are included - nothing else to download.

## Install

1. Close INFRA.
2. Run `install.bat` (it finds `C:\Program Files (x86)\Steam\steamapps\common\infra` automatically;
   for another library drag the INFRA folder onto `install.bat`). It copies:
   - `bin\d3d9.dll` - the mod
   - `bin\dxvk_d3d9.dll` - DXVK
   - `bin\openvr_api.dll` - OpenVR
   - `bin\infravr\` - `infravr.ini`, `dxvk.conf`, SteamVR controller bindings

   It also removes leftovers of versions 1 and 2.
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

**Pause menu (phone):** it appears on a panel in front of you. Sticks move through it, A
selects, B goes back, pause closes it; or point at it with either hand and pull the trigger.

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
overlay | eyes | off`), holster sizes, viewmodel offsets, and the console commands the
holsters run. `bin\infravr\dxvk.conf` - DXVK options (set `dxvk.deviceFilter` on multi-GPU PCs).

## Troubleshooting

Everything is logged to **`INFRA\infravr.log`** (DXVK writes `infra_d3d9.log` next to it).

| Symptom | What to check in infravr.log |
|---|---|
| Game starts flat, nothing in the headset | `SteamVR is not available` - start SteamVR first. `openvr_api.dll` missing -> reinstall |
| Headset shows only a grey/black void in menus | the menu screen is placed where you were looking when it opened - look around; `D3D9 device is not DXVK` = `bin\dxvk_d3d9.dll` missing |
| In-game view black | look at the `probe:` lines - `eye brightness avg 0 max 0` means the game drew nothing into the headset images; `HUD panel ... opaque` means the subtitle panel was covering the view (it switches itself off). The desktop window mirrors the left eye. Send the log |
| Stutters | lower `render_scale`; make sure `mat_queue_mode 0` is in the launch options; turn off MSAA in video options |
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

## Building

MinGW-w64 (i686) or Visual Studio (Win32):

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 tools/gen_bindings.py build/infravr/actions
```

`config/actions/` holds the generated SteamVR action manifest and default bindings.

`tests/` contains the checks used during development: `resolve_test.cpp` loads INFRA's real DLLs
and verifies every hook target and interface index; `d3d_test.cpp` + `fake_openvr.cpp` run the
D3D9 -> DXVK -> OpenVR submission path (under Wine + lavapipe) and read the submitted pixels back
through Vulkan.

## Credits & licences

- Approach from [L4D2VR](https://github.com/keyou91/l4d2vr) and Gistix's
  [Portal 2 VR](https://github.com/Gistix/portal2vr).
- [DXVK](https://github.com/doitsujin/dxvk) (zlib), [OpenVR](https://github.com/ValveSoftware/openvr) (BSD-3),
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0/MIT).
- INFRA is (c) Loiste Interactive. This is an unofficial fan mod.
