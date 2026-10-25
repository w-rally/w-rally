// ---------------------------------------------------------------------------
// ngg-core diagnostics.
//
// The library never decides WHERE its output goes - the host points the sink at
// its log file, the viewer at the console. That is not decoration: the launcher
// runs the game with CREATE_NO_WINDOW, so a printf inside the renderer goes
// nowhere at all, and a scene that fails to load is then invisible unless you
// happen to be attached with a debugger. This was learned the hard way.
// ---------------------------------------------------------------------------
#pragma once

namespace ngg {
namespace core {

// Not owned, not copied - a plain function pointer, so the caller keeps control
// of buffering and file lifetime. Null (the default) means output goes only to
// the debugger.
void SetLogSink(void (*sink)(const char*));

void Log(const char* fmt, ...);

} // namespace core
} // namespace ngg
