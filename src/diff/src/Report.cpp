#include "diff/Diff.h"

#include <algorithm>
#include <format>
#include <map>

namespace zircon::diff {
namespace {

// Only when the caller says the destination is a terminal. Escape codes in a redirected
// file corrupt it for every downstream tool.
std::string_view ColourFor(Severity severity) {
    switch (severity) {
        case Severity::Critical: return "\033[1;31m";
        case Severity::High:     return "\033[31m";
        case Severity::Medium:   return "\033[33m";
        case Severity::Low:      return "\033[36m";
        case Severity::Info:     return "\033[90m";
    }
    return "";
}

std::string Escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else
                    out.push_back(c);
        }
    }
    return out;
}

std::string MarkdownCell(std::string_view text) {
    if (text.empty()) return "";
    // A pipe inside a cell ends the cell, and UE names do contain them. Code spans handle
    // everything bar a literal backtick, escaped separately.
    std::string out = "`";
    for (const char c : text) {
        if (c == '`') out += "'";
        else if (c == '|') out += "\\|";
        else out.push_back(c);
    }
    out += "`";
    return out;
}

} // namespace

std::string RenderText(const DiffResult& result, const ir::Dump& before,
                       const ir::Dump& after, bool colour) {
    const auto tint = [&](Severity severity) { return colour ? ColourFor(severity) : ""; };
    const std::string_view reset = colour ? "\033[0m" : "";

    std::string out;

    out += std::format("{} ({})  ->  {} ({})\n",
                       before.header.source.process.empty() ? "<unknown>"
                                                            : before.header.source.process,
                       before.header.engine.version,
                       after.header.source.process.empty() ? "<unknown>"
                                                           : after.header.source.process,
                       after.header.engine.version);
    out += std::format("{}  ->  {}\n\n", before.header.created_utc, after.header.created_utc);

    out += std::format("{:<14}{:>10}  ->{:>10}\n", "types",
                       result.stats.types_before, result.stats.types_after);
    out += std::format("{:<14}{:>10}  ->{:>10}\n", "properties",
                       result.stats.properties_before, result.stats.properties_after);
    out += std::format("{:<14}{:>10}  ->{:>10}\n", "functions",
                       result.stats.functions_before, result.stats.functions_after);

    if (result.stats.types_compared > 0) {
        const double survived = 100.0 * static_cast<double>(result.stats.types_unchanged) /
                                static_cast<double>(result.stats.types_compared);
        out += std::format("\n{} of {} types present in both are byte-identical ({:.1f}%)\n",
                           result.stats.types_unchanged, result.stats.types_compared, survived);
    }

    if (result.changes.empty()) {
        out += "\nno differences\n";
        return out;
    }

    out += "\n";
    for (auto severity : {Severity::Critical, Severity::High, Severity::Medium,
                          Severity::Low, Severity::Info}) {
        const auto it = result.by_severity.find(severity);
        if (it == result.by_severity.end() || it->second == 0) continue;
        out += std::format("{}{:<10}{} {}\n", tint(severity), ToString(severity), reset,
                           it->second);
    }

    // Grouped by owning type. One type whose base moved produces a change per member, and
    // a flat list buries the single fact behind all of them.
    out += "\n";
    std::string current_path;
    std::size_t shown = 0;
    constexpr std::size_t kMaxShown = 400;

    for (const auto& change : result.changes) {
        if (change.severity < Severity::Medium) continue;
        if (shown >= kMaxShown) break;

        if (change.path != current_path) {
            current_path = change.path;
            out += std::format("\n{}\n", current_path);
        }

        out += std::format("  {}{:<9}{} {:<26}", tint(change.severity),
                           ToString(change.severity), reset, ToString(change.kind));
        if (!change.member.empty()) out += std::format(" {}", change.member);
        if (!change.before.empty() || !change.after.empty())
            out += std::format("  {} -> {}", change.before.empty() ? "-" : change.before,
                               change.after.empty() ? "-" : change.after);
        if (!change.detail.empty()) out += std::format("   ({})", change.detail);
        out += "\n";
        ++shown;
    }

    std::size_t reportable = 0;
    for (const auto& change : result.changes)
        if (change.severity >= Severity::Medium) ++reportable;
    if (reportable > shown)
        out += std::format("\n... and {} more at medium or above\n", reportable - shown);

    return out;
}

