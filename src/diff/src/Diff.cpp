#include "diff/Diff.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::diff {
namespace {

// Canonical rendering of a type, for comparison only. Has to be stable, and has to carry
// the size: two types that print alike but occupy different numbers of bytes are a breaking
// change, and a purely nominal comparison sails straight past it.
std::string Signature(const ir::TypeRef& type) {
    std::string out(ir::ToString(type.kind));
    if (!type.name.empty()) {
        out += '<';
        out += type.name;
        out += '>';
    }
    if (!type.params.empty()) {
        out += '(';
        for (std::size_t i = 0; i < type.params.size(); ++i) {
            if (i) out += ',';
            out += Signature(type.params[i]);
        }
        out += ')';
    }
    out += std::format("[{}]", type.size);
    return out;
}

std::string Signature(const ir::Function& function) {
    std::string out;
    for (const auto& param : function.params) {
        if (!out.empty()) out += ", ";
        if (param.is_return) out += "-> ";
        else if (param.is_out) out += "out ";
        out += Signature(param.type);
    }
    return out;
}

struct TypeIndex {
    std::unordered_map<std::string, const ir::Struct*> types;
    std::unordered_map<std::string, const ir::Enum*>   enums;
    std::unordered_set<std::string>                    packages;
};

TypeIndex Index(const ir::Dump& dump, const std::string& filter) {
    TypeIndex index;
    for (const auto& package : dump.packages) {
        if (!filter.empty() && package.name.find(filter) == std::string::npos) continue;
        index.packages.insert(package.name);

        for (const auto* list : {&package.classes, &package.structs})
            for (const auto& record : *list) index.types[record.path] = &record;
        for (const auto& record : package.enums) index.enums[record.path] = &record;
    }
    return index;
}

class Differ {
public:
    Differ(const DiffOptions& options, DiffResult& result)
        : options_(options), result_(result) {}

