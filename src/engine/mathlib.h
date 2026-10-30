// INFRA VR - small math library compatible with Source engine conventions.
//
// Source world space: +X forward, +Y left, +Z up, 1 unit = 1 inch.
// Angles (QAngle): x = pitch (positive looks DOWN), y = yaw, z = roll, degrees.
// Rotation matrices store the basis vectors in columns: forward, left, up.
#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>

namespace ivr {

constexpr float kPi = 3.14159265358979323846f;
inline float Deg2Rad(float d) { return d * (kPi / 180.0f); }
inline float Rad2Deg(float r) { return r * (180.0f / kPi); }

struct Vector
{
    float x = 0, y = 0, z = 0;
    Vector() = default;
    constexpr Vector(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vector operator+(const Vector &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector operator-(const Vector &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector operator*(float s) const { return {x * s, y * s, z * s}; }
    Vector operator/(float s) const { return {x / s, y / s, z / s}; }
    Vector operator-() const { return {-x, -y, -z}; }
    Vector &operator+=(const Vector &o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vector &operator-=(const Vector &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vector &operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float operator[](int i) const { return (&x)[i]; }
    float &operator[](int i) { return (&x)[i]; }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    float Length2D() const { return std::sqrt(x * x + y * y); }
    Vector Normalized() const
    {
        float l = Length();
        return l > 1e-6f ? *this / l : Vector(0, 0, 0);
    }
};

inline float Dot(const Vector &a, const Vector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vector Cross(const Vector &a, const Vector &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vector Lerp(const Vector &a, const Vector &b, float t) { return a + (b - a) * t; }

struct QAngle
{
    float x = 0, y = 0, z = 0; // pitch, yaw, roll
    QAngle() = default;
    constexpr QAngle(float p, float yw, float r) : x(p), y(yw), z(r) {}
};

inline float NormalizeYaw(float a)
{
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0)
        a += 360.0f;
    return a - 180.0f;
}

// 3x3 rotation; m[row][col]. Column 0 = forward, 1 = left, 2 = up.
struct Mat3
{
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    static Mat3 Identity() { return Mat3(); }
    Vector Col(int c) const { return {m[0][c], m[1][c], m[2][c]}; }
    void SetCol(int c, const Vector &v) { m[0][c] = v.x; m[1][c] = v.y; m[2][c] = v.z; }
    Vector Forward() const { return Col(0); }
    Vector Left() const { return Col(1); }
    Vector Up() const { return Col(2); }

    Vector operator*(const Vector &v) const
    {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 operator*(const Mat3 &o) const
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    Mat3 Transposed() const
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[j][i];
        return r;
    }
};

inline Mat3 RotationZ(float yawDeg)
{
    float s = std::sin(Deg2Rad(yawDeg)), c = std::cos(Deg2Rad(yawDeg));
    Mat3 r;
    r.m[0][0] = c; r.m[0][1] = -s; r.m[0][2] = 0;
    r.m[1][0] = s; r.m[1][1] = c;  r.m[1][2] = 0;
    r.m[2][0] = 0; r.m[2][1] = 0;  r.m[2][2] = 1;
    return r;
}

// Equivalent of Source's AngleMatrix (rotation part).
inline Mat3 AnglesToMatrix(const QAngle &a)
{
    float sy = std::sin(Deg2Rad(a.y)), cy = std::cos(Deg2Rad(a.y));
    float sp = std::sin(Deg2Rad(a.x)), cp = std::cos(Deg2Rad(a.x));
    float sr = std::sin(Deg2Rad(a.z)), cr = std::cos(Deg2Rad(a.z));
    Mat3 r;
    // forward
    r.m[0][0] = cp * cy;
    r.m[1][0] = cp * sy;
    r.m[2][0] = -sp;
    // left
    r.m[0][1] = sr * sp * cy + cr * -sy;
    r.m[1][1] = sr * sp * sy + cr * cy;
    r.m[2][1] = sr * cp;
    // up
    r.m[0][2] = (cr * sp * cy + -sr * -sy);
    r.m[1][2] = (cr * sp * sy + -sr * cy);
    r.m[2][2] = cr * cp;
    return r;
}

// Equivalent of Source's MatrixAngles.
inline QAngle MatrixToAngles(const Mat3 &m)
{
    Vector forward = m.Col(0), left = m.Col(1), up = m.Col(2);
    float xyDist = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    QAngle a;
    if (xyDist > 0.001f)
    {
        a.y = Rad2Deg(std::atan2(forward.y, forward.x));
        a.x = Rad2Deg(std::atan2(-forward.z, xyDist));
        a.z = Rad2Deg(std::atan2(left.z, up.z));
    }
    else
    {
        a.y = Rad2Deg(std::atan2(-left.x, left.y));
        a.x = Rad2Deg(std::atan2(-forward.z, xyDist));
        a.z = 0;
    }
    return a;
}

inline Vector AngleForward(const QAngle &a) { return AnglesToMatrix(a).Forward(); }

inline QAngle VectorAngles(const Vector &forward)
{
    QAngle a;
    if (std::fabs(forward.x) < 1e-6f && std::fabs(forward.y) < 1e-6f)
    {
        a.y = 0;
        a.x = forward.z > 0 ? 270.0f : 90.0f;
    }
    else
    {
        a.y = Rad2Deg(std::atan2(forward.y, forward.x));
        float tmp = std::sqrt(forward.x * forward.x + forward.y * forward.y);
        a.x = Rad2Deg(std::atan2(-forward.z, tmp));
    }
    if (a.x > 180.0f)
        a.x -= 360.0f;
    return a;
}

// Yaw (degrees, Source convention) of a rotation, ignoring pitch/roll.
inline float YawOf(const Mat3 &m)
{
    Vector f = m.Forward();
    if (std::fabs(f.x) + std::fabs(f.y) < 0.2f)
    {
        // looking nearly straight up/down: use the up vector (or its opposite)
        Vector u = m.Up() * (f.z > 0 ? -1.0f : 1.0f);
        return Rad2Deg(std::atan2(u.y, u.x));
    }
    return Rad2Deg(std::atan2(f.y, f.x));
}

struct Quat
{
    float x = 0, y = 0, z = 0, w = 1;
};

inline Mat3 QuatToMat3(const Quat &q)
{
    Mat3 r;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.m[0][0] = 1 - 2 * (yy + zz); r.m[0][1] = 2 * (xy - wz);     r.m[0][2] = 2 * (xz + wy);
    r.m[1][0] = 2 * (xy + wz);     r.m[1][1] = 1 - 2 * (xx + zz); r.m[1][2] = 2 * (yz - wx);
    r.m[2][0] = 2 * (xz - wy);     r.m[2][1] = 2 * (yz + wx);     r.m[2][2] = 1 - 2 * (xx + yy);
    return r;
}

inline Quat Mat3ToQuat(const Mat3 &m)
{
    Quat q;
    float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0)
    {
        float s = std::sqrt(trace + 1.0f) * 2;
        q.w = 0.25f * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    }
    else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2])
    {
        float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    }
    else if (m.m[1][1] > m.m[2][2])
    {
        float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    }
    else
    {
        float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return q;
}

// Axis-angle (radians) of a rotation matrix. Returns a zero vector for identity.
inline Vector RotationToAxisAngle(const Mat3 &m)
{
    Quat q = Mat3ToQuat(m);
    if (q.w < 0)
    {
        q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
    }
    float sinHalf = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (sinHalf < 1e-6f)
        return {0, 0, 0};
    float angle = 2.0f * std::atan2(sinHalf, q.w);
    return Vector(q.x, q.y, q.z) * (angle / sinHalf);
}

// Rigid transform: world = rot * local + pos.
struct Pose
{
    Vector pos;
    Mat3 rot;

    Pose operator*(const Pose &child) const { return {rot * child.pos + pos, rot * child.rot}; }
    Vector Transform(const Vector &p) const { return rot * p + pos; }
    Pose Inverse() const
    {
        Mat3 rt = rot.Transposed();
        return {rt * -pos, rt};
    }
    QAngle Angles() const { return MatrixToAngles(rot); }
    static Pose FromOriginAngles(const Vector &o, const QAngle &a) { return {o, AnglesToMatrix(a)}; }
};

// Source-compatible memory layouts ------------------------------------------------
struct matrix3x4_t
{
    float m[3][4];
    Pose ToPose() const
    {
        Pose p;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                p.rot.m[r][c] = m[r][c];
        p.pos = {m[0][3], m[1][3], m[2][3]};
        return p;
    }
    static matrix3x4_t FromPose(const Pose &p)
    {
        matrix3x4_t o;
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                o.m[r][c] = p.rot.m[r][c];
            o.m[r][3] = p.pos[r];
        }
        return o;
    }
};

struct VMatrix
{
    float m[4][4];
    static VMatrix Identity()
    {
        VMatrix v{};
        v.m[0][0] = v.m[1][1] = v.m[2][2] = v.m[3][3] = 1.0f;
        return v;
    }
    VMatrix operator*(const VMatrix &o) const
    {
        VMatrix r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }
};

// Source-style off-axis projection (view space: x right, y up, looking down -z;
// clip depth 0..1). Tangents are signed: left/down are negative.
inline VMatrix BuildProjection(float tanLeft, float tanRight, float tanDown, float tanUp, float zNear, float zFar)
{
    VMatrix p{};
    p.m[0][0] = 2.0f / (tanRight - tanLeft);
    p.m[0][2] = (tanRight + tanLeft) / (tanRight - tanLeft);
    p.m[1][1] = 2.0f / (tanUp - tanDown);
    p.m[1][2] = (tanUp + tanDown) / (tanUp - tanDown);
    p.m[2][2] = zFar / (zNear - zFar);
    p.m[2][3] = zNear * zFar / (zNear - zFar);
    p.m[3][2] = -1.0f;
    return p;
}

// Port of CClientShadowMgr::BuildPerspectiveWorldToFlashlightMatrix.
VMatrix BuildFlashlightWorldToShadow(const Vector &origin, const Quat &orientation, float hFovDeg, float vFovDeg,
                                     float zNear, float zFar);

} // namespace ivr
