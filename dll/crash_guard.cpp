// ════════════════════════════════════════════════════════════════════════════
// crash_guard.cpp  -  see header.
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>

#include "crash_guard.h"
#include "log.h"

#pragma comment(lib, "psapi.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace crash_guard {

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────
static PVOID                          g_veh   = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER   g_prevTop = nullptr;
static std::atomic<bool>              g_installed { false };
static thread_local int               t_quietDepth = 0;

// Cached "is this RIP inside our DLL?" range.
static uintptr_t g_selfBase = 0;
static uintptr_t g_selfEnd  = 0;

// Throttling: identical fault (same code+address) within this window is
// counted but only emits one log line, so a feature in a tight loop can't
// flood the console.
struct LastFault {
    DWORD       code   = 0;
    uintptr_t   addr   = 0;
    DWORD       stamp  = 0;
    uint32_t    count  = 0;
};
static LastFault g_last;
static CRITICAL_SECTION g_lock;
static std::atomic<bool> g_lockInit { false };

// ─────────────────────────────────────────────────────────────────────────────
static void EnsureLock() {
    bool expected = false;
    if (g_lockInit.compare_exchange_strong(expected, true)) {
        InitializeCriticalSection(&g_lock);
    }
}

static void CacheSelfRange() {
    HMODULE self = reinterpret_cast<HMODULE>(&__ImageBase);
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
        g_selfBase = (uintptr_t)mi.lpBaseOfDll;
        g_selfEnd  = g_selfBase + mi.SizeOfImage;
    }
}

static const char* CodeName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:        return "ACCESS_VIOLATION";
        case EXCEPTION_DATATYPE_MISALIGNMENT:   return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT:              return "BREAKPOINT";
        case EXCEPTION_SINGLE_STEP:             return "SINGLE_STEP";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DENORMAL_OPERAND:    return "FLT_DENORMAL";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "FLT_DIV0";
        case EXCEPTION_FLT_INEXACT_RESULT:      return "FLT_INEXACT";
        case EXCEPTION_FLT_INVALID_OPERATION:   return "FLT_INVALID";
        case EXCEPTION_FLT_OVERFLOW:            return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:         return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:           return "FLT_UNDERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "INT_DIV0";
        case EXCEPTION_INT_OVERFLOW:            return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:        return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:           return "IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:return "NONCONTINUABLE";
        case EXCEPTION_STACK_OVERFLOW:          return "STACK_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:     return "INVALID_DISPOSITION";
        case EXCEPTION_GUARD_PAGE:              return "GUARD_PAGE";
        case EXCEPTION_INVALID_HANDLE:          return "INVALID_HANDLE";
        case 0xE06D7363:                        return "C++_EXCEPTION";
        case 0x406D1388:                        return "DBG_THREAD_NAME";
        default:                                return "UNKNOWN";
    }
}

// True if this code is one of the "hardware faults" that would actually
// crash the host process if left to propagate.  We deliberately ignore
// debugger-generated codes (BREAKPOINT, SINGLE_STEP, DBG_THREAD_NAME) and
// the various OutputDebugString / DBG_PRINT codes the game raises every
// frame for telemetry.
static bool IsFatalCode(DWORD c) {
    switch (c) {
        case EXCEPTION_BREAKPOINT:
        case EXCEPTION_SINGLE_STEP:
        case 0x406D1388:   // MS_VC_EXCEPTION   (SetThreadName)
        case 0x40010005:   // DBG_CONTROL_C
        case 0x40010006:   // DBG_PRINTEXCEPTION_C    (OutputDebugStringA)
        case 0x4001000A:   // DBG_PRINTEXCEPTION_C    (alt)
        case 0x4001000E:   // DBG_PRINTEXCEPTION_WIDE_C (OutputDebugStringW)
        case 0x6BA:        // RPC_S_SERVER_UNAVAILABLE noise from grpc
            return false;
        default:
            return true;
    }
}

// True if the faulting IP looks like ours (ScarsTool.dll body).  We use this
// to decide whether to name & shame the fault loudly — faults inside game
// code are still logged but at INFO level so we don't spam during normal
// IL2CPP runtime warts.
static bool IpIsMine(uintptr_t ip) {
    return g_selfBase && ip >= g_selfBase && ip < g_selfEnd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Logging helpers
// ─────────────────────────────────────────────────────────────────────────────
static void EmitFault(const EXCEPTION_RECORD* er, const CONTEXT* ctx, bool unhandled) {
    DWORD     code = er->ExceptionCode;
    uintptr_t ip   = (uintptr_t)er->ExceptionAddress;
    uintptr_t op0  = er->NumberParameters >= 2 ? (uintptr_t)er->ExceptionInformation[1] : 0;
    DWORD     rw   = er->NumberParameters >= 1 ? (DWORD)er->ExceptionInformation[0]    : 0;

    EnsureLock();
    EnterCriticalSection(&g_lock);
    DWORD now = GetTickCount();
    bool repeat = (g_last.code == code && g_last.addr == ip && (now - g_last.stamp) < 2000);
    if (repeat) {
        g_last.count++;
        g_last.stamp = now;
        LeaveCriticalSection(&g_lock);
        return; // throttle: same fault, same site, within 2s
    }
    uint32_t prevCount = g_last.count;
    g_last.code  = code;
    g_last.addr  = ip;
    g_last.stamp = now;
    g_last.count = 1;
    LeaveCriticalSection(&g_lock);

    if (prevCount > 1) {
        LOGW("[guard] previous fault repeated %u additional time(s)", prevCount - 1);
    }

    char modName[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ip, &mod) && mod) {
        char full[MAX_PATH] = {};
        GetModuleFileNameA(mod, full, MAX_PATH);
        const char* slash = strrchr(full, '\\');
        strncpy_s(modName, slash ? slash + 1 : full, _TRUNCATE);
    }

    const char* cn = CodeName(code);
    bool mine = IpIsMine(ip);
    const char* level = unhandled ? "UNHANDLED" :
                        mine      ? "OURS"      : "GAME";

    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) {
        const char* op = (rw == 0) ? "read"
                       : (rw == 1) ? "write"
                       : (rw == 8) ? "DEP" : "access";
        LOGE("[guard] %s %s @ %p in %s  -> %s 0x%p   tid=%u",
             level, cn, (void*)ip, modName, op, (void*)op0,
             GetCurrentThreadId());
    } else {
        LOGE("[guard] %s %s (0x%08lX) @ %p in %s   tid=%u",
             level, cn, code, (void*)ip, modName,
             GetCurrentThreadId());
    }