    void Emit(ChangeKind kind, std::string path, std::string member,
              std::string before, std::string after, std::string detail = {}) {
        const Severity severity = DefaultSeverity(kind);
        if (severity < options_.minimum) return;

        if (!options_.include_additions &&
            (kind == ChangeKind::TypeAdded || kind == ChangeKind::PropertyAdded ||
             kind == ChangeKind::FunctionAdded || kind == ChangeKind::EnumAdded ||
             kind == ChangeKind::EnumValueAdded || kind == ChangeKind::PackageAdded))
            return;

        Change change;
        change.kind     = kind;
        change.severity = severity;
        change.path     = std::move(path);
        change.member   = std::move(member);
        change.before   = std::move(before);
        change.after    = std::move(after);
        change.detail   = std::move(detail);

        ++result_.by_severity[severity];
        ++result_.by_kind[kind];
        result_.changes.push_back(std::move(change));
    }

private:
    const DiffOptions& options_;
    DiffResult&        result_;
};

// True when nothing changed, which feeds the "how much of my work survives" statistic.
bool DiffProperties(Differ& emit, const DiffOptions& options, const ir::Struct& before,
                    const ir::Struct& after) {
    // Keyed by name *and* occurrence.
    //
    // Member names are not unique within a type. FF7 Rebirth ships a Blueprint class with
    // two properties both called "Light" at different offsets and of different types, and a
    // plain name->pointer map keeps only the last. Every earlier duplicate then compared
    // against the wrong member, so diffing that dump *against itself* reported the first
    // Light as having moved 32 bytes and changed type.
    //
    // Worst failure a diff can have. Its whole job is to be believed when it says a build
    // changed, and here it was inventing a critical change out of two identical files.
    std::unordered_map<std::string, std::vector<const ir::Property*>> before_by_name,
                                                                     after_by_name;
    for (const auto& property : before.properties) before_by_name[property.name].push_back(&property);
    for (const auto& property : after.properties)  after_by_name[property.name].push_back(&property);

    std::unordered_map<std::string, std::size_t> taken;

    bool unchanged = true;

    std::vector<const ir::Property*> removed, added;

    for (const auto& property : before.properties) {
        auto& candidates = after_by_name[property.name];
        const std::size_t occurrence = taken[property.name]++;

        if (occurrence >= candidates.size()) {
            removed.push_back(&property);
            unchanged = false;
            continue;
        }

        const ir::Property& now = *candidates[occurrence];

        if (property.offset != now.offset) {
            unchanged = false;
            emit.Emit(ChangeKind::PropertyMoved, after.path, property.name,
                      std::format("0x{:X}", property.offset),
                      std::format("0x{:X}", now.offset),
                      std::format("moved {:+d} bytes", now.offset - property.offset));
        }
        if (property.size != now.size) {
            unchanged = false;
            emit.Emit(ChangeKind::PropertyResized, after.path, property.name,
                      std::format("0x{:X}", property.size),
                      std::format("0x{:X}", now.size));
        }

        const std::string before_type = Signature(property.type);
        const std::string after_type  = Signature(now.type);
        if (before_type != after_type) {
            unchanged = false;
            emit.Emit(ChangeKind::PropertyTypeChanged, after.path, property.name,
                      before_type, after_type);
        }

        // An offset comparison can't see a bitfield move within its byte, and a mask read
        // with the wrong bit is wrong in a way that looks fine.
        if (property.is_bitfield && now.is_bitfield &&
            (property.bit_index != now.bit_index || property.field_mask != now.field_mask)) {
            unchanged = false;
            emit.Emit(ChangeKind::PropertyBitMoved, after.path, property.name,
                      std::format("bit {} mask 0x{:02X}", property.bit_index, property.field_mask),
                      std::format("bit {} mask 0x{:02X}", now.bit_index, now.field_mask));
        }

        if (property.flags != now.flags) {
            unchanged = false;
            emit.Emit(ChangeKind::PropertyFlagsChanged, after.path, property.name,
                      std::format("0x{:016X}", property.flags),
                      std::format("0x{:016X}", now.flags));
        }

        // Only when both sides actually captured defaults. They're opt-in, so one dump
        // having them and the other not says something about how the dumps were taken
        // and not about the game. Report it and the first --defaults run is all noise.
        if (!property.default_value.empty() && !now.default_value.empty() &&
            property.default_value != now.default_value) {
            unchanged = false;
            emit.Emit(ChangeKind::PropertyDefaultChanged, after.path, property.name,
                      property.default_value, now.default_value);
        }
    }

    std::unordered_map<std::string, std::size_t> seen_after;
    for (const auto& property : after.properties) {
        const std::size_t occurrence = seen_after[property.name]++;
        const auto it = before_by_name.find(property.name);
        if (it == before_by_name.end() || occurrence >= it->second.size()) {
            added.push_back(&property);
            unchanged = false;
        }
    }

    // Rename detection. A property that vanishes while another turns up at the same offset
    // with the same type is a rename, and calling that removed+added hides the fact that
    // every hardcoded offset still works.
    if (options.detect_renames) {
        std::vector<bool> added_taken(added.size(), false);

        for (const auto* gone : removed) {
            std::size_t match = added.size();
            for (std::size_t i = 0; i < added.size(); ++i) {
                if (added_taken[i]) continue;
                if (added[i]->offset != gone->offset) continue;
                if (Signature(added[i]->type) != Signature(gone->type)) continue;
                match = i;
                break;
            }
            if (match == added.size()) {
                emit.Emit(ChangeKind::PropertyRemoved, before.path, gone->name,
                          std::format("0x{:X}", gone->offset), {});
                continue;
            }

            added_taken[match] = true;
            emit.Emit(ChangeKind::PropertyRenamed, after.path, gone->name, gone->name,
                      added[match]->name,
                      std::format("same offset 0x{:X} and type; offset still valid",
                                  gone->offset));
        }

        for (std::size_t i = 0; i < added.size(); ++i)
            if (!added_taken[i])
                emit.Emit(ChangeKind::PropertyAdded, after.path, added[i]->name, {},
                          std::format("0x{:X}", added[i]->offset));
    } else {
        for (const auto* gone : removed)
            emit.Emit(ChangeKind::PropertyRemoved, before.path, gone->name,
                      std::format("0x{:X}", gone->offset), {});
        for (const auto* fresh : added)
            emit.Emit(ChangeKind::PropertyAdded, after.path, fresh->name, {},
                      std::format("0x{:X}", fresh->offset));
    }

    return unchanged;
}

bool DiffFunctions(Differ& emit, const ir::Struct& before, const ir::Struct& after) {
    // Name *and* occurrence again, and for the reason spelled out over in CompareMembers:
    // names aren't unique within a type, a plain map keeps only the last, and two dumps of
    // the same process end up disagreeing. Pairing duplicates in order fixes it.
    std::unordered_map<std::string, std::vector<const ir::Function*>> before_by_name,
                                                                     after_by_name;
    for (const auto& function : before.functions) before_by_name[function.name].push_back(&function);
    for (const auto& function : after.functions)  after_by_name[function.name].push_back(&function);

    std::unordered_map<std::string, std::size_t> taken;

    bool unchanged = true;

    for (const auto& function : before.functions) {
        auto& candidates = after_by_name[function.name];
        const std::size_t index = taken[function.name]++;

        const bool matched = index < candidates.size();
        const auto it = matched ? candidates.begin() + static_cast<std::ptrdiff_t>(index)
                                : candidates.end();
        if (!matched) {
            unchanged = false;
            emit.Emit(ChangeKind::FunctionRemoved, before.path, function.name, {}, {});
            continue;
        }

        const ir::Function& now = **it;

        const std::string before_signature = Signature(function);
        const std::string after_signature  = Signature(now);
        if (before_signature != after_signature) {
            unchanged = false;
            emit.Emit(ChangeKind::FunctionSignatureChanged, after.path, function.name,
                      before_signature.empty() ? "()" : before_signature,
                      after_signature.empty() ? "()" : after_signature);
        }

        // Only meaningful when both dumps recorded an address. A script-only function has
        // none, and 0 -> 0 really is unchanged.
        if (function.native_rva != 0 && now.native_rva != 0 &&
            function.native_rva != now.native_rva) {
            unchanged = false;
            emit.Emit(ChangeKind::FunctionMoved, after.path, function.name,
                      std::format("+0x{:X}", function.native_rva),
                      std::format("+0x{:X}", now.native_rva),
                      "any hook on this address needs re-resolving");
        }

        if (function.flags != now.flags) {
            unchanged = false;
            emit.Emit(ChangeKind::FunctionFlagsChanged, after.path, function.name,
                      std::format("0x{:08X}", function.flags),
                      std::format("0x{:08X}", now.flags));
        }
    }

    // Anything in `after` past the count of same-named entries in `before` is new.
    std::unordered_map<std::string, std::size_t> seen;
    for (const auto& function : after.functions) {
        const std::size_t index = seen[function.name]++;
        const auto it = before_by_name.find(function.name);
        if (it != before_by_name.end() && index < it->second.size()) continue;

        unchanged = false;
        emit.Emit(ChangeKind::FunctionAdded, after.path, function.name, {}, {});
    }

    return unchanged;
}

void DiffEnum(Differ& emit, const ir::Enum& before, const ir::Enum& after) {
    std::unordered_map<std::string, std::int64_t> before_values, after_values;
    for (const auto& value : before.values) before_values[value.name] = value.value;
    for (const auto& value : after.values)  after_values[value.name]  = value.value;

    for (const auto& value : before.values) {
        const auto it = after_values.find(value.name);
        if (it == after_values.end()) {
            emit.Emit(ChangeKind::EnumValueRemoved, before.path, value.name,
                      std::to_string(value.value), {});
            continue;
        }
        if (it->second != value.value)
            emit.Emit(ChangeKind::EnumValueChanged, after.path, value.name,
                      std::to_string(value.value), std::to_string(it->second));
    }

    for (const auto& value : after.values)
        if (!before_values.count(value.name))
            emit.Emit(ChangeKind::EnumValueAdded, after.path, value.name, {},
                      std::to_string(value.value));
}

} // namespace

