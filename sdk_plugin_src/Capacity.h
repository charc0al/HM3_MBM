#pragma once
#include "PatchSites.h"
#include <algorithm>

namespace Capacity {
inline uint32_t Slots(uint32_t multiplier) { return std::min(1600U*multiplier,65535U); }
inline uint32_t MaskWords(uint32_t multiplier) { return (Slots(multiplier)+63)/64; }
inline uint32_t HardCount(uint32_t multiplier) { return std::min(1437U*multiplier,Slots(multiplier)-64); }
inline uint32_t Replacement(const PatchSite& site,uint32_t multiplier) {
    const auto slots=Slots(multiplier);
    const auto words=MaskWords(multiplier);
    switch (site.rule) {
        case Rule::ManagerSize: return 0xc0+words*16;
        case Rule::SlotsWithSentinel: return slots+1;
        case Rule::SlotAllocation: return (slots+1)*192+8;
        case Rule::LastNextField: return slots*192+0x16;
        case Rule::SecondMask: return 0xc0+words*8;
        case Rule::SlotCount: return slots;
        case Rule::HardCount: return HardCount(multiplier);
        case Rule::TrackingWords: return words;
        case Rule::TrackingBytes: return words*16;
        default: return site.original*multiplier;
    }
}
}
