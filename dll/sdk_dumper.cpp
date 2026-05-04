#include "sdk_dumper.h"
#include "il2cpp_api.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>

#pragma comment(lib, "shlwapi.lib")

namespace sdk_dumper {

using namespace il2cpp;

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string SafeStr(const char* s) { return s ? std::string(s) : std::string(); }

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Make an identifier safe for C++: replace anything non-alphanumeric with '_'.
static std::string CppIdent(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (!out.empty() && out[0] >= '0' && out[0] <= '9') out = "_" + out;
    if (out.empty()) out = "_";
    return out;
}

static std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static bool EnsureDir(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Best-effort recursive-ish mkdir for the timestamped subdir we create.
static bool EnsureDirChain(const std::wstring& path) {
    std::wstring acc;
    for (size_t i = 0; i < path.size(); i++) {
        wchar_t c = path[i];
        acc += c;
        if (c == L'\\' || c == L'/') {
            if (acc.size() > 3) EnsureDir(acc);
        }
    }
    return EnsureDir(path);
}

// RAII FILE* wrapper
struct FileOut {
    FILE* fp = nullptr;
    explicit FileOut(const std::wstring& path) {
        _wfopen_s(&fp, path.c_str(), L"wb");
    }
    ~FileOut() { if (fp) fclose(fp); }
    explicit operator bool() const { return fp != nullptr; }
    void Write(const char* s) { if (fp && s) fwrite(s, 1, strlen(s), fp); }
    void Write(const std::string& s) { if (fp) fwrite(s.data(), 1, s.size(), fp); }
    void Printf(const char* fmt, ...) {
        if (!fp) return;
        va_list ap; va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-class collectors
// ─────────────────────────────────────────────────────────────────────────────

struct FieldRow {
    std::string name;
    std::string type;
    size_t      offset = 0;
    int         flags  = 0;
    bool        isStatic = false;
    bool        isLiteral = false;
};

struct ParamRow {
    std::string        type;     // pretty-printed name (e.g. "System.Int32")
    std::string        name;     // formal parameter name, may be empty
    const Il2CppType*  typePtr = nullptr;  // raw pointer for tooling
};

struct MethodRow {
    std::string name;
    std::string returnType;
    const Il2CppType* returnTypePtr = nullptr;
    std::vector<ParamRow> params;
    uint32_t    flags = 0;
    bool        isStatic = false;
    bool        isVirtual = false;
    bool        isAbstract = false;
    void*       ptr = nullptr;
};

struct PropertyRow {
    std::string name;
    bool        hasGet = false;
    bool        hasSet = false;
    // Resolved getter/setter detail (when present)
    std::string getReturnType;
    void*       getPtr = nullptr;
    std::string setParamType;
    void*       setPtr = nullptr;
    bool        isStatic = false;
};

struct ClassRow {
    std::string ns;
    std::string name;
    std::string parent;
    std::string assembly;
    bool        isEnum = false;
    bool        isValueType = false;
    int32_t     instanceSize = 0;
    std::vector<FieldRow>    fields;
    std::vector<MethodRow>   methods;
    std::vector<PropertyRow> properties;
};

static char* SafeTypeGetName(const Il2CppType* t) {
    __try {
        return GetApi().type_get_name(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static std::string TypeName(const Il2CppType* t) {
    if (!t || !GetApi().type_get_name) return "?";
    char* raw = SafeTypeGetName(t);
    if (!raw) return "?";
    std::string out(raw);
    // NOTE: il2cpp_type_get_name returns memory allocated by Unity's CRT.
    // We statically link our own CRT, so we MUST NOT call free() on it -
    // that would corrupt the heap and crash the process.  We deliberately
    // leak; the dumper runs a few times at most.
    return out;
}

static void CollectFields(Il2CppClass* klass, ClassRow& row, Stats& stats) {
    auto& a = GetApi();
    if (!a.class_get_fields || !a.class_num_fields) return;
    int n = a.class_num_fields(klass);
    if (n <= 0) return;
    row.fields.reserve(n);
    void* iter = nullptr;
    while (FieldInfo* f = a.class_get_fields(klass, &iter)) {
        FieldRow fr;
        fr.name   = SafeStr(a.field_get_name ? a.field_get_name(f) : nullptr);
        fr.type   = TypeName(a.field_get_type ? a.field_get_type(f) : nullptr);
        fr.offset = a.field_get_offset ? a.field_get_offset(f) : 0;
        fr.flags  = a.field_get_flags  ? a.field_get_flags(f)  : 0;
        fr.isStatic  = (fr.flags & IL2CPP_FIELD_ATTRIBUTE_STATIC) != 0;
        fr.isLiteral = (fr.flags & IL2CPP_FIELD_ATTRIBUTE_LITERAL) != 0;
        row.fields.push_back(std::move(fr));
        stats.fields++;
    }
}

// MethodInfo on Unity 2018+/IL2CPP starts with `Il2CppMethodPointer methodPointer`
// at offset 0. il2cpp_method_get_pointer is sometimes broken or stripped, so we
// fall back to a direct read.
static void* MethodPointerOf(const MethodInfo* m) {
    auto& a = GetApi();
    void* p = a.method_get_pointer ? a.method_get_pointer(m) : nullptr;
    if (p) return p;
    if (!m) return nullptr;
    __try { return *reinterpret_cast<void* const*>(m); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Resolve the i-th parameter's Il2CppType*.  Order of attempts:
//   1. il2cpp_method_get_param        (Unity 2021.2+)
//   2. il2cpp_method_get_param_type   (Unity <= 2021.1)
//   3. Direct read of MethodInfo->parameters[i] at offset 0x28
//      (matches Il2CppMethodInfo layout in Unity 2020+).
static const Il2CppType* ParamTypeOf(const MethodInfo* m, uint32_t idx) {
    auto& a = GetApi();
    if (a.method_get_param) {
        if (auto* t = a.method_get_param(m, idx)) return t;
    }
    if (a.method_get_param_type) {
        if (auto* t = a.method_get_param_type(m, idx)) return t;
    }
    if (!m) return nullptr;
    __try {
        const Il2CppType* const* arr =
            *reinterpret_cast<const Il2CppType* const* const*>(
                reinterpret_cast<const char*>(m) + 0x28);
        if (!arr) return nullptr;
        return arr[idx];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void CollectMethods(Il2CppClass* klass, ClassRow& row, Stats& stats) {
    auto& a = GetApi();
    if (!a.class_get_methods) return;
    void* iter = nullptr;
    while (const MethodInfo* m = a.class_get_methods(klass, &iter)) {
        MethodRow mr;
        mr.name       = SafeStr(a.method_get_name ? a.method_get_name(m) : nullptr);
        const Il2CppType* rt = a.method_get_return_type ? a.method_get_return_type(m) : nullptr;
        mr.returnType = TypeName(rt);
        mr.returnTypePtr = rt;
        mr.flags      = a.method_get_flags ? a.method_get_flags(m, nullptr) : 0;
        mr.isStatic   = (mr.flags & IL2CPP_METHOD_ATTRIBUTE_STATIC)   != 0;
        mr.isVirtual  = (mr.flags & IL2CPP_METHOD_ATTRIBUTE_VIRTUAL)  != 0;
        mr.isAbstract = (mr.flags & IL2CPP_METHOD_ATTRIBUTE_ABSTRACT) != 0;
        mr.ptr        = MethodPointerOf(m);
        uint32_t pc   = a.method_get_param_count ? a.method_get_param_count(m) : 0;
        mr.params.reserve(pc);
        for (uint32_t i = 0; i < pc; i++) {
            ParamRow pr;
            pr.typePtr = ParamTypeOf(m, i);
            pr.type    = TypeName(pr.typePtr);
            pr.name    = SafeStr(a.method_get_param_name ? a.method_get_param_name(m, i) : nullptr);
            mr.params.push_back(std::move(pr));
        }
        row.methods.push_back(std::move(mr));
        stats.methods++;
    }
}

static void CollectProperties(Il2CppClass* klass, ClassRow& row, Stats& stats) {
    auto& a = GetApi();
    if (!a.class_get_properties) return;
    void* iter = nullptr;
    while (const PropertyInfo* p = a.class_get_properties(klass, &iter)) {
        PropertyRow pr;
        pr.name = SafeStr(a.property_get_name ? a.property_get_name(p) : nullptr);

        const MethodInfo* getter = a.property_get_get ? a.property_get_get(p) : nullptr;
        const MethodInfo* setter = a.property_get_set ? a.property_get_set(p) : nullptr;
        pr.hasGet = getter != nullptr;
        pr.hasSet = setter != nullptr;

        if (getter) {
            pr.getReturnType = TypeName(a.method_get_return_type ? a.method_get_return_type(getter) : nullptr);
            pr.getPtr        = MethodPointerOf(getter);
            uint32_t gf      = a.method_get_flags ? a.method_get_flags(getter, nullptr) : 0;
            pr.isStatic      = (gf & IL2CPP_METHOD_ATTRIBUTE_STATIC) != 0;
        }
        if (setter) {
            pr.setPtr = MethodPointerOf(setter);
            uint32_t pc = a.method_get_param_count ? a.method_get_param_count(setter) : 0;
            if (pc > 0) {
                pr.setParamType = TypeName(ParamTypeOf(setter, pc - 1));
            }
            if (!getter) {
                uint32_t sf = a.method_get_flags ? a.method_get_flags(setter, nullptr) : 0;
                pr.isStatic = (sf & IL2CPP_METHOD_ATTRIBUTE_STATIC) != 0;
            }
        }

        row.properties.push_back(std::move(pr));
        stats.properties++;
    }
}

// SEH-guarded shim - kept in its own function because /EHsc forbids __try in
// functions that have objects requiring unwinding.
static bool SafeCollect(Il2CppClass* klass, const std::string& asmName,
                        Stats& stats, ClassRow& out);

static Il2CppClass* SafeImageGetClass(const Il2CppImage* img, size_t ci) {
    __try {
        return GetApi().image_get_class(img, ci);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void CollectClass(Il2CppClass* klass, const std::string& asmName, Stats& stats, ClassRow& row) {
    auto& a = GetApi();
    row.assembly     = asmName;
    row.name         = SafeStr(a.class_get_name      ? a.class_get_name(klass)      : nullptr);
    row.ns           = SafeStr(a.class_get_namespace ? a.class_get_namespace(klass) : nullptr);
    if (Il2CppClass* parent = a.class_get_parent ? a.class_get_parent(klass) : nullptr) {
        row.parent = SafeStr(a.class_get_name ? a.class_get_name(parent) : nullptr);
    }
    row.isEnum       = a.class_is_enum      ? a.class_is_enum(klass)      : false;
    row.isValueType  = a.class_is_valuetype ? a.class_is_valuetype(klass) : false;
    row.instanceSize = a.class_instance_size ? a.class_instance_size(klass) : 0;
    CollectFields(klass, row, stats);
    CollectMethods(klass, row, stats);
    CollectProperties(klass, row, stats);
}

static bool SafeCollect(Il2CppClass* klass, const std::string& asmName,
                        Stats& stats, ClassRow& out) {
    __try {
        CollectClass(klass, asmName, stats, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Writers
// ─────────────────────────────────────────────────────────────────────────────

static std::string FullName(const ClassRow& c) {
    return c.ns.empty() ? c.name : (c.ns + "." + c.name);
}

// Cached GameAssembly base for RVA printing
static uintptr_t g_gaBase = 0;
static uintptr_t g_gaSize = 0;
static void EnsureGaBase() {
    if (g_gaBase) return;
    HMODULE h = GetModuleHandleW(L"GameAssembly.dll");
    if (!h) return;
    g_gaBase = reinterpret_cast<uintptr_t>(h);
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi)))
        g_gaSize = mi.SizeOfImage;
}

static void WriteTxt(FileOut& f, const ClassRow& c) {
    f.Printf("// %s    [%s]  size=0x%X%s%s\n",
             FullName(c).c_str(), c.assembly.c_str(),
             (unsigned)c.instanceSize,
             c.isEnum ? "  enum" : "",
             c.isValueType ? "  struct" : "");
    if (!c.parent.empty()) f.Printf("//   : %s\n", c.parent.c_str());
    for (const auto& fld : c.fields) {
        f.Printf("    %s%s%-40s  %s   // 0x%zX\n",
                 fld.isStatic  ? "static "  : "",
                 fld.isLiteral ? "const "   : "",
                 fld.name.c_str(),
                 fld.type.c_str(),
                 fld.offset);
    }
    for (const auto& m : c.methods) {
        f.Printf("    %s%s(", m.isStatic ? "static " : "", m.name.c_str());
        for (size_t i = 0; i < m.params.size(); i++) {
            const auto& p = m.params[i];
            f.Printf("%s%s %s", i ? ", " : "",
                     p.type.c_str(),
                     p.name.empty() ? "_" : p.name.c_str());
        }
        uintptr_t pa = reinterpret_cast<uintptr_t>(m.ptr);
        if (pa && g_gaBase && pa >= g_gaBase && pa < g_gaBase + g_gaSize) {
            f.Printf(") -> %s   // ptr=%p  RVA=0x%llX\n",
                     m.returnType.c_str(), m.ptr,
                     (unsigned long long)(pa - g_gaBase));
        } else {
            f.Printf(") -> %s   // ptr=%p\n", m.returnType.c_str(), m.ptr);
        }
    }
    auto fmtPtr = [](void* p) -> std::string {
        if (!p) return std::string("null");
        char b[48];
        uintptr_t pa = reinterpret_cast<uintptr_t>(p);
        if (g_gaBase && pa >= g_gaBase && pa < g_gaBase + g_gaSize)
            _snprintf_s(b, sizeof(b), _TRUNCATE, "%p RVA=0x%llX", p,
                        (unsigned long long)(pa - g_gaBase));
        else
            _snprintf_s(b, sizeof(b), _TRUNCATE, "%p", p);
        return std::string(b);
    };
    for (const auto& p : c.properties) {
        // Header line
        const std::string& ty = !p.getReturnType.empty() ? p.getReturnType
                              : !p.setParamType.empty() ? p.setParamType
                              : std::string("?");
        f.Printf("    %sproperty %s %s {\n",
                 p.isStatic ? "static " : "",
                 ty.c_str(), p.name.c_str());
        if (p.hasGet) f.Printf("        get -> %s   // %s\n",
                               p.getReturnType.empty() ? "?" : p.getReturnType.c_str(),
                               fmtPtr(p.getPtr).c_str());
        if (p.hasSet) f.Printf("        set(%s)        // %s\n",
                               p.setParamType.empty() ? "?" : p.setParamType.c_str(),
                               fmtPtr(p.setPtr).c_str());
        f.Write("    }\n");
    }
    f.Write("\n");
}

static void WriteJson(FileOut& f, const ClassRow& c, bool first) {
    if (!first) f.Write(",\n");
    f.Write("  {");
    f.Printf("\"assembly\":\"%s\",", JsonEscape(c.assembly).c_str());
    f.Printf("\"namespace\":\"%s\",", JsonEscape(c.ns).c_str());
    f.Printf("\"name\":\"%s\",", JsonEscape(c.name).c_str());
    f.Printf("\"parent\":\"%s\",", JsonEscape(c.parent).c_str());
    f.Printf("\"isEnum\":%s,", c.isEnum ? "true" : "false");
    f.Printf("\"isValueType\":%s,", c.isValueType ? "true" : "false");
    f.Printf("\"instanceSize\":%d,", c.instanceSize);

    f.Write("\"fields\":[");
    for (size_t i = 0; i < c.fields.size(); i++) {
        const auto& fld = c.fields[i];
        if (i) f.Write(",");
        f.Printf("{\"name\":\"%s\",\"type\":\"%s\",\"offset\":%zu,\"flags\":%d,\"static\":%s,\"const\":%s}",
                 JsonEscape(fld.name).c_str(),
                 JsonEscape(fld.type).c_str(),
                 fld.offset, fld.flags,
                 fld.isStatic  ? "true" : "false",
                 fld.isLiteral ? "true" : "false");
    }
    f.Write("],");

    f.Write("\"methods\":[");
    for (size_t i = 0; i < c.methods.size(); i++) {
        const auto& m = c.methods[i];
        if (i) f.Write(",");
        char ptrbuf[32];
        _snprintf_s(ptrbuf, sizeof(ptrbuf), _TRUNCATE, "0x%llx", (unsigned long long)m.ptr);
        f.Printf("{\"name\":\"%s\",\"returnType\":\"%s\",\"static\":%s,\"virtual\":%s,\"abstract\":%s,\"ptr\":\"%s\",\"params\":[",
                 JsonEscape(m.name).c_str(),
                 JsonEscape(m.returnType).c_str(),
                 m.isStatic   ? "true" : "false",
                 m.isVirtual  ? "true" : "false",
                 m.isAbstract ? "true" : "false",
                 ptrbuf);
        for (size_t j = 0; j < m.params.size(); j++) {
            if (j) f.Write(",");
            char tpbuf[32];
            _snprintf_s(tpbuf, sizeof(tpbuf), _TRUNCATE, "0x%llx",
                        (unsigned long long)m.params[j].typePtr);
            f.Printf("{\"type\":\"%s\",\"name\":\"%s\",\"typePtr\":\"%s\"}",
                     JsonEscape(m.params[j].type).c_str(),
                     JsonEscape(m.params[j].name).c_str(),
                     tpbuf);
        }
        f.Write("]}");
    }
    f.Write("],");

    f.Write("\"properties\":[");
    for (size_t i = 0; i < c.properties.size(); i++) {
        const auto& p = c.properties[i];
        if (i) f.Write(",");
        char gpbuf[32], spbuf[32];
        _snprintf_s(gpbuf, sizeof(gpbuf), _TRUNCATE, "0x%llx", (unsigned long long)p.getPtr);
        _snprintf_s(spbuf, sizeof(spbuf), _TRUNCATE, "0x%llx", (unsigned long long)p.setPtr);
        f.Printf("{\"name\":\"%s\",\"static\":%s,\"get\":%s,\"set\":%s,"
                 "\"getReturnType\":\"%s\",\"getPtr\":\"%s\","
                 "\"setParamType\":\"%s\",\"setPtr\":\"%s\"}",
                 JsonEscape(p.name).c_str(),
                 p.isStatic ? "true" : "false",
                 p.hasGet ? "true" : "false",
                 p.hasSet ? "true" : "false",
                 JsonEscape(p.getReturnType).c_str(), gpbuf,
                 JsonEscape(p.setParamType).c_str(),  spbuf);
    }
    f.Write("]}");
}

static void WriteLayoutHeader(FileOut& f, const ClassRow& c) {
    if (c.fields.empty()) return;
    f.Printf("// %s   [%s]  size=0x%X\n",
             FullName(c).c_str(), c.assembly.c_str(), (unsigned)c.instanceSize);
    f.Printf("namespace Layout { namespace %s { namespace %s {\n",
             CppIdent(c.assembly).c_str(),
             CppIdent(c.ns.empty() ? std::string("global") : c.ns).c_str());
    f.Printf("  // class %s\n", c.name.c_str());
    for (const auto& fld : c.fields) {
        if (fld.isStatic || fld.isLiteral) continue;
        f.Printf("  constexpr size_t %s__%s = 0x%zX;  // %s\n",
                 CppIdent(c.name).c_str(),
                 CppIdent(fld.name).c_str(),
                 fld.offset,
                 fld.type.c_str());
    }
    f.Write("}}}\n\n");
}

static void WriteCSharpStub(FileOut& f, const ClassRow& c) {
    f.Printf("// [%s]  size=0x%X\n", c.assembly.c_str(), (unsigned)c.instanceSize);
    if (!c.ns.empty()) f.Printf("namespace %s {\n", c.ns.c_str());
    f.Printf("public %s%s %s%s%s {\n",
             c.isValueType ? "struct " : "class ",
             c.isEnum ? " /* enum */" : "",
             c.name.c_str(),
             c.parent.empty() ? "" : " : ",
             c.parent.c_str());
    for (const auto& fld : c.fields) {
        f.Printf("    public %s%s%s %s; // 0x%zX\n",
                 fld.isStatic  ? "static "  : "",
                 fld.isLiteral ? "const "   : "",
                 fld.type.c_str(),
                 fld.name.c_str(),
                 fld.offset);
    }
    for (const auto& m : c.methods) {
        f.Printf("    public %s%s %s(", m.isStatic ? "static " : "",
                 m.returnType.c_str(), m.name.c_str());
        for (size_t i = 0; i < m.params.size(); i++) {
            f.Printf("%s%s %s", i ? ", " : "",
                     m.params[i].type.c_str(),
                     m.params[i].name.empty() ? "_" : m.params[i].name.c_str());
        }
        f.Printf("); // 0x%llx\n", (unsigned long long)m.ptr);
    }
    for (const auto& p : c.properties) {
        const std::string& ty = !p.getReturnType.empty() ? p.getReturnType
                              : !p.setParamType.empty() ? p.setParamType
                              : std::string("object");
        f.Printf("    public %s%s %s { %s%s} // get=0x%llx set=0x%llx\n",
                 p.isStatic ? "static " : "",
                 ty.c_str(), p.name.c_str(),
                 p.hasGet ? "get; " : "",
                 p.hasSet ? "set; " : "",
                 (unsigned long long)p.getPtr,
                 (unsigned long long)p.setPtr);
    }
    f.Write("}\n");
    if (!c.ns.empty()) f.Write("}\n");
    f.Write("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool DumpAll(const std::wstring& outDir, Stats& stats) {
    auto& a = GetApi();
    if (!a.IsReady()) {
        printf("  [dumper] IL2CPP API not resolved.\n");
        return false;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (!EnsureDirChain(outDir)) {
        printf("  [dumper] Failed to create output directory.\n");
        return false;
    }
    EnsureGaBase();

    // Make sure we're attached as an il2cpp-managed thread before walking
    // the runtime - some accessors (notably field iteration) require it.
    Il2CppDomain* domain = a.domain_get();
    if (!domain) {
        printf("  [dumper] il2cpp_domain_get returned null.\n");
        return false;
    }
    if (a.thread_attach) a.thread_attach(domain);

    FileOut fTxt   (outDir + L"\\dump.txt");
    FileOut fJson  (outDir + L"\\dump.json");
    FileOut fHdr   (outDir + L"\\layout.h");
    FileOut fCs    (outDir + L"\\dump.cs");
    if (!fTxt || !fJson || !fHdr || !fCs) {
        printf("  [dumper] Failed to open output files.\n");
        return false;
    }

    fJson.Write("[\n");
    fHdr.Write("// Auto-generated by ScarsTool sdk_dumper\n#pragma once\n#include <cstddef>\n\n");
    fCs .Write("// Auto-generated by ScarsTool sdk_dumper\n\n");
    fTxt.Write("// ScarsTool SDK dump\n\n");

    size_t asmCount = 0;
    const Il2CppAssembly** assemblies = a.domain_get_assemblies(domain, &asmCount);
    if (!assemblies || asmCount == 0) {
        printf("  [dumper] No assemblies returned by IL2CPP.\n");
        fJson.Write("\n]\n");
        return false;
    }

    bool firstJson = true;
    for (size_t ai = 0; ai < asmCount; ai++) {
        const Il2CppAssembly* asmPtr = assemblies[ai];
        if (!asmPtr) continue;
        const Il2CppImage* img = a.assembly_get_image(asmPtr);
        if (!img) continue;
        std::string asmName = SafeStr(a.image_get_name(img));
        size_t classCount = a.image_get_class_count(img);
        stats.assemblies++;
        stats.images++;
        printf("  [dumper] %-40s  classes=%zu\n", asmName.c_str(), classCount);

        for (size_t ci = 0; ci < classCount; ci++) {
            Il2CppClass* k = SafeImageGetClass(img, ci);
            if (!k) { stats.errors++; continue; }
            ClassRow row;
            if (!SafeCollect(k, asmName, stats, row)) {
                stats.errors++;
                continue;
            }
            stats.classes++;

            WriteTxt   (fTxt,  row);
            WriteJson  (fJson, row, firstJson);
            firstJson = false;
            WriteLayoutHeader(fHdr, row);
            WriteCSharpStub  (fCs,  row);
        }
    }

    fJson.Write("\n]\n");

    printf("  [dumper] Done.  assemblies=%d classes=%d fields=%d methods=%d properties=%d errors=%d\n",
           stats.assemblies, stats.classes, stats.fields, stats.methods,
           stats.properties, stats.errors);
    return true;
}

bool DumpAllAuto(std::wstring& outDirChosen, Stats& stats) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exePath, L'\\')) *slash = 0;

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t stamp[64];
    swprintf_s(stamp, L"%04d%02d%02d_%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring root = std::wstring(exePath) + L"\\ScarsTool_dump";
    EnsureDir(root);
    outDirChosen = root + L"\\" + stamp;

    return DumpAll(outDirChosen, stats);
}

} // namespace sdk_dumper
