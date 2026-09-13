#include "Emitters.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace zircon::emit {
namespace {

// The generated SDK has to compile, and compiling is what proves the layout right. Every
// member carries a static_assert on its offset, so a wrong derivation upstream shows up as
// a compiler error instead of a silent misread at runtime.

// Container layouts aren't reflected and so can't be dumped directly. But every property
// that *uses* one reports its element size, which is the same number. Measuring beats
// hardcoding here: FText is 16 bytes in this build and 24 in others, and a wrong constant
// quietly shifts every member below it.
struct SizeTable {
    std::map<ir::TypeKind, std::int32_t> common;

    std::int32_t For(ir::TypeKind kind) const {
        const auto it = common.find(kind);
        return it == common.end() ? 0 : it->second;
    }
};

SizeTable MeasureSizes(const ir::Dump& dump) {
    std::map<ir::TypeKind, std::map<std::int32_t, int>> histogram;

    std::function<void(const ir::TypeRef&)> note = [&](const ir::TypeRef& type) {
        if (type.size > 0) ++histogram[type.kind][type.size];
        for (const auto& param : type.params) note(param);
    };

    for (const auto& package : dump.packages)
        for (const auto* list : {&package.classes, &package.structs})
            for (const auto& record : *list) {
                for (const auto& property : record.properties) note(property.type);
                for (const auto& function : record.functions)
                    for (const auto& param : function.params) note(param.type);
            }

    SizeTable table;
    for (const auto& [kind, sizes] : histogram) {
        // Mode, not max. A handful of outliers - a sparse delegate is 1 byte where an
        // inline one is 16 - must not get to define the shared type. Members that disagree
        // are caught individually by the size guard and emitted opaque.
        const auto best = std::max_element(sizes.begin(), sizes.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        table.common[kind] = best->first;
    }
    return table;
}

struct TypeIndex {
    std::unordered_map<std::string, const ir::Struct*> structs;   // path -> record
    std::unordered_map<std::string, const ir::Enum*>   enums;
    std::unordered_map<std::string, std::string>       cpp_names; // path -> C++ name
    std::unordered_map<std::string, std::string>       package_of;
    SizeTable                                          sizes;
};

std::string Indent(int depth) { return std::string(static_cast<std::size_t>(depth) * 4, ' '); }

// The CDO value, trimmed to fit on a line beside the offset. Defaults are the difference
// between a header that says a field exists and one that says what it normally holds. Still
// not the place for a full struct dump.
std::string DefaultSuffix(const ir::Property& property) {
    constexpr std::size_t kMaxDefault = 48;

    if (property.default_value.empty()) return {};

    // Nobody needs telling a container is empty or a pointer null. That's the assumption
    // already, and printing it across thousands of members costs more than it explains.
    if (property.default_value == "nullptr" || property.default_value == "[]" ||
        property.default_value == "{}" || property.default_value == "0" ||
        property.default_value == "false" || property.default_value == "None")
        return {};

    std::string value = property.default_value;

    // A newline here ends the comment and comments out the next member.
    for (char& c : value)
        if (c == '\n' || c == '\r') c = ' ';

    if (value.size() > kMaxDefault) {
        value.resize(kMaxDefault - 3);
        value += "...";
    }
    return " = " + value;
}

// Declarations vary enormously in width - a nested TMap member dwarfs a bool - so pad
// comments out to a column. Otherwise the offset column
// zig-zags and stops being scannable, which was the only reason for it.
constexpr std::size_t kCommentColumn = 68;

std::string WithComment(std::string_view declaration, std::string_view comment) {
    std::string line = Indent(1) + std::string(declaration);
    if (line.size() < kCommentColumn) line.append(kCommentColumn - line.size(), ' ');
    else line.push_back(' ');
    line += comment;
    line.push_back('\n');
    return line;
}

// Identifiers <windows.h> has already taken as macros.
//
// UE names a reflected function min, another max, an enumerator PF_MAX and a function
// PlaySound. Include windows.h before the SDK, as any injected DLL does, and the
// preprocessor rewrites those before the compiler sees them: "int32 min(int32 A, int32 B)"
// stops being a declaration.
//
// The SDK renames its own identifier instead of undefining the macro. Undefining would
// fix the header and break the caller, since TRUE, FALSE, RGB and SendMessage are names
// their code is entitled to keep using. The renamed member carries the engine's spelling
// in its comment.
//
// This list is the Win32 macro names, not anything about the engine, so it does not go
// stale with an engine release. It is also not exhaustive: 16 of 66114 identifiers emitted
// for a UE 5.6 game collided on the Windows 10.0.26100 headers, and these are those plus
// the neighbouring members of the same families.
bool ClashesWithWindowsMacro(std::string_view name) {
    static const std::set<std::string_view> kTaken = {
        "min", "max", "RGB", "TRUE", "FALSE", "DELETE", "ERROR", "IN", "OUT", "OPTIONAL",
        "NO_ERROR", "PF_MAX", "PlaySound", "DrawText", "GetObject", "GetMessage",
        "SendMessage", "PostMessage", "GetCommandLine", "GetCurrentTime",
        "GetDiskFreeSpace", "ReportEvent", "UpdateResource", "CreateWindow", "CreateFile",
        "CreateProcess", "GetClassName", "LoadImage", "DrawState", "CopyFile", "MoveFile",
        "DeleteFile", "GetUserName", "GetComputerName", "SetPort", "GetFreeSpace",
        "GetTempPath", "GetFullPathName", "FindText", "ReplaceText", "GetJob", "SetJob",
        "AddJob", "GetForm", "SetForm", "AddForm", "DeleteForm", "GetPrinter",
        "SetPrinter", "AddPrinter", "DeletePrinter", "StartDoc", "StartPage", "EndPage",
        "EndDoc", "AbortDoc", "Rectangle", "Ellipse", "Polygon", "Polyline",
    };
    return kTaken.count(name) != 0;
}

// The SDK's spelling of a reflected name: legal C++, and not something the preprocessor
// will rewrite.
std::string SafeIdentifier(std::string_view raw) {
    std::string name = util::SanitizeIdentifier(raw);
    if (ClashesWithWindowsMacro(name)) name += "_";
    return name;
}

// Unique within its owner and legal in C++. UE lets two properties differ only by
// characters the sanitizer collapses, so enforce uniqueness instead of hoping for it.
std::string UniqueMember(std::string_view raw, std::set<std::string>& taken) {
    std::string name = SafeIdentifier(raw);
    if (taken.insert(name).second) return name;

    for (int suffix = 1;; ++suffix) {
        std::string candidate = std::format("{}_{}", name, suffix);
        if (taken.insert(candidate).second) return candidate;
    }
}

// Declared ahead of use: the enum's effective width is needed by the size checks above
// and by the declaration far below, and all three have to agree or the struct layout and
// the enum definition disagree with each other.
std::string_view EnumUnderlying(const ir::Enum& record);

std::int32_t UnderlyingSize(std::string_view underlying) {
    if (underlying == "int8"  || underlying == "uint8")  return 1;
    if (underlying == "int16" || underlying == "uint16") return 2;
    if (underlying == "int32" || underlying == "uint32") return 4;
    if (underlying == "int64" || underlying == "uint64") return 8;
    return 0;
}

std::string SizedInteger(std::int32_t bytes) {
    switch (bytes) {
        case 1:  return "uint8";
        case 2:  return "uint16";
        case 4:  return "uint32";
        case 8:  return "uint64";
        default: return {};
    }
}

std::int32_t ExpectedSize(const TypeIndex& index, const ir::TypeRef& type) {
    using ir::TypeKind;
    switch (type.kind) {
        case TypeKind::Bool: case TypeKind::Int8:  case TypeKind::UInt8:  return 1;
        case TypeKind::Int16: case TypeKind::UInt16:                      return 2;
        case TypeKind::Int32: case TypeKind::UInt32: case TypeKind::Float: return 4;
        case TypeKind::Int64: case TypeKind::UInt64: case TypeKind::Double: return 8;
        case TypeKind::ObjectPtr:                                         return 8;

        case TypeKind::Enum: {
            const auto it = index.enums.find(type.name);
            return it == index.enums.end() ? 0 : UnderlyingSize(EnumUnderlying(*it->second));
        }
        case TypeKind::Struct: {
            const auto it = index.structs.find(type.name);
            return it == index.structs.end() ? 0 : it->second->size;
        }
        default:
            return index.sizes.For(type.kind);
    }
}

bool IsBitfieldGroupStart(const std::vector<ir::Property>& properties, std::size_t index) {
    return properties[index].is_bitfield &&
           (index == 0 || properties[index - 1].offset != properties[index].offset ||
            !properties[index - 1].is_bitfield);
}

// `fallback_size` covers the case where the type can't be expressed faithfully. An opaque
// byte array of the right size keeps every later offset correct, which matters far more
// than naming the type.
std::string RenderType(const TypeIndex& index, const ir::TypeRef& type,
                       std::int32_t fallback_size, std::vector<std::string>& warnings,
                       bool& opaque);

std::string RenderReferenced(const TypeIndex& index, const std::string& path,
                             std::string_view fallback) {
    const auto it = index.cpp_names.find(path);
    if (it != index.cpp_names.end()) return it->second;
    return std::string(fallback);
}

std::string RenderInner(const TypeIndex& index, const ir::TypeRef& type,
                        std::vector<std::string>& warnings) {
    bool opaque = false;
    const std::string rendered = RenderType(index, type, type.size, warnings, opaque);
    if (!opaque && !rendered.empty()) return rendered;

    // A container's own size doesn't depend on its element type; TArray is three fields
    // whatever T is. So an unresolvable element can still be named without disturbing any
    // offset. Returning empty instead produced `TArray<>`, which doesn't compile, and the
    // whole header was lost over one element type.
    return "FUnresolved";
}

std::string RenderType(const TypeIndex& index, const ir::TypeRef& type,
                       std::int32_t fallback_size, std::vector<std::string>& warnings,
                       bool& opaque) {
    using ir::TypeKind;
    opaque = false;

    switch (type.kind) {
        case TypeKind::Bool:    return "bool";
        case TypeKind::Int8:    return "int8";
        case TypeKind::Int16:   return "int16";
        case TypeKind::Int32:   return "int32";
        case TypeKind::Int64:   return "int64";
        case TypeKind::UInt8:   return "uint8";
        case TypeKind::UInt16:  return "uint16";
        case TypeKind::UInt32:  return "uint32";
        case TypeKind::UInt64:  return "uint64";
        case TypeKind::Float:   return "float";
        case TypeKind::Double:  return "double";
        case TypeKind::Name:    return "FName";
        case TypeKind::String:  return "FString";
        case TypeKind::Text:    return "FText";

        case TypeKind::Enum: {
            const auto it = index.enums.find(type.name);
            if (it == index.enums.end()) {
                // Unknown width outside the dump; guessing uint8 shifts every member below.
                opaque = true;
                return {};
            }

            // Property wins over the enum's underlying type on disagreement. It's measured
            // per use where the enum's width is inferred from other uses, and a mismatch
            // corrupts every later member.
            const std::int32_t declared = UnderlyingSize(EnumUnderlying(*it->second));
            if (fallback_size > 0 && declared > 0 && declared != fallback_size) {
                warnings.push_back(std::format(
                    "enum '{}' is declared {} bytes but used as {}; emitting a sized "
                    "integer to preserve layout", type.name, declared, fallback_size));
                return SizedInteger(fallback_size);
            }
            return index.cpp_names.at(type.name);
        }

        case TypeKind::Struct: {
            const auto it = index.structs.find(type.name);
            if (it == index.structs.end()) { opaque = true; return {}; }
            return index.cpp_names.at(type.name);
        }

        case TypeKind::ObjectPtr:
            return "class " + RenderReferenced(index, type.name, "UObject") + "*";
        case TypeKind::ClassPtr:
            return "TSubclassOf<class " + RenderReferenced(index, type.name, "UObject") + ">";
        case TypeKind::WeakPtr:
            return "TWeakObjectPtr<class " + RenderReferenced(index, type.name, "UObject") + ">";
        case TypeKind::LazyPtr:
            return "TLazyObjectPtr<class " + RenderReferenced(index, type.name, "UObject") + ">";
        case TypeKind::SoftPtr:
            return "TSoftObjectPtr<class " + RenderReferenced(index, type.name, "UObject") + ">";
        case TypeKind::SoftClassPtr:
            return "TSoftClassPtr<class " + RenderReferenced(index, type.name, "UObject") + ">";
        case TypeKind::Interface:
            return "TScriptInterface<class " + RenderReferenced(index, type.name, "IInterface") + ">";

        case TypeKind::Array:
            if (type.params.size() == 1)
                return "TArray<" + RenderInner(index, type.params[0], warnings) + ">";
            break;
        case TypeKind::Set:
            if (type.params.size() == 1)
                return "TSet<" + RenderInner(index, type.params[0], warnings) + ">";
            break;
        case TypeKind::Map:
            if (type.params.size() == 2)
                return "TMap<" + RenderInner(index, type.params[0], warnings) + ", " +
                       RenderInner(index, type.params[1], warnings) + ">";
            break;
        case TypeKind::Optional:
            if (type.params.size() == 1)
                return "TOptional<" + RenderInner(index, type.params[0], warnings) + ">";
            break;

        case TypeKind::Delegate:           return "FDelegate";
        case TypeKind::MulticastDelegate:  return "FMulticastDelegate";
        case TypeKind::FieldPath:          return "FFieldPath";

        case TypeKind::Unknown:
            break;
    }

    // Type couldn't be expressed. Emit bytes and say so: the SDK still lays out correctly
    // and the user knows what was lost.
    (void)fallback_size;
    opaque = true;
    if (!type.raw.empty())
        warnings.push_back(std::format("opaque member for unrepresentable type '{}'", type.raw));
    return {};
}

// --- padding ------------------------------------------------------------------------

void EmitPadding(std::string& out, std::int32_t from, std::int32_t to, int& pad_counter) {
    if (to <= from) return;
    out += WithComment(std::format("uint8 Pad_{:X}[0x{:X}];", pad_counter++, to - from),
                       std::format("// 0x{:04X}(0x{:04X}) MISSED OFFSET", from, to - from));
}

// --- ordering -----------------------------------------------------------------------

// Structs held by value must be complete before use, so they are emitted in dependency
// order. Pointers only need a forward declaration, which is why classes are far less
// constrained than structs.
void CollectValueDependencies(const ir::TypeRef& type, std::set<std::string>& into) {
    if (type.kind == ir::TypeKind::Struct || type.kind == ir::TypeKind::Enum) {
        if (!type.name.empty()) into.insert(type.name);
    }
    for (const auto& param : type.params) CollectValueDependencies(param, into);
}

std::vector<const ir::Struct*> TopoSort(const std::vector<const ir::Struct*>& records,
                                        std::vector<std::string>& warnings) {
    std::unordered_map<std::string, const ir::Struct*> local;
    for (const auto* record : records) local[record->path] = record;

    std::vector<const ir::Struct*> ordered;
    std::unordered_set<std::string> done;
    std::unordered_set<std::string> visiting;

    // Iterative, not recursive: a dump with 5000 interdependent structs would
    // otherwise be a stack-depth gamble.
    struct Frame { const ir::Struct* record; std::vector<std::string> deps; std::size_t at; };

    for (const auto* seed : records) {
        if (done.count(seed->path)) continue;

        std::vector<Frame> stack;
        auto push = [&](const ir::Struct* record) {
            std::set<std::string> deps;
            if (!record->super.empty()) deps.insert(record->super);
            for (const auto& property : record->properties)
                CollectValueDependencies(property.type, deps);

            std::vector<std::string> filtered;
            for (const auto& dep : deps)
                if (local.count(dep) && !done.count(dep)) filtered.push_back(dep);

            stack.push_back(Frame{record, std::move(filtered), 0});
            visiting.insert(record->path);
        };
        push(seed);

        while (!stack.empty()) {
            Frame& frame = stack.back();
            if (frame.at < frame.deps.size()) {
                const std::string dep = frame.deps[frame.at++];
                if (done.count(dep)) continue;
                if (visiting.count(dep)) {
                    // A genuine cycle. UE has them (two structs referencing each other
                    // through containers); emit anyway and note it rather than dropping
                    // a type.
                    warnings.push_back(std::format(
                        "dependency cycle involving '{}' and '{}'", frame.record->path, dep));
                    continue;
                }
                push(local[dep]);
                continue;
            }

            visiting.erase(frame.record->path);
            if (done.insert(frame.record->path).second) ordered.push_back(frame.record);
            stack.pop_back();
        }
    }
    return ordered;
}

// --- emission -----------------------------------------------------------------------

// The declared underlying type has to hold every enumerator.
//
// UEnum does not always state a width, so the IR falls back to uint8, and a flags enum
// built from bit positions overflows that immediately: ETransformGizmoSubElements reaches
// 524287. MSVC warns (C4369) and clamps the value, which would leave the SDK compiling
// while comparing against the wrong number.
//
// Widening is safe in a way narrowing is not. A property that reads the enum uses its own
// measured width, so the enum's declared type only has to be wide enough to name the
// values.
std::string_view EnumUnderlying(const ir::Enum& record) {
    const std::string_view declared =
        record.underlying.empty() ? std::string_view{"uint8"} : record.underlying;

    std::int64_t lowest = 0;
    std::int64_t highest = 0;
    for (const auto& value : record.values) {
        lowest  = std::min(lowest, value.value);
        highest = std::max(highest, value.value);
    }

    const bool negative = lowest < 0;
    const std::int32_t declared_size = UnderlyingSize(declared);

    std::int32_t needed = 1;
    if (negative) {
        // The bounds are written as signed 64-bit literals: 0x8000'0000 on its own is
        // unsigned, and negating it would stay unsigned.
        if      (lowest >= -128LL        && highest <= 127LL)        needed = 1;
        else if (lowest >= -32768LL      && highest <= 32767LL)      needed = 2;
        else if (lowest >= -2147483648LL && highest <= 2147483647LL) needed = 4;
        else                                                         needed = 8;
    } else {
        if      (highest <= 0xFFLL)        needed = 1;
        else if (highest <= 0xFFFFLL)      needed = 2;
        else if (highest <= 0xFFFFFFFFLL)  needed = 4;
        else                               needed = 8;
    }

    if (declared_size >= needed && (!negative || declared.front() == 'i')) return declared;

    switch (needed) {
        case 1:  return negative ? "int8"  : "uint8";
        case 2:  return negative ? "int16" : "uint16";
        case 4:  return negative ? "int32" : "uint32";
        default: return negative ? "int64" : "uint64";
    }
}

void EmitEnum(std::string& out, const ir::Enum& record, const TypeIndex& index) {
    const std::string name = index.cpp_names.at(record.path);

    out += std::format("// {}\n", record.path);
    out += std::format("enum class {} : {} {{\n", name, EnumUnderlying(record));

    std::set<std::string> taken;
    for (const auto& value : record.values) {
        // UE stores entries fully qualified ("EMovementMode::MOVE_Walking"); the C++ enum
        // re-adds the scope, so the prefix has to come off or every entry is duplicated.
        std::string entry = value.name;
        const auto scope = entry.rfind("::");
        if (scope != std::string::npos) entry = entry.substr(scope + 2);

        out += std::format("{}{} = {},\n", Indent(1), UniqueMember(entry, taken), value.value);
    }
    out += "};\n\n";
}

// --- reflected function wrappers -------------------------------------------------------

// "/Script/Engine.Vector" -> "/Script/Engine"
std::string PackageOfPath(std::string_view path) {
    const auto dot = path.rfind('.');
    return dot == std::string_view::npos ? std::string{} : std::string(path.substr(0, dot));
}

// Whether a signature type is already available in the package header that declares the
// method. A pointer is, through a forward declaration. A struct or enum passed by value
// from another package is not, and needs one emitted for it.
//
// A declaration is happy with an incomplete type; only the body needs the definition, and
// bodies live in Functions.hpp which includes everything.
bool SignatureIsReachable(const ir::TypeRef& type, const std::string& package) {
    using ir::TypeKind;

    switch (type.kind) {
        case TypeKind::Struct:
        case TypeKind::Enum:
            if (type.name.empty()) return false;
            if (PackageOfPath(type.name) != package) return false;
            break;

        // Containers hold their element by value, so the element has to be reachable too.
        case TypeKind::Array: case TypeKind::Set: case TypeKind::Map:
        case TypeKind::Optional:
            break;

        default:
            return true;   // scalars, and pointers that a forward declaration covers
    }

    for (const auto& param : type.params)
        if (!SignatureIsReachable(param, package)) return false;
    return true;
}

bool IsStaticFunction(const ir::Function& fn) {
    return std::find(fn.flag_names.begin(), fn.flag_names.end(), "Static") !=
           fn.flag_names.end();
}

// One parameter block per function, named for the pair so two classes can each have an
// Activate without colliding.
std::string ParamsStructName(std::string_view type_name, std::string_view function_name) {
    return std::format("{}_{}_Params", type_name, SafeIdentifier(function_name));
}

// Out parameters are references because the callee writes them. Everything else goes by
// value: "const T*&" is a reference to pointer-to-const and will not assign into the T*
// member of the parameter block, and a per-kind rule would be more surface than the copy
// is worth.
std::string RenderParameter(const ir::FunctionParam& param, const std::string& rendered,
                            const std::string& member) {
    if (param.is_out) return std::format("{}& {}", rendered, member);
    return std::format("{} {}", rendered, member);
}

// The parameter block, laid out at the offsets the engine reported. Padding matters here
// for the same reason it does in a struct: the callee reads its arguments by offset.
void EmitParamsStruct(std::string& out, const ir::Struct& record, const ir::Function& fn,
                      const TypeIndex& index, std::vector<std::string>& warnings) {
    const std::string type_name = index.cpp_names.at(record.path);
    const std::string name = ParamsStructName(type_name, fn.name);

    std::vector<ir::FunctionParam> params = fn.params;
    std::sort(params.begin(), params.end(),
              [](const ir::FunctionParam& a, const ir::FunctionParam& b) {
                  return a.offset < b.offset;
              });

    out += std::format("struct {} {{\n", name);

    std::set<std::string> taken;
    int pad_counter = 0;
    std::int32_t cursor = 0;
    for (const auto& param : params) {
        EmitPadding(out, cursor, param.offset, pad_counter);

        bool opaque = false;
        const std::string rendered = RenderType(index, param.type, param.size, warnings,
                                                opaque);
        const std::string member = UniqueMember(param.name, taken);

        if (opaque || rendered.empty()) {
            out += WithComment(std::format("uint8 {}[0x{:X}];", member, param.size),
                               std::format("// 0x{:04X}(0x{:04X}) unrepresentable",
                                           param.offset, param.size));
        } else {
            out += WithComment(std::format("{} {};", rendered, member),
                               std::format("// 0x{:04X}(0x{:04X})", param.offset,
                                           param.size));
        }
        cursor = param.offset + param.size;
    }

    // The block has to be at least as large as the last parameter ends, or the callee
    // writes past it.
    EmitPadding(out, cursor, cursor, pad_counter);
    out += "};\n\n";
}

// The declaration that sits inside the class.
std::string FunctionDeclaration(const ir::Struct& record, const ir::Function& fn,
                                const TypeIndex& index, std::set<std::string>& taken,
                                std::vector<std::string>& warnings, std::string& out_name) {
    (void)record;
    std::string return_type = "void";
    std::vector<std::string> args;

    std::set<std::string> arg_names;
    for (const auto& param : fn.params) {
        bool opaque = false;
        const std::string rendered = RenderType(index, param.type, param.size, warnings,
                                                opaque);
        if (opaque || rendered.empty()) return {};   // cannot be spelled, so no wrapper

        if (param.is_return) {
            return_type = rendered;
            continue;
        }
        args.push_back(RenderParameter(param, rendered, UniqueMember(param.name, arg_names)));
    }

    out_name = UniqueMember(fn.name, taken);

    std::string joined;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) joined += ", ";
        joined += args[i];
    }
    return std::format("{} {}({});", return_type, out_name, joined);
}

