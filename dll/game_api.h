// ════════════════════════════════════════════════════════════════════════════
// game_api.h  -  Centralised IL2CPP symbol registry for Scars of Honor.
//
//   Everything we know how to talk to in the game (classes, methods, field
//   offsets) lives in this one registry.  It is *runtime-resolved* on init via
//   il2cpp_helpers (FindClass + GetMethod) so it survives game updates as long
//   as the symbol still exists.
//
//   Design
//   ──────
//     gameapi::Resolve()                  // one-shot at startup
//     gameapi::G().player.cls             // Il2CppClass*
//     gameapi::G().player.fld_MoveSpeed   // size_t offset (from dump)
//     gameapi::G().playerMoveCtl.GetMoveSpeed  // const MethodInfo*
//
//   Add a new symbol → bump exactly one line in the struct + game_api.cpp.
//
//   The registry also feeds the in-game "Game API" inspector tab so you can
//   see in real time what resolved, what didn't, and the live pointers.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include "il2cpp_api.h"
#include <cstdint>
#include <cstddef>

namespace gameapi {

// One resolved symbol, with diagnostic info for the inspector UI.
struct ClassSym {
    const char*     ns        = "";
    const char*     name      = "";
    Il2CppClass*    cls       = nullptr;       // null = unresolved
    uint32_t        sizeBytes = 0;             // best-effort from il2cpp
    uint32_t        rva       = 0;             // 0 = unknown
};

struct MethodSym {
    const char*       label   = "";
    const char*       name    = "";
    int               argc    = -1;            // -1 = ignore
    const MethodInfo* mi      = nullptr;
    void*             ptr     = nullptr;       // resolved methodPointer
    uint32_t          rva     = 0;
};

// ── Bag of every symbol we want to talk to ────────────────────────────────
struct GameAPI {
    // Singletons / managers
    ClassSym entitiesManager;          // Entities.EntitiesManager
    MethodSym EM_get_Player;
    size_t    fld_EM_Player = 0xA0;

    // Player + player-related controllers
    ClassSym player;                   // Entities.Player                size=0x2A8
    ClassSym botPlayer;                // Entities.BotPlayer             size=0x210
    ClassSym unit;                     // Entities.Unit (base)
    ClassSym npc;                      // Entities.Npc
    ClassSym mount;                    // Entities.Mount                 size=0x2C0

    ClassSym playerMoveController;          // Entities.PlayerMoveController       size=0xE8
    ClassSym localPlayerMoveController;     // Entities.LocalPlayerMoveController  size=0x118
    ClassSym remotePlayerMoveController;    // Entities.RemotePlayerMoveController size=0x188
    ClassSym playerCameraController;        // Entities.PlayerCameraController     size=0x1A0
    ClassSym playerCombatController;        // Entities.PlayerCombatController     size=0xA0
    ClassSym playerInputController;         // Entities.PlayerInputController      size=0x90
    ClassSym playerMountController;         // Entities.PlayerMountController      size=0x90
    ClassSym basicAttackController;         // Entities.PlayerBasicAttackController size=0x40

    MethodSym Player_GetMoveSpeed;          // (none → Single)   from PlayerMoveController really
    MethodSym Unit_get_MoveSpeedMultiplier; // get_MoveSpeedMultiplier
    MethodSym Unit_IsHostile;               // IsHostile(Unit)
    MethodSym Npc_IsHostile;                // IsHostile(Unit)
    MethodSym MasterAnim_get_SpeedMultiplier;
    ClassSym  masterAnimController;         // Entities.MasterAnimatorController

    // Live stats (Unit base) -- HP/Mana/regen/etc.
    MethodSym Unit_GetStatAmount;           // (Defines.Stat) -> Single
    MethodSym Unit_IsAlive;                 // () -> bool
    MethodSym Unit_IsDead;                  // () -> bool
    MethodSym Unit_get_IsInCombat;          // () -> bool
    MethodSym Unit_get_CombatComponent;     // () -> CombatComponent
    MethodSym Unit_get_MovementComponent;   // () -> MovementComponent

