#include "Emitters.h"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>
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

// Unique within its owner and legal in C++. UE lets two properties differ only by
// characters the sanitizer collapses, so enforce uniqueness instead of hoping for it.
std::string UniqueMember(std::string_view raw, std::set<std::string>& taken) {
    std::string name = util::SanitizeIdentifier(raw);
    if (taken.insert(name).second) return name;

    for (int suffix = 1;; ++suffix) {
        std::string candidate = std::format("{}_{}", name, suffix);
        if (taken.insert(candidate).second) return candidate;
    }
}

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
            return it == index.enums.end() ? 0 : UnderlyingSize(it->second->underlying);
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
            const std::int32_t declared = UnderlyingSize(it->second->underlying);
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

void EmitEnum(std::string& out, const ir::Enum& record, const TypeIndex& index) {
    const std::string name = index.cpp_names.at(record.path);

    out += std::format("// {}\n", record.path);
    out += std::format("enum class {} : {} {{\n", name,
                       record.underlying.empty() ? "uint8" : record.underlying);

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
            std::string name = prefix + util::SanitizeIdentifier(util::LeafName(path));
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
            std::string name = util::SanitizeIdentifier(util::LeafName(path));
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

    std::string error;
    const std::string root = options.out_dir + "/SDK";
    if (!util::EnsureDirectory(root, error)) { result.error = error; return result; }

    if (!util::WriteFile(root + "/Basic.hpp", BasicHeader(index.sizes), error)) {
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

        if (!forwards.empty()) {
            for (const auto& name : forwards) out += std::format("class {};\n", name);
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

        for (const auto* record : TopoSort(records, result.warnings))
            EmitStruct(out, *record, index, result.warnings);

        out += "#pragma pack(pop)\n";

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

    const std::string sdk_path = options.out_dir + "/SDK.hpp";
    if (!util::WriteFile(sdk_path, sdk, error)) { result.error = error; return result; }
    result.files.push_back(sdk_path);

    return result;
}

} // namespace zircon::emit
