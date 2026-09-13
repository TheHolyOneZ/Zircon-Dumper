#pragma once

// Loading plugins. See Loader.cpp for why this is opt-in and not automatic.

#include <string>
#include <string_view>
#include <vector>

namespace zircon::plugin {

struct LoadResult {
    std::string path;
    bool        ok{false};
    std::size_t emitters{0};   // how many it registered
    std::string error;         // empty when ok
};

// Loads every *.dll in `directory`, in sorted order. A file that is not a Zircon plugin,
// or that fails to initialise, is reported and skipped; it doesn't abort the rest.
std::vector<LoadResult> LoadPluginsFrom(std::string_view directory);

} // namespace zircon::plugin
