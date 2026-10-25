#include "device.h"
#include "log.h"

#include <dxgi1_6.h>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace ngg {
namespace core {

namespace {

// Highest-performance adapter, skipping WARP. Only used when no LUID was given
// - i.e. when nothing is being shared and we are free to choose.
ComPtr<IDXGIAdapter1> PickAdapter(IDXGIFactory4* factory)
{
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f6))))
    {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a))); ++i)
        {
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return a;
            a.Reset();
        }
    }

    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return a;
        a.Reset();
    }
    return nullptr;
}

} // namespace

bool Device::Create(const LUID* luid, bool enableDebugLayer)
{
    // DRED - Device Removed Extended Data - before the device exists: it is a
    // debug-interface setting that only takes effect at creation. Breadcrumbs
    // record each command as the GPU completes it, so a hang names the command
    // it hung in; page-fault data names the freed or missing allocation. The
    // cost is small, and a GPU reset with no record of where is a dead end -
    // which is what the first ones on this machine were.
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }

    if (enableDebugLayer)
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            Log("core: D3D12 debug layer enabled");
        }
        else
        {
            Log("core: debug layer requested but unavailable "
                "(install the Graphics Tools optional feature)");
        }
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        Log("core: CreateDXGIFactory1 failed");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (luid)
    {
        if (FAILED(factory->EnumAdapterByLuid(*luid, IID_PPV_ARGS(&adapter))))
        {
            Log("core: no adapter with LUID %ld:%lu - the two processes are on "
                "different GPUs", (long)luid->HighPart, (unsigned long)luid->LowPart);
            return false;
        }
    }
    else
    {
        adapter = PickAdapter(factory.Get());
        if (!adapter) { Log("core: no hardware adapter found"); return false; }
    }

    DXGI_ADAPTER_DESC1 ad = {};
    adapter->GetDesc1(&ad);
    Log("core: adapter %ls (%llu MB)", ad.Description,
        (unsigned long long)(ad.DedicatedVideoMemory / (1024 * 1024)));

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        Log("core: D3D12CreateDevice failed");
        return false;
    }

    // What this adapter can actually do, logged at startup rather than
    // discovered when a feature silently fails to create. Ray tracing is the
    // one that decides a whole branch of the plan (design 1.8.3), and shader
    // model matters as much as the tier: inline RayQuery needs SM 6.5, which
    // FXC cannot emit at all.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
        rtTier = 0;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
            rtTier = (int)o5.RaytracingTier;

        D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_7 };
        shaderModel = 0;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
            shaderModel = (int)sm.HighestShaderModel;

        Log("core: raytracing tier %d.%d, highest shader model %d.%d",
            rtTier / 10, rtTier % 10, shaderModel >> 4, shaderModel & 0xF);
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&alloc)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                         nullptr, IID_PPV_ARGS(&list)))) return false;
    list->Close();
    list->SetName(L"ngg frame");

    if (SUCCEEDED(device.As(&infoQueue)))
        Log("core: D3D12 validation messages will be logged");

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return fenceEvent != nullptr;
}

bool Device::CreateSecondary(const Device& primary)
{
    if (!primary.device || !primary.queue) return false;
    device = primary.device;
    queue = primary.queue;
    rtTier = primary.rtTier;
    shaderModel = primary.shaderModel;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&alloc)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                         nullptr, IID_PPV_ARGS(&list)))) return false;
    list->Close();
    list->SetName(L"ngg secondary (BVH builds)");
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return fenceEvent != nullptr;
}

bool Device::Begin()
{
    if (FAILED(alloc->Reset())) { CheckRemoved("at Begin"); return false; }
    return SUCCEEDED(list->Reset(alloc.Get(), nullptr));
}

void Device::EndAndWait()
{
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    CheckRemoved("after a submit");
}

namespace {
const char* OpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:        return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:             return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:     return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:    return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:         return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:   return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:      return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:     return "ResolveQueryData";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:              return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
    default:                                            return "?";
    }
}
} // namespace

bool Device::CheckRemoved(const char* where)
{
    if (!device) return false;
    const HRESULT reason = device->GetDeviceRemovedReason();
    if (reason == S_OK) return false;
    if (removedReported) return true;
    removedReported = true;

    Log("core: DEVICE REMOVED %s - reason 0x%08lX (%s)", where, (unsigned long)reason,
        reason == DXGI_ERROR_DEVICE_HUNG ? "DEVICE_HUNG: a command took too long - GPU timeout/TDR" :
        reason == DXGI_ERROR_DEVICE_RESET ? "DEVICE_RESET: the driver reset the GPU" :
        reason == DXGI_ERROR_DEVICE_REMOVED ? "DEVICE_REMOVED" :
        reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DRIVER_INTERNAL_ERROR" :
        reason == DXGI_ERROR_INVALID_CALL ? "INVALID_CALL: the application did something invalid" : "other");

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(device.As(&dred))) { Log("core: (no DRED data on this device)"); return true; }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
    {
        int lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = crumbs.pHeadAutoBreadcrumbNode; n && lists < 8; n = n->pNext, ++lists)
        {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount) continue;           // this list finished: not the culprit
            Log("core:   list %ls: completed %u of %u commands; next was %s",
                n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"(unnamed)",
                done, n->BreadcrumbCount,
                done < n->BreadcrumbCount ? OpName(n->pCommandHistory[done]) : "-");
            const UINT from = done >= 4 ? done - 4 : 0;
            for (UINT i = from; i < n->BreadcrumbCount && i < done + 4; ++i)
                Log("core:     [%u]%s %s", i, i == done ? " >>" : "   ", OpName(n->pCommandHistory[i]));
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
    {
        Log("core:   page fault at GPU VA 0x%llx", (unsigned long long)pf.PageFaultVA);
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && k < 6; a = a->pNext, ++k)
            Log("core:     recently FREED nearby: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"(unnamed)", (int)a->AllocationType);
    }
    return true;
}

void Device::DrainMessages()
{
    if (!infoQueue) return;
    const UINT64 n = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i)
    {
        SIZE_T len = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(infoQueue->GetMessage(i, msg, &len))) continue;
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            Log("D3D12 [%s] %s",
                msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" :
                msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING",
                msg->pDescription);
    }
    infoQueue->ClearStoredMessages();
}

void Device::WaitForGpu()
{
    const UINT64 v = ++fenceValue;
    queue->Signal(fence.Get(), v);
    if (fence->GetCompletedValue() < v)
    {
        fence->SetEventOnCompletion(v, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void Device::Destroy()
{
    // Never tear down while the GPU still references our resources; without
    // this a clean exit becomes a device-removed report in the log.
    if (queue && fence) WaitForGpu();
    if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
    list.Reset(); alloc.Reset(); fence.Reset(); queue.Reset(); device.Reset();
}

} // namespace core
} // namespace ngg
