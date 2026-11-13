// ---------------------------------------------------------------------------
// ngg-core device: D3D12 device, queue and a single direct command list.
//
// Used by every client of ngg-core - the game and the editor (design §8)
// through WRenderer. Nothing in ngg-core knows about the game.
//
// Deliberately one allocator and one list: the renderer is synchronous (it
// waits for the GPU at the end of every frame), which is a real limitation but
// an honest one. Frame overlapping needs per-frame allocators and a ring
// allocator for constants, and that change belongs with the change that makes
// it necessary - not scattered here in anticipation.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <stdint.h>

namespace ngg {
namespace core {

struct Device
{
    Microsoft::WRL::ComPtr<ID3D12Device>              device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Fence>               fence;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue>           infoQueue;

    // Capabilities, filled by Create and logged there. rtTier is
    // D3D12_RAYTRACING_TIER as an integer (10 = 1.0, 11 = 1.1); shaderModel is
    // D3D_SHADER_MODEL (0x65 = 6.5). Inline RayQuery needs tier 1.1 AND SM 6.5.
    int rtTier = 0;
    int shaderModel = 0;
    bool SupportsInlineRaytracing() const { return rtTier >= 11 && shaderModel >= 0x65; }
    HANDLE                                            fenceEvent = nullptr;
    UINT64                                            fenceValue = 0;

    // luid == nullptr selects the highest-performance adapter (viewer, editor).
    //
    // Pass a LUID whenever a resource will be SHARED with another process:
    // shared resources do not cross adapters, and "the default adapter" is not
    // the same GPU in both processes on a switchable-graphics laptop. Getting
    // this wrong fails at OpenSharedHandle, far from the cause.
    bool Create(const LUID* luid, bool enableDebugLayer = false);

    // A SECOND command context on another Device's device and queue: its own
    // allocator, list and fence. For work that must happen while the primary
    // list is open - a BVH rebuild after an edit, recorded mid-frame by the
    // Renderer. It executes and completes on the shared queue before the
    // primary list is even submitted, so ordering on the GPU is free.
    bool CreateSecondary(const Device& primary);

    // Begin/End wrap the one command list. End submits and blocks until the GPU
    // is done, because the caller's target may be handed straight to another
    // process the moment we return.
    bool Begin();
    void EndAndWait();

    // Drain the debug layer's message queue into our log. The layer was being
    // enabled and its output discarded, which is worse than not enabling it -
    // it looks like validation is running when nothing is listening.
    void DrainMessages();

    void WaitForGpu();
    void Destroy();

    // Device removal (a GPU hang reset by the driver - TDR - or a fault), made
    // VISIBLE. Before this a removed device showed up only as later calls
    // failing quietly. Logs the reason and DRED's breadcrumbs - the last
    // command each list completed - once per device. True if removed.
    bool CheckRemoved(const char* where);
    bool removedReported = false;

    ~Device() { Destroy(); }
};

// NGG_PIX=<frame> (device.cpp): once per frame, before it is recorded - the PIX capture of that frame.
void PixFrame();

} // namespace core
} // namespace ngg
