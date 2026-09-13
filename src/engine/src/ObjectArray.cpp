#include "engine/ObjectArray.h"
#include "engine/Hooks.h"
#include "core/Log.h"

#include <algorithm>
#include <format>
#include <map>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// FChunkedFixedUObjectArray:
//   +0x00  FUObjectItem** Objects
//   +0x08  FUObjectItem*  PreAllocatedObjects
//   +0x10  int32          MaxElements
//   +0x14  int32          NumElements
//   +0x18  int32          MaxChunks
//   +0x1C  int32          NumChunks
//
// The enclosing FUObjectArray puts four ints and a bool before it, so GObjects as
// usually quoted sits 0x10 bytes earlier.
constexpr std::uint64_t kInnerOffsetInFUObjectArray = 0x10;

constexpr std::int32_t kMinObjects = 1000;        // even a trivial UE game exceeds this
constexpr std::int32_t kMaxObjects = 80'000'000;
constexpr std::int32_t kMaxChunksSane = 16384;

// Item sizes seen across 4.20 - 5.x, likeliest first. Candidates, not a guess: the index
// check below is what says which one is right.
constexpr std::uint32_t kItemSizes[] = {24, 32, 16, 40};

// Slots to sample when proving the array is real. Enough that a coincidence gets
// implausible, few enough to stay fast.
constexpr int kSampleTarget = 24;

// InternalIndex sits just past the vtable and flags in every layout we care about, so
// don't bother searching the whole object.
constexpr int kMaxIndexOffset = 0x40;

bool Readable(core::IMemorySource& memory, Address addr, std::size_t size = 8) {
    if (IsNull(addr)) return false;
    if (Raw(addr) < 0x10000) return false;                 // null page
    if (Raw(addr) >= 0x7FFFFFFFFFFFull) return false;       // non-canonical user space

    std::uint8_t probe[64];
    const std::size_t want = std::min(size, sizeof(probe));
    return memory.Read(addr, probe, want) == want;
}

// A UObject starts with a vtable pointer, which must reach readable memory that itself
// holds readable code pointers.
bool LooksLikeObject(core::IMemorySource& memory, Address object) {
    if (!Readable(memory, object, 32)) return false;

    const auto vtable = core::ReadOr<Address>(memory, object);
    if (!Readable(memory, vtable, 8)) return false;

    const auto first_virtual = core::ReadOr<Address>(memory, vtable);
    return Readable(memory, first_virtual, 8);
}

struct Candidate {
    Address       inner{};
    std::int32_t  max_elements{};
    std::int32_t  num_elements{};
    std::int32_t  max_chunks{};
    std::int32_t  num_chunks{};
    Address       chunk_table{};
    int           counters_at{};   // where the four int32s were found
};

// Cheap structural filter over every aligned slot in writable memory. Throws out the
// overwhelming majority before any pointer chasing starts.
std::optional<Candidate> ReadCandidate(core::IMemorySource& memory, Address at) {
    Candidate candidate;
    candidate.inner = at;

    if (!core::ReadInto(memory, at, candidate.chunk_table)) return std::nullopt;
    if (!Readable(memory, candidate.chunk_table, 8)) return std::nullopt;

    // The four counters sit together as MaxElements, NumElements, MaxChunks, NumChunks,
    // but *where* isn't fixed. +0x10 held through 4.27 and every UE5 up to 5.6. Then 5.7
    // moved them, and assuming the old place meant the array couldn't be found at all
    // there: the search just reported nothing, with no clue that one offset was the whole
    // problem.
    //
    // Hence searching the window. What identifies them is their order and relationship,
    // not their address — a count inside its own maximum, a chunk count inside its own
    // maximum, both in ranges a real array can have. The slot test below is what actually
    // proves the answer, so casting a slightly wider net here is free.
    constexpr int kMaxCountersAt = 0x40;

    for (int offset = 0x08; offset <= kMaxCountersAt; offset += 4) {
        std::int32_t max_elements{}, num_elements{}, max_chunks{}, num_chunks{};

        if (!core::ReadInto(memory, at + offset,        max_elements) ||
            !core::ReadInto(memory, at + offset + 0x4,  num_elements) ||
            !core::ReadInto(memory, at + offset + 0x8,  max_chunks)  ||
            !core::ReadInto(memory, at + offset + 0xC,  num_chunks))
            continue;

        if (num_elements < kMinObjects || num_elements > kMaxObjects) continue;
        if (max_elements < num_elements || max_elements > kMaxObjects) continue;
        if (num_chunks <= 0 || max_chunks <= 0 || num_chunks > max_chunks ||
            max_chunks > kMaxChunksSane)
            continue;

        // Counters must agree with each other. A chunk holds a fixed number of elements,
        // so the chunks needed for NumElements can't exceed NumChunks. Four unrelated
        // integers rarely manage that on top of everything above.
        const std::int64_t per_chunk = max_chunks > 0 ? max_elements / max_chunks : 0;
        if (per_chunk <= 0) continue;
        if ((num_elements + per_chunk - 1) / per_chunk > num_chunks) continue;

        candidate.max_elements = max_elements;
        candidate.num_elements = num_elements;
        candidate.max_chunks   = max_chunks;
        candidate.num_chunks   = num_chunks;
        candidate.counters_at  = offset;
        return candidate;
    }

    return std::nullopt;
}

