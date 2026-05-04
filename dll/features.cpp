// ════════════════════════════════════════════════════════════════════════════
// features.cpp  -  All concrete pipeline features.
//
//   Each feature is a static instance of a class derived from pipeline::Feature.
//   They self-register at static-init time (which is fine: they don't touch
//   IL2CPP until pipeline::InitAll() is called explicitly from the payload
//   thread, after the runtime is up).
//
//   Features in this file:
//     1. MiningHelperFeature   - visual + auto-press for the mining QTE
//     2. PlayerTweaksFeature   - movement / jump sliders for the local player
//     3. NodesEspFeature       - W2S markers for ResourceNodePrefabController
//     4. GenericEspFeature     - debug: type any class, draws all instances
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include "pipeline.h"
#include "renderer.h"
#include "il2cpp_helpers.h"
#include "game.h"
#include "log.h"
#include "vendor/imgui/imgui.h"

using namespace il2cpp_helpers;
using namespace il2cpp;

// ════════════════════════════════════════════════════════════════════════════
//  Local input helpers
// ════════════════════════════════════════════════════════════════════════════
static void SendKey(WORD vk) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
static void SendMouseLeft() {
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1] = in[0]; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

// ════════════════════════════════════════════════════════════════════════════
//  1. Mining helper
// ════════════════════════════════════════════════════════════════════════════
class MiningHelperFeature : public pipeline::Feature {
public:
    MiningHelperFeature()
        : Feature("Mining Helper", pipeline::Category::MiniGame, true) {}

    // ── Tunables ──
    bool  showVisual    = true;
    bool  autoPress     = false;
    float predictMs     = 60.0f;
    int   inputMode     = 0;          // 0 = LMB, 1 = 'E'    (proper int — fixed)

    // ── Counters ──
    int   crits = 0, hits = 0, misses = 0;

    // ── Cached IL2CPP refs ──
    Il2CppClass*       clsView    = nullptr;
    Il2CppClass*       clsModel   = nullptr;
    Il2CppClass*       clsRect    = nullptr;
    const MethodInfo*  mAnchored  = nullptr;

    // ── Sample state ──
    DWORD  lastTick   = 0;
    float  lastX      = 0.0f;
    float  velocity   = 0.0f;
    bool   pressedThisCycle = false;
    DWORD  lastResolveTick = 0;     // throttle FindFirstObjectOfType
    void*  cachedView  = nullptr;
    void*  cachedModel = nullptr;

    struct Sample { bool ok=false; float x=0, crit=0, hit=0, critZone=0, vel=0; };
    Sample latest;

    // Field offsets (from dump, Code.Core.dll)
    static constexpr size_t kView_HandleRect   = 0xB8;
    static constexpr size_t kModel_CritPoint   = 0x38;
    static constexpr size_t kModel_HitZone     = 0x3C;
    static constexpr size_t kModel_CritZone    = 0x40;

    void OnInit() override {
        clsView  = FindClass("World.MiniGame.UI", "UiViewMiniGameMining");
        clsModel = FindClass("World.MiniGame", "MiningGameDataModel");
        clsRect  = FindClass("UnityEngine", "RectTransform");
        if (clsRect) mAnchored = GetMethod(clsRect, "get_anchoredPosition", 0);
    }

    Sample Sample_() {
        Sample s{};
        if (!clsView || !clsModel || !mAnchored) return s;

        // Re-resolve view/model only every 500 ms; FindObjectsOfType is
        // extremely expensive (linear scan over every Unity Object).  We cache
        // even null results so the helper costs nothing while the mining UI
        // is closed.
        DWORD now = GetTickCount();
        if ((now - lastResolveTick) > 500) {
            cachedView  = FindFirstObjectOfType(clsView);
            cachedModel = FindFirstObjectOfType(clsModel);
            lastResolveTick = now;
        }
        void* view  = cachedView;
        void* model = cachedModel;
        if (!view || !model) return s;
        void* rect = *reinterpret_cast<void**>((char*)view + kView_HandleRect);
        if (!rect) return s;
        void* boxed = Invoke(mAnchored, rect, nullptr);
        if (!boxed) return s;
        auto& a = GetApi();
        void* raw = a.object_unbox ? a.object_unbox(boxed) : boxed;
        if (!raw) return s;

        s.x        = ((float*)raw)[0];
        s.crit     = *reinterpret_cast<float*>((char*)model + kModel_CritPoint);
        s.hit      = *reinterpret_cast<float*>((char*)model + kModel_HitZone);
        s.critZone = *reinterpret_cast<float*>((char*)model + kModel_CritZone);

        DWORD dt = now - lastTick;
        if (lastTick && dt > 0 && dt < 200) {
            velocity = (s.x - lastX) * 1000.0f / (float)dt;
        }
        lastTick = now;
        lastX    = s.x;
        s.vel    = velocity;
        s.ok     = true;
        return s;
    }

