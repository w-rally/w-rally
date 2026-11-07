#pragma once
// A picture to a .png file (WIC, so Windows' own encoder - no codec of our own).

#include <cstddef>
#include <string>

namespace wrally::png {

// 24-bit BGR rows, top row first, `stride` bytes apart. The path as fopen takes it (the ANSI code page).
// False when the file could not be written.
bool WriteBgr(const std::string& path, int width, int height, const unsigned char* bgr, size_t stride);

} // namespace wrally::png
