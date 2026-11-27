#pragma once
// What a frame hands the GPU that must live until the GPU has run it: its constants and other small per-frame data
// (FrameUpload), and the resources it lets go of (DeferRelease) - one set per FRAME SLOT (device.h, NGG_FRAMES).
//
// Constants: upload buffers made ONCE per device and kept mapped, in a ring per slot. Before this, each constant
// buffer was its own committed resource, created and released every frame - a driver call apiece, 0.8 ms each on the
// dev laptop: the environment map's eight alone took 6.7 ms of CPU a frame (2026-11-23).
//
// Device::BeginFrame waits for the frame that last used its slot and then calls BeginFrameSlot: that slot's ring
// starts over and what it deferred is released. With one frame in flight that is last frame; with two, the one before.

#include <d3d12.h>
#include <cstddef>
#include <cstdint>

namespace ngg {
namespace core {

// Copies `bytes` of `data` into the current slot's ring and returns its GPU address (aligned to `align`, 256 for a
// constant buffer view); 0 if no buffer could be made. Valid until that slot's frame comes round again.
D3D12_GPU_VIRTUAL_ADDRESS FrameUpload(ID3D12Device* device, const void* data, size_t bytes, size_t align = 256);

// `object` (a resource, a heap, ...) is held until the GPU has run the frame being recorded: for what a frame
// replaces or lets go of while the GPU may still read it. Null is ignored.
void DeferRelease(IUnknown* object);

// The slot being recorded, of `slots` (Device::BeginFrame, after waiting for that slot's last frame): its ring starts
// over, what it deferred is released.
void BeginFrameSlot(uint32_t slot, uint32_t slots);
uint32_t FrameSlot();    // the slot being recorded
uint32_t FrameSlots();   // frames in flight

// The GPU is idle (Device::WaitForGpu): everything deferred, of every slot, released.
void ReleaseDeferred();

// Everything released (Device::Destroy, and before a device is made): nothing of an old device may outlive it - a
// removed device kept alive by one buffer makes D3D12CreateDevice fail on its adapter.
void ReleaseFrameUploads();

} // namespace core
} // namespace ngg
