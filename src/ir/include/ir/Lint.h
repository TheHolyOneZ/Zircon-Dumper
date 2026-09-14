#pragma once

// Structural checks on a finished dump - does the IR agree with itself. Overlapping
// members, offsets past the end of a type, an enum too narrow for its own values, a super
// that isn't in the file.
//
// Every derived offset already carries evidence and a confidence value. Nothing was
// reading the finished dump back and asking whether the pieces actually fit, and that's
// where a derivation going wrong in a new way tends to show up first: some class whose
// members stop tiling it.
//
// Pure function of the IR like the emitters, so no game and no memory source needed.

#include "ir/Model.h"

#include <cstddef>
#include <string>
#include <vector>

namespace zircon::ir {

enum class LintSeverity {
    // dump contradicts itself. something upstream is wrong and whatever you render from it
    // will be wrong in the same spot.
    Error,
    // suspicious, or just not provable from one file. a filtered dump has no ancestors to
    // point at and is still perfectly good.
    Warning,
};

struct LintFinding {
    LintSeverity severity{LintSeverity::Warning};
    std::string  check;    // stable id, e.g. "property-overlap"
    std::string  where;    // "/Script/Engine.Actor.MaxSpeed"
    std::string  detail;
};

struct LintReport {
    std::vector<LintFinding> findings;

    std::size_t errors{0};
    std::size_t warnings{0};

    std::size_t types_checked{0};
    std::size_t properties_checked{0};
    std::size_t enums_checked{0};

    bool clean() const { return errors == 0 && warnings == 0; }
};

// Dump order, not sorted by severity. The first problem in a file is usually the one that
// caused the rest.
LintReport Lint(const Dump& dump);

std::string_view ToString(LintSeverity severity);

} // namespace zircon::ir
