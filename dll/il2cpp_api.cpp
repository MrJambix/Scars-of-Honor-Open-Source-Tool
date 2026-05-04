#include "il2cpp_api.h"
#include <cstdio>

namespace il2cpp {

static Api g_api;
Api& GetApi() { return g_api; }

#define RESOLVE(name) do { \
        name = reinterpret_cast<Pfn_##name>(GetProcAddress(gameAssembly, "il2cpp_" #name)); \
        if (!name) { printf("  [il2cpp] missing export: il2cpp_%s\n", #name); ok = false; } \
    } while (0)

bool Api::Resolve(HMODULE gameAssembly) {
    hMod = gameAssembly;
    bool ok = true;

    RESOLVE(thread_attach);
    RESOLVE(domain_get);
    RESOLVE(domain_get_assemblies);
    RESOLVE(assembly_get_image);
    RESOLVE(image_get_name);
    RESOLVE(image_get_class_count);
    RESOLVE(image_get_class);

    RESOLVE(class_get_name);
    RESOLVE(class_get_namespace);
    RESOLVE(class_get_parent);
    RESOLVE(class_get_flags);
    RESOLVE(class_is_enum);
    RESOLVE(class_is_valuetype);
    RESOLVE(class_instance_size);
    RESOLVE(class_num_fields);
    RESOLVE(class_get_fields);
    RESOLVE(class_get_methods);
    RESOLVE(class_get_properties);
    RESOLVE(class_get_interfaces);
    RESOLVE(class_from_name);

    RESOLVE(field_get_name);
    RESOLVE(field_get_type);
    RESOLVE(field_get_offset);
    RESOLVE(field_get_flags);

    RESOLVE(method_get_name);
    RESOLVE(method_get_return_type);
    RESOLVE(method_get_param_count);
    // method_get_param_type was removed in Unity 2021.2 in favour of
    // method_get_param.  Try both - whichever the host exports wins.
    {
        bool savedOk2 = ok;
        method_get_param_type = reinterpret_cast<Pfn_method_get_param_type>(
            GetProcAddress(gameAssembly, "il2cpp_method_get_param_type"));
        method_get_param = reinterpret_cast<Pfn_method_get_param>(
            GetProcAddress(gameAssembly, "il2cpp_method_get_param"));
        if (!method_get_param_type && !method_get_param) {
            printf("  [il2cpp] missing param-type export (will use struct fallback)\n");
        }
        ok = savedOk2;
    }
    RESOLVE(method_get_param_name);
    RESOLVE(method_get_flags);
    RESOLVE(method_get_pointer);

    RESOLVE(type_get_name);

    // Optional but very useful: convert Il2CppClass -> Il2CppType -> System.Type
    {
        bool savedOk3 = ok;
        type_get_object = reinterpret_cast<Pfn_type_get_object>(
            GetProcAddress(gameAssembly, "il2cpp_type_get_object"));
        class_get_type  = reinterpret_cast<Pfn_class_get_type>(
            GetProcAddress(gameAssembly, "il2cpp_class_get_type"));
        if (!type_get_object) printf("  [il2cpp] missing export: il2cpp_type_get_object\n");
        if (!class_get_type)  printf("  [il2cpp] missing export: il2cpp_class_get_type\n");
        ok = savedOk3;
    }

    RESOLVE(property_get_name);
    // Property accessors: il2cpp historically exposed *_get/_set; modern
    // builds export *_get_method/*_set_method.  Try both.
    property_get_get = reinterpret_cast<Pfn_property_get_get>(
        GetProcAddress(gameAssembly, "il2cpp_property_get_get_method"));
    if (!property_get_get)
        property_get_get = reinterpret_cast<Pfn_property_get_get>(
            GetProcAddress(gameAssembly, "il2cpp_property_get_get"));
    property_get_set = reinterpret_cast<Pfn_property_get_set>(
        GetProcAddress(gameAssembly, "il2cpp_property_get_set_method"));
    if (!property_get_set)
        property_get_set = reinterpret_cast<Pfn_property_get_set>(
            GetProcAddress(gameAssembly, "il2cpp_property_get_set"));
    if (!property_get_get) printf("  [il2cpp] missing export: il2cpp_property_get_get[_method]\n");
    if (!property_get_set) printf("  [il2cpp] missing export: il2cpp_property_get_set[_method]\n");

    // Optional helpers (overlay / W2S).  Failure here is non-fatal.
    bool savedOk = ok;
    RESOLVE(class_get_method_from_name);
    RESOLVE(class_get_field_from_name);
    RESOLVE(field_static_get_value);
    RESOLVE(field_get_value);
    RESOLVE(runtime_invoke);
    RESOLVE(string_new);
    RESOLVE(string_chars);
    RESOLVE(string_length);
    RESOLVE(object_get_class);
    RESOLVE(object_unbox);
    RESOLVE(array_length);
    RESOLVE(array_element_size);
    ok = savedOk; // these missing only disables overlay features

    return ok;
}

bool Api::IsReady() const {
    return hMod && domain_get && domain_get_assemblies && assembly_get_image
        && image_get_class_count && image_get_class && class_get_name;
}

} // namespace il2cpp
