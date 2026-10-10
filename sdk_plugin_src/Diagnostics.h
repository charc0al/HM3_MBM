#pragma once
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>

namespace Diagnostics {
// Independent append-only logging: the original first-impact crash left the
// SDK log empty. Normal telemetry uses the OS file cache; faults/startup flush
// explicitly. Avoid forcing storage synchronization from the render/update lock.
// This facility observes failures; it does not recover or change exception flow.
inline HANDLE file = INVALID_HANDLE_VALUE;
inline void* handler = nullptr;
inline volatile LONG exceptions = 0;
// Enable researched offsets only after the executable and patch guards pass.
// ReadProcessMemory keeps sampling/exception evidence independent of native
// locks; registry counters are observations, not an atomic allocator snapshot.
inline std::atomic<uintptr_t> nativeBase=0;
struct RegistryUsage { uint32_t used=0, capacity=0; };
inline bool ReadMemory(uintptr_t address,void* destination,size_t size) {
    SIZE_T bytes=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),destination,size,&bytes) && bytes==size;
}
inline bool Registry(RegistryUsage& usage) {
    const auto base=nativeBase.load();
    return base && ReadMemory(base+0x3194a00,&usage,sizeof(usage));
}
// Retirement can select the same fading decal on successive frames. Aggregate
// selections, not distinct freed slots, and bound disk activity per category.
struct RetirementWindow {
    uint64_t lastReport=0, selections=0;
    uint32_t calls=0, maxCount=0, maxBytes=0;
    bool reported=false;
    void Add(uint32_t selected,uint32_t count,uint32_t bytes) {
        if (!selected) return;
        ++calls;
        selections+=selected;
        if (count>maxCount) maxCount=count;
        if (bytes>maxBytes) maxBytes=bytes;
    }
    bool Due(uint64_t now) const { return calls && (!reported || now-lastReport>=1000); }
    void Reset(uint64_t now) {
        lastReport=now; reported=true; selections=0; calls=0; maxCount=0; maxBytes=0;
    }
};

inline void Write(const char* text, size_t length,bool flush=true) {
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    if (flush) FlushFileBuffers(file);
}
inline void Line(const std::string& text) {
    const auto line=text+"\r\n";
    Write(line.data(),line.size(),false);
}

// The stress fault returned from this slot's render-object Release call. Dump
// the handle and registry owner only for that exact native call site, using safe
// reads throughout; an arbitrary fault's RBP must not be interpreted as a slot.
inline void NativeFault(const CONTEXT& context) {
    const auto base=nativeBase.load();
    if (!base) return;
    char buffer[512]{};
    RegistryUsage usage{};
    if (Registry(usage)) {
        const int count=_snprintf_s(buffer,sizeof(buffer),_TRUNCATE,
            "Shared render registry at fault: used=%u capacity=%u\r\n",usage.used,usage.capacity);
        if (count>0) Write(buffer,static_cast<size_t>(count));
    }
    uintptr_t caller=0;
    uint16_t handle=0;
    if (!ReadMemory(context.Rsp,&caller,sizeof(caller)) || caller!=base+0x12d558d ||
        !ReadMemory(context.Rbp+0x96,&handle,sizeof(handle))) return;
    uintptr_t owner=0, vtable=0, release=0;
    const bool ownerRead=ReadMemory(base+0x3bf1a60+static_cast<uintptr_t>(handle)*160+0x90,&owner,sizeof(owner));
    const bool vtableRead=ownerRead && owner && ReadMemory(owner,&vtable,sizeof(vtable));
    const bool releaseRead=vtableRead && vtable && ReadMemory(vtable+0x10,&release,sizeof(release));
    const int count=_snprintf_s(buffer,sizeof(buffer),_TRUNCATE,
        "Decal cleanup reference: slot=%llX handle=%u owner=%llX vtable=%llX release=%llX readable=%d/%d/%d\r\n",
        context.Rbp,static_cast<unsigned>(handle),static_cast<unsigned long long>(owner),
        static_cast<unsigned long long>(vtable),static_cast<unsigned long long>(release),
        ownerRead,vtableRead,releaseRead);
    if (count>0) Write(buffer,static_cast<size_t>(count));
}

