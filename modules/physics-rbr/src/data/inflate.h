#pragma once
// Raw deflate (RFC 1951) decoding, for the game's .rbz zip archives (physics.rbz: every member deflated). A small
// table-free decoder after zlib's puff.c; enough for data files, not tuned for speed.

#include <cstdint>
#include <vector>

namespace phys::data {

// Decodes `in` (a raw deflate stream) and appends to `out`; `expected` is the size the caller knows (a zip's
// uncompressed size) or 0. Throws std::runtime_error on a malformed stream.
void Inflate(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected = 0);

} // namespace phys::data