// The decisive test. In a real object array the object in slot i stores i in its own
// InternalIndex field, and something that merely resembles an object array won't manage
// that for two dozen slots at one consistent offset.
//
// The test derives the offset as a side effect, which is why derivation and validation
// are one pass here instead of two.
int DeriveIndexOffset(core::IMemorySource& memory, const Candidate& candidate,
                      std::uint32_t item_size, std::uint32_t elements_per_chunk,
                      int item_object_offset, int& matched_samples) {
    matched_samples = 0;

    // Offsets still consistent with every sample so far.
    std::vector<int> surviving;
    for (int offset = 4; offset <= kMaxIndexOffset; offset += 4) surviving.push_back(offset);

    // Spread out; the early slots are bootstrap objects.
    const std::int32_t stride = std::max(1, candidate.num_elements / (kSampleTarget * 4));

    int sampled = 0;
    for (std::int32_t index = 0;
         index < candidate.num_elements && sampled < kSampleTarget && !surviving.empty();
         index += stride) {

        const std::uint32_t chunk  = static_cast<std::uint32_t>(index) / elements_per_chunk;
        const std::uint32_t within = static_cast<std::uint32_t>(index) % elements_per_chunk;
        if (static_cast<std::int32_t>(chunk) >= candidate.num_chunks) break;

        const auto chunk_ptr = core::ReadOr<Address>(memory, candidate.chunk_table + chunk * 8);
        if (!Readable(memory, chunk_ptr, 8)) continue;

        const auto object = core::ReadOr<Address>(
            memory, chunk_ptr + within * item_size + item_object_offset);
        if (IsNull(object)) continue;                  // a freed slot is normal
        if (!LooksLikeObject(memory, object)) continue;

        std::uint8_t header[kMaxIndexOffset + 4]{};
        if (memory.Read(object, header, sizeof(header)) != sizeof(header)) continue;

        std::vector<int> still_good;
        for (const int offset : surviving) {
            std::int32_t stored{};
            std::memcpy(&stored, header + offset, sizeof(stored));
            if (stored == index) still_good.push_back(offset);
        }

        // Nothing explained this slot, so this isn't an object array.
        if (still_good.empty()) return -1;

        surviving = std::move(still_good);
        ++sampled;
    }

    matched_samples = sampled;
    if (sampled < 8 || surviving.empty()) return -1;

    // Lowest surviving offset wins. InternalIndex sits early in UObject; a higher
    // coincidental match is padding or some later field.
    return surviving.front();
}

} // namespace