// First-chance capture survives Glacier's own exception handler. It only
// records evidence; the exception continues through the original handlers.
// Capture is bounded to 32 selected faults per load. Raw registers and stack
// addresses support offline disassembly; these are not symbolized stack frames.
// Use fixed local buffers rather than the SDK logger from the exception path.
inline LONG CALLBACK Exception(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != 0xc0000409) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&exceptions) > 32) return EXCEPTION_CONTINUE_SEARCH;
    char buffer[2048]{};
    const auto& c = *info->ContextRecord;
    const auto base = reinterpret_cast<size_t>(GetModuleHandleW(nullptr));
    int count = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
        "\r\nEXCEPTION %08lX thread=%lu exe_base=%llX RIP=%llX exe_rva=%llX\r\n"
        "RAX=%llX RBX=%llX RCX=%llX RDX=%llX RSI=%llX RDI=%llX\r\n"
        "R8=%llX R9=%llX R10=%llX R11=%llX R12=%llX R13=%llX R14=%llX R15=%llX\r\n"
        "RSP=%llX RBP=%llX flags=%lX parameters=%lu info0=%llX info1=%llX\r\n",
        code, GetCurrentThreadId(), static_cast<unsigned long long>(base), c.Rip, c.Rip-base,
        c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15,
        c.Rsp,c.Rbp,c.EFlags,info->ExceptionRecord->NumberParameters,
        info->ExceptionRecord->ExceptionInformation[0],info->ExceptionRecord->ExceptionInformation[1]);
    if (count > 0) Write(buffer, static_cast<size_t>(count));
    NativeFault(c);
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(c.Rip), &module)) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module,path,MAX_PATH);
        count = _snprintf_s(buffer,sizeof(buffer),_TRUNCATE,"fault_module=%ls base=%p offset=%llX\r\n",path,module,
            c.Rip-reinterpret_cast<size_t>(module));
        if (count > 0) Write(buffer,static_cast<size_t>(count));
    }
    std::array<unsigned long long,256> stack{};
    SIZE_T bytes = 0;
    ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c.Rsp),stack.data(),sizeof(stack),&bytes);
    for (size_t i=0;i+3<bytes/sizeof(stack[0]);i+=4) {
        count = _snprintf_s(buffer,sizeof(buffer),_TRUNCATE,"stack+%04zX: %016llX %016llX %016llX %016llX\r\n",
            i*8,stack[i],stack[i+1],stack[i+2],stack[i+3]);
        if (count > 0) Write(buffer,static_cast<size_t>(count));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Resolve beside the EXE rather than the process cwd, which the loader can vary.
// Append across sessions and label each startup so the prior crash remains visible.
inline void Start() {
    wchar_t path[32768]{};
    const auto length=GetModuleFileNameW(nullptr,path,32768);
    if (!length || length>=32768) return;
    std::wstring filename(path,length);
    filename.resize(filename.find_last_of(L"\\/")+1);
    filename+=L"MoreBloodDecals.diagnostic.log";
    file=CreateFileW(filename.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ | FILE_SHARE_WRITE,nullptr,
                     OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[256]{};
    _snprintf_s(buffer,sizeof(buffer),_TRUNCATE,"\r\nMoreBloodDecals build 6 (bounded admission / pressure cleanup): %04u-%02u-%02u %02u:%02u:%02u PID=%lu\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
    Write(buffer,std::strlen(buffer));
    handler=AddVectoredExceptionHandler(1,Exception);
}
// Releasing diagnostics is independent of native bridge/resource lifetime;
// pinned callbacks remain valid after the SDK destroys its plugin instance.
inline void Stop() {
    nativeBase.store(0);
    if (handler) RemoveVectoredExceptionHandler(handler);
    handler=nullptr;
    if (file != INVALID_HANDLE_VALUE) { FlushFileBuffers(file); CloseHandle(file); }
    file=INVALID_HANDLE_VALUE;
}
}
