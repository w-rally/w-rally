// ---------------------------------------------------------------------------
// ngg-shaderc: the renderer's shaders compiled at build time (modules/renderer/CMakeLists.txt runs it).
//
//   ngg-shaderc --manifest shaders.txt --dir <shaders> --dxc <folder of dxcompiler.dll + dxil.dll> --out <file.cpp>
//
// Every line of the manifest (shaders/shaders.txt) compiled with core/shader_compiler.cpp - the very code, compilers
// and arguments of the renderer's run-time compile (NGG_SHADER_DIR) - in parallel, and written out as a C++ source
// of byte arrays and a sorted table (core/shaders_embedded.h) that is linked into WRenderer.dll. Any error fails the
// build; so does DXIL that DXC could not sign (dxil.dll missing: the runtime would refuse it outside developer mode).
// ---------------------------------------------------------------------------
#include "shader_compiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using ngg::core::ShaderCompiler;

namespace {

struct Entry
{
    std::string              file, entry, profile;
    std::vector<std::string> args;
    std::string              key;      // the tokens one space apart
    int                      line = 0;
    std::vector<unsigned char> bytes;
    std::string              errors;
    bool                     ok = false;
};

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool IsSm6(const std::string& profile)
{
    const size_t us = profile.find('_');
    return us != std::string::npos && us + 1 < profile.size() && profile[us + 1] >= '6';
}

int Usage()
{
    std::fprintf(stderr, "usage: ngg-shaderc --manifest <shaders.txt> --dir <shader folder> --dxc <dxcompiler folder> "
                         "--out <generated.cpp>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    std::string manifest, dir, dxc, out;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i];
        if (k == "--manifest") manifest = argv[i + 1];
        else if (k == "--dir") dir = argv[i + 1];
        else if (k == "--dxc") dxc = argv[i + 1];
        else if (k == "--out") out = argv[i + 1];
        else return Usage();
    }
    if (manifest.empty() || dir.empty() || dxc.empty() || out.empty()) return Usage();
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    const auto t0 = std::chrono::steady_clock::now();

    std::ifstream in(manifest);
    if (!in)
    {
        std::fprintf(stderr, "ngg-shaderc: cannot read %s\n", manifest.c_str());
        return 1;
    }
    std::vector<Entry> entries;
    std::string text;
    for (int line = 1; std::getline(in, text); ++line)
    {
        if (const size_t hash = text.find('#'); hash != std::string::npos) text.erase(hash);
        std::istringstream tokens(text);
        std::vector<std::string> t;
        for (std::string w; tokens >> w;) t.push_back(w);
        if (t.empty()) continue;
        if (t.size() < 3)
        {
            std::fprintf(stderr, "%s(%d): error: <file> <entry> <profile> [args] expected\n", manifest.c_str(), line);
            return 1;
        }
        Entry e;
        e.file = t[0];
        e.entry = t[1];
        e.profile = t[2];
        e.args.assign(t.begin() + 3, t.end());
        for (const std::string& w : t) e.key += (e.key.empty() ? "" : " ") + w;
        e.line = line;
        for (const Entry& o : entries)
            if (o.key == e.key)
            {
                std::fprintf(stderr, "%s(%d): error: listed twice (line %d)\n", manifest.c_str(), line, o.line);
                return 1;
            }
        entries.push_back(std::move(e));
    }

    ShaderCompiler compiler;
    const bool needDxc = std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return IsSm6(e.profile); });
    if (needDxc)
    {
        if (!compiler.LoadDxc(Widen(dxc).c_str()))
        {
            std::fprintf(stderr, "ngg-shaderc: error: no dxcompiler.dll in %s\n", dxc.c_str());
            return 1;
        }
        if (!compiler.DxilSigned())
        {
            std::fprintf(stderr, "ngg-shaderc: error: no dxil.dll beside dxcompiler.dll in %s - the DXIL would be "
                                 "unsigned, which the runtime refuses outside developer mode\n", dxc.c_str());
            return 1;
        }
    }

    if (needDxc)
    {
        // Which DLLs signed: a dxil.dll from elsewhere (an SDK on PATH) gives other bytes.
        for (const wchar_t* name : { L"dxcompiler.dll", L"dxil.dll" })
        {
            wchar_t path[MAX_PATH] = L"?";
            if (HMODULE m = GetModuleHandleW(name)) GetModuleFileNameW(m, path, MAX_PATH);
            std::printf("ngg-shaderc: %ls\n", path);
        }
    }
    // In parallel: a DXC compiler object per call (shader_compiler.cpp), FXC is thread-safe.
    std::atomic<size_t> next{ 0 };
    auto work = [&]() {
        for (size_t i; (i = next++) < entries.size();)
        {
            Entry& e = entries[i];
            ComPtr<ID3DBlob> blob = compiler.Compile(Widen(dir + e.file), e.entry, e.profile, e.args, false, e.errors);
            if (!blob) continue;
            const unsigned char* p = static_cast<const unsigned char*>(blob->GetBufferPointer());
            e.bytes.assign(p, p + blob->GetBufferSize());
            e.ok = !IsSm6(e.profile) || ngg::core::ShaderBytecodeSigned(p, e.bytes.size());
            if (!e.ok) e.errors = "the DXIL is not signed";
        }
    };
    std::vector<std::thread> pool;
    unsigned threads = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    if (const char* t = std::getenv("NGG_SHADERC_THREADS")) threads = std::max(1, std::atoi(t));
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(work);
    for (std::thread& t : pool) t.join();

    int failed = 0;
    for (const Entry& e : entries)
        if (!e.ok)
        {
            ++failed;
            std::fprintf(stderr, "%s(%d): error: %s: %s\n", manifest.c_str(), e.line, e.key.c_str(),
                         e.errors.empty() ? "did not compile" : e.errors.c_str());
        }
    if (failed) return 1;

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
    uint64_t hash = 1469598103934665603ull;
    auto fnv = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) hash = (hash ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
    };
    std::string src;
    src.reserve(4u << 20);
    src += "// Generated by ngg-shaderc (modules/renderer/src/shaderc) from modules/renderer/shaders/shaders.txt.\n"
           "// Do not edit: edit the shaders, the build compiles them again.\n"
           "#include \"shaders_embedded.h\"\n\nnamespace ngg {\nnamespace core {\nnamespace embedded {\n\nnamespace {\n";
    size_t total = 0;
    char num[8];
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        fnv(e.key.data(), e.key.size() + 1);
        fnv(e.bytes.data(), e.bytes.size());
        total += e.bytes.size();
        src += "// " + e.key + "\nalignas(16) const unsigned char s" + std::to_string(i) + "[] = {";
        for (size_t b = 0; b < e.bytes.size(); ++b)
        {
            if (b % 32 == 0) src += "\n";
            std::snprintf(num, sizeof num, "%u,", e.bytes[b]);
            src += num;
        }
        src += "\n};\n";
    }
    src += "} // namespace\n\nconst Shader kShaders[] = {\n";
    for (size_t i = 0; i < entries.size(); ++i)
        src += "    { \"" + entries[i].key + "\", s" + std::to_string(i) + ", sizeof s" + std::to_string(i) + " },\n";
    char tail[160];
    std::snprintf(tail, sizeof tail, "};\nconst size_t kShaderCount = %zu;\nconst uint64_t kShaderSetHash = 0x%016llxull;\n",
                  entries.size(), (unsigned long long)hash);
    src += tail;
    src += "\n} // namespace embedded\n} // namespace core\n} // namespace ngg\n";

    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o.write(src.data(), (std::streamsize)src.size()))
    {
        std::fprintf(stderr, "ngg-shaderc: cannot write %s\n", out.c_str());
        return 1;
    }
    size_t dxil = 0;
    for (const Entry& e : entries) dxil += IsSm6(e.profile) ? 1 : 0;
    std::printf("ngg-shaderc: %zu shaders (%zu DXIL, %zu DXBC), %zu KB, set %016llx, in %.1f s (%u threads)\n",
                entries.size(), dxil, entries.size() - dxil, total / 1024, (unsigned long long)hash,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), threads);
    return 0;
}