std::string_view ToString(Severity severity) {
    switch (severity) {
        case Severity::Info:     return "info";
        case Severity::Low:      return "low";
        case Severity::Medium:   return "medium";
        case Severity::High:     return "high";
        case Severity::Critical: return "critical";
    }
    return "unknown";
}

std::string_view ToString(ChangeKind kind) {
    switch (kind) {
        case ChangeKind::PackageAdded:             return "package_added";
        case ChangeKind::PackageRemoved:           return "package_removed";
        case ChangeKind::TypeAdded:                return "type_added";
        case ChangeKind::TypeRemoved:              return "type_removed";
        case ChangeKind::TypeResized:              return "type_resized";
        case ChangeKind::TypeRealigned:            return "type_realigned";
        case ChangeKind::TypeSuperChanged:         return "type_super_changed";
        case ChangeKind::PropertyAdded:            return "property_added";
        case ChangeKind::PropertyRemoved:          return "property_removed";
        case ChangeKind::PropertyMoved:            return "property_moved";
        case ChangeKind::PropertyResized:          return "property_resized";
        case ChangeKind::PropertyTypeChanged:      return "property_type_changed";
        case ChangeKind::PropertyFlagsChanged:     return "property_flags_changed";
        case ChangeKind::PropertyDefaultChanged:   return "property_default_changed";
        case ChangeKind::PropertyBitMoved:         return "property_bit_moved";
        case ChangeKind::PropertyRenamed:          return "property_renamed";
        case ChangeKind::FunctionAdded:            return "function_added";
        case ChangeKind::FunctionRemoved:          return "function_removed";
        case ChangeKind::FunctionSignatureChanged: return "function_signature_changed";
        case ChangeKind::FunctionMoved:            return "function_moved";
        case ChangeKind::FunctionFlagsChanged:     return "function_flags_changed";
        case ChangeKind::EnumAdded:                return "enum_added";
        case ChangeKind::EnumRemoved:              return "enum_removed";
        case ChangeKind::EnumValueAdded:           return "enum_value_added";
        case ChangeKind::EnumValueRemoved:         return "enum_value_removed";
        case ChangeKind::EnumValueChanged:         return "enum_value_changed";
        case ChangeKind::EngineVersionChanged:     return "engine_version_changed";
        case ChangeKind::LayoutOffsetChanged:      return "layout_offset_changed";
    }
    return "unknown";
}