    // Player field offsets (verified from Code.Core dump)
    size_t fld_Player_MovementModifier      = 0x180;
    size_t fld_Player_BaseMovementSpeed     = 0x184;
    size_t fld_Player_CurrentMovementSpeed  = 0x188;
    size_t fld_Player_JumpHeight            = 0x1A8;
    size_t fld_Player_JumpSpeed             = 0x1AC;

    // Movement component
    ClassSym movementComponent;             // World.Components.MovementComponent  size=0xC8
    size_t fld_Movement_SpeedModifier       = 0x50;
    size_t fld_Movement_BaseMovementSpeed   = 0x54;
    size_t fld_Movement_CurrentMovementSpeed= 0x58;

    // Combat / stats
    ClassSym combatComponent;               // World.Components.CombatComponent
    ClassSym unitStats;                     // World.Components.UnitStats (best-effort)

    // Resource nodes (mining / gathering / mini-games)
    ClassSym resourceNode;                  // World.MiniGame.ResourceNodePrefabController size=0xF8
    size_t fld_Node_MiniGameType            = 0xF0;
    size_t fld_Node_IsDead                  = 0xF1;
    size_t fld_Node_Percentage              = 0xC8;

    // Character / race / class selection (Race & Class unlock surface)
    ClassSym characterSelectionManager;     // CharacterSelectionManager     size=0x140
    MethodSym CSM_GetRaceLocked;            // (Defines.GameRace)->bool
    MethodSym CSM_SetRaceLock;              // (Defines.GameRace)->void
    MethodSym CSM_SendUnlockedRacesRequest; // ()->void

    ClassSym  classSO;                      // ModelManagerNamespace.ClassSO
    MethodSym ClassSO_IsLockedForRace;      // (Defines.GameRace)->bool

    ClassSym  raceModelSO;                  // ModelManagerNamespace.RaceModelSO
    MethodSym RaceModelSO_get_Locked;       // ()->bool

    ClassSym  classButton;                  // UI.CharacterSelection.ClassButton
    MethodSym ClassButton_CheckIsLockedForRace;
    MethodSym ClassButton_IsInteractable;

    ClassSym  raceToggle;                   // UI.CharacterSelection.RaceToggle
    MethodSym RaceToggle_IsInteractable;

    // Interaction surface
    ClassSym  interactionManager;           // InteractionManager           size=0x138
    MethodSym IM_CheckForInteract;          // (VEGameObject)->bool

    ClassSym  interaction;                  // Interaction
    MethodSym Interaction_IsAvailableForPlayer; // (Player)->bool

    // Spells / skills
    ClassSym  spellCooldownResolver;        // Source.Scripts.World.Spells.SpellCooldownReductionResolver
    MethodSym SCR_GetCooldownReductionMs;   // static (uint, CombatComponent)->uint

    ClassSym  playerSkillListView;
    MethodSym PSLV_IsLockedByRank;
    ClassSym  uiViewSkillCategoriesView;
    MethodSym UVSCV_IsLockedByRank;
    ClassSym  uiViewSkillList;
    MethodSym UVSL_IsLockedByRank;

    // Unity-side helpers (cached for ESP / position reads)
    ClassSym  unityComponent;               // UnityEngine.Component
    MethodSym Component_get_transform;
    ClassSym  unityTransform;               // UnityEngine.Transform
    MethodSym Transform_get_position;
    MethodSym Transform_set_position;

    // Counters populated by Resolve() for the inspector banner
    int  classesResolved   = 0,  classesTotal   = 0;
    int  methodsResolved   = 0,  methodsTotal   = 0;
    bool resolved          = false;
};

// Read-only access from anywhere in the codebase.
const GameAPI& G();

// Resolve everything once IL2CPP is ready.  Cheap to call again; will only
// re-resolve symbols that are still null.
bool Resolve();

// Re-resolve forcibly (e.g. after the game reloaded its assemblies).
void Reresolve();

// Returns the GameAssembly.dll base address (cached) or nullptr.
uint8_t* GameAsmBase();

// Convenience: convert a methodPointer to a GameAssembly RVA, or 0 on miss.
uint32_t RvaOf(const void* p);

} // namespace gameapi
