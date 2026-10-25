#include "math.h"

#include <cstring>

#include "f32.h"
#include "x87.h"   // jmp:: branch helpers
#include "warn.h"
#include "x87math.h"

#include <xmmintrin.h>

// All arithmetic is F32 (f32.h): one scalar SSE instruction per operation, never x87, on every
// target. Expressions keep the original's evaluation and operand order.

namespace phys::num {

namespace {
struct V3 {
    F32 x, y, z;
};
V3 Load(const Vec3& v) { return {v.x, v.y, v.z}; }
V3 Load3(const Vec4& v) { return {v.x, v.y, v.z}; }
void Store(Vec3& o, const V3& r) { o.x = r.x.f(); o.y = r.y.f(); o.z = r.z.f(); }
F32 DotXYZ(const V3& a, const V3& b) { return (a.x * b.x + a.y * b.y) + a.z * b.z; }
V3 CrossXYZ(const V3& a, const V3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
} // namespace

void Vec4Zero(Vec4& v)
{
    v = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
}

void Vec4Copy(Vec4& dst, const Vec4& src)
{
    std::memcpy(&dst, &src, sizeof dst);   // a move, not float arithmetic: NaN bits survive
}

void Cross3(const Vec4& a, const Vec4& b, Vec4& out)
{
    const V3 r = CrossXYZ(Load3(a), Load3(b));   // all inputs read before any store
    out.x = r.x.f();
    out.y = r.y.f();
    out.z = r.z.f();
}

float Length3(const Vec4& v)
{
    const V3 a = Load3(v);
    const F32 xx = a.x * a.x, yy = a.y * a.y, zz = a.z * a.z;
    return Sqrt((xx + yy) + zz).f();
}

void Scale4(const Vec4& v, float s, Vec4& out)
{
    const F32 k = s;
    const Vec4 r{(k * F32(v.x)).f(), (k * F32(v.y)).f(), (k * F32(v.z)).f(), (k * F32(v.w)).f()};
    out = r;
}

void Lerp4(const Vec4& a, const Vec4& b, float t, Vec4& out)
{
    const F32 k = t;
    auto lerp = [&](float pa, float pb) { return ((F32(pb) - F32(pa)) * k + F32(pa)).f(); };
    const Vec4 r{lerp(a.x, b.x), lerp(a.y, b.y), lerp(a.z, b.z), lerp(a.w, b.w)};
    out = r;
}

void Sub3(const Vec3& a, const Vec3& b, Vec3& out)
{
    const V3 x = Load(a), y = Load(b);
    Store(out, {x.x - y.x, x.y - y.y, x.z - y.z});
}

void AddSub3(const Vec3& a, const Vec3& b, const Vec3& c, Vec3& out)
{
    const V3 x = Load(a), y = Load(b), z = Load(c);
    Store(out, {(x.x + y.x) - z.x, (x.y + y.y) - z.y, (x.z + y.z) - z.z});
}

float Dot3(const Vec3& a, const Vec3& b)
{
    return DotXYZ(Load(a), Load(b)).f();
}

float SubDot3(const Vec3& a, const Vec3& b, Vec3& out, const Vec3& d)
{
    const V3 x = Load(a), y = Load(b);
    const V3 r{x.x - y.x, x.y - y.y, x.z - y.z};
    Store(out, r);
    return DotXYZ(r, Load(d)).f();   // d read after the store, as in the original
}

void Cross3(const Vec3& a, const Vec3& b, Vec3& out)
{
    Store(out, CrossXYZ(Load(a), Load(b)));
}

float CrossDot3(const Vec3& a, const Vec3& b, Vec3& out, const Vec3& d)
{
    const V3 r = CrossXYZ(Load(a), Load(b));
    Store(out, r);
    return DotXYZ(r, Load(d)).f();
}

Vec4* Normalize3Approx4(Vec4& v)
{
    const F32x4 p = F32x4::Load(&v.x);
    const F32x4 sq = p * p;
    const F32 inv = RsqrtApprox((sq.v[0] + sq.v[1]) + sq.v[2]);
    (p * F32x4::Splat(inv)).Store(&v.x);
    return &v;
}

Vec4* Normalize3Approx4To(const Vec4& v, Vec4& out)
{
    const F32x4 p = F32x4::Load(&v.x);
    const F32x4 sq = p * p;
    const F32 inv = RsqrtApprox((sq.v[0] + sq.v[1]) + sq.v[2]);
    (p * F32x4::Splat(inv)).Store(&out.x);
    return &out;
}

Vec4* QuatMul(const Vec4& a, const Vec4& b, Vec4& out)
{
    F32x4 r{{F32(a.y) * F32(b.z) - F32(a.z) * F32(b.y), F32(a.z) * F32(b.x) - F32(a.x) * F32(b.z),
             F32(a.x) * F32(b.y) - F32(a.y) * F32(b.x), F32(0.0f)}};
    r = F32x4::Load(&a.x) * F32x4::Splat(F32(b.w)) + r;
    r = F32x4::Load(&b.x) * F32x4::Splat(F32(a.w)) + r;
    r.v[3] = F32(b.w) * F32(a.w) - ((F32(a.z) * F32(b.z) + F32(a.y) * F32(b.y)) + F32(b.x) * F32(a.x));
    r.Store(&out.x);
    return &out;
}

Vec4* QuatFromMat33(const float* m, Vec4& out)
{
    const float tr = ((F32(m[0]) + F32(m[8])) + F32(m[4])).f();
    if (!jmp::BE(tr, 0.0f)) {                       // comiss tr, 0; jbe
        const X87 r = Sqrt(X87(tr) + X87(1.0f));    // fld; fadd 1; fsqrt; fst s
        const float s = r.Store();
        const float f = (F32(0.5f) / F32(s)).f();
        out.w = (r * X87(0.5f)).Store();
        out.x = ((F32(m[5]) - F32(m[7])) * F32(f)).f();
        out.y = ((F32(m[6]) - F32(m[2])) * F32(f)).f();
        out.z = ((F32(m[1]) - F32(m[3])) * F32(f)).f();
        return &out;
    }
    uint32_t i = 0;
    if (!jmp::BE(m[4], m[0])) i = 1;
    if (!jmp::BE(m[8], m[i * 4])) i = 2;
    const uint32_t j = i + 1 > 2 ? 0 : i + 1, k = j + 1 > 2 ? 0 : j + 1;
    const X87 r = Sqrt((X87(m[i * 4]) - (X87(m[k * 4]) + X87(m[j * 4]))) + X87(1.0f));
    const float s = r.Store();
    float* q = &out.x;
    q[i] = (r * X87(0.5f)).Store();
    // fld 0; fld s; fucomip; lahf; test ah, 0x44; jnp: s itself when equal and ordered.
    const float f = jmp::E(s, 0.0f) && !jmp::P(s, 0.0f) ? s : (F32(0.5f) / F32(s)).f();
    q[3] = ((F32(m[j * 3 + k]) - F32(m[k * 3 + j])) * F32(f)).f();
    q[j] = ((F32(m[i * 3 + j]) + F32(m[j * 3 + i])) * F32(f)).f();
    q[k] = ((F32(m[i * 3 + k]) + F32(m[k * 3 + i])) * F32(f)).f();
    return &out;
}

void Mat44Transpose(const float* m, float* out)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) std::memcpy(&out[4 * i + j], &m[4 * j + i], 4);
}

