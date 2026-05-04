// ════════════════════════════════════════════════════════════════════════════
// game_api_inspector.cpp  -  Live read-only view of every resolved IL2CPP
// symbol the tool knows about.  Helps debug "did this resolve?" questions and
// gives any future pipeline a one-stop shop for browsing what's available.
// ════════════════════════════════════════════════════════════════════════════
#include "pipeline.h"
#include "game_api.h"
#include "log.h"
#include "vendor/imgui/imgui.h"

#include <cstdio>
#include <cstring>

namespace {

static char g_filter[64] = {};

static bool PassFilter(const char* a, const char* b = nullptr) {
    if (!g_filter[0]) return true;
    if (a && _strnicmp(a, "", 0) == 0) {} // no-op
    auto contains = [&](const char* s) {
        if (!s || !*s) return false;
        // case-insensitive substring
        size_t flen = strlen(g_filter);
        size_t slen = strlen(s);
        if (flen > slen) return false;
        for (size_t i = 0; i + flen <= slen; i++) {
            size_t k = 0;
            for (; k < flen; k++) {
                char x = s[i+k]; if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
                char y = g_filter[k]; if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
                if (x != y) break;
            }
            if (k == flen) return true;
        }
        return false;
    };
    return contains(a) || contains(b);
}

static void DrawClassRow(const gameapi::ClassSym& s) {
    if (!PassFilter(s.ns, s.name)) return;
    ImVec4 col = s.cls ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
    ImGui::TextColored(col, "%-32s %-44s  cls=%p  size=0x%X",
                       s.ns && *s.ns ? s.ns : "<root>",
                       s.name, (void*)s.cls, s.sizeBytes);
}

static void DrawMethodRow(const gameapi::MethodSym& s) {
    if (!PassFilter(s.label, s.name)) return;
    ImVec4 col = s.mi ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
    ImGui::TextColored(col, "%-72s argc=%-3d  ptr=%p  RVA=0x%07X",
                       s.label, s.argc, s.ptr, s.rva);
}

class GameApiInspectorFeature : public pipeline::Feature {
public:
    GameApiInspectorFeature()
        : Feature("Game API", pipeline::Category::Debug, true) {}

    void OnRenderUI() override {
        const auto& A = gameapi::G();
        ImGui::TextDisabled("GameAssembly base = %p", (void*)gameapi::GameAsmBase());
        ImGui::Text("Resolved: classes %d/%d   methods %d/%d   %s",
                    A.classesResolved, A.classesTotal,
                    A.methodsResolved, A.methodsTotal,
                    A.resolved ? "[OK]" : "[NOT YET]");
        ImGui::SameLine();
        if (ImGui::SmallButton("Re-resolve")) gameapi::Reresolve();

        ImGui::InputText("Filter", g_filter, sizeof(g_filter));
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Classes", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawClassRow(A.entitiesManager);
            DrawClassRow(A.player);
            DrawClassRow(A.botPlayer);
            DrawClassRow(A.unit);
            DrawClassRow(A.npc);
            DrawClassRow(A.mount);
            DrawClassRow(A.playerMoveController);
            DrawClassRow(A.localPlayerMoveController);
            DrawClassRow(A.remotePlayerMoveController);
            DrawClassRow(A.playerCameraController);
            DrawClassRow(A.playerCombatController);
            DrawClassRow(A.playerInputController);
            DrawClassRow(A.playerMountController);
            DrawClassRow(A.basicAttackController);
            DrawClassRow(A.masterAnimController);
            DrawClassRow(A.movementComponent);
            DrawClassRow(A.combatComponent);
            DrawClassRow(A.unitStats);
            DrawClassRow(A.resourcesComponent);
            DrawClassRow(A.resourceNode);
            DrawClassRow(A.characterSelectionManager);
            DrawClassRow(A.classSO);
            DrawClassRow(A.raceModelSO);
            DrawClassRow(A.classButton);
            DrawClassRow(A.raceToggle);
            DrawClassRow(A.interactionManager);
            DrawClassRow(A.interaction);
            DrawClassRow(A.interactionComponent);
            DrawClassRow(A.spellUtilities);
            DrawClassRow(A.veNode);
            DrawClassRow(A.spellsDatabase);
            DrawClassRow(A.spellTemplate);
            DrawClassRow(A.spellCooldownResolver);
            DrawClassRow(A.talentsModelSO);
            DrawClassRow(A.playerSkillListView);
            DrawClassRow(A.uiViewSkillCategoriesView);
            DrawClassRow(A.uiViewSkillList);
            DrawClassRow(A.unityComponent);
            DrawClassRow(A.unityTransform);
        }

