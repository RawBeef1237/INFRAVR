// Loads the bundled GLB hands, checks the skeleton is recognised and that bending the fingers
// moves the finger tips towards the palm. Writes <name>_rest.obj / <name>_fist.obj for a look.
// Usage: glb_test <assets/hands folder> [out folder]
#include "glb_model.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ivr;

static void WriteObj(const std::string &path, const GlbModel &m, const std::vector<float> &p)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    for (size_t v = 0; v < m.vertices.size(); ++v)
        fprintf(f, "v %f %f %f\n", p[v * 3], p[v * 3 + 1], p[v * 3 + 2]);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        fprintf(f, "f %d %d %d\n", m.indices[i] + 1, m.indices[i + 1] + 1, m.indices[i + 2] + 1);
    fclose(f);
}

// Node local transform as a row-major 3x4.
static GlbMat34 Local(const GlbNode &n)
{
    float x = n.r[0], y = n.r[1], z = n.r[2], w = n.r[3];
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    GlbMat34 m{};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
            m.m[r][c] = R[r][c] * n.s[c];
        m.m[r][3] = n.t[r];
    }
    return m;
}

static GlbMat34 Mul(const GlbMat34 &a, const GlbMat34 &b)
{
    GlbMat34 r{};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            float s = j == 3 ? a.m[i][3] : 0.0f;
            for (int k = 0; k < 3; ++k)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    }
    return r;
}

// Tangents must be unit length and perpendicular to the normals.
static int CheckTangents(const char *name, const GlbModel &m)
{
    int bad = 0;
    for (const GlbVertex &v : m.vertices)
    {
        float len = std::sqrt(v.tangent[0] * v.tangent[0] + v.tangent[1] * v.tangent[1] + v.tangent[2] * v.tangent[2]);
        float d = v.tangent[0] * v.normal[0] + v.tangent[1] * v.normal[1] + v.tangent[2] * v.normal[2];
        float nl = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
        if (std::fabs(len - 1.0f) > 0.01f || std::fabs(d) > 0.02f * nl || (v.tangent[3] != 1.0f && v.tangent[3] != -1.0f))
            ++bad;
    }
    printf("  tangents: %d of %zu bad\n", bad, m.vertices.size());
    if (bad > (int)m.vertices.size() / 100)
    {
        printf("  FAIL: %s has bad tangents\n", name);
        return 1;
    }
    return 0;
}

