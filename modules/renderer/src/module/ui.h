#pragma once
// The UI pass (renderer.h's UiDrawList): the game's 2D - menus, HUD, debug UI, Dear ImGui's draw data as it is - drawn
// straight onto the back buffer after the frame, alpha-blended, scissored per command. Its textures are its own
// (createTextureRgba: font atlases, generated images), in a shader-visible heap of its own; slot 0 is white.
//
// The renderer is synchronous (Device::EndAndWait every frame), so one upload buffer for the vertices and indices is
// enough: the GPU is done with last frame's before this frame writes it.

#include <d3d12.h>
#include <stdint.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "device.h"
#include "wrally/iface/renderer.h"

namespace wrally::render {

class UiPass {
public:
    // What tells a UI texture's handle (createTextureRgba) from a scene texture's: this bit, over the UI pass's slot.
    // A command naming anything else draws white.
    static constexpr uint32_t kTextureBit = 0x80000000u;

    bool Create(ngg::core::Device& dev, DXGI_FORMAT target, std::string& error);

    // An RGBA8 texture, uploaded now (its own command list round trip: call outside a frame). Returns its slot (1..),
    // 0 on failure.
    uint32_t AddTexture(ngg::core::Device& dev, const void* rgba, uint32_t width, uint32_t height, std::string& error);
    void ReleaseTexture(uint32_t slot);

    // Records the draw list into dev.list (open, `rtv` in RENDER_TARGET).
    void Record(ngg::core::Device& dev, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height,
                const iface::UiDrawList& ui);

private:
    static constexpr uint32_t kSlots = 256;
    bool Upload(ngg::core::Device& dev, const iface::UiDrawList& ui);
    bool Make(ngg::core::Device& dev, const void* rgba, uint32_t width, uint32_t height, uint32_t slot,
              std::string& error);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_srvStride = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textures;   // by slot; [0] white
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;                  // upload heap: vertices, then indices
    uint64_t m_bufferBytes = 0;
};

} // namespace wrally::render
