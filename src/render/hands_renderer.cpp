#include "hands_renderer.h"
#include "config.h"
#include "d3d_vr.h"
#include "flags.h"
#include "game.h"
#include "glb_model.h"
#include "hand_shaders.h"
#include "mod.h"

#include <d3dcommon.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ivr {

void Defer(std::function<void()> fn); // hooks_game.cpp

namespace hands {

namespace {

const char *kFlag = "glb_hands";
const char *kLightFlag = "world_light";
const char *kCameraFlag = "camera_model";
const char *kFlashlightFlag = "flashlight_model";
const char *kPhoneFlag = "phone_model";
constexpr int kTrustFrames = 600;
constexpr int kItemTrustFrames = 90; // an item model's first frames: where a bad model would crash
constexpr float kGamma = 1.0f / 2.2f;

// --- vertex formats ----------------------------------------------------------------------------
// Shader path: world-space position, normal, tangent (w = handedness), texture coordinates.
struct SVertex
{
    float pos[3];
    float nrm[3];
    float tan[4];
    float uv[2];
};
// Fixed-function path (and the camera screen): lit colour per vertex.
struct FVertex
{
    float x, y, z;
    D3DCOLOR color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// --- pixel shader constants (see kPixelShader) ---------------------------------------------------
enum : int
{
    PS_AMBIENT = 0,   // 6: +x -x +y -y +z -z
    PS_LCOLOR = 6,    // 4: rgb (exposure applied)
    PS_LPOS = 10,     // 4: xyz, w = range (0 = none)
    PS_LDIR = 14,     // 4: direction the light travels, w = spot falloff exponent
    PS_LATT = 18,     // 4: constant, linear, quadratic attenuation, w = 1 for a directional light
    PS_LSPOT = 22,    // 4: cos outer, 1 / (cos inner - cos outer), unused, w = 1 for a spot
    PS_EYE = 26,      // eye position, w = use the normal map
    PS_PARAMS = 27,   // x = tangent handedness factor (-1 mirrored), y = output gamma, z = normal map green sign
    PS_COUNT = 28
};

// --- shader compiler (d3dcompiler, loaded at run time) -----------------------------------------
// ID3DBlob from the SDK header. (A blob interface declared here, inside the anonymous namespace,
// had no implementation the compiler could see, so GCC turned its calls into pure-virtual calls:
// the first time D3DCompile returned a message - a warning is enough - INFRA crashed.)
using IBlob = ID3D10Blob;
using D3DCompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                       IBlob **, IBlob **);

std::vector<DWORD> g_vsCode, g_psCode; // compiled at load time, empty = fixed-function lighting

bool Compile(D3DCompileFn compile, const char *src, const char *target, std::vector<DWORD> &out)
{
    IBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(src, std::strlen(src), "infravr_hands", nullptr, nullptr, "main", target,
                         1u << 15 /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &errors);
    if (errors)
    {
        std::string msg(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
        if (FAILED(hr))
            IVR_ERR("Hand shader (%s) did not compile: %s", target, msg.c_str());
        errors->Release();
    }
    if (FAILED(hr) || !code)
        return false;
    out.resize((code->GetBufferSize() + 3) / 4);
    std::memcpy(out.data(), code->GetBufferPointer(), code->GetBufferSize());
    code->Release();
    return true;
}

void CompileShaders()
{
    if (g_Config.handsLighting != "world")
        return;
    // Windows' own compiler first, then the one INFRA ships in its bin folder.
    const std::string names[] = {"d3dcompiler_47.dll", "d3dcompiler_43.dll", ModuleDir() + "d3dcompiler_41.dll"};
    for (const std::string &n : names)
    {
        HMODULE m = LoadLibraryA(n.c_str());
        if (!m)
            continue;
        auto compile = reinterpret_cast<D3DCompileFn>(reinterpret_cast<void *>(GetProcAddress(m, "D3DCompile")));
        if (compile && Compile(compile, kVertexShader, "vs_3_0", g_vsCode) &&
            Compile(compile, kPixelShader, "ps_3_0", g_psCode))
        {
            IVR_LOG("Hand shaders compiled with %s (%u + %u bytes)", n.c_str(), (unsigned)g_vsCode.size() * 4,
                    (unsigned)g_psCode.size() * 4);
            return;
        }
        g_vsCode.clear();
        g_psCode.clear();
    }
    IVR_ERR("No shader compiler for the hands - they are lit per vertex instead (no normal maps)");
}

// --- world lighting ------------------------------------------------------------------------------
struct Lighting
{
    Vector ambient[6];
    int count = 0;
    LightDesc_t lights[4];
    float scale = 1.0f; // exposure x brightness
};

int g_lightState = 0; // 0 not tried, 1 on, -1 off
int g_lightFrames = 0;

// The engine's light at `origin` (main thread). Falls back to a light from the viewer.
Lighting LightAt(const Vector &origin, const Mat3 &eye)
{
    Lighting l;
    if (g_Config.handsLighting == "world" && g_lightState == 0)
    {
        g_lightState = (g_Game.modelRender && flags::Begin(kLightFlag)) ? 1 : -1;
        if (g_lightState < 0)
            IVR_LOG("Hand lighting: the engine's model lighting is not available - simple lighting is used");
    }
    MaterialLightingState_t s;
    if (g_lightState > 0 && g_Game.LightingAt(origin, s))
    {
        Vector tone = g_Game.ToneMapScale();
        l.scale = tone.x * g_Config.handsBrightness;
        for (int i = 0; i < 6; ++i)
            l.ambient[i] = s.ambientCube[i];
        l.count = s.localLightCount;
        for (int i = 0; i < l.count; ++i)
            l.lights[i] = s.localLights[i];
        if (++g_lightFrames == kTrustFrames)
        {
            flags::End(kLightFlag);
            IVR_LOG("Hand lighting: %d frames lit by the engine without trouble", g_lightFrames);
        }
        static double s_logAt = 0;
        if (NowSeconds() >= s_logAt)
        {
            s_logAt = NowSeconds() + 30.0;
            IVR_LOG("Hand lighting at (%.0f %.0f %.0f): ambient up (%.2f %.2f %.2f) down (%.2f %.2f %.2f), %d local "
                    "light(s), exposure %.2f",
                    origin.x, origin.y, origin.z, s.ambientCube[4].x, s.ambientCube[4].y, s.ambientCube[4].z,
                    s.ambientCube[5].x, s.ambientCube[5].y, s.ambientCube[5].z, l.count, tone.x);
        }
        return l;
    }
    // Simple: an even grey light from the viewer, a little from above (as before world lighting).
    Vector L = (eye.Up() * 0.6f - eye.Forward()).Normalized();
    for (Vector &a : l.ambient)
        a = Vector(0.18f, 0.18f, 0.18f);
    l.count = 1;
    LightDesc_t &d = l.lights[0];
    d = LightDesc_t{};
    d.type = 2;
    d.color = Vector(0.32f, 0.32f, 0.32f);
    d.direction = -L;
    d.attenuation0 = 1.0f;
    l.scale = g_Config.handsBrightness;
    return l;
}

// Same light as the pixel shader, for one point and normal (fixed-function path).
Vector Shade(const Lighting &l, const Vector &p, const Vector &n)
{
    float x2 = n.x * n.x, y2 = n.y * n.y, z2 = n.z * n.z;
    Vector c = l.ambient[n.x >= 0 ? 0 : 1] * x2 + l.ambient[n.y >= 0 ? 2 : 3] * y2 + l.ambient[n.z >= 0 ? 4 : 5] * z2;
    for (int i = 0; i < l.count; ++i)
    {
        const LightDesc_t &d = l.lights[i];
        Vector L;
        float att = 1.0f;
        if (d.type == 2)
            L = -d.direction;
        else
        {
            Vector v = d.position - p;
            float dist = std::max(v.Length(), 0.001f);
            L = v * (1.0f / dist);
            att = 1.0f / std::max(d.attenuation0 + d.attenuation1 * dist + d.attenuation2 * dist * dist, 0.0001f);
            if (d.range > 0.0f && dist > d.range)
                att = 0.0f;
            if (d.type == 3)
            {
                float k = d.thetaDot > d.phiDot ? 1.0f / (d.thetaDot - d.phiDot) : 0.0f;
                float s = std::min(1.0f, std::max(0.0f, (Dot(d.direction, -L) - d.phiDot) * k));
                att *= std::pow(s, std::max(d.falloff, 0.001f));
            }
        }
        c = c + d.color * (att * std::max(0.0f, Dot(n, L)));
    }
    return c * l.scale;
}

void PackLighting(const Lighting &l, float out[PS_COUNT][4])
{
    std::memset(out, 0, sizeof(float) * PS_COUNT * 4);
    for (int i = 0; i < 6; ++i)
    {
        Vector a = l.ambient[i] * l.scale;
        out[PS_AMBIENT + i][0] = a.x, out[PS_AMBIENT + i][1] = a.y, out[PS_AMBIENT + i][2] = a.z;
    }
    for (int i = 0; i < 4; ++i)
    {
        out[PS_LATT + i][0] = 1.0f; // unused lights: black, attenuation 1
        if (i >= l.count || l.lights[i].type <= 0 || l.lights[i].type > 3)
            continue;
        const LightDesc_t &d = l.lights[i];
        Vector c = d.color * l.scale;
        out[PS_LCOLOR + i][0] = c.x, out[PS_LCOLOR + i][1] = c.y, out[PS_LCOLOR + i][2] = c.z;
        out[PS_LPOS + i][0] = d.position.x, out[PS_LPOS + i][1] = d.position.y, out[PS_LPOS + i][2] = d.position.z;
        out[PS_LPOS + i][3] = d.range;
        out[PS_LDIR + i][0] = d.direction.x, out[PS_LDIR + i][1] = d.direction.y, out[PS_LDIR + i][2] = d.direction.z;
        out[PS_LDIR + i][3] = d.falloff;
        if (d.type == 2)
            out[PS_LATT + i][3] = 1.0f;
        else
        {
            out[PS_LATT + i][0] = d.attenuation0, out[PS_LATT + i][1] = d.attenuation1;
            out[PS_LATT + i][2] = d.attenuation2;
            if (d.attenuation0 + d.attenuation1 + d.attenuation2 <= 0.0f)
                out[PS_LATT + i][0] = 1.0f;
        }
        if (d.type == 3)
        {
            out[PS_LSPOT + i][0] = d.phiDot;
            out[PS_LSPOT + i][1] = d.thetaDot > d.phiDot ? 1.0f / (d.thetaDot - d.phiDot) : 1000.0f;
            out[PS_LSPOT + i][3] = 1.0f;
        }
    }
    out[PS_PARAMS][0] = 1.0f;
    out[PS_PARAMS][1] = kGamma;
    out[PS_PARAMS][2] = 1.0f;
}

// --- models --------------------------------------------------------------------------------------
enum : int
{
    MODEL_LEFT = 0,
    MODEL_RIGHT = 1,
    MODEL_CAMERA = 2,
    MODEL_FLASHLIGHT = 3,
    MODEL_PHONE = 4,
    MODEL_COUNT = 5
};

struct Batch
{
    int material = -1;
    std::vector<uint16_t> indices;
};

struct ModelData
{
    GlbModel glb;
    std::vector<Batch> batches; // per material, without the camera screen
    // Camera only: the screen triangles (their own vertices) with texture coordinates across the
    // screen (0..1, top left to bottom right), and the lens in the joint carrying the camera.
    std::vector<uint16_t> screenIndices;
    std::vector<float> screenST; // 2 per vertex of the model (only screen vertices are used)
    float screenAspect = 4.0f / 3.0f;
    int cameraJoint = -1; // camera: the joint carrying the camera; flashlight: the flashlight's
    Pose lensLocal;       // in cameraJoint's space (game units): camera lens / flashlight beam
    bool haveLens = false;
    // Phone only: its screen in screenJoint's space (x right, y up, z out of the glass; game
    // units), and the image shown instead of the phone's texture during INFRA's call screen.
    int screenJoint = -1;
    Pose screenLocal;
    float screenW = 0, screenH = 0;
    int itemMaterial = -1; // the item's own material (not the hand's)
    int altImage = -1;     // index into glb.images
    int indexTip = -1;     // VR hands: the vertex at the end of the index finger
};

// The vertex at the tip of the index finger: of the vertices skinned mostly to an index finger
// joint, the one furthest along the fingers (-Z in the model's frame).
int FindIndexTip(const GlbModel &m)
{
    int best = -1;
    for (size_t i = 0; i < m.vertices.size(); ++i)
    {
        const GlbVertex &v = m.vertices[i];
        int k = 0;
        for (int j = 1; j < 4; ++j)
            if (v.weights[j] > v.weights[k])
                k = j;
        int joint = v.joints[k];
        if (joint >= (int)m.joints.size())
            continue;
        int node = m.joints[joint];
        if (node < 0 || node >= (int)m.nodes.size() || m.nodes[node].finger != 1)
            continue;
        if (best < 0 || v.pos[2] < m.vertices[best].pos[2])
            best = (int)i;
    }
    return best;
}

ModelData g_model[MODEL_COUNT];
int g_state = 0;       // 0 not tried, 1 loaded, -1 failed / off
float g_curl[2][5]{};
double g_lastCurlAt = 0;
int g_framesDrawn = 0;
bool g_loggedDepth = false;
std::atomic<int> g_eyeFrame{0};

// Held-item models (camera, flashlight): a GLB of the item with the hand holding it, rigged to
// the bones of INFRA's viewmodel for that item.
struct ItemModel
{
    const char *what;      // for the log
    const char *flag;      // crash-sticky flag
    const char *handJoint; // the joint that goes on the VR hand
    int side;              // the hand the model is made for (its hand matches that VR hand model)
    int model;             // g_model index
    int state = 0;         // 0 not tried, 1 loaded, -1 off
    std::vector<std::string> jointNames;
    std::vector<GlbMat34> bones; // INFRA's bones, world, game units (under g_boneLock)
    int bonesFrame = -100;
    double shownAt = 0;
    int frames = 0;
    bool onTrial = false; // its crash-sticky flag is set
    bool trialDone = false; // tried once this session (put away fine): not on trial again
    int panelFrame = -100; // the last frame INFRA's screen panel was put on the model (main thread)
};
ItemModel g_items[ITEM_COUNT] = {
    {"Camera model", kCameraFlag, "Bip001 R Hand", HAND_RIGHT, MODEL_CAMERA},
    {"Flashlight model", kFlashlightFlag, "Bip001 L Hand", HAND_LEFT, MODEL_FLASHLIGHT},
    {"Phone model", kPhoneFlag, "Bip001 R Hand", HAND_RIGHT, MODEL_PHONE},
};
std::atomic<bool> g_phoneCall{false}; // INFRA shows its call screen on the phone
std::mutex g_boneLock;

void BuildBatches(ModelData &md, bool camera)
{
    const GlbModel &m = md.glb;
    md.batches.clear();
    md.screenIndices.clear();
    // The camera's own material: named "camera", else the last one.
    int camMat = -1;
    if (camera)
    {
        for (size_t i = 0; i < m.materials.size(); ++i)
        {
            std::string n = m.materials[i].name;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            if (n.find("camera") != std::string::npos)
                camMat = (int)i;
        }
        if (camMat < 0 && !m.primitives.empty())
            camMat = m.primitives.back().material;
    }
    const float *r = g_Config.cameraScreenUV;
    auto wrap = [](float x) { return x - std::floor(x); };
    for (const GlbPrimitive &p : m.primitives)
    {
        Batch b;
        b.material = p.material;
        for (uint32_t i = p.firstIndex; i + 2 < p.firstIndex + p.indexCount; i += 3)
        {
            const uint16_t *t = &m.indices[i];
            bool screen = false;
            if (camera && p.material == camMat)
            {
                float u = 0, v = 0;
                for (int k = 0; k < 3; ++k)
                {
                    u += wrap(m.vertices[t[k]].uv[0]) / 3.0f;
                    v += wrap(m.vertices[t[k]].uv[1]) / 3.0f;
                }
                screen = u >= r[0] && u <= r[2] && v >= r[1] && v <= r[3];
            }
            std::vector<uint16_t> &dst = screen ? md.screenIndices : b.indices;
            dst.insert(dst.end(), t, t + 3);
        }
        if (!b.indices.empty())
            md.batches.push_back(std::move(b));
    }
    if (!camera)
        return;

    // Screen: texture coordinates across it, and its shape (least squares P = A + s U + t V).
    md.screenST.assign(m.vertices.size() * 2, 0.0f);
    std::vector<float> pos, nrm;
    float none[5] = {0, 0, 0, 0, 0};
    m.Skin(none, 0.0f, pos, nrm);
    double M[3][3] = {}, R[3][3] = {};
    std::vector<char> seen(m.vertices.size(), 0);
    for (uint16_t i : md.screenIndices)
    {
        if (seen[i])
            continue;
        seen[i] = 1;
        float s = (wrap(m.vertices[i].uv[0]) - r[0]) / std::max(1e-6f, r[2] - r[0]);
        float t = (wrap(m.vertices[i].uv[1]) - r[1]) / std::max(1e-6f, r[3] - r[1]);
        md.screenST[i * 2] = s;
        md.screenST[i * 2 + 1] = t;
        double a[3] = {1.0, s, t};
        for (int x = 0; x < 3; ++x)
            for (int y = 0; y < 3; ++y)
            {
                M[x][y] += a[x] * a[y];
                R[x][y] += a[x] * pos[i * 3 + y];
            }
    }
    // Solve M * [A U V] = R (3x3, Cramer).
    auto det3 = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    double D = det3(M);
    if (std::fabs(D) > 1e-9)
    {
        double sol[3][3];
        for (int c = 0; c < 3; ++c) // unknown row (A, U, V)
            for (int k = 0; k < 3; ++k) // coordinate
            {
                double Mc[3][3];
                std::memcpy(Mc, M, sizeof(Mc));
                for (int x = 0; x < 3; ++x)
                    Mc[x][c] = R[x][k];
                sol[c][k] = det3(Mc) / D;
            }
        double w = std::sqrt(sol[1][0] * sol[1][0] + sol[1][1] * sol[1][1] + sol[1][2] * sol[1][2]);
        double h = std::sqrt(sol[2][0] * sol[2][0] + sol[2][1] * sol[2][1] + sol[2][2] * sol[2][2]);
        if (w > 0 && h > 0)
            md.screenAspect = (float)(w / h);
    }

    // Lens: the triangles inside the lens circle of the texture, in the joint that carries them.
    int joint = -1;
    double cx = 0, cy = 0, cz = 0, nx = 0, ny = 0, nz = 0;
    int n = 0;
    const float *lens = g_Config.cameraLensUV;
    std::fill(seen.begin(), seen.end(), 0);
    for (const GlbPrimitive &p : m.primitives)
    {
        if (p.material != camMat)
            continue;
        for (uint32_t i = p.firstIndex; i + 2 < p.firstIndex + p.indexCount; i += 3)
        {
            const uint16_t *t = &m.indices[i];
            float u = 0, v = 0;
            for (int k = 0; k < 3; ++k)
            {
                u += wrap(m.vertices[t[k]].uv[0]) / 3.0f;
                v += wrap(m.vertices[t[k]].uv[1]) / 3.0f;
            }
            if ((u - lens[0]) * (u - lens[0]) + (v - lens[1]) * (v - lens[1]) > lens[2] * lens[2])
                continue;
            for (int k = 0; k < 3; ++k)
            {
                if (seen[t[k]])
                    continue;
                seen[t[k]] = 1;
                const GlbVertex &gv = m.vertices[t[k]];
                if (joint < 0)
                    joint = gv.joints[0];
                cx += gv.pos[0], cy += gv.pos[1], cz += gv.pos[2];
                nx += gv.normal[0], ny += gv.normal[1], nz += gv.normal[2];
                ++n;
            }
        }
    }
    md.cameraJoint = joint;
    if (n > 0 && joint >= 0 && !md.screenIndices.empty())
    {
        // Mesh space -> the joint's space (game units, the same frame as INFRA's bone).
        GlbMat34 ib = m.InverseBind(joint);
        auto xf = [&](double x, double y, double z, bool point) {
            Vector r2;
            float in[3] = {(float)x, (float)y, (float)z};
            float *o = &r2.x;
            for (int row = 0; row < 3; ++row)
                o[row] = ib.m[row][0] * in[0] + ib.m[row][1] * in[1] + ib.m[row][2] * in[2] + (point ? ib.m[row][3] : 0);
            return r2;
        };
        Vector centre = xf(cx / n, cy / n, cz / n, true);
        Vector fwd = xf(nx, ny, nz, false).Normalized();
        // Up: the screen's top edge direction (towards smaller t).
        Vector top, bottom;
        int nt = 0, nb = 0;
        std::fill(seen.begin(), seen.end(), 0);
        for (uint16_t i : md.screenIndices)
        {
            if (seen[i])
                continue;
            seen[i] = 1;
            Vector p = xf(m.vertices[i].pos[0], m.vertices[i].pos[1], m.vertices[i].pos[2], true);
            if (md.screenST[i * 2 + 1] < 0.5f)
                top = top + p, ++nt;
            else
                bottom = bottom + p, ++nb;
        }
        Vector up = nt && nb ? (top * (1.0f / nt) - bottom * (1.0f / nb)) : Vector(0, 0, 1);
        up = (up - fwd * Dot(up, fwd)).Normalized();
        Vector left = Cross(up, fwd);
        md.lensLocal.pos = centre;
        md.lensLocal.rot.SetCol(0, fwd);
        md.lensLocal.rot.SetCol(1, left);
        md.lensLocal.rot.SetCol(2, up);
        md.haveLens = true;
    }
}

bool LoadModel(int id, const std::string &file)
{
    std::string path = ModDir() + "models\\" + file, err;
    ModelData &md = g_model[id];
    if (!md.glb.Load(path, &err))
    {
        IVR_ERR("GLB model: %s", err.c_str());
        return false;
    }
    BuildBatches(md, id == MODEL_CAMERA);
    const GlbModel &m = md.glb;
    if (id == MODEL_LEFT || id == MODEL_RIGHT)
    {
        md.indexTip = FindIndexTip(m);
        if (md.indexTip >= 0)
            IVR_LOG("GLB model %s: index fingertip at (%.3f %.3f %.3f)", file.c_str(),
                    m.vertices[md.indexTip].pos[0], m.vertices[md.indexTip].pos[1], m.vertices[md.indexTip].pos[2]);
    }
    IVR_LOG("GLB model %s: %u vertices, %u triangles, %u joints (%d finger joints), %u materials, %u images, "
            "size %.3f x %.3f x %.3f",
            file.c_str(), (unsigned)m.vertices.size(), (unsigned)m.indices.size() / 3, (unsigned)m.joints.size(),
            m.fingerJoints, (unsigned)m.materials.size(), (unsigned)m.images.size(), m.boundsMax[0] - m.boundsMin[0],
            m.boundsMax[1] - m.boundsMin[1], m.boundsMax[2] - m.boundsMin[2]);
    for (const GlbMaterial &mat : m.materials)
        IVR_LOG("  material %s: base texture %s, normal map %s", mat.name.c_str(), mat.baseImage >= 0 ? "yes" : "no",
                mat.normalImage >= 0 ? "yes" : "no");
    return true;
}

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
    if (!LoadModel(MODEL_LEFT, g_Config.handsLeftFile) || !LoadModel(MODEL_RIGHT, g_Config.handsRightFile))
    {
        IVR_ERR("GLB hands could not be loaded - INFRA's own arms are used instead");
        flags::End(kFlag);
        return false;
    }
    CompileShaders();
    g_state = 1;
    return true;
}

// The flashlight's beam: the item's own mesh (the last material) in the frame of the joint it is
// skinned to, its long axis, the wider end is the head and the lens is the part of the head
// facing out along the axis. x = beam direction, z = the joint's z made square to it.
void FindBeam(ModelData &md)
{
    const GlbModel &m = md.glb;
    if (m.primitives.size() < 2)
        return;
    const GlbPrimitive &prim = m.primitives.back();
    std::vector<int> verts;
    std::vector<int> jointUse(m.joints.size(), 0);
    for (uint32_t i = prim.firstIndex; i < prim.firstIndex + prim.indexCount; ++i)
        verts.push_back(m.indices[i]);
    std::sort(verts.begin(), verts.end());
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    for (int v : verts)
        for (int k = 0; k < 4; ++k)
            if (m.vertices[v].weights[k] > 0.5f && m.vertices[v].joints[k] < jointUse.size())
                ++jointUse[m.vertices[v].joints[k]];
    int joint = (int)(std::max_element(jointUse.begin(), jointUse.end()) - jointUse.begin());
    if (verts.size() < 8 || jointUse[joint] == 0)
        return;
    GlbMat34 ib = m.InverseBind(joint);
    auto xf = [&](const float *p, float w) {
        return Vector(ib.m[0][0] * p[0] + ib.m[0][1] * p[1] + ib.m[0][2] * p[2] + ib.m[0][3] * w,
                      ib.m[1][0] * p[0] + ib.m[1][1] * p[1] + ib.m[1][2] * p[2] + ib.m[1][3] * w,
                      ib.m[2][0] * p[0] + ib.m[2][1] * p[1] + ib.m[2][2] * p[2] + ib.m[2][3] * w);
    };
    std::vector<Vector> pos, nrm;
    Vector c(0, 0, 0);
    for (int v : verts)
    {
        pos.push_back(xf(m.vertices[v].pos, 1.0f));
        nrm.push_back(xf(m.vertices[v].normal, 0.0f).Normalized());
        c = c + pos.back();
    }
    c = c * (1.0f / pos.size());
    // Long axis: power iteration on the covariance.
    float cov[3][3] = {};
    for (const Vector &p : pos)
    {
        float d[3] = {p.x - c.x, p.y - c.y, p.z - c.z};
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                cov[a][b] += d[a] * d[b];
    }
    Vector ax(1, 1, 1);
    for (int it = 0; it < 64; ++it)
    {
        Vector n(cov[0][0] * ax.x + cov[0][1] * ax.y + cov[0][2] * ax.z,
                 cov[1][0] * ax.x + cov[1][1] * ax.y + cov[1][2] * ax.z,
                 cov[2][0] * ax.x + cov[2][1] * ax.y + cov[2][2] * ax.z);
        ax = n.Normalized();
    }
    float tmin = 1e9f, tmax = -1e9f;
    for (const Vector &p : pos)
    {
        float t = Dot(p - c, ax);
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    float slab = (tmax - tmin) * 0.05f;
    auto radius = [&](bool top) {
        float r = 0;
        for (const Vector &p : pos)
        {
            float t = Dot(p - c, ax);
            if (top ? t > tmax - slab : t < tmin + slab)
                r = std::max(r, (p - c - ax * t).Length());
        }
        return r;
    };
    if (radius(false) > radius(true))
        ax = ax * -1.0f; // the head (wider end) is along +ax
    tmax = -1e9f;
    for (const Vector &p : pos)
        tmax = std::max(tmax, Dot(p - c, ax));
    Vector centre(0, 0, 0), dir(0, 0, 0);
    int n = 0;
    for (size_t i = 0; i < pos.size(); ++i)
    {
        if (Dot(pos[i] - c, ax) > tmax - slab * 2.0f && Dot(nrm[i], ax) > 0.8f)
        {
            centre = centre + pos[i];
            dir = dir + nrm[i];
            ++n;
        }
    }
    if (n == 0)
    {
        centre = c + ax * tmax;
        dir = ax;
    }
    else
    {
        centre = centre * (1.0f / n);
        dir = dir.Normalized();
    }
    Vector up = Vector(0, 0, 1) - dir * dir.z;
    if (up.Length() < 0.1f)
        up = Vector(0, 1, 0) - dir * dir.y;
    up = up.Normalized();
    md.cameraJoint = joint;
    md.lensLocal.pos = centre + dir * 0.5f; // just in front of the glass
    md.lensLocal.rot.SetCol(0, dir);
    md.lensLocal.rot.SetCol(1, Cross(up, dir));
    md.lensLocal.rot.SetCol(2, up);
    md.haveLens = true;
}

// The phone's screen: the item material's triangles inside [phone] screen_uv, fitted as a plane
// P = A + s U + t V (s, t across the screen) in the frame of the joint they are skinned to.
void FindPhoneScreen(ModelData &md)
{
    const GlbModel &m = md.glb;
    // The item's material: not the hands' one.
    for (size_t i = 0; i < m.materials.size() && md.itemMaterial < 0; ++i)
    {
        std::string n = m.materials[i].name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find("hand") == std::string::npos)
            md.itemMaterial = (int)i;
    }
    const float *r = g_Config.phoneScreenUV;
    auto wrap = [](float x) { return x - std::floor(x); };
    std::vector<char> seen(m.vertices.size(), 0);
    std::vector<int> verts;
    for (const GlbPrimitive &p : m.primitives)
    {
        if (p.material != md.itemMaterial)
            continue;
        for (uint32_t i = p.firstIndex; i + 2 < p.firstIndex + p.indexCount; i += 3)
        {
            const uint16_t *t = &m.indices[i];
            bool in = true;
            for (int k = 0; k < 3; ++k)
            {
                float u = wrap(m.vertices[t[k]].uv[0]), v = wrap(m.vertices[t[k]].uv[1]);
                in = in && u >= r[0] && u <= r[2] && v >= r[1] && v <= r[3];
            }
            for (int k = 0; in && k < 3; ++k)
                if (!seen[t[k]])
                {
                    seen[t[k]] = 1;
                    verts.push_back(t[k]);
                }
        }
    }
    if (verts.size() < 3)
        return;
    int joint = m.vertices[verts[0]].joints[0];
    GlbMat34 ib = m.InverseBind(joint);
    auto xf = [&](const float *p, float w) {
        return Vector(ib.m[0][0] * p[0] + ib.m[0][1] * p[1] + ib.m[0][2] * p[2] + ib.m[0][3] * w,
                      ib.m[1][0] * p[0] + ib.m[1][1] * p[1] + ib.m[1][2] * p[2] + ib.m[1][3] * w,
                      ib.m[2][0] * p[0] + ib.m[2][1] * p[1] + ib.m[2][2] * p[2] + ib.m[2][3] * w);
    };
    double M[3][3] = {}, R[3][3] = {};
    Vector nsum(0, 0, 0);
    for (int i : verts)
    {
        const GlbVertex &gv = m.vertices[i];
        Vector p = xf(gv.pos, 1.0f);
        nsum = nsum + xf(gv.normal, 0.0f);
        double a[3] = {1.0, (wrap(gv.uv[0]) - r[0]) / std::max(1e-6f, r[2] - r[0]),
                       (wrap(gv.uv[1]) - r[1]) / std::max(1e-6f, r[3] - r[1])};
        double pv[3] = {p.x, p.y, p.z};
        for (int x = 0; x < 3; ++x)
            for (int y = 0; y < 3; ++y)
            {
                M[x][y] += a[x] * a[y];
                R[x][y] += a[x] * pv[y];
            }
    }
    auto det3 = [](const double q[3][3]) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
               q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    double D = det3(M);
    if (std::fabs(D) < 1e-12)
        return;
    double sol[3][3];
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k)
        {
            double Mc[3][3];
            std::memcpy(Mc, M, sizeof(Mc));
            for (int x = 0; x < 3; ++x)
                Mc[x][c] = R[x][k];
            sol[c][k] = det3(Mc) / D;
        }
    Vector A((float)sol[0][0], (float)sol[0][1], (float)sol[0][2]);
    Vector U((float)sol[1][0], (float)sol[1][1], (float)sol[1][2]); // left -> right
    Vector V((float)sol[2][0], (float)sol[2][1], (float)sol[2][2]); // top -> bottom
    md.screenW = U.Length();
    md.screenH = V.Length();
    if (md.screenW < 1e-4f || md.screenH < 1e-4f)
        return;
    Vector x = U * (1.0f / md.screenW);
    Vector y = (V * (-1.0f / md.screenH));
    y = (y - x * Dot(y, x)).Normalized();
    Vector z = Cross(x, y);
    if (Dot(z, nsum) < 0) // the texture is mirrored on the glass
    {
        x = x * -1.0f;
        z = Cross(x, y);
    }
    md.screenLocal.pos = A + U * 0.5f + V * 0.5f + z * 0.05f; // a hair in front of the glass
    md.screenLocal.rot.SetCol(0, x);
    md.screenLocal.rot.SetCol(1, y);
    md.screenLocal.rot.SetCol(2, z);
    md.screenJoint = joint;
}

bool TryLoadItem(int item)
{
    ItemModel &im = g_items[item];
    if (im.state)
        return im.state > 0;
    if (!TryLoad())
        return false;
    im.state = -1;
    const std::string &file = item == ITEM_CAMERA       ? g_Config.cameraModel
                              : item == ITEM_FLASHLIGHT ? g_Config.flashlightModel
                                                        : g_Config.phoneModel;
    if (file.empty())
        return false;
    if (!flags::Allowed(im.flag))
    {
        IVR_LOG("%s is skipped (infravr_skip.txt) - INFRA's own model is used", im.what);
        return false;
    }
    if (!LoadModel(im.model, file))
    {
        IVR_ERR("%s could not be loaded - INFRA's own model is used instead", im.what);
        return false;
    }
    const ModelData &md = g_model[im.model];
    im.jointNames.clear();
    for (int node : md.glb.joints)
        im.jointNames.push_back(md.glb.nodes[node].name);
    if (md.glb.JointByName(im.handJoint) < 0)
    {
        IVR_ERR("%s has no %s joint - INFRA's own model is used instead", im.what, im.handJoint);
        return false;
    }
    if (item == ITEM_CAMERA)
        IVR_LOG("Camera model: %u joints, screen %u triangles (aspect %.2f), lens %s", (unsigned)im.jointNames.size(),
                (unsigned)md.screenIndices.size() / 3, md.screenAspect,
                md.haveLens ? "found" : "not found (lens_offset is used)");
    else if (item == ITEM_PHONE)
    {
        ModelData &pm = g_model[im.model];
        FindPhoneScreen(pm);
        GlbImage call;
        std::string callFile = ModDir() + "models\\" + g_Config.phoneCallTexture;
        if (!g_Config.phoneCallTexture.empty() && pm.itemMaterial >= 0 && GlbModel::LoadImageFile(callFile, call))
        {
            pm.glb.images.push_back(std::move(call));
            pm.altImage = (int)pm.glb.images.size() - 1;
        }
        IVR_LOG("Phone model: %u joints, screen %s (%.1f x %.1f units), call texture %s", (unsigned)im.jointNames.size(),
                pm.screenJoint >= 0 ? "found" : "not found (the phone menu stays at the hand)", pm.screenW, pm.screenH,
                pm.altImage >= 0 ? "loaded" : "not loaded");
    }
    else
    {
        FindBeam(g_model[im.model]);
        const ModelData &fm = g_model[im.model];
        IVR_LOG("%s: %u joints, beam %s", im.what, (unsigned)im.jointNames.size(),
                fm.haveLens ? ("from the lens on joint " + im.jointNames[fm.cameraJoint]).c_str()
                            : "not found (flashlight_offset is used)");
    }
    im.state = 1;
    return true;
}

// The item is put away for longer than this: the next take-out starts with the draw animation.
constexpr int kItemGapFrames = 2;

int ItemHand(int item) { return item == ITEM_FLASHLIGHT ? g_Mod.flashlightHand : g_Mod.itemHand; }

// INFRA's phone stays still while its pause menu is up and may not set its bones up every frame.
bool ItemStale(int item, int bonesFrame)
{
    return g_eyeFrame.load() - bonesFrame > kItemGapFrames && !(item == ITEM_PHONE && g_Mod.phoneMenu);
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

// The model in the controller's frame (glTF/OpenVR axes, metres) -> game world. `side` is the hand
// the model is made for (the offset and angles are mirrored for a left hand); by default the
// controller's own.
Pose ModelToWorld(int hand, int side = -1)
{
    Pose c = g_Mod.HandWorld(hand);
    float s = g_Mod.UnitsPerMetre();
    Vector off = g_Config.handsOffset; // right, up, back in the controller frame
    QAngle ang(g_Config.handsAngles.x, g_Config.handsAngles.y, g_Config.handsAngles.z);
    if ((side < 0 ? hand : side) == HAND_LEFT)
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

// The hand an item model is drawn on, and the mirror that makes it the other hand when it is held
// in the hand it was not made for (reflection across the controller's forward/up plane).
struct CameraPlacement
{
    int hand = HAND_RIGHT;
    bool mirrored = false;
    Pose anchor;
};

CameraPlacement ItemPlace(int item)
{
    CameraPlacement c;
    c.hand = ItemHand(item);
    c.mirrored = c.hand != g_items[item].side;
    c.anchor = g_Mod.AimWorld(c.hand);
    return c;
}

Vector MirrorPoint(const CameraPlacement &c, const Vector &p)
{
    Vector l = c.anchor.rot.Transposed() * (p - c.anchor.pos);
    l.y = -l.y;
    return c.anchor.rot * l + c.anchor.pos;
}

Vector MirrorDir(const CameraPlacement &c, const Vector &d)
{
    Vector l = c.anchor.rot.Transposed() * d;
    l.y = -l.y;
    return c.anchor.rot * l;
}

// A rotation, a uniform scale and a position: x -> rot * (scale * x) + pos.
struct Sim
{
    Pose p;
    float scale = 1.0f;
    Sim operator*(const Sim &b) const { return {{p.rot * (b.p.pos * scale) + p.pos, p.rot * b.p.rot}, scale * b.scale}; }
    Sim Inverse() const
    {
        Mat3 rt = p.rot.Transposed();
        return {{rt * p.pos * (-1.0f / scale), rt}, 1.0f / scale};
    }
    static Sim From(const GlbMat34 &m)
    {
        Sim r;
        r.scale = Vector(m.m[0][0], m.m[1][0], m.m[2][0]).Length();
        float k = r.scale > 1e-9f ? 1.0f / r.scale : 1.0f;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                r.p.rot.m[a][b] = m.m[a][b] * k;
        r.p.pos = Vector(m.m[0][3], m.m[1][3], m.m[2][3]);
        return r;
    }
    static Sim From(const Pose &scaled)
    {
        GlbMat34 m{};
        for (int a = 0; a < 3; ++a)
        {
            for (int b = 0; b < 3; ++b)
                m.m[a][b] = scaled.rot.m[a][b];
        }
        m.m[0][3] = scaled.pos.x, m.m[1][3] = scaled.pos.y, m.m[2][3] = scaled.pos.z;
        return From(m);
    }
    GlbMat34 ToMat() const
    {
        GlbMat34 m{};
        for (int a = 0; a < 3; ++a)
        {
            for (int b = 0; b < 3; ++b)
                m.m[a][b] = p.rot.m[a][b] * scale;
        }
        m.m[0][3] = p.pos.x, m.m[1][3] = p.pos.y, m.m[2][3] = p.pos.z;
        return m;
    }
};

// Rest pose of a joint in its model (the inverse of its inverse bind matrix).
Sim RestJoint(const GlbModel &m, int joint) { return Sim::From(m.InverseBind(joint)).Inverse(); }

Mat3 Orthonormal(const Mat3 &r)
{
    Vector f = r.Col(0).Normalized();
    Vector u = r.Col(2);
    Vector l = Cross(u, f).Normalized();
    Mat3 o;
    o.SetCol(0, f);
    o.SetCol(1, l);
    o.SetCol(2, Cross(f, l));
    return o;
}

// INFRA's "draw" sequences (v_camera 16 frames at 30 fps, v_light 11 at 20 fps): while one plays the
// model is held in its rest pose, then eases into INFRA's animation.
constexpr double kDrawSkipSeconds = 0.55;
constexpr double kDrawBlendSeconds = 0.12;

// An item model's joints in the world. Its hand goes where the VR hand would be; the rest follow
// INFRA's animated bones relative to INFRA's hand, so the item moves in the hand (photo, battery)
// but the arm motion of INFRA's viewmodel is not used. Not mirrored here (see ItemPlace).
bool ItemBones(int item, std::vector<GlbMat34> &out)
{
    ItemModel &im = g_items[item];
    if (im.state <= 0)
        return false;
    std::vector<GlbMat34> raw;
    double shownAt;
    {
        std::lock_guard<std::mutex> lock(g_boneLock);
        if (im.bones.empty() || ItemStale(item, im.bonesFrame))
            return false;
        raw = im.bones;
        shownAt = im.shownAt;
    }
    const GlbModel &glb = g_model[im.model].glb;
    int hj = glb.JointByName(im.handJoint);
    int hand = ItemHand(item);
    if (hj < 0 || hj >= (int)raw.size() || raw.size() != glb.joints.size() || !g_Mod.HandValid(hand))
        return false;

    // Where the hand joint goes: the item model is made in the same space as the VR hand model of
    // its side (its hand at the same place, metres), so its own rest hand joint, placed on the
    // controller like that hand, puts it where the VR hand would be.
    Sim handRest = RestJoint(glb, hj);
    Sim anchor = Sim::From(ModelToWorld(hand, im.side)) * handRest;

    double t = NowSeconds() - shownAt;
    float anim = (float)std::min(1.0, std::max(0.0, (t - kDrawSkipSeconds) / kDrawBlendSeconds));
    Sim infraHandInv = Sim::From(raw[hj]).Inverse();
    Sim restHandInv = handRest.Inverse();
    out.resize(raw.size());
    for (size_t j = 0; j < raw.size(); ++j)
    {
        Sim rel = infraHandInv * Sim::From(raw[j]); // game units, rigid
        if (anim < 1.0f)
        {
            Sim rest = restHandInv * RestJoint(glb, (int)j);
            rel.p.pos = rest.p.pos + (rel.p.pos - rest.p.pos) * anim;
            Mat3 r;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    r.m[a][b] = rest.p.rot.m[a][b] + (rel.p.rot.m[a][b] - rest.p.rot.m[a][b]) * anim;
            rel.p.rot = Orthonormal(r);
        }
        rel.scale = 1.0f;
        out[j] = (anchor * rel).ToMat();
    }
    return true;
}

// --- per-frame data --------------------------------------------------------------------------------
struct DrawItem
{
    int model = 0;
    bool shader = false;
    bool mirrored = false;
    bool altTexture = false; // phone: INFRA's call screen is up
    bool panelOnScreen = false; // camera: INFRA's viewfinder panel sits on the screen, which stays dark
    std::vector<SVertex> sv;
    std::vector<FVertex> fv;
    float ps[PS_COUNT][4];
    std::vector<FVertex> screen; // camera screen triangles (world positions, s/t across the screen)
};

struct Frame
{
    std::vector<DrawItem> items;
};

void BuildItem(DrawItem &it, int model, const std::vector<float> &pos, const std::vector<float> &nrm,
               const std::vector<float> &tan, const Pose *xf, float xfScale, const CameraPlacement *mirror,
               const Lighting &light)
{
    const ModelData &md = g_model[model];
    const GlbModel &m = md.glb;
    it.model = model;
    it.shader = !g_vsCode.empty() && !g_psCode.empty();
    it.mirrored = mirror && mirror->mirrored;
    PackLighting(light, it.ps);
    it.ps[PS_PARAMS][0] = it.mirrored ? -1.0f : 1.0f;
    size_t n = m.vertices.size();
    auto place = [&](size_t i, Vector &p, Vector &nn, Vector &tt) {
        p = Vector(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
        nn = Vector(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]);
        tt = Vector(tan[i * 4], tan[i * 4 + 1], tan[i * 4 + 2]);
        if (xf)
        {
            p = xf->Transform(p);
            nn = (xf->rot * nn) * (1.0f / xfScale);
            tt = (xf->rot * tt) * (1.0f / xfScale);
        }
        if (it.mirrored)
        {
            p = MirrorPoint(*mirror, p);
            nn = MirrorDir(*mirror, nn);
            tt = MirrorDir(*mirror, tt);
        }
    };
    if (it.shader)
    {
        it.sv.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            Vector p, nn, tt;
            place(i, p, nn, tt);
            SVertex &v = it.sv[i];
            v = {{p.x, p.y, p.z}, {nn.x, nn.y, nn.z}, {tt.x, tt.y, tt.z, tan[i * 4 + 3]}, {m.vertices[i].uv[0], m.vertices[i].uv[1]}};
        }
    }
    else
    {
        it.fv.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            Vector p, nn, tt;
            place(i, p, nn, tt);
            nn = nn.Normalized();
            Vector c = Shade(light, p, nn);
            auto c8 = [&](float x) {
                return (DWORD)std::lround(std::pow(std::min(1.0f, std::max(0.0f, x)), kGamma) * 255.0f);
            };
            it.fv[i] = {p.x, p.y, p.z, D3DCOLOR_ARGB(255, c8(c.x), c8(c.y), c8(c.z)), m.vertices[i].uv[0],
                        m.vertices[i].uv[1]};
        }
    }
    if (!md.screenIndices.empty())
    {
        for (uint16_t i : md.screenIndices)
        {
            Vector p, nn, tt;
            place(i, p, nn, tt);
            it.screen.push_back({p.x, p.y, p.z, 0xFFFFFFFF, md.screenST[i * 2], md.screenST[i * 2 + 1]});
        }
    }
}

// Whether INFRA has put its call screen on the phone: the texture behind v_phone's material
// (checked a few times a second; every change is logged).
void UpdatePhoneCall()
{
    static double s_next = 0;
    static std::string s_last = "?";
    double now = NowSeconds();
    if (now < s_next)
        return;
    s_next = now + 0.25;
    std::string tex = g_Game.MaterialTexture("models/weapons/v_phone/mobile_phone_001", "$basetexture");
    if (tex != s_last)
    {
        IVR_LOG("Phone model: INFRA's phone texture is \"%s\"", tex.c_str());
        s_last = tex;
    }
    std::string lower = tex;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    g_phoneCall = lower.find("skin1") != std::string::npos;
}

std::shared_ptr<Frame> BuildFrame(const CViewSetup &v)
{
    auto fr = std::make_shared<Frame>();
    UpdateCurls();
    Mat3 eye = AnglesToMatrix(v.angles);
    std::vector<float> pos, nrm, tan;

    // An item model (camera, flashlight) replaces the VR hand that holds the item.
    bool handTaken[2] = {false, false};
    std::vector<GlbMat34> bones;
    for (int item = 0; item < ITEM_COUNT; ++item)
    {
        ItemModel &im = g_items[item];
        CameraPlacement place;
        bool drawn = TryLoadItem(item) && ItemBones(item, bones);
        if (drawn)
        {
            place = ItemPlace(item);
            drawn = !handTaken[place.hand];
        }
        if (!drawn)
        {
            // Put away without trouble: a short take-out must not leave the flag set (the next
            // launch would read it as a crash).
            if (im.onTrial)
            {
                flags::End(im.flag);
                im.onTrial = false;
                im.trialDone = true;
            }
            continue;
        }
        handTaken[place.hand] = true;
        // Only the first take-out of a session is on trial, and only briefly: INFRA closing while
        // the item is in your hand must not switch its model off for good.
        if (!im.onTrial && !im.trialDone && im.frames < kItemTrustFrames)
            im.onTrial = flags::Begin(im.flag);
        const ModelData &md = g_model[im.model];
        md.glb.SkinJoints(bones, pos, nrm, &tan);
        int hj = md.glb.JointByName(im.handJoint);
        const GlbMat34 &b = bones[hj >= 0 ? hj : 0];
        Vector origin(b.m[0][3], b.m[1][3], b.m[2][3]);
        if (place.mirrored)
            origin = MirrorPoint(place, origin);
        DrawItem it;
        BuildItem(it, im.model, pos, nrm, tan, nullptr, 1.0f, &place, LightAt(origin, eye));
        it.panelOnScreen = g_eyeFrame.load() - im.panelFrame <= kItemGapFrames;
        if (item == ITEM_PHONE)
        {
            UpdatePhoneCall();
            it.altTexture = g_phoneCall.load() && md.altImage >= 0;
        }
        fr->items.push_back(std::move(it));
        if (++im.frames == kItemTrustFrames)
        {
            flags::End(im.flag);
            im.onTrial = false;
            im.trialDone = true;
            IVR_LOG("%s: %d frames drawn without trouble", im.what, im.frames);
        }
    }

    const int heldHand = g_Mod.HeldObjectHand(); // holds a physics object: hidden, like Half-Life 2: VR
    for (int h = 0; h < 2; ++h)
    {
        if (!g_Mod.HandValid(h) || handTaken[h] || h == heldHand)
            continue;
        const ModelData &md = g_model[h];
        md.glb.Skin(g_curl[h], g_Config.handsCurlMax, pos, nrm, &tan);
        Pose w = ModelToWorld(h);
        float scale = g_Mod.UnitsPerMetre() * g_Config.handsScale;
        // Where the drawn index finger ends: what the pointer finger touches with.
        if (md.indexTip >= 0 && (size_t)md.indexTip * 3 + 2 < pos.size())
            g_Mod.SetFingerTip(h, w.Transform(Vector(pos[md.indexTip * 3], pos[md.indexTip * 3 + 1],
                                                       pos[md.indexTip * 3 + 2])));
        DrawItem it;
        BuildItem(it, h, pos, nrm, tan, &w, scale, nullptr, LightAt(w.pos, eye));
        fr->items.push_back(std::move(it));
    }
    return fr;
}

// --- render thread -----------------------------------------------------------------------------------
struct DeviceObjects
{
    std::vector<IDirect3DTexture9 *> tex[MODEL_COUNT]; // per model image
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    IDirect3DStateBlock9 *state = nullptr;
    bool shaderFailed = false;
    bool texFailed = false;
};
DeviceObjects g_dev;

IDirect3DTexture9 *Texture(IDirect3DDevice9 *dev, int model, int image)
{
    const GlbModel &m = g_model[model].glb;
    if (image < 0 || image >= (int)m.images.size() || g_dev.texFailed)
        return nullptr;
    std::vector<IDirect3DTexture9 *> &list = g_dev.tex[model];
    if (list.size() < m.images.size())
        list.resize(m.images.size(), nullptr);
    if (list[image])
        return list[image];
    const GlbImage &img = m.images[image];
    IDirect3DTexture9 *t = nullptr;
    if (FAILED(dev->CreateTexture(img.width, img.height, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t,
                                  nullptr)) ||
        !t)
    {
        IVR_ERR("GLB hands: could not create a %dx%d texture", img.width, img.height);
        g_dev.texFailed = true;
        return nullptr;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0)))
    {
        for (int y = 0; y < img.height; ++y)
        {
            const uint8_t *src = &img.rgba[(size_t)y * img.width * 4];
            uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + (size_t)y * lr.Pitch;
            for (int x = 0; x < img.width; ++x)
            {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
        t->UnlockRect(0);
    }
    list[image] = t;
    return t;
}

bool EnsureShaders(IDirect3DDevice9 *dev)
{
    if (g_dev.vs && g_dev.ps && g_dev.decl)
        return true;
    if (g_dev.shaderFailed || g_vsCode.empty() || g_psCode.empty())
        return false;
    static const D3DVERTEXELEMENT9 elems[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        {0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    if (FAILED(dev->CreateVertexShader(g_vsCode.data(), &g_dev.vs)) ||
        FAILED(dev->CreatePixelShader(g_psCode.data(), &g_dev.ps)) ||
        FAILED(dev->CreateVertexDeclaration(elems, &g_dev.decl)))
    {
        IVR_ERR("GLB hands: the device refused the hand shaders - lit per vertex instead");
        g_dev.shaderFailed = true;
        return false;
    }
    IVR_LOG("GLB hands: drawn with world lighting and normal maps");
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
    if (IsOffCenter(v)) // only part of the frustum: top, bottom, left, right as 0..1 across it
    {
        const float *o = OffCenterTBLR(v);
        float l = -tanX + 2.0f * tanX * o[2], r = -tanX + 2.0f * tanX * o[3];
        float b = -tanY + 2.0f * tanY * o[1], t = -tanY + 2.0f * tanY * o[0];
        proj._11 = 2.0f / (r - l);
        proj._22 = 2.0f / (t - b);
        proj._31 = -(r + l) / (r - l);
        proj._32 = -(t + b) / (t - b);
    }
    proj._33 = q;
    proj._34 = 1.0f;
    proj._43 = -q * zn;
}

D3DMATRIX Mul(const D3DMATRIX &a, const D3DMATRIX &b)
{
    D3DMATRIX r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// {85C31227-3DE5-4f00-9B3A-F11AC38C18B5}
const GUID kIID_Texture9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};

// The camera screen texture INFRA's lens view is copied into, and the part of it to show
// (cropped to the model screen's shape). Render thread.
IDirect3DTexture9 *ScreenTexture(float aspect, float rect[4])
{
    IDirect3DSurface9 *s = g_D3D.cameraSurface;
    if (!s || !g_Config.cameraScreenView || g_D3D.learnW <= 0 || g_D3D.learnH <= 0)
        return nullptr;
    D3DSURFACE_DESC d{};
    IDirect3DTexture9 *tex = nullptr;
    if (FAILED(s->GetDesc(&d)) || !d.Width || !d.Height ||
        FAILED(s->GetContainer(kIID_Texture9, reinterpret_cast<void **>(&tex))) || !tex)
        return nullptr;
    // The lens view is stretched over the whole texture (see hkStretchRect); crop it to the
    // screen's shape.
    float w = (float)g_D3D.learnW, h = (float)g_D3D.learnH;
    float cw = 1.0f, ch = 1.0f;
    if (w / h > aspect)
        cw = h * aspect / w;
    else
        ch = w / aspect / h;
    rect[0] = (1.0f - cw) * 0.5f;
    rect[1] = (1.0f - ch) * 0.5f;
    rect[2] = cw;
    rect[3] = ch;
    return tex; // AddRef'd by GetContainer
}

void Draw(const std::shared_ptr<Frame> &fr, const CViewSetup &v)
{
    IDirect3DDevice9 *dev = g_D3D.device;
    if (!dev || fr->items.empty())
        return;
    if (!g_dev.state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_dev.state)))
        g_dev.state = nullptr;
    if (g_dev.state)
        g_dev.state->Capture();

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
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); // double-sided materials; the right hand is mirrored
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
    for (DWORD s = 0; s < 2; ++s)
    {
        dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    bool shaders = EnsureShaders(dev);
    D3DMATRIX vpm = Mul(view, proj);
    for (const DrawItem &it : fr->items)
    {
        const ModelData &md = g_model[it.model];
        const GlbModel &m = md.glb;
        bool useShader = it.shader && shaders;
        if (useShader)
        {
            dev->SetVertexDeclaration(g_dev.decl);
            dev->SetVertexShader(g_dev.vs);
            dev->SetPixelShader(g_dev.ps);
            dev->SetVertexShaderConstantF(0, &vpm._11, 4);
            float ps[PS_COUNT][4];
            std::memcpy(ps, it.ps, sizeof(ps));
            ps[PS_EYE][0] = v.origin.x, ps[PS_EYE][1] = v.origin.y, ps[PS_EYE][2] = v.origin.z;
            ps[PS_PARAMS][2] = g_Config.handsNormalMapGreen;
            dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE); // base colour: sRGB -> linear
            for (const Batch &b : md.batches)
            {
                const GlbMaterial *mat = b.material >= 0 ? &m.materials[b.material] : nullptr;
                int baseImage = !mat ? -1 : it.altTexture && b.material == md.itemMaterial ? md.altImage : mat->baseImage;
                IDirect3DTexture9 *base = mat ? Texture(dev, it.model, baseImage) : nullptr;
                IDirect3DTexture9 *nmap =
                    mat && g_Config.handsNormalMaps ? Texture(dev, it.model, mat->normalImage) : nullptr;
                ps[PS_EYE][3] = nmap ? 1.0f : 0.0f;
                dev->SetPixelShaderConstantF(0, &ps[0][0], PS_COUNT);
                dev->SetTexture(0, base);
                dev->SetTexture(1, nmap);
                dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)it.sv.size(), (UINT)b.indices.size() / 3,
                                            b.indices.data(), D3DFMT_INDEX16, it.sv.data(), sizeof(SVertex));
            }
            dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
            dev->SetTexture(1, nullptr);
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
        }
        else if (!it.fv.empty())
        {
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(kFvf);
            for (const Batch &b : md.batches)
            {
                const GlbMaterial *mat = b.material >= 0 ? &m.materials[b.material] : nullptr;
                int baseImage = !mat ? -1 : it.altTexture && b.material == md.itemMaterial ? md.altImage : mat->baseImage;
                IDirect3DTexture9 *base = mat ? Texture(dev, it.model, baseImage) : nullptr;
                dev->SetTexture(0, base);
                dev->SetTextureStageState(0, D3DTSS_COLOROP, base ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
                dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)it.fv.size(), (UINT)b.indices.size() / 3,
                                            b.indices.data(), D3DFMT_INDEX16, it.fv.data(), sizeof(FVertex));
            }
        }
        // Camera screen: the lens view, unlit, cropped to the screen's shape. Without it, the
        // screen keeps the model's own (dark) texture.
        if (!it.screen.empty())
        {
            dev->SetVertexShader(nullptr);
            dev->SetPixelShader(nullptr);
            dev->SetFVF(kFvf);
            float rect[4];
            // INFRA's viewfinder panel is added on top (it draws additively): the screen stays dark
            // under it, as on INFRA's own camera.
            IDirect3DTexture9 *screen = it.panelOnScreen ? nullptr : ScreenTexture(md.screenAspect, rect);
            std::vector<FVertex> verts = it.screen;
            if (screen)
            {
                for (FVertex &fv : verts)
                {
                    float s = it.mirrored ? 1.0f - fv.u : fv.u; // keep the picture the right way round
                    fv.u = rect[0] + s * rect[2];
                    fv.v = rect[1] + fv.v * rect[3];
                }
                DWORD k = (DWORD)std::lround(std::min(1.0f, std::max(0.0f, g_Config.cameraScreenBrightness)) * 255.0f);
                dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, k, k, k));
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                dev->SetTexture(0, screen);
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)verts.size() / 3, verts.data(), sizeof(FVertex));
                dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
                screen->Release();
            }
            else
            {
                // No lens picture yet: a dark screen.
                for (FVertex &fv : verts)
                    fv.color = D3DCOLOR_ARGB(255, 6, 6, 8);
                dev->SetTexture(0, nullptr);
                dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
                dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)verts.size() / 3, verts.data(), sizeof(FVertex));
            }
        }
    }
    dev->SetTexture(0, nullptr);
    if (g_dev.state)
        g_dev.state->Apply();
}

} // namespace

