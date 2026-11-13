// wind-reference: the renderer's wind (modules/renderer/shaders/wind.hlsli) against its reference, Wallaby's
// tools/vegetation/plantgen/wind.py, on the golden table that wind.golden() writes (tests/data/wind_golden.json:
// 64 rows, bases thousands of metres out, times up to an hour). NGG design §2.6.
//
//   wind-reference <wind_golden.json> <tests dir, holding wind_reference.hlsl> <dxc dir>
//
// Two steps, because the golden table's inputs are doubles and the shader's are float32. Rounding the inputs alone
// moves wind.py's answer by up to ~2.4 mm: uv1.y packs the phase beside the flutter (~1e-5 of phase), and the phase
// also sets a branch's frequency, so after an hour the motion is that of a phase a hair off - constant per vertex,
// nothing a viewer sees, but more than this test's millimetre. So:
//   1. A C++ port of wind.py's displace in double must give the golden outputs from the golden inputs (1 um): it IS
//      the reference.
//   2. The shader, on a D3D12 compute queue, must give that reference's outputs from the same inputs rounded to
//      float32 within 1 mm - every row, which is where a port that loses float precision far out and late fails.
// It prints the shader's distance from the golden outputs themselves beside it (the inputs' rounding, mostly).
//   3. The golden rows rarely meet the worst case, so 512 more rows made here take it: the last 100 s of the hour, a
//      20 m/s wind at gustiness 1, short trees that lean far (bend 0.6 m, 1-3 m tall, the vertex at the top), full
//      branch and flutter weights, bases up to 3 km out - against the double reference on their float32 inputs, 1 mm.
//      Checked that it bites (2026-11-13, AMD iGPU): the shader 0.06 mm on the golden rows and 0.63 mm here; with
//      frac(f t) of the float product, or 2.2 / height as one float, 1.2-1.3 mm here (the golden rows pass either way);
//      the float base * 100 rounded in float hashed 9 of the 512 placements to another centimetre than Python.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "shader_compiler.h"
#include "wrally/formats/json.h"

using Microsoft::WRL::ComPtr;

