#include "Emitters.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit {
namespace {

// One file for /Script/Engine runs to several megabytes. Editors handle that badly and web
// viewers truncate it outright, so large packages get split across numbered parts. Hence
// every cross-link going through a location map instead of being computed from the name.
constexpr std::size_t kMaxTypesPerFile = 300;

// Past this Graphviz and Mermaid both give up, and so does a browser rendering the table.
// Emitted anyway, since quietly dropping types is worse, but reported.
constexpr std::size_t kLargePackageWarning = 2000;

std::string Join(std::string_view dir, std::string_view leaf) {
    std::filesystem::path path(dir);
    path /= leaf;
    return path.lexically_normal().string();
}

// Only what actually changes rendering. Escaping every punctuation mark turns a UE path
// into backslash soup.
std::string MdText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '\\': case '`': case '*': case '_':
            case '[':  case ']': case '<': case '>': case '|':
                out.push_back('\\');
                out.push_back(c);
                break;
            case '\r': case '\n':
                out.push_back(' ');
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

// Inside a span only two things still break the table: a pipe ends the cell, a literal
// backtick closes the span early.
std::string MdCode(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('`');
    for (const char c : text) {
        if (c == '`')                      out.push_back('\'');
        else if (c == '|')                 out += "\\|";
        else if (c == '\r' || c == '\n')   out.push_back(' ');
        else                               out.push_back(c);
    }
    out.push_back('`');
    return out;
}

std::string Slug(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        else if (c >= 'A' && c <= 'Z') out.push_back(static_cast<char>(c - 'A' + 'a'));
        else out.push_back('-');
    }
    return out;
}

struct Location {
    std::string file;     // relative to out_dir, so links work from index.md
    std::string anchor;
};

struct DocContext {
    std::unordered_map<std::string, char>        prefixes;   // type path -> C++ prefix
    std::unordered_map<std::string, Location>    locations;  // type path -> where documented
    std::unordered_set<std::string>              enum_paths;
};

// Falls back when the reference points outside the dump, which happens whenever a package
// filter excludes the target.
std::string Referenced(const DocContext& context, const ir::TypeRef& type, char fallback) {
    if (type.name.empty()) return std::string(1, fallback) + "Unknown";

    const auto it = context.prefixes.find(type.name);
    const char prefix = it == context.prefixes.end() ? fallback : it->second;
    return prefix + util::LeafName(type.name);
}

std::string RenderType(const DocContext& context, const ir::TypeRef& type);

std::string RenderParam(const DocContext& context, const ir::TypeRef& type,
                        std::size_t index) {
    if (index >= type.params.size()) return "?";
    return RenderType(context, type.params[index]);
}

// Every enumerator listed, no `default:`. The switch then raises C4062 when a TypeKind is
// added, and a zero-warning policy turns that into a build failure instead of a type
// silently rendering as "unknown".
std::string RenderType(const DocContext& context, const ir::TypeRef& type) {
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

        case ir::TypeKind::Enum:
            // Enum names usually carry their own E already.
            return type.name.empty() ? "uint8" : util::LeafName(type.name);
        case ir::TypeKind::Struct:       return Referenced(context, type, 'F');
        case ir::TypeKind::ObjectPtr:    return Referenced(context, type, 'U') + "*";
        case ir::TypeKind::WeakPtr:      return "TWeakObjectPtr<" + Referenced(context, type, 'U') + ">";
        case ir::TypeKind::LazyPtr:      return "TLazyObjectPtr<" + Referenced(context, type, 'U') + ">";
        case ir::TypeKind::SoftPtr:      return "TSoftObjectPtr<" + Referenced(context, type, 'U') + ">";
        case ir::TypeKind::SoftClassPtr: return "TSoftClassPtr<" + Referenced(context, type, 'U') + ">";
        case ir::TypeKind::ClassPtr:     return "TSubclassOf<" + Referenced(context, type, 'U') + ">";
        case ir::TypeKind::Interface:    return "TScriptInterface<" + Referenced(context, type, 'I') + ">";

        case ir::TypeKind::Array:    return "TArray<" + RenderParam(context, type, 0) + ">";
        case ir::TypeKind::Set:      return "TSet<" + RenderParam(context, type, 0) + ">";
        case ir::TypeKind::Optional: return "TOptional<" + RenderParam(context, type, 0) + ">";
        case ir::TypeKind::Map:
            return "TMap<" + RenderParam(context, type, 0) + ", " +
                   RenderParam(context, type, 1) + ">";

        case ir::TypeKind::Delegate:          return "FDelegate";
        case ir::TypeKind::MulticastDelegate: return "FMulticastDelegate";
        case ir::TypeKind::FieldPath:         return "TFieldPath";

        case ir::TypeKind::Unknown:
            // The engine's class name beats "unknown", and the size says how much space
            // the member really takes.
            return type.raw.empty() ? "unknown" : type.raw;
    }
    return "unknown";
}

