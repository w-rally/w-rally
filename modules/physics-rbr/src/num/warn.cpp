#include "warn.h"

namespace phys::num {

namespace {
void DefaultWarn(int, int, const char*, double) {}
WarnFn g_warn = &DefaultWarn;
} // namespace

void SetWarner(WarnFn fn) { g_warn = fn ? fn : &DefaultWarn; }

void Warn(int category, int level, const char* format, double value) { g_warn(category, level, format, value); }

} // namespace phys::num
