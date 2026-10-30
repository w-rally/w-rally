// ---------------------------------------------------------------------------
// DXC - the shader compiler for anything past shader model 5.1.
//
// Everything in this renderer so far compiles with D3DCompile (FXC), which tops
// out at SM 5.1 and cannot emit DXIL at all. Inline ray tracing needs
// RayQuery, which is SM 6.5. So the ray-tracing path does not begin with an
// acceleration structure - it begins with a second compiler, and that is worth
// knowing before estimating the work (design 1.8.3).
//
// LOADED AT RUNTIME, not linked. A machine without dxcompiler.dll then gets one
// clear log line and a renderer that runs without ray tracing, rather than an
// executable that will not start. The DLL and its signing companion dxil.dll
// are copied next to the binary by ngg/build-ngg.ps1; without dxil.dll the
// compiler still produces DXIL but leaves it UNSIGNED, and the runtime refuses
// unsigned DXIL unless the machine is in developer mode - which is exactly the
// kind of works-here-only failure worth spending a file copy to avoid.
//
// The scene's shaders came over too (design 5.4 N2): their light loop wants
// wave intrinsics, so they compile here first and with FXC only as the
// fallback. FXC stays for everything else on purpose. It works, the shaders it
// compiles are written against it, and porting them wholesale to prove a point
// would be a large diff with no user-visible change.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <initializer_list>
#include <string>

namespace ngg {
namespace core {

class Dxc
{
public:
    // False if the compiler could not be loaded; the caller logs and does
    // without. Safe to call more than once.
    bool Load();
    bool Ready() const { return m_createInstance != nullptr; }

    // `target` is a full profile, e.g. "cs_6_5". Returns the DXIL blob, or null
    // after logging the compiler's own diagnostics - which are far better than
    // anything this wrapper could invent.
    //
    // `extra`: further arguments as they are, e.g. L"-DALPHA_TEST=1".
    Microsoft::WRL::ComPtr<ID3DBlob> Compile(const std::string& source,
                                             const wchar_t* entry,
                                             const wchar_t* target,
                                             const char* nameForLog,
                                             std::initializer_list<const wchar_t*> extra = {});

private:
    HMODULE m_dll = nullptr;
    void*   m_createInstance = nullptr;    // DxcCreateInstanceProc
};

} // namespace core
} // namespace ngg
