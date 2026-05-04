// ════════════════════════════════════════════════════════════════════════════
// il2cpp_api.h  -  Minimal IL2CPP API surface resolved dynamically from
// GameAssembly.dll.  No headers from Unity required.
// ════════════════════════════════════════════════════════════════════════════
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Opaque IL2CPP types - we only ever pass these by pointer.
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppType;
struct FieldInfo;
struct MethodInfo;
struct PropertyInfo;

// Field flag bits (subset of Mono FIELD_ATTRIBUTE_*)
constexpr uint32_t IL2CPP_FIELD_ATTRIBUTE_STATIC   = 0x10;
constexpr uint32_t IL2CPP_FIELD_ATTRIBUTE_LITERAL  = 0x40;

// Method flag bits
constexpr uint32_t IL2CPP_METHOD_ATTRIBUTE_STATIC       = 0x0010;
constexpr uint32_t IL2CPP_METHOD_ATTRIBUTE_VIRTUAL      = 0x0040;
constexpr uint32_t IL2CPP_METHOD_ATTRIBUTE_ABSTRACT     = 0x0400;
constexpr uint32_t IL2CPP_METHOD_ATTRIBUTE_SPECIAL_NAME = 0x0800;

namespace il2cpp {

// Function pointer typedefs
using Pfn_thread_attach     = void*       (*)(Il2CppDomain*);
using Pfn_domain_get        = Il2CppDomain* (*)();
using Pfn_domain_get_assemblies = const Il2CppAssembly** (*)(const Il2CppDomain*, size_t*);
using Pfn_assembly_get_image    = const Il2CppImage* (*)(const Il2CppAssembly*);
using Pfn_image_get_name        = const char* (*)(const Il2CppImage*);
using Pfn_image_get_class_count = size_t      (*)(const Il2CppImage*);
using Pfn_image_get_class       = Il2CppClass* (*)(const Il2CppImage*, size_t);

using Pfn_class_get_name        = const char* (*)(Il2CppClass*);
using Pfn_class_get_namespace   = const char* (*)(Il2CppClass*);
using Pfn_class_get_parent      = Il2CppClass* (*)(Il2CppClass*);
using Pfn_class_get_flags       = uint32_t   (*)(const Il2CppClass*);
using Pfn_class_is_enum         = bool       (*)(const Il2CppClass*);
using Pfn_class_is_valuetype    = bool       (*)(const Il2CppClass*);
using Pfn_class_instance_size   = int32_t    (*)(Il2CppClass*);
using Pfn_class_num_fields      = int        (*)(const Il2CppClass*);
using Pfn_class_get_fields      = FieldInfo* (*)(Il2CppClass*, void**);
using Pfn_class_get_methods     = const MethodInfo* (*)(Il2CppClass*, void**);
using Pfn_class_get_properties  = const PropertyInfo* (*)(Il2CppClass*, void**);
using Pfn_class_get_interfaces  = Il2CppClass* (*)(Il2CppClass*, void**);
using Pfn_class_from_name       = Il2CppClass* (*)(const Il2CppImage*, const char*, const char*);

using Pfn_field_get_name        = const char* (*)(FieldInfo*);
using Pfn_field_get_type        = const Il2CppType* (*)(FieldInfo*);
using Pfn_field_get_offset      = size_t     (*)(FieldInfo*);
using Pfn_field_get_flags       = int        (*)(FieldInfo*);

using Pfn_method_get_name       = const char* (*)(const MethodInfo*);
using Pfn_method_get_return_type= const Il2CppType* (*)(const MethodInfo*);
using Pfn_method_get_param_count= uint32_t   (*)(const MethodInfo*);
using Pfn_method_get_param_type = const Il2CppType* (*)(const MethodInfo*, uint32_t);
using Pfn_method_get_param      = const Il2CppType* (*)(const MethodInfo*, uint32_t); // Unity 2021.2+
using Pfn_method_get_param_name = const char* (*)(const MethodInfo*, uint32_t);
using Pfn_method_get_flags      = uint32_t   (*)(const MethodInfo*, uint32_t*);
using Pfn_method_get_pointer    = void*      (*)(const MethodInfo*);

using Pfn_type_get_name         = char*      (*)(const Il2CppType*);
using Pfn_type_get_object       = void*      (*)(const Il2CppType*);
using Pfn_class_get_type        = const Il2CppType* (*)(Il2CppClass*);

using Pfn_property_get_name     = const char* (*)(const PropertyInfo*);
using Pfn_property_get_get      = const MethodInfo* (*)(const PropertyInfo*);
using Pfn_property_get_set      = const MethodInfo* (*)(const PropertyInfo*);

// ── Extra runtime helpers used by overlay / W2S ─────────────────────────────
using Pfn_class_get_method_from_name = const MethodInfo* (*)(Il2CppClass*, const char*, int);
using Pfn_class_get_field_from_name  = FieldInfo* (*)(Il2CppClass*, const char*);
using Pfn_field_static_get_value     = void  (*)(FieldInfo*, void*);
using Pfn_field_get_value            = void  (*)(void* obj, FieldInfo*, void*);
using Pfn_runtime_invoke             = void* (*)(const MethodInfo*, void* obj, void** params, void** exc);
using Pfn_string_new                 = void* (*)(const char*);
using Pfn_string_chars               = wchar_t* (*)(void*);
using Pfn_string_length              = int (*)(void*);
using Pfn_object_get_class           = Il2CppClass* (*)(void*);
using Pfn_object_unbox               = void* (*)(void*);
using Pfn_array_length               = uint32_t (*)(void*);
using Pfn_array_element_size         = uint32_t (*)(const Il2CppClass*);

struct Api {
    HMODULE                       hMod = nullptr;