    void OnTick() override {
        latest = Sample_();
        if (!latest.ok) { pressedThisCycle = false; return; }

        float futureX     = latest.x + latest.vel * (predictMs / 1000.0f);
        float deltaToCrit = futureX - latest.crit;
        bool inCrit = std::fabs(deltaToCrit) <= latest.critZone;
        bool inHit  = std::fabs(deltaToCrit) <= latest.hit;

        if (autoPress && inCrit && !pressedThisCycle) {
            if (inputMode == 1) SendKey('E'); else SendMouseLeft();
            pressedThisCycle = true;
            crits++;
        }
        // Reset cycle gate once we're well past the hit zone.
        if (!inHit) pressedThisCycle = false;
    }

    void OnRenderWorld() override {
        if (!showVisual || !latest.ok) return;

        float futureX     = latest.x + latest.vel * (predictMs / 1000.0f);
        float deltaToCrit = futureX - latest.crit;
        bool inCrit = std::fabs(deltaToCrit) <= latest.critZone;
        bool inHit  = std::fabs(deltaToCrit) <= latest.hit;

        int vw = 0, vh = 0;
        renderer::GetViewportSize(vw, vh);
        if (vw <= 0) return;

        ImGui::SetNextWindowPos(ImVec2(vw * 0.5f - 200.0f, vh * 0.18f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_Always);
        ImGui::Begin("##MiningGuide", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs);

        ImVec4 col = inCrit ? ImVec4(1, 0.45f, 0, 1) :
                     inHit  ? ImVec4(1, 0.85f, 0, 1) :
                              ImVec4(0.7f, 0.7f, 0.7f, 1);
        ImGui::TextColored(col, inCrit ? "TAP NOW (CRIT)" :
                                inHit  ? "TAP (HIT)"     : "wait...");
        ImGui::Separator();

        const float halfRange = 250.0f;
        const float barW = 380.0f;
        auto* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 barTL  = ImVec2(origin.x, origin.y + 4);
        ImVec2 barBR  = ImVec2(origin.x + barW, origin.y + 22);
        dl->AddRectFilled(barTL, barBR, IM_COL32(40, 30, 25, 255));

        auto remap = [&](float v) { return barTL.x + ((v + halfRange) / (2 * halfRange)) * barW; };
        dl->AddRectFilled(ImVec2(remap(latest.crit - latest.hit), barTL.y),
                          ImVec2(remap(latest.crit + latest.hit), barBR.y),
                          IM_COL32(220, 170, 0, 200));
        dl->AddRectFilled(ImVec2(remap(latest.crit - latest.critZone), barTL.y),
                          ImVec2(remap(latest.crit + latest.critZone), barBR.y),
                          IM_COL32(255, 90, 0, 230));
        float cx = remap(latest.x);
        dl->AddLine(ImVec2(cx, barTL.y - 2), ImVec2(cx, barBR.y + 2), IM_COL32_WHITE, 2.0f);
        float px = remap(futureX);
        dl->AddLine(ImVec2(px, barTL.y - 2), ImVec2(px, barBR.y + 2),
                    IM_COL32(80, 220, 255, 255), 1.5f);

        ImGui::Dummy(ImVec2(barW, 26));
        ImGui::Text("h=%.1f  v=%.0fpx/s  d=%.1f  hit=%.1f  crit=%.1f",
                    latest.x, latest.vel, deltaToCrit, latest.hit, latest.critZone);
        ImGui::End();
    }

    void OnRenderUI() override {
        ImGui::Checkbox("Show visual guide", &showVisual);
        ImGui::Checkbox("Auto-press at crit", &autoPress);
        ImGui::SameLine(); ImGui::TextDisabled("(use at your own risk)");
        ImGui::SliderFloat("Latency comp (ms)", &predictMs, 0.0f, 250.0f, "%.0f");
        ImGui::Combo("Inject as", &inputMode, "Mouse Left\0E key\0\0");

        ImGui::Separator();
        ImGui::Text("Counters: crit=%d  hit=%d  miss=%d", crits, hits, misses);
        if (ImGui::Button("Reset counters")) crits = hits = misses = 0;

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Live sample")) {
            ImGui::Text("ok=%d  x=%.2f  v=%.1f", latest.ok, latest.x, latest.vel);
            ImGui::Text("crit=%.2f  hit=%.2f  critZone=%.2f",
                        latest.crit, latest.hit, latest.critZone);
            ImGui::Text("View=%p Model=%p", (void*)clsView, (void*)clsModel);
        }
    }
};
static MiningHelperFeature s_mining;

