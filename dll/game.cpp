// ════════════════════════════════════════════════════════════════════════════
// game.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "game.h"
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <windows.h>

namespace game {

using namespace il2cpp_helpers;
using namespace il2cpp;

static Refs g_refs;

// Caches — FindObjectsOfType is extremely expensive (linear scan over every
// live Unity Object), so we resolve singletons at most once every N ms.
static constexpr DWORD kCacheMs = 1500;
static void*  g_cachedEM       = nullptr;
static DWORD  g_cachedEMTick   = 0;
static void*  g_cachedPlayer   = nullptr;
static DWORD  g_cachedPlayerTick = 0;

const Refs& GetRefs() { return g_refs; }

bool Resolve() {
    if (g_refs.resolved) return true;
    g_refs.Player               = FindClass("Entities", "Player");
    g_refs.EntitiesManager      = FindClass("Entities", "EntitiesManager");
    g_refs.MovementComponent    = FindClass("World.Components", "MovementComponent");
    g_refs.NodePrefabController = FindClass("World.MiniGame", "ResourceNodePrefabController");
    g_refs.Npc                  = FindClass("Entities", "Npc");
    g_refs.Component            = FindClass("UnityEngine", "Component");
    g_refs.Transform            = FindClass("UnityEngine", "Transform");
    if (g_refs.Component) g_refs.Component_get_transform = GetMethod(g_refs.Component, "get_transform", 0);
    if (g_refs.Transform) g_refs.Transform_get_position  = GetMethod(g_refs.Transform, "get_position", 0);

    // Prefer the IL2CPP property metadata so we don't depend on a hardcoded
    // backing-field offset.
    if (g_refs.EntitiesManager) g_refs.EM_get_Player = GetPropertyGet(g_refs.EntitiesManager, "Player");

    g_refs.resolved = g_refs.Player && g_refs.EntitiesManager &&
                      g_refs.Component_get_transform && g_refs.Transform_get_position;
    return g_refs.resolved;
}

const char* MiniGameTypeName(int v) {
    switch (v) {
        case 0: return "None";
        case 1: return "Mining";
        case 2: return "Woodcutting";
        case 3: return "Fishing";
        case 4: return "Crafting";
        case 5: return "Cooking";
        case 6: return "Alchemy";
        default: return "?";
    }
}

// ── Singletons ─────────────────────────────────────────────────────────────
void* EntitiesManagerInst() {
    DWORD now = GetTickCount();
    // Negative-cache too: if we were null less than kCacheMs ago, don't
    // re-scan. FindObjectsOfType is the single most expensive call we make.
    if ((now - g_cachedEMTick) < kCacheMs) return g_cachedEM;
    Resolve();
    g_cachedEMTick = now;
    if (!g_refs.EntitiesManager) { g_cachedEM = nullptr; return nullptr; }
    g_cachedEM = FindFirstObjectOfType(g_refs.EntitiesManager);
    return g_cachedEM;
}

void* MainPlayer() {
    DWORD now = GetTickCount();
    if ((now - g_cachedPlayerTick) < kCacheMs) return g_cachedPlayer;
    Resolve();
    g_cachedPlayerTick = now;
    void* em = EntitiesManagerInst();
    void* p = nullptr;
    if (em && g_refs.EM_get_Player) p = Invoke(g_refs.EM_get_Player, em, nullptr);
    if (!p && em) p = *reinterpret_cast<void**>(reinterpret_cast<char*>(em) + off::EM_Player);
    if (!p && g_refs.Player) p = FindFirstObjectOfType(g_refs.Player);
    g_cachedPlayer = p;
    return p;
}

bool GetTransformPosition(void* comp, Vec3& out) {
    Resolve();
    if (!comp || !g_refs.Component_get_transform || !g_refs.Transform_get_position) return false;
    void* tr = Invoke(g_refs.Component_get_transform, comp, nullptr);
    if (!tr) return false;
    void* boxed = Invoke(g_refs.Transform_get_position, tr, nullptr);
    if (!boxed) return false;
    auto& a = GetApi();
    void* raw = a.object_unbox ? a.object_unbox(boxed) : boxed;
    if (!raw) return false;
    out = *reinterpret_cast<Vec3*>(raw);
    return true;
}

bool GetPlayerPosition(Vec3& out) {
    void* p = MainPlayer();
    if (!p) return false;
    return GetTransformPosition(p, out);
}

// ── Speed ──────────────────────────────────────────────────────────────────
template<typename T>
static T ReadAt(void* obj, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(obj) + offset);
}
template<typename T>
static void WriteAt(void* obj, size_t offset, T value) {
    *reinterpret_cast<T*>(reinterpret_cast<char*>(obj) + offset) = value;
}

// Validate that `addr` points to readable+writable committed memory of at
// least `bytes` bytes.  Cheap (single VirtualQuery), avoids the IsBad* APIs
// which deadlock under guard pages.
static bool IsValidWritable(const void* addr, size_t bytes) {
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD okMask = PAGE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & okMask) == 0) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    // Make sure the range we want doesn't cross the region's tail.
    auto base = reinterpret_cast<const char*>(mbi.BaseAddress);
    auto end  = base + mbi.RegionSize;
    auto want = reinterpret_cast<const char*>(addr) + bytes;
    return want <= end;
}

