#pragma once
#include "NativeResources.h"
#include "Capacity.h"
#include "Diagnostics.h"
#include <mutex>
#include <atomic>

namespace NativeBridge {
inline unsigned char* base=nullptr;
inline uint32_t multiplier=0;
inline std::mutex resourceMutex;
inline std::atomic<uint64_t> seenControllers=0;
inline uint64_t lastStats=0;
inline uint32_t allocationFailures=0;

template<typename T> inline T& Field(void* object,size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object)+offset);
}
inline bool EnsureBuffers(void* manager) {
    std::lock_guard<std::mutex> guard(resourceMutex);
    auto* renderer=*reinterpret_cast<unsigned char**>(base+0x3194b80);
    auto* device=Field<ID3D12Device*>(renderer,0x10ab8);
    for (const size_t offset : {0x48U,0x50U}) {
        auto* buffer=Field<void*>(manager,offset);
        const auto bytes=Field<uint32_t>(buffer,0x10);
        if (bytes<=0x400000) continue;
        auto& allocation=Field<NativeResources::Allocation>(buffer,0x28);
        if (allocation.bytes) continue;
        const auto result=NativeResources::Create(device,bytes,allocation);
        if (FAILED(result)) {
            if (allocationFailures++<8) Diagnostics::Line(fmt::format("Dedicated decal allocation FAILED: bytes={}, HRESULT={:08X}; rejecting decal before CPU geometry allocation",bytes,static_cast<uint32_t>(result)));
            return false;
        }
        const auto address=allocation.resource->GetGPUVirtualAddress();
        Field<uint64_t>(buffer,0x60)=address;
        Diagnostics::Line(fmt::format("Dedicated decal buffer created: object={:X}, bytes={}, GPU address={:X}",reinterpret_cast<size_t>(buffer),bytes,address));
    }
    return true;
}

// This call occurs before any geometry blocks are reserved. If allocation
// fails, vanilla's existing false-return path returns the unused slot safely.
inline bool AllocateGeometry(void* manager,void** indices,void** vertices,uint16_t slot,
                             bool category,uint32_t indexCount,uint32_t vertexBytes,uint32_t stride) {
    if (!EnsureBuffers(manager)) return false;
    using Function=bool(*)(void*,void**,void**,uint16_t,bool,uint32_t,uint32_t,uint32_t);
    return reinterpret_cast<Function>(base+0x12d5800)(manager,indices,vertices,slot,category,indexCount,vertexBytes,stride);
}

// The native descriptor destructor is already deferred through the renderer's
// retirement queue. Only its final release operation changes for owned buffers.
inline void ReleaseGeometry(void* renderer,NativeResources::Allocation* allocation) {
    if (NativeResources::Release(*allocation)) {
        Diagnostics::Line("Dedicated decal buffer released by native resource retirement.");
        return;
    }
    using Function=void(*)(void*,NativeResources::Allocation*);
    reinterpret_cast<Function>(base+0x11e4ec0)(renderer,allocation);
}

inline void LogController(void* manager,uint16_t slot,uint32_t category) {
    if (!slot) return;
    auto* slots=Field<unsigned char*>(manager,8);
    const auto id=Field<uint32_t>(slots+slot*192,0x80);
    if (!id || id>64) return;
    const auto bit=uint64_t{1}<<(id-1);
    if (seenControllers.fetch_or(bit)&bit) return;
    auto* controllers=*reinterpret_cast<unsigned char**>(base+0x3947aa8);
    auto* controller=controllers+(id-1)*48;
    Diagnostics::Line(fmt::format("Controller {} category {}: expirationFade={}s, autoFadeMin={}s, autoFadeMax={}s",
        id,category,Field<float>(controller,0x18),Field<float>(controller,0x20),Field<float>(controller,0x24)));
}

inline void Retire(void* manager,uint32_t* stats,uint32_t category) {
    const auto removedBefore=stats[2];
    const auto countBefore=stats[1];
    const auto bytesBefore=stats[0];
    const auto categoryCount=Field<uint16_t>(manager,0x32+category*12);
    const auto categoryBytes=Field<uint32_t>(manager,0x38+category*12);
    const auto triangles=Field<uint32_t>(manager,0x34+category*12);
    const auto now=GetTickCount64();
    if (category==0 && now-lastStats>=5000) {
        lastStats=now;
        Diagnostics::Line(fmt::format("Pool usage: slots={}/{}; geometry={} / {} bytes; categories={}/{}; count-soft={}, geometry-soft={} bytes",
            countBefore,Capacity::Slots(multiplier),bytesBefore,0x200000U*multiplier,
            Field<uint16_t>(manager,0x32),Field<uint16_t>(manager,0x3e),1200U*multiplier,0x100000U*multiplier));
        auto* slots=Field<unsigned char*>(manager,8);
        for (uint32_t list=0;list<2;++list) {
            auto slot=Field<uint16_t>(manager,0x30+list*12);
            for (uint32_t visited=0;slot && visited<Capacity::Slots(multiplier);++visited) {
                LogController(manager,slot,list);
                slot=Field<uint16_t>(slots+slot*192,0x16);
            }
        }
    }
    const auto head=Field<uint16_t>(manager,0x30+category*12);
    LogController(manager,head,category);
    using Function=void(*)(void*,uint32_t*,uint32_t);
    reinterpret_cast<Function>(base+0x12d6220)(manager,stats,category);
    if (stats[2]!=removedBefore) {
        Diagnostics::Line(fmt::format("Capacity retirement: category={}, retired={}, globalCount={}, categoryCount={}, geometryBytes={}, categoryBytes={}, triangles={}; countPressure={}, geometryPressure={}, trianglePressure={}",
            category,stats[2]-removedBefore,countBefore,categoryCount,bytesBefore,categoryBytes,triangles,
            (categoryCount>600U*multiplier && countBefore>1200U*multiplier)||countBefore>Capacity::HardCount(multiplier),
            categoryBytes>0x80000U*multiplier && bytesBefore>0x100000U*multiplier,triangles>100000U*multiplier));
    }
}
}
