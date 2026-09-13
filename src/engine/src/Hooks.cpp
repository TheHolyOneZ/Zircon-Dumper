#include "engine/Hooks.h"
#include "core/Log.h"

namespace zircon::engine {
namespace {

// Process-wide on purpose. A hook describes the *target*, not one walk of it, so
// every layout pass and every emitter in the run has to see the same one. Every shell
// Zircon ships has exactly one target per process.
NameEntryDecoder& DecoderSlot() {
    static NameEntryDecoder decoder;
    return decoder;
}

GlobalResolver& ResolverSlot() {
    static GlobalResolver resolver;
    return resolver;
}

} // namespace

void SetNameEntryDecoder(NameEntryDecoder decoder) {
    if (decoder) core::LogInfo("a name-entry decoder was installed");
    DecoderSlot() = std::move(decoder);
}

const NameEntryDecoder& GetNameEntryDecoder() { return DecoderSlot(); }

void SetGlobalResolver(GlobalResolver resolver) {
    if (resolver) core::LogInfo("a global resolver was installed");
    ResolverSlot() = std::move(resolver);
}

const GlobalResolver& GetGlobalResolver() { return ResolverSlot(); }

bool ResolveGlobals(core::IMemorySource& memory, GlobalCandidates& out) {
    const auto& resolver = ResolverSlot();
    if (!resolver) return false;

    GlobalCandidates candidates;
    if (!resolver(memory, candidates)) return false;

    out = candidates;
    core::LogInfo("resolver offered GObjects {:#x}, FNamePool {:#x} — both still validated",
                  core::Raw(candidates.gobjects), core::Raw(candidates.name_pool));
    return true;
}

} // namespace zircon::engine
