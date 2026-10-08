// Offline check of the tracked-hands bone override against INFRA's real viewmodel files.
// Usage: hands_test.exe v_phone.mdl
#include "../src/hooks_game.cpp"

#include <cmath>
#include <vector>

using namespace ivr;

static Pose Mirror(const Pose &p) // world x reflection on the left, controller left axis on the right
{
    Pose r = p;
    r.pos.x = -r.pos.x;
    for (int c = 0; c < 3; ++c)
        r.rot.m[0][c] = -r.rot.m[0][c];
    for (int i = 0; i < 3; ++i)
        r.rot.m[i][1] = -r.rot.m[i][1];
    return r;
}

static float Diff(const Pose &a, const Pose &b)
{
    float d = (a.pos - b.pos).Length();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            d = std::max(d, 10.0f * std::fabs(a.rot.m[i][j] - b.rot.m[i][j]));
    return d;
}

int main(int argc, char **argv)
{
    FILE *f = std::fopen(argc > 1 ? argv[1] : "v_phone.mdl", "rb");
    if (!f)
        return 2;
    std::vector<uint8_t> mdl(1 << 20);
    mdl.resize(std::fread(mdl.data(), 1, mdl.size(), f));
    std::fclose(f);
    const uint8_t *studio = mdl.data();
    const uint8_t *studioHdr[1] = {studio};
    int n = *reinterpret_cast<const int *>(studio + 156), bi = *reinterpret_cast<const int *>(studio + 160);
    std::vector<matrix3x4_t> bones(n);
    for (int b = 0; b < n; ++b)
    {
        matrix3x4_t p2b;
        std::memcpy(&p2b, studio + bi + b * kStudioBoneSize + 96, sizeof(p2b));
        bones[b] = matrix3x4_t::FromPose(p2b.ToPose().Inverse());
    }
    std::vector<matrix3x4_t> orig = bones;
    alignas(16) uint8_t obj[64] = {};
    *reinterpret_cast<matrix3x4_t **>(obj + 16) = bones.data();
    g_Game.boneArrayOffset = 16;
    InitializeCriticalSection(&g_handLock);
    g_Config.handsHideUpperArm = true;

    Arm L = FindArm(studio, 'L'), R = FindArm(studio, 'R');
    int finger = FindBone(studio, "Bip001 L Finger1");
    Pose mR = orig[R.hand].ToPose(), mL = orig[L.hand].ToPose();
    HandTargets t;
    t.valid = true;
    t.item = {mR.pos + Vector{1, 2, -1}, AnglesToMatrix(QAngle(20, 30, 10))};
    t.other = Mirror(t.item);
    SetHandTargets(t);
    MoveOtherArm(obj, studioHdr);
    int fails = 0;
    auto check = [&](const char *what, float d, float tol) {
        std::printf("%-40s %8.4f %s\n", what, d, d <= tol ? "ok" : "FAIL");
        fails += d > tol;
    };
    check("symmetric: left hand stays put", Diff(bones[L.hand].ToPose(), mL), 0.5f);
    check("right hand untouched", Diff(bones[R.hand].ToPose(), mR), 1e-4f);
    if (finger >= 0)
        check("finger keeps its place on the hand",
              Diff(bones[L.hand].ToPose().Inverse() * bones[finger].ToPose(),
                   mL.Inverse() * orig[finger].ToPose()), 1e-3f);
    float up = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            up += std::fabs(bones[L.upperArm].m[r][c]);
    check("upper arm collapsed", up, 0.0f);

    // Move the other controller: the hand follows it rigidly.
    bones = orig;
    Pose moved = Pose{Vector{5, -3, 2}, RotationZ(30)} * t.other;
    HandTargets t2 = t;
    t2.other = moved;
    SetHandTargets(t2);
    MoveOtherArm(obj, studioHdr);
    check("moved: hand follows the controller", Diff(bones[L.hand].ToPose(), moved * t.other.Inverse() * mL), 0.5f);
    std::printf("item in the model's %s hand (%s)\n", g_vmItemSide ? "right" : "left",
                g_vmItemIsLight ? "flashlight" : "camera/phone");
    float spine = 0;
    int sp = FindBone(studio, "Bip001 Spine");
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            spine += std::fabs(bones[sp].m[r][c]);
    check("spine collapsed", spine, 0.0f);
    std::printf("%s\n", fails ? "FAILED" : "PASSED");
    return fails;
}