        if (ImGui::CollapsingHeader("Methods", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawMethodRow(A.EM_get_Player);
            DrawMethodRow(A.Player_GetMoveSpeed);
            DrawMethodRow(A.Unit_get_MoveSpeedMultiplier);
            DrawMethodRow(A.Unit_IsHostile);
            DrawMethodRow(A.Npc_IsHostile);
            DrawMethodRow(A.MasterAnim_get_SpeedMultiplier);
            DrawMethodRow(A.Unit_GetStatAmount);
            DrawMethodRow(A.Unit_IsAlive);
            DrawMethodRow(A.Unit_IsDead);
            DrawMethodRow(A.Unit_get_IsInCombat);
            DrawMethodRow(A.Unit_get_CombatComponent);
            DrawMethodRow(A.Unit_get_MovementComponent);
            DrawMethodRow(A.CSM_GetRaceLocked);
            DrawMethodRow(A.CSM_SetRaceLock);
            DrawMethodRow(A.CSM_SendUnlockedRacesRequest);
            DrawMethodRow(A.ClassSO_IsLockedForRace);
            DrawMethodRow(A.RaceModelSO_get_Locked);
            DrawMethodRow(A.ClassButton_CheckIsLockedForRace);
            DrawMethodRow(A.ClassButton_IsInteractable);
            DrawMethodRow(A.RaceToggle_IsInteractable);
            DrawMethodRow(A.IM_CheckForInteract);
            DrawMethodRow(A.IM_get_InteractableComponentInRange);
            DrawMethodRow(A.IM_InteractionObjectHit);
            DrawMethodRow(A.IC_InteractStart);
            DrawMethodRow(A.Interaction_IsAvailableForPlayer);
            DrawMethodRow(A.Interaction_GetId);
            DrawMethodRow(A.SU_get_InteractionManager);
            DrawMethodRow(A.VENode_GetGuid);
            DrawMethodRow(A.SpellsDatabase_get_Spells);
            DrawMethodRow(A.SCR_GetCooldownReductionMs);
            DrawMethodRow(A.PSLV_IsLockedByRank);
            DrawMethodRow(A.UVSCV_IsLockedByRank);
            DrawMethodRow(A.UVSL_IsLockedByRank);
            DrawMethodRow(A.Component_get_transform);
            DrawMethodRow(A.Transform_get_position);
            DrawMethodRow(A.Transform_set_position);
        }

        if (ImGui::CollapsingHeader("Field offsets")) {
            ImGui::Text("Player.MovementModifier      = 0x%03zX", A.fld_Player_MovementModifier);
            ImGui::Text("Player.BaseMovementSpeed     = 0x%03zX", A.fld_Player_BaseMovementSpeed);
            ImGui::Text("Player.CurrentMovementSpeed  = 0x%03zX", A.fld_Player_CurrentMovementSpeed);
            ImGui::Text("Player.JumpHeight            = 0x%03zX", A.fld_Player_JumpHeight);
            ImGui::Text("Player.JumpSpeed             = 0x%03zX", A.fld_Player_JumpSpeed);
            ImGui::Separator();
            ImGui::Text("EntitiesManager.Player       = 0x%03zX", A.fld_EM_Player);
            ImGui::Separator();
            ImGui::Text("Movement.SpeedModifier       = 0x%03zX", A.fld_Movement_SpeedModifier);
            ImGui::Text("Movement.BaseMovementSpeed   = 0x%03zX", A.fld_Movement_BaseMovementSpeed);
            ImGui::Text("Movement.CurrentMovementSpeed= 0x%03zX", A.fld_Movement_CurrentMovementSpeed);
            ImGui::Separator();
            ImGui::Text("ResourceNode.MiniGameType    = 0x%03zX", A.fld_Node_MiniGameType);
            ImGui::Text("ResourceNode.IsDead          = 0x%03zX", A.fld_Node_IsDead);
            ImGui::Text("ResourceNode.Percentage      = 0x%03zX", A.fld_Node_Percentage);
            ImGui::Separator();
            ImGui::Text("InteractionComponent.interactions = 0x%03zX", A.fld_IC_interactions);
            ImGui::Text("InteractionComponent.ownerObject  = 0x%03zX", A.fld_IC_ownerObject);
            ImGui::Text("VENode.guid                       = 0x%03zX", A.fld_VENode_guid);
            ImGui::Separator();
            ImGui::Text("SpellTemplate.id              = 0x%03zX", A.fld_Spell_id);
            ImGui::Text("SpellTemplate.channel_time    = 0x%03zX (ms)", A.fld_Spell_channel_time);
            ImGui::Text("SpellTemplate.cast_time       = 0x%03zX (ms)", A.fld_Spell_cast_time);
            ImGui::Text("SpellTemplate.cooldown        = 0x%03zX (ms)", A.fld_Spell_cooldown);
            ImGui::Text("SpellTemplate.anim_lock_delay = 0x%03zX (ms)", A.fld_Spell_anim_lock_delay);
            ImGui::Text("SpellTemplate.globalcooldown  = 0x%03zX (ms)", A.fld_Spell_globalcooldown);
            ImGui::Separator();
            ImGui::Text("List<T>._items / ._size       = 0x%03zX / 0x%03zX", A.fld_List_items, A.fld_List_size);
            ImGui::Text("T[].vector start              = 0x%03zX", A.fld_Array_data);
        }
    }
};

static GameApiInspectorFeature s_gameApiInspector;

} // anonymous
