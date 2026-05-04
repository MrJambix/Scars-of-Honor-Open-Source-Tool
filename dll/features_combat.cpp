// ════════════════════════════════════════════════════════════════════════════
// features_combat.cpp  -  Combat / character pipelines using game_api.
//
//   * StatsHud        - live HP/Mana/Stats readout for the local player
//   * TeleportTool    - read & write Transform.position on the local player
//   * PlayerEsp       - 2D world-space name overlay for every Player+BotPlayer
//   * NoClip          - placeholder shell with diagnostic
//   * AutoLoot        - placeholder shell with diagnostic
//
//   The placeholders are wired into the pipeline so the user can see them in
//   the overlay; they print why they can't safely act yet (and the inspector
//   tab / patches tab give the symbol names that still need verification).
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <vector>

#include "pipeline.h"
#include "game.h"
#include "game_api.h"
#include "il2cpp_helpers.h"
#include "log.h"
#include "vendor/imgui/imgui.h"

using namespace il2cpp_helpers;

namespace {

// ── Helpers ───────────────────────────────────────────────────────────────
// Call a 0-arg method that returns System.Single
static bool CallSingleNoArg(void* obj, const gameapi::MethodSym& m, float& out) {
    if (!obj || !m.ptr || !m.mi) return false;
    __try {
        using Fn = float(*)(void*, const MethodInfo*);
        out = ((Fn)m.ptr)(obj, m.mi);
        return _finite(out) ? true : false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Call a 1-arg method (byte enum) returning Single
static bool CallSingleByteArg(void* obj, const gameapi::MethodSym& m, uint8_t arg, float& out) {
    if (!obj || !m.ptr || !m.mi) return false;
    __try {
        using Fn = float(*)(void*, uint8_t, const MethodInfo*);
        out = ((Fn)m.ptr)(obj, arg, m.mi);
        return _finite(out) ? true : false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Call a 0-arg method returning bool
static bool CallBoolNoArg(void* obj, const gameapi::MethodSym& m, bool& out) {
    if (!obj || !m.ptr || !m.mi) return false;
    __try {
        using Fn = bool(*)(void*, const MethodInfo*);
        out = ((Fn)m.ptr)(obj, m.mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Call a 0-arg method returning a pointer
static void* CallPtrNoArg(void* obj, const gameapi::MethodSym& m) {
    if (!obj || !m.ptr || !m.mi) return nullptr;
    __try {
        using Fn = void*(*)(void*, const MethodInfo*);
        return ((Fn)m.ptr)(obj, m.mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// Call Transform.set_position(Vector3 by-value) on a transform.  Vector3 is
// 12 bytes and on Win64 IL2CPP passes it as a pointer (struct >8b).
static bool CallSetPosition(void* transform, const gameapi::MethodSym& m, const Vec3& v) {
    if (!transform || !m.ptr || !m.mi) return false;
    __try {
        Vec3 copy = v;
        using Fn = void(*)(void*, Vec3*, const MethodInfo*);
        ((Fn)m.ptr)(transform, &copy, m.mi);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Defines.Stat enum order from dump.txt (sequential values, byte-typed).
// 0=None, 1=Vitality, 2=Spirit, 3=Endurance, 4=Mind, 5=Strength, 6=Dexterity,
// 7=Intelligence, 8=Armor, 9=Armor_Penetration, 10=Magic_Defence,
// 11=Magic_Penetration, 12=Crit_Chance, 13=Crit_Hit_Damage,
// 14=Health_Regeneration, 15=Healing_Increase, 16=Haste,
// 17=Cooldown_Reduction, 18=Movement_Speed, 19=Weapon_Damage_Min,
// 20=Weapon_Damage_Max, 21=Fire_Resistance, 22=Fire_Penetration,
// 23=Frost_Resistance, 24=Frost_Penetration, 25=Healing_Reduction,
// 26=Barrier, 27=Mana_Regeneration, 28=Energy_Regeneration,
// 29=Flat_Health, 30=Flat_Mana, 31=Flat_Energy.
struct StatRow { uint8_t id; const char* label; };
static const StatRow kStats[] = {
    { 29, "Flat Health" }, { 30, "Flat Mana" }, { 31, "Flat Energy" },
    { 1,  "Vitality"    }, { 2,  "Spirit"   }, { 3,  "Endurance" },
    { 4,  "Mind"        }, { 5,  "Strength" }, { 6,  "Dexterity" },
    { 7,  "Intelligence"},
    { 8,  "Armor"       }, { 10, "Magic Defence" }, { 26, "Barrier" },
    { 12, "Crit Chance" }, { 13, "Crit Damage"   }, { 16, "Haste" },
    { 17, "Cooldown Red"}, { 18, "Move Speed"    },
    { 19, "Weapon Min"  }, { 20, "Weapon Max"    },
    { 14, "Health Regen"}, { 27, "Mana Regen"    }, { 28, "Energy Regen" },
    { 15, "Healing Inc" }, { 25, "Healing Red"   },
    { 21, "Fire Res"    }, { 22, "Fire Pen"      },
    { 23, "Frost Res"   }, { 24, "Frost Pen"     },
};

// Refresh-throttled cache keyed by (player ptr, stat id).
struct StatCache {
    void*        owner   = nullptr;
    DWORD        nextMs  = 0;
    float        values[64] = {};
    bool         valid[64]  = {};
    bool         alive  = false;
    bool         inCombat = false;
};
static StatCache g_statCache;

static void RefreshStats(void* player) {
    DWORD now = GetTickCount();
    if (player == g_statCache.owner && now < g_statCache.nextMs) return;
    g_statCache = {};
    g_statCache.owner   = player;
    g_statCache.nextMs  = now + 250; // 4 Hz is plenty
    if (!player) return;
    const auto& A = gameapi::G();
    bool b = false;
    if (CallBoolNoArg(player, A.Unit_IsAlive, b)) g_statCache.alive = b;
    if (CallBoolNoArg(player, A.Unit_get_IsInCombat, b)) g_statCache.inCombat = b;
    for (auto& s : kStats) {
        if (s.id >= 64) continue;
        float v = 0.0f;
        if (CallSingleByteArg(player, A.Unit_GetStatAmount, s.id, v)) {
            g_statCache.values[s.id] = v;
            g_statCache.valid [s.id] = true;
        }
    }
}

} // anonymous

// ════════════════════════════════════════════════════════════════════════════
// 1. Stats HUD
// ════════════════════════════════════════════════════════════════════════════
class StatsHudFeature : public pipeline::Feature {
public:
    StatsHudFeature() : Feature("Stats", pipeline::Category::Combat, true) {}

    void OnRenderUI() override {
        const auto& A = gameapi::G();
        if (!A.Unit_GetStatAmount.ptr) {
            ImGui::TextColored(ImVec4(1,0.5f,0.5f,1),
                "Unit.GetStatAmount unresolved -- check the Game API tab.");
            return;
        }
        void* p = game::MainPlayer();
        ImGui::Text("Local player ptr = %p", p);
        if (!p) { ImGui::TextDisabled("No local player yet."); return; }

        RefreshStats(p);
        ImGui::Text("Alive: %s   InCombat: %s",
            g_statCache.alive ? "yes" : "NO",
            g_statCache.inCombat ? "YES" : "no");
        ImGui::Separator();

        if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Stat");
            ImGui::TableSetupColumn("Value");
            ImGui::TableHeadersRow();
            for (auto& s : kStats) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.label);
                ImGui::TableNextColumn();
                if (g_statCache.valid[s.id])
                    ImGui::Text("%.2f", g_statCache.values[s.id]);
                else
                    ImGui::TextDisabled("--");
            }
            ImGui::EndTable();
        }
    }
};
static StatsHudFeature s_statsHud;

// ════════════════════════════════════════════════════════════════════════════
// 2. Teleport tool
// ════════════════════════════════════════════════════════════════════════════
class TeleportFeature : public pipeline::Feature {
public:
    TeleportFeature() : Feature("Teleport", pipeline::Category::Movement, true) {}

    Vec3 m_target { 0,0,0 };
    Vec3 m_saved  { 0,0,0 };
    bool m_haveSaved = false;
    char m_status[128] = "ready";

    void OnRenderUI() override {
        const auto& A = gameapi::G();
        if (!A.Component_get_transform.ptr || !A.Transform_get_position.ptr || !A.Transform_set_position.ptr) {
            ImGui::TextColored(ImVec4(1,0.5f,0.5f,1),
                "UnityEngine Transform get/set unresolved -- Game API tab.");
            return;
        }
        void* p = game::MainPlayer();
        if (!p) { ImGui::TextDisabled("No local player yet."); return; }

        Vec3 pos{};
        bool gotPos = game::GetPlayerPosition(pos);
        if (gotPos) {
            ImGui::Text("Current : %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
        } else {
            ImGui::TextDisabled("Position unavailable.");
        }

        ImGui::DragFloat3("Target", &m_target.x, 0.5f);
        if (ImGui::Button("Copy current -> target") && gotPos) m_target = pos;
        ImGui::SameLine();
        if (ImGui::Button("Save current") && gotPos) { m_saved = pos; m_haveSaved = true; }
        ImGui::SameLine();
        if (ImGui::Button("Restore saved") && m_haveSaved) Teleport(m_saved);

        ImGui::Separator();
        if (ImGui::Button("Teleport now")) Teleport(m_target);
        ImGui::SameLine();
        if (ImGui::Button("Up +5"))  { Vec3 t=pos; t.y += 5;  Teleport(t); }
        ImGui::SameLine();
        if (ImGui::Button("Up +25")) { Vec3 t=pos; t.y += 25; Teleport(t); }
        ImGui::SameLine();
        if (ImGui::Button("Down -5")){ Vec3 t=pos; t.y -= 5;  Teleport(t); }

        ImGui::TextDisabled("Status: %s", m_status);
        ImGui::TextWrapped("Note: server may snap you back if it disagrees with "
                           "the new position (typical for short-distance "
                           "teleports server doesn't auth-check).");
    }

private:
    void Teleport(const Vec3& v) {
        const auto& A = gameapi::G();
        void* p = game::MainPlayer();
        if (!p) { _snprintf_s(m_status, _TRUNCATE, "no player"); return; }
        // Validate target
        if (!_finite(v.x) || !_finite(v.y) || !_finite(v.z)) {
            _snprintf_s(m_status, _TRUNCATE, "rejected: NaN/Inf"); return;
        }
        if (fabsf(v.x) > 50000 || fabsf(v.y) > 50000 || fabsf(v.z) > 50000) {
            _snprintf_s(m_status, _TRUNCATE, "rejected: out-of-range"); return;
        }
        void* tr = CallPtrNoArg(p, A.Component_get_transform);
        if (!tr) { _snprintf_s(m_status, _TRUNCATE, "no transform"); return; }
        if (CallSetPosition(tr, A.Transform_set_position, v)) {
            _snprintf_s(m_status, _TRUNCATE,
                "teleported to %.1f,%.1f,%.1f", v.x, v.y, v.z);
            LOGI("teleport -> %.2f,%.2f,%.2f", v.x, v.y, v.z);
        } else {
            _snprintf_s(m_status, _TRUNCATE, "set_position threw");
        }
    }
};
static TeleportFeature s_teleport;

// ════════════════════════════════════════════════════════════════════════════
// 3. Player ESP (names + distance)
// ════════════════════════════════════════════════════════════════════════════
class PlayerEspFeature : public pipeline::Feature {
public:
    PlayerEspFeature() : Feature("Player ESP", pipeline::Category::Visual, false) {}

    bool   m_enabled       = true;
    bool   m_drawDistance  = true;
    float  m_maxDistance   = 250.0f;
    DWORD  m_nextRefreshMs = 0;
    std::vector<game::WorldEntity> m_cache;

    void OnTick() override {
        if (!m_enabled) return;
        DWORD now = GetTickCount();
        if (now < m_nextRefreshMs) return;
        m_nextRefreshMs = now + 500; // 2 Hz
        m_cache.clear();
        game::EnumeratePlayers(m_cache, 64);
    }

    void OnRenderWorld() override {
        if (!m_enabled) return;
        void* cam = il2cpp_helpers::CameraMain();
        if (!cam) return;
        Vec3 me{};
        bool haveMe = game::GetPlayerPosition(me);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (auto& e : m_cache) {
            if (haveMe) {
                float dx = e.worldPos.x - me.x, dy = e.worldPos.y - me.y, dz = e.worldPos.z - me.z;
                float d  = sqrtf(dx*dx + dy*dy + dz*dz);
                if (d > m_maxDistance) continue;
                Vec2 sp;
                if (!il2cpp_helpers::WorldToScreen(cam, e.worldPos, sp)) continue;
                char buf[96];
                if (m_drawDistance)
                    _snprintf_s(buf, _TRUNCATE, "%s  %.0fm", e.label[0]?e.label:"player", d);
                else
                    _snprintf_s(buf, _TRUNCATE, "%s", e.label[0]?e.label:"player");
                dl->AddText(ImVec2(sp.x, sp.y), IM_COL32(120, 200, 255, 230), buf);
            }
        }
    }

    void OnRenderUI() override {
        ImGui::Checkbox("Enabled", &m_enabled);
        ImGui::Checkbox("Show distance", &m_drawDistance);
        ImGui::SliderFloat("Max distance (m)", &m_maxDistance, 10.0f, 1000.0f, "%.0f");
        ImGui::Text("Players in cache: %zu", m_cache.size());
    }
};
static PlayerEspFeature s_playerEsp;

// ════════════════════════════════════════════════════════════════════════════
// 4. NoClip - shell.  Real implementation needs to resolve and patch the
//    KCC's collision-mask read (VibraniumKCC.VibraniumKCCMotor.CheckIfColliderValidForCollisions).
// ════════════════════════════════════════════════════════════════════════════
class NoClipFeature : public pipeline::Feature {
public:
    NoClipFeature() : Feature("NoClip (research)", pipeline::Category::Movement, false) {}
    void OnRenderUI() override {
        ImGui::TextWrapped(
            "NoClip target: VibraniumKCC.VibraniumKCCMotor.CheckIfColliderValidForCollisions(Collider) -> bool.\n"
            "Forcing it to false would skip all collision filtering.\n"
            "This isn't safe to enable yet - the motor may dereference the "
            "collider further down. Use the Patches tab once verified.\n"
            "Discovery info:\n"
            "  ptr=00007FFEDA3CDD90  RVA=0x119DD90  argc=1\n"
            "  CheckStepValidity(...) at 0x119DEC0 (try return false to glide).");
    }
};
static NoClipFeature s_noclip;

// ════════════════════════════════════════════════════════════════════════════
// 5. AutoLoot - shell.  LootableDrop has no client-side Take(); looting goes
//    through InteractionComponent.InteractStart(interactionId, unitGuid).
//    Until we map LootableDrop -> interactionId we can only enumerate them.
// ════════════════════════════════════════════════════════════════════════════
class AutoLootFeature : public pipeline::Feature {
public:
    AutoLootFeature() : Feature("AutoLoot (research)", pipeline::Category::Combat, false) {}
    void OnRenderUI() override {
        ImGui::TextWrapped(
            "AutoLoot target: LootableDrop has no client-side Take().\n"
            "Looting routes through InteractionComponent.InteractStart\n"
            "  (uint interactionId, ulong unitGuid) -> void.\n"
            "  ptr=00007FFEDA28EB40  RVA=0x105EB40\n"
            "We need to:\n"
            "  1) enumerate Entities.LootableDrop instances near the player\n"
            "  2) resolve their interactionId (likely a field on the drop)\n"
            "  3) call InteractStart for each, throttled.\n"
            "Once item #2 is mapped this feature can ship.");
    }
};
static AutoLootFeature s_autoLoot;

// ════════════════════════════════════════════════════════════════════════════
// 6. Instant Cast - shell. The cooldown patch already removes spell cooldowns;
//    cast time itself lives inside CombatComponent's per-spell timing fields.
//    Need a class-layout walk to find them.
// ════════════════════════════════════════════════════════════════════════════
class InstantCastFeature : public pipeline::Feature {
public:
    InstantCastFeature() : Feature("Instant Cast (research)", pipeline::Category::Combat, false) {}
    void OnRenderUI() override {
        ImGui::TextWrapped(
            "Instant Cast target: cast-time fields inside\n"
            "World.Components.CombatComponent (size=0x238).\n"
            "Server-authoritative: even if the client thinks cast finished\n"
            "instantly, the server may still apply the original cast time.\n"
            "Pair with the No Spell Cooldowns patch (already in Patches tab)\n"
            "for the smoothest local feel.");
    }
};
static InstantCastFeature s_instantCast;