// Severity here means what the change breaks in code that already exists, which orders
// things differently from how interesting they are. A removed property is fatal. A hundred
// new classes are no problem whatsoever.
Severity DefaultSeverity(ChangeKind kind) {
    switch (kind) {
        // Reads through a hardcoded offset now return the wrong bytes, and say nothing.
        case ChangeKind::PropertyMoved:
        case ChangeKind::PropertyResized:
        case ChangeKind::PropertyTypeChanged:
        case ChangeKind::PropertyBitMoved:
        case ChangeKind::PropertyRemoved:
        case ChangeKind::TypeRemoved:
        case ChangeKind::FunctionRemoved:
            return Severity::Critical;

        // Still resolvable, but existing code stops working until it is updated.
        case ChangeKind::TypeResized:
        case ChangeKind::TypeSuperChanged:
        case ChangeKind::FunctionMoved:
        case ChangeKind::FunctionSignatureChanged:
        case ChangeKind::EnumValueChanged:
        case ChangeKind::EnumValueRemoved:
        case ChangeKind::PackageRemoved:
        case ChangeKind::EnumRemoved:
            return Severity::High;

        // Worth knowing, rarely fatal on its own.
        case ChangeKind::TypeRealigned:
        case ChangeKind::PropertyRenamed:
        case ChangeKind::EngineVersionChanged:
        case ChangeKind::LayoutOffsetChanged:
            return Severity::Medium;

        case ChangeKind::PropertyFlagsChanged:
        case ChangeKind::FunctionFlagsChanged:
            return Severity::Low;

        // A changed default breaks no code, but it is often the whole story of a patch:
        // nothing moved, and the game behaves differently anyway. Worth seeing, never
        // worth failing a build over.
        case ChangeKind::PropertyDefaultChanged:
            return Severity::Low;

        // New surface cannot break anything that already worked.
        case ChangeKind::PackageAdded:
        case ChangeKind::TypeAdded:
        case ChangeKind::PropertyAdded:
        case ChangeKind::FunctionAdded:
        case ChangeKind::EnumAdded:
        case ChangeKind::EnumValueAdded:
            return Severity::Info;
    }
    return Severity::Info;
}

bool DiffResult::HasBreakingChanges() const {
    for (const auto& [severity, count] : by_severity)
        if (severity >= Severity::High && count > 0) return true;
    return false;
}

