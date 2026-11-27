#pragma once
// GPU timestamps a pass takes every frame, one set per FRAME SLOT (device.h, NGG_FRAMES): the frame being recorded
// writes its slot's set; its values are read when that slot comes round again - Device::BeginFrame waited for the
// frame that wrote them (last frame with one frame in flight, the one before with two). One readback shared by the
// slots, each its own range, so no read meets a write.

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace ngg {
namespace core {

class SlotTimer
{
public:
    // `count` timestamps a frame. False (and no timer) if the heap or the readback could not be made.
    bool Create(ID3D12Device* device, uint32_t count);
    void Reset();
    bool Ready() const { return m_heap != nullptr; }

    // The current slot's last resolved timestamps into out[0..count), if it resolved some since they were last taken.
    bool Take(UINT64* out);
    // Timestamp i of the frame being recorded.
    void Stamp(ID3D12GraphicsCommandList* list, uint32_t i);
    // Timestamps [first, first + n) of the frame being recorded resolved into its slot's range (Take reads them).
    void Resolve(ID3D12GraphicsCommandList* list, uint32_t first, uint32_t n);

private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_readback;
    uint32_t m_count = 0;
    bool     m_pending[2] = {};   // Device::kMaxFrames
};

} // namespace core
} // namespace ngg