// The body, emitted after every type in the file so each parameter type is complete.
void EmitFunctionBody(std::string& out, const ir::Struct& record, const ir::Function& fn,
                      const std::string& method_name, const TypeIndex& index,
                      std::vector<std::string>& warnings) {
    const std::string type_name = index.cpp_names.at(record.path);
    const std::string params_name = ParamsStructName(type_name, fn.name);

    std::string return_type = "void";
    std::string return_member;
    std::vector<std::pair<std::string, bool>> assignments;   // member, is_out
    std::vector<std::string> args;

    std::set<std::string> arg_names;
    std::set<std::string> member_names;
    for (const auto& param : fn.params) {
        bool opaque = false;
        const std::string rendered = RenderType(index, param.type, param.size, warnings,
                                                opaque);
        if (opaque || rendered.empty()) return;

        const std::string member = UniqueMember(param.name, member_names);
        if (param.is_return) {
            return_type = rendered;
            return_member = member;
            continue;
        }

        const std::string arg = UniqueMember(param.name, arg_names);
        args.push_back(RenderParameter(param, rendered, arg));
        assignments.emplace_back(arg, param.is_out);
    }

    std::string joined;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) joined += ", ";
        joined += args[i];
    }

    out += std::format("inline {} {}::{}({}) {{\n", return_type, type_name, method_name,
                       joined);
    out += std::format("{}static void* function = nullptr;\n", Indent(1));
    out += std::format("{}{} params{{}};\n", Indent(1), params_name);

    std::size_t member_index = 0;
    std::set<std::string> replay;
    for (const auto& param : fn.params) {
        if (param.is_return) continue;
        const std::string member = UniqueMember(param.name, replay);
        out += std::format("{}params.{} = {};\n", Indent(1), member,
                           assignments[member_index].first);
        ++member_index;
    }

    out += std::format("{}ZirconSDK::Call(this, function, \"{}.{}\", &params);\n",
                       Indent(1), record.path, fn.name);

    // An out parameter is only useful if what the callee wrote comes back.
    member_index = 0;
    replay.clear();
    for (const auto& param : fn.params) {
        if (param.is_return) continue;
        const std::string member = UniqueMember(param.name, replay);
        if (assignments[member_index].second)
            out += std::format("{}{} = params.{};\n", Indent(1),
                               assignments[member_index].first, member);
        ++member_index;
    }

    if (!return_member.empty())
        out += std::format("{}return params.{};\n", Indent(1), return_member);

    out += "}\n\n";
}

