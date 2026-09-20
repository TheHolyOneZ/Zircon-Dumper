#include "il2cpp/Static.h"

#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::il2cpp {
namespace {

std::int32_t ReadI32(std::span<const std::uint8_t> file, std::size_t at) {
    std::int32_t value = 0;
    std::memcpy(&value, file.data() + at, sizeof(value));
    return value;
}

std::uint16_t ReadU16(std::span<const std::uint8_t> file, std::size_t at) {
    std::uint16_t value = 0;
    std::memcpy(&value, file.data() + at, sizeof(value));
    return value;
}

// Same format the Unreal side uses, so dumps from both sort together.
std::string Utc() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>(now));
}

// "Assembly-CSharp.dll" is the file, "Assembly-CSharp" is the assembly, and the assembly is
// what goes on the end of a path. Matches what the live walk does, so a static dump and a
// live dump of one game agree on identity.
std::string AssemblyOf(std::string image) {
    if (image.size() > 4 && image.compare(image.size() - 4, 4, ".dll") == 0)
        image.resize(image.size() - 4);
    return image;
}

class Reader {
public:
    Reader(std::span<const std::uint8_t> file, const MetadataLayout& layout, StaticStats& stats)
        : file_(file), layout_(layout), stats_(stats) {}

    ir::Dump Run() {
        ir::Dump dump;
        dump.header.runtime            = "il2cpp";
        dump.header.source.kind        = "static";
        dump.header.source.main_module = "global-metadata.dat";
        dump.header.created_utc        = Utc();
        dump.header.engine.evidence    = layout_.evidence;

        // A solved layout is not a guess. Four independent constraints have to agree before
        // SolveMetadataLayout returns at all, and it refuses by name when they do not.
        dump.header.engine.version     = std::format("IL2CPP metadata v{}", layout_.version);
        dump.header.engine.confidence  = 1.0f;

        // Which reading produced this. A dual run overwrites it with both; on its own it is
        // one word, and without it a reader cannot tell a static dump from an old one that
        // predates there being more than one way to read a game.
        dump.header.sources            = {"static"};

        // A static dump has no object data by construction, which is exactly what partial
        // means everywhere else in the IR. Emitters that need more refuse rather than
        // inventing it.
        dump.header.partial = true;
        dump.header.engine.evidence.emplace_back(
            "read from global-metadata.dat with no process: names, namespaces and tokens are "
            "exact, and field types, field offsets and method addresses are absent because "
            "they live in the binary rather than in the metadata");

        // Whether a type is a struct or an enum comes from its parent, and the parent is an
        // index into an array in the binary. So every type is filed as a class here. Said
        // out loud, because a dump reporting no enums at all otherwise reads as a game that
        // has none.
        dump.header.engine.evidence.emplace_back(
            "every type is filed as a class: struct and enum come from a type's parent, which "
            "is an index into the binary rather than anything in this file. A dual run takes "
            "that from the runtime");

        const int types_span = layout_.tables.types;
        if (types_span < 0) return dump;

        const auto& span = layout_.Span(types_span);
        type_count_ = span.size / static_cast<std::uint32_t>(layout_.type_record);
        BuildNesting();

        if (layout_.tables.images >= 0 && layout_.image_types.start_slot >= 0)
            ReadByImage(dump);
        else
            ReadEverythingIntoOnePackage(dump);

        Finish(dump);
        return dump;
    }

private:
    std::int32_t TypeSlot(std::uint32_t index, int slot) const {
        const auto& span = layout_.Span(layout_.tables.types);
        return ReadI32(file_, span.offset + static_cast<std::size_t>(index) * layout_.type_record +
                                  static_cast<std::size_t>(slot) * 4);
    }

    std::uint16_t TypeCount(std::uint32_t index, int slot) const {
        const auto& span = layout_.Span(layout_.tables.types);
        return ReadU16(file_, span.offset + static_cast<std::size_t>(index) * layout_.type_record +
                                  static_cast<std::size_t>(slot) * 2);
    }

    std::string Name(std::int32_t index) const {
        return MetadataString(file_, layout_, index);
    }

