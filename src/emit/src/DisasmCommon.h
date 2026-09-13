#pragma once

// Shared scaffolding for the two disassembler emitters, IDA and Ghidra. They differ in how
// they express a type - C declaration text against DataType API calls - but the hard parts
// are the same: deciding what goes where, keeping offsets exact, and refusing to name a
// shared thunk after one of the hundreds of functions pointing at it.
//
// Header-only, so no build-system entry. Private to src/emit/src.

#include "emit/Emitter.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit::disasm {

// FUNC_Native. Without this bit a UFunction is script-only and its Func pointer is the
// interpreter thunk shared by every such function, not its own implementation.
inline constexpr std::uint32_t kFuncNative = 0x00000400;

// ---------------------------------------------------------------------------------
// Python literals
// ---------------------------------------------------------------------------------

// Everything outside printable ASCII gets escaped, keeping the generated file pure ASCII.
// Ghidra runs Jython 2.7, where a stray high byte with no source-encoding declaration is a
// syntax error.
inline std::string PyQuote(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');

    for (const unsigned char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20 || c >= 0x7F) out += std::format("\\x{:02x}", c);
                else                       out.push_back(static_cast<char>(c));
        }
    }

    out.push_back('"');
    return out;
}

// ---------------------------------------------------------------------------------
// Model: the filtered, uniquely named set of types the script will define
// ---------------------------------------------------------------------------------

struct Model {
    std::vector<const ir::Struct*> types;   // classes and script structs, in dump order
    std::vector<const ir::Enum*>   enums;

    std::unordered_map<std::string, const ir::Struct*> struct_by_path;
    std::unordered_map<std::string, const ir::Enum*>   enum_by_path;

    // Globally unique, sanitised, prefixed. Two packages can both declare `Foo`, and a
    // disassembler type namespace is flat.
    std::unordered_map<std::string, std::string> struct_names;
    std::unordered_map<std::string, std::string> enum_names;

    const std::string* StructName(const std::string& path) const {
        const auto it = struct_names.find(path);
        return it == struct_names.end() ? nullptr : &it->second;
    }
    const std::string* EnumName(const std::string& path) const {
        const auto it = enum_names.find(path);
        return it == enum_names.end() ? nullptr : &it->second;
    }
    const ir::Struct* Struct(const std::string& path) const {
        const auto it = struct_by_path.find(path);
        return it == struct_by_path.end() ? nullptr : it->second;
    }
    const ir::Enum* EnumAt(const std::string& path) const {
        const auto it = enum_by_path.find(path);
        return it == enum_by_path.end() ? nullptr : it->second;
    }
};

// The UE C++ prefix, against a prebuilt index.
//
// util::CppPrefixFor rebuilds a path->struct map per call, O(n) per type and so O(n^2) over
// a dump with ten thousand of them. Same rule, resolved once.
inline char PrefixFor(const Model& model, const ir::Struct& record) {
    if (!record.is_class) return 'F';

    const ir::Struct* current = &record;
    std::unordered_set<std::string> seen;

    while (current) {
        if (current->path == "/Script/Engine.Actor")          return 'A';
        if (current->path == "/Script/CoreUObject.Interface") return 'I';
        if (current->super.empty()) break;
        if (!seen.insert(current->path).second) break;   // guards a cyclic dump
        current = model.Struct(current->super);
    }
    return 'U';
}

inline bool PackageAccepted(std::string_view package, const EmitOptions& options) {
    return options.package_filter.empty() ||
           package.find(options.package_filter) != std::string_view::npos;
}

