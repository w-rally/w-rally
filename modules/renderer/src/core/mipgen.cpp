#include "mipgen.h"

#include <math.h>
#include <string.h>
#include <algorithm>

namespace ngg {
namespace core {

namespace {

enum Format { kNone, kDxt1, kDxt3, kDxt5 };

uint32_t FourCC(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

// DDS header offsets, counted from the start of the file (after "DDS ").
const size_t kHeaderBytes   = 4 + 124;
const size_t kOffFlags      = 4 + 4;
const size_t kOffHeight     = 4 + 8;
const size_t kOffWidth      = 4 + 12;
const size_t kOffMipCount   = 4 + 24;
const size_t kOffPfFlags    = 4 + 76;
const size_t kOffFourCC     = 4 + 80;
const size_t kOffCaps       = 4 + 104;
const uint32_t kDdsdMipCount = 0x20000;
const uint32_t kCapsComplex  = 0x8;
const uint32_t kCapsMipmap   = 0x400000;
const uint32_t kDdpfFourCC   = 0x4;

uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
void     Wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

struct Px { float r, g, b, a; };     // linear light, 0..1

// ---------------------------------------------------------------- transfer
// A function-local static: initialised exactly once even when several textures
// are generated on several threads at the same time.
struct Lut { float v[256]; };
const Lut& ToLinear()
{
    static const Lut lut = [] {
        Lut l;
        for (int i = 0; i < 256; ++i)
        {
            const float c = i / 255.0f;
            l.v[i] = (c <= 0.04045f) ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
        }
        return l;
    }();
    return lut;
}
float ToEncoded(float l, bool srgb)
{
    l = std::min(std::max(l, 0.0f), 1.0f);
    if (!srgb) return l * 255.0f;
    const float c = (l <= 0.0031308f) ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
    return c * 255.0f;
}

// ---------------------------------------------------------------- decode
void Unpack565(uint16_t c, int rgb[3])
{
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

// Colour block -> 16 texels of 8-bit RGBA. `fourColour` is forced for DXT3/5:
// the colour half of those blocks is always four-colour, whatever the endpoint
// order - only DXT1 has the three-colour + transparent mode.
void DecodeColour(const uint8_t* blk, bool fourColourAlways, uint8_t out[16][4])
{
    const uint16_t c0 = (uint16_t)(blk[0] | (blk[1] << 8));
    const uint16_t c1 = (uint16_t)(blk[2] | (blk[3] << 8));
    int p[4][4];
    Unpack565(c0, p[0]); Unpack565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    if (fourColourAlways || c0 > c1)
    {
        for (int k = 0; k < 3; ++k)
        {
            p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
        }
        p[2][3] = p[3][3] = 255;
    }
    else
    {
        for (int k = 0; k < 3; ++k) { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
        p[2][3] = 255; p[3][3] = 0;
    }
    const uint32_t bits = Rd32(blk + 4);
    for (int i = 0; i < 16; ++i)
    {
        const int s = (bits >> (2 * i)) & 3;
        for (int k = 0; k < 4; ++k) out[i][k] = (uint8_t)p[s][k];
    }
}

void DecodeAlphaDxt5(const uint8_t* blk, uint8_t out[16][4])
{
    const int a0 = blk[0], a1 = blk[1];
    int a[8] = { a0, a1 };
    if (a0 > a1)
        for (int k = 1; k <= 6; ++k) a[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
    else
    {
        for (int k = 1; k <= 4; ++k) a[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
        a[6] = 0; a[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= (uint64_t)blk[2 + k] << (8 * k);
    for (int i = 0; i < 16; ++i) out[i][3] = (uint8_t)a[(bits >> (3 * i)) & 7];
}

void DecodeAlphaDxt3(const uint8_t* blk, uint8_t out[16][4])
{
    for (int i = 0; i < 16; ++i)
    {
        const int v = (blk[i / 2] >> ((i & 1) * 4)) & 15;
        out[i][3] = (uint8_t)(v * 17);
    }
}

// ---------------------------------------------------------------- encode
uint16_t Pack565(const float c[3])
{
    const int r = std::min(31, std::max(0, (int)lroundf(c[0] * 31.0f / 255.0f)));
    const int g = std::min(63, std::max(0, (int)lroundf(c[1] * 63.0f / 255.0f)));
    const int b = std::min(31, std::max(0, (int)lroundf(c[2] * 31.0f / 255.0f)));
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Four-colour BC1 block from 16 texels of encoded RGB (0..255, float).
// Endpoints along the principal axis of the block's colours, which is what the
// palette can actually represent; then each texel takes the nearest of the
// four palette entries as decoded. `transparent` marks texels to emit as the
// DXT1 three-colour mode's index 3 (DXT1 only).
void EncodeColour(const float px[16][3], const bool* transparent, uint8_t* blk)
{
    float mean[3] = { 0, 0, 0 };
    int n = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (transparent && transparent[i]) continue;
        for (int k = 0; k < 3; ++k) mean[k] += px[i][k];
        ++n;
    }
    if (n == 0)
    {
        // Fully transparent DXT1 block: three-colour mode, every index 3.
        blk[0] = 0; blk[1] = 0; blk[2] = 0; blk[3] = 0;
        Wr32(blk + 4, 0xFFFFFFFFu);
        return;
    }
    for (float& m : mean) m /= n;

    // Covariance, then the dominant axis by power iteration.
    float cov[6] = {};
    for (int i = 0; i < 16; ++i)
    {
        if (transparent && transparent[i]) continue;
        const float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        cov[0] += d[0]*d[0]; cov[1] += d[0]*d[1]; cov[2] += d[0]*d[2];
        cov[3] += d[1]*d[1]; cov[4] += d[1]*d[2]; cov[5] += d[2]*d[2];
    }
    float axis[3] = { 0.577f, 0.577f, 0.577f };
    for (int it = 0; it < 8; ++it)
    {
        const float v[3] = { cov[0]*axis[0] + cov[1]*axis[1] + cov[2]*axis[2],
                             cov[1]*axis[0] + cov[3]*axis[1] + cov[4]*axis[2],
                             cov[2]*axis[0] + cov[4]*axis[1] + cov[5]*axis[2] };
        const float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (l < 1e-6f) break;
        for (int k = 0; k < 3; ++k) axis[k] = v[k] / l;
    }
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 16; ++i)
    {
        if (transparent && transparent[i]) continue;
        const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] +
                        (px[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t); hi = std::max(hi, t);
    }
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k) { e0[k] = mean[k] + axis[k] * hi; e1[k] = mean[k] + axis[k] * lo; }
    uint16_t c0 = Pack565(e0), c1 = Pack565(e1);

    const bool threeColour = transparent != nullptr;     // DXT1 with transparent texels
    if (threeColour) { if (c0 > c1) std::swap(c0, c1); }  // c0 <= c1 selects 3-colour mode
    else if (c0 < c1) std::swap(c0, c1);                 // c0 > c1 selects 4-colour mode

    int p[4][3];
    Unpack565(c0, p[0]); Unpack565(c1, p[1]);
    const int count = (threeColour || c0 == c1) ? 3 : 4;
    for (int k = 0; k < 3; ++k)
    {
        if (count == 4)
        {
            p[2][k] = (2 * p[0][k] + p[1][k] + 1) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k] + 1) / 3;
        }
        else
        {
            p[2][k] = (p[0][k] + p[1][k]) / 2;
            p[3][k] = p[0][k];
        }
    }

    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i)
    {
        int best = 0;
        if (threeColour && transparent[i]) best = 3;
        else
        {
            float bestD = 1e30f;
            for (int s = 0; s < count; ++s)
            {
                if (count == 3 && s == 3) continue;
                const float dr = px[i][0] - p[s][0], dg = px[i][1] - p[s][1], db = px[i][2] - p[s][2];
                const float d = dr*dr + dg*dg + db*db;
                if (d < bestD) { bestD = d; best = s; }
            }
        }
        bits |= (uint32_t)best << (2 * i);
    }
    blk[0] = (uint8_t)(c0 & 0xFF); blk[1] = (uint8_t)(c0 >> 8);
    blk[2] = (uint8_t)(c1 & 0xFF); blk[3] = (uint8_t)(c1 >> 8);
    Wr32(blk + 4, bits);
}

void EncodeAlphaDxt5(const float a[16], uint8_t* blk)
{
    float lo = 255.0f, hi = 0.0f;
    for (int i = 0; i < 16; ++i) { lo = std::min(lo, a[i]); hi = std::max(hi, a[i]); }
    int a0 = (int)lroundf(hi), a1 = (int)lroundf(lo);
    int pal[8] = { a0, a1 };
    if (a0 > a1)
        for (int k = 1; k <= 6; ++k) pal[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
    else
        for (int k = 2; k < 8; ++k) pal[k] = a0;           // flat block: every index 0
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i)
    {
        int best = 0, bestD = 1 << 30;
        for (int s = 0; s < 8; ++s)
        {
            const int d = abs((int)lroundf(a[i]) - pal[s]);
            if (d < bestD) { bestD = d; best = s; }
        }
        bits |= (uint64_t)best << (3 * i);
    }
    blk[0] = (uint8_t)a0; blk[1] = (uint8_t)a1;
    for (int k = 0; k < 6; ++k) blk[2 + k] = (uint8_t)(bits >> (8 * k));
}

void EncodeAlphaDxt3(const float a[16], uint8_t* blk)
{
    memset(blk, 0, 8);
    for (int i = 0; i < 16; ++i)
    {
        const int v = std::min(15, std::max(0, (int)lroundf(a[i] / 17.0f)));
        blk[i / 2] |= (uint8_t)(v << ((i & 1) * 4));
    }
}

// ---------------------------------------------------------------- levels
struct Level
{
    uint32_t w = 0, h = 0;
    std::vector<Px> px;
    const Px& At(uint32_t x, uint32_t y) const
    {
        return px[(size_t)std::min(y, h - 1) * w + std::min(x, w - 1)];
    }
};

float Coverage(const Level& l, float cutoff, float scale)
{
    size_t pass = 0;
    for (const Px& p : l.px) if (p.a * scale >= cutoff) ++pass;
    return l.px.empty() ? 0.0f : (float)pass / (float)l.px.size();
}

} // namespace

bool GenerateDdsMips(const uint8_t* dds, size_t bytes, const MipGenOptions& opt,
                     std::vector<uint8_t>& out, MipGenResult* result)
{
    if (!dds || bytes < kHeaderBytes || memcmp(dds, "DDS ", 4) != 0) return false;
    const uint32_t mipCount = Rd32(dds + kOffMipCount);
    if (mipCount > 1) return false;                         // the file already has them
    if (!(Rd32(dds + kOffPfFlags) & kDdpfFourCC)) return false;

    const uint32_t fcc = Rd32(dds + kOffFourCC);
    Format fmt = kNone;
    if (fcc == FourCC('D','X','T','1')) fmt = kDxt1;
    else if (fcc == FourCC('D','X','T','3')) fmt = kDxt3;
    else if (fcc == FourCC('D','X','T','5')) fmt = kDxt5;
    if (fmt == kNone) return false;                         // includes DX10 headers

    const uint32_t W = Rd32(dds + kOffWidth), H = Rd32(dds + kOffHeight);
    if (W < 2 && H < 2) return false;
    const uint32_t blockBytes = (fmt == kDxt1) ? 8 : 16;
    const uint32_t bw0 = (W + 3) / 4, bh0 = (H + 3) / 4;
    const size_t mip0Bytes = (size_t)bw0 * bh0 * blockBytes;
    if (bytes < kHeaderBytes + mip0Bytes) return false;
    const uint8_t* blocks = dds + kHeaderBytes;

    const float* g_toLinear = ToLinear().v;
    const bool srgb = opt.srgb;

    // ---- decode mip 0 to linear float
    Level base;
    base.w = W; base.h = H;
    base.px.resize((size_t)W * H);
    for (uint32_t by = 0; by < bh0; ++by)
        for (uint32_t bx = 0; bx < bw0; ++bx)
        {
            const uint8_t* blk = blocks + ((size_t)by * bw0 + bx) * blockBytes;
            uint8_t t[16][4];
            if (fmt == kDxt1) DecodeColour(blk, false, t);
            else
            {
                DecodeColour(blk + 8, true, t);
                if (fmt == kDxt3) DecodeAlphaDxt3(blk, t); else DecodeAlphaDxt5(blk, t);
            }
            for (int i = 0; i < 16; ++i)
            {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= W || y >= H) continue;
                Px& p = base.px[(size_t)y * W + x];
                p.r = srgb ? g_toLinear[t[i][0]] : t[i][0] / 255.0f;
                p.g = srgb ? g_toLinear[t[i][1]] : t[i][1] / 255.0f;
                p.b = srgb ? g_toLinear[t[i][2]] : t[i][2] / 255.0f;
                p.a = t[i][3] / 255.0f;
            }
        }

    const bool coverage = opt.alphaTested && opt.preserveCoverage && fmt != kDxt1;
    const float cutoff = opt.alphaCutoff;
    const float coverage0 = coverage ? Coverage(base, cutoff, 1.0f) : 0.0f;

    uint32_t levels = 1;
    for (uint32_t d = std::max(W, H); d > 1; d >>= 1) ++levels;

    // ---- output: header (with the mip chain declared) + mip 0 verbatim
    out.assign(dds, dds + kHeaderBytes + mip0Bytes);
    Wr32(out.data() + kOffMipCount, levels);
    Wr32(out.data() + kOffFlags, Rd32(out.data() + kOffFlags) | kDdsdMipCount);
    Wr32(out.data() + kOffCaps, Rd32(out.data() + kOffCaps) | kCapsComplex | kCapsMipmap);

    double errSum = 0.0;
    size_t errCount = 0;

    Level prev = std::move(base);
    for (uint32_t m = 1; m < levels; ++m)
    {
        Level cur;
        cur.w = std::max(1u, W >> m);
        cur.h = std::max(1u, H >> m);
        cur.px.resize((size_t)cur.w * cur.h);

        // 2x2 box from the previous UNSCALED level, in linear light. Colour is
        // alpha-weighted for alpha-tested textures, so the colour of texels the
        // test discards does not bleed into the ones it keeps.
        for (uint32_t y = 0; y < cur.h; ++y)
            for (uint32_t x = 0; x < cur.w; ++x)
            {
                const Px* s[4] = { &prev.At(2*x, 2*y), &prev.At(2*x+1, 2*y),
                                   &prev.At(2*x, 2*y+1), &prev.At(2*x+1, 2*y+1) };
                Px o = { 0, 0, 0, 0 };
                float wsum = 0.0f;
                for (const Px* q : s)
                {
                    const float w = opt.alphaTested ? q->a : 1.0f;
                    o.r += q->r * w; o.g += q->g * w; o.b += q->b * w;
                    o.a += q->a;
                    wsum += w;
                }
                if (wsum > 1e-6f) { o.r /= wsum; o.g /= wsum; o.b /= wsum; }
                else { o.r = (s[0]->r + s[1]->r + s[2]->r + s[3]->r) * 0.25f;
                       o.g = (s[0]->g + s[1]->g + s[2]->g + s[3]->g) * 0.25f;
                       o.b = (s[0]->b + s[1]->b + s[2]->b + s[3]->b) * 0.25f; }
                o.a *= 0.25f;
                cur.px[(size_t)y * cur.w + x] = o;
            }

        // Coverage: the alpha scale at which this level passes the cutoff on
        // the same fraction of texels as mip 0. Bisection on a monotonic count.
        float scale = 1.0f;
        if (coverage && coverage0 > 0.0f)
        {
            float lo = 0.0f, hi = 8.0f;
            for (int it = 0; it < 14; ++it)
            {
                const float mid = 0.5f * (lo + hi);
                if (Coverage(cur, cutoff, mid) < coverage0) lo = mid; else hi = mid;
            }
            scale = hi;
        }
        if (opt.alphaTested && result && (m == 4 || (m < 4 && m == levels - 1)))
        {
            result->coverage0 = coverage ? coverage0 : -1.0f;
            result->coverageBox = Coverage(cur, cutoff, 1.0f);
            result->coverageKept = Coverage(cur, cutoff, scale);
            result->coverageLevel = m;
        }

        // ---- encode this level
        const uint32_t bw = (cur.w + 3) / 4, bh = (cur.h + 3) / 4;
        const size_t at = out.size();
        out.resize(at + (size_t)bw * bh * blockBytes);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx)
            {
                float rgb[16][3], a[16];
                bool transp[16];
                bool anyTransp = false;
                for (int i = 0; i < 16; ++i)
                {
                    const Px& p = cur.At(bx * 4 + (i & 3), by * 4 + (i >> 2));
                    rgb[i][0] = ToEncoded(p.r, srgb);
                    rgb[i][1] = ToEncoded(p.g, srgb);
                    rgb[i][2] = ToEncoded(p.b, srgb);
                    a[i] = std::min(1.0f, p.a * scale) * 255.0f;
                    transp[i] = a[i] < 128.0f;
                    anyTransp |= transp[i];
                }
                uint8_t* blk = out.data() + at + ((size_t)by * bw + bx) * blockBytes;
                if (fmt == kDxt1) EncodeColour(rgb, anyTransp ? transp : nullptr, blk);
                else
                {
                    if (fmt == kDxt3) EncodeAlphaDxt3(a, blk); else EncodeAlphaDxt5(a, blk);
                    EncodeColour(rgb, nullptr, blk + 8);
                }

                // Re-encoding error, measured by decoding what was just written.
                uint8_t t[16][4];
                if (fmt == kDxt1) DecodeColour(blk, false, t);
                else
                {
                    DecodeColour(blk + 8, true, t);
                    if (fmt == kDxt3) DecodeAlphaDxt3(blk, t); else DecodeAlphaDxt5(blk, t);
                }
                for (int i = 0; i < 16; ++i)
                {
                    if (fmt == kDxt1 && transp[i]) continue;
                    for (int k = 0; k < 3; ++k) errSum += fabs(rgb[i][k] - t[i][k]);
                    errCount += 3;
                }
            }

        prev = std::move(cur);
    }

