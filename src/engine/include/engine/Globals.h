#pragma once

#include "core/MemorySource.h"
#include "engine/FunctionLayout.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"

#include <string>
#include <vector>

namespace zircon::engine {

struct GlobalsInfo {
    core::Address gworld{};
    core::Address append_string{};
    core::Address process_event{};
    int           process_event_slot{-1};

    std::vector<std::string> evidence;
};

GlobalsInfo FindGlobals(core::IMemorySource& memory, const ObjectArrayInfo& array,
                        const UObjectLayout& object_layout, const NamePoolInfo& pool,
                        const UFunctionLayout& function_layout);

}
