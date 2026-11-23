#include "frame_upload.h"

#include <wrl/client.h>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

struct Chunk
{
    ComPtr<ID3D12Resource> res;
    unsigned char*         mapped = nullptr;
    size_t                 size = 0;
};

constexpr size_t kChunk = 1 << 20;   // 1 MB: a frame's constants fit in one; more are added when not

struct Ring
{
    // The chunks' device, compared, NOT held: anything alive on a removed device keeps it, and D3D12 then makes no
    // new device on that adapter (the device-loss tests). ReleaseFrameUploads drops the chunks with the device.
    ID3D12Device*        device = nullptr;
    std::vector<Chunk>   chunks;
    size_t               at = 0;     // the chunk being filled
    size_t               used = 0;   // bytes of it used
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
    if (!device || !bytes) return 0;
    Ring& r = TheRing();
    if (r.device != device)
    {
        r.chunks.clear();
        r.at = r.used = 0;
        r.device = device;
    }
    if (align < 16) align = 16;
    for (;;)
    {
        if (r.at < r.chunks.size())
        {
            Chunk& c = r.chunks[r.at];
            const size_t start = (r.used + align - 1) & ~(align - 1);
            if (start + bytes <= c.size)
            {
                memcpy(c.mapped + start, data, bytes);
                r.used = start + bytes;
                return c.res->GetGPUVirtualAddress() + start;
            }
            ++r.at;   // full (or too small for this): the next chunk, or a new one below
            r.used = 0;
            continue;
        }
        Chunk c;
        if (!MakeChunk(device, bytes > kChunk ? ((bytes + 0xFFFF) & ~size_t(0xFFFF)) : kChunk, c)) return 0;
        r.chunks.push_back(c);
        r.at = r.chunks.size() - 1;
        r.used = 0;
    }
}

void ResetFrameUploads()
{
    Ring& r = TheRing();
    r.at = 0;
    r.used = 0;
}

void ReleaseFrameUploads()
{
    Ring& r = TheRing();
    r.chunks.clear();
    r.at = r.used = 0;
    r.device = nullptr;
}

} // namespace core
} // namespace ngg
