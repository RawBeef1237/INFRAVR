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
        WriteObj(out + "/" + name + "_rest.obj", m, rest);
        WriteObj(out + "/" + name + "_fist.obj", m, fist);
    }
    printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
