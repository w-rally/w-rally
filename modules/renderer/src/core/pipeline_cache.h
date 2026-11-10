// ---------------------------------------------------------------------------
// The pipelines: every pipeline state the renderer makes goes through CreatePipeline - counted and timed, made
// several at once by PipelineBatch - and, with NGG_PIPELINE_CACHE=library, kept in an ID3D12PipelineLibrary in the
// game's cache folder (UserDir()/cache/wrenderer/pipelines.bin) (docs/ngg-design.md 5.0x).
//
// WHY THE LIBRARY IS OFF. The shaders are precompiled (shaders.h), but DXIL / DXBC is still an intermediate language:
// each pipeline is compiled to the GPU's own code by the driver when it is created, and the driver keeps its own disk
// cache of that. MEASURED on the dev iGPU (Radeon 780M, 2026-11) the library saves nothing: warm, the 72 pipelines take
// ~15 ms with or without it; with the driver's cache cleared a library "hit" compiles again (AMD's library holds
// 181 KB, no GPU code) - and its loads, made under this cache's lock, undid the parallel making (the scene's batch
// 7.4 s instead of 2.9-4.3). Kept as an opt-in to measure on NVIDIA, whose driver is said to keep the code in it.
//
// KEYED by the adapter (vendor, device, subsystem, revision - not the LUID, which is new every boot), the driver's
// version and the shader set's hash (shaders_embedded.h); any mismatch, a short or damaged file, or a library the
// runtime refuses (another driver) and the file is thrown away and built again. A pipeline is named by a hash of its
// whole description, root signature included (CreateRootSignature records each one's), so a changed state is a
// different name. Not used while NGG_SHADER_DIR compiles the shaders (they change), nor under PIX.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace ngg {
namespace core {

// The cache for this device (the renderer's one), from and to `dir` (UTF-8; empty: no cache). Before any pipeline.
void OpenPipelineCache(ID3D12Device* dev, const std::string& dir);
// Writes the library when pipelines were added since the last write (after a scene's load, at the end).
void SavePipelineCache();
// Saves, then lets go of the library and the device.
void ClosePipelineCache();

// ID3D12Device::CreateGraphicsPipelineState / CreateComputePipelineState through the cache.
HRESULT CreatePipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out);
HRESULT CreatePipeline(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out);
inline HRESULT CreatePipeline(const Microsoft::WRL::ComPtr<ID3D12Device>& dev,
                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    return CreatePipeline(dev.Get(), desc, riid, out);
}
inline HRESULT CreatePipeline(const Microsoft::WRL::ComPtr<ID3D12Device>& dev,
                              const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    return CreatePipeline(dev.Get(), desc, riid, out);
}

// ID3D12Device::CreateRootSignature, remembering the signature's hash for the pipelines' names.
HRESULT CreateRootSignature(ID3D12Device* dev, UINT nodeMask, const void* blob, SIZE_T size, REFIID riid, void** out);
inline HRESULT CreateRootSignature(const Microsoft::WRL::ComPtr<ID3D12Device>& dev, UINT nodeMask, const void* blob,
                                   SIZE_T size, REFIID riid, void** out)
{
    return CreateRootSignature(dev.Get(), nodeMask, blob, size, riid, out);
}

// Pipelines made together, IN PARALLEL: the driver compiles each one to the GPU's code, seconds for the scene's
// uber shaders when its cache is cold, and one thread made them one after another (~6 s for the scene's 48 on the dev
// iGPU). Add copies the description (what it points to - shader bytes, the input layout, the root signature - must
// live until Run); Run makes them all through CreatePipeline on up to NGG_PIPELINE_THREADS threads (default: the
// CPU's cores, at most 8) and stores each into its target, or leaves the target empty and logs `failure` (when given).
// Whatever a caller decides from a pipeline's success, it decides after Run, from the targets.
class PipelineBatch
{
public:
    explicit PipelineBatch(ID3D12Device* dev) : m_dev(dev) {}
    void Add(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, Microsoft::WRL::ComPtr<ID3D12PipelineState>* out,
             const char* failure = nullptr);
    void Add(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, Microsoft::WRL::ComPtr<ID3D12PipelineState>* out,
             const char* failure = nullptr);
    // False if any failed.
    bool Run(const char* what);

private:
    struct Item
    {
        bool                                         compute = false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC           graphics = {};
        D3D12_COMPUTE_PIPELINE_STATE_DESC            computeDesc = {};
        Microsoft::WRL::ComPtr<ID3D12PipelineState>* out = nullptr;
        const char*                                  failure = nullptr;
        HRESULT                                      hr = E_FAIL;
    };
    ID3D12Device*     m_dev;
    std::vector<Item> m_items;
};

// One line for the log: pipelines made since the last call, how many came from the cache, and the time they took.
// NGG_PIPELINE_TIMES=1 logs every one.
void LogPipelineStats(const char* when);

} // namespace core
} // namespace ngg
