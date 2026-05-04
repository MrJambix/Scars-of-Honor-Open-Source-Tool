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
// 5. AutoLoot
//
//   Server-driven loot flow:
//     IM = SpellUtilities.get_InteractionManager()                  (singleton)
//     IC = IM.get_InteractableComponentInRange()                    (or null)
//     foreach Interaction in IC._interactions  (List<Interaction>):
//         IC.InteractStart(Interaction.GetId(), player.VENode.GetGuid())
//
//   We call this on a throttled tick when enabled and a player is around.
// ════════════════════════════════════════════════════════════════════════════
class AutoLootFeature : public pipeline::Feature {
public:
    AutoLootFeature() : Feature("AutoLoot", pipeline::Category::Combat, false) {}

    bool   m_enabled       = true;
    int    m_intervalMs    = 350;
    DWORD  m_nextMs        = 0;
    int    m_lastFired     = 0;
    int    m_totalFired    = 0;
    char   m_status[160]   = "idle";

    void OnTick() override {
        if (!m_enabled) return;
        DWORD now = GetTickCount();
        if (now < m_nextMs) return;
        m_nextMs = now + (DWORD)m_intervalMs;
        Pulse();
    }

    void OnRenderUI() override {
        const auto& A = gameapi::G();
        ImGui::Checkbox("Enabled", &m_enabled);
        ImGui::SliderInt("Interval (ms)", &m_intervalMs, 100, 2000);
        ImGui::Separator();
        ImGui::Text("Symbol status:");
        ImGui::Text("  IM.get_InteractableComponentInRange : %s", A.IM_get_InteractableComponentInRange.ptr ? "OK" : "MISSING");
        ImGui::Text("  IC.InteractStart                    : %s", A.IC_InteractStart.ptr               ? "OK" : "MISSING");
        ImGui::Text("  Interaction.GetId                   : %s", A.Interaction_GetId.ptr              ? "OK" : "MISSING");
        ImGui::Text("  SpellUtilities.get_InteractionManager: %s", A.SU_get_InteractionManager.ptr     ? "OK" : "MISSING");
        ImGui::Text("  VENode.GetGuid                      : %s", A.VENode_GetGuid.ptr                 ? "OK" : "MISSING");
        ImGui::Separator();
        ImGui::Text("Last pulse fired %d interaction(s); total %d", m_lastFired, m_totalFired);
        ImGui::TextWrapped("Status: %s", m_status);
        if (ImGui::Button("Pulse now")) Pulse();
    }

private:
    void Pulse() {
        m_lastFired = 0;
        const auto& A = gameapi::G();
        if (!A.SU_get_InteractionManager.ptr || !A.IM_get_InteractableComponentInRange.ptr ||
            !A.IC_InteractStart.ptr || !A.Interaction_GetId.ptr || !A.VENode_GetGuid.ptr) {
            _snprintf_s(m_status, _TRUNCATE, "required symbols missing");
            return;
        }
        void* player = game::MainPlayer();
        if (!player) { _snprintf_s(m_status, _TRUNCATE, "no player"); return; }

        __try {
            // 1) singleton IM
            using FnIM = void*(*)(const MethodInfo*);
            void* im = ((FnIM)A.SU_get_InteractionManager.ptr)(A.SU_get_InteractionManager.mi);
            if (!im) { _snprintf_s(m_status, _TRUNCATE, "IM null"); return; }

            // 2) closest InteractionComponent
            using FnIC = void*(*)(void*, const MethodInfo*);
            void* ic = ((FnIC)A.IM_get_InteractableComponentInRange.ptr)(im, A.IM_get_InteractableComponentInRange.mi);
            if (!ic) { _snprintf_s(m_status, _TRUNCATE, "no IC in range"); return; }

            // 3) player guid
            using FnGuid = uint64_t(*)(void*, const MethodInfo*);
            uint64_t guid = ((FnGuid)A.VENode_GetGuid.ptr)(player, A.VENode_GetGuid.mi);

            // 4) walk _interactions (List<Interaction>) at fld_IC_interactions
            void* list = *(void**)((uint8_t*)ic + A.fld_IC_interactions);
            if (!list) { _snprintf_s(m_status, _TRUNCATE, "_interactions null"); return; }
            void* arr  = *(void**)((uint8_t*)list + A.fld_List_items);
            int   size = *(int*)  ((uint8_t*)list + A.fld_List_size);
            if (!arr || size <= 0) { _snprintf_s(m_status, _TRUNCATE, "no interactions (size=%d)", size); return; }
            if (size > 16) size = 16;  // safety

            using FnGetId = uint32_t(*)(void*, const MethodInfo*);
            using FnStart = void(*)(void*, uint32_t, uint64_t, const MethodInfo*);
            for (int i = 0; i < size; i++) {
                void* interaction = *(void**)((uint8_t*)arr + A.fld_Array_data + (size_t)i * sizeof(void*));
                if (!interaction) continue;
                uint32_t id = ((FnGetId)A.Interaction_GetId.ptr)(interaction, A.Interaction_GetId.mi);
                if (id == 0) continue;
                ((FnStart)A.IC_InteractStart.ptr)(ic, id, guid, A.IC_InteractStart.mi);
                m_lastFired++;
                m_totalFired++;
            }
            _snprintf_s(m_status, _TRUNCATE,
                "IM=%p IC=%p guid=%llu list[%d] -> fired %d",
                im, ic, (unsigned long long)guid, size, m_lastFired);
            if (m_lastFired > 0) LOGI("autoloot: pulsed %d interactions on IC=%p", m_lastFired, ic);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(m_status, _TRUNCATE, "SEH during pulse");
        }
    }
};
static AutoLootFeature s_autoLoot;

