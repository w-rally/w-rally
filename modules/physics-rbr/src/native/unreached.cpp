#include "native/unreached.h"

#include <cstdio>
#include <cstdlib>

namespace phys::native {

void Unreached(const char* what)
{
    if (g_unreachedProbe) {
        *g_unreachedProbe = true;
        g_unreachedWhat = what;
        return;
    }
    std::fprintf(stderr, "native: %s reached - no classic run made this move, it is not ported\n", what);
    std::abort();
}

} // namespace phys::native
