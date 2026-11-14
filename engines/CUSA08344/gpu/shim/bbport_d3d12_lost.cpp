// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: why D3D12 removed the device under Dozen. Dozen has no VK_EXT_device_fault, so a device
// loss said nothing (TLOU 2026-11-02: "Swapchain acquire returned ErrorDeviceLost" after 20 min of
// play at 30 FPS, no reason). The removal reason comes from Dozen's own device (kept from the
// swapchain the Xbox host attaches). DRED (BB_DRED=1, or pf without breadcrumbs) gives no data on
// the Xbox: DXGI_ERROR_UNSUPPORTED even when enabled.
#include "bbport_d3d12_lost.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <d3d12sdklayers.h>

namespace BbD3d12Lost {
namespace {

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* names[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
        "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource",
        "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
        "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present",
        "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
        "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
        "ProcessFrames1", "BuildRaytracingAccelerationStructure",
        "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand",
        "ExecuteMetaCommand", "EstimateMotion", "ResolveMotionVectorHeap",
        "SetPipelineState1", "InitializeExtensionCommand", "ExecuteExtensionCommand",
        "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata",
    };
    return unsigned(op) < sizeof(names) / sizeof(names[0]) ? names[op] : "?";
}

const char* AllocationName(D3D12_DRED_ALLOCATION_TYPE type) {
    switch (type) {
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "queue";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
    case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "pipeline";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "command list";
    case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "fence";
    case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "descriptor heap";
    case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "heap";
    case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "query heap";
    case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "command signature";
    case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "resource";
    default: return "other";
    }
}

void PrintAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE1* node) {
    int shown = 0;
    for (; node && shown < 24; node = node->pNext, ++shown) {
        std::printf("GPU: DRED %s allocation: %s %s (object %p)\n", what,
                    AllocationName(node->AllocationType), node->ObjectNameA ? node->ObjectNameA : "",
                    static_cast<const void*>(node->pObject));
    }
    if (!shown) {
        std::printf("GPU: DRED %s allocations: none\n", what);
    }
}

void PrintBreadcrumbs(const D3D12_AUTO_BREADCRUMB_NODE1* node) {
    int lists = 0, unfinished = 0;
    for (; node; node = node->pNext, ++lists) {
        const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        if (done >= node->BreadcrumbCount || unfinished >= 6) {
            continue; // finished on the GPU, or enough shown
        }
        ++unfinished;
        std::printf("GPU: DRED unfinished command list %p (%s) on queue %p (%s): %u of %u ops done\n",
                    static_cast<void*>(node->pCommandList),
                    node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "",
                    static_cast<void*>(node->pCommandQueue),
                    node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "", done,
                    node->BreadcrumbCount);
        const UINT first = done > 8 ? done - 8 : 0;
        const UINT last = done + 4 < node->BreadcrumbCount ? done + 4 : node->BreadcrumbCount;
        for (UINT i = first; i < last; ++i) {
            const char* context = "";
            for (UINT c = 0; c < node->BreadcrumbContextsCount; ++c) {
                if (node->pBreadcrumbContexts[c].BreadcrumbIndex == i) {
                    static char narrow[128];
                    std::snprintf(narrow, sizeof(narrow), " \"%ls\"", node->pBreadcrumbContexts[c].pContextString);
                    context = narrow;
                }
            }
            std::printf("GPU: DRED   %s op %u %s%s\n", i < done ? "done" : i == done ? "AT  " : "next", i,
                        OpName(node->pCommandHistory[i]), context);
        }
    }
    std::printf("GPU: DRED %d command lists recorded, %d unfinished shown\n", lists, unfinished);
}

// Dozen's device, kept from its swapchain (bbport_d3d12_hold_swapchain, Xbox).
ID3D12Device* held_device = nullptr;

/// The device with a reference for the caller, or null (hr says why); without a kept one,
/// the process's own D3D12 device (desktop builds without a device factory).
ID3D12Device* Device(HRESULT* hr) {
    *hr = S_OK;
    if (held_device) {
        held_device->AddRef();
        return held_device;
    }
    ID3D12Device* device = nullptr;
    *hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    return SUCCEEDED(*hr) ? device : nullptr;
}

} // namespace

} // namespace BbD3d12Lost

// Dozen makes its device through its own D3D12 device factory, so D3D12CreateDevice gives a
// different one (2026-11-02: removing that one left the game's untouched). The Xbox host is
// handed Dozen's swapchain to attach to the screen (tlou194_uwp_entry.c): its queue's device
// is the game's.
extern "C" void bbport_d3d12_hold_swapchain(void* unknown) {
    using BbD3d12Lost::held_device;
    IDXGISwapChain* swapchain = nullptr;
    ID3D12Resource* buffer = nullptr;
    if (held_device || !unknown ||
        FAILED(static_cast<IUnknown*>(unknown)->QueryInterface(IID_PPV_ARGS(&swapchain)))) {
        return;
    }
    // GetDevice on a D3D12 swapchain did not give its queue (2026-11-02); its buffers know.
    const HRESULT hr = swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer));
    if (SUCCEEDED(hr) && buffer) {
        buffer->GetDevice(IID_PPV_ARGS(&held_device));
        buffer->Release();
    }
    swapchain->Release();
    std::printf("GPU: Dozen's D3D12 device %s for hang reports (%#lx)\n",
                held_device ? "kept" : "not found", hr);
    // With the D3D12 debug layer (bb-env DZN_DEBUG=d3d12, PC runs): its errors into this log as
    // they happen. A UWP app's debug output does not reach a listener outside its container.
    ID3D12InfoQueue1* messages = nullptr;
    if (held_device && SUCCEEDED(held_device->QueryInterface(IID_PPV_ARGS(&messages))) && messages) {
        DWORD cookie = 0;
        const HRESULT registered = messages->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
               LPCSTR text, void*) {
                static std::atomic<unsigned> shown{0};
                if (severity > D3D12_MESSAGE_SEVERITY_WARNING || shown.fetch_add(1) >= 2000) {
                    return;
                }
                std::printf("D3D12 %s #%d: %s\n",
                            severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                            : severity == D3D12_MESSAGE_SEVERITY_ERROR    ? "ERROR"
                                                                          : "WARNING",
                            int(id), text);
                std::fflush(stdout);
            },
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        std::printf("GPU: D3D12 debug layer messages %s\n",
                    SUCCEEDED(registered) ? "go to this log" : "could not be hooked");
        messages->Release();
    }
    std::fflush(stdout);
}