    // Who declares whom. A type's nested-types range lists the type indices it declares, so
    // reading every one of them backwards gives each nested type its outer type.
    void BuildNesting() {
        const auto* range = layout_.RangeTo(layout_.tables.nested_types);
        if (!range || layout_.tables.nested_types < 0) return;

        const auto& table = layout_.Span(layout_.tables.nested_types);
        for (std::uint32_t outer = 0; outer < type_count_; ++outer) {
            const std::int32_t first = TypeSlot(outer, range->start_slot);
            const std::uint16_t many = TypeCount(outer, range->count_slot);
            if (first < 0) continue;

            for (std::uint16_t i = 0; i < many; ++i) {
                const std::size_t at = table.offset +
                    (static_cast<std::size_t>(first) + i) * 4;
                if (at + 4 > file_.size()) break;
                const std::int32_t inner = ReadI32(file_, at);
                if (inner >= 0 && static_cast<std::uint32_t>(inner) < type_count_)
                    outer_of_[static_cast<std::uint32_t>(inner)] = outer;
            }
        }
    }

    // The C# name, built outer-first, exactly the way the live walk builds it. A nested
    // type's own name is only unique inside its outer type, and if the two readings spell
    // it differently they describe the same type twice and merge into neither.
    std::string NameOf(std::uint32_t index, int depth = 0) {
        if (depth > 16) return {};                    // a cycle, which is a broken file
        const auto it = outer_of_.find(index);
        const std::string own = Name(TypeSlot(index, layout_.name_slot));
        if (it != outer_of_.end() && it->second != index)
            return NameOf(it->second, depth + 1) + "." + own;

        const std::string space = layout_.namespace_slot >= 0
                                      ? Name(TypeSlot(index, layout_.namespace_slot))
                                      : std::string{};
        return space.empty() ? own : space + "." + own;
    }

    void ReadByImage(ir::Dump& dump) {
        const auto& images = layout_.Span(layout_.tables.images);
        const std::uint32_t count = images.size / static_cast<std::uint32_t>(layout_.image_record);

        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t at = images.offset + static_cast<std::size_t>(i) * layout_.image_record;
            const std::string file_name =
                Name(ReadI32(file_, at + static_cast<std::size_t>(layout_.image_name_slot) * 4));
            if (file_name.empty()) continue;

            const std::int32_t first = ReadI32(
                file_, at + static_cast<std::size_t>(layout_.image_types.start_slot) * 4);
            const std::uint16_t many = ReadU16(
                file_, at + static_cast<std::size_t>(layout_.image_types.count_slot) * 2);
            if (first < 0 || many == 0) continue;

            ++stats_.images;
            auto& package = PackageFor(dump, file_name);
            const std::string assembly = AssemblyOf(file_name);

            for (std::uint32_t t = 0; t < many; ++t) {
                const std::uint32_t index = static_cast<std::uint32_t>(first) + t;
                if (index >= type_count_) break;
                seen_.insert(index);
                AddType(package, index, assembly);
            }
        }