void EmitStruct(std::string& out, const ir::Struct& record, const TypeIndex& index,
                std::vector<std::string>& warnings) {
    const std::string name = index.cpp_names.at(record.path);

    // ChildProperties is a linked list the engine prepends to, so it is not reliably in
    // offset order. Sorting is required before any padding arithmetic makes sense.
    std::vector<ir::Property> properties = record.properties;
    std::sort(properties.begin(), properties.end(),
              [](const ir::Property& a, const ir::Property& b) {
                  if (a.offset != b.offset) return a.offset < b.offset;
                  return a.bit_index < b.bit_index;
              });

    const std::int32_t first_offset =
        properties.empty() ? record.size : properties.front().offset;

    // A derived type can legitimately place its own members *before* the base's reported
    // size, and C++ inheritance cannot reproduce that:
    //
    //   - an empty base has PropertiesSize 1, but the derived type starts at 0 (the same
    //     thing C++ empty-base optimization does, except UE already accounted for it)
    //   - a member can land in the base's trailing alignment padding
    //
    // Inheriting anyway would push every member past the base and break every offset, so
    // those types are emitted flat with the inherited bytes padded instead. Inheritance
    // is kept wherever it actually reproduces the layout, which is the overwhelming
    // majority.
    const auto super = record.super.empty() ? index.cpp_names.end()
                                            : index.cpp_names.find(record.super);
    const bool super_known = super != index.cpp_names.end();

    const bool flatten = !record.super.empty() &&
                         (!super_known || first_offset < record.inherited_size);

    // No alignas on generated types. Under pack(1) every one of them already has alignment
    // 1, so sizeof is exactly the bytes emitted — which the padding is computed to make
    // equal to record.size.
    //
    // Restating the engine's alignment actively breaks that, and cannot be salvaged by
    // dropping it selectively, because a type is at least as aligned as its base *and* its
    // members: alignas(1) lowers nothing. A 49-byte Blueprint class holding one 8-aligned
    // struct member still had sizeof rounded to 56. Exact size and offsets are what a
    // reading SDK needs; the engine's alignment is preserved in the comment on each type
    // rather than in the declaration.
    out += std::format("// {} {}\n", record.is_class ? "Class" : "ScriptStruct", record.path);
    out += std::format("// Size 0x{:04X} ({} bytes), alignment {}\n",
                       record.size, record.size, record.alignment);

    std::set<std::string> taken;
    int pad_counter = 0;
    std::int32_t cursor = 0;

    if (record.super.empty()) {
        out += std::format("struct {} {{\n", name);
    } else if (flatten) {
        if (!super_known) {
            out += std::format("// base '{}' is not in this dump; inherited bytes are padded\n",
                               record.super);
        } else {
            out += std::format("// base '{}' (0x{:X} bytes) is flattened: this type's first "
                               "member is at 0x{:X}, inside the base, so inheriting would "
                               "shift every offset\n",
                               record.super, record.inherited_size, first_offset);
        }
        out += std::format("struct {} {{\n", name);
    } else {
        out += std::format("struct {} : public {} {{\n", name, super->second);
        cursor = record.inherited_size;
    }

    std::vector<std::pair<std::string, std::int32_t>> asserts;   // member, offset

    for (std::size_t i = 0; i < properties.size(); ++i) {
        const auto& property = properties[i];

        if (property.offset < cursor) {
            // Overlapping members are not something to paper over: emitting them would
            // produce a struct that cannot match the engine's layout.
            warnings.push_back(std::format(
                "{}::{} at 0x{:X} overlaps the previous member (cursor 0x{:X}); skipped",
                record.path, property.name, property.offset, cursor));
            continue;
        }

        // Bitfields share a byte; the whole group is emitted at once.
        if (property.is_bitfield && IsBitfieldGroupStart(properties, i)) {
            EmitPadding(out, cursor, property.offset, pad_counter);

            int bit_cursor = 0;
            std::size_t j = i;
            for (; j < properties.size(); ++j) {
                const auto& bit = properties[j];
                if (!bit.is_bitfield || bit.offset != property.offset) break;

                if (bit.bit_index > bit_cursor)
                    out += std::format("{}uint8 BitPad_{}_{} : {};\n", Indent(1),
                                       property.offset, bit_cursor, bit.bit_index - bit_cursor);

                out += WithComment(
                    std::format("uint8 {} : 1;", UniqueMember(bit.name, taken)),
                    std::format("// 0x{:04X}(0x0001) bit {}, mask 0x{:02X}{}", bit.offset,
                                bit.bit_index, bit.field_mask, DefaultSuffix(bit)));
                bit_cursor = bit.bit_index + 1;
            }
            if (bit_cursor < 8)
                out += std::format("{}uint8 BitPad_{}_end : {};\n", Indent(1),
                                   property.offset, 8 - bit_cursor);

            cursor = property.offset + 1;
            i = j - 1;
            continue;
        }
        if (property.is_bitfield) continue;   // consumed by the group above

        EmitPadding(out, cursor, property.offset, pad_counter);

        const std::int32_t total = std::max(property.size, 1);
        const std::int32_t dim   = std::max(property.array_dim, 1);
        const std::int32_t element = total / dim;

        // Per *element*: property.size is the total across array_dim, so passing it would
        // make an 8-element byte array look like one 8-byte scalar.
        bool opaque = false;
        std::string rendered = RenderType(index, property.type, element, warnings, opaque);
        const std::string member = UniqueMember(property.name, taken);

        // The decisive guard. A rendering whose C++ size differs from the size the engine
        // reports would place every following member wrong, and the mismatch is invisible
        // until something reads the wrong bytes. Degrading to an opaque array of the right
        // size loses the type name and keeps the layout, which is the correct trade.
        if (!opaque && !rendered.empty()) {
            const std::int32_t expected = ExpectedSize(index, property.type);
            if (expected > 0 && element > 0 && expected != element) {
                warnings.push_back(std::format(
                    "{}::{} renders as {} ({} bytes) but the engine reports {}; emitted "
                    "opaque to preserve layout", record.path, property.name, rendered,
                    expected, element));
                opaque = true;
                rendered.clear();
            }
        }
        if (opaque || rendered.empty()) {
            out += WithComment(
                std::format("uint8 {}[0x{:X}];", member, total),
                std::format("// 0x{:04X}(0x{:04X}) {}", property.offset, total,
                            property.type.raw.empty() ? "opaque" : property.type.raw));
        } else if (property.array_dim > 1) {
            out += WithComment(
                std::format("{} {}[0x{:X}];", rendered, member, property.array_dim),
                std::format("// 0x{:04X}(0x{:04X}){}", property.offset, total,
                            DefaultSuffix(property)));
        } else {
            out += WithComment(std::format("{} {};", rendered, member),
                               std::format("// 0x{:04X}(0x{:04X}){}", property.offset, total,
                                           DefaultSuffix(property)));
        }

        asserts.emplace_back(member, property.offset);
        cursor = property.offset + total;
    }

    EmitPadding(out, cursor, record.size, pad_counter);

    // Declarations only. The bodies go at the end of the file, where every parameter type
    // is complete; a declaration is happy with the forward declarations above.
    if (!record.functions.empty()) {
        std::string declarations;
        for (const auto& fn : record.functions) {
            std::string method_name;
            const std::string decl =
                FunctionDeclaration(record, fn, index, taken, warnings, method_name);
            if (decl.empty()) continue;
            declarations += WithComment(
                decl, IsStaticFunction(fn) ? "// static" : std::string{});
        }
        if (!declarations.empty()) {
            out += "\n";
            out += declarations;
        }
    }

    out += "};\n";

    // The point of the whole emitter: if any derived offset is wrong, this fails to
    // compile instead of misbehaving at runtime.
    if (record.size > 0)
        out += std::format("static_assert(sizeof({}) == 0x{:04X}, \"Wrong size on {}\");\n",
                           name, record.size, name);
    for (const auto& [member, offset] : asserts)
        out += std::format("static_assert(offsetof({}, {}) == 0x{:04X}, "
                           "\"Wrong offset on {}::{}\");\n",
                           name, member, offset, name, member);
    out += "\n";
}

