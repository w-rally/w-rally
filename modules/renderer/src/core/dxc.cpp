#include "dxc.h"
#include "log.h"

#include <dxcapi.h>
#include <d3dcompiler.h>    // D3DCreateBlob, so callers see an ID3DBlob either way
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// A DLL beside the module this code is linked into (WRenderer.dll in WRally), then by the normal search. A bare
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

} // namespace

bool Dxc::Load()
{
    if (m_createInstance) return true;

    m_dll = LoadBeside(L"dxcompiler.dll");
    if (!m_dll)
    {
        Log("dxc: dxcompiler.dll not found - no shader model 6 shaders, so no "
            "inline ray tracing. Everything else is unaffected.");
        return false;
    }
    m_createInstance = (void*)GetProcAddress(m_dll, "DxcCreateInstance");
    if (!m_createInstance)
    {
        Log("dxc: dxcompiler.dll has no DxcCreateInstance");
        FreeLibrary(m_dll);
        m_dll = nullptr;
        return false;
    }

    // Not fatal, but it decides whether the DXIL this produces will be accepted
    // by a driver on a machine that is not in developer mode - so it is said out
    // loud rather than discovered as "the pipeline fails to create".
    if (!GetModuleHandleW(L"dxil.dll") && !LoadBeside(L"dxil.dll"))
        Log("dxc: WARNING - dxil.dll not found. DXIL will be unsigned, and the "
            "runtime rejects unsigned DXIL outside developer mode.");

    Log("dxc: dxcompiler.dll loaded");
    return true;
}

ComPtr<ID3DBlob> Dxc::Compile(const std::string& source, const wchar_t* entry,
                              const wchar_t* target, const char* nameForLog,
                              std::initializer_list<const wchar_t*> extra)
{
    if (!Ready()) return nullptr;

    auto create = (DxcCreateInstanceProc)m_createInstance;

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
    {
        Log("dxc: could not create the compiler objects");
        return nullptr;
    }

    DxcBuffer src = {};
    src.Ptr = source.data();
    src.Size = source.size();
    src.Encoding = DXC_CP_UTF8;

    std::vector<LPCWSTR> args = {
        L"-T", target,
        L"-E", entry,
        // Row-major by default, matching every other shader here and the camera
        // contract in scene.h. DXC's default is column-major, and a silently
        // transposed matrix is the kind of bug that reads as a geometry error.
        L"-Zpr",
#ifdef _DEBUG
        L"-Od", L"-Zi",
#else
        L"-O3",
#endif
    };
    args.insert(args.end(), extra.begin(), extra.end());

    ComPtr<IDxcResult> result;
    if (FAILED(compiler->Compile(&src, args.data(), (UINT32)args.size(),
                                 nullptr, IID_PPV_ARGS(&result))))
    {
        Log("shader '%s': DXC invocation failed", nameForLog);
        return nullptr;
    }

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors && errors->GetStringLength() > 0)
    {
        Log("shader '%s': %s", nameForLog, errors->GetStringPointer());
    }

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (FAILED(status)) return nullptr;

    ComPtr<IDxcBlob> object;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
    {
        Log("shader '%s': DXC produced no object", nameForLog);
        return nullptr;
    }

    // Hand back an ID3DBlob so callers look like the FXC ones. D3D12 takes a
    // pointer and a size either way; copying is the price of one interface.
    ComPtr<ID3DBlob> out;
    if (FAILED(D3DCreateBlob(object->GetBufferSize(), &out))) return nullptr;
    memcpy(out->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
    return out;
}

} // namespace core
} // namespace ngg
