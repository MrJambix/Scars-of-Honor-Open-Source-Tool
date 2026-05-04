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
};
static std::unordered_map<Feature*, Stat>& Stats() {
    static std::unordered_map<Feature*, Stat> g; return g;
}

static constexpr uint32_t kCrashKillThreshold = 3;
static constexpr double   kSlowTickMsBudget   = 4.0;
static constexpr double   kSlowRenderMsBudget = 6.0;

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
    for (auto* f : Registry()) {
        if (!f->Enabled()) continue;
        Stat& st = Stats()[f];
        RunGuarded(f, &DoTick, "OnTick", kSlowTickMsBudget, st.lastTickMs);
        if (st.lastTickMs > st.peakTickMs) st.peakTickMs = st.lastTickMs;
    }
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
