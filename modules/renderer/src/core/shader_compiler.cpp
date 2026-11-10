#include "shader_compiler.h"

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// A DLL beside the module this code is linked into (WRenderer.dll in W-Rally), then by the normal search. A bare
// LoadLibraryW(name) searches the EXE's folder, not ours - and a module DLL's dependencies sit beside the module.
HMODULE LoadBeside(const wchar_t* name)
{
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LoadBeside), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH))
    {
        std::wstring p(path);
        p.erase(p.find_last_of(L"\\/") + 1);
        p += name;
        // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: dxcompiler.dll's own dependencies from its folder too.
        if (HMODULE m = LoadLibraryExW(p.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            return m;
    }
    return LoadLibraryW(name);
}

HMODULE LoadFrom(const wchar_t* dir, const wchar_t* name)
{
    std::wstring p(dir);
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    p += name;
    return LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII: entry points, args

} // namespace

bool ShaderBytecodeSigned(const void* data, size_t size)
{
    if (!data || size < 20) return false;
    static const unsigned char zero[16] = {};
    return std::memcmp(static_cast<const unsigned char*>(data) + 4, zero, 16) != 0;
}

bool ShaderCompiler::LoadDxc(const wchar_t* dir)
{
    if (m_createInstance) return true;
    m_dll = dir ? LoadFrom(dir, L"dxcompiler.dll") : LoadBeside(L"dxcompiler.dll");
    if (!m_dll) return false;
    m_createInstance = (void*)GetProcAddress(m_dll, "DxcCreateInstance");
    if (!m_createInstance)
    {
        FreeLibrary(m_dll);
        m_dll = nullptr;
        return false;
    }
    // dxcompiler.dll loads dxil.dll by name when it signs; loaded here first from the same place, it is found.
    m_dxilSigner = GetModuleHandleW(L"dxil.dll") || (dir ? LoadFrom(dir, L"dxil.dll") : LoadBeside(L"dxil.dll"));
    return true;
}

ComPtr<ID3DBlob> ShaderCompiler::Compile(const std::wstring& path, const std::string& entry, const std::string& profile,
                                         const std::vector<std::string>& args, bool debug, std::string& errors)
{
    errors.clear();
    // "ps_5_1" -> FXC, "ps_6_0" -> DXC: the profile's major version.
    const size_t us = profile.find('_');
    const bool sm6 = us != std::string::npos && us + 1 < profile.size() && profile[us + 1] >= '6';
    return sm6 ? CompileDxc(path, entry, profile, args, debug, errors)
               : CompileFxc(path, entry, profile, args, debug, errors);
}

ComPtr<ID3DBlob> ShaderCompiler::CompileFxc(const std::wstring& path, const std::string& entry,
                                            const std::string& profile, const std::vector<std::string>& args,
                                            bool debug, std::string& errors)
{
    // The -D arguments as macros ("-DNAME=VALUE", "-DNAME" = 1).
    std::vector<std::string> names, values;
    for (const std::string& a : args)
    {
        if (a.size() < 3 || a.compare(0, 2, "-D") != 0) continue;
        const size_t eq = a.find('=');
        names.push_back(a.substr(2, eq == std::string::npos ? std::string::npos : eq - 2));
        values.push_back(eq == std::string::npos ? "1" : a.substr(eq + 1));
    }
    std::vector<D3D_SHADER_MACRO> defs;
    for (size_t i = 0; i < names.size(); ++i) defs.push_back({ names[i].c_str(), values[i].c_str() });
    defs.push_back({ nullptr, nullptr });

    // -O0..-O3 as DXC's: FXC's optimisation levels (its default is 1).
    UINT flags = 0;
    for (const std::string& a : args)
        if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3')
            flags = a[2] == '0' ? D3DCOMPILE_OPTIMIZATION_LEVEL0 : a[2] == '1' ? D3DCOMPILE_OPTIMIZATION_LEVEL1
                  : a[2] == '2' ? D3DCOMPILE_OPTIMIZATION_LEVEL2 : D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (debug) flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    ComPtr<ID3DBlob> out, err;
    const HRESULT hr = D3DCompileFromFile(path.c_str(), defs.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry.c_str(),
                                          profile.c_str(), flags, 0, &out, &err);
    if (err && err->GetBufferSize()) errors.assign((const char*)err->GetBufferPointer(), err->GetBufferSize() - 1);
    if (FAILED(hr))
    {
        if (errors.empty()) errors = "FXC failed (the file could not be read?)";
        return nullptr;
    }
    return out;
}

ComPtr<ID3DBlob> ShaderCompiler::CompileDxc(const std::wstring& path, const std::string& entry,
                                            const std::string& profile, const std::vector<std::string>& args,
                                            bool debug, std::string& errors)
{
    if (!m_createInstance)
    {
        errors = "DXC: dxcompiler.dll is not loaded";
        return nullptr;
    }
    auto create = (DxcCreateInstanceProc)m_createInstance;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includes;
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
        FAILED(utils->CreateDefaultIncludeHandler(&includes)))
    {
        errors = "DXC: could not create the compiler objects";
        return nullptr;
    }
    ComPtr<IDxcBlobEncoding> file;
    if (FAILED(utils->LoadFile(path.c_str(), nullptr, &file)))
    {
        errors = "DXC: the file could not be read";
        return nullptr;
    }
    DxcBuffer src = {};
    src.Ptr = file->GetBufferPointer();
    src.Size = file->GetBufferSize();
    src.Encoding = DXC_CP_UTF8;

    const std::wstring wentry = Widen(entry), wprofile = Widen(profile);
    std::vector<std::wstring> wargs;
    for (const std::string& a : args) wargs.push_back(Widen(a));
    std::vector<LPCWSTR> argv = {
        L"-T", wprofile.c_str(),
        L"-E", wentry.c_str(),
        // Row-major by default, matching every other shader here and the camera contract in scene.h. DXC's default
        // is column-major, and a silently transposed matrix is the kind of bug that reads as a geometry error.
        L"-Zpr",
    };
    if (debug) { argv.push_back(L"-Od"); argv.push_back(L"-Zi"); }
    else argv.push_back(L"-O3");
    for (const std::wstring& a : wargs) argv.push_back(a.c_str());
    argv.push_back(path.c_str());    // the source's name: its includes are found relative to it

    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&src, argv.data(), (UINT32)argv.size(), includes.Get(), IID_PPV_ARGS(&result))))
    {
        errors = "DXC invocation failed";
        return nullptr;
    }
    ComPtr<IDxcBlobUtf8> diag;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diag), nullptr)) && diag &&
        diag->GetStringLength() > 0)
        errors.assign(diag->GetStringPointer(), diag->GetStringLength());
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) return nullptr;

    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
    {
        errors = "DXC produced no object";
        return nullptr;
    }
    // An ID3DBlob, as FXC's: D3D12 takes a pointer and a size either way.
    ComPtr<ID3DBlob> out;
    if (FAILED(D3DCreateBlob(object->GetBufferSize(), &out))) return nullptr;
    std::memcpy(out->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
    return out;
}

} // namespace core
} // namespace ngg
