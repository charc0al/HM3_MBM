#pragma once
#include "PatchSites.h"
#include <algorithm>

namespace Capacity {
// Retention is constrained independently by slot count, geometry bytes and
// triangles. Scale all three; this does not extend controllers' time-based fades.
// At 50x the global soft count is 60,000, not 80,000: vanilla begins retirement
// at 1,200 even though it allocates 1,600 slots. Physical capacity therefore fits
// in uint16_t without changing native slot references (zero is the null sentinel).
inline uint32_t Slots(uint32_t multiplier) { return std::min(1600U*multiplier,65535U); }
inline uint32_t MaskWords(uint32_t multiplier) { return (Slots(multiplier)+63)/64; }
// Preserve a margin between forced retirement and physical exhaustion. The
// 64-slot headroom is our chosen cap policy, not a widened native ID format.
inline uint32_t HardCount(uint32_t multiplier) { return std::min(1437U*multiplier,Slots(multiplier)-64); }
// One layout model feeds every patched operand. The manager has an unchanged
// 0xC0-byte prefix followed by growth and fading bitsets, each rounded to uint64_t
// words. Slots are 192-byte records; their allocation has an eight-byte header.
// Constructor, mutation, fade/growth scans and destructor must use this same layout.
inline uint32_t Replacement(const PatchSite& site,uint32_t multiplier) {
    const auto slots=Slots(multiplier);
    const auto words=MaskWords(multiplier);
    switch (site.rule) {
        case Rule::ManagerSize: return 0xc0+words*16;
        case Rule::SlotsWithSentinel: return slots+1;
        case Rule::SlotAllocation: return (slots+1)*192+8;
        // Terminate the free list at the final slot's native next-link field.
        case Rule::LastNextField: return slots*192+0x16;
        case Rule::SecondMask: return 0xc0+words*8;
        case Rule::SlotCount: return slots;
        case Rule::HardCount: return HardCount(multiplier);
        case Rule::TrackingWords: return words;
        case Rule::TrackingBytes: return words*16;
        // Geometry uses independent 512-byte block allocators and their own
        // bitmaps. These sizes/budgets scale fully even after slot IDs hit the cap.
        default: return site.original*multiplier;
    }
}
}
