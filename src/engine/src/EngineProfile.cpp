#include "engine/EngineProfile.h"
#include "core/Log.h"
#include "core/PatternScanner.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>

namespace zircon::engine {
namespace {

using core::Address;
using core::Pattern;
using core::PatternScanner;
using core::ScanOptions;

// Enough context to hold the longest realistic marker plus its version suffix, e.g.
// "5.3.2-28154416+++UE5+Release-5.3". Doubled for UTF-16.
constexpr std::size_t kContextBytes = 192;

// Optionally UTF-16LE, by interleaving NUL bytes. Reuses the ordinary byte scanner instead
// of adding a separate text search.
std::optional<Pattern> LiteralPattern(std::string_view text, bool utf16) {
    std::string bytes;
    std::string mask;
    for (char c : text) {
        bytes.push_back(c);
        mask.push_back('x');
        if (utf16) {
            bytes.push_back('\0');
            mask.push_back('x');
        }
    }

    auto pattern = Pattern::FromBytesAndMask(bytes.data(), mask);
    if (!pattern) return std::nullopt;
    return pattern.value();
}

// De-interleaves UTF-16 at a given alignment. Unprintable bytes become newlines, so they
// act as token separators for the parsers below.
std::string Flatten(const std::vector<std::uint8_t>& raw, std::size_t got,
                    bool utf16, std::size_t align, std::size_t& printable) {
    std::string text;
    printable = 0;
    text.reserve(got);

    for (std::size_t i = align; i < got; i += (utf16 ? 2u : 1u)) {
        unsigned char c = raw[i];
        const bool wide_ok = !utf16 || (i + 1 < got && raw[i + 1] == 0);
        if (wide_ok && c >= 32 && c < 127) {
            text.push_back(static_cast<char>(c));
            ++printable;
        } else {
            text.push_back('\n');
        }
    }
    return text;
}

// Reaches `back` characters before the hit, so a marker preceding the anchor stays visible.
//
// UTF-16 alignment is easy to get wrong here. Backing up by a byte count can land on the
// odd byte of a wide character, and from there every de-interleaved character is garbage.
// So flatten at both alignments and keep whichever yields more printable text.
std::string ReadContext(core::IMemorySource& memory, Address at, std::size_t back,
                        bool utf16) {
    const std::size_t stride     = utf16 ? 2u : 1u;
    const std::size_t back_bytes = back * stride;
    const std::size_t span       = (back + kContextBytes) * stride;

    const std::uint64_t start = core::Raw(at) > back_bytes ? core::Raw(at) - back_bytes
                                                           : core::Raw(at);

    std::vector<std::uint8_t> raw(span);
    const std::size_t got = memory.Read(static_cast<Address>(start), raw.data(), raw.size());
    if (got == 0) return {};

    std::size_t printable_even = 0;
    std::string even = Flatten(raw, got, utf16, 0, printable_even);
    if (!utf16) return even;

    std::size_t printable_odd = 0;
    std::string odd = Flatten(raw, got, utf16, 1, printable_odd);

    return printable_odd > printable_even ? odd : even;
}

bool ParseUInt(std::string_view text, std::size_t& index, int& out) {
    const std::size_t start = index;
    int value = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
        value = value * 10 + (text[index] - '0');
        if (value > 999) return false;
        ++index;
    }
    if (index == start) return false;
    out = value;
    return true;
}

// Recognises the two shapes UE actually ships:
//   ++UE5+Release-5.6-CL-44394996     stock engine
//   ++Project+SN2-Release-CL-123362   licensee-branded, carries no version at all
void ParseUeMarker(std::string_view text, std::vector<VersionHint>& out) {
    std::size_t i = 0;
    while ((i = text.find("++", i)) != std::string_view::npos) {
        std::size_t cursor = i + 2;

        // Stock form: "UE" <digit> "+Release-" <major> "." <minor>
        if (text.compare(cursor, 2, "UE") == 0) {
            std::size_t probe = cursor + 2;
            int generation = 0;
            if (ParseUInt(text, probe, generation) &&
                text.compare(probe, 9, "+Release-") == 0) {
                probe += 9;
                int major = 0, minor = 0;
                if (ParseUInt(text, probe, major) && probe < text.size() &&
                    text[probe] == '.' && (++probe, ParseUInt(text, probe, minor))) {
                    VersionHint hint;
                    hint.major = major;
                    hint.minor = minor;
                    hint.raw   = std::string(text.substr(i, probe - i));
                    out.push_back(std::move(hint));
                    i = probe;
                    continue;
                }
            }
        }

        // Branded form: anything else between "++" and "-Release" / "+rel". Recording that
        // it's a licensee build is useful in itself, since it tells the user why no version
        // turned up instead of leaving them guessing.
        const std::size_t line_end = text.find('\n', i);
        const std::string_view span = text.substr(i, (line_end == std::string_view::npos
                                                      ? text.size() : line_end) - i);
        if (span.size() > 4 && span.size() < 96 &&
            (span.find("-Release") != std::string_view::npos ||
             span.find("+rel")     != std::string_view::npos ||
             span.find("-CL-")     != std::string_view::npos)) {
            VersionHint hint;
            hint.raw      = std::string(span);
            hint.licensee = true;
            out.push_back(std::move(hint));
        }
        i += 2;
    }
}

void ParseUnrealEngineMarker(std::string_view text, std::vector<VersionHint>& out) {
    std::size_t i = 0;
    const std::string_view needle = "UnrealEngine-";
    while ((i = text.find(needle, i)) != std::string_view::npos) {
        std::size_t cursor = i + needle.size();
        int major = 0, minor = 0;
        if (ParseUInt(text, cursor, major) && cursor < text.size() && text[cursor] == '.' &&
            (++cursor, ParseUInt(text, cursor, minor))) {
            VersionHint hint;
            hint.major = major;
            hint.minor = minor;
            hint.raw   = std::string(text.substr(i, cursor - i));
            out.push_back(std::move(hint));
        }
        i = cursor > i ? cursor : i + needle.size();
    }
}

} // namespace

