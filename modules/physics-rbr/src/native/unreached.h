#pragma once
// A move no classic run makes (a vtable slot or a branch without ported code): reported and the program stopped - or,
// with g_unreachedProbe set (a check asking whether a walk can be made), the move skipped and *g_unreachedProbe set.
// Every part of the native physics uses it (library rbr-base, under all the others).

namespace phys::native {

void Unreached(const char* what);
inline thread_local bool* g_unreachedProbe = nullptr;
inline thread_local const char* g_unreachedWhat = nullptr;   // the last move a probe skipped

} // namespace phys::native