// ════════════════════════════════════════════════════════════════════════════
// 6. Instant Cast
//
//   Walk SpellsDatabase.get_Spells() (List<SpellTemplate>), back up the
//   original timing fields, then zero them.  This is purely client-side: the
//   server still validates server-side timing, but the local cast bar will
//   complete instantly and the client UI behaves accordingly.  The companion
//   No-Cooldowns patch covers the recharge half.
// ════════════════════════════════════════════════════════════════════════════
class InstantCastFeature : public pipeline::Feature {
public:
    InstantCastFeature() : Feature("Instant Cast", pipeline::Category::Combat, false) {}

    struct Saved {
        void*    spell        = nullptr;
        uint32_t cast_time    = 0;
        uint32_t channel_time = 0;
        uint32_t anim_lock    = 0;
        uint32_t global_cd    = 0;
    };
    std::vector<Saved> m_saved;
    bool m_active           = false;
    bool m_zeroChannel      = true;
    bool m_zeroAnimLock     = true;
    bool m_zeroGCD          = false;  // GCD often desynchronises -- off by default
    char m_status[160]      = "not applied";
    uint32_t m_lastCount    = 0;

    void OnRenderUI() override {
        const auto& A = gameapi::G();
        ImGui::Checkbox("Also zero channel_time",     &m_zeroChannel);
        ImGui::Checkbox("Also zero anim_lock_delay",  &m_zeroAnimLock);
        ImGui::Checkbox("Also zero globalcooldown",   &m_zeroGCD);
        ImGui::Separator();
        ImGui::Text("Symbol status:");
        ImGui::Text("  SpellsDatabase.get_Spells : %s", A.SpellsDatabase_get_Spells.ptr ? "OK" : "MISSING");
        ImGui::Text("  spellTemplate class       : %s", A.spellTemplate.cls ? "OK" : "MISSING");
        ImGui::Separator();
        if (!m_active) {
            if (ImGui::Button("Apply (zero cast times)")) Apply();
        } else {
            ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "ACTIVE: %u spells modified", m_lastCount);
            if (ImGui::Button("Restore originals")) Restore();
        }
        ImGui::TextWrapped("Status: %s", m_status);
    }

    void OnShutdown() override { if (m_active) Restore(); }

