#pragma once

#include "core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zircon::core {

struct ProcessInfo {
    std::uint32_t pid{};
    std::string   name;      // "StormEscape-Win64-Shipping.exe"
    std::string   path;      // full image path, empty when we lack access to read it
    bool          is_64bit{true};
};

// Enumerates running processes. Deliberately does not open them: a plain snapshot needs
// no special rights, while OpenProcess on every process on the machine is slow, noisy,
// and fails on anything elevated.
//
// `path` is therefore best-effort. Callers must treat an empty path as "unknown", not
// as "not a game".
std::vector<ProcessInfo> EnumerateProcesses();

} // namespace zircon::core