// The camera model: INFRA's v_camera bones (Source units) must drive it as one rigid camera held
// in the hand, exactly as the model's own rest pose does.
static int TestCamera(const std::string &path, const std::string &out)
{
    GlbModel m;
    std::string err;
    if (!m.Load(path, &err))
    {
        printf("camera_hand: FAIL %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    printf("camera_hand: %zu vertices, %zu triangles, %zu joints, %zu materials, %zu images, %zu primitives\n",
           m.vertices.size(), m.indices.size() / 3, m.joints.size(), m.materials.size(), m.images.size(),
           m.primitives.size());
    for (const GlbMaterial &mat : m.materials)
    {
        printf("  material %s: base %d, normal map %d\n", mat.name.c_str(), mat.baseImage, mat.normalImage);
        if (mat.baseImage < 0 || mat.normalImage < 0)
        {
            printf("  FAIL: expected a base texture and a normal map\n");
            ++failures;
        }
    }
    failures += CheckTangents("camera_hand", m);
    int hand = m.JointByName("Bip001 R Hand"), cam = m.JointByName("w_camera003");
    if (hand < 0 || cam < 0 || m.primitives.size() != 2)
    {
        printf("  FAIL: expected the v_camera bones (Bip001 R Hand, w_camera003) and two primitives\n");
        return failures + 1;
    }
    // Bone-to-world like the game: the hand somewhere in the world (Source units), its children
    // from their local transforms (which are v_camera's own bones).
    GlbMat34 H{};
    float c = std::cos(0.7f), s = std::sin(0.7f);
    float HR[3][3] = {{c, -s, 0}, {s, c, 0}, {0, 0, 1}};
    for (int r = 0; r < 3; ++r)
    {
        for (int k = 0; k < 3; ++k)
            H.m[r][k] = HR[r][k];
    }
    H.m[0][3] = 512, H.m[1][3] = -256, H.m[2][3] = 64;
    std::vector<GlbMat34> world(m.joints.size());
    std::vector<GlbMat34> nodeWorld(m.nodes.size());
    int handNode = m.joints[hand];
    for (size_t j = 0; j < m.joints.size(); ++j)
    {
        std::vector<int> chain;
        for (int n = m.joints[j]; n >= 0 && n != handNode; n = m.nodes[n].parent)
            chain.push_back(n);
        GlbMat34 w = H;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            w = Mul(w, Local(m.nodes[*it]));
        world[j] = w;
    }
    std::vector<float> p, n, t;
    m.SkinJoints(world, p, n, &t);
    // Camera primitive extent, in its own (w_camera003) frame.
    const GlbPrimitive &cp = m.primitives[1];
    const GlbMat34 &W = world[cam];
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t i = cp.firstIndex; i < cp.firstIndex + cp.indexCount; ++i)
    {
        int v = m.indices[i];
        float d[3] = {p[v * 3] - W.m[0][3], p[v * 3 + 1] - W.m[1][3], p[v * 3 + 2] - W.m[2][3]};
        for (int a = 0; a < 3; ++a)
        {
            float l = W.m[0][a] * d[0] + W.m[1][a] * d[1] + W.m[2][a] * d[2];
            lo[a] = std::fmin(lo[a], l);
            hi[a] = std::fmax(hi[a], l);
        }
    }
    printf("  camera in its bone frame: x [%.2f %.2f] y [%.2f %.2f] z [%.2f %.2f] (game units)\n", lo[0], hi[0], lo[1],
           hi[1], lo[2], hi[2]);
    float ex[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    if (std::fabs(ex[0] - 5.46f) > 0.3f || std::fabs(ex[1] - 0.84f) > 0.2f || std::fabs(ex[2] - 3.2f) > 0.3f)
    {
        printf("  FAIL: the camera should be about 5.5 x 0.8 x 3.2 game units in its bone frame\n");
        ++failures;
    }
    // The hand holds it: every hand vertex within 8 units of the camera's centre.
    float cc[3] = {W.m[0][3], W.m[1][3], W.m[2][3]}, far = 0;
    const GlbPrimitive &hp = m.primitives[0];
    for (uint32_t i = hp.firstIndex; i < hp.firstIndex + hp.indexCount; ++i)
    {
        int v = m.indices[i];
        float dx = p[v * 3] - cc[0], dy = p[v * 3 + 1] - cc[1], dz = p[v * 3 + 2] - cc[2];
        far = std::fmax(far, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    printf("  farthest hand vertex from the camera: %.2f game units\n", far);
    if (far > 9.0f || far < 2.0f)
    {
        printf("  FAIL: the hand is not around the camera\n");
        ++failures;
    }
    // Same pose through the model's own rest skinning (scaled by the model's root) - the shapes
    // must match: compare distances between a few vertices.
    std::vector<float> rp, rn;
    float zero[5] = {0, 0, 0, 0, 0};
    m.Skin(zero, 0.0f, rp, rn);
    double ratioMin = 1e9, ratioMax = 0;
    for (size_t v = 1; v < m.vertices.size(); v += 97)
    {
        auto dist = [](const std::vector<float> &q, size_t a, size_t b) {
            float dx = q[a * 3] - q[b * 3], dy = q[a * 3 + 1] - q[b * 3 + 1], dz = q[a * 3 + 2] - q[b * 3 + 2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        float a = dist(p, 0, v), b = dist(rp, 0, v);
        if (b > 1e-4f)
        {
            ratioMin = std::fmin(ratioMin, a / b);
            ratioMax = std::fmax(ratioMax, a / b);
        }
    }
    printf("  game pose vs rest pose scale: %.3f .. %.3f\n", ratioMin, ratioMax);
    if (ratioMax / ratioMin > 1.01)
    {
        printf("  FAIL: the game bones do not reproduce the model's shape\n");
        ++failures;
    }
    FILE *f = fopen((out + "/camera_hand_game.obj").c_str(), "w");
    if (f)
    {
        for (size_t v = 0; v < m.vertices.size(); ++v)
            fprintf(f, "v %f %f %f\n", p[v * 3], p[v * 3 + 1], p[v * 3 + 2]);
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
            fprintf(f, "f %d %d %d\n", m.indices[i] + 1, m.indices[i + 1] + 1, m.indices[i + 2] + 1);
        fclose(f);
    }
    return failures;
}

int main(int argc, char **argv)
{
    std::string dir = argc > 1 ? argv[1] : "assets/hands";
    std::string out = argc > 2 ? argv[2] : ".";
    int failures = 0;
    for (const char *name : {"vr_hand_left", "vr_hand_right"})
    {
        GlbModel m;
        std::string err;
        if (!m.Load(dir + "/" + name + ".glb", &err))
        {
            printf("%s: FAIL %s\n", name, err.c_str());
            ++failures;
            continue;
        }
        printf("%s: %zu vertices, %zu triangles, %zu joints, %d finger joints, texture %dx%d\n", name,
               m.vertices.size(), m.indices.size() / 3, m.joints.size(), m.fingerJoints, m.texWidth, m.texHeight);
        printf("  rest bounds x [%.3f %.3f] y [%.3f %.3f] z [%.3f %.3f]\n", m.boundsMin[0], m.boundsMax[0],
               m.boundsMin[1], m.boundsMax[1], m.boundsMin[2], m.boundsMax[2]);
        bool boundsOk = true;
        for (int k = 0; k < 3; ++k)
            boundsOk = boundsOk && m.boundsMin[k] < m.boundsMax[k];
        if (!boundsOk || m.boundsMax[0] - m.boundsMin[0] < 0.1f)
        {
            printf("  FAIL: rest bounds are wrong (a hand is wider than 10 cm)\n");
            ++failures;
        }
        if (m.fingerJoints < 15)
        {
            printf("  FAIL: expected the biped finger joints\n");
            ++failures;
        }
        std::vector<float> rest, fist, n;
        float zero[5] = {0, 0, 0, 0, 0}, full[5] = {1, 1, 1, 1, 1};
        m.Skin(zero, 75.0f, rest, n);
        m.Skin(full, 75.0f, fist, n);
        // Vertices driven by the index finger's last segment move most; measure the largest move.
        float maxMove = 0;
        float cx = 0, cy = 0, cz = 0;
        for (size_t v = 0; v < m.vertices.size(); ++v)
        {
            float dx = fist[v * 3] - rest[v * 3], dy = fist[v * 3 + 1] - rest[v * 3 + 1], dz = fist[v * 3 + 2] - rest[v * 3 + 2];
            maxMove = std::fmax(maxMove, std::sqrt(dx * dx + dy * dy + dz * dz));
            cx += rest[v * 3];
            cy += rest[v * 3 + 1];
            cz += rest[v * 3 + 2];
        }
        size_t nv = m.vertices.size();
        printf("  centre (%.3f %.3f %.3f), largest finger-tip move for a fist: %.3f m\n", cx / nv, cy / nv, cz / nv,
               maxMove);
        if (maxMove < 0.03f)
        {
            printf("  FAIL: the fingers do not bend\n");
            ++failures;
        }
        failures += CheckTangents(name, m);
        WriteObj(out + "/" + name + "_rest.obj", m, rest);
        WriteObj(out + "/" + name + "_fist.obj", m, fist);
    }
    failures += TestCamera(dir + "/camera_hand.glb", out);
    {
        // The flashlight model: v_light's bones (left hand, flashlight, body), hand joint first.
        GlbModel f;
        std::string err;
        if (!f.Load(dir + "/flashlight_hand.glb", &err))
        {
            printf("flashlight_hand: FAIL %s\n", err.c_str());
            ++failures;
        }
        else
        {
            printf("flashlight_hand: %zu vertices, %zu joints, %zu materials\n", f.vertices.size(), f.joints.size(),
                   f.materials.size());
            if (f.JointByName("Bip001 L Hand") != 0 || f.JointByName("flashlight") < 0 || f.materials.size() != 2)
            {
                printf("  FAIL: expected v_light's bones (Bip001 L Hand first, flashlight) and two materials\n");
                ++failures;
            }
            failures += CheckTangents("flashlight_hand", f);
        }
        GlbModel ph;
        if (!ph.Load(dir + "/phone_hand.glb", &err))
        {
            printf("phone_hand: FAIL %s\n", err.c_str());
            ++failures;
        }
        else
        {
            printf("phone_hand: %zu vertices, %zu joints, %zu materials\n", ph.vertices.size(), ph.joints.size(),
                   ph.materials.size());
            if (ph.JointByName("Bip001 R Hand") != 0 || ph.JointByName("cellular_phone") < 0 || ph.materials.size() != 2)
            {
                printf("  FAIL: expected v_phone's bones (Bip001 R Hand first, cellular_phone) and two materials\n");
                ++failures;
            }
            failures += CheckTangents("phone_hand", ph);
        }
        GlbImage call;
        if (!GlbModel::LoadImageFile(dir + "/phone_hand_call.png", call) || call.width != 1024)
        {
            printf("phone_hand_call.png: FAIL could not be decoded\n");
            ++failures;
        }
    }
    printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
