#pragma once
// The frame's constants and other small per-frame data, in upload buffers made ONCE per device and kept mapped.
//
// Before this, each constant buffer was its own committed resource, created and released every frame - a driver call
// apiece (CreateCommittedResource maps, zeroes and tracks a resource), 0.8 ms each on the dev laptop: the environment
// map's eight alone took 6.7 ms of CPU a frame (2026-11-23).
//
// The frame is synchronous (Device::EndAndWait waits for the GPU), so the ring starts over after that wait
// (ResetFrameUploads): everything a frame placed here is read by then. Frames that overlap would need one ring per
// frame in flight.

#include <d3d12.h>
#include <cstddef>

namespace ngg {
namespace core {

// Copies `bytes` of `data` into this frame's ring and returns its GPU address (aligned to `align`, 256 for a constant
// buffer view); 0 if no buffer could be made. Valid until the next ResetFrameUploads.
D3D12_GPU_VIRTUAL_ADDRESS FrameUpload(ID3D12Device* device, const void* data, size_t bytes, size_t align = 256);

// The GPU has finished every frame recorded so far: the ring starts over (Device::EndAndWait).
void ResetFrameUploads();

// The ring's buffers released (Device::Destroy, and before a device is made): nothing of an old device may outlive
// it - a removed device kept alive by one buffer makes D3D12CreateDevice fail on its adapter.
void ReleaseFrameUploads();

} // namespace core
} // namespace ngg
