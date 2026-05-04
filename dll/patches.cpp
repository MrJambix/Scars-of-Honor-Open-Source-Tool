// ════════════════════════════════════════════════════════════════════════════
// patches.cpp  -  Runtime symbol-resolved binary patcher pipeline.
//
//   Patches target IL2CPP methods *by name* (namespace.class.method/argc), not
//   by hardcoded RVA, so they survive game updates as long as the symbol still
//   exists.  Each patch:
//
//     1. Resolves the live address via il2cpp_helpers (FindClass + GetMethod)
//        + MethodInfo->methodPointer.  Logs the live address + computed RVA.
//     2. Verifies the page is committed + executable.
//     3. Captures original bytes, writes the patch payload through
//        VirtualProtect(PAGE_EXECUTE_READWRITE).
//     4. FlushInstructionCache.
//     5. Toggling OFF restores originals; on detach, every applied patch is
//        auto-restored in OnShutdown().
// ════════════════════════════════════════════════════════════════════════════
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <cstring>
#include <vector>
#include <string>

#include "pipeline.h"
#include "log.h"
#include "il2cpp_api.h"
#include "il2cpp_helpers.h"
#include "vendor/imgui/imgui.h"

using namespace il2cpp_helpers;
using il2cpp::GetApi;

namespace {

// ── Patch payloads ────────────────────────────────────────────────────────
// RET_FALSE: xor eax,eax ; ret
// RET_TRUE : mov al,1    ; ret
// RET      : ret
// RET_FLT  : mov rax,&g_speedValue ; movss xmm0,[rax] ; ret  (return *g_speedValue)
static const uint8_t kRetFalse[] = { 0x31, 0xC0, 0xC3 };
static const uint8_t kRetTrue [] = { 0xB0, 0x01, 0xC3 };
static const uint8_t kRet     [] = { 0xC3 };
// mov eax, 0xFFFFFFFF ; ret    (return UINT32_MAX, used to swallow cooldowns)
static const uint8_t kRetU32Max[] = { 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3 };

// Live speed value the GetMoveSpeed stub reads from.  Updated frame-by-frame
// from the UI slider; the patched function returns whatever this holds at the
// moment of the call.  Aligned to a cache line for clean atomic float writes.
alignas(64) static float g_speedValue = 7.0f;
static float            g_speedDefault = 7.0f; // captured first apply (best effort)

// Builder: emits a 15-byte stub at `out`:
//   48 B8 <imm64>          movabs rax, &g_speedValue
//   F3 0F 10 00            movss  xmm0, dword ptr [rax]
//   C3                     ret
static void BuildSpeedStub(uint8_t out[15], const float* src) {
    out[0] = 0x48; out[1] = 0xB8;
    uint64_t addr = reinterpret_cast<uint64_t>(src);
    memcpy(out + 2, &addr, sizeof(addr));
    out[10] = 0xF3; out[11] = 0x0F; out[12] = 0x10; out[13] = 0x00;
    out[14] = 0xC3;
}
// Placeholder bytes used only so PatchSpec table compiles; real bytes are
// generated at apply time for the speed group.
static uint8_t kSpeedStub[15] = { 0x48,0xB8,0,0,0,0,0,0,0,0, 0xF3,0x0F,0x10,0x00, 0xC3 };

// ── Patch site (resolved by name) ─────────────────────────────────────────
struct PatchSpec {
    const char*    label;
    const char*    ns;       // IL2CPP namespace
    const char*    klass;    // class name
    const char*    method;   // method name
    int            argc;     // -1 = ignore (pick first match)
    const uint8_t* bytes;
    size_t         size;
};

struct PatchGroup {
    const char* id;
    const char* name;
    const char* desc;
    std::vector<PatchSpec> specs;
};

// Per-instance live state
struct LiveState {
    uint8_t*             addr      = nullptr; // resolved live pointer
    uint32_t             rva       = 0;
    bool                 resolved  = false;
    bool                 applied   = false;
    bool                 verifyFail = false;
    char                 errMsg[96] = {};
    std::vector<uint8_t> original; // bytes captured before first apply
};

// ── Patch table ───────────────────────────────────────────────────────────
static const PatchGroup g_groups[] = {
    {
        "class_unlock",
        "Class Unlocker",
        "Unlock all classes (Warrior/Paladin) for every race.",
        {
            { "ClassSO.IsLockedForRace -> false",
              "ModelManagerNamespace", "ClassSO", "IsLockedForRace", 1,
              kRetFalse, sizeof(kRetFalse) },
            { "ClassButton.CheckIsLockedForRace -> false",
              "UI.CharacterSelection", "ClassButton", "CheckIsLockedForRace", -1,
              kRetFalse, sizeof(kRetFalse) },
            { "ClassButton.IsInteractable -> true",
              "UI.CharacterSelection", "ClassButton", "IsInteractable", 0,
              kRetTrue, sizeof(kRetTrue) },
        }
    },
    {
        "race_unlock",
        "Race Unlocker",
        "Unlock all races (High Elf, Orc, Meon, ...).",
        {
            { "CharacterSelectionManager.GetRaceLocked -> false",
              "", "CharacterSelectionManager", "GetRaceLocked", 1,
              kRetFalse, sizeof(kRetFalse) },
            { "RaceModelSO.get_Locked -> false",
              "ModelManagerNamespace", "RaceModelSO", "get_Locked", 0,
              kRetFalse, sizeof(kRetFalse) },
            { "RaceToggle.IsInteractable -> true",
              "UI.CharacterSelection", "RaceToggle", "IsInteractable", 0,
              kRetTrue, sizeof(kRetTrue) },
            { "CharacterSelectionManager.SetRaceLock -> nop",
              "", "CharacterSelectionManager", "SetRaceLock", 1,
              kRet, sizeof(kRet) },
            { "CharacterSelectionManager.SendUnlockedRacesRequest -> nop",
              "", "CharacterSelectionManager", "SendUnlockedRacesRequest", 0,
              kRet, sizeof(kRet) },
        }
    },
    {
        "movespeed_live",
        "Move Speed Override",
        "PlayerMoveController.GetMoveSpeed() returns a value you control live.",
        {
            { "PlayerMoveController.GetMoveSpeed -> *g_speedValue",
              "Entities", "PlayerMoveController", "GetMoveSpeed", 0,
              kSpeedStub, sizeof(kSpeedStub) },
        }
    },
    {
        "no_cooldowns",
        "No Spell Cooldowns",
        "Force SpellCooldownReductionResolver to return UINT32_MAX, "
        "so the resulting cooldown is always clamped to zero.",
        {
            { "SpellCooldownReductionResolver.GetCooldownReductionMilliseconds -> UINT32_MAX",
              "Source.Scripts.World.Spells", "SpellCooldownReductionResolver",
              "GetCooldownReductionMilliseconds", 2,
              kRetU32Max, sizeof(kRetU32Max) },
        }
    },
    {
        "skill_rank_unlock",
        "Unlock Rank-Locked Skills",
        "Make all rank-locked skills appear unlocked in the skill book / "
        "skill list / category view.",
        {
            { "PlayerSkillListView.IsLockedByRank -> false",
              "Source.Scripts.World.Skills.UI", "PlayerSkillListView",
              "IsLockedByRank", 1,
              kRetFalse, sizeof(kRetFalse) },
            { "UIViewSkillCategoriesView.IsLockedByRank -> false",
              "Source.Scripts.World.Skills.UI", "UIViewSkillCategoriesView",
              "IsLockedByRank", 1,
              kRetFalse, sizeof(kRetFalse) },
            { "UIViewSkillList.IsLockedByRank -> false",
              "Source.Scripts.World.Skills.UI", "UIViewSkillList",
              "IsLockedByRank", 1,
              kRetFalse, sizeof(kRetFalse) },
        }
    },
    {
        "always_interact",
        "Always Can Interact",
        "InteractionManager.CheckForInteract -> true and "
        "Interaction.IsAvailableForPlayer -> true.",
        {
            { "InteractionManager.CheckForInteract -> true",
              "", "InteractionManager", "CheckForInteract", 1,
              kRetTrue, sizeof(kRetTrue) },
            { "Interaction.IsAvailableForPlayer -> true",
              "", "Interaction", "IsAvailableForPlayer", 1,
              kRetTrue, sizeof(kRetTrue) },
        }
    },
};

// Group id of the movespeed override (index into g_groups). We special-case
// it during apply to inject the live address of g_speedValue.
static constexpr size_t kSpeedGroupIdx = 2;

static std::vector<std::vector<LiveState>> g_state;

// ── GameAssembly base ────────────────────────────────────────────────────
static uint8_t* GameAsmBase() {
    static uint8_t* cached = nullptr;
    if (cached) return cached;
    cached = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GameAssembly.dll"));
    return cached;
}

// ── Memory protection helpers ────────────────────────────────────────────
static bool LooksExecutable(const void* addr, size_t bytes) {
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & execMask) == 0) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    auto end  = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    auto want = reinterpret_cast<const uint8_t*>(addr) + bytes;
    return want <= end;
}

