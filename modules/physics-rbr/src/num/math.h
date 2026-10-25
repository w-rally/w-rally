// The vector, matrix and quaternion primitives - bit-exact reimplementations of RichardBurnsRally_SSE.exe's (design doc
// section 1.1 for the rules), shared by the classic and the native physics.
//
// Every function mirrors the original's float operations one to one, in the same order and
// with the same operand order (the NaN payload follows the first operand), and is checked
// against the original by the oracle's fuzzer and the lab (phys/lab). The address of the
// original is part of each declaration's comment - it is the specification.
//
// Types match the game's memory layout: Vec4 is 16 bytes, unaligned (the game uses movups);
// Mat33 is 9 floats, rows of 3.
#pragma once
#include <cstdint>

namespace phys::num {

struct Vec4 {
    float x, y, z, w;
};

static_assert(sizeof(Vec4) == 16, "game layout");

// The collision code (0x4fcxxx) works on packed 12-byte vectors.
struct Vec3 {
    float x, y, z;
};

static_assert(sizeof(Vec3) == 12, "game layout");

struct Mat33 {
    float m[9];
};

static_assert(sizeof(Mat33) == 36, "game layout");

// 0x465d80  Vec4::Zero - all four lanes +0.0 (xorps).
void Vec4Zero(Vec4& v);

// 0x43c5b0  Vec4 assignment - a 16-byte copy (movups: bits, NaNs included, unchanged).
void Vec4Copy(Vec4& dst, const Vec4& src);

// 0x43c630  out.xyz = a x b; out.w is not written. All inputs are read before any store,
// so out may alias a or b.
void Cross3(const Vec4& a, const Vec4& b, Vec4& out);

// 0x473b30  |v.xyz| = sqrt((x*x + y*y) + z*z) - squares by mulps (w squared too, unused), the
// sums in this order, sqrtss (exact). The original returns it through x87 st0, which is exact
// for a float.
float Length3(const Vec4& v);

// 0x473cd0  out = s * v, all four lanes (mulps, the broadcast scalar is the first operand).
void Scale4(const Vec4& v, float s, Vec4& out);

// 0x473c90  out = (b - a) * t + a, all four lanes (subps, mulps, addps).
void Lerp4(const Vec4& a, const Vec4& b, float t, Vec4& out);

// ---- Vec3 (collision code) ------------------------------------------------------------

// 0x4fcd10  out = a - b.
void Sub3(const Vec3& a, const Vec3& b, Vec3& out);

// 0x4fccb0  out = (a + b) - c.
void AddSub3(const Vec3& a, const Vec3& b, const Vec3& c, Vec3& out);

// 0x4fcee0  (a.x*b.x + a.y*b.y) + a.z*b.z.
float Dot3(const Vec3& a, const Vec3& b);

// 0x4fcd60  out = a - b, then returns dot(out, d) - out is stored BEFORE d is read, which
// matters when d aliases out.
float SubDot3(const Vec3& a, const Vec3& b, Vec3& out, const Vec3& d);

// 0x4fcf30  out = a x b (same arithmetic as 0x43c630 on packed vectors).
void Cross3(const Vec3& a, const Vec3& b, Vec3& out);

// 0x4fcfa0  out = a x b, then returns dot(out, d) - out stored before d is read.
float CrossDot3(const Vec3& a, const Vec3& b, Vec3& out, const Vec3& d);

// 0x43c6a0  thiscall(v): v *= rsqrt((x*x + y*y) + z*z) on all four lanes (w too), with the
// rsqrtss approximation (D6). Returns v.
Vec4* Normalize3Approx4(Vec4& v);

// 0x4cdc40  thiscall(v, out): out = v * rsqrt((x*x + y*y) + z*z), all four lanes. Returns out.
Vec4* Normalize3Approx4To(const Vec4& v, Vec4& out);

// 0x483dd0  thiscall(a, b, out): quaternion product. r.xyz = a.xyz x b.xyz, r.w = 0; then
// r += a * b.w and r += b * a.w (four lanes, product first); r.w = b.w * a.w -
// ((a.z b.z + a.y b.y) + b.x a.x). out = r; returns out.
Vec4* QuatMul(const Vec4& a, const Vec4& b, Vec4& out);

// 0x4e4d10  thiscall(m, out): rotation matrix (3x3, rows of 3 floats) to quaternion. With
// tr = (m00 + m22) + m11 > 0: r = sqrt(tr + 1) (x87), w = r * 0.5, f = 0.5 / float(r), xyz from
// the off-diagonal differences * f. Else i = the largest diagonal (strict >, m11 then m22),
// j = i + 1 mod 3, k = j + 1 mod 3: r = sqrt((m_ii - (m_kk + m_jj)) + 1) (x87), q[i] = r * 0.5,
// f = float(r) == 0 ? float(r) : 0.5 / float(r); w = (m_jk - m_kj) f, q[j] = (m_ij + m_ji) f,
// q[k] = (m_ik + m_ki) f. Returns out.
Vec4* QuatFromMat33(const float* m, Vec4& out);

// 0x502570  thiscall(m, out): out = m transposed (4 x 4, words copied).
void Mat44Transpose(const float* m, float* out);

// 0x504990  thiscall(m, out): out = m^-1 by cofactors of the transpose (the scalar form of Intel's 4 x 4 inverse), then
// times 1 / det (SSE throughout, the original's operation order kept literally).
void Mat44Invert(const float* m, float* out);

// 0x48a440  cdecl(v) -> int: v > 0 ? 1 : (0 > v ? -1 : 0) (NaN: 0).
int32_t SignOf(float v);

// 0x4e0700  thiscall(v, out, s): out = s * v, four lanes (broadcast s first). Returns out.
Vec4* Scale4To(const Vec4& v, Vec4& out, float s);

// 0x52e530  cdecl(p, c): p[1] = p[0] == 0 (ordered) ? 0 : (p[3] * p[2]) / (p[0] * c).
void RatioUpdate(float* p, float c);

// 0x4fcc70  v *= s in place (v first). Returns v.
Vec3* Scale3InPlace(Vec3& v, float s);

// 0x4fcdd0  out = a + b. Returns out.
Vec3* Add3(const Vec3& a, const Vec3& b, Vec3& out);

// 0x4fd030  out = v * s (v is the first operand, unlike Scale4).
void Scale3(const Vec3& v, float s, Vec3& out);

// 0x4fd0f0  v *= rsqrtss((x*x + y*y) + z*z), in place. The original's rsqrtss is a
// vendor-specific approximation; per D6 this uses the fixed table of approx.h instead, identical
// on every machine (the oracle routes the original's rsqrtss through the same table).
void Normalize3Approx(Vec3& v);

// 0x4f5480  a = minps(a, b), all four lanes (a NaN in either lane yields b's lane).
void Min4(Vec4& a, const Vec4& b);

// 0x4f54b0  a = maxps(a, b), all four lanes (a NaN in either lane yields b's lane).
void Max4(Vec4& a, const Vec4& b);

// 0x528dd0  v[i] = (x*row0 + y*row1) + z*row2 for n vec4s, in place, packed, rows at m, m+12,
// m+24 loaded 4 wide (so the 4th lane reads one float past each row; m must have 40 bytes).
// The broadcast component is the first mulps operand. Returns v.
Vec4* RotateRows4(const float* m, Vec4* v, uint32_t n);

// 0x528b00  v[i] = v[i] + t for n vec4s, all four lanes (addps; unrolled by 4). Returns v.
Vec4* AddVec4Array(const Vec4& t, Vec4* v, uint32_t n);

// 0x528b90  thiscall(t, v, n): v[i] = v[i] - t for n vec4s, all four lanes (subps; unrolled by 4). Returns v.
Vec4* SubVec4Array(const Vec4& t, Vec4* v, uint32_t n);

// 0x528fa0  thiscall(m, v, n): v[i].xyz = M * v[i].xyz for n vec4s in place (w untouched), M's rows at m, m + 3, m + 6,
// each row (m1 y + m2 z) + m0 x in that operand order (scalar SSE).
void Mat33RotateArray(const float* m, Vec4* v, uint32_t n);

// 0x51c8c0  out = a * b, all four lanes (mulps a, b). Returns out.
Vec4* Mul4(const Vec4& a, const Vec4& b, Vec4& out);

// 0x5322e0  stdcall(a, b, c, out), m = 0 - b * c, r = (-1 / b) * a when b > 0.001 else 0:
//   a > 0:  m > a ? r : (0 > m ? 0 : c)
//   else:   a > m ? r : (m > 0 ? 0 : c)
// (all comparisons ordered: a NaN takes the else branch). The result is stored, not returned,
// so c passes through bit for bit.
void ClampedRatio(float a, float b, float c, float& out);

// 0x4e0ae0  thiscall(box, other, outMin, outMax) over vec4 pairs (min @0, max @0x10):
// Min4To(box.min, other.min, outMin), then Max4To(box.max, other.max, outMax). Returns outMax.
Vec4* Bounds4Union(const Vec4* box, const Vec4* other, Vec4& outMin, Vec4& outMax);

// 0x4e0750 / 0x4e0780  out = minps(a, b) / maxps(a, b), all four lanes. Return out.
Vec4* Min4To(const Vec4& a, const Vec4& b, Vec4& out);

Vec4* Max4To(const Vec4& a, const Vec4& b, Vec4& out);

// 0x4921a0  quaternion normalise, in place, all four lanes: v *= rsqrt(((x*x + y*y) + z*z) + w*w)
// with the D6 rsqrt (squares by mulps; the sum in this order). Returns v.
Vec4* Normalize4Approx(Vec4& v);

// 0x4e4c70  thiscall(v, out): out = v * rsqrt(((x*x + y*y) + z*z) + w*w) as Normalize4Approx, v unchanged. Returns out.
Vec4* Normalize4ApproxTo(const Vec4& v, Vec4& out);

// 0x4a19e0  thiscall(a, out, b): out.xyz = b + a (per lane, b first); out.w is the original's uninitialised stack (0
// here). Returns out.
Vec4* AddXyz4(const Vec4& a, Vec4& out, const Vec4& b);

// 0x4f3d40  thiscall(a, b, out): out = (a x b) * rsqrt((x x + y y) + z z) of the cross product, all four lanes (packed:
// a * b.yzx - b * a.yzx, then .yzx; so w = a.w b.w - b.w a.w, 0 unless infinite / NaN), rsqrtss as RsqrtApprox. Returns out.
Vec4* CrossNormalize4Approx(const Vec4& a, const Vec4& b, Vec4& out);

// 0x465da0  out.xyz = a.xyz - b.xyz. The original copies out.w from an uninitialised stack
// slot - not reproducible; this writes +0 and the lane is a "don't care" (w-lane rule).
Vec4* Sub3W(const Vec4& a, const Vec4& b, Vec4& out);

// 0x483a90  m = 0 (all nine +0.0). Returns m.
Mat33& Mat33Zero(Mat33& m);

// 0x528c20  out = b * a (out[i][j] = sum_k b[i][k] * a[k][j]), each of the nine sums in the
// original's own operand order. `a` is `this` in the original. Returns out.
Mat33& Mat33Mul(const Mat33& a, Mat33& out, const Mat33& b);

// Result of BracketSearch: table[i0] <= x <= table[i1], t = position between them.
struct Bracket {
    int32_t i0, i1;
    float t;
};

// 0x528980  cdecl: locate x in a sorted table of n floats, starting from the previous result's
// i0 (a hint), walking up or down. Above the last entry: {n-2, n-1, 1}; at/below the first:
// {0, 1, 0}; else t = (x - table[i0]) / (table[i1] - table[i0]). NaN behaviour follows the
// original's jumps (a NaN x takes the downward branch and stops at once - reading table[hint-1]).
void BracketSearch(const float* table, float x, uint32_t n, Bracket* res);

// 0x52e3a0  cdecl: *acc = f / (m[1] * m[0]) + *acc  (SSE).
void AddDivProduct(const float* m, float f, float* acc);

// 0x502400  thiscall(m, v, out): out = ((r0 * v.x + r1 * v.y) + r2 * v.z) + r3 * v.w, rows r0..r3 =
// m[0..15] (packed SSE, per lane). Returns out.
Vec4* Mat44MulVec4(const float* m, const Vec4* v, Vec4* out);

// Thermal lumps (the helpers around AddDivProduct): a state {mass, temperature}, a definition
// whose first float is the specific heat.
// 0x52e3d0  cdecl(def, q, s): s[1] = q / (def[0] * s[0]) + s[1]  - heat q into lump s (SSE).
void AddHeatToLump(const float* def, float q, float* s);

// 0x52e460  cdecl(a, b): mix lump a into b. m = a[0] + b[0]; m > 1e-6: b[1] = (a[1] * a[0] + b[1] * b[0]) / m;
// otherwise b[1] = a[1] if a[0] > b[0]. Then b[0] = m.
void MixLumps(const float* a, float* b);

// 0x52e400  cdecl(def, s) -> float (st0): boil-off above def[3]. s[1] > def[3]: e = (((s[1] - def[3]) *
// def[0]) * s[0]) / def[1], s[1] = def[3], s[0] -= e; s[0] < 0: returns s[0] + e and s[0] = 0, else e.
// Not above: 0.
float BoilOffLump(const float* def, float* s);

// 0x52e4c0  cdecl(p, k): p[3] = p[2] == 0 ? +0 : ((p[1] / p[2]) * p[0]) * k  (ucomiss: NaN p[2] computes).
void RateTimes(float* p, float k);

// 0x43c6e0  rotation matrix of quaternion q (x, y, z, w), no normalisation:
//   m[0] = 1-2(zz+yy)  m[1] = 2(zw+xy)    m[2] = 2(xz-yw)
//   m[3] = 2(xy-zw)    m[4] = 1-2(xx+zz)  m[5] = 2(xw+yz)
//   m[6] = 2(yw+xz)    m[7] = 2(yz-xw)    m[8] = 1-2(xx+yy)
void Mat33FromQuat(Mat33& out, const Vec4& q);

// 0x4e07b0  thiscall(v): 1 if 0 > x, 0 > y and 0 > z (ordered), else 0.
int AllNegative3(const float* v);

} // namespace phys::num
