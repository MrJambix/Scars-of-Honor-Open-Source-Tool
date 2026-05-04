// ════════════════════════════════════════════════════════════════════════════
// crash_guard.h  -  Process-wide safety net.
//
//   * Installs a Vectored Exception Handler that catches first-chance
//     hardware exceptions (AV, illegal-insn, div0, stack-overflow, ...)
//     whose faulting IP lies inside ScarsTool.dll / imgui code.  Each is
//     logged in detail (code, address, module, thread) so a feature that
//     silently __try/__except'd no longer disappears off the log.
//
//   * Installs an UnhandledExceptionFilter as a last line of defence so
//     a fault that escaped every SEH layer is still logged before the OS
//     terminates the host (and the user gets an actual diagnostic instead
//     of a silent CTD).
//
//   * Provides a re-entrant SafeCall<T>() helper for any code path outside
//     the pipeline framework that wants the same per-call SEH treatment.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <windows.h>

namespace crash_guard {

// Install both VEH + UnhandledExceptionFilter.  Call once at startup, after
// the log subsystem is initialised.  Idempotent.
void Install();

// Remove our handlers.  Call from DLL_PROCESS_DETACH.
void Uninstall();

// Per-thread guard counter — when >0 the VEH suppresses repeat logging for
// "expected" probing exceptions (used by the IL2CPP/Vector2 readers that
// deliberately generate AVs while sniffing memory).
struct QuietScope {
    QuietScope();
    ~QuietScope();
};

// Raw counter primitives for code paths that use __try (and therefore can't
// host C++ objects with destructors).  Always pair Begin/End.
void QuietBegin();
void QuietEnd();

// Convenience wrapper: run the lambda inside __try/__except, log anything
// that fires, and return false on exception.  Templated so it inlines cleanly.
template <class Fn>
inline bool SafeCall(const char* tag, Fn&& fn) {
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        extern void NotifySwallowed(const char* tag, unsigned long code);
        NotifySwallowed(tag, GetExceptionCode());
        return false;
    }
}

// Used by the SEH wrappers in other TUs (renderer, pipeline, ...) to push a
// formatted "exception swallowed" line into the log without pulling in the
// whole crash_guard module's includes.
void NotifySwallowed(const char* tag, unsigned long code);

} // namespace crash_guard
