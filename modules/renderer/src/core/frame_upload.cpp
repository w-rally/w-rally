#include "frame_upload.h"

#include <wrl/client.h>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

constexpr uint32_t kSlots = 2;       // Device::kMaxFrames
constexpr size_t   kChunk = 1 << 20; // 1 MB: a frame's constants fit in one; more are added when not

struct Chunk
{
    ComPtr<ID3D12Resource> res;
    unsigned char*         mapped = nullptr;
    size_t                 size = 0;
};

struct Slot
{
    std::vector<Chunk>            chunks;
    size_t                        at = 0;     // the chunk being filled
    size_t                        used = 0;   // bytes of it used
    std::vector<ComPtr<IUnknown>> deferred;   // held until this slot's frame has run
};

struct Ring
{
    // The chunks' device, compared, NOT held: anything alive on a removed device keeps it, and D3D12 then makes no
    // new device on that adapter (the device-loss tests). ReleaseFrameUploads drops the chunks with the device.
    ID3D12Device* device = nullptr;
    Slot          slots[kSlots];
    uint32_t      current = 0;
    uint32_t      count = 1;
};

Ring& TheRing()
{
    static Ring r;
    return r;
}

bool MakeChunk(ID3D12Device* dev, size_t size, Chunk& out)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&out.res))))
        return false;
    void* p = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(out.res->Map(0, &none, &p))) { out.res.Reset(); return false; }
    out.mapped = static_cast<unsigned char*>(p);
    out.size = size;
    return true;
}

} // namespace

D3D12_GPU_VIRTUAL_ADDRESS FrameUpload(ID3D12Device* device, const void* data, size_t bytes, size_t align)
{
    void* cpu = nullptr;
    const D3D12_GPU_VIRTUAL_ADDRESS va = FrameAlloc(device, bytes, align, &cpu);
    if (va) memcpy(cpu, data, bytes);
    return va;
}

D3D12_GPU_VIRTUAL_ADDRESS FrameAlloc(ID3D12Device* device, size_t bytes, size_t align, void** cpu)
{
    if (!device || !bytes || !cpu) return 0;
    Ring& r = TheRing();
    if (r.device != device)
    {
        for (Slot& s : r.slots) s.chunks.clear(), s.at = s.used = 0;
        r.device = device;
    }
    Slot& s = r.slots[r.current];
    if (align < 16) align = 16;
    for (;;)
    {
        if (s.at < s.chunks.size())
        {
            Chunk& c = s.chunks[s.at];
            const size_t start = (s.used + align - 1) & ~(align - 1);
            if (start + bytes <= c.size)
            {
                *cpu = c.mapped + start;
                s.used = start + bytes;
                return c.res->GetGPUVirtualAddress() + start;
            }
            ++s.at;   // full (or too small for this): the next chunk, or a new one below
            s.used = 0;
            continue;
        }
        Chunk c;
        if (!MakeChunk(device, bytes > kChunk ? ((bytes + 0xFFFF) & ~size_t(0xFFFF)) : kChunk, c)) return 0;
        s.chunks.push_back(c);
        s.at = s.chunks.size() - 1;
        s.used = 0;
    }
}

void DeferRelease(IUnknown* object)
{
    if (!object) return;
    Ring& r = TheRing();
    r.slots[r.current].deferred.emplace_back(object);
}

void BeginFrameSlot(uint32_t slot, uint32_t slots)
{
    Ring& r = TheRing();
    r.count = slots < 1 ? 1 : slots > kSlots ? kSlots : slots;
    r.current = slot % r.count;
    Slot& s = r.slots[r.current];
    s.at = s.used = 0;
    s.deferred.clear();
}

uint32_t FrameSlot() { return TheRing().current; }
uint32_t FrameSlots() { return TheRing().count; }

void ReleaseDeferred()
{
    for (Slot& s : TheRing().slots) s.deferred.clear();
}

void ReleaseFrameUploads()
{
    Ring& r = TheRing();
    for (Slot& s : r.slots)
    {
        s.chunks.clear();
        s.at = s.used = 0;
        s.deferred.clear();
    }
    r.device = nullptr;
}

} // namespace core
} // namespace ngg
