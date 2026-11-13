// Windows crash report: an unhandled exception or abort() writes the log, then the fault, the
// registers, the stack as module+offset and the loaded modules, and a message box names the log.
#include "port_platform.hpp"
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PSAPI_VERSION 2
#include <psapi.h>

namespace
{
LONG sCrashing;

// No allocation and no CRT stream: the heap or a FILE lock may be what broke.
void Out(const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    n = n < 0 ? 0 : n > (int)sizeof(line) - 3 ? (int)sizeof(line) - 3 : n;
    line[n++] = '\r';
    line[n++] = '\n';
    port_LogWrite(line, n);
}

const char* ExceptionName(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "float denormal operand";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
        case EXCEPTION_FLT_INEXACT_RESULT: return "float inexact result";
        case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
        case EXCEPTION_FLT_OVERFLOW: return "float overflow";
        case EXCEPTION_FLT_STACK_CHECK: return "float stack check";
        case EXCEPTION_FLT_UNDERFLOW: return "float underflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in page error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW: return "integer overflow";
        case EXCEPTION_INVALID_DISPOSITION: return "invalid disposition";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case 0xC0000374: return "heap corruption";
        case 0xC0000409: return "stack buffer overrun";
        case 0xE06D7363: return "C++ exception (MSVC)";
        default: return "unknown";
    }
}

// "reg2.exe+0x1234", or the bare address outside any module.
void Where(DWORD64 address, char* out, size_t size)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(address), &module) &&
        GetModuleFileNameW(module, path, MAX_PATH))
    {
        const wchar_t* name = wcsrchr(path, L'\\');
        char utf8[MAX_PATH * 3];
        if (!WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1, utf8, sizeof(utf8), NULL, NULL))
        {
            strcpy(utf8, "?");
        }
        snprintf(out, size, "%s+0x%llx", utf8, (unsigned long long)(address - reinterpret_cast<DWORD64>(module)));
        return;
    }
    snprintf(out, size, "0x%llx", (unsigned long long)address);
}

void LogRegisters(const CONTEXT& c)
{
#if defined(_M_X64) || defined(__x86_64__)
    Out("rax %016llx rbx %016llx rcx %016llx rdx %016llx", c.Rax, c.Rbx, c.Rcx, c.Rdx);
    Out("rsi %016llx rdi %016llx rbp %016llx rsp %016llx", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    Out("r8  %016llx r9  %016llx r10 %016llx r11 %016llx", c.R8, c.R9, c.R10, c.R11);
    Out("r12 %016llx r13 %016llx r14 %016llx r15 %016llx", c.R12, c.R13, c.R14, c.R15);
    Out("rip %016llx eflags %08lx", c.Rip, c.EFlags);
#elif defined(_M_IX86) || defined(__i386__)
    Out("eax %08lx ebx %08lx ecx %08lx edx %08lx", c.Eax, c.Ebx, c.Ecx, c.Edx);
    Out("esi %08lx edi %08lx ebp %08lx esp %08lx", c.Esi, c.Edi, c.Ebp, c.Esp);
    Out("eip %08lx eflags %08lx", c.Eip, c.EFlags);
#endif
}

// The unwind data of each frame's function (.pdata), from the faulting context.
void LogStack(const CONTEXT& start)
{
    Out("Stack:");
#if defined(_M_X64) || defined(__x86_64__)
    const NT_TIB* pTib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const DWORD64 low = reinterpret_cast<DWORD64>(pTib->StackLimit);
    const DWORD64 high = reinterpret_cast<DWORD64>(pTib->StackBase);
    CONTEXT c = start;
    for (int i = 0; i < 64; i++)
    {
        char where[MAX_PATH * 3 + 32];
        Where(c.Rip, where, sizeof(where));
        Out("  #%02d %s", i, where);
        if (c.Rsp < low || c.Rsp + 8 > high)
        {
            break;
        }
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION pFunction = c.Rip ? RtlLookupFunctionEntry(c.Rip, &imageBase, NULL) : NULL;
        if (pFunction)
        {
            void* pHandlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, pFunction, &c, &pHandlerData, &establisher, NULL);
        }
        else
        {
            // A leaf function, or a jump to a bad address: the return address is on top of the stack.
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
        if (!c.Rip)
        {
            break;
        }
    }
#else
    char where[MAX_PATH * 3 + 32];
    Where(start.Eip, where, sizeof(where));
    Out("  #00 %s (no stack walk in 32-bit builds)", where);
#endif
}

void LogModules()
{
    HMODULE modules[256];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
    {
        return;
    }
    Out("Modules:");
    const DWORD count = bytes / sizeof(HMODULE) < 256 ? bytes / sizeof(HMODULE) : 256;
    for (DWORD i = 0; i < count; i++)
    {
        wchar_t path[MAX_PATH];
        char utf8[MAX_PATH * 3];
        MODULEINFO info = {};
        if (GetModuleFileNameW(modules[i], path, MAX_PATH) &&
            WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL))
        {
            GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info));
            Out("  %p %8lx %s", info.lpBaseOfDll, info.SizeOfImage, utf8);
        }
    }
}

void Report(const char* summary, const CONTEXT* pContext, const EXCEPTION_RECORD* pRecord)
{
    const char* path = port_WriteLog();
    const Uint64 ms = SDL_GetTicks();
    Out("[%4u.%03u] CRASH: %s", (unsigned)(ms / 1000), (unsigned)(ms % 1000), summary);
    if (pRecord && pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2)
    {
        const ULONG_PTR kind = pRecord->ExceptionInformation[0];
        Out("%s of address 0x%llx", kind == 0 ? "Read" : kind == 1 ? "Write" : kind == 8 ? "Execution" : "Access",
            (unsigned long long)pRecord->ExceptionInformation[1]);
    }
    Out("Thread %lu", GetCurrentThreadId());
    if (pContext)
    {
        LogRegisters(*pContext);
        LogStack(*pContext);
    }
    LogModules();
    Out("End of the crash report");

    char text[2048];
    if (*path)
    {
        snprintf(text, sizeof(text), "reG2 crashed (%s).\n\nPlease send this file:\n%s", summary, path);
    }
    else
    {
        snprintf(text, sizeof(text), "reG2 crashed (%s).\n\nNo log file could be written.", summary);
    }
    if (!getenv("GTA2_NO_MESSAGEBOX"))
    {
        wchar_t wide[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 2048))
        {
            MessageBoxW(NULL, wide, L"reG2", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        }
    }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pInfo)
{
    if (InterlockedExchange(&sCrashing, 1))
    {
        // Another thread is reporting; the process ends with it.
        SleepEx(INFINITE, FALSE);
    }
    const EXCEPTION_RECORD* pRecord = pInfo->ExceptionRecord;
    char where[MAX_PATH * 3 + 32];
    Where(reinterpret_cast<DWORD64>(pRecord->ExceptionAddress), where, sizeof(where));
    char summary[MAX_PATH * 3 + 96];
    snprintf(summary, sizeof(summary), "exception 0x%08lx %s at %s", pRecord->ExceptionCode, ExceptionName(pRecord->ExceptionCode), where);
    Report(summary, pInfo->ContextRecord, pRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

void OnAbort(int)
{
    if (InterlockedExchange(&sCrashing, 1))
    {
        return;
    }
    CONTEXT context = {};
    RtlCaptureContext(&context);
    Report("abort() called", &context, NULL);
}
} // namespace

void port_InstallCrashHandler()
{
    // Room for the report after a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(OnUnhandledException);
    signal(SIGABRT, OnAbort);
}