// Appended to Basic.hpp. UE dispatches a reflected call through UObject::ProcessEvent,
// which is virtual, and its vtable index is not in the reflection data. Deriving it would
// mean binary analysis with nothing independent to check the answer against, so the SDK
// names the one thing it cannot know and asks for it once. The function itself is
// addressed by full object path, which the dump does have.
constexpr std::string_view kCallRuntime = R"(
// --- calling reflected functions ------------------------------------------------------
//
// Two hooks stand between a wrapper and the game:
//
//   ZirconSDK::FindFunction   resolve a UFunction by full path, e.g.
//                             "/Script/Engine.PawnMovementComponent.AddInputVector"
//   ZirconSDK::ProcessEvent   invoke it: object, function, parameter block
//
// Injected alongside zircon.dll, both are free:
//
//     ZirconSDK::BindToZirconPayload();
//
// FindFunction comes from the payload, which has already walked the object graph.
// ProcessEvent comes from the vtable slot below, when the dump this SDK was generated
// from carried one.
//
// Each wrapper resolves its UFunction once and caches it. A wrapper on a Static function
// still needs an object to dispatch through; UE uses the class default object for those.
//
// BindToZirconPayload only exists when <windows.h> has already been included. This header
// will not pull it in: it defines several hundred macros, among them ERROR, DELETE,
// GetObject and Rectangle, and a generated SDK is thousands of reflected names that did
// not agree to avoid them. Anything injected has windows.h anyway.

