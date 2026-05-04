// ════════════════════════════════════════════════════════════════════════════
// pipeline.cpp
//
//   Lifecycle dispatcher that ALSO:
//     * Wraps every feature callback in a SEH __try/__except so that an
//       access-violation in one feature does NOT crash the whole game.
//     * Times every callback and logs a warning when one feature monopolises
//       the renderer thread (this used to cause visible lag).
//     * Auto-disables a feature after N consecutive crashes.
// ════════════════════════════════════════════════════════════════════════════
#include "pipeline.h"
#include "log.h"
#include "crash_guard.h"
#include "vendor/imgui/imgui.h"
#include <windows.h>
#include <unordered_map>

namespace pipeline {

const char* CategoryName(Category c) {
    switch (c) {
        case Category::Combat:   return "Combat";
        case Category::Movement: return "Movement";
        case Category::Visual:   return "Visual";
        case Category::MiniGame: return "Mini-Game";
        case Category::Debug:    return "Debug";
    }
    return "?";
}

static std::vector<Feature*>& Registry() {
    static std::vector<Feature*> g; return g;
}

// Per-feature stats (auto-disable, perf monitoring).
struct Stat {
    uint32_t crashes      = 0;
    uint32_t slowFrames   = 0;
    double   lastTickMs   = 0.0;
    double   peakTickMs   = 0.0;
    double   lastRenderMs = 0.0;
    double   peakRenderMs = 0.0;
    bool     killed       = false;
    // Adaptive backoff: when a feature's OnTick blows the per-frame budget
    // we postpone its next tick to keep the render thread responsive.
    DWORD    nextTickAt   = 0;     // GetTickCount() gate
    DWORD    backoffMs    = 0;     // current backoff window (0 = no backoff)
};
static std::unordered_map<Feature*, Stat>& Stats() {
    static std::unordered_map<Feature*, Stat> g; return g;
}

static constexpr uint32_t kCrashKillThreshold = 3;
static constexpr double   kSlowTickMsBudget   = 4.0;
static constexpr double   kSlowRenderMsBudget = 6.0;

// Maximum cumulative time we let TickAll() consume on a single render-thread
// frame.  Beyond this we defer the remaining features round-robin to the
// next frame so we never compound feature spikes into a visible hitch.
static constexpr double   kFrameTickBudgetMs  = 6.0;

// Adaptive backoff bounds for features whose individual OnTick blew the
// budget.  We double on every overrun and reset on a clean tick.
static constexpr DWORD    kBackoffMinMs       = 50;
static constexpr DWORD    kBackoffMaxMs       = 1000;

static double NowMs() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// SEH-guarded trampoline.  Plain function (no C++ unwind objects) so MSVC
// accepts __try/__except.  Returns SEH code, or 0 on success.
typedef void (*VoidThunk)(Feature*);
static unsigned long GuardedCall(VoidThunk thunk, Feature* f) {
    __try {
        thunk(f);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

static void DoInit(Feature* f)        { f->OnInit(); }
static void DoTick(Feature* f)        { f->OnTick(); }
static void DoRenderWorld(Feature* f) { f->OnRenderWorld(); }
static void DoRenderUI(Feature* f)    { f->OnRenderUI(); }
static void DoShutdown(Feature* f)    { f->OnShutdown(); }

Feature::Feature(const char* name, Category cat, bool enabledByDefault)
    : m_name(name ? name : "?"), m_cat(cat), m_enabled(enabledByDefault) {
    Register(this);
}

void Register(Feature* f) {
    if (f) Registry().push_back(f);
}

const std::vector<Feature*>& All() { return Registry(); }

static void RunGuarded(Feature* f, VoidThunk thunk, const char* phase,
                       double budgetMs, double& outMs) {
    Stat& st = Stats()[f];
    if (st.killed) return;

    double t0 = NowMs();
    unsigned long ec = GuardedCall(thunk, f);
    double dt = NowMs() - t0;
    outMs = dt;

    if (ec) {
        st.crashes++;
        crash_guard::NotifySwallowed(f->Name(), ec);
        LOGE("[%s] EXCEPTION 0x%08lX in %s (count=%u)",
             f->Name(), ec, phase, st.crashes);
        if (st.crashes >= kCrashKillThreshold) {
            st.killed = true;
            f->SetEnabled(false);
            LOGE("[%s] auto-disabled after %u crashes", f->Name(), st.crashes);
        }
        return;
    }

    if (dt > budgetMs) {
        st.slowFrames++;
        // Throttle the warn log so it doesn't itself become spam.
        if ((st.slowFrames & 0x3F) == 1) {
            LOGW("[%s] %s slow: %.2f ms (budget %.1f, slow=%u)",
                 f->Name(), phase, dt, budgetMs, st.slowFrames);
        }
    }
}

void InitAll() {
    LOGI("pipeline: initialising %zu feature(s)", Registry().size());
    for (auto* f : Registry()) {
        double ms = 0;
        RunGuarded(f, &DoInit, "OnInit", 50.0, ms);
        LOGI("  + %-22s [%s]  init=%.2f ms",
             f->Name(), CategoryName(f->Cat()), ms);
    }
}

void TickAll() {
    // Round-robin start index so a heavy feature near the front of the list
    // can't permanently starve features behind it when we hit the per-frame
    // budget cap.
    static size_t s_rrCursor = 0;
    const auto& reg = Registry();
    if (reg.empty()) return;

    DWORD now = GetTickCount();
    double frameStart = NowMs();
    size_t n = reg.size();
    size_t startIdx = s_rrCursor % n;

    for (size_t i = 0; i < n; i++) {
        size_t idx = (startIdx + i) % n;
        Feature* f = reg[idx];
        if (!f->Enabled()) continue;
        Stat& st = Stats()[f];
        if (st.killed) continue;

        // Frame budget: if we've already burned through the per-frame ms
        // budget, defer the rest to next frame starting from this index.
        double spent = NowMs() - frameStart;
        if (spent > kFrameTickBudgetMs) {
            s_rrCursor = idx;          // resume here next frame
            return;
        }

        // Per-feature backoff window after a blown OnTick.
        if (st.nextTickAt && now < st.nextTickAt) continue;

        RunGuarded(f, &DoTick, "OnTick", kSlowTickMsBudget, st.lastTickMs);
        if (st.lastTickMs > st.peakTickMs) st.peakTickMs = st.lastTickMs;

        // Adapt: blown budget -> grow backoff window (1.5x), clean -> reset.
        if (st.lastTickMs > kSlowTickMsBudget) {
            DWORD prev = st.backoffMs ? st.backoffMs : kBackoffMinMs;
            DWORD next = prev + (prev >> 1);   // *1.5
            if (next > kBackoffMaxMs) next = kBackoffMaxMs;
            st.backoffMs  = next;
            st.nextTickAt = now + next;
        } else {
            st.backoffMs  = 0;
            st.nextTickAt = 0;
        }
    }
    // Full sweep completed -- advance cursor so next frame starts further on.
    s_rrCursor = (startIdx + 1) % n;
}

void RenderWorldAll() {
    for (auto* f : Registry()) {
        if (!f->Enabled()) continue;
        Stat& st = Stats()[f];
        RunGuarded(f, &DoRenderWorld, "OnRenderWorld",
                   kSlowRenderMsBudget, st.lastRenderMs);
        if (st.lastRenderMs > st.peakRenderMs) st.peakRenderMs = st.lastRenderMs;
    }
}

void RenderUITab(Feature* f) {
    if (!f) return;
    ImGui::Checkbox("Enabled", f->EnabledPtr());
    Stat& st = Stats()[f];
    if (st.killed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),
            "  AUTO-DISABLED (crashes=%u)", st.crashes);
        if (ImGui::SmallButton("Reset")) { st.killed = false; st.crashes = 0; }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  tick %.2f ms (peak %.2f) | render %.2f ms (peak %.2f)",
        st.lastTickMs, st.peakTickMs, st.lastRenderMs, st.peakRenderMs);
    ImGui::Separator();
    double ms = 0;
    RunGuarded(f, &DoRenderUI, "OnRenderUI", 8.0, ms);
}

void ShutdownAll() {
    for (auto* f : Registry()) {
        double ms = 0;
        RunGuarded(f, &DoShutdown, "OnShutdown", 50.0, ms);
    }
    Stats().clear();
}

} // namespace pipeline
