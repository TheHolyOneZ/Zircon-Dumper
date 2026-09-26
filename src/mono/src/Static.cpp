#include "mono/Static.h"

#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::mono {
namespace {

constexpr std::uint32_t kFieldStatic  = 0x0010;
constexpr std::uint32_t kFieldLiteral = 0x0040;

std::string Utc() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(now));
}

std::string PathOf(const AssemblyMetadata& assembly, std::size_t index) {
    const auto name = assembly.FullNameOf(index);
    if (name.empty()) return {};
    return assembly.name.empty() ? name : name + ", " + assembly.name;
}

std::unordered_set<std::string> PathsIn(const ir::Dump& dump) {
    std::unordered_set<std::string> out;
    for (const auto& package : dump.packages) {
        for (const auto& record : package.classes) out.insert(record.path);
        for (const auto& record : package.structs) out.insert(record.path);
        for (const auto& record : package.enums)   out.insert(record.path);
    }
    return out;
}

} // namespace

ir::Dump BuildStaticDump(const std::vector<AssemblyMetadata>& assemblies, StaticStats& stats) {
    ir::Dump dump;
    dump.header.runtime            = "mono";
    dump.header.source.kind        = "static";
    dump.header.source.main_module = "Managed";
    dump.header.created_utc        = Utc();
    dump.header.sources            = {"static"};
    dump.header.partial            = true;

    dump.header.engine.evidence.emplace_back(
        "read from the game's own managed assemblies with no process: names, namespaces, "
        "tokens, interfaces, method signatures, IL RVAs and enum values are exact, because "
        "all of it is ECMA-335 metadata rather than anything Unity invented");
    dump.header.engine.evidence.emplace_back(
        "field offsets are absent: the CLI does not store them, the runtime computes a "
        "layout when a type is first used. Run live, or --mode dual, for offsets");
    dump.header.engine.evidence.emplace_back(
        "an interface named against a facade assembly -- netstandard, System.Runtime -- is "
        "recorded as the metadata names it. The type itself lives in mscorlib and reaches the "
        "facade through a type forwarder, so the reference is correct and does not resolve "
        "inside this dump");
    dump.header.engine.confidence = 1.0f;

    for (const auto& assembly : assemblies) {
        ++stats.assemblies;

        ir::Package package;
        package.name             = assembly.name;
        package.assembly_version = assembly.version;
        package.mvid             = assembly.mvid;

        std::unordered_set<std::string> taken;

        for (std::size_t i = 0; i < assembly.types.size(); ++i) {
            const auto& row = assembly.types[i];
            const auto path = PathOf(assembly, i);
            if (path.empty()) continue;
            if (!taken.insert(path).second) { ++stats.indistinguishable; continue; }
            ++stats.types;

            if (row.is_enum) {
                ir::Enum record;
                record.name       = row.name;
                record.path       = path;
                record.underlying = row.underlying;
                record.values_resolved = row.enum_values_resolved;
                for (const auto& [name, value] : row.enum_values) {
                    record.values.push_back(ir::EnumValue{name, value});
                    ++stats.enum_values;
                }
                if (!record.values_resolved) ++stats.enums_without_values;
                ++stats.enums;
                package.enums.push_back(std::move(record));
                continue;
            }

            ir::Struct record;
            record.name         = row.name;
            record.path         = path;
            record.name_space   = row.name_space;
            record.token        = row.token;
            record.is_class     = !row.is_valuetype;
            record.is_valuetype = row.is_valuetype;
            record.is_interface = row.is_interface;
            record.is_abstract  = row.is_abstract;
            record.is_generic   = row.name.find('`') != std::string::npos;
            record.source       = "static";
            record.interfaces   = row.interfaces;

            for (const auto& field : row.fields) {
                ir::Property property;
                property.name      = field.name;
                property.is_static = (field.flags & kFieldStatic) != 0;
                property.offset    = 0;
                property.offset_unresolved = true;
                property.flags     = field.flags;
                property.type.raw  = "<in the binary>";
                if (field.flags & kFieldLiteral) property.type.raw = "<a const>";
                ++stats.fields;
                record.properties.push_back(std::move(property));
            }

            for (const auto& method : row.methods) {
                ir::Function function;
                function.name   = method.name;
                function.flags  = method.flags;
                function.token  = method.token;
                function.il_rva = method.rva;
                if (method.rva != 0) ++stats.with_il;
                ++stats.methods;
                record.functions.push_back(std::move(function));
            }

            for (const auto& property : row.properties) {
                ir::Accessor accessor;
                accessor.name   = property.name;
                accessor.flags  = property.flags;
                accessor.getter = property.getter;
                accessor.setter = property.setter;
                ++stats.accessors;
                record.accessors.push_back(std::move(accessor));
            }

            if (row.is_valuetype) {
                ++stats.structs;
                package.structs.push_back(std::move(record));
            } else {
                ++stats.classes;
                package.classes.push_back(std::move(record));
            }
        }

        dump.packages.push_back(std::move(package));
    }

    std::sort(dump.packages.begin(), dump.packages.end(),
              [](const ir::Package& a, const ir::Package& b) { return a.name < b.name; });
    for (auto& package : dump.packages) {
        const auto by_path = [](const auto& a, const auto& b) { return a.path < b.path; };
        std::sort(package.classes.begin(), package.classes.end(), by_path);
        std::sort(package.structs.begin(), package.structs.end(), by_path);
        std::sort(package.enums.begin(), package.enums.end(), by_path);
    }
    return dump;
}