static bool WriteBytes(uint8_t* dst, const uint8_t* src, size_t n,
                       std::vector<uint8_t>& original) {
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    __try {
        if (original.empty()) original.assign(dst, dst + n);
        memcpy(dst, src, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD tmp = 0; VirtualProtect(dst, n, oldProt, &tmp);
        return false;
    }
    DWORD tmp = 0;
    VirtualProtect(dst, n, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

static bool RestoreBytes(uint8_t* dst, const std::vector<uint8_t>& original) {
    if (original.empty()) return true;
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, original.size(), PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    __try {
        memcpy(dst, original.data(), original.size());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD tmp = 0; VirtualProtect(dst, original.size(), oldProt, &tmp);
        return false;
    }
    DWORD tmp = 0;
    VirtualProtect(dst, original.size(), oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, original.size());
    return true;
}

// ── Symbol resolution ────────────────────────────────────────────────────
static void* MethodPointerFromMI(const MethodInfo* m) {
    if (!m) return nullptr;
    auto& a = GetApi();
    void* p = a.method_get_pointer ? a.method_get_pointer(m) : nullptr;
    if (p) return p;
    __try { return *reinterpret_cast<void* const*>(m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void Resolve(LiveState& st, const PatchSpec& s) {
    st.resolved = false;
    st.errMsg[0] = 0;
    Il2CppClass* k = FindClass(s.ns, s.klass);
    if (!k) {
        _snprintf_s(st.errMsg, sizeof(st.errMsg), _TRUNCATE,
                    "class %s.%s not found", s.ns, s.klass);
        return;
    }
    const MethodInfo* m = GetMethod(k, s.method, s.argc);
    if (!m) {
        _snprintf_s(st.errMsg, sizeof(st.errMsg), _TRUNCATE,
                    "method %s(argc=%d) not found", s.method, s.argc);
        return;
    }
    void* p = MethodPointerFromMI(m);
    if (!p) {
        _snprintf_s(st.errMsg, sizeof(st.errMsg), _TRUNCATE,
                    "methodPointer null for %s", s.method);
        return;
    }
    st.addr     = reinterpret_cast<uint8_t*>(p);
    uint8_t* base = GameAsmBase();
    st.rva      = base ? (uint32_t)(st.addr - base) : 0;
    st.resolved = true;
}

// ── Group ops ────────────────────────────────────────────────────────────
static void EnsureStateSized() {
    if (g_state.size() == _countof(g_groups)) return;
    g_state.resize(_countof(g_groups));
    for (size_t i = 0; i < _countof(g_groups); i++)
        g_state[i].resize(g_groups[i].specs.size());
}

static void ResolveAll() {
    EnsureStateSized();
    for (size_t i = 0; i < _countof(g_groups); i++) {
        for (size_t k = 0; k < g_groups[i].specs.size(); k++) {
            LiveState& st = g_state[i][k];
            if (!st.resolved) Resolve(st, g_groups[i].specs[k]);
        }
    }
}

static int ApplyGroup(size_t gi) {
    EnsureStateSized();
    int ok = 0;
    const PatchGroup& g = g_groups[gi];
    for (size_t i = 0; i < g.specs.size(); i++) {
        const PatchSpec& s = g.specs[i];
        LiveState& st      = g_state[gi][i];
        if (!st.resolved)  Resolve(st, s);
        if (!st.resolved) {
            LOGW("patches: skip %s — %s", s.label, st.errMsg);
            continue;
        }
        if (st.applied) { ok++; continue; }
        if (!LooksExecutable(st.addr, s.size)) {
            st.verifyFail = true;
            LOGW("patches: %s: %p not executable", s.label, st.addr);
            continue;
        }
        // Special-case the live speed stub: rebuild bytes with the *runtime*
        // address of g_speedValue so the slider can update the speed without
        // re-patching the function on every change.
        uint8_t patchBuf[15];
        const uint8_t* patchBytes = s.bytes;
        if (gi == kSpeedGroupIdx) {
            BuildSpeedStub(patchBuf, &g_speedValue);
            patchBytes = patchBuf;
        }
        if (memcmp(st.addr, patchBytes, s.size) == 0) {
            st.applied = true; ok++;
            LOGI("patches: %s already in place @ %p (RVA=0x%X)",
                 s.label, st.addr, st.rva);
            continue;
        }
        if (WriteBytes(st.addr, patchBytes, s.size, st.original)) {
            st.applied = true; st.verifyFail = false; ok++;
            LOGI("patches: applied %s @ %p (RVA=0x%X)", s.label, st.addr, st.rva);
        } else {
            st.verifyFail = true;
            LOGE("patches: write FAILED for %s @ %p", s.label, st.addr);
        }
    }
    return ok;
}

static int RestoreGroup(size_t gi) {
    EnsureStateSized();
    int ok = 0;
    const PatchGroup& g = g_groups[gi];
    for (size_t i = 0; i < g.specs.size(); i++) {
        LiveState& st = g_state[gi][i];
        if (!st.applied || !st.addr) continue;
        if (RestoreBytes(st.addr, st.original)) {
            st.applied = false; ok++;
            LOGI("patches: restored %s @ %p", g.specs[i].label, st.addr);
        }
    }
    return ok;
}

static bool GroupApplied(size_t gi) {
    EnsureStateSized();
    for (auto& s : g_state[gi]) if (s.applied) return true;
    return false;
}

} // anonymous

// ── Pipeline feature ─────────────────────────────────────────────────────
class PatchesFeature : public pipeline::Feature {
public:
    PatchesFeature() : Feature("Patches", pipeline::Category::Combat, true) {}

    void OnInit() override {
        // Resolve everything once IL2CPP is up.  Cheap (one FindClass + one
        // GetMethod per patch, both cached internally).
        ResolveAll();
        int total = 0, ok = 0;
        for (auto& v : g_state) for (auto& s : v) { total++; if (s.resolved) ok++; }
        LOGI("patches: resolved %d/%d symbols", ok, total);
    }

    void OnShutdown() override {
        for (size_t i = 0; i < _countof(g_groups); i++) RestoreGroup(i);
    }

    void OnRenderUI() override {
        uint8_t* base = GameAsmBase();
        ImGui::TextDisabled("GameAssembly base = %p", base);
        if (!base) {
            ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),
                "GameAssembly.dll not loaded yet.");
            return;
        }
        EnsureStateSized();

        ImGui::TextWrapped("All patch sites resolve by class+method name.  "
                           "RVAs shown are computed live and survive game "
                           "updates as long as the symbol still exists.");
        if (ImGui::SmallButton("Re-resolve")) {
            for (auto& v : g_state) for (auto& s : v) s.resolved = false;
            ResolveAll();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply all")) {
            for (size_t i=0;i<_countof(g_groups);i++) ApplyGroup(i);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Restore all")) {
            for (size_t i=0;i<_countof(g_groups);i++) RestoreGroup(i);
        }
        ImGui::Separator();

        for (size_t i = 0; i < _countof(g_groups); i++) {
            const PatchGroup& g = g_groups[i];
            ImGui::PushID((int)i);
            bool on = GroupApplied(i);
            if (ImGui::Checkbox(g.name, &on)) {
                if (on) ApplyGroup(i); else RestoreGroup(i);
            }
            ImGui::SameLine(); ImGui::TextDisabled("%s", g.desc);

            if (ImGui::TreeNode("Sites")) {
                for (size_t k = 0; k < g.specs.size(); k++) {
                    const PatchSpec& s = g.specs[k];
                    LiveState&       st = g_state[i][k];
                    // For the live-speed group the on-disk bytes contain a
                    // runtime pointer, so just trust st.applied for "match".
                    bool match;
                    if (i == kSpeedGroupIdx) {
                        match = st.applied && st.resolved &&
                                LooksExecutable(st.addr, s.size);
                    } else {
                        match = st.resolved && LooksExecutable(st.addr, s.size) &&
                                memcmp(st.addr, s.bytes, s.size) == 0;
                    }
                    ImVec4 col = !st.resolved ? ImVec4(1,0.5f,0.5f,1) :
                                 st.verifyFail ? ImVec4(1,0.4f,0.4f,1) :
                                 match         ? ImVec4(0.4f,1,0.4f,1) :
                                                 ImVec4(0.85f,0.85f,0.85f,1);
                    if (st.resolved) {
                        ImGui::TextColored(col,
                            "%s.%s.%s/%d  ptr=%p  RVA=0x%07X  [%s]",
                            s.ns, s.klass, s.method, s.argc,
                            (void*)st.addr, st.rva,
                            st.verifyFail ? "BAD" : (match ? "ON" : "off"));
                    } else {
                        ImGui::TextColored(col,
                            "%s.%s.%s/%d  UNRESOLVED — %s",
                            s.ns, s.klass, s.method, s.argc, st.errMsg);
                    }
                }
                ImGui::TreePop();
            }
            // Live slider for the move-speed override group.  Writing to
            // g_speedValue is picked up by the patched function on the very
            // next call — no re-patch, no per-frame work.
            if (i == kSpeedGroupIdx) {
                ImGui::Indent();
                float v = g_speedValue;
                if (ImGui::SliderFloat("Speed (live)", &v, 0.1f, 100.0f, "%.2f")) {
                    if (v >= 0.0f && v <= 1000.0f) g_speedValue = v;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset 7")) g_speedValue = 7.0f;
                ImGui::SameLine();
                if (ImGui::SmallButton("25"))     g_speedValue = 25.0f;
                ImGui::SameLine();
                if (ImGui::SmallButton("50"))     g_speedValue = 50.0f;
                ImGui::TextDisabled("Live float @ %p (read every GetMoveSpeed call)",
                                    (void*)&g_speedValue);
                ImGui::Unindent();
            }
            ImGui::PopID();
            ImGui::Separator();
        }
    }
};
static PatchesFeature s_patches;
