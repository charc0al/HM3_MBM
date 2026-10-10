#pragma once
#include "NativeResources.h"
#include "Capacity.h"
#include "Diagnostics.h"
#include "PressurePolicy.h"
#include <mutex>
#include <atomic>

namespace NativeBridge {
// This state outlives the SDK plugin object because the module is pinned.
// The resource mutex serializes our lazy descriptor publication; native code
// remains responsible for synchronizing its own decal lists and geometry pools.
inline unsigned char* base=nullptr;
inline uint32_t multiplier=0;
inline std::mutex resourceMutex;
inline std::atomic<uint64_t> seenControllers=0;
inline uint64_t lastStats=0;
inline uint32_t allocationFailures=0;
inline Diagnostics::RetirementWindow retirementWindows[2];
inline uint64_t lastRegistrySample=0;
inline uint32_t registryHighWater=0;
inline Diagnostics::RegistryUsage registryUsage;
// All policy state is protected by the native manager's recursive critical
// section. Allocation takes it explicitly; the update/retirement caller holds it.
inline PressurePolicy::Admission admission;
inline bool sharedPressure=false;
inline bool geometryFailurePending=false, geometryRecovery=false;
inline uint64_t registryRejected=0, rateRejected=0, allocationRejected=0, pressureScheduled=0;

// Offsets are researched layouts for the hash-guarded EXE, not SDK contracts.
// Manager: +8 slots; +0x30/+0x3C category lists; +0x48/+0x50 index/vertex objects.
// Each category is {uint16 head, uint16 count, uint32 triangles, uint32 bytes}.
// Buffer: +0x10 capacity bytes; +0x28 allocation descriptor; +0x60 GPU address.
template<typename T> inline T& Field(void* object,size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object)+offset);
}
// Allocate on first use, when the renderer/device exist. Pre-populating the
// descriptor makes native upload bypass its fixed-page allocation branch while
// retaining native command submission, barriers and offset calculations. Buffers
// <=4 MiB continue to use vanilla's path. The first buffer can survive a failure
// creating the second; a later attempt skips it and retries the missing buffer.
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
// Only the specific decal caller is redirected; the native allocator itself
// remains untouched and is called with its original ABI and arguments.
inline bool AllocateGeometry(void* manager,void** indices,void** vertices,uint16_t slot,
                             bool category,uint32_t indexCount,uint32_t vertexBytes,uint32_t stride) {
    auto* lock=reinterpret_cast<CRITICAL_SECTION*>(static_cast<unsigned char*>(manager)+0x88);
    EnterCriticalSection(lock);
    // The exact EXE/layout is guarded at startup. These aligned native counters
    // are observations, not reservations; the large free margin covers other
    // renderer threads. Check every admission, not just five-second telemetry.
    const auto used=*reinterpret_cast<volatile uint32_t*>(base+0x3194a00);
    const auto capacity=*reinterpret_cast<volatile uint32_t*>(base+0x3194a04);
    const uint64_t bytes=((static_cast<uint64_t>(indexCount)*2+511)&~uint64_t{511})+
        ((static_cast<uint64_t>(vertexBytes)+stride+511)&~uint64_t{511});
    bool allowed=true;
    if (!PressurePolicy::CanAdmit(used,capacity)) { ++registryRejected; allowed=false; }
    else if (!admission.Allow(GetTickCount64(),bytes)) { ++rateRejected; allowed=false; }
    else if (!EnsureBuffers(manager)) { ++allocationRejected; allowed=false; }
    using Function=bool(*)(void*,void**,void**,uint16_t,bool,uint32_t,uint32_t,uint32_t);
    // false takes the caller's existing unused-slot return/temporary cleanup
    // route, before geometry reservation and before persistent render cloning.
    const bool result=allowed && reinterpret_cast<Function>(base+0x12d5800)(manager,indices,vertices,slot,category,indexCount,vertexBytes,stride);
    if (allowed && !result) { ++allocationRejected; geometryFailurePending=true; }
    LeaveCriticalSection(lock);
    return result;
}

// The native descriptor destructor is already deferred through the renderer's
// retirement queue. Only its final release operation changes for owned buffers.
// Both final destruction and upload migration enter here. The private marker
// prevents our dedicated resources from reaching vanilla's page/free-list logic;
// everything else is delegated, including allocations from unrelated buffers
// that happen to use these same native buffer methods.
inline void ReleaseGeometry(void* renderer,NativeResources::Allocation* allocation) {
    if (NativeResources::Release(*allocation)) {
        Diagnostics::Line("Dedicated decal buffer released by native resource retirement.");
        return;
    }
    using Function=void(*)(void*,NativeResources::Allocation*);
    reinterpret_cast<Function>(base+0x11e4ec0)(renderer,allocation);
}

