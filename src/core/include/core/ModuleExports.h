#pragma once

#include "core/MemorySource.h"
#include "core/PeImage.h"

#include <cstdint>
#include <vector>

namespace zircon::core {

std::vector<PeExport> ReadModuleExports(IMemorySource& memory, Address module_base);

} // namespace zircon::core
