#pragma once

// Two places where a target can be strange enough that derivation alone will not reach it,
// and where someone who knows *this* game can supply the missing step.
//
// Both are deliberately narrow, and neither is trusted. A hook supplies an input; Zircon
// still proves it with the same checks it applies to anything it found itself. A global
// resolver that returns a wrong address gets the wrong address rejected, not written into
// the dump — which is the only way an extension point is safe in a tool whose entire value
// is that its output is correct.
//
// The engine layer owns these because that is where they are called. It does not know the
// plugin layer exists: a hook is a std::function, and whoever wants to fill it in — the
// plugin bridge, a test, a future scripting host — links downward, never the reverse.

#include "core/MemorySource.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace zircon::engine {

// --- encrypted or otherwise non-standard FName pools ---------------------------------
//
// Some shipped games encrypt name-pool entries, or store them in a layout no amount of
// deriving will recognise. The decoder is handed the address of one entry and fills a
// buffer with the plain bytes of that entry: the two-byte header followed by the
// characters, exactly as an unencrypted build would have them in memory.
//
// Returns the number of bytes written, or 0 to decline — declining is normal and means
// the ordinary path runs, so a decoder that only handles some entries is fine.
using NameEntryDecoder = std::function<std::size_t(core::IMemorySource& memory,
                                                   core::Address entry,
                                                   std::uint8_t* out,
                                                   std::size_t capacity)>;

void SetNameEntryDecoder(NameEntryDecoder decoder);
const NameEntryDecoder& GetNameEntryDecoder();

// --- globals that cannot be scanned for ------------------------------------------------
//
// A game that relocates or encrypts GObjects can still be dumped by someone who knows
// where it ends up. A resolver runs *before* the built-in search and supplies candidates;
// whatever it returns goes through the same validation as a scan result, so a wrong answer
// is rejected.
struct GlobalCandidates {
    core::Address gobjects{};    // the FUObjectArray, or null to leave it to the scan
    core::Address name_pool{};   // the FNamePool block array, or null likewise
};

using GlobalResolver = std::function<bool(core::IMemorySource& memory,
                                          GlobalCandidates& out)>;

void SetGlobalResolver(GlobalResolver resolver);
const GlobalResolver& GetGlobalResolver();

// Asks the registered resolver, if any. Returns false when there is none or it declined.
bool ResolveGlobals(core::IMemorySource& memory, GlobalCandidates& out);

} // namespace zircon::engine
