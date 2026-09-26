#pragma once

#include "mono/Assembly.h"
#include "ir/Model.h"

#include <cstddef>
#include <string>
#include <vector>

namespace zircon::mono {

struct StaticStats {
    std::size_t assemblies{0};
    std::size_t types{0};
    std::size_t classes{0};
    std::size_t structs{0};
    std::size_t enums{0};
    std::size_t fields{0};
    std::size_t methods{0};
    std::size_t accessors{0};
    std::size_t enum_values{0};
    std::size_t enums_without_values{0};
    std::size_t with_il{0};
    std::size_t indistinguishable{0};
};

ir::Dump BuildStaticDump(const std::vector<AssemblyMetadata>& assemblies, StaticStats& stats);

struct MergeStats {
    std::size_t matched{0};
    std::size_t live_only{0};
    std::size_t static_only{0};
    std::size_t enums_filled{0};
    std::size_t il_filled{0};
    std::size_t conflicts{0};
};

ir::Dump MergeDumps(const ir::Dump& live, const ir::Dump& from_metadata, MergeStats& stats);

} // namespace zircon::mono
