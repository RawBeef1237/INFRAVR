#include "hands_renderer.h"
#include "config.h"
#include "d3d_vr.h"
#include "flags.h"
#include "game.h"
#include "glb_model.h"
#include "mod.h"

#include <algorithm>
#include <functional>
#include <cmath>
#include <memory>
#include <vector>

namespace ivr {

void Defer(std::function<void()> fn); // hooks_game.cpp

namespace hands {

namespace {

const char *kFlag = "glb_hands";
constexpr int kTrustFrames = 600;

struct Vertex
{
    float x, y, z;
    D3DCOLOR color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Frame
{
    std::vector<Vertex> verts[2];
    bool visible[2] = {false, false};
};

GlbModel g_model[2];
int g_state = 0; // 0 not tried, 1 loaded, -1 failed / off
float g_curl[2][5]{};
double g_lastCurlAt = 0;
int g_framesDrawn = 0;
bool g_loggedDepth = false;

// Render thread only.
IDirect3DTexture9 *g_tex[2] = {nullptr, nullptr};
IDirect3DStateBlock9 *g_state9 = nullptr;
bool g_texFailed = false;

bool TryLoad()
{
    if (g_state)
        return g_state > 0;
    g_state = -1;
    if (g_Config.handsModel != "glb")
        return false;
    if (!flags::Begin(kFlag))
    {
        IVR_LOG("GLB hands are skipped (infravr_skip.txt) - INFRA's own arms are used");
        return false;
    }
    const std::string files[2] = {g_Config.handsLeftFile, g_Config.handsRightFile};
    for (int h = 0; h < 2; ++h)
    {
        std::string path = ModuleDir() + "infravr\\" + files[h], err;
        if (!g_model[h].Load(path, &err))
        {
            IVR_ERR("GLB hands: %s - INFRA's own arms are used instead", err.c_str());
            flags::End(kFlag);
            return false;
        }
        const GlbModel &m = g_model[h];
        IVR_LOG("GLB hands: %s: %u vertices, %u triangles, %u joints (%d finger joints), texture %dx%d, "
                "size %.3f x %.3f x %.3f m",
                files[h].c_str(), (unsigned)m.vertices.size(), (unsigned)m.indices.size() / 3,
                (unsigned)m.joints.size(), m.fingerJoints, m.texWidth, m.texHeight, m.boundsMax[0] - m.boundsMin[0],
                m.boundsMax[1] - m.boundsMin[1], m.boundsMax[2] - m.boundsMin[2]);
    }
    g_state = 1;
    return true;
}

// Finger curls for this frame: SteamVR/OpenXR finger data, or grip and trigger buttons.
void UpdateCurls()
{
    double now = NowSeconds();
    float dt = g_lastCurlAt > 0 ? (float)std::min(0.1, now - g_lastCurlAt) : 0.0f;
    g_lastCurlAt = now;
    const InputState &in = g_VR.Input();
    for (int h = 0; h < 2; ++h)
    {
        float target[5];
        if (!g_Config.handsFingers)
            std::fill(target, target + 5, 0.15f);
        else if (g_VR.skeletonValid[h])
            std::copy(g_VR.fingerCurl[h], g_VR.fingerCurl[h] + 5, target);
        else
        {
            bool grip = h == HAND_LEFT ? in.gripLeft.state : in.gripRight.state;
            bool trig = h == HAND_LEFT ? in.useLeft.state : in.useRight.state;
            target[0] = grip ? 0.7f : 0.2f;
            target[1] = trig ? 0.9f : 0.15f;
            target[2] = target[3] = target[4] = grip ? 1.0f : 0.2f;
        }
        // Finger data is smooth already; buttons jump, so ease towards them (about 60 ms).
        float k = g_VR.skeletonValid[h] ? 1.0f : std::min(1.0f, dt * 16.0f);
        for (int f = 0; f < 5; ++f)
            g_curl[h][f] += (target[f] - g_curl[h][f]) * k;
    }
}

// The model in the controller's frame (glTF/OpenVR axes, metres) -> game world.
Pose ModelToWorld(int hand)
{
    Pose c = g_Mod.HandWorld(hand);
    float s = g_Mod.UnitsPerMetre();
    Vector off = g_Config.handsOffset; // right, up, back in the controller frame
    QAngle ang(g_Config.handsAngles.x, g_Config.handsAngles.y, g_Config.handsAngles.z);
    if (hand == HAND_LEFT)
    {
        off.x = -off.x;
        ang.y = -ang.y;
        ang.z = -ang.z;
    }
    // OpenVR (x right, y up, z back) -> Source (x forward, y left, z up).
    Mat3 C;
    C.m[0][0] = 0, C.m[0][1] = 0, C.m[0][2] = -1;
    C.m[1][0] = -1, C.m[1][1] = 0, C.m[1][2] = 0;
    C.m[2][0] = 0, C.m[2][1] = 1, C.m[2][2] = 0;
    Pose local;
    local.rot = AnglesToMatrix(ang) * C;
    local.pos = Vector(-off.z, -off.x, off.y) * s;
    Pose p = c * local;
    float k = s * g_Config.handsScale;
    for (auto &row : p.rot.m)
        for (float &e : row)
            e *= k;
    return p;
}

std::shared_ptr<Frame> BuildFrame(const CViewSetup &v)
{
    auto fr = std::make_shared<Frame>();
    UpdateCurls();
    // Head-lamp style light: from the viewer, a little from above.
    Mat3 eye = AnglesToMatrix(v.angles);
    Vector L = (eye.Up() * 0.6f - eye.Forward()).Normalized();
    float bright = g_Config.handsBrightness;
    std::vector<float> pos, nrm;
    for (int h = 0; h < 2; ++h)
    {
        if (!g_Mod.HandValid(h))
            continue;
        const GlbModel &m = g_model[h];
        m.Skin(g_curl[h], g_Config.handsCurlMax, pos, nrm);
        Pose w = ModelToWorld(h);
        Mat3 nrot = w.rot; // uniform scale: normals only need renormalising
        std::vector<Vertex> &out = fr->verts[h];
        out.resize(m.indices.size());
        std::vector<Vertex> unique(m.vertices.size());
        for (size_t i = 0; i < m.vertices.size(); ++i)
        {
            Vector p = w.Transform(Vector(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]));
            Vector n = (nrot * Vector(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2])).Normalized();
            float d = std::max(0.0f, Dot(n, L));
            float lum = std::min(1.0f, bright * (0.42f + 0.58f * d));
            auto c8 = [&](float c) { return (DWORD)std::lround(std::min(1.0f, std::max(0.0f, c * lum)) * 255.0f); };
            unique[i] = {p.x, p.y, p.z, D3DCOLOR_ARGB(255, c8(m.baseColor[0]), c8(m.baseColor[1]), c8(m.baseColor[2])),
                         m.vertices[i].uv[0], m.vertices[i].uv[1]};
        }
        for (size_t i = 0; i < m.indices.size(); ++i)
            out[i] = unique[m.indices[i]];
        fr->visible[h] = true;
    }
    return fr;
}

bool EnsureTexture(IDirect3DDevice9 *dev, int h)
{
    if (g_tex[h] || g_texFailed)
        return g_tex[h] != nullptr;
    const GlbModel &m = g_model[h];
    if (m.texRGBA.empty())
        return false;
    if (FAILED(dev->CreateTexture(m.texWidth, m.texHeight, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &g_tex[h], nullptr)) ||
        !g_tex[h])
    {
        IVR_ERR("GLB hands: could not create the %dx%d hand texture", m.texWidth, m.texHeight);
        g_texFailed = true;
        return false;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_tex[h]->LockRect(0, &lr, nullptr, 0)))
    {
        for (int y = 0; y < m.texHeight; ++y)
        {
            const uint8_t *src = &m.texRGBA[(size_t)y * m.texWidth * 4];
            uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + (size_t)y * lr.Pitch;
            for (int x = 0; x < m.texWidth; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
        g_tex[h]->UnlockRect(0);
    }
    return true;
}

// D3D matrices (row vectors) for a Source view.
void ViewProjection(const CViewSetup &v, D3DMATRIX &view, D3DMATRIX &proj)
{
    Mat3 r = AnglesToMatrix(v.angles);
    Vector f = r.Forward(), l = r.Left(), u = r.Up(), o = v.origin;
    view = {};
    // view space: x = right (-left), y = up, z = forward
    view._11 = -l.x, view._12 = u.x, view._13 = f.x;
    view._21 = -l.y, view._22 = u.y, view._23 = f.y;
    view._31 = -l.z, view._32 = u.z, view._33 = f.z;
    view._41 = Dot(l, o), view._42 = -Dot(u, o), view._43 = -Dot(f, o), view._44 = 1.0f;
    float tanX = std::tan(Deg2Rad(v.fov * 0.5f));
    float tanY = tanX / std::max(0.1f, v.m_flAspectRatio);
    float zn = std::max(0.1f, v.zNear), zf = std::max(zn + 1.0f, v.zFar);
    float q = zf / (zf - zn);
    proj = {};
    proj._11 = 1.0f / tanX;
    proj._22 = 1.0f / tanY;
    proj._33 = q;
    proj._34 = 1.0f;
    proj._43 = -q * zn;
}

void Draw(const std::shared_ptr<Frame> &fr, const CViewSetup &v)
{
    IDirect3DDevice9 *dev = g_D3D.device;
    if (!dev || (!fr->visible[0] && !fr->visible[1]))
        return;
    if (!g_state9 && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state9)))
        g_state9 = nullptr;
    if (g_state9)
        g_state9->Capture();

