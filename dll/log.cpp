// ════════════════════════════════════════════════════════════════════════════
// log.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace logx {

static constexpr size_t kRingCap = 512;

static CRITICAL_SECTION g_cs;
static bool             g_inited = false;
static Entry            g_ring[kRingCap];
static size_t           g_head = 0;   // next write slot
static size_t           g_count = 0;
static bool             g_mirror = true;

void Init() {
    if (g_inited) return;
    InitializeCriticalSection(&g_cs);
    g_inited = true;
}

void Shutdown() {
    if (!g_inited) return;
    DeleteCriticalSection(&g_cs);
    g_inited = false;
}

void SetConsoleMirror(bool on) { g_mirror = on; }
bool ConsoleMirror()           { return g_mirror; }

static const char* LevelTag(Level l) {
    switch (l) {
        case Level::Info: return "\x1b[37mINFO\x1b[0m";
        case Level::Warn: return "\x1b[33mWARN\x1b[0m";
        case Level::Err:  return "\x1b[31m ERR\x1b[0m";
        case Level::Dbg:  return "\x1b[36m DBG\x1b[0m";
    }
    return "?";
}

void Write(Level lvl, const char* fmt, ...) {
    if (!g_inited) Init();

    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);

    DWORD now = GetTickCount();

    EnterCriticalSection(&g_cs);
    Entry& e = g_ring[g_head];
    e.tickMs = now;
    e.level  = lvl;
    strncpy_s(e.text, sizeof(e.text), buf, _TRUNCATE);
    g_head = (g_head + 1) % kRingCap;
    if (g_count < kRingCap) g_count++;
    bool mirror = g_mirror;
    LeaveCriticalSection(&g_cs);

    if (mirror) {
        SYSTEMTIME st; GetLocalTime(&st);
        printf("[%02u:%02u:%02u.%03u] %s  %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            LevelTag(lvl), buf);
    }
}

void Snapshot(std::vector<Entry>& out, size_t maxItems) {
    if (!g_inited) return;
    EnterCriticalSection(&g_cs);
    size_t n = g_count;
    if (maxItems && maxItems < n) n = maxItems;
    out.clear();
    out.reserve(n);
    // Walk oldest -> newest.
    size_t start = (g_count < kRingCap) ? 0 : g_head;
    if (maxItems && g_count > maxItems) start = (g_head + kRingCap - maxItems) % kRingCap;
    for (size_t i = 0; i < n; i++) {
        out.push_back(g_ring[(start + i) % kRingCap]);
    }
    LeaveCriticalSection(&g_cs);
}

void Clear() {
    if (!g_inited) return;
    EnterCriticalSection(&g_cs);
    g_head = 0; g_count = 0;
    LeaveCriticalSection(&g_cs);
}

} // namespace logx