inline Model BuildModel(const ir::Dump& dump, const EmitOptions& options,
                        std::vector<std::string>& warnings) {
    Model model;

    for (const auto& package : dump.packages) {
        if (!PackageAccepted(package.name, options)) continue;

        for (const auto& record : package.classes) {
            model.types.push_back(&record);
            model.struct_by_path.emplace(record.path, &record);
        }
        for (const auto& record : package.structs) {
            model.types.push_back(&record);
            model.struct_by_path.emplace(record.path, &record);
        }
        for (const auto& record : package.enums) {
            model.enums.push_back(&record);
            model.enum_by_path.emplace(record.path, &record);
        }
    }

    // Names only after every type is indexed: the prefix depends on the super chain, and a
    // base may live in a package listed later.
    std::unordered_set<std::string> used;

    const auto claim = [&used](std::string candidate, std::string_view package) {
        if (used.insert(candidate).second) return candidate;

        // Package first, since that still reads as a name. Counter only if it collides too.
        std::string qualified =
            candidate + "_" + util::SanitizeIdentifier(util::PackageFileStem(package));
        if (used.insert(qualified).second) return qualified;

        for (int index = 2;; ++index) {
            std::string numbered = std::format("{}_{}", qualified, index);
            if (used.insert(numbered).second) return numbered;
        }
    };

    for (const auto* record : model.types) {
        const std::string leaf = util::SanitizeIdentifier(util::LeafName(record->path));
        const std::string candidate = std::string(1, PrefixFor(model, *record)) + leaf;
        model.struct_names.emplace(record->path,
                                   claim(candidate, util::PackageName(record->path)));
    }

    for (const auto* record : model.enums) {
        std::string leaf = util::SanitizeIdentifier(util::LeafName(record->path));
        if (leaf.empty() || leaf[0] != 'E') leaf = "E" + leaf;
        model.enum_names.emplace(record->path,
                                 claim(leaf, util::PackageName(record->path)));
    }

    if (model.types.empty() && model.enums.empty()) {
        warnings.push_back(options.package_filter.empty()
            ? "the dump contains no types"
            : "no package matched the filter '" + options.package_filter + "'");
    }
    return model;
}

// ---------------------------------------------------------------------------------
// Enum widths
// ---------------------------------------------------------------------------------

inline int WidthFromUnderlying(std::string_view underlying) {
    if (underlying == "uint8"  || underlying == "int8")  return 1;
    if (underlying == "uint16" || underlying == "int16") return 2;
    if (underlying == "uint32" || underlying == "int32") return 4;
    if (underlying == "uint64" || underlying == "int64") return 8;
    return 1;   // UE's default underlying type is uint8
}

inline bool UnderlyingIsSigned(std::string_view underlying) {
    return !underlying.empty() && underlying[0] == 'i';
}

// What the enum must actually occupy. Declaring `: unsigned __int8` then listing a value of
// 300 is a parse error, and UE enums do carry sentinels well outside their nominal range,
// so widen the declared width to whatever the values need.
inline int EnumWidth(const ir::Enum& record) {
    int width = WidthFromUnderlying(record.underlying);
    const bool is_signed = UnderlyingIsSigned(record.underlying);

    for (const auto& value : record.values) {
        int needed = 1;
        if (is_signed) {
            if (value.value < -128 || value.value > 127)             needed = 2;
            if (value.value < -32768 || value.value > 32767)         needed = 4;
            if (value.value < -2147483648LL || value.value > 2147483647LL) needed = 8;
        } else {
            const std::uint64_t raw = static_cast<std::uint64_t>(value.value);
            if (value.value < 0)          needed = 8;   // a negative in an unsigned enum
            else if (raw > 0xFFFFFFFFull) needed = 8;
            else if (raw > 0xFFFFull)     needed = 4;
            else if (raw > 0xFFull)       needed = 2;
        }
        width = std::max(width, needed);
    }
    return width;
}

// ---------------------------------------------------------------------------------
// Readable type rendering, used in comments
// ---------------------------------------------------------------------------------

