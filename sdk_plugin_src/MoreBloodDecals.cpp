#include <IPluginInterface.h>
#include <Logging.h>
#include <bcrypt.h>
#include "PatchSites.h"

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <limits>
#include "Diagnostics.h"
#include "Capacity.h"
#include "CallSites.h"
#include "NativeBridge.h"

// Startup coordinator for a larger *existing* native decal manager. Two changes
// must agree: Capacity.h patches its allocation/layout/retirement constants, and
// NativeBridge.h supplies GPU buffers too large for the generic page allocator.
// Rendering, slot allocation, fade execution and list maintenance remain native.
// PressurePolicy bounds admission and schedules expiry through native fields.
// All native addresses are RVAs in the single executable identified by the hash;
// adding the loaded module base handles ASLR, not compatibility with other builds.
namespace {
constexpr size_t ManagerPointerRva = 0x3194d58;

// The original call sites are five-byte E8 rel32 instructions. The DLL can load
// outside their +/-2 GiB reach, so place tiny tail-jump thunks near the EXE and
// let each thunk jump to a full 64-bit DLL address. This preserves the native
// return address, argument registers and stack arguments (Windows x64 ABI).
// Order is positional: these targets must match NativeCallSites() exactly.
unsigned char* PrepareThunks(unsigned char* base,size_t imageSize) {
    const auto start=(reinterpret_cast<size_t>(base)+imageSize+0xffff)&~size_t{0xffff};
    const auto end=reinterpret_cast<size_t>(base)+0x70000000;
    unsigned char* block=nullptr;
    for (size_t address=start;address<end;) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address),&region,sizeof(region))) break;
        if (region.State==MEM_FREE && region.RegionSize>=0x10000) {
            block=static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(address),0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
            if (block) break;
        }
        address=(reinterpret_cast<size_t>(region.BaseAddress)+region.RegionSize+0xffff)&~size_t{0xffff};
    }
    if (!block) return nullptr;
    const std::array<size_t,4> targets={reinterpret_cast<size_t>(&NativeBridge::AllocateGeometry),
        reinterpret_cast<size_t>(&NativeBridge::ReleaseGeometry),reinterpret_cast<size_t>(&NativeBridge::ReleaseGeometry),
        reinterpret_cast<size_t>(&NativeBridge::Retire)};
    for (size_t i=0;i<targets.size();++i) {
        // jmp qword ptr [rip+0]; absolute target. Does not clobber arguments.
        const unsigned char jump[6]={0xff,0x25,0,0,0,0};
        std::memcpy(block+i*16,jump,sizeof(jump));
        std::memcpy(block+i*16+6,&targets[i],sizeof(size_t));
    }
    DWORD old=0;
    if (!VirtualProtect(block,0x10000,PAGE_EXECUTE_READ,&old)) {
        VirtualFree(block,0,MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(),block,0x10000);
    return block;
}

// Build identification is deliberately stricter than a version string. The
// on-disk hash identifies the researched binary; Init also checks in-memory
// instructions to reject conflicting patches applied by another plugin.
std::string ExecutableSha256() {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    std::ifstream file(std::filesystem::path(path.data()), std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    bool success = true;
    std::array<unsigned char, 65536> buffer{};
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = file.gcount();
        if (count > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) {
            success = false;
            break;
        }
    }
    if (!file.eof()) success = false;
    std::array<unsigned char, 32> digest{};
    if (success && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) success = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

template<typename T> std::optional<T> Read(size_t address) {
    T value{};
    SIZE_T bytes = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &bytes) || bytes != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}

