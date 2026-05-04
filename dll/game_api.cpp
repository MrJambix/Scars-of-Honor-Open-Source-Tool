// ════════════════════════════════════════════════════════════════════════════
// game_api.cpp  -  Resolve every IL2CPP symbol the tool talks to.
//
//   * Walks the registry once (Resolve), logs counts.
//   * Each symbol is independently optional: a missing class or method only
//     leaves its slot null and is reported in the inspector tab; nothing else
//     breaks.
//   * Fast: every FindClass / GetMethod is cached inside il2cpp_helpers.
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "game_api.h"
#include "il2cpp_helpers.h"
#include "log.h"

namespace gameapi {

using namespace il2cpp_helpers;
using il2cpp::GetApi;

static GameAPI g_api;

const GameAPI& G() { return g_api; }

uint8_t* GameAsmBase() {
    static uint8_t* cached = nullptr;
    if (cached) return cached;
    cached = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GameAssembly.dll"));
    return cached;
}

uint32_t RvaOf(const void* p) {
    uint8_t* base = GameAsmBase();
    if (!p || !base) return 0;
    return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p) - base);
}

// ── Method-pointer fetch with SEH fallback ────────────────────────────────
static void* MethodPointerFromMI(const MethodInfo* m) {
    if (!m) return nullptr;
    auto& a = GetApi();
    if (a.method_get_pointer) {
        if (void* p = a.method_get_pointer(m)) return p;
    }
    __try { return *reinterpret_cast<void* const*>(m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ── Resolve one ClassSym ──────────────────────────────────────────────────
static void ResolveClass(ClassSym& s, const char* ns, const char* name) {
    s.ns   = ns;
    s.name = name;
    if (s.cls) return;                               // already resolved
    s.cls = FindClass(ns, name);
    if (!s.cls) return;
    auto& a = GetApi();
    if (a.class_instance_size) s.sizeBytes = (uint32_t)a.class_instance_size(s.cls);
}

// ── Resolve one MethodSym ─────────────────────────────────────────────────
static void ResolveMethod(MethodSym& s, const ClassSym& owner,
                          const char* label, const char* name, int argc) {
    s.label = label;
    s.name  = name;
    s.argc  = argc;
    if (s.mi) return;
    if (!owner.cls) return;
    s.mi  = GetMethod(owner.cls, name, argc);
    s.ptr = MethodPointerFromMI(s.mi);
    s.rva = RvaOf(s.ptr);
}

// ── Public Resolve ────────────────────────────────────────────────────────
bool Resolve() {
    if (!Init()) return false;

    auto& A = g_api;

    // Classes ── (ns, name) pairs ─────────────────────────────────────────
    ResolveClass(A.entitiesManager,            "Entities",                                 "EntitiesManager");
    ResolveClass(A.player,                     "Entities",                                 "Player");
    ResolveClass(A.botPlayer,                  "Entities",                                 "BotPlayer");
    ResolveClass(A.unit,                       "Entities",                                 "Unit");
    ResolveClass(A.npc,                        "Entities",                                 "Npc");
    ResolveClass(A.mount,                      "Entities",                                 "Mount");

    ResolveClass(A.playerMoveController,       "Entities",                                 "PlayerMoveController");
    ResolveClass(A.localPlayerMoveController,  "Entities",                                 "LocalPlayerMoveController");
    ResolveClass(A.remotePlayerMoveController, "Entities",                                 "RemotePlayerMoveController");
    ResolveClass(A.playerCameraController,     "Entities",                                 "PlayerCameraController");
    ResolveClass(A.playerCombatController,     "Entities",                                 "PlayerCombatController");
    ResolveClass(A.playerInputController,      "Entities",                                 "PlayerInputController");
    ResolveClass(A.playerMountController,      "Entities",                                 "PlayerMountController");
    ResolveClass(A.basicAttackController,      "Entities",                                 "PlayerBasicAttackController");
    ResolveClass(A.masterAnimController,       "Entities",                                 "MasterAnimatorController");

    ResolveClass(A.movementComponent,          "World.Components",                         "MovementComponent");
    ResolveClass(A.combatComponent,            "World.Components",                         "CombatComponent");
    ResolveClass(A.unitStats,                  "World.Components",                         "UnitStats");

    ResolveClass(A.resourceNode,               "World.MiniGame",                           "ResourceNodePrefabController");

    ResolveClass(A.characterSelectionManager,  "",                                         "CharacterSelectionManager");
    ResolveClass(A.classSO,                    "ModelManagerNamespace",                    "ClassSO");
    ResolveClass(A.raceModelSO,                "ModelManagerNamespace",                    "RaceModelSO");
    ResolveClass(A.classButton,                "UI.CharacterSelection",                    "ClassButton");
    ResolveClass(A.raceToggle,                 "UI.CharacterSelection",                    "RaceToggle");

    ResolveClass(A.interactionManager,         "",                                         "InteractionManager");
    ResolveClass(A.interaction,                "",                                         "Interaction");

    ResolveClass(A.spellCooldownResolver,      "Source.Scripts.World.Spells",              "SpellCooldownReductionResolver");
    ResolveClass(A.playerSkillListView,        "Source.Scripts.World.Skills.UI",           "PlayerSkillListView");
    ResolveClass(A.uiViewSkillCategoriesView,  "Source.Scripts.World.Skills.UI",           "UIViewSkillCategoriesView");
    ResolveClass(A.uiViewSkillList,            "Source.Scripts.World.Skills.UI",           "UIViewSkillList");

    ResolveClass(A.unityComponent,             "UnityEngine",                              "Component");
    ResolveClass(A.unityTransform,             "UnityEngine",                              "Transform");

    // Methods ─────────────────────────────────────────────────────────────
    ResolveMethod(A.EM_get_Player,              A.entitiesManager, "EntitiesManager.get_Player",   "get_Player",  0);

    ResolveMethod(A.Player_GetMoveSpeed,        A.playerMoveController, "PlayerMoveController.GetMoveSpeed", "GetMoveSpeed", 0);
    ResolveMethod(A.Unit_get_MoveSpeedMultiplier, A.unit, "Unit.get_MoveSpeedMultiplier", "get_MoveSpeedMultiplier", 0);
    ResolveMethod(A.Unit_IsHostile,             A.unit, "Unit.IsHostile",                "IsHostile", 1);
    ResolveMethod(A.Unit_GetStatAmount,         A.unit, "Unit.GetStatAmount",            "GetStatAmount", 1);
    ResolveMethod(A.Unit_IsAlive,               A.unit, "Unit.IsAlive",                  "IsAlive",   0);
    ResolveMethod(A.Unit_IsDead,                A.unit, "Unit.IsDead",                   "IsDead",    0);
    ResolveMethod(A.Unit_get_IsInCombat,        A.unit, "Unit.get_IsInCombat",           "get_IsInCombat", 0);
    ResolveMethod(A.Unit_get_CombatComponent,   A.unit, "Unit.get_CombatComponent",      "get_CombatComponent", 0);
    ResolveMethod(A.Unit_get_MovementComponent, A.unit, "Unit.get_MovementComponent",    "get_MovementComponent", 0);
    ResolveMethod(A.Npc_IsHostile,              A.npc,  "Npc.IsHostile",                 "IsHostile", 1);
    ResolveMethod(A.MasterAnim_get_SpeedMultiplier, A.masterAnimController,
                  "MasterAnimatorController.get_SpeedMultiplier", "get_SpeedMultiplier", 0);

    ResolveMethod(A.CSM_GetRaceLocked,          A.characterSelectionManager,
                  "CharacterSelectionManager.GetRaceLocked",       "GetRaceLocked", 1);
    ResolveMethod(A.CSM_SetRaceLock,            A.characterSelectionManager,
                  "CharacterSelectionManager.SetRaceLock",         "SetRaceLock",   1);
    ResolveMethod(A.CSM_SendUnlockedRacesRequest, A.characterSelectionManager,
                  "CharacterSelectionManager.SendUnlockedRacesRequest", "SendUnlockedRacesRequest", 0);

    ResolveMethod(A.ClassSO_IsLockedForRace,    A.classSO,
                  "ClassSO.IsLockedForRace",                       "IsLockedForRace", 1);
    ResolveMethod(A.RaceModelSO_get_Locked,     A.raceModelSO,
                  "RaceModelSO.get_Locked",                        "get_Locked", 0);
    ResolveMethod(A.ClassButton_CheckIsLockedForRace, A.classButton,
                  "ClassButton.CheckIsLockedForRace",              "CheckIsLockedForRace", -1);
    ResolveMethod(A.ClassButton_IsInteractable, A.classButton,
                  "ClassButton.IsInteractable",                    "IsInteractable", 0);
    ResolveMethod(A.RaceToggle_IsInteractable,  A.raceToggle,
                  "RaceToggle.IsInteractable",                     "IsInteractable", 0);

    ResolveMethod(A.IM_CheckForInteract,        A.interactionManager,
                  "InteractionManager.CheckForInteract",           "CheckForInteract", 1);
    ResolveMethod(A.Interaction_IsAvailableForPlayer, A.interaction,
                  "Interaction.IsAvailableForPlayer",              "IsAvailableForPlayer", 1);

    ResolveMethod(A.SCR_GetCooldownReductionMs, A.spellCooldownResolver,
                  "SpellCooldownReductionResolver.GetCooldownReductionMilliseconds",
                  "GetCooldownReductionMilliseconds", 2);

    ResolveMethod(A.PSLV_IsLockedByRank,        A.playerSkillListView,
                  "PlayerSkillListView.IsLockedByRank",            "IsLockedByRank", 1);
    ResolveMethod(A.UVSCV_IsLockedByRank,       A.uiViewSkillCategoriesView,
                  "UIViewSkillCategoriesView.IsLockedByRank",      "IsLockedByRank", 1);
    ResolveMethod(A.UVSL_IsLockedByRank,        A.uiViewSkillList,
                  "UIViewSkillList.IsLockedByRank",                "IsLockedByRank", 1);

    ResolveMethod(A.Component_get_transform,    A.unityComponent, "Component.get_transform", "get_transform", 0);
    ResolveMethod(A.Transform_get_position,     A.unityTransform, "Transform.get_position",  "get_position",  0);
    ResolveMethod(A.Transform_set_position,     A.unityTransform, "Transform.set_position",  "set_position",  1);

    // Tally diagnostics ───────────────────────────────────────────────────
    ClassSym* classes[] = {
        &A.entitiesManager, &A.player, &A.botPlayer, &A.unit, &A.npc, &A.mount,
        &A.playerMoveController, &A.localPlayerMoveController, &A.remotePlayerMoveController,
        &A.playerCameraController, &A.playerCombatController, &A.playerInputController,
        &A.playerMountController, &A.basicAttackController, &A.masterAnimController,
        &A.movementComponent, &A.combatComponent, &A.unitStats, &A.resourceNode,
        &A.characterSelectionManager, &A.classSO, &A.raceModelSO, &A.classButton, &A.raceToggle,
        &A.interactionManager, &A.interaction, &A.spellCooldownResolver,
        &A.playerSkillListView, &A.uiViewSkillCategoriesView, &A.uiViewSkillList,
        &A.unityComponent, &A.unityTransform,
    };
    MethodSym* methods[] = {
        &A.EM_get_Player, &A.Player_GetMoveSpeed, &A.Unit_get_MoveSpeedMultiplier,
        &A.Unit_IsHostile, &A.Npc_IsHostile, &A.MasterAnim_get_SpeedMultiplier,
        &A.Unit_GetStatAmount, &A.Unit_IsAlive, &A.Unit_IsDead,
        &A.Unit_get_IsInCombat, &A.Unit_get_CombatComponent, &A.Unit_get_MovementComponent,
        &A.CSM_GetRaceLocked, &A.CSM_SetRaceLock, &A.CSM_SendUnlockedRacesRequest,
        &A.ClassSO_IsLockedForRace, &A.RaceModelSO_get_Locked,
        &A.ClassButton_CheckIsLockedForRace, &A.ClassButton_IsInteractable,
        &A.RaceToggle_IsInteractable,
        &A.IM_CheckForInteract, &A.Interaction_IsAvailableForPlayer,
        &A.SCR_GetCooldownReductionMs,
        &A.PSLV_IsLockedByRank, &A.UVSCV_IsLockedByRank, &A.UVSL_IsLockedByRank,
        &A.Component_get_transform, &A.Transform_get_position, &A.Transform_set_position,
    };

    A.classesTotal   = (int)(sizeof(classes) / sizeof(classes[0]));
    A.methodsTotal   = (int)(sizeof(methods) / sizeof(methods[0]));
    A.classesResolved = 0;
    A.methodsResolved = 0;
    for (auto* c : classes) if (c->cls) A.classesResolved++;
    for (auto* m : methods) if (m->mi)  A.methodsResolved++;
    A.resolved = true;

    LOGI("game_api: resolved %d/%d classes, %d/%d methods",
         A.classesResolved, A.classesTotal,
         A.methodsResolved, A.methodsTotal);

    return A.classesResolved > 0;
}

void Reresolve() {
    // Wipe non-offset slots and re-run.
    GameAPI fresh{};
    // Preserve known field offsets.
    fresh.fld_EM_Player                     = g_api.fld_EM_Player;
    fresh.fld_Player_MovementModifier       = g_api.fld_Player_MovementModifier;
    fresh.fld_Player_BaseMovementSpeed      = g_api.fld_Player_BaseMovementSpeed;
    fresh.fld_Player_CurrentMovementSpeed   = g_api.fld_Player_CurrentMovementSpeed;
    fresh.fld_Player_JumpHeight             = g_api.fld_Player_JumpHeight;
    fresh.fld_Player_JumpSpeed              = g_api.fld_Player_JumpSpeed;
    fresh.fld_Movement_SpeedModifier        = g_api.fld_Movement_SpeedModifier;
    fresh.fld_Movement_BaseMovementSpeed    = g_api.fld_Movement_BaseMovementSpeed;
    fresh.fld_Movement_CurrentMovementSpeed = g_api.fld_Movement_CurrentMovementSpeed;
    fresh.fld_Node_MiniGameType             = g_api.fld_Node_MiniGameType;
    fresh.fld_Node_IsDead                   = g_api.fld_Node_IsDead;
    fresh.fld_Node_Percentage               = g_api.fld_Node_Percentage;
    g_api = fresh;
    Resolve();
}

} // namespace gameapi
