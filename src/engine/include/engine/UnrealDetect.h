#pragma once

#include "core/ProcessList.h"

#include <string>
#include <vector>

namespace zircon::engine {

// How sure we are that a process is an Unreal Engine game, and why. Confidence is
// reported, never thresholded internally: the CLI shows everything with a score,
// the GUI sorts by it, and the user stays able to attach to something we scored low.
struct UnrealCandidate {
    core::ProcessInfo process;
    std::string       project;     // "StormEscape" from StormEscape-Win64-Shipping.exe
    float             confidence{0.0f};
    std::vector<std::string> evidence;
};

// Scores a single process without opening it for memory access. Everything here comes
// from the executable name and path, so it is cheap enough to run across every process
// on the machine and safe to run repeatedly for a GUI refresh.
UnrealCandidate ScoreProcess(const core::ProcessInfo& process);

// All running processes that score above `minimum_confidence`, best first.
std::vector<UnrealCandidate> DetectUnrealProcesses(float minimum_confidence = 0.5f);

} // namespace zircon::engine
