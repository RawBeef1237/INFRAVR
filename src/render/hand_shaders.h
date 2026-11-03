// INFRA VR - HLSL for the hand and camera models (compiled at run time with d3dcompiler, see
// hands_renderer.cpp; tests/shader_test.cpp compiles them too).
//
// Pixel shader constants: c0-c5 ambient cube (+x -x +y -y +z -z), c6-c9 light colours, c10-c13
// light positions (w = range), c14-c17 light directions (w = spot exponent), c18-c21 attenuation
// (w = 1 directional), c22-c25 spot cosines (w = 1 spot), c26 eye (w = use normal map),
// c27 x = tangent handedness factor, y = output gamma, z = normal map green sign.
#pragma once

namespace ivr {
namespace hands {

inline const char kVertexShader[] = R"(
row_major float4x4 g_viewProj : register(c0);
struct VIn { float3 pos : POSITION; float3 nrm : NORMAL; float4 tan : TANGENT; float2 uv : TEXCOORD0; };
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; float3 nrm : TEXCOORD2; float4 tan : TEXCOORD3; };
VOut main(VIn i)
{
    VOut o;
    o.pos = mul(float4(i.pos, 1.0), g_viewProj);
    o.uv = i.uv;
    o.wpos = i.pos;
    o.nrm = i.nrm;
    o.tan = i.tan;
    return o;
}
)";

// Source's model lighting: ambient cube plus up to four point, spot or directional lights with
// the engine's attenuation, Lambert diffuse, times the HDR exposure; sRGB texture in, gamma out.
inline const char kPixelShader[] = R"(
sampler2D g_base : register(s0);
sampler2D g_normal : register(s1);
float4 g_amb[6] : register(c0);
float4 g_lcolor[4] : register(c6);
float4 g_lpos[4] : register(c10);
float4 g_ldir[4] : register(c14);
float4 g_latt[4] : register(c18);
float4 g_lspot[4] : register(c22);
float4 g_eye : register(c26);
float4 g_params : register(c27);
struct PIn { float2 uv : TEXCOORD0; float3 wpos : TEXCOORD1; float3 nrm : TEXCOORD2; float4 tan : TEXCOORD3; };
float4 main(PIn i) : COLOR
{
    float3 N = normalize(i.nrm);
    float3 V = g_eye.xyz - i.wpos;
    float flip = dot(N, V) < 0.0 ? -1.0 : 1.0; // double-sided: light the side we see
    N *= flip;
    if (g_eye.w > 0.5)
    {
        float3 T = normalize(i.tan.xyz - N * dot(N, i.tan.xyz));
        float3 B = cross(N, T) * (i.tan.w * g_params.x * flip);
        float3 t = tex2D(g_normal, i.uv).xyz * 2.0 - 1.0;
        t.y *= g_params.z;
        N = normalize(T * t.x + B * t.y + N * t.z);
    }
    float3 n2 = N * N;
    float3 light = n2.x * (N.x >= 0.0 ? g_amb[0].rgb : g_amb[1].rgb) +
                   n2.y * (N.y >= 0.0 ? g_amb[2].rgb : g_amb[3].rgb) +
                   n2.z * (N.z >= 0.0 ? g_amb[4].rgb : g_amb[5].rgb);
    for (int k = 0; k < 4; ++k)
    {
        float3 d = g_lpos[k].xyz - i.wpos;
        float dist = max(length(d), 0.001);
        float3 L = lerp(d / dist, -g_ldir[k].xyz, g_latt[k].w);
        float att = 1.0 / max(dot(g_latt[k].xyz, float3(1.0, dist, dist * dist)), 0.0001);
        att = lerp(att, 1.0, g_latt[k].w);
        float inRange = (g_lpos[k].w > 0.0 && dist > g_lpos[k].w) ? 0.0 : 1.0;
        float spot = pow(saturate((dot(g_ldir[k].xyz, -L) - g_lspot[k].x) * g_lspot[k].y), max(g_ldir[k].w, 0.001));
        att *= lerp(1.0, spot, g_lspot[k].w) * inRange;
        light += g_lcolor[k].rgb * (att * saturate(dot(N, L)));
    }
    float4 base = tex2D(g_base, i.uv);
    float3 c = base.rgb * light;
    return float4(pow(saturate(c), g_params.y), 1.0);
}
)";

} // namespace hands
} // namespace ivr
