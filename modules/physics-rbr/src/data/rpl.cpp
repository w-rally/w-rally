#include "rpl.h"

#include <cstdio>
#include <cstring>

namespace phys::data {

namespace {

uint32_t BitReverse32(uint32_t x)
{
    uint32_t r = 0;
    for (int i = 0; i < 32; ++i, x >>= 1) r = (r << 1) | (x & 1);
    return r;
}

struct Reader {
    const std::vector<uint8_t>& d;
    size_t o;
    size_t end;
    std::string err;

    bool Need(size_t n)
    {
        if (err.empty() && o + n > end) err = "truncated at 0x" + Hex(o);
        return err.empty();
    }
    static std::string Hex(size_t v)
    {
        char b[20];
        std::snprintf(b, sizeof b, "%zx", v);
        return b;
    }
    uint32_t U32()
    {
        if (!Need(4)) return 0;
        uint32_t v;
        std::memcpy(&v, d.data() + o, 4);
        o += 4;
        return v;
    }
    float F32()
    {
        const uint32_t u = U32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    bool Bytes(void* dst, size_t n)
    {
        if (!Need(n)) return false;
        std::memcpy(dst, d.data() + o, n);
        o += n;
        return true;
    }
    bool Magic(const char* m)
    {
        if (!Need(4)) return false;
        if (std::memcmp(d.data() + o, m, 4) != 0) {
            err = std::string("expected '") + m + "' at 0x" + Hex(o);
            return false;
        }
        return true;
    }
};

// One buffer: u32 blocks, then per block u32 count (<= 1024) and count elements.
bool Buffer(Reader& r, const char* name, uint32_t elemSize, std::vector<RplBuffer>& out)
{
    RplBuffer b;
    b.name = name;
    b.elemSize = elemSize;
    b.blocks = r.U32();
    for (uint32_t i = 0; i < b.blocks && r.err.empty(); ++i) {
        const uint32_t n = r.U32();
        if (n > 1024) r.err = std::string(name) + ": block of " + std::to_string(n) + " at 0x" + Reader::Hex(r.o - 4);
        if (!r.Need(size_t(n) * elemSize)) break;
        b.data.insert(b.data.end(), r.d.begin() + r.o, r.d.begin() + r.o + size_t(n) * elemSize);
        r.o += size_t(n) * elemSize;
        b.count += n;
    }
    out.push_back(std::move(b));
    return r.err.empty();
}

// A state buffer and its event buffer behind a u32 (the body's: the sample interval).
bool Pair(Reader& r, const char* name, uint32_t stateSize, uint32_t eventSize, std::vector<RplBuffer>& out, uint32_t* hdr)
{
    const uint32_t h = r.U32();
    if (hdr) *hdr = h;
    const std::string n(name);
    return Buffer(r, (n + ".states").c_str(), stateSize, out) && Buffer(r, (n + ".events").c_str(), eventSize, out);
}

// CMrReplayBody (vslot 9 = 0x60cab0): id +0x78, +0x7c, byte +0x75, then 0x60c630.
bool Body(Reader& r, RplChannel& ch)
{
    const uint32_t id = r.U32();
    ch.a7c = r.U32();
    ch.flags75 = r.U32();
    if (r.err.empty() && id != ch.id) r.err = "channel id " + std::to_string(id) + " != table id " + std::to_string(ch.id);
    return Pair(r, "body", 0x20, 0x70, ch.buffers, &ch.interval);
}

bool Channel(Reader& r, RplChannel& ch)
{
    switch (ch.type) {
    case 1:   // CMrReplayBody
        return Body(r, ch);
    case 2:   // CMrReplayCar (0x613fa0): the body, then +0x250, +0x2bc, +0x20c, +0xc0
        return Body(r, ch) && Pair(r, "car250", 0xc, 0x24, ch.buffers, nullptr) && Pair(r, "car2bc", 0x20, 0x4c, ch.buffers, nullptr) &&
               Buffer(r, "car20c", 0x14, ch.buffers) && Buffer(r, "carC0.a", 0xc, ch.buffers) && Buffer(r, "carC0.b", 0x60, ch.buffers) &&
               Buffer(r, "carC0.c", 0x1c, ch.buffers);
    case 4:   // CMrReplayHingedBody (0x61d0d0): +0xb8, +0x130, then the body
        return Pair(r, "hingeB8", 8, 0x1c, ch.buffers, nullptr) && Buffer(r, "hinge130", 8, ch.buffers) && Body(r, ch);
    default:
        r.err = "unknown channel type " + std::to_string(ch.type);
        return false;
    }
}

} // namespace

uint32_t RplCrc(const uint8_t* p, size_t n)
{
    static uint32_t table[256];
    static bool built;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) table[i] = BitReverse32(i);
        built = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) c = (c >> 8) ^ table[(p[i] ^ c) & 0xff];
    return ~c;
}