std::string RenderSignature(const DocContext& context, const ir::Function& function) {
    std::string returns = "void";
    std::string params;

    for (const auto& param : function.params) {
        if (param.is_return) {
            returns = RenderType(context, param.type);
            continue;
        }
        if (!params.empty()) params += ", ";
        if (param.is_const) params += "const ";
        if (param.is_out)   params += "out ";
        params += RenderType(context, param.type) + " " + param.name;
    }
    return returns + " " + function.name + "(" + params + ")";
}

std::string RenderFlags(const std::vector<std::string>& names, std::uint64_t raw) {
    if (!names.empty()) {
        std::string out;
        for (const auto& name : names) {
            if (!out.empty()) out += "\\|";
            out += name;
        }
        return out;
    }
    // Only for a dump taken before property flag names were decoded. Hex is all there is,
    // and it beats an empty column reading as "no flags".
    return raw == 0 ? std::string{} : std::format("0x{:016X}", raw);
}

// Classes, then structs, then enums. The order a reader wants, and the order the location
// map assigns parts in, so links and content never disagree.
std::size_t TypeCount(const ir::Package& package) {
    return package.classes.size() + package.structs.size() + package.enums.size();
}

std::string PartFile(const std::string& base, std::size_t part) {
    return part == 0 ? base + ".md" : std::format("{}.part{}.md", base, part + 1);
}

} // namespace

