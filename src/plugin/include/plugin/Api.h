#pragma once

// The host half of the plugin ABI.

#include "core/MemorySource.h"
#include "emit/Emitter.h"
#include "zircon/plugin.h"

#include <string>
#include <string_view>

namespace zircon::plugin {

// What a plugin is actually holding when it has a ZnEmitContext*. Declared here rather
// than in the C header because a plugin must never see inside it.
struct EmitContext {
    emit::EmitOptions options;
    emit::EmitResult  result;

    // Writes one file, refusing any path that would land outside options.out_dir.
    int Write(std::string_view relative, std::string_view bytes);
};

// The vtable handed to every plugin. One shared instance; it holds no state.
const ZnApi* HostApi();

// A ZnTarget is just an IMemorySource seen through the ABI.
ZnTarget* AsTarget(core::IMemorySource& memory);

} // namespace zircon::plugin