namespace {

struct In {
    double pos[3], nrm[3], attr[2], h, base[3], height, bend, t, dir[3], speed, gust, out[3];
};

#pragma pack(push, 4)
struct GpuRow {
    float base[3], v[3], nrm[3], attr[2], h, height, bend, t, wind[4], pad;
};
#pragma pack(pop)
static_assert(sizeof(GpuRow) == 80, "GpuRow is wind_reference.hlsl's Row");

const double kTau = 6.283185307179586;

uint32_t Pcg(uint32_t v)
{
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

double PlacementPhase(const double base[3])
{
    const uint32_t qa = uint32_t(int64_t(std::nearbyint(base[0] * 100.0)));   // round half even, as Python's round()
    const uint32_t qb = uint32_t(int64_t(std::nearbyint(base[2] * 100.0)));
    return double(Pcg(qa ^ Pcg(qb)) & 0xFFFFFFu) / 16777216.0;
}

double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// wind.py displace, y up, in double: the bent position.
void Displace(const In& r, double out[3])
{
    double d[3] = {r.dir[0], 0.0, r.dir[2]};
    const double dl = std::sqrt(Dot(d, d));
    for (double& c : d) c /= dl > 1e-9 ? dl : 1e-9;
    const double whole = std::floor(r.attr[1]), ph = r.attr[1] - whole, fl = whole / 100.0, w = r.attr[0];
    const double php = PlacementPhase(r.base);
    const double s = r.speed / 10.0;
    const double gust = 1.0 + r.gust * (0.5 + 0.5 * std::sin(kTau * (0.12 * r.t - 0.02 * Dot(r.pos, d))));
    const double f0 = 2.2 / (r.height > 0.5 ? r.height : 0.5);
    const double lean = r.bend * s * s * gust * (0.75 + 0.25 * std::sin(kTau * (f0 * r.t + php)));
    double hf = r.h / (r.height > 1e-6 ? r.height : 1e-6);
    hf = hf < 0.0 ? 0.0 : hf > 1.0 ? 1.0 : hf;
    double v[3], bent[3];
    for (int k = 0; k < 3; ++k) v[k] = r.pos[k] - r.base[k];
    const double len = std::sqrt(Dot(v, v));
    for (int k = 0; k < 3; ++k) bent[k] = v[k] + d[k] * lean * hf * hf;
    const double bl = std::sqrt(Dot(bent, bent));
    for (double& c : bent) c *= len / (bl > 1e-9 ? bl : 1e-9);
    const double fb = 0.3 * (1.0 + 0.4 * ph);
    const double ab = w * 0.25 * s * gust;
    const double along = 0.6 * (0.5 + 0.5 * std::sin(kTau * (fb * r.t + ph + php)));
    const double bob = 0.4 * std::sin(kTau * (1.3 * fb * r.t + 1.7 * ph + php));
    const double ff = 2.5 * (1.0 + 0.5 * ph);
    const double kv[3] = {0.7, 1.3, 0.9};
    const double flut = fl * 0.025 * s * gust * std::sin(kTau * (ff * r.t + 7.3 * ph + Dot(r.pos, kv)));
    for (int k = 0; k < 3; ++k)
        out[k] = r.base[k] + bent[k] + ab * (d[k] * along + (k == 1 ? bob : 0.0)) + flut * r.nrm[k];
}

double Dist(const double a[3], const double b[3])
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

int Fail(const std::string& why)
{
    std::printf("wind-reference: FAILED - %s\n", why.c_str());
    return 1;
}

bool Vec(const wrally::formats::Json& row, const char* key, double* out, size_t n)
{
    const wrally::formats::Json* a = row.Get(key);
    if (!a || a->type != wrally::formats::Json::Array || a->items.size() != n) return false;
    for (size_t k = 0; k < n; ++k) out[k] = a->items[k].number;
    return true;
}

// The shader over the rows on the first hardware adapter (WARP if none): a float4 per row (the bent offset, w the
// placement's phase).
std::string RunGpu(const std::wstring& shaderPath, const std::wstring& dxcDir, const std::vector<GpuRow>& rows,
                   std::vector<float>& out, std::string& adapterName)
{
    ngg::core::ShaderCompiler sc;
    if (!sc.LoadDxc(dxcDir.c_str())) return "dxcompiler.dll not loaded from the dxc folder";
    std::string errors;
    ComPtr<ID3DBlob> cs = sc.Compile(shaderPath, "CSMain", "cs_6_0", {"-HV", "2018"}, false, errors);
    if (!cs) return "the shader did not compile:\n" + errors;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "no DXGI factory";
    ComPtr<ID3D12Device> dev;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
            adapterName = name;
            break;
        }
    }
    if (!dev) {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
            return "no D3D12 device";
        adapterName = "WARP";
    }

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    ComPtr<ID3D12RootSignature> rs;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr)) ||
        FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rs))))
        return "no root signature";
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs.Get();
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return "no compute pipeline";

    auto buffer = [&](D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = heap;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = flags;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    };
    const UINT64 inBytes = rows.size() * sizeof(GpuRow), outBytes = rows.size() * 16;
    ComPtr<ID3D12Resource> in = buffer(D3D12_HEAP_TYPE_UPLOAD, inBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> uav = buffer(D3D12_HEAP_TYPE_DEFAULT, outBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> rb = buffer(D3D12_HEAP_TYPE_READBACK, outBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!in || !uav || !rb) return "no buffers";
    void* p = nullptr;
    in->Map(0, nullptr, &p);
    std::memcpy(p, rows.data(), size_t(inBytes));
    in->Unmap(0, nullptr);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return "no command list";
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootShaderResourceView(0, in->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, uav->GetGPUVirtualAddress());
    list->Dispatch(UINT((rows.size() + 63) / 64), 1, 1);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = uav.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);
    list->CopyResource(rb.Get(), uav.Get());
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    if (fence->GetCompletedValue() != 1) return "the GPU did not finish";
    out.resize(rows.size() * 4);
    rb->Map(0, nullptr, &p);
    std::memcpy(out.data(), p, size_t(outBytes));
    rb->Unmap(0, nullptr);
    return "";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) return Fail("usage: wind-reference <wind_golden.json> <tests dir> <dxc dir>");
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const wrally::formats::Json root = wrally::formats::ParseJson(ss.str());
    if (root.Str("format") != "wallaby-wind-golden" || root.Num("version") != 1 || root.Str("up") != "y")
        return Fail(std::string(argv[1]) + " is not a wallaby-wind-golden version 1 table, y up");
    const wrally::formats::Json* rowsJ = root.Get("rows");
    if (!rowsJ || rowsJ->items.empty()) return Fail("no rows");

    std::vector<In> rows;
    size_t goldenRows = 0;
    for (const wrally::formats::Json& r : rowsJ->items) {
        In in{};
        if (!Vec(r, "pos", in.pos, 3) || !Vec(r, "nrm", in.nrm, 3) || !Vec(r, "wind", in.attr, 2) || !Vec(r, "base", in.base, 3) ||
            !Vec(r, "direction", in.dir, 3) || !Vec(r, "out", in.out, 3))
            return Fail("a row is missing a vector");
        in.h = r.Num("h"), in.height = r.Num("height"), in.bend = r.Num("bend"), in.t = r.Num("t");
        in.speed = r.Num("speed"), in.gust = r.Num("gustiness");
        rows.push_back(in);
    }
    goldenRows = rows.size();
    uint32_t seed = 12345u;
    auto rnd = [&](double lo, double hi) { seed = seed * 1664525u + 1013904223u; return lo + (hi - lo) * (seed >> 8) / 16777216.0; };
    for (int i = 0; i < 512; ++i) {
        In in{};
        in.base[0] = rnd(-3000, 3000), in.base[1] = rnd(-50, 900), in.base[2] = rnd(-3000, 3000);
        in.height = rnd(1.0, 3.0), in.bend = 0.6, in.h = in.height;
        const double ang = rnd(0, kTau);
        const double local[3] = {rnd(-1, 1), in.h, rnd(-1, 1)};
        for (int k = 0; k < 3; ++k) in.pos[k] = in.base[k] + local[k];
        const double n[3] = {rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
        const double nl = std::sqrt(Dot(n, n)) + 1e-9;
        for (int k = 0; k < 3; ++k) in.nrm[k] = n[k] / nl;
        in.attr[0] = 1.0, in.attr[1] = 100.0 + rnd(0, 1);
        in.t = rnd(3500, 3600);
        in.dir[0] = std::cos(ang), in.dir[1] = 0.0, in.dir[2] = std::sin(ang);
        in.speed = 20.0, in.gust = 1.0;
        Displace(in, in.out);
        rows.push_back(in);
    }

    // 1. The double port is the reference.
    double worstRef = 0.0;
    for (size_t i = 0; i < goldenRows; ++i) {
        const In& r = rows[i];
        double o[3];
        Displace(r, o);
        worstRef = (std::max)(worstRef, Dist(o, r.out));
    }
    std::printf("the C++ port of wind.py in double: %zu rows, at most %.3g m from the golden outputs\n", goldenRows, worstRef);
    if (worstRef > 1e-6) return Fail("the double port does not reproduce the golden table");

    // 2. The inputs as the shader holds them (float32), the reference on exactly those, and the shader.
    std::vector<GpuRow> gpu(rows.size());
    std::vector<In> rounded = rows;
    for (size_t i = 0; i < rows.size(); ++i) {
        const In& r = rows[i];
        In& q = rounded[i];
        GpuRow& g = gpu[i];
        for (int k = 0; k < 3; ++k) {
            g.base[k] = float(r.base[k]);
            g.v[k] = float(r.pos[k] - r.base[k]);
            g.nrm[k] = float(r.nrm[k]);
            q.base[k] = g.base[k];
            q.pos[k] = double(g.base[k]) + double(g.v[k]);
            q.nrm[k] = g.nrm[k];
        }
        g.attr[0] = float(r.attr[0]), g.attr[1] = float(r.attr[1]);
        g.h = float(r.h), g.height = float(r.height), g.bend = float(r.bend), g.t = float(r.t);
        g.wind[0] = float(r.dir[0]), g.wind[1] = float(r.dir[2]), g.wind[2] = float(r.speed), g.wind[3] = float(r.gust);
        q.attr[0] = g.attr[0], q.attr[1] = g.attr[1], q.h = g.h, q.height = g.height, q.bend = g.bend, q.t = g.t;
        q.dir[0] = g.wind[0], q.dir[1] = 0.0, q.dir[2] = g.wind[1], q.speed = g.wind[2], q.gust = g.wind[3];
    }
    std::vector<float> out;
    std::string adapter;
    const std::wstring dir = std::filesystem::path(argv[2]).wstring();
    const std::wstring dxc = std::filesystem::path(argv[3]).wstring();
    if (std::string e = RunGpu(dir + L"\\wind_reference.hlsl", dxc, gpu, out, adapter); !e.empty()) return Fail(e);

    double worst = 0.0, worstStress = 0.0, worstGolden = 0.0, worstInputs = 0.0;
    size_t worstRow = 0, worstStressRow = 0, phaseMismatch = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        double ref[3], got[3];
        Displace(rounded[i], ref);
        for (int k = 0; k < 3; ++k) got[k] = rounded[i].base[k] + double(out[i * 4 + k]);
        const double e = Dist(got, ref);
        if (double(out[i * 4 + 3]) != PlacementPhase(rounded[i].base)) ++phaseMismatch;
        if (i >= goldenRows) {
            if (e > worstStress) worstStress = e, worstStressRow = i - goldenRows;
            continue;
        }
        if (e > worst) worst = e, worstRow = i;
        worstGolden = (std::max)(worstGolden, Dist(got, rows[i].out));
        worstInputs = (std::max)(worstInputs, Dist(ref, rows[i].out));
    }
    std::printf("the shader (%s) on the golden rows: at most %.3f mm from the reference on the same float32 inputs (row %zu); "
                "%.3f mm from the golden outputs (the inputs' rounding alone: %.3f mm)\n",
                adapter.c_str(), worst * 1000.0, worstRow, worstGolden * 1000.0, worstInputs * 1000.0);
    std::printf("the shader on %zu worst-case rows (the hour's end, 20 m/s, gustiness 1, full weights): at most %.3f mm (row %zu)\n",
                rows.size() - goldenRows, worstStress * 1000.0, worstStressRow);
    std::printf("placement phases: %zu of %zu differ from the PCG hash in C++\n", phaseMismatch, rows.size());
    if (phaseMismatch) return Fail("WindPhase (the PCG hash) does not agree with Python's bit for bit");
    if (worst > 1e-3 || worstStress > 1e-3) return Fail("the shader is more than 1 mm from the reference");
    std::printf("wind-reference: all good\n");
    return 0;
}
