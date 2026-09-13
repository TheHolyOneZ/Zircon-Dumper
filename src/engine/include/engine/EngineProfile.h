#pragma once

#include "core/MemorySource.h"

#include <optional>
#include <string>
#include <vector>

namespace zircon::engine {

// The engine facts that actually change memory layout. Version numbers are secondary:
// what the reflection walker needs to know is which shapes to expect.
struct EngineProfile {
    int major{0};                     // 0 when unknown
    int minor{0};

    // Layout-affecting switches. Each carries its own confidence in `notes` rather than
    // being defaulted behind the scenes, because guessing one of these wrong corrupts every
    // offset downstream.
    bool uses_fproperty{true};        // FProperty (>= 4.25) vs UProperty (<= 4.24)
    bool chunked_gobjects{true};      // FChunkedFixedUObjectArray vs FFixedUObjectArray
    bool chunked_name_pool{true};     // FNamePool (>= 4.23) vs TNameEntryArray
    bool case_preserving_name{false}; // WITH_CASE_PRESERVING_NAME
    bool outline_number{false};       // FNAME_OUTLINE_NUMBER

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Known() const { return major != 0; }
    std::string VersionString() const;

    // Derives the layout switches implied by a known version number. Only fills in what
    // has not already been established by direct observation, since a measured fact
    // always beats an inference from a version string.
    void ApplyVersionDefaults();
};

// Stage 1 fingerprinting: scan the image for embedded engine version strings.
//
// A *hint only*, and deliberately reported as one. Two measured failure modes, both in
// docs/UE-Test.md: licensee builds replace the string outright, and some shipping games
// carry no version string at all. A hit raises confidence, a miss proves nothing. Stage 2
// derives the layout from memory and is the authority.
struct VersionHint {
    int         major{0};
    int         minor{0};
    std::string raw;          // the literal matched text, for the dump header
    bool        licensee{false};   // branded build: "++Project+SN2-Release-CL-123362"
};

std::vector<VersionHint> ScanVersionStrings(core::IMemorySource& memory);

// Runs every fingerprinting stage the given source supports. A static image can only
// reach stage 1, so the returned profile will say so in its confidence and evidence.
EngineProfile FingerprintEngine(core::IMemorySource& memory);

} // namespace zircon::engine
