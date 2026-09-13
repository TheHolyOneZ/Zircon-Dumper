#pragma once

// Loading the payload DLL into a target process.
//
// The one part of Zircon that writes to another process, and it writes exactly one thing:
// the path of the DLL to load. The target's own loader does the rest through LoadLibraryW.
// Nothing here hides the module, and injection is refused outright when the target has
// anti-cheat loaded (see docs/SCOPE.md).

#include "core/Types.h"

#include <cstdint>
#include <string_view>

namespace zircon::core {

Result<void> Inject(std::uint32_t pid, std::string_view dll_path);

} // namespace zircon::core
