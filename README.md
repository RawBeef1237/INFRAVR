# INFRA VR 0.8.7

A VR mod for **INFRA** (Loiste Interactive, 2016) with motion controls. It works like
[L4D2VR](https://github.com/keyou91/l4d2vr) and [Portal 2 VR](https://github.com/Gistix/portal2vr): a `d3d9.dll`
in INFRA's `bin` folder hooks the Source engine, the game is rendered through DXVK (Vulkan), and
the frames go to SteamVR through OpenVR.

## What changed in 0.8.7

- **Main menu pointer after using the phone menu:** once INFRA has seen a controller, its main
  menu screen lets the controller's sticks move the mouse cursor every frame, which fought the VR
  pointer. That controller cursor is now switched off while the pointer drives the main menu (the
  log says `Main menu: INFRA's controller cursor switched off ...`). The first clicks in a menu
  now also log whether the cursor is visible and what INFRA's screen cursor is doing.

## What changed in 0.8.6

- **Phone menu controls fixed:** INFRA ships its own copy of the Xbox controller library
  (`bin\xinput1_3.dll`) and reads the controller through that copy, while 0.8.5 only hooked the
  Windows one, so INFRA never saw the VR controllers as a gamepad. Both copies are hooked now. The
  log shows `XInput: hooked ...\bin\xinput1_3.dll` at startup and `XInput: INFRA reads
  controller 1` once the game polls it.

## What changed in 0.8.5

- **Phone menu with real gamepad controls:** the cursor dot is gone. While the phone menu is open
  the VR controllers act as an Xbox controller for INFRA: the right stick is the right stick, the
  left stick the left stick, A is A, B is B, the triggers are the triggers and the stick clicks are
  the stick clicks, so the menu behaves exactly as it does with a controller plugged in. Outside
  the phone menu that virtual controller sits idle (a real controller still works). The holsters,
  the phone button and pause still close the menu. `[vr] phone_gamepad = false` sends menu keys
  instead.

## What changed in 0.8.4

- **Phone menu in your hand:** taking the phone out of its hip holster (or the phone button) opens
  INFRA's phone (pause) menu again, but now as the screen of the phone in your hand: it moves with
  the controller and the phone model stays visible. Putting the phone back, the phone button or
  pause closes it. `[vr] phone_menu_in_world = true` brings back the standing panel.
- **Phone menu controls that work:** INFRA's own controller support does not step through its
  phone screens, it moves a cursor over them with the stick and clicks with A. 0.8.3 sent menu
  buttons the menu never took. Now the right stick moves a cursor (a dot on the phone screen), A
  clicks and B goes back; the other buttons do nothing while the menu is open, and the holsters
  keep working. No pointer beam in the phone menu (`phone_menu_pointer = true` brings it back);
  the main menu still has it. `phone_cursor_speed` sets the cursor speed.
- 0.8.3's ini also switched the phone menu itself off by mistake (`[vr] phone_menu`); replace the
  ini with this one.

## What changed in 0.8.3

- **The phone is a held item again:** taking it out of its holster no longer opens the pause
  menu. While the phone is out, the right stick, A and B go to the phone's screen (move, select,
  back) instead of turning, jumping and reloading; moving, grabbing and the holsters work as
  usual, and putting the phone away brings the normal buttons back. `[holsters] phone_menu = true`
  brings back 0.8.2's phone-opens-the-pause-menu behaviour. The log notes whether the phone's
  screen took each button ("Phone menu: controller button ...").

## What changed in 0.8.2

- **Startup crash:** all four startup crashes so far (0.5 on 7 October, 0.8 and 0.8.1 today) are
  the same one: NVIDIA's Vulkan driver fails inside SteamVR's first frame hand-off while INFRA is
  still loading its main menu map. They happened whether or not multicore was on. The mod now sends
  nothing to SteamVR until the main menu map has loaded (or 20 seconds have passed); SteamVR keeps
  its own loading screen up for that second or two. The log says "Sending frames to SteamVR" when
  it starts.
- **The phone takes over the controls:** taking the phone out of its hip holster (or pressing the
  phone button) now opens INFRA's phone menu, like the phone does in INFRA. While it is out the
  right stick moves through the menu, A selects, B goes back, and moving, turning, jumping and
  reloading are off. Putting the phone back in its holster, the phone button or pause closes it and
  the normal controls come back. The holsters keep working while the phone is out.
  `[holsters] phone_menu = false` brings back holding the phone without its menu.