namespace BbD3d12Lost {

void EnableDred() {
    // Off unless BB_DRED=1 (or BB_DRED=pf): the Xbox gives no DRED data (2026-11-02).
    const char* env = std::getenv("BB_DRED");
    if (!env || env[0] == '0' || !env[0]) {
        return;
    }
    const bool breadcrumbs = !(env && env[0] == 'p');
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if (FAILED(hr) || !settings) {
        std::printf("GPU: DRED unavailable (%#lx); a device loss will give only the reason\n", hr);
        return;
    }
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetAutoBreadcrumbsEnablement(breadcrumbs ? D3D12_DRED_ENABLEMENT_FORCED_ON
                                                       : D3D12_DRED_ENABLEMENT_FORCED_OFF);
    settings->Release();
    std::printf("GPU: DRED on (page faults%s)\n", breadcrumbs ? ", breadcrumbs" : "");
    std::fflush(stdout);
}

void Report() {
    // D3D12 keeps one device per adapter: this is Dozen's device (Xbox: one adapter).
    HRESULT hr;
    ID3D12Device* device = Device(&hr);
    if (!device) {
        std::printf("GPU: D3D12 device not reachable for the removal reason (%#lx)\n", hr);
        std::fflush(stdout);
        return;
    }
    const HRESULT reason = device->GetDeviceRemovedReason();
    const char* name = reason == DXGI_ERROR_DEVICE_HUNG ? "device hung (GPU timeout)"
                       : reason == DXGI_ERROR_DEVICE_REMOVED ? "device removed (page fault or bad command)"
                       : reason == DXGI_ERROR_DEVICE_RESET ? "device reset (bad command)"
                       : reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "driver internal error"
                       : reason == DXGI_ERROR_INVALID_CALL ? "invalid call"
                       : reason == E_OUTOFMEMORY ? "out of memory"
                       : reason == S_OK ? "not removed (a new device?)"
                                        : "other";
    std::printf("GPU: D3D12 removal reason %#lx: %s\n", reason, name);
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred))) && dred) {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
        hr = dred->GetPageFaultAllocationOutput1(&fault);
        if (SUCCEEDED(hr)) {
            std::printf("GPU: DRED page fault at GPU VA %#llx\n",
                        static_cast<unsigned long long>(fault.PageFaultVA));
            PrintAllocations("existing", fault.pHeadExistingAllocationNode);
            PrintAllocations("recently freed", fault.pHeadRecentFreedAllocationNode);
        } else {
            std::printf("GPU: DRED page fault output %#lx\n", hr);
        }
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
        hr = dred->GetAutoBreadcrumbsOutput1(&crumbs);
        if (SUCCEEDED(hr)) {
            PrintBreadcrumbs(crumbs.pHeadAutoBreadcrumbNode);
        } else {
            std::printf("GPU: DRED breadcrumbs output %#lx\n", hr);
        }
        dred->Release();
    } else {
        std::printf("GPU: DRED data not available on this device\n");
    }
    // Detroit (2026-11-08): the debug layer's stored messages (DZN_DEBUG=d3d12), the last 200,
    // for when the live callback could not be hooked (no ID3D12InfoQueue1).
    ID3D12InfoQueue* queue = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
        const UINT64 count = queue->GetNumStoredMessages();
        std::printf("GPU: D3D12 debug layer holds %llu messages\n", static_cast<unsigned long long>(count));
        for (UINT64 i = count > 200 ? count - 200 : 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(queue->GetMessage(i, nullptr, &size)) || !size) {
                continue;
            }
            auto* message = static_cast<D3D12_MESSAGE*>(std::malloc(size));
            if (message && SUCCEEDED(queue->GetMessage(i, message, &size))) {
                std::printf("D3D12 stored #%d sev %d: %.*s\n", int(message->ID), int(message->Severity),
                            int(message->DescriptionByteLength), message->pDescription);
            }
            std::free(message);
        }
        queue->Release();
    } else {
        std::printf("GPU: no D3D12 debug layer on this device (DZN_DEBUG=d3d12 turns it on)\n");
    }
    device->Release();
    std::fflush(stdout);
}

bool ForceRemoval() {
    HRESULT hr;
    ID3D12Device* device = Device(&hr);
    if (!device) {
        return false;
    }
    ID3D12Device5* device5 = nullptr;
    const bool removed = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) && device5;
    if (removed) {
        device5->RemoveDevice();
        device5->Release();
    }
    device->Release();
    return removed;
}

} // namespace BbD3d12Lost

#else
namespace BbD3d12Lost {
void EnableDred() {}
void Report() {}
bool ForceRemoval() {
    return false;
}
} // namespace BbD3d12Lost
#endif
