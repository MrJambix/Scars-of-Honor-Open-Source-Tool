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

    // Resources (HP/Mana/Energy/etc.) on the player Unit
    ClassSym resourcesComponent;            // World.Components.ResourcesComponent  size=0x78
    size_t fld_RC_resources                  = 0x50;  // List<Defines.ResourceStruct>
    size_t fld_RC_guid                       = 0x68;  // ulong owner guid
    size_t fld_RC_unit                       = 0x70;  // Entities.Unit owner
    // ResourceStruct value-type element layout (stride 0x10 inside T[] storage)
    size_t resourceStruct_stride             = 0x10;
    size_t fld_Resource_id                   = 0x00;  // byte (Defines.ResourceID)
    size_t fld_Resource_current              = 0x04;  // uint32
    size_t fld_Resource_max                  = 0x08;  // uint32
    size_t fld_Resource_orderId              = 0x0C;  // byte
    size_t fld_Resource_isActive             = 0x0D;  // bool

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
    MethodSym IM_get_InteractableComponentInRange; // ()->InteractionComponent
    MethodSym IM_InteractionObjectHit;      // (Player, Vector2)->InteractionComponent

    ClassSym  interactionComponent;         // InteractionComponent       size=0x108
    MethodSym IC_InteractStart;             // (uint interactionId, ulong unitGuid)->void
    size_t    fld_IC_interactions = 0xB0;   // List<Interaction>
    size_t    fld_IC_ownerObject  = 0xC8;   // Entities.WorldObject

    ClassSym  interaction;                  // Interaction               size=0x70
    MethodSym Interaction_IsAvailableForPlayer; // (Player)->bool
    MethodSym Interaction_GetId;            // ()->uint
    size_t    fld_Interaction_id  = 0x10;   // first field of InteractionData blob

    ClassSym  spellUtilities;               // SpellUtilities (static helpers)
    MethodSym SU_get_InteractionManager;    // static ()->InteractionManager

    ClassSym  veNode;                       // VibraniumEngine.Nodes.VENode
    MethodSym VENode_GetGuid;               // ()->ulong  (player guid lives here)
    size_t    fld_VENode_guid  = 0x38;

    // Spells / skills
    ClassSym  spellsDatabase;               // SpellsDatabase
    MethodSym SpellsDatabase_get_Spells;    // ()->List<SpellTemplate>
    ClassSym  spellTemplate;                // Source.Scripts.World.Spells.SpellTemplate  size=0xF8
    // Verified offsets on SpellTemplate
    size_t    fld_Spell_id              = 0x10;
    size_t    fld_Spell_max_range       = 0x28;  // float
    size_t    fld_Spell_channel_time    = 0x38;  // ms (UInt32)
    size_t    fld_Spell_cast_time       = 0x3C;  // ms (UInt32)
    size_t    fld_Spell_cooldown        = 0x44;  // ms
    size_t    fld_Spell_anim_lock_delay = 0x48;  // ms
    size_t    fld_Spell_globalcooldown  = 0x4C;  // ms

    // Generic IL2CPP container offsets (System.Collections.Generic.List + T[])
    size_t    fld_List_items = 0x10;        // T[] backing array
    size_t    fld_List_size  = 0x18;        // int count
    size_t    fld_Array_data = 0x20;        // first element of vector<T>

    ClassSym  spellCooldownResolver;        // Source.Scripts.World.Spells.SpellCooldownReductionResolver
    MethodSym SCR_GetCooldownReductionMs;   // static (uint, CombatComponent)->uint

    // Talents (UnlimitedTalentPoints)
    ClassSym  talentsModelSO;               // Talents.UI.UiDataTalentsModelSO  size=0x38
    size_t    fld_Talents_available = 0x20; // UInt32
    size_t    fld_Talents_spent     = 0x24; // UInt32
    size_t    fld_Talents_refund    = 0x28; // UInt32

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