inline std::string Describe(const ir::TypeRef& type) {
    const auto leaf = [](const std::string& path) {
        return path.empty() ? std::string("?") : util::LeafName(path);
    };
    const auto inner = [&type](std::size_t index) -> std::string {
        return index < type.params.size() ? Describe(type.params[index]) : std::string("?");
    };

    switch (type.kind) {
        case ir::TypeKind::Bool:    return "bool";
        case ir::TypeKind::Int8:    return "int8";
        case ir::TypeKind::Int16:   return "int16";
        case ir::TypeKind::Int32:   return "int32";
        case ir::TypeKind::Int64:   return "int64";
        case ir::TypeKind::UInt8:   return "uint8";
        case ir::TypeKind::UInt16:  return "uint16";
        case ir::TypeKind::UInt32:  return "uint32";
        case ir::TypeKind::UInt64:  return "uint64";
        case ir::TypeKind::Float:   return "float";
        case ir::TypeKind::Double:  return "double";
        case ir::TypeKind::Name:    return "FName";
        case ir::TypeKind::String:  return "FString";
        case ir::TypeKind::Text:    return "FText";
        case ir::TypeKind::Enum:    return leaf(type.name);
        case ir::TypeKind::Struct:  return "F" + leaf(type.name);
        case ir::TypeKind::ObjectPtr:    return leaf(type.name) + "*";
        case ir::TypeKind::WeakPtr:      return "TWeakObjectPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::LazyPtr:      return "TLazyObjectPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::SoftPtr:      return "TSoftObjectPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::SoftClassPtr: return "TSoftClassPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::ClassPtr:     return "TSubclassOf<" + leaf(type.name) + ">";
        case ir::TypeKind::Interface:    return "TScriptInterface<" + leaf(type.name) + ">";
        case ir::TypeKind::Array:        return "TArray<" + inner(0) + ">";
        case ir::TypeKind::Set:          return "TSet<" + inner(0) + ">";
        case ir::TypeKind::Map:          return "TMap<" + inner(0) + ", " + inner(1) + ">";
        case ir::TypeKind::Delegate:            return "FDelegate";
        case ir::TypeKind::MulticastDelegate:   return "FMulticastDelegate";
        case ir::TypeKind::FieldPath:           return "TFieldPath";
        case ir::TypeKind::Optional:            return "TOptional<" + inner(0) + ">";
        case ir::TypeKind::Unknown:
            return type.raw.empty() ? "unknown" : type.raw;
    }
    return type.raw.empty() ? "unknown" : type.raw;
}

inline std::string Signature(const ir::Function& function) {
    std::string returns = "void";
    std::string params;

    for (const auto& param : function.params) {
        if (param.is_return) {
            returns = Describe(param.type);
            continue;
        }
        if (!params.empty()) params += ", ";
        if (param.is_out)   params += "out ";
        if (param.is_const) params += "const ";
        params += Describe(param.type) + " " + param.name;
    }
    return returns + " " + function.name + "(" + params + ")";
}

// ---------------------------------------------------------------------------------
// Member layout
// ---------------------------------------------------------------------------------

struct MemberSlot {
    std::int32_t offset{0};
    std::int32_t size{0};
    std::string  name;

    const ir::Property* property{nullptr};   // null for padding and for the base slot

    // Every bool packed into this byte. More than one and it's a bitfield: emit the byte
    // once for the whole byte.
    std::vector<const ir::Property*> packed;

    bool padding{false};
    bool base{false};
    std::string base_path;     // set when base is true and the base type is known
};

struct Layout {
    std::vector<MemberSlot> slots;
    std::int32_t total_size{0};   // may exceed record.size if a member overruns
};