bool Enabled() { return TryLoad(); }

bool ItemModelEnabled(int item) { return item >= 0 && item < ITEM_COUNT && TryLoadItem(item); }

const std::vector<std::string> &ItemJointNames(int item) { return g_items[item].jointNames; }

void SetItemBones(int item, const std::vector<GlbMat34> &world)
{
    ItemModel &im = g_items[item];
    std::lock_guard<std::mutex> lock(g_boneLock);
    if (im.bones.empty() || g_eyeFrame.load() - im.bonesFrame > kItemGapFrames)
        im.shownAt = NowSeconds(); // taken out: the draw animation starts now
    im.bones = world;
    im.bonesFrame = g_eyeFrame.load();
}

bool CameraModelShown()
{
    const ItemModel &im = g_items[ITEM_CAMERA];
    if (im.state <= 0)
        return false;
    std::lock_guard<std::mutex> lock(g_boneLock);
    return !im.bones.empty() && g_eyeFrame.load() - im.bonesFrame <= kItemGapFrames;
}

namespace {
// The camera's lens / the flashlight's beam as the item model is drawn (x out of it, z up).
bool ItemLens(int item, Pose &out)
{
    if (g_items[item].state <= 0)
        return false;
    const ModelData &md = g_model[g_items[item].model];
    if (!md.haveLens || md.cameraJoint < 0)
        return false;
    std::vector<GlbMat34> bones;
    if (!ItemBones(item, bones) || md.cameraJoint >= (int)bones.size())
        return false;
    Sim joint = Sim::From(bones[md.cameraJoint]); // carries the model's scale
    Sim lensLocal{md.lensLocal, 1.0f};
    Pose lens = (joint * lensLocal).p;
    CameraPlacement place = ItemPlace(item);
    if (place.mirrored)
    {
        Vector f = MirrorDir(place, lens.rot.Forward()), u = MirrorDir(place, lens.rot.Up());
        lens.pos = MirrorPoint(place, lens.pos);
        lens.rot.SetCol(0, f);
        lens.rot.SetCol(1, Cross(u, f));
        lens.rot.SetCol(2, u);
    }
    lens.rot = Orthonormal(lens.rot);
    out = lens;
    return true;
}
} // namespace

