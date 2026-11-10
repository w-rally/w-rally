// ---------------------------------------------------------------------------
// The pipeline cache: every pipeline state the renderer makes goes through CreatePipeline, and is kept in an
// ID3D12PipelineLibrary in the game's cache folder (UserDir()/cache/wrenderer/pipelines.bin), so the next start takes
// the driver's compiled GPU code from there instead of compiling it again (docs/ngg-design.md, "Shaders and the
// pipeline cache").
//
// WHAT IT SAVES. The shaders are precompiled (shaders.h), but DXIL / DXBC is still an intermediate language: each
// pipeline is compiled to the GPU's own code by the driver when it is created. The driver keeps its own disk cache of
// that; the library is meant for when that cache is cold or evicted. MEASURED on the dev iGPU (Radeon 780M, 2026-11):
// it saves nothing there - warm, the 72 pipelines take ~15 ms with or without it; with the driver's cache cleared
// they take ~8 s with or without it (AMD's library holds 181 KB, no GPU code: the driver compiles again). Kept
// because it costs nothing and other drivers (NVIDIA's) are said to keep the compiled code in it - unmeasured.
//
// KEYED by the adapter (vendor, device, subsystem, revision - not the LUID, which is new every boot), the driver's
// version and the shader set's hash (shaders_embedded.h); any mismatch, a short or damaged file, or a library the
// runtime refuses (another driver) and the file is thrown away and built again. A pipeline is named by a hash of its
// whole description, root signature included (CreateRootSignature records each one's), so a changed state is a
// different name. Off with NGG_PIPELINE_CACHE=off, and while NGG_SHADER_DIR compiles the shaders (they change).
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

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

// One line for the log: pipelines made since the last call, how many came from the cache, and the time they took.
// NGG_PIPELINE_TIMES=1 logs every one.
void LogPipelineStats(const char* when);

} // namespace core
} // namespace ngg
