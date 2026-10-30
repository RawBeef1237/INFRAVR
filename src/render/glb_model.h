// INFRA VR - skinned glTF binary (.glb) hand model: loading, finger posing and CPU skinning.
// No D3D in here (hands_renderer.cpp draws it), so tests can load and pose the models directly.
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

class GlbModel
{
public:
    bool Load(const std::string &path, std::string *error = nullptr);
    bool Loaded() const { return !vertices.empty(); }

    // Poses the fingers: curl[f] 0 = rest .. 1 = fully bent, maxDegrees per joint at full curl.
    // Fills `out` with skinned positions and normals (model space, metres, glTF axes).
    void Skin(const float curl[5], float maxDegrees, std::vector<float> &outPos, std::vector<float> &outNormal) const;

    std::vector<GlbVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<GlbNode> nodes;
    std::vector<int> joints;               // skin joint -> node
    std::vector<float> inverseBind;        // 16 floats per joint, column-major
    int fingerJoints = 0;                  // joints recognised as finger segments
    // Base colour texture, RGBA8 (empty if the model has none).
    int texWidth = 0, texHeight = 0;
    std::vector<uint8_t> texRGBA;
    float baseColor[4] = {1, 1, 1, 1};
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0}; // rest pose, model space
};

} // namespace ivr