## What changed in 0.8.1

- **Startup crash with 0.8:** the crash happened inside NVIDIA's Vulkan driver while DXVK-GPLALL
  was allocating memory for a draw in the main menu, before the mod's multicore option could switch
  anything on. The stock DXVK 2.7.1 that ran fine through 0.7 is the default again; DXVK-GPLALL is
  still included as `bin\dxvk_d3d9_gplall.dll` and `[vr] dxvk = gplall` picks it. The D3D device
  is now also created thread-safe, because INFRA's own "multicore rendering" video option starts
  the game on its render thread before the mod switches it off. The log now shows the game's
  starting `mat_queue_mode` and the device flags.
- **Phone menu controls:** while the phone (pause) menu is open, the right stick moves between
  items, A selects and B goes back. They are sent to the menu as INFRA's own controller buttons, so
  B steps back a page instead of closing the whole menu.

## What changed in 0.8

- **DXVK from L4D2VR:** the included DXVK is now DXVK-GPLALL 2.6.1-6, the same build L4D2VR
  uses (stock DXVK 2.6.1 plus background shader compiling). New effects no longer make the game
  hitch while their shaders compile. `dxvk.enableAsync = False` in `bin\infravr\dxvk.conf`
  turns that off if something shows up missing for a moment.
- **Antialiasing:** the eye images are drawn with 4x MSAA, so edges are smooth instead of jagged.
  `[vr] antialiasing` sets it to 0 (off), 2, 4 or 8. INFRA's own antialiasing option only ever
  affected the desktop window. The log says "Antialiasing: eyes drawn with 4x MSAA" when it is on.
- **Multicore rendering (experimental, off by default):** `[vr] multicore = true` lets INFRA
  render on a second CPU core (`mat_queue_mode 2`), which can raise the frame rate when the game is
  CPU bound. The mod hands its per-frame work to the engine's render thread and sends each frame
  with the head pose it was drawn for. Menus and level loads switch back to one core; the log
  says "Multicore rendering on/off".
- **Crouch in real life:** when your head drops 0.30 m (`[movement] crouch_drop`) below your
  standing height, you crouch in game too, so you can duck under things. The game's own lowering
  of the view is cancelled out, so the view only goes as low as your head. `real_crouch = false`
  turns it off. Recenter while standing straight so it knows your height.

## What changed in 0.7

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

## What changed in 0.6.3

- **Flashlight beam in the left hand:** INFRA does not use Source's usual flashlight code; it has
  its own (the `infra_flashlight_*` settings). The mod now moves that one to the flashlight hand,
  which is the left hand by default. The log notes the first time it does ("INFRA flashlight: moved
  ...").
- **Looking around in the phone menu:** if the game stops drawing new 3D frames while its pause
  menu (the phone) is open, the last frame is shown again together with the head position it was
  drawn for. SteamVR then keeps it fixed in the world, so you can look around instead of the view
  following your head. When the menu closes the log says how many frames that happened for.

## What changed in 0.6.2

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

## What changed in 0.6.1

- **Arms fixed after the first tracked-hands test:** the flashlight model is now held by the hand
  the flashlight beam comes from (in 0.6 it sat on the other controller), and its offsets are
  mirrored for the left hand. The shoulders, upper arms and body of the arm models are now hidden
  on both sides, so the big sleeve that filled the corner of the view is gone; only forearms and
  hands show.

## What changed in 0.6

- **Both hands tracked:** INFRA's camera, flashlight and phone models already contain both arms.
  Until now the whole model followed the hand holding the item, so the other arm stayed wherever
  the animation put it. Now that arm follows your other controller, holding the same grip mirrored,
  and its upper arm and shoulder are hidden so only the forearm and hand show. Turn it off with
  `tracked = false` under `[hands]` in `infravr.ini`.
- **Finger tracking read from SteamVR:** the controllers' finger curl is read (new skeleton actions
  in the bindings) and logged every 5 seconds for the first minute ("Finger curl ..."). Fingers do
  not move on the models yet; this checks what Quest 3 and PSVR2 report first.

## What changed in 0.5.1

