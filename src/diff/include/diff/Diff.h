#pragma once

// IR-to-IR structural diff.
//
// The question this answers is not "what is different" but "what did the game update
// break". Those are not the same question: hundreds of new classes are noise, while one
// property moving four bytes silently corrupts every read through a hardcoded offset.
// Changes are therefore classified by what they break, not by what kind of node they
// touch.
//
// Like the emitters, this is a pure function of two dumps. It needs no target, works
// across providers, and is testable against checked-in fixtures.

#include "ir/Model.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::diff {

// Ordered by how loudly it should be reported.
enum class Severity {
    Info,       // new surface; nothing that existed before can break
    Low,        // cosmetic or metadata
    Medium,     // may matter depending on use
    High,       // will break something that reads or hooks this
    Critical,   // silently reads wrong memory, or the thing is simply gone
};

std::string_view ToString(Severity severity);

enum class ChangeKind {
    PackageAdded, PackageRemoved,

    TypeAdded, TypeRemoved, TypeResized, TypeRealigned, TypeSuperChanged,

    PropertyAdded, PropertyRemoved, PropertyMoved, PropertyResized,
    PropertyTypeChanged, PropertyFlagsChanged, PropertyBitMoved, PropertyRenamed,
    PropertyDefaultChanged,

    FunctionAdded, FunctionRemoved, FunctionSignatureChanged, FunctionMoved,
    FunctionFlagsChanged,

    EnumAdded, EnumRemoved, EnumValueAdded, EnumValueRemoved, EnumValueChanged,

    EngineVersionChanged, LayoutOffsetChanged,
};

std::string_view ToString(ChangeKind kind);
Severity DefaultSeverity(ChangeKind kind);

struct Change {
    ChangeKind  kind{};
    Severity    severity{};
    std::string path;      // owning type, or the package / global scope
    std::string member;    // property, function or enum value; empty at type level
    std::string before;
    std::string after;
    std::string detail;    // human sentence, only where the fields are not self-evident
};

struct DiffOptions {
    std::string package_filter;

    // Additions dominate the raw count on any real update and never break existing code,
    // so they are easy to drop when the question is "what broke".
    bool include_additions{true};

    Severity minimum{Severity::Info};

    // A property that vanishes while another appears at the same offset with the same
    // type is almost always a rename, and reporting it as removed+added hides that the
    // offset is still good. Costs one pass over the unmatched members of each type.
    bool detect_renames{true};
};

struct DiffStats {
    std::size_t types_before{}, types_after{};
    std::size_t properties_before{}, properties_after{};
    std::size_t functions_before{}, functions_after{};

    // Types present in both dumps whose members are all byte-identical. The headline
    // number for "how much of my work survives".
    std::size_t types_unchanged{};
    std::size_t types_compared{};
};

struct DiffResult {
    std::vector<Change> changes;
    DiffStats stats;

    std::map<Severity, std::size_t>   by_severity;
    std::map<ChangeKind, std::size_t> by_kind;

    bool HasBreakingChanges() const;
};

DiffResult Diff(const ir::Dump& before, const ir::Dump& after,
                const DiffOptions& options = {});

// --- reporting ----------------------------------------------------------------------

// Human summary: counts, then the breaking changes grouped by type. Deliberately leads
// with what broke rather than with statistics.
std::string RenderText(const DiffResult& result, const ir::Dump& before,
                       const ir::Dump& after, bool colour = false);

std::string RenderJson(const DiffResult& result);

// A migration report: every moved offset as a before/after table, so the changes can be
// applied to hardcoded constants directly.
std::string RenderMarkdown(const DiffResult& result, const ir::Dump& before,
                           const ir::Dump& after);

} // namespace zircon::diff
