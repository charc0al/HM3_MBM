#pragma once
#include <d3d12.h>
#include <cstdint>
#include <cstddef>

namespace NativeResources {
constexpr uint32_t OwnedPage=0x4d424447; // "MBDG", outside native page indices 0..255.
// ABI mirror of the renderer's allocation descriptor, embedded at buffer+0x28.
// Native upload consumes this directly; it must not acquire C++ ownership fields.
// Native allocations suballocate pages; ours own one resource with offset zero.
// threadKind=0 makes the upload path retain it instead of migrating/reallocating.
struct Allocation {
    uint32_t bytes=0;
    uint32_t offset=0;
    uint32_t page=0;
    uint32_t threadKind=0;
    ID3D12Resource* resource=nullptr;
};
static_assert(sizeof(Allocation)==24);
static_assert(offsetof(Allocation,resource)==16);

// The generic renderer allocator has fixed 4 MiB pages and returns an empty
// descriptor for oversized requests; native upload subsequently dereferences it.
// A dedicated default-heap buffer bypasses that limit without changing page
// sizes for unrelated rendering. COMMON is the initial state; the existing
// native upload/barrier path manages subsequent use. No upload heap is owned here.
inline HRESULT Create(ID3D12Device* device,uint32_t bytes,Allocation& out) {
    if (!device || !bytes) return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=bytes;
    desc.Height=1;
    desc.DepthOrArraySize=1;
    desc.MipLevels=1;
    desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource=nullptr;
    const auto result=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,__uuidof(ID3D12Resource),reinterpret_cast<void**>(&resource));
    if (FAILED(result)) return result;
    // Publish only a fully created allocation. ReleaseGeometry intercepts the
    // private page marker before the generic renderer can interpret it as an index.
    out={bytes,0,OwnedPage,0,resource};
    return S_OK;
}
// Called at the renderer's existing retirement point, not when a decal fades:
// many decals share this whole buffer. The descriptor owns the resource's COM
// reference. False means it belongs to vanilla and must use vanilla's release.
inline bool Release(Allocation& allocation) {
    if (allocation.page!=OwnedPage) return false;
    if (allocation.resource) allocation.resource->Release();
    allocation={};
    return true;
}
}