bool CameraLens(Pose &out) { return ItemLens(ITEM_CAMERA, out); }

bool FlashlightBeam(Pose &out) { return ItemLens(ITEM_FLASHLIGHT, out); }

bool PhoneScreen(Pose &out, float &width, float &height)
{
    if (g_items[ITEM_PHONE].state <= 0)
        return false;
    const ModelData &md = g_model[MODEL_PHONE];
    std::vector<GlbMat34> bones;
    if (md.screenJoint < 0 || !ItemBones(ITEM_PHONE, bones) || md.screenJoint >= (int)bones.size())
        return false;
    Sim joint = Sim::From(bones[md.screenJoint]);
    Pose sp = (joint * Sim{md.screenLocal, 1.0f}).p;
    sp.rot = Orthonormal(sp.rot);
    CameraPlacement place = ItemPlace(ITEM_PHONE);
    if (place.mirrored)
    {
        Vector x = MirrorDir(place, sp.rot.Col(0)), y = MirrorDir(place, sp.rot.Col(1));
        sp.pos = MirrorPoint(place, sp.pos);
        x = x * -1.0f; // keep the picture readable: mirror its right edge back
        sp.rot.SetCol(0, x);
        sp.rot.SetCol(1, y);
        sp.rot.SetCol(2, Cross(x, y));
    }
    out = sp;
    width = md.screenW * joint.scale;
    height = md.screenH * joint.scale;
    return true;
}