    Pfn_thread_attach             thread_attach = nullptr;
    Pfn_domain_get                domain_get = nullptr;
    Pfn_domain_get_assemblies     domain_get_assemblies = nullptr;
    Pfn_assembly_get_image        assembly_get_image = nullptr;
    Pfn_image_get_name            image_get_name = nullptr;
    Pfn_image_get_class_count     image_get_class_count = nullptr;
    Pfn_image_get_class           image_get_class = nullptr;

    Pfn_class_get_name            class_get_name = nullptr;
    Pfn_class_get_namespace       class_get_namespace = nullptr;
    Pfn_class_get_parent          class_get_parent = nullptr;
    Pfn_class_get_flags           class_get_flags = nullptr;
    Pfn_class_is_enum             class_is_enum = nullptr;
    Pfn_class_is_valuetype        class_is_valuetype = nullptr;
    Pfn_class_instance_size       class_instance_size = nullptr;
    Pfn_class_num_fields          class_num_fields = nullptr;
    Pfn_class_get_fields          class_get_fields = nullptr;
    Pfn_class_get_methods         class_get_methods = nullptr;
    Pfn_class_get_properties      class_get_properties = nullptr;
    Pfn_class_get_interfaces      class_get_interfaces = nullptr;
    Pfn_class_from_name           class_from_name = nullptr;

    Pfn_field_get_name            field_get_name = nullptr;
    Pfn_field_get_type            field_get_type = nullptr;
    Pfn_field_get_offset          field_get_offset = nullptr;
    Pfn_field_get_flags           field_get_flags = nullptr;

    Pfn_method_get_name           method_get_name = nullptr;
    Pfn_method_get_return_type    method_get_return_type = nullptr;
    Pfn_method_get_param_count    method_get_param_count = nullptr;
    Pfn_method_get_param_type     method_get_param_type = nullptr;
    Pfn_method_get_param          method_get_param      = nullptr;
    Pfn_method_get_param_name     method_get_param_name = nullptr;
    Pfn_method_get_flags          method_get_flags = nullptr;
    Pfn_method_get_pointer        method_get_pointer = nullptr;

    Pfn_type_get_name             type_get_name = nullptr;
    Pfn_type_get_object           type_get_object = nullptr;
    Pfn_class_get_type            class_get_type = nullptr;

    Pfn_property_get_name         property_get_name = nullptr;
    Pfn_property_get_get          property_get_get = nullptr;
    Pfn_property_get_set          property_get_set = nullptr;

    Pfn_class_get_method_from_name class_get_method_from_name = nullptr;
    Pfn_class_get_field_from_name  class_get_field_from_name = nullptr;
    Pfn_field_static_get_value     field_static_get_value = nullptr;
    Pfn_field_get_value            field_get_value = nullptr;
    Pfn_runtime_invoke             runtime_invoke = nullptr;
    Pfn_string_new                 string_new = nullptr;
    Pfn_string_chars               string_chars = nullptr;
    Pfn_string_length              string_length = nullptr;
    Pfn_object_get_class           object_get_class = nullptr;
    Pfn_object_unbox               object_unbox = nullptr;
    Pfn_array_length               array_length = nullptr;
    Pfn_array_element_size         array_element_size = nullptr;

    bool Resolve(HMODULE gameAssembly);
    bool IsReady() const;
};

Api& GetApi();

} // namespace il2cpp
