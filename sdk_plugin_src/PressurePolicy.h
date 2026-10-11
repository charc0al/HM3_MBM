#pragma once
#include "Capacity.h"
#include <cstdint>
#include <algorithm>

namespace PressurePolicy {
// Enlarging decal storage does not enlarge the renderer's shared uint16 handle
// registry. Leave room for particles, scene objects and allocations on other
// threads. Hysteresis starts eviction before admission has to stop and avoids
// repeatedly opening/closing admission at one boundary. Build 7 uses more of
// this pool after build 6's ordinary-weapon tests: at capacity 65536, start
// cleanup with 4096 free, recover to 6144, and stop admission with 2048 free.
// These reserves observe shared counters; they cannot reserve handles against
// unrelated renderer threads. Keep creation throttling and bounded cleanup.
inline constexpr uint32_t Free(uint32_t used,uint32_t capacity) { return used<capacity ? capacity-used : 0; }
inline constexpr uint32_t AdmissionReserve(uint32_t capacity) { return capacity/32; }
inline constexpr uint32_t CleanupReserve(uint32_t capacity) { return capacity/16; }
inline constexpr uint32_t RecoveryReserve(uint32_t capacity) { return capacity/16+capacity/32; }
inline bool CanAdmit(uint32_t used,uint32_t capacity) { return capacity && Free(used,capacity)>AdmissionReserve(capacity); }
inline bool UpdatePressure(bool active,uint32_t used,uint32_t capacity) {
    if (!capacity) return true;
    const auto free=Free(used,capacity);
    return active ? free<RecoveryReserve(capacity) : free<=CleanupReserve(capacity);
}

// Bound both CPU creation work and transient geometry uploads. These are burst
// and throughput limits, not a smaller persistent pool. Tokens refill with wall
// time; the per-update cap prevents a slow frame from admitting a giant burst.
struct Admission {
    static constexpr uint32_t PerUpdate=256;
    static constexpr double PerSecond=4096, ByteRate=8*1024*1024, ByteBurst=256*1024;
    uint64_t last=0;
    double countTokens=PerUpdate, byteTokens=ByteBurst;
    uint32_t thisUpdate=0;
    bool initialized=false;
    void NewUpdate() { thisUpdate=0; }
    bool Allow(uint64_t now,uint64_t bytes) {
        if (!initialized) { last=now; initialized=true; }
        const auto seconds=static_cast<double>(now-last)/1000;
        last=now;
        countTokens=std::min<double>(PerUpdate,countTokens+seconds*PerSecond);
        byteTokens=std::min(ByteBurst,byteTokens+seconds*ByteRate);
        if (thisUpdate>=PerUpdate || countTokens<1 || byteTokens<bytes) return false;
        ++thisUpdate; countTokens-=1; byteTokens-=static_cast<double>(bytes);
        return true;
    }
};

template<typename T> inline T& Field(void* object,size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object)+offset);
}

// Preserve native list/geometry ownership. We only schedule expiry through the
// same flag, timer and MSB-first fading bitset used by native 12D6220; native
// 12D6400 then releases references/blocks and returns slots at its usual point
// later in this update. Never unlink or destroy a render object in our wrapper.
// Soft pressure gets a short fade. Shared-registry pressure expires old marks
// immediately, in bounded batches, because another several-second fade cannot
// protect a global allocator that is about to exhaust.
inline uint32_t Retire(void* manager,uint32_t* stats,uint32_t category,uint32_t multiplier,bool immediatePressure) {
    constexpr uint32_t MaxScheduled=256, MaxExamined=1024;
    constexpr float PressureFade=0.15f;
    auto* slots=Field<unsigned char*>(manager,8);
    auto slot=Field<uint16_t>(manager,0x30+category*12);
    uint32_t categoryCount=Field<uint16_t>(manager,0x32+category*12);
    uint32_t triangles=Field<uint32_t>(manager,0x34+category*12);
    uint32_t categoryBytes=Field<uint32_t>(manager,0x38+category*12);
    uint32_t scheduled=0, examined=0;
    const auto subtract=[](uint32_t a,uint32_t b) { return a>b ? a-b : 0U; };
    while (slot && slot<=Capacity::Slots(multiplier) && examined<MaxExamined && scheduled<MaxScheduled) {
        const bool hard=stats[1]>Capacity::HardCount(multiplier);
        const bool pressure=(categoryCount>600U*multiplier && stats[1]>1200U*multiplier) || hard ||
            (categoryBytes>0x80000U*multiplier && stats[0]>0x100000U*multiplier) || triangles>100000U*multiplier;
        if (!immediatePressure && !pressure) break;
        ++examined;
        auto* entry=slots+static_cast<size_t>(slot)*192;
        auto& flags=Field<uint8_t>(entry,0xbc);
        auto& timer=Field<float>(entry,0xb0);
        const float target=(immediatePressure || hard) ? 0.0f : PressureFade;
        // Do not restart an already-running fade on every pressure frame.
        if (!(flags&2) || timer>target) { timer=target; ++scheduled; }
        flags=static_cast<uint8_t>((flags&0xef)|2);
        const auto bit=static_cast<uint32_t>(slot)-1;
        Field<uint64_t>(manager,0xc0+Capacity::MaskWords(multiplier)*8+(bit/64)*8)|=uint64_t{1}<<(63-bit%64);
        const auto bytes=Field<uint32_t>(entry,0)+Field<uint32_t>(entry,8);
        const auto slotTriangles=Field<uint16_t>(entry,0x14)/3U;
        categoryCount=subtract(categoryCount,1);
        categoryBytes=subtract(categoryBytes,bytes);
        triangles=subtract(triangles,slotTriangles);
        stats[0]=subtract(stats[0],bytes); stats[1]=subtract(stats[1],1);
        ++stats[2]; stats[3]+=slotTriangles; stats[4]+=bytes;
        slot=Field<uint16_t>(entry,0x16);
    }
    return scheduled;
}
}
