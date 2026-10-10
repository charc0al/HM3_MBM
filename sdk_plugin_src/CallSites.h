// Generated native call guards.
// Each row guards one original E8 rel32 CALL before its displacement is replaced.
// Order must match PrepareThunks targets: allocation, two releases, retirement.
// Calls route through nearby absolute-jump thunks; the native callees stay intact.
#pragma once
#include <array>
#include <cstddef>
struct NativeCallSite { const char* name; size_t rva; std::array<unsigned char,5> expected; };
inline std::array<NativeCallSite,4> NativeCallSites() { return {{
{"geometry allocation",0x13526cf,{0xe8,0x2c,0x31,0xf8,0xff}},
{"buffer destructor release",0x121ccfd,{0xe8,0xbe,0x81,0xfc,0xff}},
{"buffer migration release",0x121cd86,{0xe8,0x35,0x81,0xfc,0xff}},
{"retirement diagnostics",0x12d604b,{0xe8,0xd0,0x01,0x00,0x00}},
}}; }