// Open every affected code page before changing any instruction. A protection
// failure therefore leaves the complete native layout unchanged.
// This is a startup-only operation, not an atomic live patch: the manager must
// not exist yet, and its native paths must not be running during these writes.
bool Apply(unsigned char* base, const std::array<PatchSite, 35>& sites, uint32_t multiplier,unsigned char* thunks) {
    const auto calls=NativeCallSites();
    std::array<int32_t,4> displacements{};
    for (size_t i=0;i<calls.size();++i) {
        const auto distance=reinterpret_cast<intptr_t>(thunks+i*16)-reinterpret_cast<intptr_t>(base+calls[i].rva+5);
        if (distance<std::numeric_limits<int32_t>::min() || distance>std::numeric_limits<int32_t>::max()) return false;
        displacements[i]=static_cast<int32_t>(distance);
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::map<size_t, DWORD> pages;
    for (const auto& call:NativeCallSites()) {
        const auto address=reinterpret_cast<size_t>(base+call.rva);
        for (auto page=address/system.dwPageSize*system.dwPageSize;
             page<=(address+4)/system.dwPageSize*system.dwPageSize;page+=system.dwPageSize) pages.emplace(page,0);
    }
    // Layout fields include constructor allocations, destructor loop bounds and
    // relocated bitset operands. Patching just the visible limits would leave
    // native accesses addressing the old allocation or the wrong bitset.
    for (const auto& site : sites) {
        const auto address = reinterpret_cast<size_t>(base + site.rva + site.byteOffset);
        for (auto page = address / system.dwPageSize * system.dwPageSize;
             page <= (address + sizeof(uint32_t) - 1) / system.dwPageSize * system.dwPageSize;
             page += system.dwPageSize) {
            pages.emplace(page, 0);
        }
    }
    std::vector<std::pair<size_t, DWORD>> opened;
    for (const auto& [page, unused] : pages) {
        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(page), system.dwPageSize, PAGE_EXECUTE_READWRITE, &previous)) {
            for (const auto& [address, protection] : opened) {
                DWORD ignored = 0;
                VirtualProtect(reinterpret_cast<void*>(address), system.dwPageSize, protection, &ignored);
            }
            return false;
        }
        opened.emplace_back(page, previous);
    }
    for (const auto& site : sites) {
        const auto value = Capacity::Replacement(site, multiplier);
        std::memcpy(base + site.rva + site.byteOffset, &value, sizeof(value));
    }
    // Keep the CALL opcode and replace only its displacement. Both release
    // sites use the same bridge, covering destruction and upload migration.
    for (size_t i=0;i<calls.size();++i) {
        std::memcpy(base+calls[i].rva+1,&displacements[i],sizeof(int32_t));
    }
    for (const auto& [address, protection] : opened) {
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), system.dwPageSize);
        DWORD ignored = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), system.dwPageSize, protection, &ignored)) {
            Logger::Warn("[MoreBloodDecals] Could not restore code page protection: {}.", GetLastError());
        }
    }
    return true;
}
}

