// The original's warnings (0x43c8e0: category, level, printf format, one double): some numerics report invalid inputs
// through them (FloorS32's "Invalid argument"). Nothing by default; a host may hand them on.
#pragma once

namespace phys::num {

using WarnFn = void (*)(int category, int level, const char* format, double value);

void SetWarner(WarnFn fn);   // nullptr restores the default (ignore)
void Warn(int category, int level, const char* format, double value);

} // namespace phys::num
