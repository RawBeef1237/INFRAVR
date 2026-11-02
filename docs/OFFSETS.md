# Offsets and signatures

Everything the mod reads or hooks in INFRA, with how it was found and how sure we are.
INFRA build: Steam app 251110, PatchVersion 3.3.3 (Portal 2 / Alien Swarm branch of Source).

Status grades (see RUNTIME.md): **static** = read from the binary, **wine** = resolved by
`tests/resolve_test` loading INFRA's real DLLs, **headset** = seen working in the game,
**unverified**, **verified-fail** = checked and it does not exist or does not work.

RVAs are relative to the module base. The mod never relies on an RVA alone: functions are found
by RTTI vtable + slot or by a unique byte signature, and the RVA is the cross-check.

## Interfaces

| Interface | Module | Status |
|---|---|---|
| VEngineClient015 | engine.dll | wine |
| VEngineRenderView013 | engine.dll | wine |
| VClient016 | client.dll | static |
| VClientEntityList003 | client.dll | wine |
| VMaterialSystem080 | materialsystem.dll | wine |
| VGUI_Surface031 | vguimatsurface.dll | wine |
| VGUI_InputInternal001 | vgui2.dll | not found under Wine (vgui2 needs the running game) |
| VEngineModel016 (IVModelRender) | engine.dll | static |

## materialsystem.dll