bool ItemPanel(int item, const char *bone, const Vector &centre, const Mat3 &frame, Pose &out, float &scale)
{
    if (item < 0 || item >= ITEM_COUNT || g_items[item].state <= 0 || !bone)
        return false;
    const ModelData &md = g_model[g_items[item].model];
    int j = md.glb.JointByName(bone);
    std::vector<GlbMat34> bones;
    if (j < 0 || !ItemBones(item, bones) || j >= (int)bones.size())
        return false;
    Sim b = Sim::From(bones[j]); // INFRA's bone moved onto the VR hand, with the model's scale
    Pose p;
    p.pos = b.p.rot * (centre * b.scale) + b.p.pos;
    p.rot = Orthonormal(b.p.rot * frame); // keeps x (width) and z (out of the panel)
    CameraPlacement place = ItemPlace(item);
    if (place.mirrored)
    {
        Vector x = MirrorDir(place, p.rot.Col(0)) * -1.0f, z = MirrorDir(place, p.rot.Col(2));
        p.pos = MirrorPoint(place, p.pos);
        p.rot.SetCol(0, x);
        p.rot.SetCol(1, Cross(z, x));
        p.rot.SetCol(2, z);
    }
    g_items[item].panelFrame = g_eyeFrame.load();
    scale = b.scale;
    out = p;
    return true;
}

float CameraScreenAspect() { return g_model[MODEL_CAMERA].screenAspect; }

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
    if (eye == 1)
    {
        g_eyeFrame.fetch_add(1);
        if (++g_framesDrawn == kTrustFrames)
        {
            flags::End(kFlag);
            IVR_LOG("GLB hands: %d frames drawn without trouble", g_framesDrawn);
        }
    }
}

void OnDeviceReset()
{
    for (auto &list : g_dev.tex)
    {
        for (auto &t : list)
            if (t)
                t->Release();
        list.clear();
    }
    if (g_dev.state)
        g_dev.state->Release();
    if (g_dev.vs)
        g_dev.vs->Release();
    if (g_dev.ps)
        g_dev.ps->Release();
    if (g_dev.decl)
        g_dev.decl->Release();
    g_dev.state = nullptr;
    g_dev.vs = nullptr;
    g_dev.ps = nullptr;
    g_dev.decl = nullptr;
    g_dev.shaderFailed = false;
}

} // namespace hands
} // namespace ivr
