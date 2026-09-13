#include "Emitters.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit {
namespace {

// Roughly where Graphviz stops producing anything a human can read and Mermaid starts
// refusing outright. The graph still gets emitted in full, since dropping classes would
// make the output quietly wrong, but the caller hears about it.
constexpr std::size_t kUnreadableNodeCount = 1500;

std::string Join(std::string_view dir, std::string_view leaf) {
    std::filesystem::path path(dir);
    path /= leaf;
    return path.lexically_normal().string();
}

// DOT strings are double quoted, so only the quote and the escape itself end one early.
std::string DotString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '\r' || c == '\n') { out.push_back(' '); continue; }
        out.push_back(c);
    }
    return out;
}

// Mermaid has no escape character inside a quoted label, only HTML entities. An unescaped
// quote or angle bracket takes the whole diagram down with it.
std::string MermaidLabel(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '"':  out += "#quot;"; break;
            case '<':  out += "#lt;";   break;
            case '>':  out += "#gt;";   break;
            case '&':  out += "#amp;";  break;
            case '\r': case '\n': out.push_back(' '); break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

struct ClassRef {
    const ir::Struct* record{};
    const ir::Package* package{};
};

} // namespace

EmitResult EmitGraphs(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "the dump is partial (no object data); pass --allow-partial to "
                       "graph it anyway";
        return result;
    }

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

    // Index the *whole* dump, not just selected packages. A filtered package still
    // inherits from classes outside the filter, and those edges are the interesting ones.
    std::unordered_map<std::string, ClassRef> by_path;
    std::unordered_map<std::string, char>     prefixes;
    for (const auto& package : dump.packages)
        for (const auto& record : package.classes)
            by_path[record.path] = ClassRef{&record, &package};

    auto display_for = [&](const std::string& path) -> std::string {
        const auto known = by_path.find(path);
        if (known == by_path.end()) return util::LeafName(path);

        auto cached = prefixes.find(path);
        if (cached == prefixes.end()) {
            cached = prefixes.emplace(path, util::CppPrefixFor(dump, *known->second.record))
                         .first;
        }
        return cached->second + known->second.record->name;
    };

    // ---- per-package class forests ---------------------------------------------------
    std::unordered_set<std::string> used_stems;

    for (const auto* package : packages) {
        if (package->classes.empty()) continue;

        std::string base = util::PackageFileStem(package->name);
        std::string stem = base;
        for (int suffix = 2; !used_stems.insert(stem).second; ++suffix)
            stem = std::format("{}_{}", base, suffix);

        // Edges first, so both renderings describe the same graph.
        struct Edge { std::string from; std::string to; bool external{false}; };
        std::vector<Edge> edges;
        std::set<std::string> nodes;
        std::set<std::string> external_nodes;

        for (const auto& record : package->classes) {
            nodes.insert(record.path);
            if (record.super.empty()) continue;

            // Self-super would loop a traversal. Nothing here traverses, but it would
            // also draw a meaningless self-arrow.
            if (record.super == record.path) {
                result.warnings.push_back("self-referential super on " + record.path);
                continue;
            }

            const auto super = by_path.find(record.super);
            const bool external = super == by_path.end() || super->second.package != package;
            if (external) external_nodes.insert(record.super);
            else          nodes.insert(record.super);

            edges.push_back(Edge{record.path, record.super, external});
        }

        if (nodes.size() + external_nodes.size() > kUnreadableNodeCount) {
            result.warnings.push_back(std::format(
                "{} graphs {} nodes; most renderers will struggle with it",
                package->name, nodes.size() + external_nodes.size()));
        }

        // DOT. rankdir=BT puts bases above derived, the conventional reading.
        {
            std::string out;
            out += std::format("// {} class inheritance\n", package->name);
            out += std::format("digraph \"{}\" {{\n", DotString(package->name));
            out += "  rankdir=BT;\n  node [shape=box, fontname=\"Consolas\"];\n\n";

            for (const auto& path : nodes)
                out += std::format("  \"{}\" [label=\"{}\"];\n", DotString(path),
                                   DotString(display_for(path)));

            if (!external_nodes.empty()) out += "\n";
            for (const auto& path : external_nodes) {
                out += std::format("  \"{}\" [label=\"{}\", style=dashed, color=gray];\n",
                                   DotString(path), DotString(display_for(path)));
            }

            out += "\n";
            for (const auto& edge : edges) {
                out += std::format("  \"{}\" -> \"{}\"{};\n", DotString(edge.from),
                                   DotString(edge.to),
                                   edge.external ? " [style=dashed, color=gray]" : "");
            }
            out += "}\n";

            const std::string path = Join(Join(options.out_dir, "packages"), stem + ".dot");
            if (!util::WriteFile(path, out, result.error)) return result;
            result.files.push_back(path);
        }

        // Mermaid. Node ids are synthesised because Mermaid identifiers can't contain the
        // slashes and dots a UE path is made of.
        {
            std::unordered_map<std::string, std::string> ids;
            auto id_for = [&](const std::string& path) -> const std::string& {
                auto it = ids.find(path);
                if (it == ids.end())
                    it = ids.emplace(path, std::format("n{}", ids.size())).first;
                return it->second;
            };

            std::string out;
            out += std::format("%% {} class inheritance\n", package->name);
            out += "graph BT\n";

            for (const auto& path : nodes)
                out += std::format("  {}[\"{}\"]\n", id_for(path),
                                   MermaidLabel(display_for(path)));
            for (const auto& path : external_nodes)
                out += std::format("  {}[\"{}\"]:::external\n", id_for(path),
                                   MermaidLabel(display_for(path)));

            for (const auto& edge : edges)
                out += std::format("  {} --> {}\n", id_for(edge.from), id_for(edge.to));

            out += "  classDef external stroke-dasharray: 4 4, color:#888;\n";

            const std::string path = Join(Join(options.out_dir, "packages"), stem + ".mmd");
            if (!util::WriteFile(path, out, result.error)) return result;
            result.files.push_back(path);
        }
    }

    // ---- top-level package graph ------------------------------------------------------
    // Five thousand classes in one picture is not a picture. The package graph answers
    // "what depends on what" at a scale a person can hold in their head; per-package files
    // hold the detail.
    {
        std::map<std::pair<std::string, std::string>, int> package_edges;
        std::set<std::string> package_nodes;

        for (const auto* package : packages) {
            package_nodes.insert(package->name);
            for (const auto& record : package->classes) {
                if (record.super.empty() || record.super == record.path) continue;

                const auto super = by_path.find(record.super);
                const std::string other = super == by_path.end()
                    ? util::PackageName(record.super) : super->second.package->name;
                if (other == package->name) continue;

                package_nodes.insert(other);
                ++package_edges[{package->name, other}];
            }
        }

        {
            std::string out;
            out += "// package-level inheritance; see packages/ for class detail\n";
            out += "digraph \"packages\" {\n  rankdir=BT;\n";
            out += "  node [shape=box, fontname=\"Consolas\"];\n\n";

            for (const auto& name : package_nodes)
                out += std::format("  \"{}\";\n", DotString(name));
            out += "\n";

            for (const auto& [edge, count] : package_edges) {
                out += std::format("  \"{}\" -> \"{}\" [label=\"{}\"];\n",
                                   DotString(edge.first), DotString(edge.second), count);
            }
            out += "}\n";

            const std::string path = Join(options.out_dir, "inheritance.dot");
            if (!util::WriteFile(path, out, result.error)) return result;
            result.files.push_back(path);
        }

        {
            std::unordered_map<std::string, std::string> ids;
            auto id_for = [&](const std::string& name) -> const std::string& {
                auto it = ids.find(name);
                if (it == ids.end())
                    it = ids.emplace(name, std::format("p{}", ids.size())).first;
                return it->second;
            };

            std::string out;
            out += "%% package-level inheritance; see packages/ for class detail\n";
            out += "graph BT\n";

            for (const auto& name : package_nodes)
                out += std::format("  {}[\"{}\"]\n", id_for(name), MermaidLabel(name));

            for (const auto& [edge, count] : package_edges) {
                out += std::format("  {} -->|{}| {}\n", id_for(edge.first), count,
                                   id_for(edge.second));
            }

            const std::string path = Join(options.out_dir, "inheritance.mmd");
            if (!util::WriteFile(path, out, result.error)) return result;
            result.files.push_back(path);
        }
    }

    return result;
}

} // namespace zircon::emit