class MoreBloodDecals : public IPluginInterface {
public:
    void Init() override {
        if (initialized_) return;
        initialized_ = true;
        Diagnostics::Start();
        const auto enabled = GetSettingBool("decals", "enabled", true);
        const auto multiplier = GetSettingInt("decals", "capacity_multiplier", 50);
        Diagnostics::Line(fmt::format("Settings: enabled={}, multiplier={}",enabled,multiplier));
        if (!HasSetting("decals", "enabled")) SetSettingBool("decals", "enabled", enabled);
        if (!HasSetting("decals", "capacity_multiplier")) SetSettingInt("decals", "capacity_multiplier", multiplier);
        if (!enabled || multiplier == 1) {
            Logger::Info("[MoreBloodDecals] Using vanilla capacity. Setting changes require a full restart.");
            return;
        }
        if (multiplier < 1 || multiplier > 50) {
            Logger::Warn("[MoreBloodDecals] capacity_multiplier must be 1..50; no changes applied.");
            return;
        }
        const auto digest = ExecutableSha256();
        Diagnostics::Line(fmt::format("Executable SHA-256: {}",digest));
        if (digest != ExpectedExecutableSha256) {
            Logger::Warn("[MoreBloodDecals] Unsupported executable SHA-256 {}; no changes applied.", digest);
            return;
        }
        // Constants describe the allocation the constructor is about to make.
        // An already-created manager cannot be resized by changing its code.
        base_ = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto manager = Read<size_t>(reinterpret_cast<size_t>(base_) + ManagerPointerRva);
        Diagnostics::Line(fmt::format("Executable base={:X}; pre-patch manager={:X}",reinterpret_cast<size_t>(base_),manager.value_or(0)));
        if (!manager || *manager != 0) {
            Logger::Warn("[MoreBloodDecals] Decal manager already exists or cannot be inspected. Enable at startup and fully restart; no changes applied.");
            return;
        }
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        // Validate complete instructions, not just the four-byte operands we
        // replace. All guards must pass before publishing any modified code.
        const auto sites = PatchSites();
        for (const auto& site : sites) {
            if (site.rva + site.expected.size() > imageSize ||
                std::memcmp(base_ + site.rva, site.expected.data(), site.expected.size()) != 0) {
                Logger::Warn("[MoreBloodDecals] Unexpected instruction bytes at {}; no changes applied.", site.name);
                return;
            }
        }
        for (const auto& call:NativeCallSites()) {
            if (call.rva+call.expected.size()>imageSize || std::memcmp(base_+call.rva,call.expected.data(),call.expected.size())!=0) {
                Diagnostics::Line(fmt::format("Unexpected native call bytes at {}; no changes applied",call.name));
                return;
            }
        }
        // Native calls will enter static DLL functions without an IPluginInterface
        // instance. Pinning keeps those functions and their state valid even if
        // the SDK unloads the plugin object; live disable is intentionally unsupported.
        HMODULE pinned=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&NativeBridge::AllocateGeometry),&pinned)) {
            Diagnostics::Line("Could not pin native bridge module; no changes applied");
            return;
        }
        auto* thunks=PrepareThunks(base_,imageSize);
        if (!thunks) {
            Diagnostics::Line("Could not allocate nearby bridge thunks; no changes applied");
            return;
        }
        multiplier_ = static_cast<uint32_t>(multiplier);
        NativeBridge::base=base_;
        NativeBridge::multiplier=multiplier_;
        if (!Apply(base_, sites, multiplier_,thunks)) {
            VirtualFree(thunks,0,MEM_RELEASE);
            Logger::Warn("[MoreBloodDecals] Could not open code pages; no changes applied.");
            return;
        }
        applied_ = true;
        Diagnostics::nativeBase.store(reinterpret_cast<uintptr_t>(base_));
        Diagnostics::Line(fmt::format("Applied {} capacity sites and {} native bridges: {}x retention, {} slots, category/global soft counts {}/{}, hard global {}, geometry {} MiB; bridge module pinned until exit",
            sites.size(),NativeCallSites().size(),multiplier_,Capacity::Slots(multiplier_),600U*multiplier_,1200U*multiplier_,Capacity::HardCount(multiplier_),2U*multiplier_));
        Logger::Info("[MoreBloodDecals] {}x retention test: {} slots, {} MiB geometry; dedicated large buffers. In-game validation required.",
            multiplier_,Capacity::Slots(multiplier_),2U*multiplier_);
    }

    // Confirm the native constructor actually used the patched dimensions.
    // These are CPU-side descriptors; dedicated GPU allocation is lazy and is
    // confirmed separately when the first decal reaches AllocateGeometry.
    void OnEngineInitialized() override {
        Diagnostics::Line(fmt::format("OnEngineInitialized: applied={}",applied_));
        if (!applied_) return;
        const auto manager = Read<size_t>(reinterpret_cast<size_t>(base_) + ManagerPointerRva);
        if (!manager || !*manager) {
            Logger::Warn("[MoreBloodDecals] Manager is not available at engine-initialized notification; allocation not yet confirmed.");
            return;
        }
        const auto slots = Read<size_t>(*manager + 8);
        // The slot allocation's eight-byte array header includes sentinel slot 0.
        const auto count = slots && *slots ? Read<uint64_t>(*slots - 8) : std::nullopt;
        const auto indices = Read<uint32_t>(*manager + 0x64);
        const auto vertices = Read<uint32_t>(*manager + 0x7c);
        const auto indexObject = Read<size_t>(*manager + 0x48);
        const auto vertexObject = Read<size_t>(*manager + 0x50);
        const auto indexBytes = indexObject && *indexObject ? Read<uint32_t>(*indexObject + 0x10) : std::nullopt;
        const auto vertexBytes = vertexObject && *vertexObject ? Read<uint32_t>(*vertexObject + 0x10) : std::nullopt;
        Diagnostics::Line(fmt::format("Runtime manager={:X}; slots={}; index/vertex blocks={}/{}; buffer bytes={}/{}",
            *manager,count.value_or(0),indices.value_or(0),vertices.value_or(0),indexBytes.value_or(0),vertexBytes.value_or(0)));
        if (count != static_cast<uint64_t>(Capacity::Slots(multiplier_))+1 || indices != 1600U * multiplier_ || vertices != 2496U * multiplier_ ||
            indexBytes != 0xc8000U * multiplier_ || vertexBytes != 0x138000U * multiplier_) {
            Logger::Error("[MoreBloodDecals] Runtime allocation did not match the requested capacity. Restart and retain the SDK log.");
            return;
        }
        Logger::Info("[MoreBloodDecals] Runtime allocation confirmed: {} usable slots; index/vertex buffers {}/{} bytes.",
                     *count - 1, *indexBytes, *vertexBytes);
    }

    ~MoreBloodDecals() override {
        // Native instructions now address an enlarged native allocation. Restoring
        // old offsets/counts while it exists would corrupt its tracking layout.
        // Native bridges own large resources and must remain callable. This DLL
        // is pinned; static bridge state and thunks live until process exit.
        if (applied_) Logger::Info("[MoreBloodDecals] Capacity changes remain active until game exit. Fully restart to disable or change the multiplier.");
        Diagnostics::Line("Plugin unloading; removing exception capture.");
        Diagnostics::Stop();
    }

private:
    unsigned char* base_ = nullptr;
    uint32_t multiplier_ = 0;
    bool initialized_ = false;
    bool applied_ = false;
};

DEFINE_ZHM_PLUGIN(MoreBloodDecals);