DiffResult Diff(const ir::Dump& before, const ir::Dump& after, const DiffOptions& options) {
    DiffResult result;
    Differ emit(options, result);

    const TypeIndex old_index = Index(before, options.package_filter);
    const TypeIndex new_index = Index(after, options.package_filter);

    result.stats.types_before = old_index.types.size();
    result.stats.types_after  = new_index.types.size();

    for (const auto& [path, record] : old_index.types) {
        result.stats.properties_before += record->properties.size();
        result.stats.functions_before  += record->functions.size();
    }
    for (const auto& [path, record] : new_index.types) {
        result.stats.properties_after += record->properties.size();
        result.stats.functions_after  += record->functions.size();
    }

    // --- header ---------------------------------------------------------------------
    // An engine version change explains everything downstream, so it is reported first
    // rather than left for the reader to infer from a wall of moved offsets.
    if (before.header.engine.version != after.header.engine.version)
        emit.Emit(ChangeKind::EngineVersionChanged, "<engine>", {},
                  before.header.engine.version, after.header.engine.version,
                  "a different engine version explains widespread layout changes");

    {
        std::unordered_map<std::string, std::int32_t> old_offsets;
        for (const auto& entry : before.header.offsets) old_offsets[entry.name] = entry.value;
        for (const auto& entry : after.header.offsets) {
            const auto it = old_offsets.find(entry.name);
            if (it != old_offsets.end() && it->second != entry.value)
                emit.Emit(ChangeKind::LayoutOffsetChanged, "<layout>", entry.name,
                          std::format("0x{:X}", it->second),
                          std::format("0x{:X}", entry.value),
                          "a core layout offset moved; every derived offset shifts with it");
        }
    }

    // --- packages -------------------------------------------------------------------
    for (const auto& name : old_index.packages)
        if (!new_index.packages.count(name))
            emit.Emit(ChangeKind::PackageRemoved, name, {}, {}, {});
    for (const auto& name : new_index.packages)
        if (!old_index.packages.count(name))
            emit.Emit(ChangeKind::PackageAdded, name, {}, {}, {});

    // --- types ----------------------------------------------------------------------
    // Sorted so output is deterministic: the indices are hash maps, and an unstable
    // report order would make two runs of the same diff impossible to compare.
    std::vector<std::string> old_paths;
    old_paths.reserve(old_index.types.size());
    for (const auto& [path, record] : old_index.types) old_paths.push_back(path);
    std::sort(old_paths.begin(), old_paths.end());

    for (const auto& path : old_paths) {
        const ir::Struct& record = *old_index.types.at(path);

        const auto it = new_index.types.find(path);
        if (it == new_index.types.end()) {
            emit.Emit(ChangeKind::TypeRemoved, path, {},
                      std::format("0x{:X} bytes", record.size), {});
            continue;
        }

        const ir::Struct& now = *it->second;
        ++result.stats.types_compared;
        bool unchanged = true;

        if (record.size != now.size) {
            unchanged = false;
            emit.Emit(ChangeKind::TypeResized, path, {},
                      std::format("0x{:X}", record.size), std::format("0x{:X}", now.size),
                      std::format("{:+d} bytes", now.size - record.size));
        }
        if (record.alignment != now.alignment) {
            unchanged = false;
            emit.Emit(ChangeKind::TypeRealigned, path, {}, std::to_string(record.alignment),
                      std::to_string(now.alignment));
        }
        if (record.super != now.super) {
            unchanged = false;
            emit.Emit(ChangeKind::TypeSuperChanged, path, {},
                      record.super.empty() ? "<none>" : record.super,
                      now.super.empty() ? "<none>" : now.super);
        }

        if (!DiffProperties(emit, options, record, now)) unchanged = false;
        if (!DiffFunctions(emit, record, now))           unchanged = false;

        if (unchanged) ++result.stats.types_unchanged;
    }

    std::vector<std::string> new_paths;
    new_paths.reserve(new_index.types.size());
    for (const auto& [path, record] : new_index.types) new_paths.push_back(path);
    std::sort(new_paths.begin(), new_paths.end());

    for (const auto& path : new_paths)
        if (!old_index.types.count(path))
            emit.Emit(ChangeKind::TypeAdded, path, {}, {},
                      std::format("0x{:X} bytes", new_index.types.at(path)->size));

    // --- enums ----------------------------------------------------------------------
    std::vector<std::string> old_enums;
    old_enums.reserve(old_index.enums.size());
    for (const auto& [path, record] : old_index.enums) old_enums.push_back(path);
    std::sort(old_enums.begin(), old_enums.end());

    for (const auto& path : old_enums) {
        const auto it = new_index.enums.find(path);
        if (it == new_index.enums.end()) {
            emit.Emit(ChangeKind::EnumRemoved, path, {}, {}, {});
            continue;
        }
        DiffEnum(emit, *old_index.enums.at(path), *it->second);
    }

    std::vector<std::string> new_enums;
    new_enums.reserve(new_index.enums.size());
    for (const auto& [path, record] : new_index.enums) new_enums.push_back(path);
    std::sort(new_enums.begin(), new_enums.end());

    for (const auto& path : new_enums)
        if (!old_index.enums.count(path))
            emit.Emit(ChangeKind::EnumAdded, path, {}, {}, {});

    // Most severe first, then by type, so the report opens with what actually broke.
    std::stable_sort(result.changes.begin(), result.changes.end(),
                     [](const Change& a, const Change& b) {
                         if (a.severity != b.severity) return a.severity > b.severity;
                         if (a.path != b.path) return a.path < b.path;
                         return a.member < b.member;
                     });

    return result;
}

} // namespace zircon::diff