// Builds a gap-free, non-overlapping cover of the struct.
//
// Exactness is the entire value of these emitters: a member at the wrong offset is worse
// than no member, because everything a user reads through it comes back mistyped. So a
// property that cannot be placed is dropped with a warning rather than shifted, and gaps
// become explicit padding rather than being left to the disassembler to guess.
inline Layout ComputeLayout(const Model& model, const ir::Struct& record,
                            std::vector<std::string>& warnings) {
    Layout layout;
    std::int32_t cursor = 0;

    const auto pad_to = [&](std::int32_t target) {
        if (target <= cursor) return;
        MemberSlot slot;
        slot.offset  = cursor;
        slot.size    = target - cursor;
        slot.padding = true;
        slot.name    = std::format("pad_{:04X}", static_cast<unsigned>(cursor));
        layout.slots.push_back(std::move(slot));
        cursor = target;
    };

    // The inherited region. Embedding the base as a member gives the disassembler real
    // type propagation into inherited fields; it is only safe when the base's own size
    // agrees with where this type says its members start.
    if (record.inherited_size > 0) {
        const ir::Struct* base = record.super.empty() ? nullptr : model.Struct(record.super);
        const bool usable = base && base->size == record.inherited_size &&
                            model.StructName(record.super) != nullptr;

        MemberSlot slot;
        slot.offset = 0;
        slot.size   = record.inherited_size;
        slot.name   = "baseclass_0";
        if (usable) {
            slot.base      = true;
            slot.base_path = record.super;
        } else {
            slot.padding = true;
            slot.name    = "pad_base";
            if (base && base->size != record.inherited_size) {
                warnings.push_back(std::format(
                    "{}: base {} is {} bytes but members start at {}; emitting padding "
                    "instead of the base type", record.path, record.super, base->size,
                    record.inherited_size));
            }
        }
        layout.slots.push_back(std::move(slot));
        cursor = record.inherited_size;
    }

    // Group by offset so packed bools collapse into the single byte they share.
    std::vector<const ir::Property*> ordered;
    ordered.reserve(record.properties.size());
    for (const auto& property : record.properties) ordered.push_back(&property);

    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ir::Property* a, const ir::Property* b) {
                         return a->offset < b->offset;
                     });

    std::unordered_set<std::string> used_names;
    used_names.insert("baseclass_0");

    for (std::size_t i = 0; i < ordered.size();) {
        const std::int32_t offset = ordered[i]->offset;

        std::size_t end = i;
        while (end < ordered.size() && ordered[end]->offset == offset) ++end;

        const std::size_t group = end - i;
        std::vector<const ir::Property*> members(ordered.begin() + static_cast<std::ptrdiff_t>(i),
                                                 ordered.begin() + static_cast<std::ptrdiff_t>(end));
        i = end;

        if (offset < cursor) {
            warnings.push_back(std::format(
                "{}: property '{}' at offset {} overlaps the preceding member ending at "
                "{}; skipped", record.path, members.front()->name, offset, cursor));
            continue;
        }

        std::int32_t size = 0;
        bool bitfield = false;

        if (group > 1) {
            // Several properties at one offset is only legitimate for packed bools, which
            // each occupy a distinct bit of the same byte.
            const bool all_packed = std::all_of(members.begin(), members.end(),
                [](const ir::Property* p) { return p->is_bitfield; });
            if (!all_packed) {
                warnings.push_back(std::format(
                    "{}: {} properties share offset {} but are not all bitfields; keeping "
                    "'{}' only", record.path, group, offset, members.front()->name));
                members.resize(1);
            } else {
                bitfield = true;
                size = 1;
            }
        }

        if (!bitfield) {
            const ir::Property& property = *members.front();
            size = property.size;
            if (size <= 0) size = property.type.size * std::max(1, property.array_dim);
            if (size <= 0) {
                warnings.push_back(std::format(
                    "{}: property '{}' has no usable size; skipped",
                    record.path, property.name));
                continue;
            }
        }

        pad_to(offset);

        MemberSlot slot;
        slot.offset   = offset;
        slot.size     = size;
        slot.property = members.front();
        if (bitfield) slot.packed = members;

        std::string name = util::SanitizeIdentifier(members.front()->name);
        // A duplicate member name is a hard error in both a C declaration and a Ghidra
        // structure, and UE does ship structs with repeated property names.
        if (!used_names.insert(name).second) {
            for (int suffix = 1;; ++suffix) {
                std::string candidate = std::format("{}_{}", name, suffix);
                if (used_names.insert(candidate).second) { name = candidate; break; }
            }
        }
        slot.name = std::move(name);

        layout.slots.push_back(std::move(slot));
        cursor = offset + size;
    }

    if (cursor > record.size && record.size > 0) {
        warnings.push_back(std::format(
            "{}: members reach {} bytes but the type reports {}; using the larger size",
            record.path, cursor, record.size));
    }

    layout.total_size = std::max(record.size, cursor);
    pad_to(layout.total_size);
    return layout;
}

// ---------------------------------------------------------------------------------
// Dependency order
// ---------------------------------------------------------------------------------

// Depth-first post-order over "needs a complete definition of". A pointer member does not
// create one; an embedded base or a by-value struct member does.
inline std::vector<const ir::Struct*> TopoSort(const Model& model,
                                               std::vector<std::string>& warnings) {
    std::vector<const ir::Struct*> ordered;
    ordered.reserve(model.types.size());

    enum class Mark : std::uint8_t { None, Open, Done };
    std::unordered_map<const ir::Struct*, Mark> marks;

    // Explicit stack, not recursion: a deep inheritance chain in a hostile or
    // corrupt dump would otherwise overflow the stack.
    struct Frame {
        const ir::Struct* record;
        std::vector<const ir::Struct*> deps;
        std::size_t next{0};
    };

    const auto dependencies = [&model](const ir::Struct& record) {
        std::vector<const ir::Struct*> deps;
        if (!record.super.empty() && record.inherited_size > 0) {
            if (const ir::Struct* base = model.Struct(record.super)) {
                if (base->size == record.inherited_size) deps.push_back(base);
            }
        }
        for (const auto& property : record.properties) {
            if (property.type.kind != ir::TypeKind::Struct) continue;
            if (const ir::Struct* used = model.Struct(property.type.name)) {
                if (used->size == property.type.size) deps.push_back(used);
            }
        }
        return deps;
    };

    for (const auto* root : model.types) {
        if (marks[root] == Mark::Done) continue;

        std::vector<Frame> stack;
        stack.push_back(Frame{root, dependencies(*root), 0});
        marks[root] = Mark::Open;

        while (!stack.empty()) {
            Frame& frame = stack.back();

            if (frame.next < frame.deps.size()) {
                const ir::Struct* dep = frame.deps[frame.next++];
                const Mark mark = marks[dep];

                if (mark == Mark::Done) continue;
                if (mark == Mark::Open) {
                    // A cycle cannot be satisfied by ordering. Emitting anyway is correct
                    // here because the member that closed the loop is byte-sized padding
                    // in practice, but the user should know the dump contains one.
                    warnings.push_back(std::format(
                        "cyclic type dependency involving {} and {}; definition order is "
                        "arbitrary for these", frame.record->path, dep->path));
                    continue;
                }

                marks[dep] = Mark::Open;
                stack.push_back(Frame{dep, dependencies(*dep), 0});
                continue;
            }

            marks[frame.record] = Mark::Done;
            ordered.push_back(frame.record);
            stack.pop_back();
        }
    }
    return ordered;
}

