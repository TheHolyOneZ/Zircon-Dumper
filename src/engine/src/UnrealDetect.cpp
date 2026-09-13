#include "engine/UnrealDetect.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <filesystem>

namespace zircon::engine {
namespace {

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
    if (suffix.size() > text.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

bool ContainsNoCase(std::string_view text, std::string_view needle) {
    if (needle.empty() || needle.size() > text.size()) return false;
    const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    return std::tolower(static_cast<unsigned char>(a)) ==
                                           std::tolower(static_cast<unsigned char>(b));
                                });
    return it != text.end();
}

// UE names its shipping executable <Project>-<Platform>-<Config>.exe. Shipping is the
// common case; Development and Test builds are out there too and dump just as well.
constexpr std::array<std::string_view, 6> kExeSuffixes = {
    "-Win64-Shipping.exe",
    "-Win64-Development.exe",
    "-Win64-Test.exe",
    "-WinGDK-Shipping.exe",
    "-Win32-Shipping.exe",
    "-Win64-DebugGame.exe",
};

// Up from Binaries/Win64 to the project root, checking for the directory shape every
// cooked UE build has. Separates a real game from something merely named like one.
bool HasUnrealLayout(const std::filesystem::path& exe, std::vector<std::string>& evidence) {
    std::error_code ec;
    bool any = false;

    // <Root>/<Project>/Binaries/Win64/Game.exe  ->  up 3 is <Root>
    auto dir = exe.parent_path();
    for (int levels = 0; levels < 5 && !dir.empty(); ++levels) {
        if (std::filesystem::is_directory(dir / "Engine" / "Binaries", ec)) {
            evidence.push_back("Engine/Binaries present");
            any = true;
            break;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }

    auto content = exe.parent_path();
    for (int levels = 0; levels < 4 && !content.empty(); ++levels) {
        if (std::filesystem::is_directory(content / "Content" / "Paks", ec)) {
            evidence.push_back("Content/Paks present");
            any = true;
            break;
        }
        const auto parent = content.parent_path();
        if (parent == content) break;
        content = parent;
    }

    return any;
}

} // namespace

UnrealCandidate ScoreProcess(const core::ProcessInfo& process) {
    UnrealCandidate candidate;
    candidate.process = process;

    float score = 0.0f;

    for (const auto& suffix : kExeSuffixes) {
        if (!EndsWithNoCase(process.name, suffix)) continue;

        candidate.project = process.name.substr(0, process.name.size() - suffix.size());
        candidate.evidence.push_back(std::string("executable name matches '*")
                                     + std::string(suffix) + "'");
        score += 0.7f;
        break;
    }

    if (!process.path.empty()) {
        if (ContainsNoCase(process.path, "\\Binaries\\Win64\\") ||
            ContainsNoCase(process.path, "/Binaries/Win64/")) {
            candidate.evidence.push_back("lives in Binaries/Win64");
            score += 0.2f;
        }

        std::error_code ec;
        const std::filesystem::path exe(process.path);
        if (std::filesystem::exists(exe, ec) && HasUnrealLayout(exe, candidate.evidence))
            score += 0.25f;
    } else {
        // Usually elevated or protected, not uninteresting. Say so instead of quietly
        // scoring it down.
        candidate.evidence.push_back("image path unavailable (try running elevated)");
    }

    // Renamed executables are common in shipped games. FF7 Rebirth ships as
    // ff7rebirth_.exe with no UE naming at all, so directory layout alone has to be able
    // to carry a process over the line.
    if (candidate.project.empty() && score > 0.0f) {
        const auto dot = process.name.find_last_of('.');
        candidate.project = dot == std::string::npos ? process.name : process.name.substr(0, dot);
        candidate.evidence.push_back("non-standard executable name; project inferred");
    }

    candidate.confidence = std::min(score, 1.0f);
    return candidate;
}

std::vector<UnrealCandidate> DetectUnrealProcesses(float minimum_confidence) {
    std::vector<UnrealCandidate> found;

    for (const auto& process : core::EnumerateProcesses()) {
        auto candidate = ScoreProcess(process);
        if (candidate.confidence >= minimum_confidence)
            found.push_back(std::move(candidate));
    }

    std::sort(found.begin(), found.end(),
              [](const UnrealCandidate& a, const UnrealCandidate& b) {
                  if (a.confidence != b.confidence) return a.confidence > b.confidence;
                  return a.process.name < b.process.name;
              });

    core::LogDebug("scanned processes, {} look like Unreal games", found.size());
    return found;
}

} // namespace zircon::engine
