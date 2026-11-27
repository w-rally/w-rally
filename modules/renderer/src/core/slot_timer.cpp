#include "slot_timer.h"
#include "device.h"
#include "frame_upload.h"

#include <cstring>

namespace ngg {
namespace core {

static_assert(Device::kMaxFrames == 2, "SlotTimer::m_pending holds one flag per frame slot");

bool SlotTimer::Create(ID3D12Device* device, uint32_t count)
{
    Reset();
    if (!device || !count) return false;
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = count * Device::kMaxFrames;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (UINT64)count * Device::kMaxFrames * sizeof(UINT64);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_heap))) ||
        FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&m_readback))))
    {
        Reset();
        return false;
    }
    m_count = count;
    return true;
}

void SlotTimer::Reset()
{
    m_heap.Reset();
    m_readback.Reset();
    m_count = 0;
    m_pending[0] = m_pending[1] = false;
}

bool SlotTimer::Take(UINT64* out)
{
    const uint32_t s = FrameSlot();
    if (!m_heap || !m_pending[s]) return false;
    m_pending[s] = false;
    UINT64* t = nullptr;
    const D3D12_RANGE r = { (SIZE_T)s * m_count * sizeof(UINT64), (SIZE_T)(s + 1) * m_count * sizeof(UINT64) };
    if (FAILED(m_readback->Map(0, &r, reinterpret_cast<void**>(&t)))) return false;
    memcpy(out, t + (size_t)s * m_count, m_count * sizeof(UINT64));
    const D3D12_RANGE none = { 0, 0 };
    m_readback->Unmap(0, &none);
    return true;
}

void SlotTimer::Stamp(ID3D12GraphicsCommandList* list, uint32_t i)
{
    if (m_heap && i < m_count) list->EndQuery(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, FrameSlot() * m_count + i);
}

void SlotTimer::Resolve(ID3D12GraphicsCommandList* list, uint32_t first, uint32_t n)
{
    if (!m_heap || first + n > m_count) return;
    const uint32_t s = FrameSlot();
    list->ResolveQueryData(m_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, s * m_count + first, n, m_readback.Get(),
                           ((UINT64)s * m_count + first) * sizeof(UINT64));
    m_pending[s] = true;
}

} // namespace core
} // namespace ngg
