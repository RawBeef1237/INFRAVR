// INFRA VR - skinned glTF binary (.glb) hand model: loading, finger posing and CPU skinning.
// No D3D in here (hands_renderer.cpp draws it), so tests can load and pose the models directly.
//
// Every primitive keeps its material (base colour and normal map); tangents are worked out from
// the texture coordinates at load time so the normal maps can be used. A model can also be posed
// from outside: SkinJoints takes a world matrix per skin joint (the camera model takes them from
// INFRA's own v_camera animation, whose bones it shares).
//
// Expected model (the bundled vr_hand_left/right.glb): one skinned mesh, a 3ds Max biped hand
// skeleton ("Bip001 R Hand", "Bip001 R Finger0".."Finger43"), metres, glTF axes (Y up, -Z
// forward, X right) with the wrist near the origin and the fingers pointing along -Z - the same
// frame as an OpenVR/OpenXR controller pose. Finger joints bend around their local Z axis.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ivr {

struct GlbVertex
{
    float pos[3];
    float normal[3];
    float tangent[4]; // xyz along +u, w = +-1: the normal map's "up" (+Y, towards -v) is w * cross(N, T)
    float uv[2];
    uint16_t joints[4];
    float weights[4];
};

struct GlbNode
{
    std::string name;
    int parent = -1;
    float t[3] = {0, 0, 0};
    float r[4] = {0, 0, 0, 1}; // quaternion x y z w
    float s[3] = {1, 1, 1};
    // Finger this joint belongs to (0 thumb .. 4 pinky) and its segment from the knuckle, or -1.
    int finger = -1, segment = -1;
};

struct GlbImage
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // RGBA8, top row first
};

struct GlbMaterial
{
    std::string name;
    int baseImage = -1;   // into images, -1 = none
    int normalImage = -1; // tangent-space normal map (glTF: +X right, +Y up), -1 = none
    float baseColor[4] = {1, 1, 1, 1};
};

struct GlbPrimitive
{
    uint32_t firstIndex = 0, indexCount = 0; // range of `indices`
    int material = -1;
};

// Row-major 3x4 (like Source's matrix3x4_t): world = M * [x y z 1].
struct GlbMat34
{
    float m[3][4];
};

class GlbModel
{
public:
    bool Load(const std::string &path, std::string *error = nullptr);
    bool Loaded() const { return !vertices.empty(); }

    // Poses the fingers: curl[f] 0 = rest .. 1 = fully bent, maxDegrees per joint at full curl.
    // Fills `out` with skinned positions and normals (model space, metres, glTF axes).
    // outTangent (optional) gets 4 floats per vertex (w = the vertex's handedness).
    void Skin(const float curl[5], float maxDegrees, std::vector<float> &outPos, std::vector<float> &outNormal,
              std::vector<float> *outTangent = nullptr) const;
    // Skins with a world transform per skin joint (jointWorld[j] maps joint j's local space to
    // the world; units are whatever the joints use). Same outputs as Skin.
    void SkinJoints(const std::vector<GlbMat34> &jointWorld, std::vector<float> &outPos,
                    std::vector<float> &outNormal, std::vector<float> *outTangent = nullptr) const;
    // Skin joint by node name, or -1.
    int JointByName(const std::string &name) const;
    // Inverse bind matrix of joint j as a row-major 3x4 (mesh space -> joint j's local space).
    GlbMat34 InverseBind(int joint) const;
    // Decodes an image file (PNG, JPEG, ...) to RGBA8.
    static bool LoadImageFile(const std::string &path, GlbImage &out);

    std::vector<GlbVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<GlbNode> nodes;
    std::vector<int> joints;               // skin joint -> node
    std::vector<float> inverseBind;        // 16 floats per joint, column-major
    int fingerJoints = 0;                  // joints recognised as finger segments
    std::vector<GlbImage> images;
    std::vector<GlbMaterial> materials;
    std::vector<GlbPrimitive> primitives;
    // The first material's base colour texture (kept for the log and the test).
    int texWidth = 0, texHeight = 0;
    float baseColor[4] = {1, 1, 1, 1};
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0}; // rest pose, model space
};

} // namespace ivr