    if (result)
    {
        result->levels = levels;
        result->meanAbsError = errCount ? (float)(errSum / errCount) : 0.0f;
    }
    return true;
}

bool DecodeDdsAlpha(const uint8_t* dds, size_t bytes, uint32_t& width, uint32_t& height,
                    std::vector<uint8_t>& alpha)
{
    if (!dds || bytes < kHeaderBytes || memcmp(dds, "DDS ", 4) != 0) return false;
    const uint32_t W = Rd32(dds + kOffWidth), H = Rd32(dds + kOffHeight);
    if (!W || !H || W > 16384 || H > 16384) return false;
    const uint32_t pf = Rd32(dds + kOffPfFlags);
    const uint8_t* data = dds + kHeaderBytes;
    alpha.assign((size_t)W * H, 255);
    if (pf & kDdpfFourCC)
    {
        const uint32_t fcc = Rd32(dds + kOffFourCC);
        Format fmt = kNone;
        if (fcc == FourCC('D','X','T','1')) fmt = kDxt1;
        else if (fcc == FourCC('D','X','T','3')) fmt = kDxt3;
        else if (fcc == FourCC('D','X','T','5')) fmt = kDxt5;
        if (fmt == kNone) return false;                     // includes DX10 headers
        const uint32_t blockBytes = (fmt == kDxt1) ? 8 : 16;
        const uint32_t bw = (W + 3) / 4, bh = (H + 3) / 4;
        if (bytes - kHeaderBytes < (size_t)bw * bh * blockBytes) return false;
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx)
            {
                const uint8_t* blk = data + ((size_t)by * bw + bx) * blockBytes;
                uint8_t t[16][4];
                if (fmt == kDxt1) DecodeColour(blk, false, t);
                else if (fmt == kDxt3) DecodeAlphaDxt3(blk, t);
                else DecodeAlphaDxt5(blk, t);
                for (int i = 0; i < 16; ++i)
                {
                    const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < W && y < H) alpha[(size_t)y * W + x] = t[i][3];
                }
            }
        width = W, height = H;
        return true;
    }
    // Uncompressed: 32 bits a texel, the alpha where its mask says (DDPF_ALPHAPIXELS), else opaque.
    const uint32_t bits = Rd32(dds + 4 + 84), aMask = (pf & 0x1) ? Rd32(dds + 4 + 100) : 0;
    if (bits != 32) return false;
    if (bytes - kHeaderBytes < (size_t)W * H * 4) return false;
    if (aMask)
    {
        int shift = 0;
        while (!((aMask >> shift) & 1)) ++shift;
        const uint32_t top = aMask >> shift;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            alpha[i] = (uint8_t)(((Rd32(data + i * 4) & aMask) >> shift) * 255u / top);
    }
    width = W, height = H;
    return true;
}

} // namespace core
} // namespace ngg