        // Anything the image ranges never reached. Counted rather than dropped in silence.
        for (std::uint32_t index = 0; index < type_count_; ++index)
            if (!seen_.count(index)) ++stats_.orphan_types;
    }

    void ReadEverythingIntoOnePackage(ir::Dump& dump) {
        auto& package = PackageFor(dump, "<unknown>");
        for (std::uint32_t index = 0; index < type_count_; ++index)
            AddType(package, index, {});
    }

    void AddType(ir::Package& package, std::uint32_t index, const std::string& assembly) {
        ir::Struct record;
        record.name       = Name(TypeSlot(index, layout_.name_slot));
        record.name_space = layout_.namespace_slot >= 0
                                ? Name(TypeSlot(index, layout_.namespace_slot))
                                : std::string{};
        if (record.name.empty()) return;

        // Same identity rule as the live walk: outer-first for a nested type, then the
        // assembly on the end, because a C# name is only unique inside its assembly. Two
        // readings of one game have to agree on this or nothing downstream can match them.
        record.path = NameOf(index);
        if (record.path.empty()) return;
        if (!assembly.empty()) record.path += ", " + assembly;

        record.is_class = true;
        if (layout_.type_token_slot >= 0)
            record.token = static_cast<std::uint32_t>(TypeSlot(index, layout_.type_token_slot));

        AddMembers(record, index);
        ++stats_.types;
        package.classes.push_back(std::move(record));
    }

    void AddMembers(ir::Struct& record, std::uint32_t index) {
        if (const auto* range = layout_.RangeTo(layout_.tables.fields)) {
            const std::int32_t first = TypeSlot(index, range->start_slot);
            const std::uint16_t many = TypeCount(index, range->count_slot);
            const auto& span = layout_.Span(layout_.tables.fields);
            for (std::uint16_t f = 0; first >= 0 && f < many; ++f) {
                const std::size_t at = span.offset +
                    (static_cast<std::size_t>(first) + f) * layout_.field_record;
                if (at + static_cast<std::size_t>(layout_.field_record) > file_.size()) break;

                ir::Property property;
                property.name = Name(ReadI32(file_, at));
                if (property.name.empty()) continue;

                // The type is an index into an array that is not in this file, so there is
                // nothing honest to put here.
                property.offset_unresolved = true;
                property.type.raw = "<from the binary>";
                ++stats_.fields;
                record.properties.push_back(std::move(property));
            }
        }

        if (const auto* range = layout_.RangeTo(layout_.tables.methods)) {
            const std::int32_t first = TypeSlot(index, range->start_slot);
            const std::uint16_t many = TypeCount(index, range->count_slot);
            const auto& span = layout_.Span(layout_.tables.methods);
            for (std::uint16_t m = 0; first >= 0 && m < many; ++m) {
                const std::size_t at = span.offset +
                    (static_cast<std::size_t>(first) + m) * layout_.method_record;
                if (at + static_cast<std::size_t>(layout_.method_record) > file_.size()) break;

                ir::Function function;
                function.name = Name(ReadI32(file_, at));
                if (function.name.empty()) continue;

                if (layout_.method_token_slot >= 0)
                    function.token = static_cast<std::uint32_t>(
                        ReadI32(file_, at + static_cast<std::size_t>(layout_.method_token_slot) * 4));
                ++stats_.methods;
                record.functions.push_back(std::move(function));
            }
        }
    }

    ir::Package& PackageFor(ir::Dump& dump, const std::string& name) {
        for (auto& package : dump.packages)
            if (package.name == name) return package;
        dump.packages.push_back(ir::Package{name, {}, {}, {}});
        return dump.packages.back();
    }

    void Finish(ir::Dump& dump) {
        std::sort(dump.packages.begin(), dump.packages.end(),
                  [](const ir::Package& a, const ir::Package& b) { return a.name < b.name; });
    }

    std::span<const std::uint8_t> file_;
    const MetadataLayout&         layout_;
    StaticStats&                  stats_;
    std::uint32_t                 type_count_{0};
    std::unordered_set<std::uint32_t> seen_;
    std::unordered_map<std::uint32_t, std::uint32_t> outer_of_;
};

} // namespace

ir::Dump ReadStaticDump(std::span<const std::uint8_t> file, const MetadataLayout& layout,
                        StaticStats& stats) {
    Reader reader(file, layout, stats);
    return reader.Run();
}



// ===================================================================================
// Dual mode
// ===================================================================================

namespace {

// Every type in a dump, by path. Path is the identity everywhere else in the IR -- the
// linter rejects two types sharing one, the diff matches on it -- so it is what the two
// readings get joined on. The token corroborates and is reported when it disagrees.
std::unordered_map<std::string, const ir::Struct*> ByPath(const ir::Dump& dump) {
    std::unordered_map<std::string, const ir::Struct*> out;
    for (const auto& package : dump.packages) {
        for (const auto& record : package.classes) out.emplace(record.path, &record);
        for (const auto& record : package.structs) out.emplace(record.path, &record);
    }
    return out;
}

// Every path a dump already uses, enums included.
//
// Enums are not ir::Structs so they are not in ByPath, and leaving them out of the "does the
// live side already have this?" test means a static type lands beside a live enum of the same
// path. Two records on one path is a contradiction the linter rejects, and Zdex, whose types
// table has a unique path, silently keeps whichever it saw first -- which is how a merged
// Cave Crawlers went up with 10 enums instead of 1,641.
std::unordered_set<std::string> PathsIn(const ir::Dump& dump) {
    std::unordered_set<std::string> out;
    for (const auto& package : dump.packages) {
        for (const auto& record : package.classes) out.insert(record.path);
        for (const auto& record : package.structs) out.insert(record.path);
        for (const auto& record : package.enums)   out.insert(record.path);
    }
    return out;
}

void NoteConflict(ir::Dump& into, MergeStats& stats, std::string path, std::string member,
                  std::string field, std::string live, std::string other, std::string used) {
    ++stats.conflicts;
    into.header.conflicts.push_back(ir::Conflict{std::move(path), std::move(member),
                                                 std::move(field), std::move(live),
                                                 std::move(other), std::move(used)});
}

} // namespace

