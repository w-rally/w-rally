// A fault's stack, symbolised from the PDB, to stderr (PHYS_SIM_STACK=1; native_sim.cpp's fault filter calls it) - from
// plugin-rbr's sim.cpp, beside the classic Sim it shared that file with.
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdio>
#include <cstring>

namespace phys::sim {

namespace {

void PrintStack(const CONTEXT& ctx)
{
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT c = ctx;
    if (c.Rip < 0x10000) {   // a call through a null pointer: start at its caller (the return address on the stack)
        std::memcpy(&c.Rip, reinterpret_cast<const void*>(c.Rsp), 8);
        c.Rsp += 8;
    }
    STACKFRAME64 f{};
    f.AddrPC.Offset = c.Rip, f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = c.Rbp, f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = c.Rsp, f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &c, nullptr, SymFunctionTableAccess64,
                                          SymGetModuleBase64, nullptr); ++i) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO), sym->MaxNameLen = 255;
        DWORD64 d = 0;
        DWORD d2 = 0;
        IMAGEHLP_LINE64 line{sizeof line};
        const bool named = SymFromAddr(proc, f.AddrPC.Offset, &d, sym);
        const bool lined = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &d2, &line);
        std::fprintf(stderr, "  %-70.70s %s:%lu\n", named ? sym->Name : "?", lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
    }
}


} // namespace

void PrintFaultStack(const void* context) { PrintStack(*static_cast<const CONTEXT*>(context)); }

} // namespace phys::sim