void Mat44Invert(const float* m, float* outF)
{
    uint8_t* const out = reinterpret_cast<uint8_t*>(outF);
    auto Get = [](const uint8_t* p, uint32_t o) {
        float v;
        std::memcpy(&v, p + o, 4);
        return v;
    };
    auto Put = [](uint8_t* p, uint32_t o, float v) { std::memcpy(p + o, &v, 4); };
    float s[28] = {};   // the original's frame (0x70 bytes); the transpose at +0x30
    Mat44Transpose(m, &s[12]);
    F32 x0 = 0.0f, x1 = 0.0f, x2 = 0.0f, x3 = 0.0f, x4 = 0.0f, x5 = 0.0f, x6 = 0.0f, x7 = 0.0f;
    // Translated one instruction per line from the disassembly (cofactors, then 1 / det in x1).
    x7 = F32(s[23]);   // 5049a3
    x1 = F32(s[19]);   // 5049a9
    x2 = F32(s[26]);   // 5049af
    x6 = F32(s[27]);   // 5049b5
    x5 = F32(s[18]);   // 5049bb
    x4 = x7;   // 5049c1
    x5 = x5 * x6;   // 5049c4
    x0 = x6;   // 5049c8
    x0 = x0 * F32(s[22]);   // 5049cb
    x4 = x4 * x2;   // 5049d1
    x3 = x1;   // 5049d5
    x3 = x3 * x2;   // 5049d8
    x2 = F32(s[18]);   // 5049dc
    x2 = x2 * x7;   // 5049e2
    s[4] = x2.f();   // 5049e6
    x2 = x1;   // 5049ef
    x2 = x2 * F32(s[22]);   // 5049f2
    s[5] = x2.f();   // 5049f8
    x2 = F32(s[14]);   // 5049fe
    x7 = x2;   // 504a04
    x7 = x7 * x6;   // 504a07
    x6 = F32(s[15]);   // 504a0b
    x6 = x6 * F32(s[26]);   // 504a11
    s[7] = x6.f();   // 504a17
    x6 = x2;   // 504a1d
    x6 = x6 * F32(s[23]);   // 504a20
    s[8] = x6.f();   // 504a26
    x6 = F32(s[15]);   // 504a2c
    x6 = x6 * F32(s[22]);   // 504a32
    s[9] = x6.f();   // 504a38
    s[6] = x7.f();   // 504a3e
    x7 = F32(s[21]);   // 504a44
    x7 = x7 * x3;   // 504a4a
    s[3] = x3.f();   // 504a4e
    x3 = F32(s[17]);   // 504a54
    x3 = x3 * x0;   // 504a5a
    x6 = x2;   // 504a5e
    x6 = x6 * x1;   // 504a61
    s[10] = x6.f();   // 504a65
    x6 = F32(s[15]);   // 504a6b
    x6 = x6 * F32(s[18]);   // 504a71
    s[11] = x6.f();   // 504a77
    x6 = F32(s[25]);   // 504a7d
    x6 = x6 * F32(s[4]);   // 504a83
    x6 = x6 + x7;   // 504a89
    x7 = F32(s[21]);   // 504a8d
    x6 = x6 + x3;   // 504a93
    s[0] = x0.f();   // 504a97
    x0 = F32(s[25]);   // 504a9c
    x7 = x7 * x5;   // 504aa2
    x3 = x0;   // 504aa6
    x3 = x3 * F32(s[5]);   // 504aa9
    x3 = x3 + x7;   // 504aaf
    x7 = F32(s[17]);   // 504ab3
    x7 = x7 * x4;   // 504ab9
    x3 = x3 + x7;   // 504abd
    x6 = x6 - x3;   // 504ac1
    x3 = F32(s[21]);   // 504ac5
    Put(out, 0x0, x6.f());   // 504acb
    x6 = x0;   // 504acf
    x6 = x6 * F32(s[9]);   // 504ad2
    x0 = x0 * F32(s[8]);   // 504ad8
    x7 = x3;   // 504ade
    x7 = x7 * F32(s[6]);   // 504ae1
    x3 = x3 * F32(s[7]);   // 504ae7
    x6 = x6 + x7;   // 504aed
    x7 = F32(s[13]);   // 504af1
    x0 = x0 + x3;   // 504af7
    x3 = F32(s[13]);   // 504afb
    x3 = x3 * F32(s[0]);   // 504b01
    x7 = x7 * x4;   // 504b06
    x6 = x6 + x7;   // 504b0a
    x0 = x0 + x3;   // 504b0e
    x6 = x6 - x0;   // 504b12
    s[1] = x4.f();   // 504b16
    s[2] = x5.f();   // 504b1c
    Put(out, 0x10, x6.f());   // 504b22
    x0 = F32(s[25]);   // 504b27
    x3 = F32(s[17]);   // 504b2d
    x6 = x0;   // 504b33
    x6 = x6 * F32(s[10]);   // 504b36
    x0 = x0 * F32(s[11]);   // 504b3c
    x7 = x3;   // 504b42
    x7 = x7 * F32(s[7]);   // 504b45
    x3 = x3 * F32(s[6]);   // 504b4b
    x6 = x6 + x7;   // 504b51
    x7 = F32(s[13]);   // 504b55
    x7 = x7 * x5;   // 504b5b
    x6 = x6 + x7;   // 504b5f
    x0 = x0 + x3;   // 504b63
    x3 = F32(s[13]);   // 504b67
    x3 = x3 * F32(s[3]);   // 504b6d
    x0 = x0 + x3;   // 504b73
    x3 = F32(s[17]);   // 504b77
    x6 = x6 - x0;   // 504b7d
    x0 = F32(s[21]);   // 504b81
    Put(out, 0x20, x6.f());   // 504b87
    x6 = x0;   // 504b8c
    x6 = x6 * F32(s[11]);   // 504b8f
    x0 = x0 * F32(s[10]);   // 504b95
    x7 = x3;   // 504b9b
    x7 = x7 * F32(s[8]);   // 504b9e
    x3 = x3 * F32(s[9]);   // 504ba4
    x6 = x6 + x7;   // 504baa
    x7 = F32(s[13]);   // 504bae
    x7 = x7 * F32(s[5]);   // 504bb4
    x6 = x6 + x7;   // 504bba
    x0 = x0 + x3;   // 504bbe
    x3 = F32(s[13]);   // 504bc2
    x3 = x3 * F32(s[4]);   // 504bc8
    x0 = x0 + x3;   // 504bce
    x3 = F32(s[24]);   // 504bd2
    x6 = x6 - x0;   // 504bd8
    x0 = F32(s[20]);   // 504bdc
    Put(out, 0x30, x6.f());   // 504be2
    x7 = x0;   // 504be7
    x7 = x7 * x5;   // 504bea
    x5 = F32(s[16]);   // 504bee
    x5 = x5 * x4;   // 504bf4
    x6 = x3;   // 504bf8
    x6 = x6 * F32(s[5]);   // 504bfb
    x6 = x6 + x7;   // 504c01
    x7 = F32(s[16]);   // 504c05
    x6 = x6 + x5;   // 504c0b
    x5 = x3;   // 504c0f
    x5 = x5 * F32(s[4]);   // 504c12
    x4 = x0;   // 504c18
    x4 = x4 * F32(s[3]);   // 504c1b
    x5 = x5 + x4;   // 504c21
    x4 = F32(s[0]);   // 504c25
    x7 = x7 * x4;   // 504c2a
    x5 = x5 + x7;   // 504c2e
    x6 = x6 - x5;   // 504c32
    Put(out, 0x4, x6.f());   // 504c36
    x5 = x3;   // 504c3b
    x5 = x5 * F32(s[8]);   // 504c3e
    x6 = x0;   // 504c44
    x6 = x6 * F32(s[7]);   // 504c47
    x5 = x5 + x6;   // 504c4d
    x6 = F32(s[12]);   // 504c51
    x6 = x6 * x4;   // 504c57
    x5 = x5 + x6;   // 504c5b
    x4 = x3;   // 504c5f
    x4 = x4 * F32(s[9]);   // 504c62
    x6 = x0;   // 504c68
    x6 = x6 * F32(s[6]);   // 504c6b
    x4 = x4 + x6;   // 504c71
    x6 = F32(s[12]);   // 504c75
    x6 = x6 * F32(s[1]);   // 504c7b
    x4 = x4 + x6;   // 504c81
    x5 = x5 - x4;   // 504c85
    x4 = F32(s[16]);   // 504c89
    Put(out, 0x14, x5.f());   // 504c8f
    x5 = x3;   // 504c94
    x5 = x5 * F32(s[11]);   // 504c97
    x6 = x4;   // 504c9d
    x6 = x6 * F32(s[6]);   // 504ca0
    x5 = x5 + x6;   // 504ca6
    x4 = x4 * F32(s[7]);   // 504caa
    x6 = F32(s[12]);   // 504cb0
    x6 = x6 * F32(s[3]);   // 504cb6
    x5 = x5 + x6;   // 504cbc
    x7 = F32(s[25]);   // 504cc0
    x6 = x3;   // 504cc6
    x6 = x6 * F32(s[10]);   // 504cc9
    x6 = x6 + x4;   // 504ccf
    x4 = F32(s[12]);   // 504cd3
    x4 = x4 * F32(s[2]);   // 504cd9
    x6 = x6 + x4;   // 504cdf
    x5 = x5 - x6;   // 504ce3
    x4 = F32(s[16]);   // 504ce7
    x6 = x4;   // 504ced
    x6 = x6 * F32(s[9]);   // 504cf0
    x4 = x4 * F32(s[8]);   // 504cf6
    Put(out, 0x24, x5.f());   // 504cfc
    x5 = x0;   // 504d01
    x5 = x5 * F32(s[10]);   // 504d04
    x5 = x5 + x6;   // 504d0a
    x6 = F32(s[12]);   // 504d0e
    x6 = x6 * F32(s[4]);   // 504d14
    x5 = x5 + x6;   // 504d1a
    x6 = x0;   // 504d1e
    x6 = x6 * F32(s[11]);   // 504d21
    x6 = x6 + x4;   // 504d27
    x4 = F32(s[12]);   // 504d2b
    x4 = x4 * F32(s[5]);   // 504d31
    x6 = x6 + x4;   // 504d37
    x5 = x5 - x6;   // 504d3b
    x6 = F32(s[16]);   // 504d3f
    x6 = x6 * x7;   // 504d45
    s[2] = x6.f();   // 504d49
    x6 = x3;   // 504d4f
    x6 = x6 * F32(s[17]);   // 504d52
    s[3] = x6.f();   // 504d58
    x6 = F32(s[16]);   // 504d5e
    x6 = x6 * F32(s[21]);   // 504d64
    s[4] = x6.f();   // 504d6a
    x6 = x0;   // 504d70
    x6 = x6 * F32(s[17]);   // 504d73
    s[5] = x6.f();   // 504d79
    x6 = F32(s[12]);   // 504d7f
    x6 = x6 * x7;   // 504d85
    s[6] = x6.f();   // 504d89
    x6 = x3;   // 504d8f
    x6 = x6 * F32(s[13]);   // 504d92
    s[7] = x6.f();   // 504d98
    x6 = F32(s[12]);   // 504d9e
    x6 = x6 * F32(s[21]);   // 504da4
    s[8] = x6.f();   // 504daa
    x6 = x0;   // 504db0
    x6 = x6 * F32(s[13]);   // 504db3
    s[9] = x6.f();   // 504db9
    x6 = F32(s[12]);   // 504dbf
    x6 = x6 * F32(s[17]);   // 504dc5
    s[10] = x6.f();   // 504dcb
    x6 = F32(s[16]);   // 504dd1
    x6 = x6 * F32(s[13]);   // 504dd7
    s[11] = x6.f();   // 504ddd
    x4 = x0;   // 504de3
    x4 = x4 * x7;   // 504de6
    s[0] = x4.f();   // 504dea
    x6 = x1;   // 504def
    x6 = x6 * x4;   // 504df2
    x4 = F32(s[23]);   // 504df6
    x4 = x4 * F32(s[3]);   // 504dfc
    x6 = x6 + x4;   // 504e02
    x4 = F32(s[27]);   // 504e06
    x4 = x4 * F32(s[4]);   // 504e0c
    Put(out, 0x34, x5.f());   // 504e12
    x6 = x6 + x4;   // 504e17
    x5 = x3;   // 504e1b
    x5 = x5 * F32(s[21]);   // 504e1e
    s[1] = x5.f();   // 504e24
    x4 = x1;   // 504e2a
    x4 = x4 * x5;   // 504e2d
    x5 = F32(s[27]);   // 504e31
    x5 = x5 * F32(s[5]);   // 504e37
    x4 = x4 + x5;   // 504e40
    x5 = F32(s[23]);   // 504e44
    x5 = x5 * F32(s[2]);   // 504e4a
    x4 = x4 + x5;   // 504e50
    x6 = x6 - x4;   // 504e54
    x5 = F32(s[15]);   // 504e58
    x4 = F32(s[27]);   // 504e5e
    x4 = x4 * F32(s[9]);   // 504e64
    Put(out, 0x8, x6.f());   // 504e6a
    x6 = x5;   // 504e6e
    x6 = x6 * F32(s[1]);   // 504e71
    x5 = x5 * F32(s[0]);   // 504e77
    x6 = x6 + x4;   // 504e7c
    x4 = F32(s[23]);   // 504e80
    x7 = x4;   // 504e86
    x4 = x4 * F32(s[7]);   // 504e89
    x7 = x7 * F32(s[6]);   // 504e8f
    x5 = x5 + x4;   // 504e95
    x4 = F32(s[27]);   // 504e99
    x4 = x4 * F32(s[8]);   // 504e9f
    x6 = x6 + x7;   // 504ea5
    x5 = x5 + x4;   // 504ea9
    x4 = F32(s[15]);   // 504ead
    x6 = x6 - x5;   // 504eb3
    Put(out, 0x18, x6.f());   // 504eb7
    x5 = x4;   // 504ebc
    x5 = x5 * F32(s[2]);   // 504ebf
    x4 = x4 * F32(s[3]);   // 504ec5
    x6 = x1;   // 504ecb
    x6 = x6 * F32(s[7]);   // 504ece
    x6 = x6 + x5;   // 504ed4
    x5 = F32(s[27]);   // 504ed8
    x5 = x5 * F32(s[10]);   // 504ede
    x6 = x6 + x5;   // 504ee4
    x5 = F32(s[11]);   // 504ee8
    x7 = x1;   // 504eee
    x7 = x7 * F32(s[6]);   // 504ef1
    x7 = x7 + x4;   // 504ef7
    x4 = F32(s[27]);   // 504efb
    x4 = x4 * x5;   // 504f01
    x7 = x7 + x4;   // 504f05
    x4 = F32(s[15]);   // 504f09
    x4 = x4 * F32(s[5]);   // 504f0f
    x6 = x6 - x7;   // 504f15
    Put(out, 0x28, x6.f());   // 504f19
    x6 = x1;   // 504f1e
    x6 = x6 * F32(s[8]);   // 504f21
    x1 = x1 * F32(s[9]);   // 504f27
    x6 = x6 + x4;   // 504f2d
    x4 = F32(s[23]);   // 504f31
    x7 = x4;   // 504f37
    x4 = x4 * F32(s[10]);   // 504f3a
    x7 = x7 * x5;   // 504f40
    x5 = F32(s[15]);   // 504f44
    x5 = x5 * F32(s[4]);   // 504f4a
    x6 = x6 + x7;   // 504f50
    x1 = x1 + x5;   // 504f54
    x5 = F32(s[18]);   // 504f58
    x1 = x1 + x4;   // 504f5e
    x4 = F32(s[26]);   // 504f62
    x6 = x6 - x1;   // 504f68
    Put(out, 0x38, x6.f());   // 504f6c
    x6 = x4;   // 504f71
    x6 = x6 * F32(s[5]);   // 504f74
    x4 = x4 * F32(s[4]);   // 504f7a
    x1 = x5;   // 504f80
    x1 = x1 * F32(s[1]);   // 504f83
    x5 = x5 * F32(s[0]);   // 504f89
    x6 = x6 + x1;   // 504f8e
    x1 = F32(s[22]);   // 504f92
    x4 = x4 + x5;   // 504f98
    x7 = x1;   // 504f9c
    x7 = x7 * F32(s[2]);   // 504f9f
    x6 = x6 + x7;   // 504fa5
    x5 = x1;   // 504fa9
    x5 = x5 * F32(s[3]);   // 504fac
    x4 = x4 + x5;   // 504fb2
    x6 = x6 - x4;   // 504fb6
    x4 = F32(s[26]);   // 504fba
    Put(out, 0xc, x6.f());   // 504fc0
    x6 = x2;   // 504fc5
    x6 = x6 * F32(s[0]);   // 504fc8
    x5 = x4;   // 504fcd
    x5 = x5 * F32(s[8]);   // 504fd0
    x4 = x4 * F32(s[9]);   // 504fd6
    x6 = x6 + x5;   // 504fdc
    x5 = x1;   // 504fe0
    x5 = x5 * F32(s[7]);   // 504fe3
    x6 = x6 + x5;   // 504fe9
    x5 = F32(s[6]);   // 504fed
    x7 = x2;   // 504ff3
    x7 = x7 * F32(s[1]);   // 504ff6
    x7 = x7 + x4;   // 504ffc
    x4 = x1;   // 505000
    x4 = x4 * x5;   // 505003
    x7 = x7 + x4;   // 505007
    x4 = F32(s[26]);   // 50500b
    x6 = x6 - x7;   // 505011
    Put(out, 0x1c, x6.f());   // 505015
    x7 = x4;   // 50501a
    x7 = x7 * F32(s[11]);   // 50501d
    x4 = x4 * F32(s[10]);   // 505023
    x6 = x2;   // 505029
    x6 = x6 * F32(s[3]);   // 50502c
    x6 = x6 + x7;   // 505032
    x7 = F32(s[18]);   // 505036
    x7 = x7 * x5;   // 50503c
    x5 = x2;   // 505040
    x5 = x5 * F32(s[2]);   // 505043
    x5 = x5 + x4;   // 505049
    x4 = F32(s[18]);   // 50504d
    x4 = x4 * F32(s[7]);   // 505053
    x5 = x5 + x4;   // 505059
    x4 = F32(s[18]);   // 50505d
    x6 = x6 + x7;   // 505063
    x6 = x6 - x5;   // 505067
    Put(out, 0x2c, x6.f());   // 50506b
    x5 = x2;   // 505070
    x5 = x5 * F32(s[4]);   // 505073
    x2 = x2 * F32(s[5]);   // 505079
    x6 = x4;   // 50507f
    x6 = x6 * F32(s[9]);   // 505082
    x4 = x4 * F32(s[8]);   // 505088
    x5 = x5 + x6;   // 50508e
    x6 = x1;   // 505092
    x1 = x1 * F32(s[11]);   // 505095
    x6 = x6 * F32(s[10]);   // 50509b
    x2 = x2 + x4;   // 5050a1
    x2 = x2 + x1;   // 5050a5
    x1 = F32(s[12]);   // 5050a9
    x5 = x5 + x6;   // 5050af
    x5 = x5 - x2;   // 5050b3
    Put(out, 0x3c, x5.f());   // 5050b7
    x1 = x1 * F32(Get(out, 0x0));   // 5050bc
    x0 = x0 * F32(Get(out, 0x20));   // 5050c0
    x3 = x3 * F32(Get(out, 0x30));   // 5050c5
    x0 = x0 + x1;   // 5050ca
    x1 = F32(Get(out, 0x10));   // 5050ce
    x1 = x1 * F32(s[16]);   // 5050d3
    x0 = x0 + x3;   // 5050d9
    x0 = x0 + x1;   // 5050dd
    x1 = F32(1.0f);   // 5050e1
    x1 = x1 / x0;   // 5050e9
    for (uint32_t r = 0; r < 4; ++r) {   // scale by 1 / det: x1 * a, b * x1, c * x1, x1 * d per row
        Put(out, 16 * r + 0, (x1 * F32(Get(out, 16 * r + 0))).f());
        Put(out, 16 * r + 4, (F32(Get(out, 16 * r + 4)) * x1).f());
        Put(out, 16 * r + 8, (F32(Get(out, 16 * r + 8)) * x1).f());
        Put(out, 16 * r + 12, (x1 * F32(Get(out, 16 * r + 12))).f());
    }
}

