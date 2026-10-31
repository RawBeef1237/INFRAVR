#include "glb_model.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ivr {

namespace {

// Column-major 4x4 matrices, like glTF.
struct M4
{
    float m[16];
    static M4 Identity()
    {
        M4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    M4 operator*(const M4 &b) const
    {
        M4 r{};
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr)
            {
                float s = 0;
                for (int k = 0; k < 4; ++k)
                    s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
};

M4 Trs(const float t[3], const float q[4], const float s[3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = M4::Identity();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    r.m[1] = (2 * (x * y + z * w)) * s[0];
    r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1];
    r.m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2];
    r.m[9] = (2 * (y * z - x * w)) * s[2];
    r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0];
    r.m[13] = t[1];
    r.m[14] = t[2];
    return r;
}

void QuatMul(const float a[4], const float b[4], float out[4])
{
    float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                  a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                  a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    std::memcpy(out, r, sizeof(r));
}

// "Bip001 R Finger23" -> finger 2, segment 3; "Bip001 R Finger0" -> thumb, segment 0.
void ParseFinger(GlbNode &n)
{
    size_t p = n.name.find("Finger");
    if (p == std::string::npos)
        return;
    const char *d = n.name.c_str() + p + 6;
    if (d[0] < '0' || d[0] > '4')
        return;
    n.finger = d[0] - '0';
    n.segment = (d[1] >= '0' && d[1] <= '9') ? d[1] - '0' : 0;
}

// Per-vertex tangents from positions and texture coordinates (summed over the triangles, then
// made perpendicular to the normal). w says which way the normal map's +Y points.
void ComputeTangents(std::vector<GlbVertex> &verts, const std::vector<uint16_t> &idx)
{
    std::vector<float> t(verts.size() * 3, 0.0f), b(verts.size() * 3, 0.0f);
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
    {
        const GlbVertex &v0 = verts[idx[i]], &v1 = verts[idx[i + 1]], &v2 = verts[idx[i + 2]];
        float e1[3], e2[3];
        for (int k = 0; k < 3; ++k)
        {
            e1[k] = v1.pos[k] - v0.pos[k];
            e2[k] = v2.pos[k] - v0.pos[k];
        }
        float du1 = v1.uv[0] - v0.uv[0], dv1 = v1.uv[1] - v0.uv[1];
        float du2 = v2.uv[0] - v0.uv[0], dv2 = v2.uv[1] - v0.uv[1];
        float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f)
            continue;
        float r = 1.0f / det;
        float T[3], B[3];
        for (int k = 0; k < 3; ++k)
        {
            T[k] = (e1[k] * dv2 - e2[k] * dv1) * r; // d position / d u
            B[k] = (e2[k] * du1 - e1[k] * du2) * r; // d position / d v
        }
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
            {
                t[idx[i + c] * 3 + k] += T[k];
                b[idx[i + c] * 3 + k] += B[k];
            }
    }
    for (size_t v = 0; v < verts.size(); ++v)
    {
        GlbVertex &gv = verts[v];
        const float *n = gv.normal;
        float *T = &t[v * 3];
        float d = n[0] * T[0] + n[1] * T[1] + n[2] * T[2];
        float o[3] = {T[0] - n[0] * d, T[1] - n[1] * d, T[2] - n[2] * d};
        float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (len < 1e-8f)
        {
            // No usable texture direction: any vector perpendicular to the normal.
            float a[3] = {std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
            d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
            for (int k = 0; k < 3; ++k)
                o[k] = a[k] - n[k] * d;
            len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        }
        for (int k = 0; k < 3; ++k)
            gv.tangent[k] = o[k] / std::max(len, 1e-8f);
        // Texture "up" is -v; it should be +cross(N, T) for w = +1.
        float c[3] = {n[1] * gv.tangent[2] - n[2] * gv.tangent[1], n[2] * gv.tangent[0] - n[0] * gv.tangent[2],
                      n[0] * gv.tangent[1] - n[1] * gv.tangent[0]};
        const float *B = &b[v * 3];
        gv.tangent[3] = (c[0] * -B[0] + c[1] * -B[1] + c[2] * -B[2]) < 0.0f ? -1.0f : 1.0f;
    }
}

} // namespace