std::string RenderJson(const DiffResult& result) {
    std::string out = "{\n";
    out += std::format("  \"types_before\": {},\n", result.stats.types_before);
    out += std::format("  \"types_after\": {},\n", result.stats.types_after);
    out += std::format("  \"types_compared\": {},\n", result.stats.types_compared);
    out += std::format("  \"types_unchanged\": {},\n", result.stats.types_unchanged);
    out += std::format("  \"properties_before\": {},\n", result.stats.properties_before);
    out += std::format("  \"properties_after\": {},\n", result.stats.properties_after);
    out += std::format("  \"functions_before\": {},\n", result.stats.functions_before);
    out += std::format("  \"functions_after\": {},\n", result.stats.functions_after);
    out += std::format("  \"breaking\": {},\n", result.HasBreakingChanges() ? "true" : "false");

    out += "  \"by_severity\": {";
    bool first = true;
    for (const auto& [severity, count] : result.by_severity) {
        if (!first) out += ",";
        first = false;
        out += std::format("\n    \"{}\": {}", ToString(severity), count);
    }
    out += first ? "},\n" : "\n  },\n";

    out += "  \"changes\": [";
    for (std::size_t i = 0; i < result.changes.size(); ++i) {
        const auto& change = result.changes[i];
        if (i) out += ",";
        out += "\n    {";
        out += std::format("\"kind\": \"{}\", ", ToString(change.kind));
        out += std::format("\"severity\": \"{}\", ", ToString(change.severity));
        out += std::format("\"path\": \"{}\"", Escape(change.path));
        if (!change.member.empty()) out += std::format(", \"member\": \"{}\"", Escape(change.member));
        if (!change.before.empty()) out += std::format(", \"before\": \"{}\"", Escape(change.before));
        if (!change.after.empty())  out += std::format(", \"after\": \"{}\"", Escape(change.after));
        if (!change.detail.empty()) out += std::format(", \"detail\": \"{}\"", Escape(change.detail));
        out += "}";
    }
    out += result.changes.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

std::string RenderMarkdown(const DiffResult& result, const ir::Dump& before,
                           const ir::Dump& after) {
    std::string out;

    out += "# Migration report\n\n";
    out += std::format("`{}` ({}) -> `{}` ({})\n\n",
                       before.header.source.process, before.header.engine.version,
                       after.header.source.process, after.header.engine.version);

    if (!result.HasBreakingChanges()) {
        out += "**No breaking changes.** Hardcoded offsets and hooks still hold.\n\n";
    } else {
        out += "**Breaking changes present.** Everything in the tables below has to be "
               "updated before existing code reads the right memory again.\n\n";
    }

    out += "| | before | after |\n|---|---:|---:|\n";
    out += std::format("| types | {} | {} |\n", result.stats.types_before,
                       result.stats.types_after);
    out += std::format("| properties | {} | {} |\n", result.stats.properties_before,
                       result.stats.properties_after);
    out += std::format("| functions | {} | {} |\n\n", result.stats.functions_before,
                       result.stats.functions_after);

    if (result.stats.types_compared > 0) {
        out += std::format("{} of {} types present in both dumps are byte-identical.\n\n",
                           result.stats.types_unchanged, result.stats.types_compared);
    }

    // What makes this a migration report rather than a change log. Every moved offset, as
    // a table you can apply to constants directly.
    std::map<std::string, std::vector<const Change*>> moved;
    for (const auto& change : result.changes)
        if (change.kind == ChangeKind::PropertyMoved) moved[change.path].push_back(&change);

    if (!moved.empty()) {
        std::size_t total = 0;
        for (const auto& [path, changes] : moved) total += changes.size();

        out += std::format("## Moved offsets ({})\n\n", total);
        out += "Any constant holding one of these now points at the wrong bytes.\n\n";

        for (const auto& [path, changes] : moved) {
            out += std::format("### {}\n\n", path);
            out += "| member | old | new | delta |\n|---|---:|---:|---:|\n";
            for (const auto* change : changes) {
                const auto old_value = std::strtoll(change->before.c_str() + 2, nullptr, 16);
                const auto new_value = std::strtoll(change->after.c_str() + 2, nullptr, 16);
                out += std::format("| {} | {} | {} | {:+d} |\n", MarkdownCell(change->member),
                                   MarkdownCell(change->before), MarkdownCell(change->after),
                                   new_value - old_value);
            }
            out += "\n";
        }
    }

    // Renames keep their offset, so they're a documentation fix, not a layout one. Mixing
    // them into the moved table overstates the damage.
    std::vector<const Change*> renamed;
    for (const auto& change : result.changes)
        if (change.kind == ChangeKind::PropertyRenamed) renamed.push_back(&change);

    if (!renamed.empty()) {
        out += std::format("## Renamed, offset unchanged ({})\n\n", renamed.size());
        out += "| type | old name | new name |\n|---|---|---|\n";
        for (const auto* change : renamed)
            out += std::format("| {} | {} | {} |\n", MarkdownCell(change->path),
                               MarkdownCell(change->before), MarkdownCell(change->after));
        out += "\n";
    }

    std::vector<const Change*> hooks;
    for (const auto& change : result.changes)
        if (change.kind == ChangeKind::FunctionMoved) hooks.push_back(&change);

    if (!hooks.empty()) {
        out += std::format("## Functions that moved ({})\n\n", hooks.size());
        out += "Hooks on these addresses need re-resolving.\n\n";
        out += "| type | function | old RVA | new RVA |\n|---|---|---:|---:|\n";
        for (const auto* change : hooks)
            out += std::format("| {} | {} | {} | {} |\n", MarkdownCell(change->path),
                               MarkdownCell(change->member), MarkdownCell(change->before),
                               MarkdownCell(change->after));
        out += "\n";
    }

    std::vector<const Change*> gone;
    for (const auto& change : result.changes)
        if (change.kind == ChangeKind::TypeRemoved ||
            change.kind == ChangeKind::PropertyRemoved ||
            change.kind == ChangeKind::FunctionRemoved)
            gone.push_back(&change);

    if (!gone.empty()) {
        out += std::format("## Removed ({})\n\n", gone.size());
        out += "| kind | type | member |\n|---|---|---|\n";
        for (const auto* change : gone)
            out += std::format("| {} | {} | {} |\n", ToString(change->kind),
                               MarkdownCell(change->path), MarkdownCell(change->member));
        out += "\n";
    }

    return out;
}

} // namespace zircon::diff