namespace ZirconSDK {

inline void* (*FindFunction)(const char* full_path) = nullptr;
inline void  (*ProcessEvent)(void* object, void* function, void* params) = nullptr;

// UObject::ProcessEvent's vtable slot on the build this SDK was generated from.
//
// -1 when the dump did not carry one, in which case set ProcessEvent yourself. It is not
// a number that can be read out of the engine: it was confirmed by calling a function
// whose answer was known, and only because someone asked for that to happen.
//
// It belongs to this build. An SDK regenerated after a game update carries the slot that
// update has.
inline constexpr int kProcessEventSlot = %PE_SLOT%;

// The ProcessEvent for one object, taken from its own vtable. Every UObject shares the
// implementation unless it overrides it, so any object will do.
inline void CallProcessEvent(void* object, void* function, void* params) {
    auto vtable = *reinterpret_cast<void***>(object);
    auto fn = reinterpret_cast<void(*)(void*, void*, void*)>(vtable[kProcessEventSlot]);
    fn(object, function, params);
}

#ifdef _WINDOWS_
// Fills in FindFunction from zircon.dll when the payload is loaded in this process, which
// is the case for anything injected alongside it. Resolving an object by path is the walk
// the payload has already done, so there is no reason to write it twice.
//
// False when the payload is not there, in which case set FindFunction yourself.
inline bool BindToZirconPayload() {
    HMODULE payload = ::GetModuleHandleA("zircon.dll");
    if (payload == nullptr) return false;

    auto resolve = reinterpret_cast<void* (*)(const char*)>(
        reinterpret_cast<void*>(::GetProcAddress(payload, "zircon_find_object")));
    if (resolve == nullptr) return false;

    FindFunction = resolve;

    // The slot came from the dump, so nothing has to be found at runtime.
    if (kProcessEventSlot >= 0 && ProcessEvent == nullptr) ProcessEvent = &CallProcessEvent;

    return ProcessEvent != nullptr;
}
#endif

// False when the hooks are unset or the function was not found, which is worth being able
// to tell apart from a call that ran and did nothing.
inline bool Call(void* object, void*& cached, const char* full_path, void* params) {
    if (ProcessEvent == nullptr) return false;
    if (cached == nullptr) {
        if (FindFunction == nullptr) return false;
        cached = FindFunction(full_path);
        if (cached == nullptr) return false;
    }
    ProcessEvent(object, cached, params);
    return true;
}

} // namespace ZirconSDK
)";