bool GlbModel::Load(const std::string &path, std::string *error)
{
    auto fail = [&](const std::string &e) {
        if (error)
            *error = e;
        vertices.clear();
        return false;
    };
    cgltf_options opt{};
    cgltf_data *data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success)
        return fail("cannot read " + path);
    struct Free
    {
        cgltf_data *d;
        ~Free() { cgltf_free(d); }
    } guard{data};
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success)
        return fail("cannot load the buffers of " + path);

    // Nodes (parents and rest transforms).
    nodes.assign(data->nodes_count, GlbNode());
    for (size_t i = 0; i < data->nodes_count; ++i)
    {
        const cgltf_node &cn = data->nodes[i];
        GlbNode &n = nodes[i];
        n.name = cn.name ? cn.name : "";
        n.parent = cn.parent ? (int)(cn.parent - data->nodes) : -1;
        if (cn.has_translation)
            std::memcpy(n.t, cn.translation, sizeof(n.t));
        if (cn.has_rotation)
            std::memcpy(n.r, cn.rotation, sizeof(n.r));
        if (cn.has_scale)
            std::memcpy(n.s, cn.scale, sizeof(n.s));
        ParseFinger(n);
    }

    const cgltf_node *meshNode = nullptr;
    for (size_t i = 0; i < data->nodes_count && !meshNode; ++i)
        if (data->nodes[i].mesh && data->nodes[i].skin)
            meshNode = &data->nodes[i];
    if (!meshNode)
        return fail("no skinned mesh in " + path);
    const cgltf_skin *skin = meshNode->skin;
    joints.clear();
    inverseBind.assign(skin->joints_count * 16, 0.0f);
    fingerJoints = 0;
    for (size_t j = 0; j < skin->joints_count; ++j)
    {
        int node = (int)(skin->joints[j] - data->nodes);
        joints.push_back(node);
        fingerJoints += nodes[node].finger >= 0;
        if (skin->inverse_bind_matrices)
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, &inverseBind[j * 16], 16);
        else
            inverseBind[j * 16] = inverseBind[j * 16 + 5] = inverseBind[j * 16 + 10] = inverseBind[j * 16 + 15] = 1.0f;
    }

    vertices.clear();
    indices.clear();
    images.clear();
    materials.clear();
    primitives.clear();
    texWidth = texHeight = 0;
    // Images and materials, in glTF order (only images a material uses are decoded).
    std::vector<int> imageSlot(data->images_count, -1);
    auto loadImage = [&](const cgltf_texture *tex) -> int {
        const cgltf_image *img = tex ? tex->image : nullptr;
        if (!img || !img->buffer_view)
            return -1;
        size_t idx = (size_t)(img - data->images);
        if (imageSlot[idx] >= 0)
            return imageSlot[idx];
        const uint8_t *bytes = cgltf_buffer_view_data(img->buffer_view);
        int w = 0, h = 0, c = 0;
        uint8_t *px = bytes ? stbi_load_from_memory(bytes, (int)img->buffer_view->size, &w, &h, &c, 4) : nullptr;
        if (!px)
            return -1;
        GlbImage gi;
        gi.width = w;
        gi.height = h;
        gi.rgba.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
        images.push_back(std::move(gi));
        imageSlot[idx] = (int)images.size() - 1;
        return imageSlot[idx];
    };
    for (size_t i = 0; i < data->materials_count; ++i)
    {
        const cgltf_material &cm = data->materials[i];
        GlbMaterial gm;
        gm.name = cm.name ? cm.name : "";
        std::memcpy(gm.baseColor, cm.pbr_metallic_roughness.base_color_factor, sizeof(gm.baseColor));
        gm.baseImage = loadImage(cm.pbr_metallic_roughness.base_color_texture.texture);
        gm.normalImage = loadImage(cm.normal_texture.texture);
        materials.push_back(gm);
    }

    for (size_t p = 0; p < meshNode->mesh->primitives_count; ++p)
    {
        const cgltf_primitive &prim = meshNode->mesh->primitives[p];
        if (prim.type != cgltf_primitive_type_triangles)
            continue;
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (size_t a = 0; a < prim.attributes_count; ++a)
        {
            const cgltf_attribute &at = prim.attributes[a];
            if (at.type == cgltf_attribute_type_position)
                pos = at.data;
            else if (at.type == cgltf_attribute_type_normal)
                nrm = at.data;
            else if (at.type == cgltf_attribute_type_texcoord && at.index == 0)
                uv = at.data;
            else if (at.type == cgltf_attribute_type_joints && at.index == 0)
                jnt = at.data;
            else if (at.type == cgltf_attribute_type_weights && at.index == 0)
                wgt = at.data;
        }
        if (!pos)
            continue;
        size_t base = vertices.size();
        if (base + pos->count > 65535)
            return fail("too many vertices in " + path);
        for (size_t v = 0; v < pos->count; ++v)
        {
            GlbVertex gv{};
            cgltf_accessor_read_float(pos, v, gv.pos, 3);
            if (nrm)
                cgltf_accessor_read_float(nrm, v, gv.normal, 3);
            if (uv)
                cgltf_accessor_read_float(uv, v, gv.uv, 2);
            if (jnt && wgt)
            {
                cgltf_uint j4[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(jnt, v, j4, 4);
                cgltf_accessor_read_float(wgt, v, gv.weights, 4);
                for (int k = 0; k < 4; ++k)
                    gv.joints[k] = (uint16_t)std::min<cgltf_uint>(j4[k], (cgltf_uint)joints.size() - 1);
            }
            else
                gv.weights[0] = 1.0f;
            vertices.push_back(gv);
        }
        GlbPrimitive gp;
        gp.firstIndex = (uint32_t)indices.size();
        if (prim.indices)
            for (size_t i = 0; i < prim.indices->count; ++i)
                indices.push_back((uint16_t)(base + cgltf_accessor_read_index(prim.indices, i)));
        else
            for (size_t i = 0; i < pos->count; ++i)
                indices.push_back((uint16_t)(base + i));
        gp.indexCount = (uint32_t)indices.size() - gp.firstIndex;
        gp.material = prim.material ? (int)(prim.material - data->materials) : -1;
        primitives.push_back(gp);

        if (gp.material >= 0 && texWidth == 0)
        {
            const GlbMaterial &gm = materials[gp.material];
            std::memcpy(baseColor, gm.baseColor, sizeof(baseColor));
            if (gm.baseImage >= 0)
            {
                texWidth = images[gm.baseImage].width;
                texHeight = images[gm.baseImage].height;
            }
        }
    }
    ComputeTangents(vertices, indices);
    if (vertices.empty() || indices.empty())
        return fail("no triangles in " + path);

    std::vector<float> p, n;
    float none[5] = {0, 0, 0, 0, 0};
    Skin(none, 0.0f, p, n);
    for (int k = 0; k < 3; ++k)
        boundsMin[k] = boundsMax[k] = p[k];
    for (size_t v = 1; v < vertices.size(); ++v)
        for (int k = 0; k < 3; ++k)
        {
            float c = p[v * 3 + k];
            if (c < boundsMin[k])
                boundsMin[k] = c;
            if (c > boundsMax[k])
                boundsMax[k] = c;
        }
    return true;
}

