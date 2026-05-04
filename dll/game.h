// ════════════════════════════════════════════════════════════════════════════
// game.h  -  High-level wrappers around Scars-specific IL2CPP classes.
//   Resolves singletons (EntitiesManager, MainPlayer) and exposes typed
//   accessors for fields we tweak in the dev overlay.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include "il2cpp_helpers.h"
#include <vector>
#include <cstdint>

namespace game {

// ── Field offsets (Code.Core.dll, Standalone.0.03.1.5170.staging) ──────────
// Verified from the v2 dump (pointers + RVAs populated).
namespace off {
    // Entities.Player : Unit : MonoBehaviour            size=0x2A8
    constexpr size_t Player_MovementModifier      = 0x180;
    constexpr size_t Player_BaseMovementSpeed     = 0x184;
    constexpr size_t Player_CurrentMovementSpeed  = 0x188;
    constexpr size_t Player_JumpHeight            = 0x1A8;
    constexpr size_t Player_JumpSpeed             = 0x1AC;

    // World.Components.MovementComponent : VEComponent  size=0xC8
    constexpr size_t Movement_SpeedModifier       = 0x50;
    constexpr size_t Movement_BaseMovementSpeed   = 0x54;
    constexpr size_t Movement_CurrentMovementSpeed= 0x58;

    // Entities.EntitiesManager : MonoBehaviour          size=0xC0
    constexpr size_t EM_Player                    = 0xA0;

    // World.MiniGame.ResourceNodePrefabController       size=0xF8
    constexpr size_t Node_MiniGameType            = 0xF0;
    constexpr size_t Node_IsDead                  = 0xF1;
    constexpr size_t Node_Percentage              = 0xC8;
}

// MiniGameType enum values (best-effort; verify from dump if unsure)
enum class MiniGameType : int {
    None = 0, Mining, Woodcutting, Fishing, Crafting, Cooking, Alchemy
};
const char* MiniGameTypeName(int v);

// ── Resolved class refs (lazy) ─────────────────────────────────────────────
struct Refs {
    Il2CppClass* Player              = nullptr;
    Il2CppClass* EntitiesManager     = nullptr;
    Il2CppClass* MovementComponent   = nullptr;
    Il2CppClass* NodePrefabController= nullptr;
    Il2CppClass* Npc                 = nullptr;
    Il2CppClass* Component           = nullptr;
    Il2CppClass* Transform           = nullptr;
    const MethodInfo* Component_get_transform = nullptr;
    const MethodInfo* Transform_get_position  = nullptr;
    // Property getters resolved via il2cpp_property_get_get_method
    const MethodInfo* EM_get_Player           = nullptr;
    const MethodInfo* Unit_get_Position       = nullptr;
    bool resolved = false;
};

const Refs& GetRefs();
bool        Resolve();          // call once IL2CPP is up

// ── Entity accessors ───────────────────────────────────────────────────────
void* MainPlayer();             // Entities.Player* or null
void* EntitiesManagerInst();    // Entities.EntitiesManager* or null

bool  GetPlayerPosition(Vec3& out);
bool  GetTransformPosition(void* unityComponent, Vec3& out);

// ── Speed control ──────────────────────────────────────────────────────────
struct SpeedTweak {
    bool  lockBase    = false;  bool  lockCurrent = false;
    bool  lockMod     = false;
    float baseSpeed   = 4.0f;   float currentSpeed = 4.0f;
    float modifier    = 1.0f;
    float jumpHeight  = 1.5f;   bool  lockJump    = false;
};
void ApplySpeedTweaks(const SpeedTweak& t);   // writes to Player every call
bool ReadSpeedSnapshot(SpeedTweak& out);      // populate sliders from current values

// ── Object enumeration ─────────────────────────────────────────────────────
struct WorldEntity {
    void*      obj      = nullptr;   // raw IL2CPP MonoBehaviour pointer
    Vec3       worldPos {};
    int        miniGameType = 0;     // for nodes
    bool       isDead   = false;
    float      percentage = 0.0f;
    char       label[64] {};
};

void EnumerateNodes(std::vector<WorldEntity>& out, int max = 256);
void EnumerateNpcs (std::vector<WorldEntity>& out, int max = 256);
void EnumeratePlayers(std::vector<WorldEntity>& out, int max = 64);

} // namespace game
