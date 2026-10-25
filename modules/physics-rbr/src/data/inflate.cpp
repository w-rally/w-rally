#include "inflate.h"

#include <stdexcept>

namespace phys::data {

namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    uint16_t count[kMaxBits + 1];
    uint16_t symbol[288];
};

class Bits {
public:
    Bits(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    int Get(int need)
    {
        long v = buf_;
        while (cnt_ < need) {
            if (at_ == n_) throw std::runtime_error("inflate: input ended");
            v |= static_cast<long>(p_[at_++]) << cnt_;
            cnt_ += 8;
        }
        buf_ = static_cast<int>(v >> need);
        cnt_ -= need;
        return static_cast<int>(v & ((1L << need) - 1));
    }
    void Align() { buf_ = 0, cnt_ = 0; }
    uint8_t Byte()
    {
        if (at_ == n_) throw std::runtime_error("inflate: input ended");
        return p_[at_++];
    }

private:
    const uint8_t* p_;
    size_t n_, at_ = 0;
    int buf_ = 0, cnt_ = 0;
};

int Decode(Bits& s, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= s.Get(1);
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw std::runtime_error("inflate: bad code");
}

void Build(Huffman& h, const uint16_t* length, int n)
{
    for (uint16_t& c : h.count) c = 0;
    for (int s = 0; s < n; ++s) ++h.count[length[s]];
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int s = 0; s < n; ++s)
        if (length[s]) h.symbol[offs[length[s]]++] = static_cast<uint16_t>(s);
}

constexpr uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint16_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint16_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Codes(Bits& s, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist)
{
    for (;;) {
        int sym = Decode(s, lit);
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("inflate: bad length code");
            const int len = kLenBase[sym] + s.Get(kLenExtra[sym]);
            const int d = Decode(s, dist);
            if (d >= 30) throw std::runtime_error("inflate: bad distance code");
            const size_t back = kDistBase[d] + s.Get(kDistExtra[d]);
            if (back > out.size()) throw std::runtime_error("inflate: distance too far back");
            const size_t from = out.size() - back;
            for (int k = 0; k < len; ++k) out.push_back(out[from + k]);
        }
    }
}

} // namespace

void Inflate(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected)
{
    out.reserve(out.size() + expected);
    Bits s(in, n);
    static Huffman fixedLit, fixedDist;
    static bool fixedBuilt;
    if (!fixedBuilt) {
        uint16_t len[288];
        int i = 0;
        for (; i < 144; ++i) len[i] = 8;
        for (; i < 256; ++i) len[i] = 9;
        for (; i < 280; ++i) len[i] = 7;
        for (; i < 288; ++i) len[i] = 8;
        Build(fixedLit, len, 288);
        for (i = 0; i < 30; ++i) len[i] = 5;
        Build(fixedDist, len, 30);
        fixedBuilt = true;
    }
    int last;
    do {
        last = s.Get(1);
        const int type = s.Get(2);
        if (type == 0) {   // stored
            s.Align();
            const unsigned lo = s.Byte(), hi = s.Byte(), nlo = s.Byte(), nhi = s.Byte();
            const unsigned len = lo | hi << 8;
            if (len != (~(nlo | nhi << 8) & 0xffffu)) throw std::runtime_error("inflate: bad stored block");
            for (unsigned k = 0; k < len; ++k) out.push_back(s.Byte());
        } else if (type == 1) {
            Codes(s, out, fixedLit, fixedDist);
        } else if (type == 2) {   // dynamic
            const int nlen = s.Get(5) + 257, ndist = s.Get(5) + 1, ncode = s.Get(4) + 4;
            if (nlen > 286 || ndist > 30) throw std::runtime_error("inflate: bad counts");
            static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint16_t lengths[320] = {};
            for (int k = 0; k < ncode; ++k) lengths[kOrder[k]] = static_cast<uint16_t>(s.Get(3));
            Huffman lencode;
            Build(lencode, lengths, 19);
            int index = 0;
            while (index < nlen + ndist) {
                int sym = Decode(s, lencode);
                if (sym < 16) {
                    lengths[index++] = static_cast<uint16_t>(sym);
                } else {
                    uint16_t len = 0;
                    int rep;
                    if (sym == 16) {
                        if (!index) throw std::runtime_error("inflate: repeat with no length");
                        len = lengths[index - 1];
                        rep = 3 + s.Get(2);
                    } else if (sym == 17) {
                        rep = 3 + s.Get(3);
                    } else {
                        rep = 11 + s.Get(7);
                    }
                    if (index + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                    while (rep--) lengths[index++] = len;
                }
            }
            Huffman lit, dist;
            Build(lit, lengths, nlen);
            Build(dist, lengths + nlen, ndist);
            Codes(s, out, lit, dist);
        } else {
            throw std::runtime_error("inflate: bad block type");
        }
    } while (!last);
}

} // namespace phys::data