// Every identifier the SDK emits, guarded so it stops being a macro if it is one.
//
// The rename list above covers the names worth protecting for the caller's sake. It
// cannot cover the rest: Ready Or Not has an enumerator called TRANSPARENT, which wingdi.h
// defines, and no list written against one game predicts the next one. So the names that
// are not renamed get an #undef that only fires when the name really is a macro on
// whichever Windows SDK is compiling.
//
// A user macro that shares a name with a reflected identifier loses. The SDK cannot spell
// the name any other way, and a header that does not compile helps nobody. Anything whose
// loss would actually hurt belongs on the rename list instead, which is what it is for.
std::string UndefHeader(const TypeIndex& index) {
    std::set<std::string> names;

    auto note = [&](std::string_view raw) {
        std::string name = util::SanitizeIdentifier(raw);
        // Renamed already, so the macro is welcome to keep the original spelling.
        if (name.empty() || ClashesWithWindowsMacro(name)) return;
        names.insert(std::move(name));
    };

    for (const auto& [path, record] : index.enums) {
        (void)path;
        for (const auto& value : record->values) {
            std::string entry = value.name;
            const auto scope = entry.rfind("::");
            note(scope == std::string::npos ? entry : entry.substr(scope + 2));
        }
    }

    for (const auto& [path, record] : index.structs) {
        (void)path;
        for (const auto& property : record->properties) note(property.name);
        for (const auto& function : record->functions) {
            note(function.name);
            for (const auto& param : function.params) note(param.name);
        }
    }

    std::string out =
        "#pragma once\n\n"
        "// Reflected names a Windows header may have taken as a macro first.\n"
        "//\n"
        "// Guarded, so nothing happens unless the name really is one. Names whose loss\n"
        "// would hurt the caller are renamed in the SDK instead; see Basic.hpp.\n\n";

    for (const auto& name : names)
        out += std::format("#ifdef {0}\n#undef {0}\n#endif\n", name);

    return out;
}

std::string BasicHeader(const SizeTable& sizes) {
    // Sizes come from the dump, not from constants. The engine does not reflect its own
    // container layouts, but every property using one reports its element size, and those
    // differ between builds: FText is 16 bytes here and 24 elsewhere. A hardcoded value
    // shifts every member following one of these types, without complaint.
    auto size_of = [&](ir::TypeKind kind, std::int32_t fallback) {
        const std::int32_t measured = sizes.For(kind);
        return measured > 0 ? measured : fallback;
    };

    const std::int32_t text      = size_of(ir::TypeKind::Text, 0x18);
    const std::int32_t map       = size_of(ir::TypeKind::Map, 0x50);
    const std::int32_t set       = size_of(ir::TypeKind::Set, 0x50);
    const std::int32_t weak      = size_of(ir::TypeKind::WeakPtr, 0x08);
    const std::int32_t lazy      = size_of(ir::TypeKind::LazyPtr, 0x1C);
    const std::int32_t soft      = size_of(ir::TypeKind::SoftPtr, 0x28);
    const std::int32_t softclass = size_of(ir::TypeKind::SoftClassPtr, 0x28);
    const std::int32_t iface     = size_of(ir::TypeKind::Interface, 0x10);
    const std::int32_t del       = size_of(ir::TypeKind::Delegate, 0x10);
    const std::int32_t multicast = size_of(ir::TypeKind::MulticastDelegate, 0x10);
    const std::int32_t fieldpath = size_of(ir::TypeKind::FieldPath, 0x20);
    const std::int32_t array     = size_of(ir::TypeKind::Array, 0x10);
    const std::int32_t string    = size_of(ir::TypeKind::String, 0x10);

    return std::format(R"(#pragma once

// Zircon SDK - basic types.
//
// The engine does not reflect its own container layouts, so these are reconstructed from
// the sizes that properties using them reported. The static_asserts are the safety net:
// if any of it is wrong for this target, the SDK fails to compile rather than reading the
// wrong bytes at runtime.

#include <cstdint>
#include <cstddef>

using int8   = std::int8_t;
using int16  = std::int16_t;
using int32  = std::int32_t;
using int64  = std::int64_t;
using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

struct FName {{
    uint32 ComparisonIndex;
    uint32 Number;
}};
static_assert(sizeof(FName) == 0x8, "Wrong size on FName");

template <typename T>
struct TArray {{
    T*    Data;
    int32 Count;
    int32 Max;

    int32 Num() const {{ return Count; }}
    T&       operator[](int32 i)       {{ return Data[i]; }}
    const T& operator[](int32 i) const {{ return Data[i]; }}
}};
static_assert(sizeof(TArray<int32>) == 0x{:X}, "Wrong size on TArray");

struct FString : TArray<wchar_t> {{}};
static_assert(sizeof(FString) == 0x{:X}, "Wrong size on FString");

struct FText {{ uint8 Opaque[0x{:X}]; }};

template <typename K, typename V> struct TMap  {{ uint8 Opaque[0x{:X}]; }};
template <typename T>             struct TSet  {{ uint8 Opaque[0x{:X}]; }};

template <typename T> struct TWeakObjectPtr  {{ uint8 Opaque[0x{:X}]; }};
template <typename T> struct TLazyObjectPtr  {{ uint8 Opaque[0x{:X}]; }};
template <typename T> struct TSoftObjectPtr  {{ uint8 Opaque[0x{:X}]; }};
template <typename T> struct TSoftClassPtr   {{ uint8 Opaque[0x{:X}]; }};
template <typename T> struct TScriptInterface{{ uint8 Opaque[0x{:X}]; }};
template <typename T> struct TSubclassOf     {{ class UClass* Class; }};

// TOptional's size depends on its payload, so it cannot be one fixed layout. Members whose
// size disagrees with this are emitted opaque by the size guard instead.
template <typename T> struct TOptional {{ T Value; bool bIsSet; }};

// Named placeholder for an element type the dump could not resolve. Only ever appears
// inside a container, whose size is independent of it.
using FUnresolved = uint8;

struct FDelegate          {{ uint8 Opaque[0x{:X}]; }};
struct FMulticastDelegate {{ uint8 Opaque[0x{:X}]; }};
struct FFieldPath         {{ uint8 Opaque[0x{:X}]; }};
)",
        array, string, text, map, set, weak, lazy, soft, softclass, iface,
        del, multicast, fieldpath);
}

} // namespace

