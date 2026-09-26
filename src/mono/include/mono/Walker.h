#pragma once

#include "mono/Bridge.h"
#include "ir/Model.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::mono {

struct WalkOptions {
    std::string filter;
    bool resolve_enum_values{true};
    std::function<void(std::string_view)> breadcrumb;
    std::vector<std::string> skip;
};

struct WalkStats {
    std::size_t assemblies{0};
    std::size_t images{0};
    std::size_t classes{0};
    std::size_t enums{0};
    std::size_t fields{0};
    std::size_t methods{0};
    std::size_t accessors{0};
    std::size_t nested{0};
    std::size_t open_generics{0};
    std::size_t unresolved_offsets{0};
    std::size_t enums_without_values{0};
    std::size_t skipped{0};
    std::size_t contradictory_bases{0};
    std::size_t unreadable_types{0};
};

ir::Dump Walk(IBridge& bridge, const WalkOptions& options, WalkStats& stats);

} // namespace zircon::mono
