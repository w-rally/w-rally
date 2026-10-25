#pragma once
// RBR replay (.rpl) reader. Format version 8 (vanilla 1.02); written by CReplayManager (0x49a970, the serializer used
// both to size and to write) and its MrReplay channels. See doc/physics-standalone-plan.md (S6) for how it was mapped.
//
// A replay does NOT re-run the physics in the game: playback animates the stored states. For us it holds both the
// input (the quantized controls the physics consumed) and the reference (exact state samples).

#include <cstdint>
#include <string>
#include <vector>

namespace phys::data {

// 0x490c00 over a table that 0x490c40 builds as table[i] = bitreverse32(i) (the reflection loop overwrites the CRC it
// just computed - a bug in the original, kept: it is the file's checksum). crc = ~fold(0xffffffff, bytes).
uint32_t RplCrc(const uint8_t* p, size_t n);

// The quantized controls (0x48c910 from the filtered CSigCarControl `in`), stored verbatim per run of tics.
struct RplControls {
    uint8_t flags;       // in +0: event bits, OR-ed within a tic and cleared after it (1 / 2 = gear up / down - tentative)
    uint8_t in4;         // in +4
    uint8_t in8;         // in +8
    int8_t steering;     // in +0x14: cvtt((x + 1) * 127.5 - 128), -128..127
    uint8_t throttle;    // in +0x1c: ftol(x * 255 + 0.5)   (axis names tentative until the dequantizer is read)
    uint8_t brake;       // in +0x20
    uint8_t handbrake;   // in +0x24
    uint8_t clutch;      // in +0x18
    uint8_t in28;        // in +0x28
};

// A run of tics with the same controls (the manager's +0x44 array of 12-byte records).
struct RplRun {
    uint16_t tics;       // the run length; all runs add up to RplHeader::tics
    uint8_t posCrc;      // low byte of RplCrc over the car position xyz (CSigCarPosition) when the run started
    RplControls c;
};

// The file header (0x4c bytes) - the manager fields +0x14.. as written.
struct RplHeader {
    uint32_t crc;        // RplCrc over [4, size)
    uint32_t size;       // the file's meaningful length (older files can have bytes after it)
    uint32_t version;    // 8
    char nick[17];       // 16 bytes in the file
    uint32_t stage;      // manager +0x14 (41 = the stage of phystest.rpl)
    uint32_t car;        // manager +0x18 (the car slot - to be confirmed)
    uint32_t m1c[6];     // manager +0x1c..+0x30, meaning unknown (0 in phystest.rpl)
    float m34;           // manager +0x34
    float m38;           // manager +0x38 (15.0 in phystest.rpl)
    uint32_t runs;       // manager +0x3c + 1: the number of control runs
    uint32_t tics;       // manager +0x40: recorded tics
};

// One recorded buffer of a channel: blocks of up to 1024 fixed-size elements, concatenated here.
struct RplBuffer {
    std::string name;
    uint32_t elemSize = 0;
    uint32_t blocks = 0;
    uint32_t count = 0;
    std::vector<uint8_t> data;   // count * elemSize
    const uint8_t* At(uint32_t i) const { return data.data() + size_t(i) * elemSize; }
};

// A body pose sample (CMrReplayBody state, 0x20 bytes): exact copies of the solid's position (+0x140) and orientation
// (+0x100). Checked on phystest.rpl: sample k == the car after recorded tic 5 k + 4.
struct RplPose {
    float pos[3];
    float unknown;       // changes smoothly (e.g. -96 .. -88 over 16 samples); not identified yet
    float quat[4];
};

// A recorded object (MrReplay): type 1 body (track objects ...), 2 car, 4 hinged body.
struct RplChannel {
    uint32_t id = 0, type = 0;
    uint32_t a7c = 0, flags75 = 0;   // the body's +0x7c and byte +0x75 as written (meaning unknown)
    uint32_t interval = 0;           // tics per state sample (5)
    std::vector<RplBuffer> buffers;  // in file order; buffers[0] / [1] = the body's states (RplPose) / events (0x70)
    const RplBuffer* Find(const std::string& name) const;
};

struct Replay {
    RplHeader header;
    std::vector<RplRun> runs;
    std::vector<uint8_t> damage;     // SaveDamageToStream (0x567930): 'HGMD' .. 'HGMD' (the RBRTM damage buffer format)
    float data14[6] = {};            // replay data +0x14..+0x28 (meaning unknown)
    std::string setup;               // 'PTSC' block: the car setup as .lsp text
    float channelDt = 0;             // seconds per tic (0.007)
    std::vector<RplChannel> channels;
};

// Parses a whole .rpl (checksum, version 8, and every byte up to header.size accounted for). False with *err set otherwise.
bool ReadReplay(const std::vector<uint8_t>& file, Replay& out, std::string* err);

// The pose samples of a channel's body (empty if none).
std::vector<RplPose> Poses(const RplChannel& ch);

static_assert(sizeof(RplPose) == 0x20, "file layout");

} // namespace phys::data
