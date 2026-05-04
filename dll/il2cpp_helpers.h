// ════════════════════════════════════════════════════════════════════════════
// il2cpp_helpers.h  -  High-level wrappers around the resolved IL2CPP API.
//   * Find class by namespace+name across all assemblies (cached)
//   * Get method / field by name (cached)
//   * Camera.main + WorldToScreenPoint
//   * Find first live MonoBehaviour by type
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#include "il2cpp_api.h"
#include <string>

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

namespace il2cpp_helpers {

bool          Init();                    // attach current thread, prime caches
Il2CppClass*  FindClass(const char* ns, const char* name);
const MethodInfo* GetMethod(Il2CppClass* k, const char* name, int argc = -1);
FieldInfo*    GetField(Il2CppClass* k, const char* name);

// ── Property helpers ───────────────────────────────────────────────────────
// Resolve a property by name and return its getter / setter MethodInfo.
const MethodInfo* GetPropertyGet(Il2CppClass* k, const char* name);
const MethodInfo* GetPropertySet(Il2CppClass* k, const char* name);

// Read a property as a raw object pointer (handles boxed value types).
// Pass `unbox=true` for value-type properties (returns pointer to inline data).
void* GetPropertyObject(void* instance, const MethodInfo* getter, bool unbox = false);

// Read a property of a primitive value type (int/float/etc) into `out`.
template <typename T>
bool GetPropertyValue(void* instance, const MethodInfo* getter, T& out);

// Set a property by invoking its setter with a single primitive argument.
template <typename T>
bool SetPropertyValue(void* instance, const MethodInfo* setter, T value);

void* Invoke(const MethodInfo* m, void* obj, void** args = nullptr);

// Template implementations
template <typename T>
bool GetPropertyValue(void* instance, const MethodInfo* getter, T& out) {
    void* boxed = Invoke(getter, instance, nullptr);
    if (!boxed) return false;
    // For primitive value types runtime_invoke returns a boxed object.
    // unbox to get a pointer to the inline data.
    extern void* __il2cpp_unbox(void*);
    void* raw = __il2cpp_unbox(boxed);
    if (!raw) return false;
    out = *reinterpret_cast<T*>(raw);
    return true;
}

template <typename T>
bool SetPropertyValue(void* instance, const MethodInfo* setter, T value) {
    if (!setter) return false;
    void* args[1] = { (void*)&value };
    Invoke(setter, instance, args);
    return true;
}

// Returns the active main camera (UnityEngine.Camera) instance pointer or null.
void* CameraMain();

// Returns true if (worldPos) is in front of camera and writes screen px.
// origin = bottom-left in Unity convention; returned with origin top-left,
// matching ImGui's screen coordinate system.
bool  WorldToScreen(void* camera, const Vec3& worldPos, Vec2& outScreen);

// FindObjectsOfType<T>() - returns the IL2CPP array, plus its element count.
// Pass the System.Type representation via UnityEngine.Object.FindObjectsOfType.
// Caller must NOT free the array.
void* FindObjectsOfType(Il2CppClass* unityType, uint32_t& outCount);

// Cached variant: returns the most recent FindObjectsOfType<T> result if it
// is younger than ttlMs, otherwise re-queries.  This is the recommended
// entry point for per-frame / per-tick consumers since several pipelines
// usually want the same type within a few ms of each other.
void* FindObjectsOfTypeCached(Il2CppClass* unityType, uint32_t& outCount,
                              unsigned long ttlMs = 200);

// Read the first instance of the given Unity component type, or null.
void* FindFirstObjectOfType(Il2CppClass* unityType);

} // namespace il2cpp_helpers
