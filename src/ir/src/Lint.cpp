#include "ir/Lint.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::ir {
namespace {

struct Context {
    LintReport report;

    std::unordered_map<std::string, const Struct*> types;
    std::unordered_set<std::string> enums;

    void Add(LintSeverity severity, std::string check, std::string where,
             std::string detail) {
        if (severity == LintSeverity::Error) ++report.errors;
        else                                 ++report.warnings;
        report.findings.push_back(LintFinding{severity, std::move(check), std::move(where),
                                              std::move(detail)});
    }

    void Error(std::string check, std::string where, std::string detail) {
        Add(LintSeverity::Error, std::move(check), std::move(where), std::move(detail));
    }
    void Warn(std::string check, std::string where, std::string detail) {
        Add(LintSeverity::Warning, std::move(check), std::move(where), std::move(detail));
    }
};

bool FitsUnderlying(std::string_view underlying, std::int64_t value) {
    const bool is_signed = !underlying.empty() && underlying[0] == 'i';
    int bits = 8;
    if (underlying == "uint16" || underlying == "int16") bits = 16;
    if (underlying == "uint32" || underlying == "int32") bits = 32;
    if (underlying == "uint64" || underlying == "int64") bits = 64;
    if (bits == 64) return true;

    if (is_signed) {
        const std::int64_t limit = std::int64_t(1) << (bits - 1);
        return value >= -limit && value < limit;
    }
    if (value < 0) return false;
    return value < (std::int64_t(1) << bits);
}

// only the kinds whose `name` is supposed to name something in this dump. a container's
// own name is empty, and Unknown keeps the engine's class name, which isn't a path.
bool NamesAType(TypeKind kind) {
    switch (kind) {
        case TypeKind::Struct:
        case TypeKind::ObjectPtr:
        case TypeKind::ClassPtr:
        case TypeKind::WeakPtr:
        case TypeKind::LazyPtr:
        case TypeKind::SoftPtr:
        case TypeKind::SoftClassPtr:
        case TypeKind::Interface:
            return true;
        default:
            return false;
    }
}

void CheckTypeRef(Context& context, const TypeRef& type, const std::string& where,
                  int depth = 0) {
    if (depth > 16) {
        context.Error("type-too-deep", where,
                      "the type tree nests more than 16 deep, which no UE property does");
        return;
    }

    if (type.kind == TypeKind::Enum) {
        if (type.name.empty())
            context.Error("unnamed-enum-ref", where, "an enum property names no enum");
        else if (!context.enums.count(type.name))
            context.Warn("dangling-type-ref", where,
                         std::format("enum '{}' is not in this dump", type.name));
    } else if (NamesAType(type.kind)) {
        if (type.name.empty()) {
            // object pointer with no class is fine, UE has properties typed on
            // UObject itself. a struct with no struct isn't.
            if (type.kind == TypeKind::Struct)
                context.Error("unnamed-struct-ref", where,
                              "a struct property names no struct");
        } else if (!context.types.count(type.name)) {
            context.Warn("dangling-type-ref", where,
                         std::format("type '{}' is not in this dump", type.name));
        }
    }

    for (const auto& param : type.params) CheckTypeRef(context, param, where, depth + 1);
}

void CheckEnum(Context& context, const Enum& record) {
    ++context.report.enums_checked;

    if (record.path.empty()) {
        context.Error("empty-path", record.name, "an enum has no path");
        return;
    }

    std::unordered_set<std::string> names;
    for (const auto& value : record.values) {
        if (!names.insert(value.name).second) {
            context.Warn("duplicate-enumerator", record.path,
                         std::format("'{}' appears more than once", value.name));
        }
        if (!FitsUnderlying(record.underlying, value.value)) {
            // the C4369 thing from 0.2.0, except it fires here instead of as a
            // compiler warning three steps later that nobody reads
            context.Error("enum-underlying-narrow", record.path,
                          std::format("{} = {} does not fit in {}", value.name,
                                      value.value, record.underlying));
        }
    }
}

void CheckStruct(Context& context, const Struct& record) {
    ++context.report.types_checked;

    if (record.path.empty()) {
        context.Error("empty-path", record.name, "a type has no path");
        return;
    }

    if (!record.super.empty() && !context.types.count(record.super)) {
        context.Warn("dangling-super", record.path,
                     std::format("super '{}' is not in this dump", record.super));
    } else if (!record.super.empty() && record.inherited_size > 0) {
        const Struct* base = context.types.at(record.super);
        if (base->size != record.inherited_size) {
            context.Warn("inherited-size-mismatch", record.path,
                         std::format("members start at {} but {} is {} bytes",
                                     record.inherited_size, record.super, base->size));
        }
    }

    if (record.inherited_size > record.size && record.size > 0) {
        context.Error("inherited-exceeds-size", record.path,
                      std::format("inherited region is {} bytes of a {}-byte type",
                                  record.inherited_size, record.size));
    }

    if (record.size <= 0 && !record.properties.empty()) {
        context.Error("zero-size-with-members", record.path,
                      std::format("reports {} bytes but declares {} properties",
                                  record.size, record.properties.size()));
    }

    if (record.path == record.super) {
        context.Error("self-inheritance", record.path, "the type is its own super");
    }

    // sorted, so overlap is one comparison against the previous member instead of an
    // all-pairs walk. big classes run to hundreds of properties.
    std::vector<const Property*> ordered;
    ordered.reserve(record.properties.size());

    std::unordered_set<std::string> property_names;
    for (const auto& property : record.properties) {
        ++context.report.properties_checked;
        ordered.push_back(&property);

        const std::string where = record.path + "." + property.name;

        if (property.name.empty())
            context.Error("unnamed-property", record.path, "a property has no name");
        else if (!property_names.insert(property.name).second)
            context.Warn("duplicate-property", record.path,
                         std::format("'{}' is declared more than once", property.name));

        if (property.offset < 0)
            context.Error("negative-offset", where,
                          std::format("offset {}", property.offset));

        if (property.array_dim < 1)
            context.Error("bad-array-dim", where,
                          std::format("array dimension {}", property.array_dim));

        if (property.size < 0)
            context.Error("negative-size", where, std::format("size {}", property.size));

        if (record.size > 0 && property.size > 0 &&
            property.offset + property.size > record.size) {
            context.Error("member-overruns-type", where,
                          std::format("ends at {} in a {}-byte type",
                                      property.offset + property.size, record.size));
        }

        if (property.is_bitfield) {
            if (property.field_mask == 0)
                context.Error("bitfield-no-mask", where,
                              "marked as a bitfield with an empty mask, so no write to it "
                              "can touch the right bit");
            if (property.bit_index < 0 || property.bit_index > 7)
                context.Error("bitfield-bad-bit", where,
                              std::format("bit index {}", property.bit_index));
        }

        if (property.size > 0 && property.type.size > 0) {
            const std::int32_t expected = property.type.size * property.array_dim;
            if (expected != property.size) {
                context.Warn("size-disagrees-with-type", where,
                             std::format("{} bytes, but the element type is {} x {}",
                                         property.size, property.type.size,
                                         property.array_dim));
            }
        }

        CheckTypeRef(context, property.type, where);
    }

    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Property* a, const Property* b) {
                         return a->offset < b->offset;
                     });

    for (std::size_t i = 1; i < ordered.size(); ++i) {
        const Property& previous = *ordered[i - 1];
        const Property& current  = *ordered[i];
        if (previous.size <= 0 || current.size <= 0) continue;

        if (current.offset == previous.offset) {
            // the one legitimate case - bools packed into a byte, each owning a bit
            if (previous.is_bitfield && current.is_bitfield) {
                if ((previous.field_mask & current.field_mask) != 0) {
                    context.Error("bitfield-mask-clash",
                                  record.path + "." + current.name,
                                  std::format("shares bit mask 0x{:02X} with {}",
                                              previous.field_mask & current.field_mask,
                                              previous.name));
                }
                continue;
            }
            context.Error("shared-offset", record.path + "." + current.name,
                          std::format("offset {} is also {}, and they are not both "
                                      "bitfields", current.offset, previous.name));
            continue;
        }

        if (current.offset < previous.offset + previous.size) {
            context.Error("property-overlap", record.path + "." + current.name,
                          std::format("starts at {} but {} runs to {}", current.offset,
                                      previous.name, previous.offset + previous.size));
        }
    }

    for (const auto& function : record.functions) {
        const std::string where = record.path + "." + function.name;
        if (function.name.empty())
            context.Error("unnamed-function", record.path, "a function has no name");

        std::size_t returns = 0;
        for (const auto& param : function.params) {
            if (param.is_return) ++returns;
            if (param.offset < 0)
                context.Error("negative-offset", where + "(" + param.name + ")",
                              std::format("parameter offset {}", param.offset));
            CheckTypeRef(context, param.type, where + "(" + param.name + ")");
        }
        if (returns > 1) {
            context.Error("multiple-returns", where,
                          std::format("{} parameters are marked as the return value",
                                      returns));
        }

        if (!function.script.empty() && function.script_size == 0) {
            context.Warn("script-without-size", where,
                         "decompiled statements are present but the bytecode size is 0");
        }
    }

    for (const auto& interface_path : record.interfaces) {
        if (!context.types.count(interface_path)) {
            context.Warn("dangling-interface", record.path,
                         std::format("interface '{}' is not in this dump", interface_path));
        }
    }
}

} // namespace

