// ════════════════════════════════════════════════════════════════════════════
// il2cpp_helpers.cpp
// ════════════════════════════════════════════════════════════════════════════
#include "il2cpp_helpers.h"
#include "renderer.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <string>

namespace il2cpp_helpers {

using namespace il2cpp;

// ─────────────────────────────────────────────────────────────────────────────
// Caches
// ─────────────────────────────────────────────────────────────────────────────
struct ClassKey {
    std::string ns;
    std::string name;
    bool operator==(const ClassKey& o) const { return ns == o.ns && name == o.name; }
};
struct ClassKeyHash {
    size_t operator()(const ClassKey& k) const {
        return std::hash<std::string>{}(k.ns) ^ (std::hash<std::string>{}(k.name) << 1);
    }
};

static std::unordered_map<ClassKey, Il2CppClass*, ClassKeyHash> g_classCache;

// Lazy refs to common UnityEngine classes
static Il2CppClass* g_clsCamera     = nullptr;
static const MethodInfo* g_mGetCameraMain = nullptr;
static const MethodInfo* g_mWorldToScreen = nullptr;
static Il2CppClass* g_clsObject     = nullptr; // UnityEngine.Object
static const MethodInfo* g_mFindObjectsOfType = nullptr;
static Il2CppClass* g_clsType       = nullptr; // System.Type
static const MethodInfo* g_mGetTypeFromHandle = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
bool Init() {
    auto& a = GetApi();
    if (!a.IsReady()) return false;
    if (Il2CppDomain* d = a.domain_get(); d && a.thread_attach) a.thread_attach(d);
    return true;
}

Il2CppClass* FindClass(const char* ns, const char* name) {
    if (!ns) ns = "";
    ClassKey key{ ns, name };
    if (auto it = g_classCache.find(key); it != g_classCache.end()) return it->second;

    auto& a = GetApi();
    if (!a.domain_get_assemblies || !a.assembly_get_image || !a.class_from_name) return nullptr;

    Il2CppDomain* dom = a.domain_get();
    if (!dom) return nullptr;
    size_t n = 0;
    const Il2CppAssembly** asms = a.domain_get_assemblies(dom, &n);
    Il2CppClass* found = nullptr;
    for (size_t i = 0; i < n && !found; i++) {
        const Il2CppImage* img = a.assembly_get_image(asms[i]);
        if (!img) continue;
        found = a.class_from_name(img, ns, name);
    }
    g_classCache[key] = found;
    return found;
}

const MethodInfo* GetMethod(Il2CppClass* k, const char* name, int argc) {
    if (!k || !name) return nullptr;
    auto& a = GetApi();
    if (a.class_get_method_from_name && argc >= 0) {
        if (const MethodInfo* m = a.class_get_method_from_name(k, name, argc)) return m;
    }
    if (!a.class_get_methods || !a.method_get_name) return nullptr;
    void* iter = nullptr;
    while (const MethodInfo* m = a.class_get_methods(k, &iter)) {
        const char* mn = a.method_get_name(m);
        if (!mn || strcmp(mn, name) != 0) continue;
        if (argc < 0) return m;
        uint32_t pc = a.method_get_param_count ? a.method_get_param_count(m) : 0;
        if ((int)pc == argc) return m;
    }
    return nullptr;
}

FieldInfo* GetField(Il2CppClass* k, const char* name) {
    if (!k || !name) return nullptr;
    auto& a = GetApi();
    if (a.class_get_field_from_name) return a.class_get_field_from_name(k, name);
    if (!a.class_get_fields || !a.field_get_name) return nullptr;
    void* iter = nullptr;
    while (FieldInfo* f = a.class_get_fields(k, &iter)) {
        const char* fn = a.field_get_name(f);
        if (fn && strcmp(fn, name) == 0) return f;
    }
    return nullptr;
}

// ── Properties ─────────────────────────────────────────────────────────────
static const PropertyInfo* FindProperty(Il2CppClass* k, const char* name) {
    if (!k || !name) return nullptr;
    auto& a = GetApi();
    if (!a.class_get_properties || !a.property_get_name) return nullptr;
    void* iter = nullptr;
    while (const PropertyInfo* p = a.class_get_properties(k, &iter)) {
        const char* pn = a.property_get_name(p);
        if (pn && strcmp(pn, name) == 0) return p;
    }
    return nullptr;
}

const MethodInfo* GetPropertyGet(Il2CppClass* k, const char* name) {
    auto& a = GetApi();
    const PropertyInfo* p = FindProperty(k, name);
    if (!p || !a.property_get_get) return nullptr;
    return a.property_get_get(p);
}

const MethodInfo* GetPropertySet(Il2CppClass* k, const char* name) {
    auto& a = GetApi();
    const PropertyInfo* p = FindProperty(k, name);
    if (!p || !a.property_get_set) return nullptr;
    return a.property_get_set(p);
}

// Used by GetPropertyValue<T> template (defined in header)
void* __il2cpp_unbox(void* boxed) {
    if (!boxed) return nullptr;
    auto& a = GetApi();
    return a.object_unbox ? a.object_unbox(boxed) : boxed;
}

void* GetPropertyObject(void* instance, const MethodInfo* getter, bool unbox) {
    if (!getter) return nullptr;
    void* res = Invoke(getter, instance, nullptr);
    if (!res) return nullptr;
    return unbox ? __il2cpp_unbox(res) : res;
}

void* Invoke(const MethodInfo* m, void* obj, void** args) {
    auto& a = GetApi();
    if (!m || !a.runtime_invoke) return nullptr;
    void* exc = nullptr;
    __try {
        return a.runtime_invoke(m, obj, args, &exc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera
// ─────────────────────────────────────────────────────────────────────────────
static void EnsureCameraResolved() {
    if (g_mGetCameraMain && g_mWorldToScreen) return;
    g_clsCamera = FindClass("UnityEngine", "Camera");
    if (!g_clsCamera) return;
    g_mGetCameraMain  = GetMethod(g_clsCamera, "get_main", 0);
    // WorldToScreenPoint(Vector3) and WorldToScreenPoint(Vector3, MonoOrStereoscopicEye).
    // Prefer the 1-arg overload.
    g_mWorldToScreen  = GetMethod(g_clsCamera, "WorldToScreenPoint", 1);
}

void* CameraMain() {
    EnsureCameraResolved();
    if (!g_mGetCameraMain) return nullptr;
    return Invoke(g_mGetCameraMain, nullptr, nullptr);
}

bool WorldToScreen(void* camera, const Vec3& worldPos, Vec2& outScreen) {
    EnsureCameraResolved();
    if (!camera || !g_mWorldToScreen) return false;

    // Vector3 is a value-type struct; il2cpp_runtime_invoke expects a pointer
    // to the boxed/unboxed value.  Passing the raw struct address works for
    // value-type parameters in IL2CPP.
    Vec3 in = worldPos;
    void* args[1] = { &in };
    void* boxed = Invoke(g_mWorldToScreen, camera, args);
    if (!boxed) return false;

    // Boxed Vector3 -> unbox -> read x,y,z
    auto& a = GetApi();
    void* raw = a.object_unbox ? a.object_unbox(boxed) : boxed;
    if (!raw) return false;

    Vec3 s = *reinterpret_cast<Vec3*>(raw);
    if (s.z <= 0.0f) return false; // behind camera

    int vw = 0, vh = 0;
    renderer::GetViewportSize(vw, vh);
    if (vh <= 0) return false;

    // Unity returns origin at bottom-left.  Flip Y for ImGui (top-left).
    outScreen.x = s.x;
    outScreen.y = (float)vh - s.y;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Find objects
// ─────────────────────────────────────────────────────────────────────────────
static void EnsureFindResolved() {
    if (g_mFindObjectsOfType && g_mGetTypeFromHandle) return;
    g_clsObject = FindClass("UnityEngine", "Object");
    g_clsType   = FindClass("System", "Type");
    if (g_clsObject) g_mFindObjectsOfType = GetMethod(g_clsObject, "FindObjectsOfType", 1);
    if (g_clsType)   g_mGetTypeFromHandle = GetMethod(g_clsType,   "GetTypeFromHandle", 1);
}

// Build a System.Type from an Il2CppClass.  Preferred path uses
// il2cpp_class_get_type + il2cpp_type_get_object (the public API).  As a
// fallback we try the offset-based read used by older code; offsets vary by
// Unity version so we try a couple of likely candidates.
static void* MakeSystemType(Il2CppClass* cls) {
    if (!cls) return nullptr;
    auto& a = GetApi();
    EnsureFindResolved();

    // Path 1: clean public API.
    if (a.class_get_type && a.type_get_object) {
        if (const Il2CppType* t = a.class_get_type(cls)) {
            if (void* sysType = a.type_get_object(t)) return sysType;
        }
    }

    // Path 2: type_get_object + offset-based Il2CppType* read.
    if (a.type_get_object && g_mGetTypeFromHandle) {
        for (size_t off : { (size_t)0xF8, (size_t)0xC8, (size_t)0xD0, (size_t)0xE0,
                            (size_t)0xE8, (size_t)0xF0, (size_t)0x100 }) {
            const Il2CppType* t = nullptr;
            __try {
                t = *reinterpret_cast<const Il2CppType* const*>(
                        reinterpret_cast<const char*>(cls) + off);
            } __except (EXCEPTION_EXECUTE_HANDLER) { t = nullptr; }
            if (!t) continue;
            if (void* sysType = a.type_get_object(t)) return sysType;
        }
    }

    // Path 3: GetTypeFromHandle.
    if (g_mGetTypeFromHandle) {
        for (size_t off : { (size_t)0xF8, (size_t)0xC8, (size_t)0xD0, (size_t)0xE0,
                            (size_t)0xE8, (size_t)0xF0, (size_t)0x100 }) {
            const Il2CppType* t = nullptr;
            __try {
                t = *reinterpret_cast<const Il2CppType* const*>(
                        reinterpret_cast<const char*>(cls) + off);
            } __except (EXCEPTION_EXECUTE_HANDLER) { t = nullptr; }
            if (!t) continue;
            void* args[1] = { (void*)&t };
            if (void* sysType = Invoke(g_mGetTypeFromHandle, nullptr, args)) return sysType;
        }
    }
    return nullptr;
}

void* FindObjectsOfType(Il2CppClass* unityType, uint32_t& outCount) {
    outCount = 0;
    EnsureFindResolved();
    if (!g_mFindObjectsOfType) return nullptr;
    void* sysType = MakeSystemType(unityType);
    if (!sysType) return nullptr;
    void* args[1] = { sysType };
    void* arr = Invoke(g_mFindObjectsOfType, nullptr, args);
    if (!arr) return nullptr;
    auto& a = GetApi();
    if (a.array_length) outCount = a.array_length(arr);
    return arr;
}

void* FindFirstObjectOfType(Il2CppClass* unityType) {
    uint32_t n = 0;
    void* arr = FindObjectsOfType(unityType, n);
    if (!arr || n == 0) return nullptr;
    // IL2CPP arrays: header is 0x20 bytes on x64, then elements.
    void** elems = reinterpret_cast<void**>(reinterpret_cast<char*>(arr) + 0x20);
    return elems[0];
}

} // namespace il2cpp_helpers