EmitResult EmitCppSdk(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "this dump is partial (no live objects), so an SDK would be "
                       "incomplete; pass --allow-partial to override";
        return result;
    }

    // --- index --------------------------------------------------------------------
    TypeIndex index;
    index.sizes = MeasureSizes(dump);
    std::map<std::string, std::vector<const ir::Struct*>> by_package_structs;
    std::map<std::string, std::vector<const ir::Enum*>>   by_package_enums;

    auto included = [&](const std::string& package) {
        return options.package_filter.empty() ||
               package.find(options.package_filter) != std::string::npos;
    };

    for (const auto& package : dump.packages) {
        if (!included(package.name)) continue;

        for (const auto& record : package.structs) {
            index.structs[record.path] = &record;
            index.package_of[record.path] = package.name;
            by_package_structs[package.name].push_back(&record);
        }
        for (const auto& record : package.classes) {
            index.structs[record.path] = &record;
            index.package_of[record.path] = package.name;
            by_package_structs[package.name].push_back(&record);
        }
        for (const auto& record : package.enums) {
            index.enums[record.path] = &record;
            index.package_of[record.path] = package.name;
            by_package_enums[package.name].push_back(&record);
        }
    }

    if (index.structs.empty() && index.enums.empty()) {
        result.error = "nothing to emit (the package filter matched no types)";
        return result;
    }

    // C++ names are assigned globally so a collision between packages is resolved once,
    // not rediscovered per file.
    {
        std::set<std::string> taken;
        for (const auto& [path, record] : index.structs) {
            const char prefix = util::CppPrefixFor(dump, *record);
            std::string name = prefix + SafeIdentifier(util::LeafName(path));
            if (!taken.insert(name).second) {
                const std::string package = util::PackageFileStem(util::PackageName(path));
                name = std::format("{}_{}", name, package);
                for (int n = 1; !taken.insert(name).second; ++n)
                    name = std::format("{}{}_{}", prefix, util::LeafName(path), n);
            }
            index.cpp_names[path] = name;
        }
        for (const auto& [path, record] : index.enums) {
            (void)record;
            std::string name = SafeIdentifier(util::LeafName(path));
            if (name.empty() || name[0] != 'E') name = "E" + name;
            if (!taken.insert(name).second) {
                for (int n = 1;; ++n) {
                    std::string candidate = std::format("{}_{}", name, n);
                    if (taken.insert(candidate).second) { name = candidate; break; }
                }
            }
            index.cpp_names[path] = name;
        }
    }

    // Accumulated across every package, written once at the end.
    std::string params_blocks;
    std::string function_bodies;

    std::string error;
    const std::string root = options.out_dir + "/SDK";
    if (!util::EnsureDirectory(root, error)) { result.error = error; return result; }

    if (!util::WriteFile(root + "/Undef.hpp", UndefHeader(index), error)) {
        result.error = error;
        return result;
    }
    result.files.push_back(root + "/Undef.hpp");

    // The one number in the runtime that is not the same for every target.
    int process_event_slot = -1;
    for (const auto& offset : dump.header.offsets)
        if (offset.name == "UObject.ProcessEvent") process_event_slot = offset.value;

    std::string call_runtime{kCallRuntime};
    if (const auto at = call_runtime.find("%PE_SLOT%"); at != std::string::npos)
        call_runtime.replace(at, std::strlen("%PE_SLOT%"),
                             std::format("{}", process_event_slot));

    if (!util::WriteFile(root + "/Basic.hpp",
                         "#pragma once\n#include \"Undef.hpp\"\n" +
                             BasicHeader(index.sizes) + call_runtime,
                         error)) {
        result.error = error;
        return result;
    }
    result.files.push_back(root + "/Basic.hpp");

    // --- per package --------------------------------------------------------------
    std::vector<std::string> emitted_packages;

    for (const auto& [package, records] : by_package_structs) {
        const std::string stem = util::PackageFileStem(package);

        std::string out;
        out += std::format("#pragma once\n\n// Package {}\n// Generated by Zircon {}\n\n",
                           package, dump.header.tool_version);
        // A type held *by value* — a base class, a struct member, an enum member — must be
        // complete, and it frequently lives in another package. Without these includes the
        // header only compiles by accident, when some other header happened to pull the
        // dependency in first.
        //
        // Pointers deliberately do not count: they need only a forward declaration, and
        // treating them as dependencies would make almost every package depend on almost
        // every other.
        std::set<std::string> dependency_packages;
        for (const auto* record : records) {
            std::set<std::string> deps;
            if (!record->super.empty()) deps.insert(record->super);
            for (const auto& property : record->properties)
                CollectValueDependencies(property.type, deps);

            // Function signatures name types by value too. A method declared here that
            // returns a struct from another package needs that package included, and a
            // property never referencing it is no reason to leave it out.
            for (const auto& fn : record->functions)
                for (const auto& param : fn.params)
                    CollectValueDependencies(param.type, deps);

            for (const auto& dep : deps) {
                const auto it = index.package_of.find(dep);
                if (it == index.package_of.end()) continue;
                if (it->second == package) continue;
                dependency_packages.insert(util::PackageFileStem(it->second));
            }
        }

        out += "#include \"Basic.hpp\"\n";
        for (const auto& dep : dependency_packages)
            out += std::format("#include \"{}.hpp\"\n", dep);
        out += "\n";

        // Forward declarations cover every class referenced by pointer, including ones in
        // other packages, which is what keeps per-package headers independent of include
        // order.
        std::set<std::string> forwards;
        {
            std::function<void(const ir::TypeRef&)> collect = [&](const ir::TypeRef& type) {
                const bool is_pointer =
                    type.kind == ir::TypeKind::ObjectPtr || type.kind == ir::TypeKind::ClassPtr ||
                    type.kind == ir::TypeKind::WeakPtr   || type.kind == ir::TypeKind::LazyPtr ||
                    type.kind == ir::TypeKind::SoftPtr   || type.kind == ir::TypeKind::SoftClassPtr ||
                    type.kind == ir::TypeKind::Interface;
                if (is_pointer && !type.name.empty()) {
                    const auto it = index.cpp_names.find(type.name);
                    if (it != index.cpp_names.end()) forwards.insert(it->second);
                }
                for (const auto& param : type.params) collect(param);
            };

            // Function signatures reference classes by pointer too, and a wrapper is
            // declared inside the class where only the forward declaration is in scope.
            for (const auto* record : records)
                for (const auto& fn : record->functions)
                    for (const auto& param : fn.params) collect(param.type);
        }

        // Every type named in a function signature, declared ahead of the first class that
        // mentions it.
        //
        // Not only the ones from other packages: a type defined later in this same header
        // is just as unavailable to a method declared earlier, and the sort that orders
        // definitions has no reason to agree with the order signatures reference them in.
        // Declaring a type that is defined further down is harmless.
        std::set<std::string> forward_structs;
        std::map<std::string, std::string> forward_enums;   // name -> underlying
        {
            std::function<void(const ir::TypeRef&)> collect = [&](const ir::TypeRef& type) {
                if (!type.name.empty()) {
                    if (type.kind == ir::TypeKind::Struct) {
                        const auto it = index.cpp_names.find(type.name);
                        if (it != index.cpp_names.end()) forward_structs.insert(it->second);
                    } else if (type.kind == ir::TypeKind::Enum) {
                        const auto named = index.cpp_names.find(type.name);
                        const auto record = index.enums.find(type.name);
                        if (named != index.cpp_names.end() && record != index.enums.end())
                            forward_enums[named->second] =
                                std::string(EnumUnderlying(*record->second));
                    }
                }
                for (const auto& param : type.params) collect(param);
            };

            for (const auto* record : records)
                for (const auto& fn : record->functions)
                    for (const auto& param : fn.params) collect(param.type);

        }

        for (const auto* record : records)
            for (const auto& property : record->properties) {
                std::function<void(const ir::TypeRef&)> walk = [&](const ir::TypeRef& type) {
                    const bool is_pointer =
                        type.kind == ir::TypeKind::ObjectPtr || type.kind == ir::TypeKind::ClassPtr ||
                        type.kind == ir::TypeKind::WeakPtr   || type.kind == ir::TypeKind::LazyPtr ||
                        type.kind == ir::TypeKind::SoftPtr   || type.kind == ir::TypeKind::SoftClassPtr ||
                        type.kind == ir::TypeKind::Interface;
                    if (is_pointer && !type.name.empty()) {
                        const auto it = index.cpp_names.find(type.name);
                        if (it != index.cpp_names.end()) forwards.insert(it->second);
                    }
                    for (const auto& param : type.params) walk(param);
                };
                walk(property.type);
            }

        // "struct", matching how every type is defined below. Declaring one as "class"
        // and defining it as "struct" is a mismatch MSVC reports at /W4.
        if (!forwards.empty()) {
            for (const auto& name : forwards) out += std::format("struct {};\n", name);
            out += "\n";
        }

        const auto enums = by_package_enums.find(package);
        if (enums != by_package_enums.end())
            for (const auto* record : enums->second) EmitEnum(out, *record, index);

        // Generated layouts are byte-exact: padding is computed from the engine's real
        // offsets, so the compiler must not insert any of its own. Without this, a double
        // following three bytes of padding lands at 8 rather than 3 and every subsequent
        // offset assert fails. Basic.hpp stays outside the pragma on purpose — those types
        // are hand-written at natural alignment.
        out += "#pragma pack(push, 1)\n\n";

        const auto sorted = TopoSort(records, result.warnings);
        for (const auto* record : sorted)
            EmitStruct(out, *record, index, result.warnings);

        out += "#pragma pack(pop)\n";

        // Parameter blocks and bodies go to Functions.hpp instead of here. A body needs
        // every signature type complete, and a package header sees only Basic.hpp plus
        // forward declarations.
        for (const auto* record : sorted) {
            std::set<std::string> taken;
            for (const auto& fn : record->functions) {
                std::string method_name;
                const std::string decl =
                    FunctionDeclaration(*record, fn, index, taken, result.warnings,
                                        method_name);
                if (decl.empty()) continue;
                EmitParamsStruct(params_blocks, *record, fn, index, result.warnings);
                EmitFunctionBody(function_bodies, *record, fn, method_name, index,
                                 result.warnings);
            }
        }

        const std::string path = std::format("{}/{}.hpp", root, stem);
        if (!util::WriteFile(path, out, error)) { result.error = error; return result; }
        result.files.push_back(path);
        emitted_packages.push_back(stem);
    }

    // Also emit enum-only packages, which the struct loop above skips entirely.
    for (const auto& [package, enums] : by_package_enums) {
        if (by_package_structs.count(package)) continue;

        const std::string stem = util::PackageFileStem(package);
        std::string out = std::format("#pragma once\n\n// Package {}\n\n#include \"Basic.hpp\"\n\n",
                                      package);
        for (const auto* record : enums) EmitEnum(out, *record, index);

        const std::string path = std::format("{}/{}.hpp", root, stem);
        if (!util::WriteFile(path, out, error)) { result.error = error; return result; }
        result.files.push_back(path);
        emitted_packages.push_back(stem);
    }

    // --- function wrappers ------------------------------------------------------------
    //
    // One header, included after every package, so a parameter type is complete whichever
    // package the engine put it in. Splitting these per package would need the packages to
    // include one another, and an include graph across six hundred headers is worse than
    // one file that arrives last.
    std::sort(emitted_packages.begin(), emitted_packages.end());
    emitted_packages.erase(std::unique(emitted_packages.begin(), emitted_packages.end()),
                           emitted_packages.end());

    if (!function_bodies.empty()) {
        std::string functions =
            "#pragma once\n\n"
            "// Wrappers for every reflected function, and the parameter block each one\n"
            "// passes. Included after all packages: a body needs its parameter types\n"
            "// complete, and those come from wherever the engine put them.\n"
            "//\n"
            "// Set ZirconSDK::FindFunction and ZirconSDK::ProcessEvent once before calling\n"
            "// any of these. See Basic.hpp.\n\n";

        for (const auto& stem : emitted_packages)
            functions += std::format("#include \"{}.hpp\"\n", stem);

        // Parameter blocks sit at engine offsets, so the packing pragma applies to them
        // exactly as it does to any other reflected type.
        functions += "\n#pragma pack(push, 1)\n\n";
        functions += params_blocks;
        functions += "#pragma pack(pop)\n\n";
        functions += function_bodies;

        const std::string functions_path = root + "/Functions.hpp";
        if (!util::WriteFile(functions_path, functions, error)) {
            result.error = error;
            return result;
        }
        result.files.push_back(functions_path);
    }

    // --- umbrella header ----------------------------------------------------------
    std::sort(emitted_packages.begin(), emitted_packages.end());
    emitted_packages.erase(std::unique(emitted_packages.begin(), emitted_packages.end()),
                           emitted_packages.end());

    std::string sdk = std::format(
        "#pragma once\n\n"
        "// Zircon SDK for {}\n"
        "// Engine {} (confidence {:.0f}%), generated {}\n"
        "//\n"
        "// {} packages, {} classes, {} structs, {} enums\n\n"
        "#include \"SDK/Basic.hpp\"\n\n",
        dump.header.source.process, dump.header.engine.version,
        dump.header.engine.confidence * 100.0, dump.header.created_utc,
        emitted_packages.size(), dump.TotalClasses(), dump.TotalStructs(), dump.TotalEnums());

    for (const auto& stem : emitted_packages) sdk += std::format("#include \"SDK/{}.hpp\"\n", stem);
    if (!function_bodies.empty()) sdk += "\n#include \"SDK/Functions.hpp\"\n";

    const std::string sdk_path = options.out_dir + "/SDK.hpp";
    if (!util::WriteFile(sdk_path, sdk, error)) { result.error = error; return result; }
    result.files.push_back(sdk_path);

    return result;
}

} // namespace zircon::emit