- **Crash at start-up on PCs with two GPUs:** the crash reports from 0.5 showed SteamVR crashing
  inside the NVIDIA driver while taking the game's first picture, with an AMD graphics driver also
  loaded (typically a CPU with built-in graphics next to the graphics card). The game could end up
  rendering on one GPU while SteamVR runs on the other. The mod now asks SteamVR which GPU it uses
  and makes DXVK render on that one. The log lists the GPUs and says which was picked; a
  `dxvk.deviceFilter` line in `infravr\dxvk.conf` still overrides it.

## What changed in 0.5

- **Warped view on Quest 3:** the mod read SteamVR's up/down view angles the wrong way round. On
  headsets that see as far up as down (PSVR2, Index) that made no difference; on Quest 3, which
  sees further one way, the picture sat too high or low and warped as you looked around. Fixed.
- **Phone menu in the world:** the in-game pause menu is now a panel standing in front of you
  (a little below eye level, tilted toward you) instead of a screen on the phone in your hand,
  and the phone model is hidden while it is open. Point with either hand and pull the trigger,
  or use the sticks, A and B. `phone_menu_in_world = false` brings back the hand-held screen.

## What changed in 0.4

- **Frozen picture after loading a level, real fix:** on a level load INFRA frees every texture
  nobody holds on to, and that included the mod's eye images. From then on the game drew the view
  into its window and the headset kept the last frame. The mod now holds on to its images, and if
  the game ever draws the eyes into the window again it makes new ones (the log says so).
- **Main menu on the flat screen:** INFRA's main menu is a live 3D scene. It now shows on the flat
  virtual screen like the other menus (point and pull the trigger, or left stick and A). The pause
  menu in game stays on the phone in your hand. `main_menu_in_vr = true` brings back the 3D menu.

## What changed in 0.3.1

- **Frozen picture after loading a level:** when a level loads, INFRA swaps the eye images for new
  ones. The mod kept sending the old ones, so the headset and the window showed the last frame
  from before the load while the game carried on. It now follows the game to the new images
  (the log says "the game replaced its texture").

## What changed in 0.3

- **Menus with the controllers, gamepad style:** in the main menu and other flat menus the left
  stick moves between menu items, A selects and B goes back, like INFRA's own gamepad support.
  Pointing with the beam and pulling the trigger still clicks; the right stick scrolls.
- **Clicks reach the game:** if the INFRA window is not in front (for example after clicking the
  SteamVR window), the mod now brings it back before clicking. The log says when Windows refuses.
- **Frozen view diagnostics:** every probe now logs how many frames went to SteamVR and how many it
  showed, and warns when the eye picture stops changing.

## What changed in 0.2.1

- **Stretched lines, second round:** the screenshot from 0.2 showed the game only draws a
  16:9 picture in the top-left of each eye image (about 1568x882 of 2564x2634) and the rest is the
  last row and column smeared out. 0.2.1 forces `mat_viewportscale` back to 1 and, if the game
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
  dot shows where it meets the screen; the trigger clicks. Version 0.0 (the original release) relied on SteamVR's own
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

## Why version 2 showed a black headset

INFRA does not run on the Source SDK 2013 engine. Its binaries are the **Portal 2 / Alien Swarm
branch** of Source (same interfaces as Portal 2: `VEngineClient015`, `VClient016`,
`VEngineRenderView013`, `VMaterialSystem080`...). Version 2 used SDK 2013 interface versions and
function numbers, so its engine hooks never installed and nothing was ever drawn into the headset
textures. On top of that, INFRA's material system refuses to create render targets after the game
has started, so the eye textures could not be created either.

This version is rebuilt on layouts read directly out of INFRA's own `engine.dll`, `client.dll`,
`server.dll` and `materialsystem.dll`, and every function it hooks is checked against INFRA's
binaries (the mod finds them again by signature/RTTI if an address ever moves). It uses OpenVR.

## Version numbers

Versions are `0.MINOR.PATCH`. Small updates (bug fixes, tweaks to defaults) raise the third digit,
for example 0.8 to 0.8.1. Larger updates (new features or reworked systems) raise the second digit
and reset the third, for example 0.8.7 to 0.9.

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
- [DXVK](https://github.com/doitsujin/dxvk) (zlib; DXVK 2.7.1 and [DXVK-GPLALL](https://github.com/Digger1955/dxvk-gplall) 2.6.1-6, commit 7927e64, d3d9 only),  [OpenVR](https://github.com/ValveSoftware/openvr) (BSD-3),
  [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache-2.0/MIT).
- INFRA is (c) Loiste Interactive. This is an unofficial fan mod.
