#include "mathlib.h"

namespace ivr {

// Port of MatrixBuildPerspective (mathlib/vmatrix.cpp, Source SDK 2013).
static VMatrix SourceBuildPerspective(float fovX, float fovY, float zNear, float zFar)
{
    float width = 2.0f * zNear * std::tan(fovX * (kPi / 180.0f) * 0.5f);
    float height = 2.0f * zNear * std::tan(fovY * (kPi / 180.0f) * 0.5f);

    VMatrix dst{};
    dst.m[0][0] = 2.0f * zNear / width;
    dst.m[1][1] = 2.0f * zNear / height;
    dst.m[2][2] = -zFar / (zNear - zFar);
    dst.m[3][2] = 1.0f;
    dst.m[2][3] = zNear * zFar / (zNear - zFar);

    VMatrix negateXY = VMatrix::Identity();
    negateXY.m[0][0] = -1.0f;
    negateXY.m[1][1] = -1.0f;
    dst = negateXY * dst;

    VMatrix addW = VMatrix::Identity();
    addW.m[0][3] = 1.0f;
    addW.m[1][3] = 1.0f;
    addW.m[2][3] = 0.0f;
    dst = addW * dst;

    VMatrix scaleHalf = VMatrix::Identity();
    scaleHalf.m[0][0] = 0.5f;
    scaleHalf.m[1][1] = 0.5f;
    dst = scaleHalf * dst;
    return dst;
}

VMatrix BuildFlashlightWorldToShadow(const Vector &origin, const Quat &orientation, float hFovDeg, float vFovDeg,
                                     float zNear, float zFar)
{
    // BuildWorldToShadowMatrix (clientshadowmgr.cpp)
    Mat3 basis = QuatToMat3(orientation);
    Vector forward = basis.Forward(), left = basis.Left(), up = basis.Up();
    Mat3 flipped;
    flipped.SetCol(0, left);
    flipped.SetCol(1, up);
    flipped.SetCol(2, forward);
    Mat3 rt = flipped.Transposed();

    VMatrix view = VMatrix::Identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            view.m[r][c] = rt.m[r][c];
    Vector translation = rt * origin;
    view.m[0][3] = -translation.x;
    view.m[1][3] = -translation.y;
    view.m[2][3] = -translation.z;
    view.m[3][0] = view.m[3][1] = view.m[3][2] = 0.0f;
    view.m[3][3] = 1.0f;

    VMatrix perspective = SourceBuildPerspective(hFovDeg, vFovDeg, zNear, zFar);
    return perspective * view;
}

} // namespace ivr
