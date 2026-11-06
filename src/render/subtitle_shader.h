// INFRA VR - the subtitle panel's pixel shader and draw pass (subtitles.cpp; tests/subtitle_test.cpp
// runs the same pass on known pixels).
#pragma once
#include <d3d9.h>
#include <algorithm>

namespace ivr {
namespace subtitles {

// ps_2_0, compiled from this HLSL with D3DCompile (Wine 9 d3dcompiler_47, target ps_2_0):
//   sampler2D s : register(s0);
//   float4 k : register(c0); // x = gain, y = background opacity
//   float4 main(float2 uv : TEXCOORD0) : COLOR
//   {
//       float4 c = tex2D(s, uv);
//       float a = saturate(max(max(c.r, c.g), max(c.b, c.a)) * k.x);
//       float o = max(a, k.y);
//       return float4(saturate(c.rgb / max(o, 0.004)), o);
//   }
// The HUD is painted onto transparent black, so its colours are already multiplied by coverage:
// dividing by the new alpha gives the straight colour SteamVR blends with.
inline const DWORD kSubtitlePS[] = {
    0xffff0200, 0x0025fffe, 0x42415443, 0x0000001c, 0x0000006c, 0xffff0200, 0x00000002, 0x0000001c,
    0x00000000, 0x00000000, 0x00000044, 0x00000002, 0x00000001, 0x00000048, 0x00000000, 0x00000058,
    0x00000003, 0x00000001, 0x0000005c, 0x00000000, 0xabab006b, 0x00030001, 0x00040001, 0x00000001,
    0x00000000, 0xabab0073, 0x000c0004, 0x00010001, 0x00000001, 0x00000000, 0x33646b76, 0x68732d64,
    0x72656461, 0x312e3120, 0x57282030, 0x20656e69, 0x646e7562, 0x2964656c, 0xababab00, 0x05000051,
    0xa00f0001, 0x3b83126f, 0x00000000, 0x00000000, 0x00000000, 0x0200001f, 0x80000000, 0xb0030000,
    0x0200001f, 0x90000000, 0xa00f0800, 0x02000001, 0x80030000, 0xb0040000, 0x02000001, 0x800f0001,
    0xa0e40000, 0x03000042, 0x800f0000, 0x80e40000, 0xa0e40800, 0x02000001, 0x80010002, 0x80000000,
    0x02000001, 0x80010003, 0x80010000, 0x0300000b, 0x80010002, 0x80000002, 0x80000003, 0x02000001,
    0x80010003, 0x80020000, 0x02000001, 0x80010004, 0x80030000, 0x0300000b, 0x80010003, 0x80000003,
    0x80000004, 0x0300000b, 0x80010002, 0x80000002, 0x80000003, 0x02000001, 0x80010003, 0x80000001,
    0x03000005, 0x80010002, 0x80000002, 0x80000003, 0x02000001, 0x80110002, 0x80000002, 0x02000001,
    0x80010001, 0x80010001, 0x0300000b, 0x80010001, 0x80000002, 0x80000001, 0x02000001, 0x80070000,
    0x80240000, 0x02000001, 0x80010002, 0xa0000001, 0x0300000b, 0x80010002, 0x80000001, 0x80000002,
    0x02000001, 0x80070002, 0x80000002, 0x02000006, 0x80010002, 0x80000002, 0x02000006, 0x80020002,
    0x80550002, 0x02000006, 0x80040002, 0x80aa0002, 0x03000005, 0x80070000, 0x80240000, 0x80240002,
    0x02000001, 0x80170000, 0x80240000, 0x02000001, 0x80010002, 0x80000000, 0x02000001, 0x80010002,
    0x80000002, 0x02000001, 0x80010003, 0x80010000, 0x02000001, 0x80020002, 0x80000003, 0x02000001,
    0x80010000, 0x80020000, 0x02000001, 0x80040002, 0x80000000, 0x02000001, 0x80080002, 0x80000001,
    0x02000001, 0x800f0000, 0x80e40002, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff,
};

// The subtitle region of a HUD texture: r = left top right bottom in HudLayout.res units (480 =
// the screen height, x measured from the screen centre like "c-250"), clamped to the texture.
inline RECT Region(const float r[4], UINT width, UINT height)
{
    float s = (float)height / 480.0f, cx = width * 0.5f;
    auto x = [&](float v) { return (LONG)std::clamp(cx + v * s, 0.0f, (float)width); };
    auto y = [&](float v) { return (LONG)std::clamp(v * s, 0.0f, (float)height); };
    return RECT{x(r[0]), y(r[1]), x(r[2]), y(r[3])};
}

// Draws src (w x h) into dst through ps with the alpha rebuilt. Changes device state: the caller
// saves and restores it.
inline bool DrawPanel(IDirect3DDevice9 *device, IDirect3DTexture9 *src, IDirect3DSurface9 *dst, UINT width,
                      UINT height, IDirect3DPixelShader9 *ps, float gain, float background)
{
    device->SetRenderTarget(0, dst);
    device->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = {0, 0, width, height, 0.0f, 1.0f};
    device->SetViewport(&vp);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(ps);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, src);
    const DWORD ss[][2] = {
        {D3DSAMP_MINFILTER, D3DTEXF_POINT},    {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE},     {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
        {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP}, {D3DSAMP_SRGBTEXTURE, FALSE},
    };
    for (const auto &v : ss)
        device->SetSamplerState(0, (D3DSAMPLERSTATETYPE)v[0], v[1]);
    const DWORD rs[][2] = {
        {D3DRS_ZENABLE, FALSE},          {D3DRS_ZWRITEENABLE, FALSE},          {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE},  {D3DRS_SCISSORTESTENABLE, FALSE},     {D3DRS_FOGENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE},         {D3DRS_CLIPPLANEENABLE, 0},           {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_COLORWRITEENABLE, 0xF},
    };
    for (const auto &v : rs)
        device->SetRenderState((D3DRENDERSTATETYPE)v[0], v[1]);
    float k[4] = {std::max(0.01f, gain), background, 0, 0};
    device->SetPixelShaderConstantF(0, k, 1);

    struct Vtx
    {
        float x, y, z, rhw, u, v;
    };
    float w = (float)width - 0.5f, h = (float)height - 0.5f;
    const Vtx quad[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w, -0.5f, 0, 1, 1, 0}, {-0.5f, h, 0, 1, 0, 1}, {w, h, 0, 1, 1, 1}};
    bool ok = false;
    if (SUCCEEDED(device->BeginScene()))
    {
        ok = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx)));
        device->EndScene();
    }
    return ok;
}

} // namespace subtitles
} // namespace ivr