int32_t SignOf(float v)
{
    if (!jmp::BE(v, 0.0f)) return 1;    // comiss v, 0; jbe
    if (!jmp::BE(0.0f, v)) return -1;   // comiss 0, v; jbe
    return 0;
}

Vec4* Scale4To(const Vec4& v, Vec4& out, float s)
{
    (F32x4::Splat(F32(s)) * F32x4::Load(&v.x)).Store(&out.x);
    return &out;
}

void RatioUpdate(float* p, float c)
{
    const float p0 = p[0];
    // ucomiss p0, 0; lahf; test ah, 0x44; jnp: 0 when equal and ordered
    p[1] = p0 == 0.0f ? 0.0f : ((F32(p[3]) * F32(p[2])) / (F32(p0) * F32(c))).f();
}

Vec3* Scale3InPlace(Vec3& v, float s)
{
    v.x = (F32(v.x) * F32(s)).f();
    v.y = (F32(v.y) * F32(s)).f();
    v.z = (F32(v.z) * F32(s)).f();
    return &v;
}

Vec3* Add3(const Vec3& a, const Vec3& b, Vec3& out)
{
    const F32 x = F32(a.x) + F32(b.x), y = F32(a.y) + F32(b.y), z = F32(a.z) + F32(b.z);   // all loads first
    out.x = x.f();
    out.y = y.f();
    out.z = z.f();
    return &out;
}