// Controller IDs identify a separate 64-entry metadata pool, not decal capacity
// or resource hashes. Log the first observation per ID to expose fade policies;
// this is a diagnostic snapshot, not tracking of later parameter changes/reuse.
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

// Its caller holds the manager's critical section and invokes us for
// each of two categories sharing global budgets. stats is an in/out aggregate:
// [geometryBytes, count, retiredCount, retiredTriangles, retiredGeometryBytes].
// Retirement schedules fading (or immediate expiry under hard pressure), so
// retiredCount is not necessarily the number of GPU slots freed this frame.
inline void Retire(void* manager,uint32_t* stats,uint32_t category) {
    const auto removedBefore=stats[2];
    const auto countBefore=stats[1];
    const auto bytesBefore=stats[0];
    const auto categoryCount=Field<uint16_t>(manager,0x32+category*12);
    const auto categoryBytes=Field<uint32_t>(manager,0x38+category*12);
    const auto triangles=Field<uint32_t>(manager,0x34+category*12);
    const auto now=GetTickCount64();
    if (category==0) {
        admission.NewUpdate();
        const auto used=*reinterpret_cast<volatile uint32_t*>(base+0x3194a00);
        const auto capacity=*reinterpret_cast<volatile uint32_t*>(base+0x3194a04);
        sharedPressure=PressurePolicy::UpdatePressure(sharedPressure,used,capacity);
        // Independent index/vertex exhaustion or fragmentation can reject an
        // allocation below the combined byte budget. Free a bounded old batch
        // next update rather than repeatedly retrying a permanently full pool.
        geometryRecovery=geometryFailurePending;
        geometryFailurePending=false;
    }
    // Observe the separate global render-object allocator twice per second.
    // Never acquire its lock while the decal manager's lock is held. The peak
    // is sampled, so short bursts may still be caught only by exception capture.
    if (category==0 && now-lastRegistrySample>=500) {
        lastRegistrySample=now;
        if (Diagnostics::Registry(registryUsage) && registryUsage.used>registryHighWater)
            registryHighWater=registryUsage.used;
    }
    if (category==0 && now-lastStats>=5000) {
        lastStats=now;
        Diagnostics::Line(fmt::format("Pool usage: slots={}/{}; geometry={} / {} bytes; categories={}/{}; count-soft={}, geometry-soft={} bytes",
            countBefore,Capacity::Slots(multiplier),bytesBefore,0x200000U*multiplier,
            Field<uint16_t>(manager,0x32),Field<uint16_t>(manager,0x3e),1200U*multiplier,0x100000U*multiplier));
        Diagnostics::Line(fmt::format("Shared render registry: used={}, capacity={}, sampledHighWater={} (includes non-decal render objects)",
            registryUsage.used,registryUsage.capacity,registryHighWater));
        Diagnostics::Line(fmt::format("Pressure control totals: sharedPressure={}, geometryRecovery={}, registryRejected={}, rateRejected={}, allocationRejected={}, expiryScheduled={}",
            sharedPressure,geometryRecovery,registryRejected,rateRejected,allocationRejected,pressureScheduled));
        // Sample a bounded prefix for controllers hidden behind the head;
        // scanning tens of thousands of slots is unnecessary telemetry work.
        auto* slots=Field<unsigned char*>(manager,8);
        for (uint32_t list=0;list<2;++list) {
            auto slot=Field<uint16_t>(manager,0x30+list*12);
            for (uint32_t visited=0;slot && visited<128;++visited) {
                LogController(manager,slot,list);
                slot=Field<uint16_t>(slots+slot*192,0x16);
            }
        }
    }
    const auto head=Field<uint16_t>(manager,0x30+category*12);
    LogController(manager,head,category);
    pressureScheduled+=PressurePolicy::Retire(manager,stats,category,multiplier,sharedPressure || geometryRecovery);
    // Pressure flags describe the pre-call snapshot and can overlap. Timed
    // auto-fade is handled elsewhere and does not appear as capacity retirement.
    auto& window=retirementWindows[category];
    window.Add(stats[2]-removedBefore,countBefore,bytesBefore);
    if (window.Due(now)) {
        Diagnostics::Line(fmt::format("Capacity retirement window: category={}, calls={}, selections={}, peakCount={}, peakGeometryBytes={}; latest globalCount={}, categoryCount={}, geometryBytes={}, categoryBytes={}, triangles={}; countPressure={}, geometryPressure={}, trianglePressure={}, sharedPressure={}",
            category,window.calls,window.selections,window.maxCount,window.maxBytes,countBefore,categoryCount,bytesBefore,categoryBytes,triangles,
            (categoryCount>600U*multiplier && countBefore>1200U*multiplier)||countBefore>Capacity::HardCount(multiplier),
            categoryBytes>0x80000U*multiplier && bytesBefore>0x100000U*multiplier,triangles>100000U*multiplier,sharedPressure));
        window.Reset(now);
    }
}
}
