#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace ngg {
namespace core {

namespace {
void (*g_sink)(const char*) = nullptr;
}

void SetLogSink(void (*sink)(const char*)) { g_sink = sink; }

void Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (g_sink) g_sink(buf);

    // Always, even with a sink attached: when the renderer dies mid-frame the
    // debugger output is often the only record that survives.
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

} // namespace core
} // namespace ngg