ir::Dump MergeDumps(const ir::Dump& live, const ir::Dump& from_metadata, MergeStats& stats) {
    ir::Dump out = live;
    out.header.sources = {"live", "static"};
    out.header.conflicts.clear();

    // A dual dump is only partial if the live half was. The metadata half always is, by
    // construction, and saying the merged result is partial because of that would refuse
    // every emitter for no reason.
    out.header.partial = live.header.partial;

    for (const auto& line : from_metadata.header.engine.evidence)
        out.header.engine.evidence.push_back("metadata: " + line);

    // Each half knows something the other does not: the metadata names its own version, the
    // live runtime is what the confidence was measured against.
    if (out.header.engine.version.empty())
        out.header.engine.version = from_metadata.header.engine.version;

    const auto live_types = PathsIn(live);
    const auto meta_types = ByPath(from_metadata);

    // Pass one: what the runtime already has, corroborated against the metadata.
    for (auto& package : out.packages) {
        const auto fill = [&](std::vector<ir::Struct>& records) {
            for (auto& record : records) {
                const auto it = meta_types.find(record.path);
                if (it == meta_types.end()) {
                    ++stats.live_only;
                    record.source = "live";
                    continue;
                }
                ++stats.in_both;
                record.source = "both";

                const ir::Struct& other = *it->second;

                // The token is the one thing both sides state outright, so it is the one
                // thing that can be checked rather than merged. A mismatch means the two
                // files are not describing the same build, and that is worth shouting about.
                if (other.token != 0 && record.token != 0 && other.token != record.token) {
                    NoteConflict(out, stats, record.path, {}, "token",
                                 std::format("{:#x}", record.token),
                                 std::format("{:#x}", other.token), "live");
                } else if (record.token == 0 && other.token != 0) {
                    record.token = other.token;   // the metadata knew and the runtime did not
                }

                // Members the metadata declares and the walk never saw. A stripped build is
                // the usual reason, and the name alone is worth more than nothing.
                std::unordered_set<std::string> have;
                have.reserve(record.properties.size());
                for (const auto& property : record.properties) have.insert(property.name);
                for (const auto& property : other.properties) {
                    if (have.count(property.name)) continue;
                    auto copy = property;
                    copy.offset_unresolved = true;
                    record.properties.push_back(std::move(copy));
                    NoteConflict(out, stats, record.path, property.name, "field",
                                 "absent", "declared", "static");
                }
            }
        };
        fill(package.classes);
        fill(package.structs);
    }

    // Pass two: types the metadata declares that the runtime never built. Nothing ever
    // touched them, so the class cache never held one -- they are still part of the build.
    for (const auto& package : from_metadata.packages) {
        for (const auto& record : package.classes) {
            if (live_types.count(record.path)) continue;
            ++stats.static_only;

            auto copy = record;
            copy.source = "static";
            ir::Package* into = nullptr;
            for (auto& existing : out.packages)
                if (existing.name == package.name) { into = &existing; break; }
            if (!into) {
                out.packages.push_back(ir::Package{package.name, {}, {}, {}});
                into = &out.packages.back();
            }
            into->classes.push_back(std::move(copy));
        }
    }

    std::sort(out.packages.begin(), out.packages.end(),
              [](const ir::Package& a, const ir::Package& b) { return a.name < b.name; });
    return out;
}

} // namespace zircon::il2cpp