ir::Dump MergeDumps(const ir::Dump& live, const ir::Dump& from_metadata, MergeStats& stats) {
    ir::Dump out = live;
    out.header.sources = {"live", "static"};
    out.header.partial = false;

    std::unordered_map<std::string, const ir::Enum*> enums_by_path;
    std::unordered_map<std::string, const ir::Struct*> types_by_path;
    std::unordered_map<std::string, const ir::Package*> package_of;
    for (const auto& package : from_metadata.packages) {
        for (const auto& record : package.enums) enums_by_path.emplace(record.path, &record);
        for (const auto& record : package.classes) types_by_path.emplace(record.path, &record);
        for (const auto& record : package.structs) types_by_path.emplace(record.path, &record);
        package_of.emplace(package.name, &package);
    }

    for (auto& package : out.packages) {
        if (const auto it = package_of.find(package.name); it != package_of.end()) {
            package.assembly_version = it->second->assembly_version;
            package.mvid             = it->second->mvid;
        }

        for (auto& record : package.enums) {
            const auto it = enums_by_path.find(record.path);
            if (it == enums_by_path.end()) continue;
            if (record.values_resolved || !it->second->values_resolved) continue;

            std::unordered_map<std::string, std::int64_t> value_of;
            for (const auto& entry : it->second->values) value_of.emplace(entry.name, entry.value);

            bool all = true;
            for (auto& entry : record.values) {
                const auto found = value_of.find(entry.name);
                if (found == value_of.end()) { all = false; continue; }
                entry.value = found->second;
            }
            if (all && !record.values.empty()) {
                record.values_resolved = true;
                ++stats.enums_filled;
            }
            if (!it->second->underlying.empty()) record.underlying = it->second->underlying;
        }

        const auto fill = [&](std::vector<ir::Struct>& records) {
            for (auto& record : records) {
                const auto it = types_by_path.find(record.path);
                if (it == types_by_path.end()) {
                    ++stats.live_only;
                    if (record.source.empty()) record.source = "live";
                    continue;
                }
                ++stats.matched;
                record.source = "both";

                std::unordered_map<std::uint32_t, std::uint32_t> il_of;
                std::unordered_map<std::string, std::uint32_t> il_by_name;
                for (const auto& method : it->second->functions) {
                    if (method.token) il_of.emplace(method.token, method.il_rva);
                    il_by_name.emplace(method.name, method.il_rva);
                }
                for (auto& method : record.functions) {
                    if (method.il_rva != 0) continue;
                    if (const auto found = il_of.find(method.token);
                        method.token && found != il_of.end()) {
                        method.il_rva = found->second;
                        if (method.il_rva) ++stats.il_filled;
                        continue;
                    }
                    if (const auto found = il_by_name.find(method.name);
                        found != il_by_name.end()) {
                        method.il_rva = found->second;
                        if (method.il_rva) ++stats.il_filled;
                    }
                }
            }
        };
        fill(package.classes);
        fill(package.structs);
    }

    const auto live_paths = PathsIn(out);
    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t i = 0; i < out.packages.size(); ++i)
        index_of.emplace(out.packages[i].name, i);

    for (const auto& package : from_metadata.packages) {
        std::vector<ir::Struct> classes, structs;
        std::vector<ir::Enum>   enums;

        for (const auto& record : package.classes)
            if (!live_paths.count(record.path)) classes.push_back(record);
        for (const auto& record : package.structs)
            if (!live_paths.count(record.path)) structs.push_back(record);
        for (const auto& record : package.enums)
            if (!live_paths.count(record.path)) enums.push_back(record);

        const std::size_t added = classes.size() + structs.size() + enums.size();
        if (added == 0) continue;
        stats.static_only += added;

        const auto it = index_of.find(package.name);
        if (it == index_of.end()) {
            ir::Package fresh;
            fresh.name             = package.name;
            fresh.assembly_version = package.assembly_version;
            fresh.mvid             = package.mvid;
            fresh.classes          = std::move(classes);
            fresh.structs          = std::move(structs);
            fresh.enums            = std::move(enums);
            index_of.emplace(fresh.name, out.packages.size());
            out.packages.push_back(std::move(fresh));
            continue;
        }

        auto& target = out.packages[it->second];
        target.classes.insert(target.classes.end(), classes.begin(), classes.end());
        target.structs.insert(target.structs.end(), structs.begin(), structs.end());
        target.enums.insert(target.enums.end(), enums.begin(), enums.end());
    }

    for (const auto& line : from_metadata.header.engine.evidence)
        out.header.engine.evidence.push_back("metadata: " + line);

    std::sort(out.packages.begin(), out.packages.end(),
              [](const ir::Package& a, const ir::Package& b) { return a.name < b.name; });
    for (auto& package : out.packages) {
        const auto by_path = [](const auto& a, const auto& b) { return a.path < b.path; };
        std::sort(package.classes.begin(), package.classes.end(), by_path);
        std::sort(package.structs.begin(), package.structs.end(), by_path);
        std::sort(package.enums.begin(), package.enums.end(), by_path);
    }
    return out;
}

} // namespace zircon::mono
