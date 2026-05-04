// ════════════════════════════════════════════════════════════════════════════
// log.h  -  Thread-safe logger.
//
//   * Writes to the payload console (with ANSI colors).
//   * Mirrors every line into a fixed ring buffer that the in-overlay
//     "Console" feature can render.
//   * Designed to be cheap and lock-light: one CRITICAL_SECTION around the
//     ring + one printf to the console.  Safe to call from any thread,
//     including the renderer's Present-hook thread.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace logx {

enum class Level : uint8_t { Info, Warn, Err, Dbg };

struct Entry {
    uint32_t  tickMs;
    Level     level;
    char      text[240];
};

void Init();
void Shutdown();

void Write(Level lvl, const char* fmt, ...);

// Snapshot the ring into `out` (newest last).  Cheap; runs under a CS.
void Snapshot(std::vector<Entry>& out, size_t maxItems = 0);
void Clear();

// Toggle live console output (ring buffer always fills).
void SetConsoleMirror(bool on);
bool ConsoleMirror();

} // namespace logx

#define LOGI(...) ::logx::Write(::logx::Level::Info, __VA_ARGS__)
#define LOGW(...) ::logx::Write(::logx::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::logx::Write(::logx::Level::Err,  __VA_ARGS__)
#define LOGD(...) ::logx::Write(::logx::Level::Dbg,  __VA_ARGS__)