const RplBuffer* RplChannel::Find(const std::string& name) const
{
    for (const RplBuffer& b : buffers)
        if (b.name == name) return &b;
    return nullptr;
}

bool ReadReplay(const std::vector<uint8_t>& file, Replay& out, std::string* err)
{
    out = Replay{};
    Reader r{file, 0, file.size(), {}};
    auto fail = [&](const std::string& e) {
        if (err) *err = e;
        return false;
    };
    RplHeader& h = out.header;
    h.crc = r.U32();
    h.size = r.U32();
    if (!r.err.empty()) return fail(r.err);
    if (h.size < 0x4c || h.size > file.size()) return fail("bad size field");
    r.end = h.size;
    if (RplCrc(file.data() + 4, h.size - 4) != h.crc) return fail("checksum mismatch");
    h.version = r.U32();
    if (h.version != 8) return fail("version " + std::to_string(h.version) + " (only 8 is known)");
    r.Bytes(h.nick, 16);
    h.nick[16] = 0;
    h.stage = r.U32();
    h.car = r.U32();
    for (uint32_t& v : h.m1c) v = r.U32();
    h.m34 = r.F32();
    h.m38 = r.F32();
    h.runs = r.U32();
    h.tics = r.U32();
    if (!r.err.empty()) return fail(r.err);
    // The control runs.
    if (!r.Need(size_t(h.runs) * 12)) return fail(r.err);
    out.runs.resize(h.runs);
    uint64_t tics = 0;
    for (RplRun& run : out.runs) {
        const uint8_t* p = file.data() + r.o;
        std::memcpy(&run.tics, p, 2);
        run.posCrc = p[2];
        RplControls& c = run.c;
        c.flags = p[3];
        c.in4 = p[4];
        c.in8 = p[5];
        c.steering = static_cast<int8_t>(p[6]);
        c.throttle = p[7];
        c.brake = p[8];
        c.handbrake = p[9];
        c.clutch = p[10];
        c.in28 = p[11];
        tics += run.tics;
        r.o += 12;
    }
    if (tics != h.tics) return fail("runs add up to " + std::to_string(tics) + " tics, header says " + std::to_string(h.tics));
    // The damage stream, 'HGMD' .. 'HGMD' (its internals: SaveDamageToStream, the RBRTM damage buffer).
    if (!r.Magic("HGMD")) return fail(r.err);
    const size_t dmg0 = r.o;
    size_t dmgEnd = 0;
    for (size_t i = dmg0 + 4; i + 4 <= r.end; ++i)
        if (std::memcmp(file.data() + i, "HGMD", 4) == 0 && i + 4 + 24 + 4 <= r.end && std::memcmp(file.data() + i + 28, "PTSC", 4) == 0) {
            dmgEnd = i + 4;
            break;
        }
    if (!dmgEnd) return fail("no closing 'HGMD' followed by the setup");
    out.damage.assign(file.begin() + dmg0, file.begin() + dmgEnd);
    r.o = dmgEnd;
    for (float& f : out.data14) f = r.F32();
    // The setup ('PTSC', length, .lsp text), then a word 0.
    if (!r.Magic("PTSC")) return fail(r.err);
    r.o += 4;
    const uint32_t len = r.U32();
    if (!r.Need(len)) return fail(r.err);
    out.setup.assign(reinterpret_cast<const char*>(file.data() + r.o), len);
    while (!out.setup.empty() && out.setup.back() == 0) out.setup.pop_back();
    r.o += len;
    if (r.U32() != 0 && r.err.empty()) return fail("expected 0 after the setup");
    // The MrReplay channels (0x619e20): count, dt, {id, type} pairs, then each channel's vslot 9.
    const uint32_t n = r.U32();
    out.channelDt = r.F32();
    if (n > 4096) return fail("channel count " + std::to_string(n));
    out.channels.resize(n);
    for (RplChannel& ch : out.channels) {
        ch.id = r.U32();
        ch.type = r.U32();
    }
    for (RplChannel& ch : out.channels)
        if (!Channel(r, ch)) return fail(r.err);
    if (!r.err.empty()) return fail(r.err);
    if (r.o != h.size) return fail("parsed to 0x" + Reader::Hex(r.o) + ", size is 0x" + Reader::Hex(h.size));
    return true;
}

std::vector<RplPose> Poses(const RplChannel& ch)
{
    std::vector<RplPose> v;
    const RplBuffer* b = ch.Find("body.states");
    if (!b) return v;
    v.resize(b->count);
    std::memcpy(v.data(), b->data.data(), b->data.size());
    return v;
}

} // namespace phys::data