int GlbModel::JointByName(const std::string &name) const
{
    for (size_t j = 0; j < joints.size(); ++j)
        if (nodes[joints[j]].name == name)
            return (int)j;
    return -1;
}

GlbMat34 GlbModel::InverseBind(int joint) const
{
    GlbMat34 r{};
    const float *m = &inverseBind[(size_t)joint * 16]; // column-major
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 4; ++c)
            r.m[row][c] = m[c * 4 + row];
    return r;
}

namespace {

// Blends the skinning matrices (column-major 4x4, 16 floats each) per vertex.
void SkinWith(const std::vector<GlbVertex> &vertices, const std::vector<M4> &skinM, std::vector<float> &outPos,
              std::vector<float> &outNormal, std::vector<float> *outTangent)
{
    outPos.resize(vertices.size() * 3);
    outNormal.resize(vertices.size() * 3);
    if (outTangent)
        outTangent->resize(vertices.size() * 4);
    for (size_t v = 0; v < vertices.size(); ++v)
    {
        const GlbVertex &gv = vertices[v];
        float p[3] = {0, 0, 0}, nn[3] = {0, 0, 0}, tt[3] = {0, 0, 0};
        for (int k = 0; k < 4; ++k)
        {
            float w = gv.weights[k];
            if (w <= 0.0f)
                continue;
            const float *m = skinM[gv.joints[k]].m;
            for (int r = 0; r < 3; ++r)
            {
                p[r] += w * (m[r] * gv.pos[0] + m[4 + r] * gv.pos[1] + m[8 + r] * gv.pos[2] + m[12 + r]);
                nn[r] += w * (m[r] * gv.normal[0] + m[4 + r] * gv.normal[1] + m[8 + r] * gv.normal[2]);
                tt[r] += w * (m[r] * gv.tangent[0] + m[4 + r] * gv.tangent[1] + m[8 + r] * gv.tangent[2]);
            }
        }
        float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (len > 1e-6f)
            for (float &c : nn)
                c /= len;
        std::memcpy(&outPos[v * 3], p, sizeof(p));
        std::memcpy(&outNormal[v * 3], nn, sizeof(nn));
        if (outTangent)
        {
            float tl = std::sqrt(tt[0] * tt[0] + tt[1] * tt[1] + tt[2] * tt[2]);
            float *o = &(*outTangent)[v * 4];
            for (int k = 0; k < 3; ++k)
                o[k] = tl > 1e-6f ? tt[k] / tl : 0.0f;
            o[3] = gv.tangent[3];
        }
    }
}

} // namespace