// ════════════════════════════════════════════════════════════════════════════
//  2. Player tweaks (movement / jump)
// ════════════════════════════════════════════════════════════════════════════
class PlayerTweaksFeature : public pipeline::Feature {
public:
    PlayerTweaksFeature()
        : Feature("Player Tweaks", pipeline::Category::Movement, true) {}

    game::SpeedTweak  t;
    Vec3              lastPos{};
    bool              havePos = false;

    void OnInit() override { game::Resolve(); }

    void OnTick() override {
        // Apply locked values every frame so the game can't overwrite them.
        game::ApplySpeedTweaks(t);
        Vec3 p; if (game::GetPlayerPosition(p)) { lastPos = p; havePos = true; }
    }

    void OnRenderUI() override {
        void* p = game::MainPlayer();
        const auto& r = game::GetRefs();
        ImGui::Text("Player ptr: %p", p);
        ImGui::TextDisabled("EM cls=%p  EM_get_Player=%p",
                            (void*)r.EntitiesManager, (const void*)r.EM_get_Player);
        void* em = game::EntitiesManagerInst();
        ImGui::TextDisabled("EM inst=%p", em);
        if (havePos)
            ImGui::Text("Pos: %.1f, %.1f, %.1f", lastPos.x, lastPos.y, lastPos.z);

        if (ImGui::Button("Snapshot from game")) {
            game::SpeedTweak snap{};
            if (game::ReadSpeedSnapshot(snap)) {
                t.baseSpeed = snap.baseSpeed;
                t.currentSpeed = snap.currentSpeed;
                t.modifier = snap.modifier;
                t.jumpHeight = snap.jumpHeight;
            }
        }
        ImGui::Separator();

        ImGui::Checkbox("Lock##b", &t.lockBase);    ImGui::SameLine();
        ImGui::SliderFloat("Base speed",    &t.baseSpeed,    0.0f, 30.0f, "%.2f");

        ImGui::Checkbox("Lock##c", &t.lockCurrent); ImGui::SameLine();
        ImGui::SliderFloat("Current speed", &t.currentSpeed, 0.0f, 30.0f, "%.2f");

        ImGui::Checkbox("Lock##m", &t.lockMod);     ImGui::SameLine();
        ImGui::SliderFloat("Modifier",      &t.modifier,     0.0f, 10.0f, "%.2f");

        ImGui::Checkbox("Lock##j", &t.lockJump);    ImGui::SameLine();
        ImGui::SliderFloat("Jump height",   &t.jumpHeight,   0.0f, 10.0f, "%.2f");

        ImGui::Separator();
        ImGui::TextWrapped("Note: Attack speed has no single field in CombatComponent. "
                           "It is driven by per-spell cooldowns (_spellCooldowns @ 0x1D8). "
                           "A reliable attack-speed slider needs a hook — not yet implemented.");
    }
};
static PlayerTweaksFeature s_player;

// ════════════════════════════════════════════════════════════════════════════
//  3. Resource Nodes ESP
// ════════════════════════════════════════════════════════════════════════════
class NodesEspFeature : public pipeline::Feature {
public:
    NodesEspFeature() : Feature("Nodes ESP", pipeline::Category::Visual, true) {}

    bool  showLabel    = true;
    bool  hideDead     = true;
    bool  showDistance = true;
    float maxDistance  = 200.0f;
    int   refreshHz    = 4;        // refresh enumeration N times/sec
    DWORD lastRefresh  = 0;

    std::vector<game::WorldEntity> nodes;

    void OnTick() override {
        DWORD now = GetTickCount();
        DWORD interval = (refreshHz > 0) ? (1000u / (DWORD)refreshHz) : 250u;
        if (now - lastRefresh < interval) return;
        lastRefresh = now;
        game::EnumerateNodes(nodes, 256);
    }