std::optional<ObjectArrayInfo> FindObjectArray(core::IMemorySource& memory,
                                               const EngineProfile& profile) {
    if (!memory.Caps().live_objects) {
        core::LogWarn("object array lookup needs live memory; a PE on disk has an empty "
                      ".data section and no objects to find");
        return std::nullopt;
    }

    const auto* main_module = memory.MainModule();
    if (!main_module) {
        core::LogWarn("no main module; cannot scope the object array scan");
        return std::nullopt;
    }

    const std::uint64_t module_lo = Raw(main_module->base);
    const std::uint64_t module_hi = module_lo + main_module->size;

    // GUObjectArray is a global in the executable's writable data, so the main module's
    // writable regions are enough. No need to walk the heap.
    std::vector<core::RegionInfo> targets;
    for (const auto& region : memory.Regions()) {
        const std::uint64_t lo = Raw(region.base);
        if (lo < module_lo || lo >= module_hi) continue;
        if (!core::HasFlag(region.protect, core::RegionProtect::Write)) continue;
        if (!core::HasFlag(region.protect, core::RegionProtect::Read)) continue;
        targets.push_back(region);
    }

    core::LogDebug("scanning {} writable region(s) in {} for the object array",
                   targets.size(), main_module->name);

    std::vector<ObjectArrayInfo> found;

    // Pulled out of the loop so a resolver's candidate gets judged by the identical rule.
    // A hook's job is to say *where* to look, never to be believed about what's there.
    auto evaluate = [&](Address at) -> std::optional<ObjectArrayInfo> {
        {
            auto candidate = ReadCandidate(memory, at);
            if (!candidate) return std::nullopt;

            // MaxElements / MaxChunks gives the real chunk stride. 65536 is the engine
            // default but hasn't always been, so derive first and fall back second.
            std::uint32_t elements_per_chunk =
                candidate->max_chunks > 0
                    ? static_cast<std::uint32_t>(candidate->max_elements / candidate->max_chunks)
                    : 0;
            if (elements_per_chunk == 0) elements_per_chunk = 64 * 1024;

            for (const std::uint32_t item_size : kItemSizes) {
                // 0 first: every engine through 5.6 uses it, so a build where both would
                // pass should be read the ordinary way.
                int samples = 0;
                int index_offset = -1;
                int object_offset = 0;

                for (const int candidate_offset : {0, 8, 16}) {
                    if (static_cast<std::uint32_t>(candidate_offset) + 8 > item_size) continue;

                    samples = 0;
                    index_offset = DeriveIndexOffset(memory, *candidate, item_size,
                                                     elements_per_chunk, candidate_offset,
                                                     samples);
                    if (index_offset >= 0) { object_offset = candidate_offset; break; }
                }
                if (index_offset < 0) continue;

                ObjectArrayInfo info;
                info.inner              = candidate->inner;
                info.gobjects           = candidate->inner - kInnerOffsetInFUObjectArray;
                info.chunked            = true;
                info.num_elements       = candidate->num_elements;
                info.max_elements       = candidate->max_elements;
                info.num_chunks         = candidate->num_chunks;
                info.max_chunks         = candidate->max_chunks;
                info.elements_per_chunk = elements_per_chunk;
                info.item_size          = item_size;
                info.item_object_offset = object_offset;
                info.index_offset       = index_offset;

                // Scales with how many slots agreed. Two dozen independent index matches
                // at one offset is not something a false positive does.
                info.confidence = std::min(0.99f, 0.6f + 0.015f * static_cast<float>(samples));

                info.evidence.push_back(std::format(
                    "{} objects across {} chunks of {}", candidate->num_elements,
                    candidate->num_chunks, elements_per_chunk));
                info.evidence.push_back(std::format(
                    "{} sampled slots stored their own index at +{:#x}", samples, index_offset));
                info.evidence.push_back(std::format(
                    "FUObjectItem size {} bytes, UObject* at +{:#x} within it",
                    item_size, object_offset));
                info.evidence.push_back(std::format(
                    "element and chunk counters at +{:#x} within the array", candidate->counters_at));

                return info;
            }
        }
        return std::nullopt;
    };

    if (GlobalCandidates hint; ResolveGlobals(memory, hint) && !IsNull(hint.gobjects)) {
        // A resolver names GObjects, the FUObjectArray itself, but the scan works in terms
        // of the inner FChunkedFixedUObjectArray. Undo the shift the scan applies.
        if (auto info = evaluate(hint.gobjects + kInnerOffsetInFUObjectArray)) {
            info->evidence.push_back("address supplied by a resolver hook, then validated");
            core::LogInfo("object array at {:#x} (from a resolver hook): {} objects",
                          Raw(info->inner), info->num_elements);
            return info;
        }
        core::LogWarn("the resolver's GObjects address did not validate; scanning instead");
    }

    for (const auto& region : targets) {
        for (std::uint64_t offset = 0; offset + 0x20 <= region.size; offset += 8) {
            if (auto info = evaluate(region.base + offset)) found.push_back(std::move(*info));
        }
    }

    if (found.empty()) {
        core::LogWarn("no object array found in {} writable region(s)", targets.size());
        return std::nullopt;
    }

    // The engine has exactly one GUObjectArray, so ambiguity here means something is
    // wrong. Say so rather than quietly taking the first.
    if (found.size() > 1) {
        core::LogWarn("{} object-array candidates; taking the one with the most objects",
                      found.size());
        std::sort(found.begin(), found.end(),
                  [](const ObjectArrayInfo& a, const ObjectArrayInfo& b) {
                      return a.num_elements > b.num_elements;
                  });
        found.front().evidence.push_back(
            std::format("{} candidates found; selected the largest", found.size()));
        found.front().confidence *= 0.9f;
    }

    const auto& best = found.front();
    core::LogInfo("object array at {:#x} (GObjects {:#x}): {} objects, index offset +{:#x}",
                  Raw(best.inner), Raw(best.gobjects), best.num_elements, best.index_offset);

    (void)profile;   // stage 2 gets by without the version hint
    return found.front();
}

Address ObjectAt(core::IMemorySource& memory, const ObjectArrayInfo& info,
                 std::int32_t index) {
    if (index < 0 || index >= info.num_elements) return Address{};

    const std::uint32_t chunk  = static_cast<std::uint32_t>(index) / info.elements_per_chunk;
    const std::uint32_t within = static_cast<std::uint32_t>(index) % info.elements_per_chunk;
    if (static_cast<std::int32_t>(chunk) >= info.num_chunks) return Address{};

    const auto chunk_table = core::ReadOr<Address>(memory, info.inner);
    if (IsNull(chunk_table)) return Address{};

    const auto chunk_ptr = core::ReadOr<Address>(memory, chunk_table + chunk * 8);
    if (IsNull(chunk_ptr)) return Address{};

    return core::ReadOr<Address>(
        memory, chunk_ptr + within * info.item_size + info.item_object_offset);
}

} // namespace zircon::engine