EmitResult EmitDocs(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "the dump is partial (no object data); pass --allow-partial to "
                       "document it anyway";
        return result;
    }

    // ---- select packages -------------------------------------------------------------
    std::vector<const ir::Package*> packages;
    for (const auto& package : dump.packages) {
        if (!options.package_filter.empty() &&
            package.name.find(options.package_filter) == std::string::npos)
            continue;
        packages.push_back(&package);
    }

    if (packages.empty()) {
        result.error = options.package_filter.empty()
            ? "the dump contains no packages"
            : "package filter '" + options.package_filter + "' matched no packages";
        return result;
    }

    // ---- assign every type a file and anchor before rendering anything ---------------
    // Two passes, because a super link can point forwards into a part not yet written, or
    // into a different package entirely.
    DocContext context;
    std::unordered_map<const ir::Package*, std::string> stems;
    std::unordered_set<std::string> used_stems;

    for (const auto* package : packages) {
        std::string base = util::PackageFileStem(package->name);

        // Two packages can share a leaf name (/Script/Engine and /Game/Engine). Without
        // this the second overwrites the first's files without a word.
        std::string candidate = base;
        for (int suffix = 2; !used_stems.insert(candidate).second; ++suffix)
            candidate = std::format("{}_{}", base, suffix);
        stems[package] = candidate;

        std::size_t index = 0;
        auto assign = [&](const std::string& path, const std::string& display) {
            Location location;
            location.file   = PartFile(candidate, index / kMaxTypesPerFile);
            location.anchor = Slug(display);
            context.locations[path] = std::move(location);
            ++index;
        };

        for (const auto& record : package->classes) {
            context.prefixes[record.path] = util::CppPrefixFor(dump, record);
            assign(record.path, context.prefixes[record.path] + record.name);
        }
        for (const auto& record : package->structs) {
            context.prefixes[record.path] = 'F';
            assign(record.path, "F" + record.name);
        }
        for (const auto& record : package->enums) {
            context.enum_paths.insert(record.path);
            assign(record.path, record.name);
        }

        if (TypeCount(*package) > kLargePackageWarning) {
            result.warnings.push_back(std::format(
                "{} holds {} types; its documentation is split across {} parts",
                package->name, TypeCount(*package),
                (TypeCount(*package) + kMaxTypesPerFile - 1) / kMaxTypesPerFile));
        }
    }

    // ---- index -----------------------------------------------------------------------
    {
        std::string out;
        out += "# Zircon reflection reference\n\n";
        out += std::format("Generated by Zircon {} at {}\n\n",
                           MdText(dump.header.tool_version), MdText(dump.header.created_utc));

        out += "| | |\n|---|---|\n";
        out += std::format("| Source | {} ({}) |\n", MdCode(dump.header.source.process),
                           MdText(dump.header.source.kind));
        out += std::format("| Engine | {} ({:.0f}% confidence) |\n",
                           MdText(dump.header.engine.version.empty()
                                      ? "unknown" : dump.header.engine.version),
                           static_cast<double>(dump.header.engine.confidence) * 100.0);
        out += std::format("| Packages | {} |\n", packages.size());
        out += std::format("| Classes | {} |\n", dump.TotalClasses());
        out += std::format("| Structs | {} |\n", dump.TotalStructs());
        out += std::format("| Enums | {} |\n", dump.TotalEnums());
        out += std::format("| Properties | {} |\n", dump.TotalProperties());
        out += std::format("| Functions | {} |\n\n", dump.TotalFunctions());

        if (!dump.header.engine.evidence.empty()) {
            out += "## How the engine was identified\n\n";
            for (const auto& line : dump.header.engine.evidence)
                out += "- " + MdText(line) + "\n";
            out += "\n";
        }

        out += "## Packages\n\n";
        out += "| Package | Classes | Structs | Enums |\n|---|---:|---:|---:|\n";

        std::vector<const ir::Package*> sorted = packages;
        std::sort(sorted.begin(), sorted.end(),
                  [](const ir::Package* a, const ir::Package* b) { return a->name < b->name; });

        for (const auto* package : sorted) {
            out += std::format("| [{}]({}) | {} | {} | {} |\n",
                               MdText(package->name), PartFile(stems[package], 0),
                               package->classes.size(), package->structs.size(),
                               package->enums.size());
        }

        const std::string path = Join(options.out_dir, "index.md");
        if (!util::WriteFile(path, out, result.error)) return result;
        result.files.push_back(path);
    }

    // ---- one file per package part ----------------------------------------------------
    for (const auto* package : packages) {
        const std::string& stem = stems[package];

        // The displayed name must carry the same prefix the target's own heading uses, or
        // link text and anchor disagree. "Object" pointing at "#uobject" reads as broken
        // even though it resolves.
        auto display_of = [&](const std::string& path) {
            const auto prefix = context.prefixes.find(path);
            const char letter = prefix == context.prefixes.end() ? 'U' : prefix->second;
            return letter + util::LeafName(path);
        };

        // Every doc file sits in the output directory, so a bare file name is always the
        // right relative path.
        auto link_to = [&](const std::string& path) {
            const std::string display = display_of(path);
            const auto it = context.locations.find(path);
            if (it == context.locations.end()) return MdCode(display);
            return std::format("[{}]({}#{})", MdCode(display), it->second.file,
                               it->second.anchor);
        };

        struct Part {
            std::string toc;
            std::string body;
        };
        std::vector<Part> parts((TypeCount(*package) + kMaxTypesPerFile - 1) /
                                std::max<std::size_t>(kMaxTypesPerFile, 1));
        if (parts.empty()) parts.resize(1);

        std::size_t index = 0;
        auto part_for = [&]() -> Part& {
            return parts[std::min(index / kMaxTypesPerFile, parts.size() - 1)];
        };

        auto emit_struct = [&](const ir::Struct& record) {
            Part& part = part_for();
            const char prefix = record.is_class ? context.prefixes[record.path] : 'F';
            const std::string display = prefix + record.name;

            part.toc += std::format("- [{}](#{})\n", MdCode(display), Slug(display));

            part.body += std::format("### {} <a id=\"{}\"></a>\n\n", MdText(display),
                                     Slug(display));
            part.body += std::format("{}\n\n", MdCode(record.path));
            part.body += std::format("Size `0x{:X}` ({} bytes), alignment `{}`",
                                     record.size, record.size, record.alignment);
            if (!record.super.empty())
                part.body += ", inherits " + link_to(record.super);
            part.body += "\n\n";

            if (!record.interfaces.empty()) {
                part.body += "Implements:";
                for (const auto& iface : record.interfaces)
                    part.body += " " + link_to(iface);
                part.body += "\n\n";
            }

            if (!record.properties.empty()) {
                // Only worth its width when the dump actually has defaults. Otherwise every
                // table in the reference gets a column of dashes.
                const bool defaults = std::any_of(
                    record.properties.begin(), record.properties.end(),
                    [](const ir::Property& p) { return !p.default_value.empty(); });

                part.body += defaults
                    ? "| Offset | Size | Type | Name | Default | Flags |\n|---|---|---|---|---|---|\n"
                    : "| Offset | Size | Type | Name | Flags |\n|---|---|---|---|---|\n";
                for (const auto& property : record.properties) {
                    std::string name = property.name;
                    if (property.array_dim > 1)
                        name += std::format("[{}]", property.array_dim);

                    std::string suffix;
                    if (property.is_bitfield) {
                        // Two properties legitimately share a byte, and anyone reproducing
                        // the layout needs the exact mask.
                        suffix = std::format(" : 1  // bit {}, mask 0x{:02X}",
                                             property.bit_index, property.field_mask);
                    }

                    if (defaults) {
                        part.body += std::format(
                            "| `0x{:04X}` | `0x{:X}` | {} | {} | {} | {} |\n",
                            property.offset, property.size,
                            MdCode(RenderType(context, property.type)),
                            MdCode(name + suffix),
                            MdCode(property.default_value),
                            RenderFlags(property.flag_names, property.flags));
                    } else {
                        part.body += std::format("| `0x{:04X}` | `0x{:X}` | {} | {} | {} |\n",
                                                 property.offset, property.size,
                                                 MdCode(RenderType(context, property.type)),
                                                 MdCode(name + suffix),
                                                 RenderFlags(property.flag_names, property.flags));
                    }
                }
                part.body += "\n";
            }

            if (!record.functions.empty()) {
                part.body += "**Functions**\n\n";
                for (const auto& function : record.functions) {
                    part.body += "- " + MdCode(RenderSignature(context, function));
                    if (function.native_rva != 0)
                        part.body += std::format(" &mdash; `+0x{:X}`", function.native_rva);
                    if (!function.flag_names.empty()) {
                        std::string flags;
                        for (const auto& flag : function.flag_names) {
                            if (!flags.empty()) flags += "\\|";
                            flags += flag;
                        }
                        part.body += " &mdash; " + flags;
                    }
                    part.body += "\n";
                }
                part.body += "\n";
            }

            ++index;
        };

        for (const auto& record : package->classes) emit_struct(record);
        for (const auto& record : package->structs) emit_struct(record);

        for (const auto& record : package->enums) {
            Part& part = part_for();
            part.toc += std::format("- [{}](#{})\n", MdCode(record.name), Slug(record.name));

            part.body += std::format("### {} <a id=\"{}\"></a>\n\n", MdText(record.name),
                                     Slug(record.name));
            part.body += std::format("{} &mdash; underlying `{}`{}\n\n", MdCode(record.path),
                                     MdText(record.underlying),
                                     record.is_flags ? ", bit flags" : "");

            if (!record.values.empty()) {
                part.body += "| Name | Value |\n|---|---:|\n";
                for (const auto& value : record.values)
                    part.body += std::format("| {} | {} |\n", MdCode(value.name), value.value);
                part.body += "\n";
            }
            ++index;
        }

        for (std::size_t part_index = 0; part_index < parts.size(); ++part_index) {
            std::string out;
            out += std::format("# {}\n\n", MdText(package->name));
            if (parts.size() > 1) {
                out += std::format("Part {} of {} &mdash; ", part_index + 1, parts.size());
                for (std::size_t other = 0; other < parts.size(); ++other) {
                    if (other) out += " ";
                    if (other == part_index) out += std::format("**{}**", other + 1);
                    else out += std::format("[{}]({})", other + 1,
                                            PartFile(stem, other));
                }
                out += "\n\n";
            }
            out += "[Index](index.md)\n\n";

            if (!parts[part_index].toc.empty())
                out += "## Contents\n\n" + parts[part_index].toc + "\n";
            out += parts[part_index].body;

            const std::string path = Join(options.out_dir, PartFile(stem, part_index));
            if (!util::WriteFile(path, out, result.error)) return result;
            result.files.push_back(path);
        }
    }

    return result;
}

} // namespace zircon::emit