std::string EngineProfile::VersionString() const {
    if (!Known()) return "unknown";
    return std::format("{}.{}", major, minor);
}

void EngineProfile::ApplyVersionDefaults() {
    if (!Known()) return;

    const int combined = major * 100 + minor;

    // FProperty replaced UProperty in 4.25, the biggest structural fork in the whole walk.
    uses_fproperty = combined >= 425;

    // FNamePool replaced TNameEntryArray in 4.23.
    chunked_name_pool = combined >= 423;

    // Shipping builds have used the chunked object array since 4.20. The fixed array
    // survives mostly in older or heavily customised builds, so this stays a default that
    // direct observation is expected to overrule.
    chunked_gobjects = combined >= 420;

    evidence.push_back(std::format(
        "layout defaults from {}.{}: {}, {}, {}", major, minor,
        uses_fproperty ? "FProperty" : "UProperty",
        chunked_name_pool ? "FNamePool" : "TNameEntryArray",
        chunked_gobjects ? "chunked GObjects" : "fixed GObjects"));
}

std::vector<VersionHint> ScanVersionStrings(core::IMemorySource& memory) {
    std::vector<VersionHint> hints;

    PatternScanner scanner(memory);

    // Version strings live in read-only data. The executable-only default misses the lot.
    ScanOptions options;
    options.executable_only = false;
    options.max_results     = 256;

    // Anchors, not full markers. "++" alone is far too common to scan for, and anchoring
    // only on "++UE" misses every licensee build — Subnautica 2 ships
    // "++Project+SN2-Release" and RV There Yet "++RideGamejam+rel-1.2", neither of which
    // contains "UE". Anchoring on the release token instead catches both families, and
    // the context parser does the discrimination.
    //
    // Both encodings are always scanned: these strings are frequently UTF-16 only. Every
    // branded marker in our local test corpus is UTF-16 with no ASCII copy at all.
    constexpr std::string_view kAnchors[] = {
        "+Release-",      // ++UE5+Release-5.6
        "-Release",       // ++Project+SN2-Release
        "+rel-",          // ++RideGamejam+rel-1.2
        "UnrealEngine-",  // UnrealEngine-5.7
    };

    // How far back from the anchor the marker can start. "++RideGamejam" is 13
    // characters; 96 leaves generous room without dragging in unrelated neighbours.
    constexpr std::size_t kLookBehind = 96;

    for (const auto anchor : kAnchors) {
        for (const bool utf16 : {false, true}) {
            auto pattern = LiteralPattern(anchor, utf16);
            if (!pattern) continue;

            for (const auto hit : scanner.Scan(*pattern, options)) {
                const std::string context = ReadContext(memory, hit, kLookBehind, utf16);
                if (context.empty()) continue;

                // Run both parsers on every context: one anchor can sit inside either
                // marker shape, and a miss is free.
                ParseUeMarker(context, hints);
                ParseUnrealEngineMarker(context, hints);
            }
        }
    }

    // Deduplicate on the raw text; a marker typically appears many times in one image.
    std::sort(hints.begin(), hints.end(), [](const VersionHint& a, const VersionHint& b) {
        return a.raw < b.raw;
    });
    hints.erase(std::unique(hints.begin(), hints.end(),
                            [](const VersionHint& a, const VersionHint& b) {
                                return a.raw == b.raw;
                            }),
                hints.end());

    return hints;
}

EngineProfile FingerprintEngine(core::IMemorySource& memory) {
    EngineProfile profile;

    const auto hints = ScanVersionStrings(memory);

    // Prefer a hint that actually carries a version number. Branded markers are recorded
    // as evidence but never treated as a version.
    std::map<std::pair<int, int>, int> votes;
    for (const auto& hint : hints) {
        if (hint.major != 0) ++votes[{hint.major, hint.minor}];
    }

    if (!votes.empty()) {
        const auto best = std::max_element(votes.begin(), votes.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        profile.major = best->first.first;
        profile.minor = best->first.second;

        // A version string is corroboration, never proof: a licensee can ship any string
        // they like, and a fork can leave a stale one behind. Cap what stage 1 can claim.
        profile.confidence = votes.size() == 1 ? 0.6f : 0.4f;

        for (const auto& hint : hints) {
            if (hint.major != 0)
                profile.evidence.push_back(std::format("version string: {}", hint.raw));
        }

        if (votes.size() > 1) {
            profile.evidence.push_back(std::format(
                "{} conflicting version strings; confidence reduced", votes.size()));
        }

        profile.ApplyVersionDefaults();
    }

    for (const auto& hint : hints) {
        if (hint.licensee) {
            profile.evidence.push_back(
                std::format("licensee-branded build string: {}", hint.raw));
        }
    }

    if (!profile.Known()) {
        profile.confidence = 0.0f;
        profile.evidence.push_back(
            "no usable engine version string; layout must be derived from memory (P1 stage 2)");
    }

    if (!memory.Caps().live_objects) {
        profile.evidence.push_back(
            "static image: stage 2 layout derivation unavailable, hint only");
        profile.confidence = std::min(profile.confidence, 0.5f);
    }

    core::LogDebug("fingerprint: {} (confidence {:.2f}) from {} hint(s)",
                   profile.VersionString(), profile.confidence, hints.size());
    return profile;
}

} // namespace zircon::engine
