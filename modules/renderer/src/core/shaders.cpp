#include "shaders.h"
#include "shader_compiler.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

double NowMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

struct Library
{
    std::mutex     lock;
    bool           init = false;
    std::wstring   dir;               // the shader files, with a trailing slash
    ShaderCompiler compiler;
    bool           dxcTried = false;
    int            count = 0, failed = 0;
    double         ms = 0;

    void Init()
    {
        if (init) return;
        init = true;
        if (const char* d = std::getenv("NGG_SHADER_DIR"); d && *d)
        {
            dir = Widen(d);
            Log("shaders: compiled at run time from %s (NGG_SHADER_DIR)", d);
        }
        else
            dir = Widen(NGG_SHADER_SRC_DIR);
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    }
    bool Dxc()
    {
        if (!dxcTried)
        {
            dxcTried = true;
            if (!compiler.LoadDxc())
                Log("shaders: dxcompiler.dll not found - no shader model 6 shaders, so no inline ray tracing");
            else if (!compiler.DxilSigned())
                Log("shaders: WARNING - dxil.dll not found. DXIL will be unsigned, and the runtime rejects unsigned "
                    "DXIL outside developer mode.");
        }
        return compiler.DxcReady();
    }
};

Library& Lib()
{
    static Library lib;
    return lib;
}

// NGG_SHADER_DUMP=<dir>: every shader as it is handed out, as <key>.bin (spaces '.', '=' '_') - for RGA, and to
// compare two builds' shaders byte for byte.
void Dump(const std::string& key, ID3DBlob* blob)
{
    static const char* dir = std::getenv("NGG_SHADER_DUMP");
    if (!dir || !*dir || !blob) return;
    std::string name = key;
    for (char& c : name) c = c == ' ' ? '.' : c == '=' ? '_' : c;
    if (FILE* f = fopen((std::string(dir) + "/" + name + ".bin").c_str(), "wb"))
    {
        fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f);
        fclose(f);
    }
}

bool IsSm6(const char* profile)
{
    const char* us = std::strchr(profile, '_');
    return us && us[1] >= '6';
}

} // namespace

ComPtr<ID3DBlob> LoadShader(const char* file, const char* entry, const char* profile,
                            std::initializer_list<const char*> args)
{
    return LoadShader(file, entry, profile, std::vector<std::string>(args.begin(), args.end()));
}

ComPtr<ID3DBlob> LoadShader(const char* file, const char* entry, const char* profile,
                            const std::vector<std::string>& args)
{
    Library& lib = Lib();
    std::lock_guard<std::mutex> hold(lib.lock);
    lib.Init();
    std::string key = std::string(file) + " " + entry + " " + profile;
    for (const std::string& a : args) key += " " + a;

    if (IsSm6(profile) && !lib.Dxc())
    {
        Log("shader %s: shader model 6 needs dxcompiler.dll", key.c_str());
        return nullptr;
    }
    const double t0 = NowMs();
#ifdef _DEBUG
    const bool debug = true;
#else
    const bool debug = false;
#endif
    std::string errors;
    ComPtr<ID3DBlob> out = lib.compiler.Compile(lib.dir + Widen(file), entry, profile, args, debug, errors);
    lib.ms += NowMs() - t0;
    ++lib.count;
    // DXC's diagnostics always (warnings too, as they always were); FXC's when it failed - its X4000 "potentially
    // uninitialized" warnings on out parameters are noise the renderer never printed.
    if (!errors.empty() && (!out || IsSm6(profile))) Log("shader %s: %s", key.c_str(), errors.c_str());
    if (!out) ++lib.failed;
    Dump(key, out.Get());
    return out;
}

bool ShaderModel6Available()
{
    Library& lib = Lib();
    std::lock_guard<std::mutex> hold(lib.lock);
    lib.Init();
    return lib.Dxc();
}

std::string FlattenedShaderSource(const char* file)
{
    Library& lib = Lib();
    std::wstring dir;
    {
        std::lock_guard<std::mutex> hold(lib.lock);
        lib.Init();
        dir = lib.dir;
    }
    // #include "x" lines replaced by the file, relative to the including one; each file once (the guards' job).
    std::vector<std::wstring> seen;
    std::string out;
    auto read = [](const std::wstring& p, std::string& s) {
        FILE* f = _wfopen(p.c_str(), L"rb");
        if (!f) return false;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
        return true;
    };
    auto walk = [&](auto&& self, const std::wstring& path) -> bool {
        for (const std::wstring& s : seen) if (_wcsicmp(s.c_str(), path.c_str()) == 0) return true;
        seen.push_back(path);
        std::string s;
        if (!read(path, s)) return false;
        const std::wstring base = path.substr(0, path.find_last_of(L"\\/") + 1);
        size_t at = 0;
        while (at < s.size())
        {
            size_t eol = s.find('\n', at);
            if (eol == std::string::npos) eol = s.size();
            const std::string line = s.substr(at, eol - at);
            const size_t q0 = line.find("#include \""), q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 10);
            if (q0 != std::string::npos && q1 != std::string::npos && line.find_first_not_of(" \t") == q0)
            {
                if (!self(self, base + Widen(line.substr(q0 + 10, q1 - q0 - 10)))) return false;
            }
            else
                out += line + "\n";
            at = eol + 1;
        }
        return true;
    };
    if (!walk(walk, dir + Widen(file))) return {};
    return out;
}

void LogShaderStats(const char* when)
{
    Library& lib = Lib();
    std::lock_guard<std::mutex> hold(lib.lock);
    if (!lib.count) return;
    Log("shaders: %d compiled at run time%s in %.0f ms (%s)", lib.count,
        lib.failed ? " (some FAILED)" : "", lib.ms, when);
    lib.count = lib.failed = 0;
    lib.ms = 0;
}

} // namespace core
} // namespace ngg