// ---------------------------------------------------------------------------------
// Function sites
// ---------------------------------------------------------------------------------

struct FunctionSite {
    std::uint64_t rva{0};
    std::string   name;        // ClassName_FunctionName
    std::string   signature;
    std::string   path;        // owning class, for the comment
};

// Collects the addresses worth renaming.
//
// Two filters, both load-bearing. Script-only functions point at the interpreter thunk,
// not at their own code, so renaming that address after one of them would mislabel the
// shared entry point for every Blueprint function in the binary. And any address claimed
// by more than one UFunction is likewise not a unique implementation, so it is left
// alone regardless of flags.
inline std::vector<FunctionSite> CollectFunctions(const ir::Dump& dump,
                                                  const EmitOptions& options,
                                                  const Model& model,
                                                  std::vector<std::string>& warnings) {
    struct Candidate {
        const ir::Struct*   owner{};
        const ir::Function* function{};
    };
    std::map<std::uint64_t, std::vector<Candidate>> by_rva;

    std::size_t script_only = 0;

    for (const auto& package : dump.packages) {
        if (!PackageAccepted(package.name, options)) continue;

        for (const auto& record : package.classes) {
            for (const auto& function : record.functions) {
                if (function.native_rva == 0) continue;
                if (!(function.flags & kFuncNative)) { ++script_only; continue; }
                by_rva[function.native_rva].push_back(Candidate{&record, &function});
            }
        }
    }

    std::vector<FunctionSite> sites;
    std::size_t shared = 0;
    std::unordered_set<std::string> used_names;

    for (const auto& [rva, candidates] : by_rva) {
        if (candidates.size() > 1) {
            shared += candidates.size();
            continue;
        }

        const auto& only = candidates.front();
        const std::string* type_name = model.StructName(only.owner->path);
        const std::string owner = type_name ? *type_name
                                            : util::SanitizeIdentifier(only.owner->name);

        std::string name = owner + "_" + util::SanitizeIdentifier(only.function->name);
        if (!used_names.insert(name).second) {
            for (int suffix = 1;; ++suffix) {
                std::string candidate = std::format("{}_{}", name, suffix);
                if (used_names.insert(candidate).second) { name = candidate; break; }
            }
        }

        sites.push_back(FunctionSite{rva, std::move(name), Signature(*only.function),
                                     only.owner->path});
    }

    if (script_only > 0) {
        warnings.push_back(std::format(
            "{} script-only functions were not renamed: their Func pointer is the shared "
            "interpreter thunk, not their own code", script_only));
    }
    if (shared > 0) {
        warnings.push_back(std::format(
            "{} native functions share an address with another and were not renamed",
            shared));
    }
    return sites;
}

// ---------------------------------------------------------------------------------
// Shared preflight
// ---------------------------------------------------------------------------------

// Returns an error string when the dump cannot support an import, empty otherwise.
inline std::string Preflight(const ir::Dump& dump, const EmitOptions& options) {
    if (dump.header.partial && !options.allow_partial) {
        return "this dump is partial (a static image has no live objects), so there are "
               "no types to import; pass allow_partial to emit anyway";
    }
    return {};
}

} // namespace zircon::emit::disasm