    // Depth test against the scene only when the bound depth buffer matches the eye image.
    bool depth = false;
    IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
    if (g_Config.handsDepthTest && SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt &&
        SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds)
    {
        D3DSURFACE_DESC a{}, b{};
        rt->GetDesc(&a);
        ds->GetDesc(&b);
        depth = a.Width <= b.Width && a.Height <= b.Height && a.MultiSampleType == b.MultiSampleType;
        if (!g_loggedDepth)
        {
            g_loggedDepth = true;
            IVR_LOG("GLB hands: eye target %ux%u (%d samples), depth buffer %ux%u (%d samples) - depth test %s",
                    a.Width, a.Height, (int)a.MultiSampleType, b.Width, b.Height, (int)b.MultiSampleType,
                    depth ? "on" : "off");
        }
    }
    if (rt)
        rt->Release();
    if (ds)
        ds->Release();

    D3DVIEWPORT9 vp = {0, 0, (DWORD)v.width, (DWORD)v.height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    D3DMATRIX view, proj, ident = {};
    ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
    ViewProjection(v, view, proj);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetTransform(D3DTS_VIEW, &view);
    dev->SetTransform(D3DTS_PROJECTION, &proj);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); // the right hand is a mirrored export
    dev->SetRenderState(D3DRS_ZENABLE, depth ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, depth ? TRUE : FALSE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (int h = 0; h < 2; ++h)
    {
        if (!fr->visible[h] || fr->verts[h].empty())
            continue;
        bool tex = EnsureTexture(dev, h);
        dev->SetTexture(0, tex ? g_tex[h] : nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, tex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)fr->verts[h].size() / 3, fr->verts[h].data(), sizeof(Vertex));
    }
    dev->SetTexture(0, nullptr);
    if (g_state9)
        g_state9->Apply();
}

} // namespace

bool Enabled() { return TryLoad(); }

void DrawEye(int eye, const CViewSetup &v)
{
    if (!TryLoad())
        return;
    static std::shared_ptr<Frame> s_frame;
    if (eye == 0 || !s_frame)
        s_frame = BuildFrame(v); // both eyes show the same pose
    std::shared_ptr<Frame> fr = s_frame;
    CViewSetup view = v;
    Defer([fr, view] { Draw(fr, view); });
    if (eye == 1 && ++g_framesDrawn == kTrustFrames)
    {
        flags::End(kFlag);
        IVR_LOG("GLB hands: %d frames drawn without trouble", g_framesDrawn);
    }
}

void OnDeviceReset()
{
    for (auto &t : g_tex)
    {
        if (t)
            t->Release();
        t = nullptr;
    }
    if (g_state9)
        g_state9->Release();
    g_state9 = nullptr;
}

} // namespace hands
} // namespace ivr