void GlbModel::SkinJoints(const std::vector<GlbMat34> &jointWorld, std::vector<float> &outPos,
                          std::vector<float> &outNormal, std::vector<float> *outTangent) const
{
    std::vector<M4> skinM(joints.size(), M4::Identity());
    for (size_t j = 0; j < joints.size() && j < jointWorld.size(); ++j)
    {
        M4 w = M4::Identity();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                w.m[c * 4 + r] = jointWorld[j].m[r][c];
        M4 ib;
        std::memcpy(ib.m, &inverseBind[j * 16], sizeof(ib.m));
        skinM[j] = w * ib;
    }
    SkinWith(vertices, skinM, outPos, outNormal, outTangent);
}

void GlbModel::Skin(const float curl[5], float maxDegrees, std::vector<float> &outPos,
                    std::vector<float> &outNormal, std::vector<float> *outTangent) const
{
    // Global transforms with the fingers bent. Parents come before children in these exports,
    // but do not rely on it: resolve recursively with a memo.
    std::vector<M4> global(nodes.size());
    std::vector<char> done(nodes.size(), 0);
    for (size_t start = 0; start < nodes.size(); ++start)
    {
        // Walk up to the first resolved ancestor, then resolve down.
        std::vector<int> chain;
        for (int i = (int)start; i >= 0 && !done[i]; i = nodes[i].parent)
            chain.push_back(i);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            const GlbNode &n = nodes[*it];
            float r[4];
            std::memcpy(r, n.r, sizeof(r));
            if (n.finger >= 0 && maxDegrees != 0.0f)
            {
                // Biped fingers: segment 0 runs through the palm (it barely moves), 1-3 are the
                // phalanges. The thumb's segment 0 is its metacarpal.
                static const float kFinger[4] = {0.1f, 1.0f, 1.0f, 0.8f};
                static const float kThumb[4] = {0.3f, 0.7f, 0.7f, 0.0f};
                float k = n.segment < 4 ? (n.finger == 0 ? kThumb : kFinger)[n.segment] : 0.0f;
                float a = 0.5f * curl[n.finger] * k * maxDegrees * 3.14159265f / 180.0f;
                float bend[4] = {0, 0, std::sin(a), std::cos(a)}; // about local +Z (flexion)
                QuatMul(n.r, bend, r);
            }
            M4 local = Trs(n.t, r, n.s);
            global[*it] = n.parent >= 0 ? global[n.parent] * local : local;
            done[*it] = 1;
        }
    }
    std::vector<M4> skinM(joints.size());
    for (size_t j = 0; j < joints.size(); ++j)
    {
        M4 ib;
        std::memcpy(ib.m, &inverseBind[j * 16], sizeof(ib.m));
        skinM[j] = global[joints[j]] * ib;
    }
    SkinWith(vertices, skinM, outPos, outNormal, outTangent);
}

} // namespace ivr