// Heuristic: a live IL2CPP managed object has a valid Il2CppClass* in its
// first 8 bytes pointing into GameAssembly.dll's image.  If that fails, the
// pointer is stale (player despawned / scene changed) and we must NOT write.
static bool LooksLikeManagedObject(void* obj) {
    if (!IsValidWritable(obj, 0x40)) return false;
    void* klass = nullptr;
    __try { klass = *reinterpret_cast<void**>(obj); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!IsValidWritable(klass, 0x10)) return false;
    return true;
}

// Public alias — exposed so feature code can validate cached pointers.
bool IsLikelyAlive(void* managedObj) {
    // Reject Unity's freed-slot poison bit pattern outright (cheap fast path).
    if (!managedObj) return false;
    if (reinterpret_cast<uintptr_t>(managedObj) == 0xFFFFFFFFFFFFFFFFull) return false;
    return LooksLikeManagedObject(managedObj);
}

static bool SafeWriteFloat(void* obj, size_t offset, float value) {
    void* addr = reinterpret_cast<char*>(obj) + offset;
    if (!IsValidWritable(addr, sizeof(float))) return false;
    __try { *reinterpret_cast<float*>(addr) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeReadFloat(void* obj, size_t offset, float& out) {
    void* addr = reinterpret_cast<char*>(obj) + offset;
    if (!IsValidWritable(addr, sizeof(float))) return false;
    __try { out = *reinterpret_cast<float*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadSpeedSnapshot(SpeedTweak& out) {
    void* p = MainPlayer();
    if (!LooksLikeManagedObject(p)) return false;
    bool ok = true;
    ok &= SafeReadFloat(p, off::Player_BaseMovementSpeed,    out.baseSpeed);
    ok &= SafeReadFloat(p, off::Player_CurrentMovementSpeed, out.currentSpeed);
    ok &= SafeReadFloat(p, off::Player_MovementModifier,     out.modifier);
    ok &= SafeReadFloat(p, off::Player_JumpHeight,           out.jumpHeight);
    return ok;
}

// Clamp slider values to safe ranges so a stray text-edit (e.g. user typing
// 1e30 in CTRL+click input) can't NaN the player's locomotion.
static float ClampSpeed(float v)  { if (!_finite(v)) return 0.f; if (v < 0.f) return 0.f; if (v > 100.f) return 100.f; return v; }
static float ClampMod(float v)    { if (!_finite(v)) return 1.f; if (v < 0.f) return 0.f; if (v > 50.f)  return 50.f;  return v; }
static float ClampJump(float v)   { if (!_finite(v)) return 0.f; if (v < 0.f) return 0.f; if (v > 50.f)  return 50.f;  return v; }

void ApplySpeedTweaks(const SpeedTweak& t) {
    if (!t.lockBase && !t.lockCurrent && !t.lockMod && !t.lockJump) return;
    void* p = MainPlayer();
    if (!LooksLikeManagedObject(p)) return;
    if (t.lockBase)    SafeWriteFloat(p, off::Player_BaseMovementSpeed,    ClampSpeed(t.baseSpeed));
    if (t.lockCurrent) SafeWriteFloat(p, off::Player_CurrentMovementSpeed, ClampSpeed(t.currentSpeed));
    if (t.lockMod)     SafeWriteFloat(p, off::Player_MovementModifier,     ClampMod  (t.modifier));
    if (t.lockJump)    SafeWriteFloat(p, off::Player_JumpHeight,           ClampJump (t.jumpHeight));
}

// ── Enumeration ────────────────────────────────────────────────────────────
static void EnumerateOfType(Il2CppClass* cls, int max,
                            std::vector<WorldEntity>& out,
                            void(*fill)(void*, WorldEntity&)) {
    out.clear();
    if (!cls) return;
    uint32_t n = 0;
    // Use the shared TTL cache so multiple ESP features asking for the same
    // type within a frame don't each pay for a full FindObjectsOfType.
    void* arr = il2cpp_helpers::FindObjectsOfTypeCached(cls, n, 250);
    if (!arr || n == 0) return;
    void** elems = reinterpret_cast<void**>(reinterpret_cast<char*>(arr) + 0x20);
    int limit = (int)n < max ? (int)n : max;
    out.reserve(limit);
    for (int i = 0; i < limit; i++) {
        void* o = elems[i];
        if (!IsLikelyAlive(o)) continue;   // skip use-after-free poisoned slots
        WorldEntity we{};
        we.obj = o;
        if (!GetTransformPosition(o, we.worldPos)) continue;
        if (fill) fill(o, we);
        out.push_back(we);
    }
}

static void FillNode(void* o, WorldEntity& we) {
    we.miniGameType = ReadAt<int>(o, off::Node_MiniGameType);
    we.isDead       = ReadAt<bool>(o, off::Node_IsDead);
    we.percentage   = ReadAt<float>(o, off::Node_Percentage);
    _snprintf_s(we.label, sizeof(we.label), _TRUNCATE,
                "%s%s", MiniGameTypeName(we.miniGameType),
                we.isDead ? " (dead)" : "");
}

void EnumerateNodes(std::vector<WorldEntity>& out, int max) {
    Resolve();
    EnumerateOfType(g_refs.NodePrefabController, max, out, FillNode);
}

void EnumerateNpcs(std::vector<WorldEntity>& out, int max) {
    Resolve();
    EnumerateOfType(g_refs.Npc, max, out, [](void*, WorldEntity& we) {
        std::strcpy(we.label, "NPC");
    });
}

void EnumeratePlayers(std::vector<WorldEntity>& out, int max) {
    Resolve();
    EnumerateOfType(g_refs.Player, max, out, [](void*, WorldEntity& we) {
        std::strcpy(we.label, "Player");
    });
}

} // namespace game
