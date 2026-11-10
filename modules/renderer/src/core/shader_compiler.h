// ---------------------------------------------------------------------------
// The shader compilers, for a shader file in modules/renderer/shaders: FXC (D3DCompile, shader model 5.x, DXBC) and
// DXC (dxcompiler.dll, shader model 6.x, DXIL - RayQuery and wave intrinsics need it).
//
// TWO USERS, ONE CODE PATH. The build's ngg-shaderc (src/shaderc) compiles every shader in shaders.txt with this and
// embeds the bytes in WRenderer.dll; the renderer compiles with it only in the development mode (NGG_SHADER_DIR,
// shaders.h). The same arguments in the same order, so a shader compiled at run time is the very bytes the build
// embedded - which is how the move out of the C++ string literals was proven a pure move.
//
// DXC is LOADED AT RUN TIME, not linked: dxcompiler.dll and its signing companion dxil.dll (third_party/dxc). Without
// dxil.dll DXC still produces DXIL but leaves it UNSIGNED, and the runtime rejects unsigned DXIL unless the machine is
// in developer mode - the build tool therefore refuses unsigned output (a shipped game never compiles a shader, so it
// needs neither DLL).
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3dcommon.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace ngg {
namespace core {

class ShaderCompiler
{
public:
    // dxcompiler.dll from `dir` (and its dxil.dll from there), or null: beside the module this code is linked into
    // (WRenderer.dll), then by the normal search. False if it could not be loaded; FXC works without it. Safe to call
    // more than once.
    bool LoadDxc(const wchar_t* dir = nullptr);
    bool DxcReady() const { return m_createInstance != nullptr; }
    bool DxilSigned() const { return m_dxilSigner; }

    // `path`: the shader file (its includes are found beside it, or by their relative path). `profile`: "ps_5_1" -
    // FXC - or "cs_6_5" - DXC. `args`: further compiler arguments as DXC takes them ("-DNAME=VALUE", "-HV", "2018");
    // FXC takes the -D ones and -O0..-O3 and ignores the others. `debug`: no optimisation, debug info (the Debug
    // build's run-time compile). Returns the bytecode, or null with the compiler's own diagnostics in `errors`
    // (warnings land there too on success).
    Microsoft::WRL::ComPtr<ID3DBlob> Compile(const std::wstring& path, const std::string& entry,
                                             const std::string& profile, const std::vector<std::string>& args,
                                             bool debug, std::string& errors);

private:
    Microsoft::WRL::ComPtr<ID3DBlob> CompileFxc(const std::wstring& path, const std::string& entry,
                                                const std::string& profile, const std::vector<std::string>& args,
                                                bool debug, std::string& errors);
    Microsoft::WRL::ComPtr<ID3DBlob> CompileDxc(const std::wstring& path, const std::string& entry,
                                                const std::string& profile, const std::vector<std::string>& args,
                                                bool debug, std::string& errors);

    HMODULE m_dll = nullptr;
    void*   m_createInstance = nullptr;    // DxcCreateInstanceProc
    bool    m_dxilSigner = false;          // dxil.dll was found: DXC signs what it compiles
};

// A DXIL container is signed when its digest (bytes 4..19) is not all zero. DXBC always carries its checksum there.
bool ShaderBytecodeSigned(const void* data, size_t size);

} // namespace core
} // namespace ngg