std::string_view ToString(LintSeverity severity) {
    return severity == LintSeverity::Error ? "error" : "warning";
}

LintReport Lint(const Dump& dump) {
    Context context;

    // index first. half the checks are "is this path here", and a property in the first
    // package routinely points at a class in the last.
    for (const auto& package : dump.packages) {
        for (const auto* list : {&package.classes, &package.structs}) {
            for (const auto& record : *list) {
                if (record.path.empty()) continue;
                if (!context.types.emplace(record.path, &record).second) {
                    context.Error("duplicate-path", record.path,
                                  "two types in this dump claim the same path");
                }
            }
        }
        for (const auto& record : package.enums) {
            if (record.path.empty()) continue;
            if (!context.enums.insert(record.path).second) {
                context.Error("duplicate-path", record.path,
                              "two enums in this dump claim the same path");
            }
        }
    }

    if (dump.schema_version != kSchemaVersion) {
        context.Warn("schema-version", "header",
                     std::format("dump is schema {}, this build understands {}",
                                 dump.schema_version, kSchemaVersion));
    }

    for (const auto& package : dump.packages) {
        if (package.name.empty())
            context.Error("empty-package-name", "<package>", "a package has no name");

        for (const auto& record : package.enums)   CheckEnum(context, record);
        for (const auto& record : package.classes) CheckStruct(context, record);
        for (const auto& record : package.structs) CheckStruct(context, record);
    }

    return std::move(context.report);
}

} // namespace zircon::ir