#if defined(_M_X64)
    if (ctx && unhandled) {
        LOGE("[guard]   RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
             ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
        LOGE("[guard]   RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX",
             ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
        LOGE("[guard]   R8 =%016llX R9 =%016llX R10=%016llX R11=%016llX",
             ctx->R8,  ctx->R9,  ctx->R10, ctx->R11);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// VEH — runs FIRST-chance, before any SEH handler.  Our job is to log faults
// originating in our code.  We always return CONTINUE_SEARCH so the existing
// __try/__except in pipeline.cpp / renderer.cpp / il2cpp_helpers.cpp still
// gets to swallow the exception as designed.
// ─────────────────────────────────────────────────────────────────────────────
static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!IsFatalCode(code))                return EXCEPTION_CONTINUE_SEARCH;
    if (t_quietDepth > 0)                  return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    bool mine = IpIsMine(ip);

    // Game-side first-chance noise filter.  Unity / Mono / IL2CPP / Sentry
    // routinely raise hardware AVs and C++ throws as part of normal control
    // flow (null-deref -> NullReferenceException, job-system probes, gRPC
    // unavailability, etc.) and catch them themselves a few frames upstack.
    // Logging them just spams the console and makes real ScarsTool faults
    // impossible to spot.  Only let game-side faults through to the log
    // when the UnhandledExceptionFilter eventually catches them.
    if (!mine) return EXCEPTION_CONTINUE_SEARCH;

    // Never run our log path while another exception is in flight on this
    // thread (the log/criticalsection itself could fault).
    static thread_local int reentry = 0;
    if (reentry > 0) return EXCEPTION_CONTINUE_SEARCH;
    ++reentry;
    EmitFault(ep->ExceptionRecord, ep->ContextRecord, /*unhandled=*/false);
    --reentry;

    return EXCEPTION_CONTINUE_SEARCH;
}

// ─────────────────────────────────────────────────────────────────────────────
// UnhandledExceptionFilter — last line of defence.  If we get here the fault
// was NOT caught by any SEH handler upstack; the host is about to die.  Our
// ONLY job is to log it -- we deliberately do NOT terminate the thread or
// the process ourselves.  Returning EXECUTE_HANDLER would kill the offending
// thread (which, if it was a Unity worker / render thread, freezes the game
// instantly).  Returning CONTINUE_SEARCH after the previous filter lets the
// game's own crash reporter (Sentry) and finally Windows Error Reporting
// handle termination cleanly.
// ─────────────────────────────────────────────────────────────────────────────
static LONG WINAPI TopLevel(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && IsFatalCode(ep->ExceptionRecord->ExceptionCode)) {
        // Best-effort log; protect against re-entry / log subsystem AV.
        static thread_local int reentry = 0;
        if (reentry == 0) {
            ++reentry;
            __try {
                EmitFault(ep->ExceptionRecord, ep->ContextRecord, /*unhandled=*/true);
                LOGE("[guard] *** UNHANDLED EXCEPTION -- letting host crash handler take over ***");
            } __except (EXCEPTION_EXECUTE_HANDLER) { /* nothing we can do */ }
            --reentry;
        }
    }
    // Chain to the previous filter (game's own crash reporter, e.g. Sentry).
    // Whatever it decides, we propagate -- we never override with our own
    // EXECUTE_HANDLER, which would terminate the thread mid-fault.
    if (g_prevTop) return g_prevTop(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
void Install() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;
    EnsureLock();
    CacheSelfRange();
    g_veh     = AddVectoredExceptionHandler(/*first*/1, &Veh);
    g_prevTop = SetUnhandledExceptionFilter(&TopLevel);
    LOGI("[guard] crash guard installed (self=%p..%p)",
         (void*)g_selfBase, (void*)g_selfEnd);
}

void Uninstall() {
    bool expected = true;
    if (!g_installed.compare_exchange_strong(expected, false)) return;
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    SetUnhandledExceptionFilter(g_prevTop);
    g_prevTop = nullptr;
}

QuietScope::QuietScope()  { ++t_quietDepth; }
QuietScope::~QuietScope() { --t_quietDepth; }
void QuietBegin() { ++t_quietDepth; }
void QuietEnd()   { --t_quietDepth; }

void NotifySwallowed(const char* tag, unsigned long code) {
    // VEH already logged the fault if it was hardware-fatal.  Add a short
    // breadcrumb naming WHICH SEH wrapper swallowed it so the user can map
    // a fault to a feature/hook even when the throttle skipped the VEH log.
    LOGW("[guard] swallowed %s in %s (tid=%u)",
         CodeName((DWORD)code), tag ? tag : "?", GetCurrentThreadId());
}

} // namespace crash_guard
