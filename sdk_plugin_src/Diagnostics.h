#pragma once
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace Diagnostics {
inline HANDLE file = INVALID_HANDLE_VALUE;
inline void* handler = nullptr;
inline volatile LONG exceptions = 0;

inline void Write(const char* text, size_t length) {
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    FlushFileBuffers(file);
}
inline void Line(const std::string& text) {
    Write(text.data(), text.size());
    Write("\r\n", 2);
}

// First-chance capture survives Glacier's own exception handler. It only
// records evidence; the exception continues through the original handlers.
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
    _snprintf_s(buffer,sizeof(buffer),_TRUNCATE,"\r\nMoreBloodDecals build 4 (50x retention / dedicated GPU buffers): %04u-%02u-%02u %02u:%02u:%02u PID=%lu\r\n",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,GetCurrentProcessId());
    Write(buffer,std::strlen(buffer));
    handler=AddVectoredExceptionHandler(1,Exception);
}
inline void Stop() {
    if (handler) RemoveVectoredExceptionHandler(handler);
    handler=nullptr;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    file=INVALID_HANDLE_VALUE;
}
}
