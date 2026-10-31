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

## materialsystem.dll

| What | Value | Status |
|---|---|---|
| "game started" flag in CMaterialSystem (cleared briefly to create the eye render targets) | +0x2BC8 | wine, headset |
| vtable slot 33 GetBackBufferDimensions | RVA 0x167e0 (jumps to the shader device vtable +0xC) | static, wine |
| vtable slots 91 / 92 Begin/EndRenderTargetAllocation | 91 = RVA 0xb250 | static, wine |
| vtable slot 96 CreateNamedRenderTargetTextureEx | RVA 0xb170 | static, wine |
| vtable slot 112 GetRenderContext | RVA 0xbc00 | static, wine |
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

## How to re-check after a game update

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686-mingw.cmake -DINFRAVR_TESTS=ON
cmake --build build
wine build/resolve_test.exe 'Z:\path\to\infra'
```

Every value above is printed; `world_eye_size install 1` means the size-mode signature still
matches exactly once and the hook goes in.
