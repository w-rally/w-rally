#pragma once
// The game's small 3D math (phys-drive's, unchanged - the arithmetic in the same order, so the frames are the same):
// vectors, the physics' quaternions, the axis swap between the physics (z up) and the renderer (y up), the blends a
// frame between two physics steps is drawn with, and the renderer's row-major matrices (row vectors, translation in
// row 3).

#include <cmath>
#include <cstring>

namespace wrally::math {

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Norm(V3 a)
{
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 0 ? a * (1.0f / l) : a;
}
// The physics' (x, y, z) in the renderer's axes: (x, z, y).
inline V3 ToRender(V3 p) { return {p.x, p.z, p.y}; }

// A vector of the body's frame into the world's (the pose's quaternion x, y, z, w).
inline V3 Rotate(const float q[4], V3 v)
{
    const V3 u{q[0], q[1], q[2]};
    const V3 t = Cross(u, v) * 2.0f;
    return v + t * q[3] + Cross(u, t);
}

// What is drawn between two physics steps (a frame falls between them): a + (b - a) t, and for rotations the normalised
// blend along the shorter way. Equal ends give the end exactly (a resting object stays as it is).
inline void Lerp(const float* a, const float* b, float t, int n, float* out)
{
    for (int i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}
inline void Nlerp(const float a[4], const float b[4], float t, float out[4])
{
    if (!std::memcmp(a, b, 16)) {
        std::memcpy(out, b, 16);
        return;
    }
    const float sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
    float l = 0;
    for (int i = 0; i < 4; ++i) l += (out[i] = a[i] + (b[i] * sign - a[i]) * t) * out[i];
    l = l > 0 ? 1.0f / std::sqrt(l) : 1.0f;
    for (int i = 0; i < 4; ++i) out[i] *= l;
}

inline void LookAt(V3 eye, V3 at, float* m)
{
    const V3 f = Norm(at - eye), r = Norm(Cross({0, 1, 0}, f)), u = Cross(f, r);
    const V3 ax[3] = {r, u, f};
    for (int c = 0; c < 3; ++c) {
        m[0 * 4 + c] = ax[c].x;
        m[1 * 4 + c] = ax[c].y;
        m[2 * 4 + c] = ax[c].z;
        m[3 * 4 + c] = -(ax[c].x * eye.x + ax[c].y * eye.y + ax[c].z * eye.z);
    }
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

inline void Perspective(float fovY, float aspect, float zn, float zf, float* m)
{
    std::memset(m, 0, 16 * sizeof(float));
    const float ys = 1.0f / std::tan(fovY * 0.5f), q = zf / (zf - zn);
    m[0] = ys / aspect;
    m[5] = ys;
    m[10] = q;
    m[11] = 1.0f;
    m[14] = -q * zn;
}

// a then b (row vectors: a * b)
inline void Mul44(const float* a, const float* b, float* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
}

} // namespace wrally::math