| What | Value | Status |
|---|---|---|
| "game started" flag in CMaterialSystem (cleared briefly to create the eye render targets) | +0x2BC8 | wine, headset |
| vtable slot 33 GetBackBufferDimensions | RVA 0x167e0 (jumps to the shader device vtable +0xC) | static, wine |
| vtable slots 91 / 92 Begin/EndRenderTargetAllocation | 91 = RVA 0xb250 | static, wine |
| vtable slot 96 CreateNamedRenderTargetTextureEx | RVA 0xb170 | static, wine |
| vtable slot 112 GetRenderContext | RVA 0xbc00 | static, wine |
| vtable slot 81 FindMaterial(name, group, complain, prefix) | RVA 0x17390, `ret 0x10` | static (Windows DLL; the macOS dylib's CMaterialSystem vtable has FindMaterial at 81 and GetRenderContext at 112) |
| IMaterial slot 0 GetName, slot 11 FindVar(name, found*, complain) | CMaterial vtable RVA 0x92c5c: slot 0 `add ecx,0x18; jmp` (name symbol), slot 11 `ret 0xc` | static (Windows and macOS agree) |
| IMaterialVar slot 0 GetTextureValue, ITexture slot 0 GetName | CMaterialVar vtable RVA 0x962d4, CTexture vtable RVA 0x983ec | static (Windows and macOS agree) |
| IMatRenderContext slot 144 / 145 Set/GetToneMappingScaleLinear (145 returns a Vector through a hidden result pointer, `ret 4`; the queued context copies its +0x1c0) | slot numbers | static (MaterialSystem.dll and the macOS materialsystem.dylib symbols agree) |
| vtable slot 13: a two-int setter writing +0x2ac4 / +0x2ac8 | probably SetAdapter, not a size override | static |
| SetRenderTargetFrameBufferSizeOverrides (what Black Mesa VR uses) | not present in INFRA | verified-fail |
| CTexture::ApplyRenderTargetSizeMode | RVA 0x2e3b0, 0x138 bytes up to its jump table (0x1002e4e8 at the preferred base) | static, wine (one signature hit) |
| its signature | `55 8B EC 0F B7 41 34 56 8B 75 08 89 06 0F B7 51 36 57 8B 7D 0C 89 17 8B 41 50 48 83 F8 05` | wine |
| g_pMaterialSystem used by it | global at 0x100fa738 -> 0x101046f0 (the same object as VMaterialSystem080); calls vtable +0x84 = slot 33 | static |

`ApplyRenderTargetSizeMode` reads the requested size (ushorts at +0x34 / +0x36), switches on the
size mode (+0x50, cases 1..6) and calls GetBackBufferDimensions for every mode that depends on the
frame buffer. world_size.cpp answers with the eye size when the return address is inside it.

## client.dll

| What | RVA | Status |
|---|---|---|
| CViewRender::RenderView | 0x198e90 | wine, headset |
| CViewRender::DrawViewModels (camera weapon pass) | 0x198820 | wine, headset |
| ClientModeShared::CreateMove | 0xa4910 | wine, headset |
| C_BasePlayer::CalcViewModelView | 0x1ea9e0 | wine, headset |
| CFlashlightEffect::UpdateLight | 0xcb160 | wine, headset |
| C_INFRA_Player vtable / slot 243 | 0x4e0b2c / 0x52fa0 | wine |
| C_BaseAnimating bone array | +0xB10 | wine, headset |
| mstudiobone_t size (MDL v49) | 216 | headset |
| CViewSetup size / fov / origin / aspect | 0xF4 / 0x58 / 0x60 / 0x88 | static asserts, headset |
| CViewSetup off-center top / bottom / left / right, m_bOffCenter = bit 0 of the flag byte | 0xE0 / 0xE4 / 0xE8 / 0xEC, flags 0xF0 | unverified (Alien Swarm SDK layout, counted from the aspect at 0x88; `vr.offcenter_eyes = false` turns its use off) |
| g_INFRAScreenClicker | found by RTTI | wine |

## server.dll

| What | RVA | Status |
|---|---|---|
| CGrabController::UpdateObject | 0x3a9820 | wine, headset |
| CGrabController::UpdateObject (viewmodel variant) | 0x3ab320 | wine |
| CGrabController::ComputeError | 0x3a7740 | wine |
| CBasePlayer::EyeAngles | 0xd78f0 | wine, headset |
| Weapon_ShootPosition | 0x34bbc0 | wine, headset |

## engine.dll / vguimatsurface.dll

| What | Value | Status |
|---|---|---|
| IVRenderView slot 38 (VGui_Paint) | RVA 0x11a040 | wine, headset |
| IVEngineClient slot 104 (ExecuteClientCmd) | RVA 0x71140 | wine, headset |
| ISurface slot 56 (IsCursorVisible) | RVA 0x5850 | wine |
| IVModelRender slot 26 ComputeLightingState(int count, const LightingQuery_t*, MaterialLightingState_t*, ITexture**) | RVA 0xe2da0 (0x100e2da0 at the preferred base), `__thiscall`, `ret 0x10` | static (engine.dll, cross-checked with the engine.dylib symbols) |
| LightingQuery_t | 0x10 bytes: origin 0, instance (ushort, 0xFFFF = none) 0xC, ambient boost (bool) 0xE | static |
| MaterialLightingState_t | 0x1B8 bytes: ambient cube (6 Vectors) 0, lighting origin 0x48, local light count 0x54, 4 x LightDesc_t 0x58 | static |
| LightDesc_t | 0x58 bytes: type 0, color 4, position 0x10, direction 0x1C, range 0x28, falloff 0x2C, attenuation 0/1/2 0x30/0x34/0x38, theta/phi 0x3C/0x40, thetaDot/phiDot 0x44/0x48, oneOver 0x4C, flags 0x50, rangeSq 0x54 | static |

## models

| What | Value | Status |
|---|---|---|
| v_camera.mdl | 55 bones (both arms, `Camera`, `w_camera003`, flashlight on the left hand), 7 sequences: idle, fire, dryfire, draw, reload_draw, holster, reload | static (read from the MDL) |
| v_light.mdl | 55 bones (`Bip001 L Hand` 33, `flashlight` 34, `body` 35), sequences idle01, draw (11 frames at 20 fps), reload_draw, holster, reload | static (read from the MDL) |
| v_phone.mdl | one skin family (mobile_phone_001, player_body_work, player_hands); material models/weapons/v_phone/mobile_phone_001 ($basetexture models/props_office/mobile_phone_001); sequences idle01, fire, answer, talk_idle, hangup, draw (13 frames at 30 fps), holster, text | static (read from the MDL and VMT) |
| v_phone.mdl / v_camera.mdl screen panel attachments | v_phone: `controlpanel0_ll` / `_ur` on bone 53 `cellular_phone`, local (0.2, -1.75, 1.13) / (0.2, 2.09, -1.14), axes rows (0 0 1 / 0 1 0 / -1 0 0), panel 2.27 x 3.84 units. v_camera: on bone 11 `Camera`, local (1.8, 0, -1.3) / (-1.7, 0, 1.25), axes rows (-1 0 0 / 0 0 1 / 0 1 0), panel 3.5 x 2.55. Both panels sit on the GLB models' screens in rest pose (same axes, centre within 6 mm) | static (read from the MDLs, checked against the GLBs) |
| studiohdr_t numlocalattachments / localattachmentindex | 240 / 244; mstudioattachment_t is 92 bytes (name offset, flags, local bone at 8, matrix3x4_t at 12) | static (read from v_phone.mdl, v_camera.mdl) |
| INFRA's screen panels | C_VGuiScreen (CINFRAPhoneScreen, CINFRACameraScreen, CINFRAMenu*Screen in client.dll RTTI) parented to the viewmodel's controlpanel0_ll attachment; the attachment follows the bone matrices left after BuildTransformations. No vertices are skinned to `cellular_phone` (v_phone) or `Camera` (v_camera) | static (client.dll strings; v_phone.vvd / v_camera.vvd weights) |
| phone_hand.glb | 22 skin joints (`Bip001 R Hand` first, fingers, `cellular_phone`, `Plane002`); made in the same space as vr_hand_right.glb; screen at texture 0.041 0.133 - 0.480 0.879, a plane about 7.3 x 12.4 cm | static |
| flashlight_hand.glb | 22 skin joints named like v_light's (`Bip001 L Hand` first, fingers, `flashlight`, `body`); made in the same space as vr_hand_left.glb | static, wine |
| camera_hand.glb | 22 skin joints named like v_camera's (`Bip001 R Hand`, fingers, `w_camera003`); its local bone transforms equal v_camera's bind pose; camera mesh skinned to `w_camera003` only; no animations of its own | static, wine (`tests/glb_test`) |
| camera_hand.glb texture: screen quad / lens | UV 0.113 0.134 to 0.486 0.404 / centre 0.891 0.716, radius 0.044 (v is stored +1, wrapped) | static |

## How to re-check after a game update

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686-mingw.cmake -DINFRAVR_TESTS=ON
cmake --build build
wine build/resolve_test.exe 'Z:\path\to\infra'
```

Every value above is printed; `world_eye_size install 1` means the size-mode signature still
matches exactly once and the hook goes in.