private:
    void* GrabSpellsDatabase() {
        // Source: Player -> Unit.get_CombatComponent -> _spellsDatabase at 0x50.
        const auto& A = gameapi::G();
        void* p = game::MainPlayer();
        if (!p || !A.Unit_get_CombatComponent.ptr) return nullptr;
        __try {
            using FnCC = void*(*)(void*, const MethodInfo*);
            void* cc = ((FnCC)A.Unit_get_CombatComponent.ptr)(p, A.Unit_get_CombatComponent.mi);
            if (!cc) return nullptr;
            return *(void**)((uint8_t*)cc + 0x50);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void Apply() {
        const auto& A = gameapi::G();
        m_saved.clear();
        m_lastCount = 0;
        if (!A.SpellsDatabase_get_Spells.ptr) {
            _snprintf_s(m_status, _TRUNCATE, "SpellsDatabase.get_Spells unresolved");
            return;
        }
        void* db = GrabSpellsDatabase();
        if (!db) { _snprintf_s(m_status, _TRUNCATE, "no SpellsDatabase via CombatComponent yet"); return; }
        __try {
            using FnSpells = void*(*)(void*, const MethodInfo*);
            void* list = ((FnSpells)A.SpellsDatabase_get_Spells.ptr)(db, A.SpellsDatabase_get_Spells.mi);
            if (!list) { _snprintf_s(m_status, _TRUNCATE, "spells list null"); return; }
            void* arr  = *(void**)((uint8_t*)list + A.fld_List_items);
            int   size = *(int*)  ((uint8_t*)list + A.fld_List_size);
            if (!arr || size <= 0) { _snprintf_s(m_status, _TRUNCATE, "empty spells list (size=%d)", size); return; }
            if (size > 4096) size = 4096;
            m_saved.reserve((size_t)size);
            for (int i = 0; i < size; i++) {
                void* sp = *(void**)((uint8_t*)arr + A.fld_Array_data + (size_t)i * sizeof(void*));
                if (!sp) continue;
                Saved s{};
                s.spell        = sp;
                s.cast_time    = *(uint32_t*)((uint8_t*)sp + A.fld_Spell_cast_time);
                s.channel_time = *(uint32_t*)((uint8_t*)sp + A.fld_Spell_channel_time);
                s.anim_lock    = *(uint32_t*)((uint8_t*)sp + A.fld_Spell_anim_lock_delay);
                s.global_cd    = *(uint32_t*)((uint8_t*)sp + A.fld_Spell_globalcooldown);
                m_saved.push_back(s);
                *(uint32_t*)((uint8_t*)sp + A.fld_Spell_cast_time) = 0;
                if (m_zeroChannel)  *(uint32_t*)((uint8_t*)sp + A.fld_Spell_channel_time)    = 0;
                if (m_zeroAnimLock) *(uint32_t*)((uint8_t*)sp + A.fld_Spell_anim_lock_delay) = 0;
                if (m_zeroGCD)      *(uint32_t*)((uint8_t*)sp + A.fld_Spell_globalcooldown)  = 0;
                m_lastCount++;
            }
            m_active = true;
            _snprintf_s(m_status, _TRUNCATE, "OK: zeroed cast times on %u spells", m_lastCount);
            LOGI("instantcast: zeroed %u spells", m_lastCount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(m_status, _TRUNCATE, "SEH during apply (partial: %zu)", m_saved.size());
        }
    }

    void Restore() {
        const auto& A = gameapi::G();
        size_t restored = 0;
        __try {
            for (auto& s : m_saved) {
                if (!s.spell) continue;
                *(uint32_t*)((uint8_t*)s.spell + A.fld_Spell_cast_time)       = s.cast_time;
                *(uint32_t*)((uint8_t*)s.spell + A.fld_Spell_channel_time)    = s.channel_time;
                *(uint32_t*)((uint8_t*)s.spell + A.fld_Spell_anim_lock_delay) = s.anim_lock;
                *(uint32_t*)((uint8_t*)s.spell + A.fld_Spell_globalcooldown)  = s.global_cd;
                restored++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        m_saved.clear();
        m_active = false;
        _snprintf_s(m_status, _TRUNCATE, "restored %zu spell(s)", restored);
        LOGI("instantcast: restored %zu spells", restored);
    }
};
static InstantCastFeature s_instantCast;

// ════════════════════════════════════════════════════════════════════════════
// 7. Unlimited Resources (Mana / Energy / Fury / Stamina ...)
//
//   Every tick we find every World.Components.ResourcesComponent in the scene
//   (it derives from VEComponent : MonoBehaviour, so FindObjectsOfType works),
//   pick the one whose _unit pointer equals our MainPlayer, and walk its
//   _resources list.  For each ResourceStruct entry whose id is enabled we
//   write CurrentAmount := MaxAmount.
//
//   Caveats (be honest):
//     * This is a CLIENT-SIDE memory write.  The server still validates spell
//       costs and may reject casts you can't actually afford.  In single
//       player / playtest builds where the client also runs the simulation
//       this is enough; on a strict authoritative server, expect refusals.
//     * HP is OFF by default - if you want godmode flip the toggle, but
//       expect the server to either rubber-band you back or eventually
//       desync the unit.
// ════════════════════════════════════════════════════════════════════════════
class UnlimitedManaFeature : public pipeline::Feature {
public:
    UnlimitedManaFeature() : Feature("Unlimited Mana", pipeline::Category::Combat, false) {}

    // Tunables
    bool m_refillMana    = true;
    bool m_refillEnergy  = true;
    bool m_refillFury    = true;
    bool m_refillStamina = true;
    bool m_refillOther   = false;   // any non-HP resource we don't have a flag for
    bool m_refillHP      = false;   // godmode-ish, off by default
    int  m_intervalMs    = 200;

    // Diagnostics
    DWORD    m_nextTick   = 0;
    uint32_t m_lastWrites = 0;
    uint32_t m_totalWrites= 0;
    char     m_status[160]= "idle";

    // ResourceID ordinal values (assigned in declaration order in the enum).
    enum : uint8_t {
        RID_HP      = 0,
        RID_Mana    = 1,
        RID_Energy  = 2,
        RID_Fury    = 3,
        // 4..N follow but are assorted server-side resources we treat as "other".
        RID_Stamina_Hint = 22,   // Resource_Stamina sits near the top of the enum
    };

    bool ShouldRefill(uint8_t id) const {
        if (id == RID_HP)      return m_refillHP;
        if (id == RID_Mana)    return m_refillMana;
        if (id == RID_Energy)  return m_refillEnergy;
        if (id == RID_Fury)    return m_refillFury;
        // Stamina enum index isn't fixed across builds; treat anything we don't
        // explicitly recognise as "other".  Stamina toggle gates that bucket too.
        if (m_refillStamina || m_refillOther) return true;
        return false;
    }

    void OnTick() override {
        const auto& A = gameapi::G();
        if (!A.resourcesComponent.cls) {
            _snprintf_s(m_status, _TRUNCATE, "ResourcesComponent class unresolved");
            return;
        }
        DWORD now = GetTickCount();
        if (now < m_nextTick) return;
        m_nextTick = now + (DWORD)(m_intervalMs > 25 ? m_intervalMs : 25);

        void* player = game::MainPlayer();
        if (!player) { m_lastWrites = 0; _snprintf_s(m_status, _TRUNCATE, "no local player"); return; }

        uint32_t writes = 0;
        __try {
            uint32_t n = 0;
            // Cached: lots of features ask for ResourcesComponent within the
            // same frame -- share one FindObjectsOfType call across them.
            void* arr = FindObjectsOfTypeCached(A.resourcesComponent.cls, n, 200);
            if (!arr || n == 0) {
                _snprintf_s(m_status, _TRUNCATE, "no ResourcesComponent in scene");
                m_lastWrites = 0;
                return;
            }
            void** elems = reinterpret_cast<void**>((char*)arr + A.fld_Array_data);
            for (uint32_t i = 0; i < n; i++) {
                void* rc = elems[i];
                if (!game::IsLikelyAlive(rc)) continue;   // skip freed/poisoned slots
                void* unit = *(void**)((uint8_t*)rc + A.fld_RC_unit);
                if (unit != player) continue;

                void* list = *(void**)((uint8_t*)rc + A.fld_RC_resources);
                if (!game::IsLikelyAlive(list)) continue;
                void* itemsArr = *(void**)((uint8_t*)list + A.fld_List_items);
                int   size     = *(int*)  ((uint8_t*)list + A.fld_List_size);
                if (!itemsArr || size <= 0 || size > 64) continue;

                uint8_t* data = (uint8_t*)itemsArr + A.fld_Array_data;
                for (int j = 0; j < size; j++) {
                    uint8_t* el = data + (size_t)j * A.resourceStruct_stride;
                    uint8_t  id  = *(uint8_t* )(el + A.fld_Resource_id);
                    uint32_t cur = *(uint32_t*)(el + A.fld_Resource_current);
                    uint32_t mx  = *(uint32_t*)(el + A.fld_Resource_max);
                    if (mx == 0 || cur >= mx) continue;
                    if (!ShouldRefill(id)) continue;
                    *(uint32_t*)(el + A.fld_Resource_current) = mx;
                    writes++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(m_status, _TRUNCATE, "SEH during refill (cancelled)");
            return;
        }
        m_lastWrites = writes;
        m_totalWrites += writes;
        _snprintf_s(m_status, _TRUNCATE,
                    "OK: wrote %u this tick, %u total", m_lastWrites, m_totalWrites);
    }

    void OnRenderUI() override {
        ImGui::TextWrapped("Continuously sets CurrentAmount := MaxAmount on the local "
                           "player's ResourcesComponent.  Client-side only -- the server "
                           "still validates spell costs.");
        ImGui::Separator();
        ImGui::Checkbox("Refill Mana",     &m_refillMana);
        ImGui::Checkbox("Refill Energy",   &m_refillEnergy);
        ImGui::Checkbox("Refill Fury",     &m_refillFury);
        ImGui::Checkbox("Refill Stamina",  &m_refillStamina);
        ImGui::Checkbox("Refill any other non-HP resource", &m_refillOther);
        ImGui::Separator();
        ImGui::Checkbox("Refill HP (godmode-ish, may desync)", &m_refillHP);
        ImGui::SliderInt("Interval (ms)", &m_intervalMs, 25, 1000);
        ImGui::Separator();
        const auto& A = gameapi::G();
        ImGui::Text("ResourcesComponent class: %s",
                    A.resourcesComponent.cls ? "OK" : "MISSING");
        ImGui::Text("Status: %s", m_status);
    }
};
static UnlimitedManaFeature s_unlimitedMana;

// ════════════════════════════════════════════════════════════════════════════
// 8. Unlimited Talent Points
//
//   Talents.UI.UiDataTalentsModelSO : ScriptableObject (size=0x38).
//     0x20 AvailableTalentPoints  UInt32
//     0x24 SpentTalentPoints      UInt32
//     0x28 RefundValue            UInt32
//
//   ScriptableObject derives from UnityEngine.Object so FindObjectsOfType
//   works.  We just keep AvailableTalentPoints clamped to a high value.
//
//   Caveat: server still validates spend on actual application.  Whether the
//   talent gets persisted depends on the server check; the client will at
//   least let you click everything.
// ════════════════════════════════════════════════════════════════════════════
class UnlimitedTalentsFeature : public pipeline::Feature {
public:
    UnlimitedTalentsFeature()
        : Feature("Unlimited Talent Points", pipeline::Category::Combat, false) {}

    int      m_target     = 9999;
    int      m_intervalMs = 250;
    DWORD    m_nextTick   = 0;
    uint32_t m_lastWrites = 0;
    char     m_status[160]= "idle";

    void OnTick() override {
        const auto& A = gameapi::G();
        if (!A.talentsModelSO.cls) {
            _snprintf_s(m_status, _TRUNCATE, "talentsModelSO unresolved");
            return;
        }
        DWORD now = GetTickCount();
        if (now < m_nextTick) return;
        m_nextTick = now + (DWORD)(m_intervalMs > 50 ? m_intervalMs : 50);

        uint32_t writes = 0;
        __try {
            uint32_t n = 0;
            void* arr = FindObjectsOfTypeCached(A.talentsModelSO.cls, n, 500);
            if (!arr || n == 0) {
                _snprintf_s(m_status, _TRUNCATE, "no UiDataTalentsModelSO in scene");
                m_lastWrites = 0;
                return;
            }
            void** elems = reinterpret_cast<void**>((char*)arr + A.fld_Array_data);
            uint32_t target = (uint32_t)(m_target < 0 ? 0 : m_target);
            for (uint32_t i = 0; i < n; i++) {
                void* m = elems[i];
                if (!game::IsLikelyAlive(m)) continue;   // skip freed slots
                uint32_t cur = *(uint32_t*)((uint8_t*)m + A.fld_Talents_available);
                if (cur < target) {
                    *(uint32_t*)((uint8_t*)m + A.fld_Talents_available) = target;
                    writes++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(m_status, _TRUNCATE, "SEH (cancelled)");
            return;
        }
        m_lastWrites = writes;
        _snprintf_s(m_status, _TRUNCATE, "OK: bumped %u tree(s) to %d", writes, m_target);
    }

    void OnRenderUI() override {
        ImGui::TextWrapped("Forces every UiDataTalentsModelSO instance's "
                           "AvailableTalentPoints to the target value.  "
                           "Client-side -- the server still authorizes the actual "
                           "talent purchase, so this may or may not stick.");
        ImGui::Separator();
        ImGui::SliderInt("Target points", &m_target, 0, 9999);
        ImGui::SliderInt("Interval (ms)", &m_intervalMs, 50, 2000);
        const auto& A = gameapi::G();
        ImGui::Text("talentsModelSO class : %s", A.talentsModelSO.cls ? "OK" : "MISSING");
        ImGui::Text("Status: %s", m_status);
    }
};
static UnlimitedTalentsFeature s_unlimitedTalents;

// ════════════════════════════════════════════════════════════════════════════
// 9. Unlimited Spell Range
//
//   SpellTemplate.max_range (float) lives at offset 0x28 inside every entry
//   of SpellsDatabase.get_Spells().  We back the originals up, multiply by
//   a user-controlled factor, and restore on disable.
//
//   The client uses max_range to gate "can cast / target in range" UI.  The
//   server has its own range check, so casts may still get rejected at long
//   distances; expect best-mileage out of this for UI / aim-assist purposes.
// ════════════════════════════════════════════════════════════════════════════
class UnlimitedRangeFeature : public pipeline::Feature {
public:
    UnlimitedRangeFeature() : Feature("Unlimited Spell Range", pipeline::Category::Combat, false) {}

    struct Saved { void* spell = nullptr; float range = 0.0f; };
    std::vector<Saved> m_saved;
    bool   m_active   = false;
    float  m_target   = 5000.0f;        // absolute range to write
    char   m_status[160] = "not applied";
    uint32_t m_lastCount = 0;

    void OnRenderUI() override {
        const auto& A = gameapi::G();
        ImGui::TextWrapped("Overwrites SpellTemplate.max_range on every spell to "
                           "the target value below.  Client-side change; the server "
                           "may still reject very distant casts.");
        ImGui::Separator();
        ImGui::SliderFloat("Target max_range", &m_target, 50.0f, 100000.0f, "%.0f");
        ImGui::Text("Symbol status:");
        ImGui::Text("  SpellsDatabase.get_Spells : %s",
                    A.SpellsDatabase_get_Spells.ptr ? "OK" : "MISSING");
        ImGui::Text("  spellTemplate class       : %s",
                    A.spellTemplate.cls ? "OK" : "MISSING");
        ImGui::Separator();
        if (!m_active) {
            if (ImGui::Button("Apply")) Apply();
        } else {
            ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1),
                               "ACTIVE: %u spells overwritten", m_lastCount);
            if (ImGui::Button("Restore originals")) Restore();
            ImGui::SameLine();
            if (ImGui::Button("Re-apply (new value)")) { Restore(); Apply(); }
        }
        ImGui::TextWrapped("Status: %s", m_status);
    }

    void OnShutdown() override { if (m_active) Restore(); }

private:
    // Reuse the InstantCast helper path inline.
    void* GrabSpellsDatabase() {
        const auto& A = gameapi::G();
        void* p = game::MainPlayer();
        if (!p || !A.Unit_get_CombatComponent.ptr) return nullptr;
        __try {
            using FnCC = void*(*)(void*, const MethodInfo*);
            void* cc = ((FnCC)A.Unit_get_CombatComponent.ptr)(p, A.Unit_get_CombatComponent.mi);
            if (!cc) return nullptr;
            return *(void**)((uint8_t*)cc + 0x50);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    void Apply() {
        const auto& A = gameapi::G();
        m_saved.clear();
        m_lastCount = 0;
        if (!A.SpellsDatabase_get_Spells.ptr) {
            _snprintf_s(m_status, _TRUNCATE, "SpellsDatabase.get_Spells unresolved");
            return;
        }
        void* db = GrabSpellsDatabase();
        if (!db) { _snprintf_s(m_status, _TRUNCATE, "no SpellsDatabase yet (login first)"); return; }
        __try {
            using FnSpells = void*(*)(void*, const MethodInfo*);
            void* list = ((FnSpells)A.SpellsDatabase_get_Spells.ptr)(db, A.SpellsDatabase_get_Spells.mi);
            if (!list) { _snprintf_s(m_status, _TRUNCATE, "spells list null"); return; }
            void* arr  = *(void**)((uint8_t*)list + A.fld_List_items);
            int   size = *(int*)  ((uint8_t*)list + A.fld_List_size);
            if (!arr || size <= 0) { _snprintf_s(m_status, _TRUNCATE, "empty spells list (size=%d)", size); return; }
            if (size > 4096) size = 4096;
            m_saved.reserve((size_t)size);
            for (int i = 0; i < size; i++) {
                void* sp = *(void**)((uint8_t*)arr + A.fld_Array_data + (size_t)i * sizeof(void*));
                if (!sp) continue;
                Saved s{};
                s.spell = sp;
                s.range = *(float*)((uint8_t*)sp + A.fld_Spell_max_range);
                m_saved.push_back(s);
                *(float*)((uint8_t*)sp + A.fld_Spell_max_range) = m_target;
                m_lastCount++;
            }
            m_active = true;
            _snprintf_s(m_status, _TRUNCATE, "OK: set max_range=%.0f on %u spells",
                        m_target, m_lastCount);
            LOGI("range: bumped %u spells to %.0f", m_lastCount, m_target);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(m_status, _TRUNCATE, "SEH during apply (partial: %zu)", m_saved.size());
        }
    }

    void Restore() {
        const auto& A = gameapi::G();
        size_t restored = 0;
        __try {
            for (auto& s : m_saved) {
                if (!s.spell) continue;
                *(float*)((uint8_t*)s.spell + A.fld_Spell_max_range) = s.range;
                restored++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        m_saved.clear();
        m_active = false;
        _snprintf_s(m_status, _TRUNCATE, "restored %zu spell(s)", restored);
        LOGI("range: restored %zu spells", restored);
    }
};
static UnlimitedRangeFeature s_unlimitedRange;

// ════════════════════════════════════════════════════════════════════════════
// 10. God Mode (aggressive HP refill)
//
//   The cleanest / safest way to "godmode" without finding an invulnerability
//   flag (the dump shows none -- damage is server-authoritative): keep HP at
//   max every single Present().  If incoming damage is small enough that the
//   client would otherwise tick to 0 between server snapshots, we beat it
//   back up to full before death registers locally.
//
//   Reality check:
//     * Server still computes damage and may snap you back when its tick
//       lands (rubber-banding).  Against slow boss hits this works great;
//       against burst PvP it WILL fail.
//     * If the server decides you died you'll respawn -- this won't stop
//       that.  It's a damage-display / survival-against-trash hack.
//
//   Two extras you can toggle:
//     * "Also overwrite ALL units' HP" -- writes max=current on every Unit's
//       ResourcesComponent in the scene.  Useful for tests; obviously not for
//       PvP.  OFF by default.
//     * "Restore on disable" -- pointless for HP (game owns it) but kept off.
// ════════════════════════════════════════════════════════════════════════════
class GodModeFeature : public pipeline::Feature {
public:
    GodModeFeature() : Feature("God Mode (HP refill)", pipeline::Category::Combat, false) {}

    bool     m_allUnits   = false;
    int      m_intervalMs = 100;   // 0 = every frame; default 10 Hz to keep
                                   // FindObjectsOfType off the hot path.
    DWORD    m_nextTick   = 0;
    uint32_t m_lastWrites = 0;
    uint64_t m_totalWrites= 0;
    char     m_status[160]= "idle";

    static constexpr uint8_t RID_HP = 0;

    void OnTick() override {
        const auto& A = gameapi::G();
        if (!A.resourcesComponent.cls) {
            _snprintf_s(m_status, _TRUNCATE, "ResourcesComponent class unresolved");
            return;
        }
        DWORD now = GetTickCount();
        if (m_intervalMs > 0 && now < m_nextTick) return;
        m_nextTick = now + (DWORD)m_intervalMs;

        void* player = m_allUnits ? nullptr : game::MainPlayer();
        if (!m_allUnits && !player) {
            m_lastWrites = 0;
            _snprintf_s(m_status, _TRUNCATE, "no local player");
            return;
        }

        uint32_t writes = 0;
        __try {
            uint32_t n = 0;
            void* arr = FindObjectsOfTypeCached(A.resourcesComponent.cls, n, 200);
            if (!arr || n == 0) {
                _snprintf_s(m_status, _TRUNCATE, "no ResourcesComponent in scene");
                m_lastWrites = 0;
                return;
            }
            void** elems = reinterpret_cast<void**>((char*)arr + A.fld_Array_data);
            for (uint32_t i = 0; i < n; i++) {
                void* rc = elems[i];
                if (!game::IsLikelyAlive(rc)) continue;   // skip freed slots
                if (!m_allUnits) {
                    void* unit = *(void**)((uint8_t*)rc + A.fld_RC_unit);
                    if (unit != player) continue;
                }
                void* list = *(void**)((uint8_t*)rc + A.fld_RC_resources);
                if (!game::IsLikelyAlive(list)) continue;
                void* itemsArr = *(void**)((uint8_t*)list + A.fld_List_items);
                int   size     = *(int*)  ((uint8_t*)list + A.fld_List_size);
                if (!itemsArr || size <= 0 || size > 64) continue;

                uint8_t* data = (uint8_t*)itemsArr + A.fld_Array_data;
                for (int j = 0; j < size; j++) {
                    uint8_t* el = data + (size_t)j * A.resourceStruct_stride;
                    uint8_t  id  = *(uint8_t* )(el + A.fld_Resource_id);
                    if (id != RID_HP) continue;
                    uint32_t cur = *(uint32_t*)(el + A.fld_Resource_current);
                    uint32_t mx  = *(uint32_t*)(el + A.fld_Resource_max);
                    if (mx == 0 || cur >= mx) continue;
                    *(uint32_t*)(el + A.fld_Resource_current) = mx;
                    writes++;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            _snprintf_s(m_status, _TRUNCATE, "SEH during HP refill (cancelled)");
            return;
        }
        m_lastWrites = writes;
        m_totalWrites += writes;
        if (writes) {
            _snprintf_s(m_status, _TRUNCATE,
                        "OK: %u HP write(s) this tick, %llu total",
                        m_lastWrites, (unsigned long long)m_totalWrites);
        }
    }

    void OnRenderUI() override {
        ImGui::TextWrapped(
            "Forces CurrentHP := MaxHP on the local player every frame.  "
            "This is a CLIENT-SIDE write; the server is authoritative for "
            "damage.  Works well against slow ticks (boss DOTs, mob trash) "
            "but burst damage in PvP can still kill you in one server tick.");
        ImGui::Separator();
        ImGui::Checkbox("Also refill ALL units' HP (cheaty)", &m_allUnits);
        ImGui::SliderInt("Interval (ms, 0 = every frame)", &m_intervalMs, 0, 1000);
        ImGui::Separator();
        const auto& A = gameapi::G();
        ImGui::Text("ResourcesComponent class : %s",
                    A.resourcesComponent.cls ? "OK" : "MISSING");
        ImGui::Text("Status: %s", m_status);
        ImGui::Text("Last tick writes: %u   Lifetime writes: %llu",
                    m_lastWrites, (unsigned long long)m_totalWrites);
    }
};
static GodModeFeature s_godMode;
