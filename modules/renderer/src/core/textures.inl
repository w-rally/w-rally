// ---------------------------------------------------------------------------
// DDS loading for the NGGT chunk. Included by scene.cpp.
//
// Scope is deliberately exactly what RBR ships and no more: BC1 (DXT1) and BC2
// (DXT3), which is all 82 textures of Joux Verte use - and, behind a DX10
// header, the block formats PBR maps come in (BC4, BC5 for normals, BC7: a
// car's maps, assets/car_model.h). Anything else is reported by name and
// skipped rather than guessed at - a texture that renders as garbage is much
// harder to diagnose than one that is visibly missing.
// ---------------------------------------------------------------------------

namespace {

#pragma pack(push, 1)
struct DdsPixelFormat
{
    uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
};
struct DdsHeader
{
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormat pf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

static_assert(sizeof(DdsHeader) == 124, "DDS_HEADER must be 124 bytes");

const uint32_t kDdpfFourCC = 0x4;

uint32_t MakeFourCC(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

// Returns DXGI_FORMAT_UNKNOWN for anything we do not handle.
// `linear` says this texture is DATA, not artwork: a normal map or an
// occlusion/roughness/metallic pack. Those encode numbers, not colours, and
// pushing them through the sRGB decode bends every value - a 0.5 roughness
// would arrive as 0.21. The distinction cannot be recovered from the DDS, which
// is why the MATERIAL has to say which slot a texture is bound to.
DXGI_FORMAT DdsFormat(const DdsPixelFormat& pf, UINT* blockBytes, bool linear, uint32_t dx10Format = 0)
{
    // The DX10 header's own format (DirectXTex's texconv writes one for BC4/5/7): a colour map as the file says
    // (sRGB or not), data always UNORM.
    if (dx10Format)
    {
        // (TYPELESS - Pillow writes it - read as UNORM, or as sRGB for colour.)
        switch ((DXGI_FORMAT)dx10Format)
        {
        case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
            *blockBytes = 8;
            return linear ? DXGI_FORMAT_BC1_UNORM : dx10Format == DXGI_FORMAT_BC1_UNORM ? DXGI_FORMAT_BC1_UNORM : DXGI_FORMAT_BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
            *blockBytes = 16;
            return linear ? DXGI_FORMAT_BC2_UNORM : dx10Format == DXGI_FORMAT_BC2_UNORM ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
            *blockBytes = 16;
            return linear ? DXGI_FORMAT_BC3_UNORM : dx10Format == DXGI_FORMAT_BC3_UNORM ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            *blockBytes = 16;
            return linear ? DXGI_FORMAT_BC7_UNORM : dx10Format == DXGI_FORMAT_BC7_UNORM ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_BC7_UNORM_SRGB;
        case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: *blockBytes = 8;  return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: *blockBytes = 16; return DXGI_FORMAT_BC5_UNORM;
        default: break;
        }
        *blockBytes = 0;
        return DXGI_FORMAT_UNKNOWN;
    }
    if (pf.flags & kDdpfFourCC)
    {
        if (linear)
        {
            if (pf.fourCC == MakeFourCC('D','X','T','1')) { *blockBytes = 8;  return DXGI_FORMAT_BC1_UNORM; }
            if (pf.fourCC == MakeFourCC('D','X','T','3')) { *blockBytes = 16; return DXGI_FORMAT_BC2_UNORM; }
            if (pf.fourCC == MakeFourCC('D','X','T','5')) { *blockBytes = 16; return DXGI_FORMAT_BC3_UNORM; }
        }
        // _SRGB, not _UNORM: this is ARTWORK, authored and stored in sRGB, and
        // lighting has to happen in linear space. Reading it as UNORM treats an
        // sRGB value as if it were linear, which makes albedo too bright - an
        // error that partly cancelled the missing output encode and so hid
        // both. The hardware does the decode for free.
        if (pf.fourCC == MakeFourCC('D','X','T','1')) { *blockBytes = 8;  return DXGI_FORMAT_BC1_UNORM_SRGB; }
        if (pf.fourCC == MakeFourCC('D','X','T','3')) { *blockBytes = 16; return DXGI_FORMAT_BC2_UNORM_SRGB; }
        if (pf.fourCC == MakeFourCC('D','X','T','5')) { *blockBytes = 16; return DXGI_FORMAT_BC3_UNORM_SRGB; }
    }
    *blockBytes = 0;
    return DXGI_FORMAT_UNKNOWN;
}

// Create a GPU texture from a DDS blob and schedule its upload on `list`.
// The returned upload buffer must stay alive until the list has executed -
// the caller keeps it in a vector and releases the lot after waiting.
ComPtr<ID3D12Resource> CreateTextureFromDDS(ID3D12Device* dev,
                                            ID3D12GraphicsCommandList* list,
                                            const uint8_t* data, size_t bytes,
                                            ComPtr<ID3D12Resource>& outUpload,
                                            const char* nameForLog,
                                            bool linear)
{
    if (bytes < 4 + sizeof(DdsHeader) || memcmp(data, "DDS ", 4) != 0)
    {
        Log("texture '%s': not a DDS file", nameForLog);
        return nullptr;
    }

    auto* h = reinterpret_cast<const DdsHeader*>(data + 4);
    // A DX10 header follows the header when its fourCC says so: the format first, 20 bytes in all.
    const bool dx10 = (h->pf.flags & kDdpfFourCC) && h->pf.fourCC == MakeFourCC('D','X','1','0');
    if (dx10 && bytes < 4 + sizeof(DdsHeader) + 20)
    {
        Log("texture '%s': DX10 header cut short", nameForLog);
        return nullptr;
    }
    const uint32_t dx10Format = dx10 ? *reinterpret_cast<const uint32_t*>(data + 4 + sizeof(DdsHeader)) : 0;
    UINT blockBytes = 0;
    DXGI_FORMAT fmt = DdsFormat(h->pf, &blockBytes, linear, dx10 ? (dx10Format ? dx10Format : 0xFFFFFFFFu) : 0);
    if (fmt == DXGI_FORMAT_UNKNOWN)
    {
        Log("texture '%s': unsupported DDS pixel format (fourCC 0x%08X) - skipped",
               nameForLog, h->pf.fourCC);
        return nullptr;
    }

    const UINT mips = h->mipMapCount ? h->mipMapCount : 1;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = h->width;
    desc.Height = h->height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = (UINT16)mips;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> tex;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&tex))))
    {
        Log("texture '%s': CreateCommittedResource failed", nameForLog);
        return nullptr;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(mips);
    std::vector<UINT> rows(mips);
    std::vector<UINT64> rowBytes(mips);
    UINT64 uploadBytes = 0;
    dev->GetCopyableFootprints(&desc, 0, mips, 0, fp.data(), rows.data(),
                               rowBytes.data(), &uploadBytes);

    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub = {};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = uploadBytes;
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.Format = DXGI_FORMAT_UNKNOWN;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&outUpload))))
        return nullptr;

    uint8_t* dst = nullptr;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(outUpload->Map(0, &none, reinterpret_cast<void**>(&dst))))
        return nullptr;

    // Walk the DDS mip chain. Block-compressed rows are rows OF BLOCKS, which
    // is why the source pitch uses (dim+3)/4 rather than the pixel dimension.
    const uint8_t* src = data + 4 + sizeof(DdsHeader) + (dx10 ? 20 : 0);
    const uint8_t* srcEnd = data + bytes;

    for (UINT m = 0; m < mips; ++m)
    {
        const UINT w = (h->width >> m) ? (h->width >> m) : 1;
        const UINT hgt = (h->height >> m) ? (h->height >> m) : 1;
        const UINT blocksW = (w + 3) / 4;
        const UINT blocksH = (hgt + 3) / 4;
        const UINT srcPitch = blocksW * blockBytes;

        if (src + (size_t)srcPitch * blocksH > srcEnd)
        {
            Log("texture '%s': DDS data ends early at mip %u - truncated file",
                   nameForLog, m);
            break;
        }

        for (UINT r = 0; r < blocksH; ++r)
            memcpy(dst + fp[m].Offset + (size_t)r * fp[m].Footprint.RowPitch,
                   src + (size_t)r * srcPitch, srcPitch);

        src += (size_t)srcPitch * blocksH;
    }
    outUpload->Unmap(0, nullptr);

    for (UINT m = 0; m < mips; ++m)
    {
        D3D12_TEXTURE_COPY_LOCATION d = {};
        d.pResource = tex.Get();
        d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.SubresourceIndex = m;

        D3D12_TEXTURE_COPY_LOCATION s = {};
        s.pResource = outUpload.Get();
        s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        s.PlacedFootprint = fp[m];

        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    }

    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = tex.Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &toSrv);

    return tex;
}

// 1x1 white, so an object whose texture is missing draws with its vertex
// colour instead of vanishing or sampling garbage.
ComPtr<ID3D12Resource> CreateWhiteTexture(ID3D12Device* dev,
                                          ID3D12GraphicsCommandList* list,
                                          ComPtr<ID3D12Resource>& outUpload)
{
    const uint32_t white = 0xFFFFFFFFu;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12Resource> tex;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&tex))))
        return nullptr;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub = {};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = total; ub.Height = 1;
    ub.DepthOrArraySize = 1; ub.MipLevels = 1;
    ub.Format = DXGI_FORMAT_UNKNOWN;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&outUpload))))
        return nullptr;

    uint8_t* dst = nullptr;
    D3D12_RANGE none = { 0, 0 };
    outUpload->Map(0, &none, reinterpret_cast<void**>(&dst));
    memcpy(dst + fp.Offset, &white, sizeof(white));
    outUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION d = {};
    d.pResource = tex.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION s = {};
    s.pResource = outUpload.Get();
    s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint = fp;
    list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);

    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = tex.Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &toSrv);

    return tex;
}

} // namespace
