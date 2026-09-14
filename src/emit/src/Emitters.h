#pragma once

// Internal declarations. Each emitter lives in its own translation unit so that
// adding a format touches exactly one new file plus the registry.

#include "emit/Emitter.h"

namespace zircon::emit {

EmitResult EmitCppSdk(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitUsmap(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitIda(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitGhidra(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitBinja(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitReClass(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitDocs(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitFridaJs(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitPythonStubs(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitGraphs(const ir::Dump& dump, const EmitOptions& options);
EmitResult EmitJson(const ir::Dump& dump, const EmitOptions& options);

} // namespace zircon::emit