void Scale3(const Vec3& v, float s, Vec3& out)
{
    const V3 x = Load(v);
    const F32 k = s;
    Store(out, {x.x * k, x.y * k, x.z * k});
}

void Normalize3Approx(Vec3& v)
{
    const V3 x = Load(v);
    const F32 inv = RsqrtApprox((x.x * x.x + x.y * x.y) + x.z * x.z);
    Store(v, {x.x * inv, x.y * inv, x.z * inv});
}

void Min4(Vec4& a, const Vec4& b)
{
    _mm_storeu_ps(&a.x, _mm_min_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
}

void Max4(Vec4& a, const Vec4& b)
{
    _mm_storeu_ps(&a.x, _mm_max_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
}

Vec4* RotateRows4(const float* m, Vec4* v, uint32_t n)
{
    const F32x4 r0 = F32x4::Load(m), r1 = F32x4::Load(m + 3), r2 = F32x4::Load(m + 6);
    for (uint32_t i = 0; i < n; ++i) {
        const F32x4 p = F32x4::Load(&v[i].x);
        const F32x4 x = F32x4::Splat(p.v[0]), y = F32x4::Splat(p.v[1]), z = F32x4::Splat(p.v[2]);
        ((x * r0 + y * r1) + z * r2).Store(&v[i].x);
    }
    return v;
}

Vec4* AddVec4Array(const Vec4& t, Vec4* v, uint32_t n)
{
    const F32x4 d = F32x4::Load(&t.x);
    for (uint32_t i = 0; i < n; ++i) (F32x4::Load(&v[i].x) + d).Store(&v[i].x);
    return v;
}

Vec4* SubVec4Array(const Vec4& t, Vec4* v, uint32_t n)
{
    const __m128 d = _mm_loadu_ps(&t.x);
    for (uint32_t i = 0; i < n; ++i) _mm_storeu_ps(&v[i].x, _mm_sub_ps(_mm_loadu_ps(&v[i].x), d));
    return v;
}

void Mat33RotateArray(const float* m, Vec4* v, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        const F32 x = F32(v[i].x), y = F32(v[i].y), z = F32(v[i].z);
        v[i].x = ((F32(m[1]) * y + F32(m[2]) * z) + F32(m[0]) * x).f();
        v[i].y = ((F32(m[4]) * y + F32(m[5]) * z) + x * F32(m[3])).f();
        v[i].z = ((F32(m[7]) * y + F32(m[8]) * z) + F32(m[6]) * x).f();
    }
}

Vec4* Mul4(const Vec4& a, const Vec4& b, Vec4& out)
{
    (F32x4::Load(&a.x) * F32x4::Load(&b.x)).Store(&out.x);
    return &out;
}

Vec4* Min4To(const Vec4& a, const Vec4& b, Vec4& out)
{
    _mm_storeu_ps(&out.x, _mm_min_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
    return &out;
}

Vec4* Max4To(const Vec4& a, const Vec4& b, Vec4& out)
{
    _mm_storeu_ps(&out.x, _mm_max_ps(_mm_loadu_ps(&a.x), _mm_loadu_ps(&b.x)));
    return &out;
}

Vec4* Normalize4Approx(Vec4& v)
{
    const F32 x = v.x, y = v.y, z = v.z, w = v.w;
    const F32 inv = RsqrtApprox(((x * x + y * y) + z * z) + w * w);
    (F32x4::Load(&v.x) * F32x4::Splat(inv)).Store(&v.x);
    return &v;
}

Vec4* Normalize4ApproxTo(const Vec4& v, Vec4& out)
{
    const F32 x = v.x, y = v.y, z = v.z, w = v.w;
    const F32 inv = RsqrtApprox(((x * x + y * y) + z * z) + w * w);
    (F32x4::Load(&v.x) * F32x4::Splat(inv)).Store(&out.x);
    return &out;
}

Vec4* AddXyz4(const Vec4& a, Vec4& out, const Vec4& b)
{
    const float x = (F32(b.x) + F32(a.x)).f(), y = (F32(b.y) + F32(a.y)).f(), z = (F32(b.z) + F32(a.z)).f();
    out.x = x;
    out.y = y;
    out.z = z;
    out.w = 0.0f;
    return &out;
}

Vec4* CrossNormalize4Approx(const Vec4& a, const Vec4& b, Vec4& out)
{
    const __m128 A = _mm_loadu_ps(&a.x), B = _mm_loadu_ps(&b.x);
    __m128 c = _mm_sub_ps(_mm_mul_ps(A, _mm_shuffle_ps(B, B, 0xc9)), _mm_mul_ps(B, _mm_shuffle_ps(A, A, 0xc9)));
    c = _mm_shuffle_ps(c, c, 0xc9);   // a x b, w = a.w b.w - b.w a.w
    alignas(16) float l[4];
    _mm_store_ps(l, c);
    const F32 inv = RsqrtApprox((F32(l[0]) * F32(l[0]) + F32(l[1]) * F32(l[1])) + F32(l[2]) * F32(l[2]));
    (F32x4::Load(l) * F32x4::Splat(inv)).Store(&out.x);
    return &out;
}

Vec4* Sub3W(const Vec4& a, const Vec4& b, Vec4& out)
{
    const Vec4 r{(F32(a.x) - F32(b.x)).f(), (F32(a.y) - F32(b.y)).f(), (F32(a.z) - F32(b.z)).f(), 0.0f};
    out = r;
    return &out;
}

X87 TableLerpU(const float* t, int32_t a, int32_t b, int32_t c, int32_t d)
{
    // sub wraps on overflow (C++ int subtraction would be undefined): do it in uint32.
    auto sub = [](int32_t x, int32_t y) { return static_cast<int32_t>(static_cast<uint32_t>(x) - static_cast<uint32_t>(y)); };
    const X87 q = (FildUnsigned(sub(a, b)) * FildUnsigned(d)) / FildUnsigned(sub(c, b));
    const int32_t i = Ftol(q);
    const X87 f = q - FildUnsigned(i);
    return (X87(1.0f) - f) * X87(t[i]) + f * X87(t[i + 1]);
}

namespace {
// CurveSample and FloatCurveSample: the same code over points of `stride` bytes, value at `ofs`.
X87 SampleCurve(const uint8_t* table, float x, float lo, float hi, int32_t n, uint32_t stride, uint32_t ofs)
{
    const int32_t last = static_cast<int32_t>(static_cast<uint32_t>(n) - 1u);   // dec: wraps
    const float q = ((F32(x) - F32(lo)) * F32(static_cast<float>(last)) / (F32(hi) - F32(lo))).f();
    int32_t i;
    if (!jmp::B(q, 0.0f) && !jmp::BE(2147483648.0f, q)) {
        i = _mm_cvtt_ss2si(_mm_set_ss(q));
    } else if (!jmp::B(q, 2147483648.0f)) {
        i = 0x7fffffff;
    } else if (!jmp::B(q, -2147483648.0f)) {
        i = _mm_cvtt_ss2si(_mm_set_ss(q));
    } else {
        Warn(6, 5, "FloorS32: Invalid argument %f\n", static_cast<double>(q));
        i = INT32_MIN;
    }
    auto y = [table, stride](uint32_t k, uint32_t ofs) {
        float v;
        std::memcpy(&v, table + k * stride + ofs, 4);
        return v;
    };
    if (i > static_cast<int32_t>(static_cast<uint32_t>(last) - 1u)) return X87(y(static_cast<uint32_t>(last), ofs));
    if (i < 0) return X87(y(0, ofs));
    const X87 f = X87(q) - X87::Raw(static_cast<double>(i));   // fild; fsubr q
    return (X87(1.0f) - f) * X87(y(static_cast<uint32_t>(i), ofs)) + f * X87(y(static_cast<uint32_t>(i), ofs + stride));
}
} // namespace

X87 CurveSample(const uint8_t* table, float x, float lo, float hi, int32_t n) { return SampleCurve(table, x, lo, hi, n, 16, 0xc); }

X87 FloatCurveSample(const float* table, float x, float lo, float hi, int32_t n)
{
    return SampleCurve(reinterpret_cast<const uint8_t*>(table), x, lo, hi, n, 4, 0);
}

void TriBlend(float x, float* out)
{
    if (x == 0.0f) {                              // ucomiss x, 0; lahf; test ah, 0x44; jnp
        out[2] = 1.0f;
        out[3] = 0.0f;
        out[4] = 0.0f;
        out[5] = 0.0f;
        return;
    }
    const F32 one = F32(1.0f), y = F32(x) * F32(2.0f);
    if (!jmp::BE(x, 0.5f)) {                      // comiss x, 0.5; jbe
        const F32 a = y - one;
        out[3] = a.f();
        out[4] = (one - a).f();
        out[5] = 0.0f;
        out[2] = 0.0f;
        return;
    }
    out[3] = 0.0f;
    out[4] = y.f();
    out[5] = (one - y).f();
    out[2] = 0.0f;
}

X87 SoftCurve(float x)
{
    float a, b;
    const uint32_t ab = 0x3e924925, bb = 0x3fa49249;   // 2/7, 9/7 as the original's floats
    std::memcpy(&a, &ab, 4);
    std::memcpy(&b, &bb, 4);
    if (!jmp::BE(1.0f, x))                              // comiss 1, x; jbe
        return (X87(b) - X87(x) * X87(a)) * X87(x);     // fld x; fmul a; fsubr b; fmul x
    const X87 r = X87(1.0f) / X87(x);
    return X87(1.0f) / (r * (X87(b) - X87(a) * r));     // fld a; fmul st1; fsubr b; fmulp; fdivr 1
}

void SplitPair(float* p, float a, float b)
{
    if (jmp::BE(p[0], a)) {                          // comiss p0, a; jbe
        float e;
        const uint32_t eb = 0x3727c5ac;                // 1e-5
        std::memcpy(&e, &eb, 4);
        p[0] = e;
        p[2] = e;
        p[1] = e;
        return;
    }
    const F32 d = F32(p[0]) - F32(a);
    const float r = (F32(1.0f) / (F32(p[0]) / d)).f();
    const X87 rr = X87(r) * X87(r);
    p[0] = d.f();
    const float k = ((rr / Sqrt(X87(r))) * X87(p[2])).Store();
    const F32 p3k = F32(p[3]) * F32(k);
    p[2] = k;
    p[1] = (p3k / (d * F32(b))).f();
}

void ClampedRatio(float a, float b, float c, float& out)
{
    const float m = (F32(0.0f) - F32(b) * F32(c)).f();
    auto ratio = [&](float& o) {
        if (jmp::BE(b, 0.001f)) o = 0.0f;   // comiss b, 0.001; jbe
        else o = ((F32(-1.0f) / F32(b)) * F32(a)).f();
    };
    if (!jmp::BE(a, 0.0f)) {               // comiss a, 0; jbe
        if (!jmp::BE(m, a)) return ratio(out);
        if (!jmp::BE(0.0f, m)) out = 0.0f;
        else std::memcpy(&out, &c, 4);
        return;
    }
    if (!jmp::BE(a, m)) return ratio(out);
    if (!jmp::BE(m, 0.0f)) out = 0.0f;
    else std::memcpy(&out, &c, 4);
}

Vec4* Bounds4Union(const Vec4* box, const Vec4* other, Vec4& outMin, Vec4& outMax)
{
    Min4To(box[0], other[0], outMin);
    return Max4To(box[1], other[1], outMax);
}

Mat33& Mat33Zero(Mat33& m)
{
    for (float& e : m.m) e = 0.0f;
    return m;
}

Mat33& Mat33Mul(const Mat33& a, Mat33& out, const Mat33& b)
{
    auto A = [&](int i) { return F32(a.m[i]); };
    auto B = [&](int i) { return F32(b.m[i]); };
    float c[9];   // computed in full before the store: out may alias a or b
    c[0] = ((B(0) * A(0) + B(2) * A(6)) + B(1) * A(3)).f();
    c[1] = ((B(2) * A(7) + A(4) * B(1)) + A(1) * B(0)).f();
    c[2] = ((B(2) * A(8) + B(1) * A(5)) + A(2) * B(0)).f();
    c[3] = ((A(6) * B(5) + B(4) * A(3)) + B(3) * A(0)).f();
    c[4] = ((A(1) * B(3) + B(5) * A(7)) + B(4) * A(4)).f();
    c[5] = ((A(8) * B(5) + B(4) * A(5)) + B(3) * A(2)).f();
    c[6] = ((B(6) * A(0) + B(8) * A(6)) + B(7) * A(3)).f();
    c[7] = ((B(8) * A(7) + A(1) * B(6)) + A(4) * B(7)).f();
    c[8] = ((B(8) * A(8) + A(2) * B(6)) + B(7) * A(5)).f();
    std::memcpy(out.m, c, sizeof c);
    return out;
}

void BracketSearch(const float* table, float x, uint32_t n, Bracket* res)
{
    int32_t c = res->i0;
    if (!jmp::BE(x, table[c])) {                      // comiss x, table[hint]; jbe -> down
        if (!jmp::BE(x, table[n - 1])) {              // above the last entry: clamp
            res->i1 = static_cast<int32_t>(n) - 1;
            res->i0 = static_cast<int32_t>(n) - 2;
            res->t = 1.0f;
            return;
        }
        do {
            ++c;
        } while (jmp::A(x, table[c]));                 // comiss x, table[c]; ja
        res->i0 = c - 1;
        res->i1 = c;
        res->t = ((F32(x) - F32(table[c - 1])) / (F32(table[c]) - F32(table[c - 1]))).f();
        return;
    }
    if (!jmp::B(table[0], x)) {                        // comiss table[0], x; jb -> search down
        res->i0 = 0;
        res->i1 = 1;
        res->t = 0.0f;
        return;
    }
    float v;
    do {
        v = table[c - 1];
        --c;
    } while (jmp::A(v, x));                            // comiss table[c], x; ja
    res->i1 = c + 1;
    res->i0 = c;
    res->t = ((F32(x) - F32(table[c])) / (F32(table[c + 1]) - F32(table[c]))).f();
}

void AddDivProduct(const float* m, float f, float* acc)
{
    *acc = (F32(f) / (F32(m[1]) * F32(m[0])) + F32(*acc)).f();
}

Vec4* Mat44MulVec4(const float* m, const Vec4* v, Vec4* out)
{
    const F32x4 p = F32x4::Load(&v->x);
    (((F32x4::Load(m) * F32x4::Splat(p.v[0]) + F32x4::Load(m + 4) * F32x4::Splat(p.v[1])) +
      F32x4::Load(m + 8) * F32x4::Splat(p.v[2])) + F32x4::Load(m + 12) * F32x4::Splat(p.v[3])).Store(&out->x);
    return out;
}

X87 RingAverage(float x, uint8_t* ring)
{
    uint32_t n, i;
    std::memcpy(&n, ring, 4);
    std::memcpy(&i, ring + 4, 4);
    ++i;
    std::memcpy(ring + 4, &i, 4);
    if (i > n - 1) {
        i = 0;
        std::memcpy(ring + 4, &i, 4);
    }
    std::memcpy(ring + 8 + 4 * i, &x, 4);
    F32 s = 0.0f;
    for (uint32_t k = 0; k < n; ++k) {
        float v;
        std::memcpy(&v, ring + 8 + 4 * k, 4);
        s = F32(v) + s;
    }
    return X87(s) / FildUnsigned(static_cast<int32_t>(n));
}

X87 TableLerpScaled(float t, const float* table, uint32_t n)
{
    const float q = (FildUnsigned(static_cast<int32_t>(n)) * X87(t)).Store();
    const float two31 = 2147483648.0f;
    uint32_t i;
    if (!jmp::B(q, 0.0f) && jmp::A(two31, q)) {
        i = static_cast<uint32_t>(_mm_cvtt_ss2si(_mm_set_ss(q)));
    } else if (!jmp::B(q, two31)) {
        i = 0x7fffffff;
    } else if (!jmp::B(q, -two31)) {
        i = static_cast<uint32_t>(_mm_cvtt_ss2si(_mm_set_ss(q)));
    } else {
        Warn(6, 5, "FloorS32: Invalid argument %f\n", static_cast<double>(q));
        i = 0x80000000u;
    }
    if (i > n) i = n;   // cmova
    const X87 f = X87(q) - FildUnsigned(static_cast<int32_t>(i));
    return (X87(1.0f) - f) * X87(table[i]) + f * X87(table[i + 1]);
}

X87 LumpTake(float* p, float a, float b, float c)
{
    if (jmp::BE(p[2], b)) return X87(0.0f);
    const F32 one = 1.0f, k1 = 1.2857143f, k2 = 0.2857143f;   // 0x3fa49249, 0x3e924925
    const F32 r = F32(p[2]) / F32(b);
    F32 f;
    if (!jmp::BE(1.0f, r.f())) {
        f = (k1 - r * k2) * r;
    } else {
        const F32 ir = one / r;
        f = one / ((k1 - ir * k2) * ir);
    }
    const float h = ((one - one / f) * F32(p[0])).f();
    if (!jmp::BE(h, a)) {
        SplitPair(p, a, c);
        return X87(a);
    }
    if (!jmp::BE(h, 0.0f)) {
        SplitPair(p, h, c);
        return X87(h);
    }
    return X87(0.0f);
}

void AddHeatToLump(const float* def, float q, float* s)
{
    s[1] = (F32(q) / (F32(def[0]) * F32(s[0])) + F32(s[1])).f();
}

void MixLumps(const float* a, float* b)
{
    const F32 m = F32(a[0]) + F32(b[0]);
    if (!jmp::BE(m.f(), 1e-6f)) {   // comiss m, 1e-6 (0x358637bd); jbe
        b[1] = ((F32(a[1]) * F32(a[0]) + F32(b[1]) * F32(b[0])) / m).f();
    } else if (!jmp::BE(a[0], b[0])) {
        std::memcpy(&b[1], &a[1], 4);
    }
    b[0] = m.f();
}

float BoilOffLump(const float* def, float* s)
{
    if (jmp::BE(s[1], def[3])) return 0.0f;
    const F32 e = (((F32(s[1]) - F32(def[3])) * F32(def[0])) * F32(s[0])) / F32(def[1]);
    const F32 left = F32(s[0]) - e;
    std::memcpy(&s[1], &def[3], 4);
    s[0] = left.f();
    F32 r = e;
    if (!jmp::BE(0.0f, left.f())) {   // comiss 0, left; jbe
        r = left + e;
        s[0] = 0.0f;
    }
    return X87(r).Store();   // through memory and fld
}

void RateTimes(float* p, float k)
{
    F32 r;   // xorps: +0
    if (!(p[2] == 0.0f)) r = ((F32(p[1]) / F32(p[2])) * F32(p[0])) * F32(k);   // ucomiss; jnp: equal and ordered skips
    p[3] = r.f();
}

void Mat33FromQuat(Mat33& out, const Vec4& q)
{
    // The original forms q.x*q, q.y*q and q.z*q with mulps (broadcast lane first) and reads
    // these nine products back; the rest is scalar, in this order.
    const F32 qx = q.x, qy = q.y, qz = q.z, qw = q.w;
    const F32 xx = qx * qx, xy = qx * qy, xz = qx * qz, xw = qx * qw;
    const F32 yy = qy * qy, yz = qy * qz, yw = qy * qw;
    const F32 zz = qz * qz, zw = qz * qw;
    const F32 two = 2.0f;   // [0x72ad94]
    const F32 one = 1.0f;   // [0x795e14]
    out.m[0] = (one - (zz + yy) * two).f();
    out.m[3] = ((xy - zw) * two).f();
    out.m[6] = ((yw + xz) * two).f();
    out.m[1] = ((zw + xy) * two).f();
    out.m[4] = (one - (xx + zz) * two).f();
    out.m[7] = ((yz - xw) * two).f();
    out.m[2] = ((xz - yw) * two).f();
    out.m[5] = ((xw + yz) * two).f();
    out.m[8] = (one - (xx + yy) * two).f();
}

int AllNegative3(const float* v)
{
    // comiss 0, v[k]; jbe: false for v[k] >= 0 or NaN
    return jmp::A(0.0f, v[0]) && jmp::A(0.0f, v[1]) && jmp::A(0.0f, v[2]) ? 1 : 0;
}

} // namespace phys::num