    void OnRenderWorld() override {
        void* cam = CameraMain();
        if (!cam) return;
        Vec3 myPos{}; bool haveMy = game::GetPlayerPosition(myPos);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (auto& n : nodes) {
            if (hideDead && n.isDead) continue;
            float dist = 0.0f;
            if (haveMy) {
                float dx = n.worldPos.x - myPos.x;
                float dy = n.worldPos.y - myPos.y;
                float dz = n.worldPos.z - myPos.z;
                dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > maxDistance) continue;
            }
            Vec2 sp;
            if (!WorldToScreen(cam, n.worldPos, sp)) continue;

            ImU32 col = ColorFor(n.miniGameType);
            dl->AddCircleFilled(ImVec2(sp.x, sp.y), 5.0f, col);
            dl->AddCircle(ImVec2(sp.x, sp.y), 5.0f, IM_COL32(0,0,0,200), 0, 1.0f);

            if (showLabel) {
                char buf[96];
                if (showDistance && haveMy)
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s  %.0fm", n.label, dist);
                else
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", n.label);
                dl->AddText(ImVec2(sp.x + 8, sp.y - 8), IM_COL32_WHITE, buf);
            }
        }
    }

    void OnRenderUI() override {
        ImGui::Checkbox("Show labels",   &showLabel);
        ImGui::Checkbox("Show distance", &showDistance);
        ImGui::Checkbox("Hide dead",     &hideDead);
        ImGui::SliderFloat("Max distance", &maxDistance, 10.0f, 500.0f, "%.0f m");
        ImGui::SliderInt("Refresh Hz",    &refreshHz,   1, 30);
        ImGui::Separator();
        ImGui::Text("Nodes tracked: %zu", nodes.size());
        if (ImGui::Button("Force refresh")) lastRefresh = 0;
        ImGui::Separator();
        ImGui::TextDisabled("Color legend:");
        Legend("Mining",      IM_COL32(255,140, 60,230));
        Legend("Woodcutting", IM_COL32(160,100, 50,230));
        Legend("Fishing",     IM_COL32( 80,180,255,230));
        Legend("Crafting",    IM_COL32(200,200,200,230));
        Legend("Cooking",     IM_COL32(255,200, 90,230));
        Legend("Alchemy",     IM_COL32(180,100,255,230));
    }

private:
    static ImU32 ColorFor(int type) {
        switch (type) {
            case 1: return IM_COL32(255,140, 60,230);  // Mining
            case 2: return IM_COL32(160,100, 50,230);  // Woodcutting
            case 3: return IM_COL32( 80,180,255,230);  // Fishing
            case 4: return IM_COL32(200,200,200,230);  // Crafting
            case 5: return IM_COL32(255,200, 90,230);  // Cooking
            case 6: return IM_COL32(180,100,255,230);  // Alchemy
        }
        return IM_COL32(120,255,120,230);
    }
    static void Legend(const char* name, ImU32 col) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x+12, p.y+12), col);
        ImGui::Dummy(ImVec2(14, 12));
        ImGui::SameLine(); ImGui::Text("%s", name);
    }
};
static NodesEspFeature s_nodes;

// ════════════════════════════════════════════════════════════════════════════
//  4. Generic ESP (debug — type any class)
// ════════════════════════════════════════════════════════════════════════════
class GenericEspFeature : public pipeline::Feature {
public:
    GenericEspFeature() : Feature("Generic ESP", pipeline::Category::Debug, false) {}

    char ns[64]   = "Entities";
    char name[64] = "Npc";
    int  maxDraw  = 64;
    int  refreshHz = 4;
    DWORD lastRefresh = 0;
    std::vector<game::WorldEntity> cached;
    Il2CppClass* lastCls = nullptr;
    char lastNs[64] = "";
    char lastName[64] = "";

    void OnTick() override {
        DWORD now = GetTickCount();
        DWORD interval = (refreshHz > 0) ? (1000u / (DWORD)refreshHz) : 250u;
        if (now - lastRefresh < interval) return;
        lastRefresh = now;

        // Re-resolve class only when name changed.
        if (strcmp(lastNs, ns) != 0 || strcmp(lastName, name) != 0) {
            strncpy_s(lastNs,   sizeof(lastNs),   ns,   _TRUNCATE);
            strncpy_s(lastName, sizeof(lastName), name, _TRUNCATE);
            lastCls = FindClass(ns, name);
        }
        if (!lastCls) { cached.clear(); return; }

        cached.clear();
        uint32_t n = 0;
        void* arr = FindObjectsOfType(lastCls, n);
        if (!arr) return;
        void** elems = reinterpret_cast<void**>((char*)arr + 0x20);
        int limit = (int)n < maxDraw ? (int)n : maxDraw;
        cached.reserve(limit);
        for (int i = 0; i < limit; i++) {
            game::WorldEntity we{};
            we.obj = elems[i];
            if (!game::GetTransformPosition(we.obj, we.worldPos)) continue;
            _snprintf_s(we.label, sizeof(we.label), _TRUNCATE, "%s#%d", name, i);
            cached.push_back(we);
        }
    }

