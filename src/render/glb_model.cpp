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
        if (prim.indices)
            for (size_t i = 0; i < prim.indices->count; ++i)
                indices.push_back((uint16_t)(base + cgltf_accessor_read_index(prim.indices, i)));
        else
            for (size_t i = 0; i < pos->count; ++i)
                indices.push_back((uint16_t)(base + i));

        if (prim.material && texRGBA.empty())
        {
            const cgltf_pbr_metallic_roughness &pbr = prim.material->pbr_metallic_roughness;
            std::memcpy(baseColor, pbr.base_color_factor, sizeof(baseColor));
            const cgltf_texture *tex = pbr.base_color_texture.texture;
            const cgltf_image *img = tex ? tex->image : nullptr;
            if (img && img->buffer_view)
            {
                const uint8_t *bytes = cgltf_buffer_view_data(img->buffer_view);
                int w = 0, h = 0, c = 0;
                uint8_t *px = bytes ? stbi_load_from_memory(bytes, (int)img->buffer_view->size, &w, &h, &c, 4) : nullptr;
                if (px)
                {
                    texWidth = w;
                    texHeight = h;
                    texRGBA.assign(px, px + (size_t)w * h * 4);
                    stbi_image_free(px);
                }
            }
        }
    }
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

void GlbModel::Skin(const float curl[5], float maxDegrees, std::vector<float> &outPos,
                    std::vector<float> &outNormal) const
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
    outPos.resize(vertices.size() * 3);
    outNormal.resize(vertices.size() * 3);
    for (size_t v = 0; v < vertices.size(); ++v)
    {
        const GlbVertex &gv = vertices[v];
        float p[3] = {0, 0, 0}, nn[3] = {0, 0, 0};
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
            }
        }
        float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (len > 1e-6f)
            for (float &c : nn)
                c /= len;
        std::memcpy(&outPos[v * 3], p, sizeof(p));
        std::memcpy(&outNormal[v * 3], nn, sizeof(nn));
    }
}

} // namespace ivr
