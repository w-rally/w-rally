// ---------------------------------------------------------------------------
// ngg-core device: D3D12 device, queue and a single direct command list.
//
// Used by every client of ngg-core - the game and the editor (design §8)
// through WRenderer. Nothing in ngg-core knows about the game.
//
// FRAMES IN FLIGHT (NGG_FRAMES=1|2, default 1): the frame's list is recorded
// on its slot's allocator (BeginFrame/EndFrame). With one, EndFrame waits for
// the GPU as the renderer always did; with two, the CPU records frame N+1
// while the GPU runs frame N, and BeginFrame waits only for the frame that
// last used its slot (N-1). What a frame hands the GPU must then live per
// slot: constants (frame_upload.h FrameUpload), buffers it frees
// (DeferRelease), readbacks, descriptors it rewrites - NGG design §1.4.
// Begin/EndAndWait are the IMMEDIATE path (a texture upload, a capture): their
// own allocator, submitted and waited for at once.
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
    static constexpr uint32_t kMaxFrames = 2;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    alloc;                    // the immediate path's
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    frameAlloc[kMaxFrames];   // a frame slot's each
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    uint32_t frames = 1;                // frames in flight (NGG_FRAMES), read at Create
    uint32_t slot = 0;                  // the frame being recorded's slot
    uint64_t frameNumber = 0;           // frames begun
    UINT64   slotFence[kMaxFrames] = {};   // the fence value each slot's last frame signals
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

    // A FRAME: BeginFrame waits for the frame that last used the next slot,
    // starts that slot's constants and releases what it deferred
    // (frame_upload.h), and opens the list on its allocator. EndFrame submits;
    // with one frame in flight it also waits for the GPU.
    bool BeginFrame();
    void EndFrame();

    // IMMEDIATE work outside a frame (a texture's upload, a capture): the list
    // on its own allocator; EndAndWait submits and blocks until the GPU is done
    // - with everything before it - because the caller may hand the target
    // straight to another process the moment we return.
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