    void OnRenderWorld() override {
        void* cam = CameraMain();
        if (!cam) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (auto& e : cached) {
            Vec2 sp;
            if (!WorldToScreen(cam, e.worldPos, sp)) continue;
            dl->AddCircleFilled(ImVec2(sp.x, sp.y), 4.0f, IM_COL32(0,255,120,220));
            dl->AddText(ImVec2(sp.x + 6, sp.y - 6), IM_COL32_WHITE, e.label);
        }
    }

    void OnRenderUI() override {
        ImGui::InputText("Namespace", ns,   sizeof(ns));
        ImGui::InputText("Type",      name, sizeof(name));
        ImGui::SliderInt("Max draw",  &maxDraw, 1, 512);
        ImGui::SliderInt("Refresh Hz", &refreshHz, 1, 30);
        ImGui::Text("Class ptr: %p", (void*)lastCls);
        ImGui::Text("Tracked: %zu", cached.size());
        if (ImGui::Button("Force refresh")) lastRefresh = 0;
    }
};
static GenericEspFeature s_genericEsp;

// ════════════════════════════════════════════════════════════════════════════
//  5. Console (live log viewer)
// ════════════════════════════════════════════════════════════════════════════
class ConsoleFeature : public pipeline::Feature {
public:
    ConsoleFeature() : Feature("Console", pipeline::Category::Debug, true) {}

    bool autoScroll = true;
    bool mirrorToConsole = true;
    char filter[64] = "";
    std::vector<logx::Entry> entries;

    void OnRenderUI() override {
        ImGui::Checkbox("Mirror to console", &mirrorToConsole);
        logx::SetConsoleMirror(mirrorToConsole);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &autoScroll);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) logx::Clear();
        ImGui::SameLine();
        if (ImGui::Button("Test")) {
            LOGI("info test");
            LOGW("warn test");
            LOGE("err  test");
            LOGD("dbg  test");
        }
        ImGui::InputText("Filter", filter, sizeof(filter));

        ImGui::Separator();
        logx::Snapshot(entries, 0);
        ImGui::BeginChild("##log", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (auto& e : entries) {
            if (filter[0] && !strstr(e.text, filter)) continue;
            ImVec4 col(1,1,1,1);
            const char* tag = "INF";
            switch (e.level) {
                case logx::Level::Info: col = ImVec4(0.85f,0.85f,0.85f,1); tag = "INF"; break;
                case logx::Level::Warn: col = ImVec4(1.00f,0.80f,0.30f,1); tag = "WRN"; break;
                case logx::Level::Err:  col = ImVec4(1.00f,0.40f,0.40f,1); tag = "ERR"; break;
                case logx::Level::Dbg:  col = ImVec4(0.45f,0.85f,1.00f,1); tag = "DBG"; break;
            }
            ImGui::TextColored(col, "[%8u] %s  %s", e.tickMs, tag, e.text);
        }
        if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
};
static ConsoleFeature s_console;

// ════════════════════════════════════════════════════════════════════════════
//  6. Performance overlay
// ════════════════════════════════════════════════════════════════════════════
class PerfFeature : public pipeline::Feature {
public:
    PerfFeature() : Feature("Performance", pipeline::Category::Debug, false) {}

    bool drawHud = true;

    void OnRenderWorld() override {
        if (!drawHud) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const auto& io = ImGui::GetIO();
        char buf[96];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "ScarsTool  %.0f FPS  (%.2f ms)  feats=%zu",
                    io.Framerate, 1000.0f / (io.Framerate > 1 ? io.Framerate : 1),
                    pipeline::All().size());
        dl->AddRectFilled(ImVec2(8, 8), ImVec2(8 + 360, 28), IM_COL32(0,0,0,140));
        dl->AddText(ImVec2(14, 11), IM_COL32(180,255,180,255), buf);
    }

    void OnRenderUI() override {
        ImGui::Checkbox("Draw HUD", &drawHud);
        ImGui::TextDisabled("Per-feature timings show in each feature's tab.");
    }
};
static PerfFeature s_perf;

