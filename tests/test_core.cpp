// Dependency-free test runner. CI must never need a game installed, so everything here
// runs against a synthetic in-memory source or a file the test itself writes.

#include "core/Log.h"
#include "core/MemorySource.h"
#include "core/PatternScanner.h"
#include "core/PeImage.h"
#include "core/ProcessList.h"
#include "engine/EngineProfile.h"
#include "engine/ClassLayout.h"
#include "engine/EnumLayout.h"
#include "engine/Hooks.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/StructLayout.h"
#include "engine/UnrealDetect.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace zircon::core;

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

// A memory source backed by a byte vector at a chosen base. Lets us test the read
// helpers, the scanner and the cache without any OS or target involvement.
class FakeMemory final : public IMemorySource {
public:
    FakeMemory(Address base, std::vector<std::uint8_t> bytes, bool writable = false)
        : base_(base), bytes_(std::move(bytes)) {
        modules_.push_back(ModuleInfo{"fake.exe", "", base_, bytes_.size()});

        // Writable is opt-in because several tests rely on a scan finding *nothing* here:
        // a region the scanner will not consider is what makes "the hook supplied this"
        // unambiguous.
        auto protect = RegionProtect::Read | RegionProtect::Execute;
        if (writable) protect = protect | RegionProtect::Write;
        regions_.push_back(RegionInfo{base_, bytes_.size(), protect, true});
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        ++read_calls;
        if (Raw(addr) < Raw(base_)) return 0;

        const std::uint64_t offset = Raw(addr) - Raw(base_);
        if (offset >= bytes_.size()) return 0;

        const std::size_t available = std::min<std::size_t>(size, bytes_.size() - offset);
        std::memcpy(out, bytes_.data() + offset, available);
        return available;
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }
    Capabilities Caps() const override { return Capabilities{true, false, false, true}; }
    std::string Describe() const override { return "fake"; }

    int read_calls = 0;

private:
    Address                   base_;
    std::vector<std::uint8_t> bytes_;
    std::vector<ModuleInfo>   modules_;
    std::vector<RegionInfo>   regions_;
};

void TestAddress() {
    const auto base = static_cast<Address>(0x140000000ull);
    CHECK(Raw(base + 0x1000) == 0x140001000ull);
    CHECK(Raw(base - 0x1000) == 0x13FFFF000ull);
    CHECK(IsNull(static_cast<Address>(0)));
    CHECK(!IsNull(base));
}

void TestPatternParsing() {
    const auto simple = Pattern::Parse("48 8B 05 ? ? ? ?");
    CHECK(simple.ok());
    CHECK(simple.value().Size() == 7);
    CHECK(simple.value().FirstFixedIndex() == 0);
    CHECK(simple.value().ToString() == "48 8B 05 ?? ?? ?? ??");

    // "??" and "?" must be interchangeable, and spacing must not matter.
    const auto double_wild = Pattern::Parse("48??8B");
    CHECK(double_wild.ok());
    CHECK(double_wild.value().Size() == 3);

    // Leading wildcards mean the cheap first-byte skip has to start later.
    const auto leading = Pattern::Parse("? ? 8B 05");
    CHECK(leading.ok());
    CHECK(leading.value().FirstFixedIndex() == 2);

    CHECK(!Pattern::Parse("").ok());
    CHECK(!Pattern::Parse("? ? ?").ok());        // all wildcards is a bug, not a query
    CHECK(!Pattern::Parse("48 8B ZZ").ok());
    CHECK(!Pattern::Parse("48 8").ok());          // truncated final byte

    // The mask governs the length, so embedded NUL bytes survive. A string_view-based
    // API would truncate this literal to two bytes at the first \x00.
    const auto masked = Pattern::FromBytesAndMask("\x48\x8B\x00\x00", "xx??");
    CHECK(masked.ok());
    CHECK(masked.value().Size() == 4);
    CHECK(masked.value().ToString() == "48 8B ?? ??");

    // A fixed NUL byte must survive as a real constraint, not be dropped.
    const auto with_nul = Pattern::FromBytesAndMask("\x48\x00\x8B", "xxx");
    CHECK(with_nul.ok());
    CHECK(with_nul.value().Size() == 3);
    CHECK(with_nul.value().ToString() == "48 00 8B");

    CHECK(!Pattern::FromBytesAndMask("\x48", "").ok());
    CHECK(!Pattern::FromBytesAndMask(nullptr, "x").ok());
    CHECK(!Pattern::FromBytesAndMask("\x48\x8B", "??").ok());   // all wildcards
}

void TestPatternMatching() {
    const auto pattern = Pattern::Parse("48 8B ? 05").value();
    const std::uint8_t yes[] = {0x48, 0x8B, 0xFF, 0x05};
    const std::uint8_t also[] = {0x48, 0x8B, 0x00, 0x05};
    const std::uint8_t no[]  = {0x48, 0x8B, 0xFF, 0x06};
    CHECK(pattern.MatchesAt(yes));
    CHECK(pattern.MatchesAt(also));   // the wildcard byte genuinely does not matter
    CHECK(!pattern.MatchesAt(no));
}

void TestScanner() {
    // Three hits, one of them overlapping the chunk logic's first-byte skip path.
    std::vector<std::uint8_t> bytes(4096, 0x90);
    const std::uint8_t needle[] = {0x48, 0x8B, 0x05, 0xDE, 0xAD, 0xBE, 0xEF};
    std::memcpy(bytes.data() + 100,  needle, sizeof(needle));
    std::memcpy(bytes.data() + 2000, needle, sizeof(needle));
    std::memcpy(bytes.data() + 3000, needle, sizeof(needle));

    const auto base = static_cast<Address>(0x140000000ull);
    FakeMemory memory(base, bytes);
    PatternScanner scanner(memory);

    const auto pattern = Pattern::Parse("48 8B 05 ? ? ? ?").value();
    const auto hits = scanner.Scan(pattern);
    CHECK(hits.size() == 3);
    if (hits.size() == 3) {
        CHECK(Raw(hits[0]) == Raw(base) + 100);
        CHECK(Raw(hits[1]) == Raw(base) + 2000);
        CHECK(Raw(hits[2]) == Raw(base) + 3000);
    }

    ScanOptions capped;
    capped.max_results = 2;
    CHECK(scanner.Scan(pattern, capped).size() == 2);

    CHECK(scanner.ScanFirst(pattern).has_value());

    // Ambiguity must not quietly resolve to the first hit: a signature matching three
    // places will pick a different one on the next game build.
    CHECK(!scanner.ScanUnique(pattern).has_value());

    const auto unique = Pattern::Parse("48 8B 05 DE AD BE EF").value();
    ScanOptions only_first;
    only_first.max_results = 0;
    CHECK(scanner.Scan(unique, only_first).size() == 3);

    CHECK(scanner.ScanFirst(Pattern::Parse("11 22 33 44 55 66").value()) == std::nullopt);
}

void TestRipRelative() {
    // 48 8B 05 <disp32> at 0x1000, disp = 0x20 -> RIP(0x1007) + 0x20 = 0x1027
    std::vector<std::uint8_t> bytes(256, 0);
    const std::uint8_t insn[] = {0x48, 0x8B, 0x05, 0x20, 0x00, 0x00, 0x00};
    std::memcpy(bytes.data(), insn, sizeof(insn));

    const auto base = static_cast<Address>(0x1000);
    FakeMemory memory(base, bytes);

    const auto target = ResolveRipRelative(memory, base, 3, 7);
    CHECK(target.has_value());
    CHECK(target && Raw(*target) == 0x1027);

    // Negative displacement must resolve backwards, not wrap.
    std::vector<std::uint8_t> back(256, 0);
    const std::uint8_t insn_back[] = {0x48, 0x8B, 0x05, 0xF0, 0xFF, 0xFF, 0xFF};   // -0x10
    std::memcpy(back.data(), insn_back, sizeof(insn_back));
    FakeMemory memory_back(base, back);
    const auto behind = ResolveRipRelative(memory_back, base, 3, 7);
    CHECK(behind && Raw(*behind) == 0x1007 - 0x10);

    // A displacement that does not fit inside the instruction is a caller bug.
    CHECK(ResolveRipRelative(memory, base, 5, 7) == std::nullopt);
}

void TestReadHelpers() {
    std::vector<std::uint8_t> bytes(256, 0);
    const char text[] = "UObject";
    std::memcpy(bytes.data() + 16, text, sizeof(text));
    std::uint32_t value = 0xDEADBEEF;
    std::memcpy(bytes.data() + 64, &value, sizeof(value));

    const auto base = static_cast<Address>(0x2000);
    FakeMemory memory(base, bytes);

    CHECK(ReadOr<std::uint32_t>(memory, base + 64) == 0xDEADBEEF);
    CHECK(ReadOr<std::uint32_t>(memory, static_cast<Address>(0x1)) == 0);   // unmapped
    CHECK(ReadCString(memory, base + 16) == "UObject");

    // A string with no terminator must stop at the cap.
    std::vector<std::uint8_t> unterminated(300, 'A');
    FakeMemory runaway(base, unterminated);
    CHECK(ReadCString(runaway, base, 64).size() == 64);
}

void TestCache() {
    std::vector<std::uint8_t> bytes(64 * 1024, 0);
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(i & 0xFF);

    const auto base = static_cast<Address>(0x140000000ull);
    auto inner = std::make_unique<FakeMemory>(base, bytes);
    FakeMemory* raw = inner.get();

    auto cached = MakeCached(std::move(inner), 1 << 20);

    // Repeated reads of the same address must hit the cache, not the source.
    std::uint32_t first = 0, second = 0;
    CHECK(ReadInto(*cached, base + 8, first));
    const int after_first = raw->read_calls;
    CHECK(ReadInto(*cached, base + 8, second));
    CHECK(first == second);
    CHECK(raw->read_calls == after_first);

    // Values must still be correct, including reads that straddle a 4 KiB page.
    std::uint8_t straddle[8] = {};
    CHECK(ReadArrayInto(*cached, base + 4094, straddle, sizeof(straddle)));
    for (std::size_t i = 0; i < sizeof(straddle); ++i)
        CHECK(straddle[i] == static_cast<std::uint8_t>((4094 + i) & 0xFF));

    // Reading past the end must report a short read, not invent bytes.
    std::uint8_t tail[16] = {};
    const std::size_t got = cached->Read(base + bytes.size() - 4, tail, sizeof(tail));
    CHECK(got == 4);
}

void TestPeImage() {
    // Parse a real PE that every Windows machine has, then assert the invariants that
    // matter to us. Version-specific values would just rot.
    auto image = PeImage::FromFile("C:\\Windows\\System32\\kernel32.dll");
    CHECK(image.ok());
    if (!image.ok()) return;

    const auto& pe = image.value();
    CHECK(pe.Is64Bit());
    CHECK(pe.PreferredBase() != 0);
    CHECK(pe.SizeOfImage() > 0);
    CHECK(!pe.Sections().empty());
    CHECK(pe.SectionNamed(".text") != nullptr);
    CHECK(!pe.Exports().empty());

    // kernel32 must export these; if our export parsing drifts, this catches it.
    bool found_load_library = false;
    for (const auto& symbol : pe.Exports())
        if (symbol.name == "LoadLibraryW") found_load_library = true;
    CHECK(found_load_library);

    // Every section must live inside the declared image size.
    for (const auto& section : pe.Sections())
        CHECK(section.rva < pe.SizeOfImage());

    // Reading the DOS header back through the RVA path must yield "MZ".
    std::uint16_t magic = 0;
    CHECK(pe.ReadRva(0, &magic, sizeof(magic)) == sizeof(magic));
    CHECK(magic == 0x5A4D);

    CHECK(!PeImage::FromFile("C:\\Windows\\System32\\does_not_exist_zircon.dll").ok());
}

void TestStaticSourceRejectsLiveObjects() {
    auto source = OpenStaticImage("C:\\Windows\\System32\\kernel32.dll");
    CHECK(source.ok());
    if (!source.ok()) return;

    // The whole point of the capability flag: a static image can never enumerate
    // objects, and the reflection layer keys off this.
    CHECK(!source.value()->Caps().live_objects);
    CHECK(!source.value()->Caps().writable);
    CHECK(!source.value()->Caps().can_call);
    CHECK(source.value()->MainModule() != nullptr);
}

void TestUnrealDetection() {
    using zircon::engine::ScoreProcess;

    // Names taken from the real games listed in docs/UE-Test.md. Paths are synthetic so
    // the test passes on a machine with no games installed; the on-disk layout bonus
    // simply does not apply, which is exactly how an uninstalled target should score.
    auto make = [](const char* name, const char* path) {
        ProcessInfo info;
        info.pid  = 1234;
        info.name = name;
        info.path = path;
        return info;
    };

    const auto shipping = ScoreProcess(make(
        "StormEscape-Win64-Shipping.exe",
        "D:\\Games\\Funnel Runners\\StormEscape\\Binaries\\Win64\\StormEscape-Win64-Shipping.exe"));
    CHECK(shipping.confidence >= 0.85f);
    CHECK(shipping.project == "StormEscape");

    // Non-shipping configurations are just as dumpable and must not be missed.
    const auto development = ScoreProcess(make(
        "MyGame-Win64-Development.exe",
        "D:\\Games\\MyGame\\Binaries\\Win64\\MyGame-Win64-Development.exe"));
    CHECK(development.confidence >= 0.85f);
    CHECK(development.project == "MyGame");

    // FF7 Rebirth ships as ff7rebirth_.exe with no UE naming at all. Directory layout
    // alone has to carry it, or we would miss one of the most interesting targets.
    const auto renamed = ScoreProcess(make(
        "ff7rebirth_.exe",
        "D:\\Games\\FF7\\End\\Binaries\\Win64\\ff7rebirth_.exe"));
    CHECK(renamed.confidence > 0.0f);
    CHECK(renamed.project == "ff7rebirth_");

    // Ordinary Windows processes must score zero, or the GUI list becomes noise.
    CHECK(ScoreProcess(make("explorer.exe", "C:\\Windows\\explorer.exe")).confidence == 0.0f);
    CHECK(ScoreProcess(make("chrome.exe",
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe")).confidence == 0.0f);

    // A missing path means "unknown", never "not a game": elevated targets report none.
    const auto no_path = ScoreProcess(make("SomeGame-Win64-Shipping.exe", ""));
    CHECK(no_path.confidence >= 0.7f);
    CHECK(!no_path.evidence.empty());

    // Enumeration must work and must always find the process running this test.
    const auto processes = EnumerateProcesses();
    CHECK(!processes.empty());
    bool found_self = false;
    for (const auto& process : processes)
        if (process.pid == ::GetCurrentProcessId()) found_self = true;
    CHECK(found_self);
}

// Builds a byte buffer containing the given strings, each encoded as requested, so the
// version scanner can be exercised without any game binary.
void AppendText(std::vector<std::uint8_t>& buffer, std::string_view text, bool utf16) {
    for (char c : text) {
        buffer.push_back(static_cast<std::uint8_t>(c));
        if (utf16) buffer.push_back(0);
    }
    for (int i = 0; i < (utf16 ? 8 : 4); ++i) buffer.push_back(0);
}

void TestVersionFingerprint() {
    using zircon::engine::FingerprintEngine;
    using zircon::engine::ScanVersionStrings;

    const auto base = static_cast<Address>(0x140000000ull);

    // Stock ASCII marker.
    {
        std::vector<std::uint8_t> bytes(256, 0x90);
        AppendText(bytes, "++UE5+Release-5.6-CL-44394996", false);
        bytes.resize(bytes.size() + 256, 0x90);

        FakeMemory memory(base, bytes);
        const auto profile = FingerprintEngine(memory);
        CHECK(profile.Known());
        CHECK(profile.major == 5 && profile.minor == 6);
        CHECK(profile.uses_fproperty);        // 5.6 is well past the 4.25 fork
        CHECK(profile.chunked_name_pool);
    }

    // The same marker in UTF-16. Every branded string in our local corpus is UTF-16
    // only, so this encoding cannot be an afterthought.
    {
        std::vector<std::uint8_t> bytes(256, 0x90);
        AppendText(bytes, "++UE4+Release-4.27-CL-18319896", true);
        bytes.resize(bytes.size() + 256, 0x90);

        FakeMemory memory(base, bytes);
        const auto profile = FingerprintEngine(memory);
        CHECK(profile.Known());
        CHECK(profile.major == 4 && profile.minor == 27);
        CHECK(profile.uses_fproperty);        // 4.27 > 4.25
    }

    // A pre-4.25 build must select the UProperty path, which is a different walk
    // entirely.
    {
        std::vector<std::uint8_t> bytes(128, 0x90);
        AppendText(bytes, "++UE4+Release-4.22", false);
        bytes.resize(bytes.size() + 128, 0x90);

        FakeMemory memory(base, bytes);
        const auto profile = FingerprintEngine(memory);
        CHECK(profile.Known());
        CHECK(profile.major == 4 && profile.minor == 22);
        CHECK(!profile.uses_fproperty);
        CHECK(!profile.chunked_name_pool);    // TNameEntryArray before 4.23
    }

    // Licensee build: branded string, no version anywhere. Must report unknown rather
    // than guess, and must surface the brand so the user knows why.
    {
        std::vector<std::uint8_t> bytes(256, 0x90);
        AppendText(bytes, "++Project+SN2-Release", true);
        bytes.resize(bytes.size() + 256, 0x90);

        FakeMemory memory(base, bytes);
        const auto profile = FingerprintEngine(memory);
        CHECK(!profile.Known());
        CHECK(profile.confidence == 0.0f);

        bool named_brand = false;
        for (const auto& line : profile.evidence)
            if (line.find("++Project+SN2-Release") != std::string::npos) named_brand = true;
        CHECK(named_brand);
    }

    // "+rel-" is the other branded shape seen in the wild (RV There Yet).
    {
        std::vector<std::uint8_t> bytes(256, 0x90);
        AppendText(bytes, "++RideGamejam+rel-1.2-CL-17120", true);
        bytes.resize(bytes.size() + 256, 0x90);

        FakeMemory memory(base, bytes);
        CHECK(!ScanVersionStrings(memory).empty());
        CHECK(!FingerprintEngine(memory).Known());
    }

    // No markers at all: unknown, zero confidence, and an explicit reason.
    {
        std::vector<std::uint8_t> bytes(1024, 0x90);
        FakeMemory memory(base, bytes);
        const auto profile = FingerprintEngine(memory);
        CHECK(!profile.Known());
        CHECK(profile.confidence == 0.0f);
        CHECK(!profile.evidence.empty());
    }
}

// Builds a synthetic FNamePool: a block of entries, plus an array of block pointers
// pointing at it. Lets the whole pool path be tested with no game running.
struct SyntheticPool {
    std::vector<std::uint8_t> bytes;
    Address                   base{};
    std::uint64_t             blocks_offset{};
    std::vector<std::pair<std::uint32_t, std::string>> entries;   // id -> name
};

SyntheticPool BuildNamePool(Address base, const std::vector<std::string>& names,
                            bool case_preserving, std::uint32_t stride = 2,
                            std::uint32_t reserved = 0) {
    SyntheticPool pool;
    pool.base = base;

    // Block contents start some way in so the block pointer is not the buffer start.
    constexpr std::uint64_t kBlockOffset = 0x400;
    pool.bytes.assign(0x2000, 0);

    const std::uint32_t char_offset = case_preserving ? 6u : 2u;
    std::uint64_t cursor = kBlockOffset;

    for (const auto& name : names) {
        // Entry offsets are counted in stride units of 2 bytes.
        const std::uint32_t id =
            static_cast<std::uint32_t>((cursor - kBlockOffset) / stride);

        const std::uint16_t header =
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(name.size()) << 6);
        std::memcpy(pool.bytes.data() + cursor + (char_offset - 2), &header, sizeof(header));
        std::memcpy(pool.bytes.data() + cursor + char_offset, name.data(), name.size());

        pool.entries.emplace_back(id, name);

        cursor += char_offset + name.size() + reserved;
        cursor = (cursor + stride - 1) & ~static_cast<std::uint64_t>(stride - 1);
    }

    // The blocks array: one valid block pointer followed by nulls.
    pool.blocks_offset = 0x100;
    const std::uint64_t block_address = Raw(base) + kBlockOffset;
    std::memcpy(pool.bytes.data() + pool.blocks_offset, &block_address, sizeof(block_address));

    return pool;
}

void TestNamePool() {
    using zircon::engine::NamePoolInfo;
    using zircon::engine::ResolveFName;
    using zircon::engine::ResolveName;

    const auto base = static_cast<Address>(0x140000000ull);

    // "None" must come first: the pool finder anchors on it.
    const std::vector<std::string> names = {
        "None", "ByteProperty", "IntProperty", "Actor", "StaticMeshComponent",
    };

    for (const bool case_preserving : {false, true}) {
        auto synthetic = BuildNamePool(base, names, case_preserving);
        FakeMemory memory(base, synthetic.bytes);

        NamePoolInfo pool;
        pool.blocks            = base + synthetic.blocks_offset;
        pool.block_offset_bits = 16;
        pool.stride            = 2;
        pool.len_shift         = 6;
        pool.case_preserving   = case_preserving;

        for (const auto& [id, expected] : synthetic.entries)
            CHECK(ResolveName(memory, pool, id) == expected);

        // FName Number renders as a suffix one lower than the stored value, and 0 means
        // no suffix at all.
        const auto actor_id = synthetic.entries[3].first;
        CHECK(ResolveFName(memory, pool, actor_id, 0) == "Actor");
        CHECK(ResolveFName(memory, pool, actor_id, 1) == "Actor_0");
        CHECK(ResolveFName(memory, pool, actor_id, 4) == "Actor_3");

        // A wild id must fail cleanly.
        CHECK(ResolveName(memory, pool, 0x7FFFFFFFu).empty());
    }

    // An unresolvable name must come back empty, never as binary noise: emitters treat
    // an empty name as unresolved, but would happily write junk into a header.
    {
        std::vector<std::uint8_t> noise(0x1000);
        for (std::size_t i = 0; i < noise.size(); ++i)
            noise[i] = static_cast<std::uint8_t>(i * 7 + 3);

        FakeMemory memory(base, noise);
        NamePoolInfo pool;
        pool.blocks            = base;
        pool.block_offset_bits = 16;
        pool.stride            = 2;
        pool.len_shift         = 6;

        int garbage_returned = 0;
        for (std::uint32_t id = 0; id < 64; ++id) {
            const std::string name = ResolveName(memory, pool, id);
            for (const char c : name)
                if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126)
                    ++garbage_returned;
        }
        CHECK(garbage_returned == 0);
    }
}

// --- synthetic object world ------------------------------------------------------

// A miniature UE object graph: an unchunked FUObjectArray of classes, each with a class
// default object, laid out at the offsets the derivation is supposed to discover. Enough
// to exercise DeriveClassLayout without a game, which is the point — every test here has
// to run on a machine with nothing installed.
struct SyntheticWorld {
    std::vector<std::uint8_t> bytes;
    Address                   base{};

    zircon::engine::ObjectArrayInfo array;
    zircon::engine::UObjectLayout   object_layout;
    zircon::engine::UStructLayout   struct_layout;
    zircon::engine::NamePoolInfo    pool;

    // Where the decoy lives, so a test can fill it in or leave it null.
    int decoy_offset{};
};

// The layout this world uses. Deliberately not the offsets of any real engine build: a
// derivation that passes only because it guessed UE 5.6's numbers is not a derivation.
namespace synth {
constexpr int kIndex   = 0x0C;
constexpr int kClass   = 0x10;
constexpr int kName    = 0x18;
constexpr int kOuter   = 0x20;
constexpr int kSuper   = 0x38;
constexpr int kChildren= 0x40;
constexpr int kChildPr = 0x48;
constexpr int kPropSize= 0x50;
constexpr int kCdo     = 0x88;   // what DeriveClassLayout must find
constexpr int kDecoy   = 0x78;   // a self-referential pointer that is *not* the CDO
constexpr int kObjSize = 0x100;
} // namespace synth

SyntheticWorld BuildObjectWorld(Address base, int class_count, bool with_decoy) {
    using namespace synth;
    SyntheticWorld world;
    world.base = base;
    world.decoy_offset = kDecoy;

    // Names first: every object's FName id indexes this pool.
    std::vector<std::string> names = {"None", "Class", "Object"};
    for (int i = 0; i < class_count; ++i) {
        names.push_back("Klass" + std::to_string(i));
        names.push_back("Default__Klass" + std::to_string(i));
        names.push_back("Inst" + std::to_string(i));
    }

    auto pool = BuildNamePool(base, names, false);
    world.pool.blocks            = base + pool.blocks_offset;
    world.pool.block_offset_bits = 16;
    world.pool.stride            = 2;
    world.pool.len_shift         = 6;

    auto name_id = [&](const std::string& want) -> std::uint32_t {
        for (const auto& [id, text] : pool.entries)
            if (text == want) return id;
        return 0;
    };

    // The object region sits after the name pool inside one flat buffer.
    constexpr std::uint64_t kObjectsAt  = 0x4000;
    constexpr std::uint64_t kSlotsAt    = 0x2800;
    constexpr std::uint64_t kChunkTable = 0x2400;
    constexpr std::uint64_t kArrayAt    = 0x2000;

    world.bytes = pool.bytes;
    world.bytes.resize(0x4000 + static_cast<std::size_t>(class_count) * 3 * kObjSize + 0x200, 0);

    auto put64 = [&](std::uint64_t at, std::uint64_t value) {
        std::memcpy(world.bytes.data() + at, &value, sizeof(value));
    };
    auto put32 = [&](std::uint64_t at, std::uint32_t value) {
        std::memcpy(world.bytes.data() + at, &value, sizeof(value));
    };

    // Object 0 is the meta-class: the fixed point whose class is itself, named "Class".
    const std::uint64_t meta = kObjectsAt;
    const std::int32_t total = class_count * 3 + 1;

    put32(meta + kIndex, 0);
    put64(meta + kClass, Raw(base) + meta);          // its own class — the fixed point
    put32(meta + kName,  name_id("Class"));
    put64(meta + kSuper, 0);
    put32(meta + kPropSize, kObjSize);

    // Then class_count classes, each followed by its default object and one ordinary
    // instance.
    for (int i = 0; i < class_count; ++i) {
        const std::uint64_t klass = kObjectsAt + static_cast<std::uint64_t>(1 + i * 3) * kObjSize;
        const std::uint64_t cdo   = klass + kObjSize;
        const std::uint64_t inst  = cdo + kObjSize;

        put32(klass + kIndex, 1 + i * 3);
        put64(klass + kClass, Raw(base) + meta);     // a class's class is the meta-class
        put32(klass + kName,  name_id("Klass" + std::to_string(i)));
        put64(klass + kSuper, 0);
        put32(klass + kPropSize, kObjSize);
        put64(klass + kCdo,   Raw(base) + cdo);

        put32(cdo + kIndex, 2 + i * 3);
        put64(cdo + kClass, Raw(base) + klass);      // the CDO's class is its class
        put32(cdo + kName,  name_id("Default__Klass" + std::to_string(i)));

        put32(inst + kIndex, 3 + i * 3);
        put64(inst + kClass, Raw(base) + klass);
        put32(inst + kName,  name_id("Inst" + std::to_string(i)));

        // The decoy, and the whole point of this test: a slot at a *lower* offset holding
        // a pointer to an ordinary instance of the class. It is a live object, and it
        // names this very class as its class, so it closes the same loop the CDO does,
        // for every class, perfectly. Only the Default__ name tells the two apart — which
        // is exactly the shape of the bug this project hit six times before.
        if (with_decoy) put64(klass + kDecoy, Raw(base) + inst);
    }

    // The slot array: FUObjectItem is a pointer plus three int32s of flags.
    constexpr std::uint32_t kItemSize = 24;
    for (std::int32_t i = 0; i < total; ++i)
        put64(kSlotsAt + static_cast<std::uint64_t>(i) * kItemSize,
              Raw(base) + kObjectsAt + static_cast<std::uint64_t>(i) * kObjSize);

    // ObjectAt always walks inner -> chunk table -> chunk -> slot, so the world has to
    // supply all three even though one chunk holds everything here.
    put64(kChunkTable, Raw(base) + kSlotsAt);
    put64(kArrayAt,    Raw(base) + kChunkTable);

    world.array.inner             = base + kArrayAt;
    world.array.gobjects          = base + kArrayAt;
    world.array.chunked           = true;
    world.array.num_elements      = total;
    world.array.max_elements      = total;
    world.array.num_chunks        = 1;
    world.array.max_chunks        = 1;
    world.array.elements_per_chunk = static_cast<std::uint32_t>(total);
    world.array.item_size         = kItemSize;
    world.array.index_offset = kIndex;
    world.array.confidence   = 1.0f;

    world.object_layout.index_offset = kIndex;
    world.object_layout.class_offset = kClass;
    world.object_layout.name_offset  = kName;
    world.object_layout.outer_offset = kOuter;
    world.object_layout.uclass_object = base + meta;
    world.object_layout.confidence   = 1.0f;

    world.struct_layout.super_struct     = kSuper;
    world.struct_layout.children         = kChildren;
    world.struct_layout.child_properties = kChildPr;
    world.struct_layout.properties_size  = kPropSize;
    world.struct_layout.object_class     = base + meta;
    world.struct_layout.confidence       = 1.0f;

    return world;
}

void TestClassDefaultObject() {
    using zircon::engine::DeriveClassLayout;
    using zircon::engine::GetClassDefaultObject;

    const auto base = static_cast<Address>(0x140000000ull);

    // With the decoy present, the derivation must still land on the real CDO slot. The
    // decoy is the shape of every bug this project kept hitting: a field that passes a
    // weak, self-consistent test perfectly.
    for (const bool decoy : {false, true}) {
        auto world = BuildObjectWorld(base, 64, decoy);
        FakeMemory memory(base, world.bytes);

        const auto layout = DeriveClassLayout(memory, world.array, world.pool,
                                              world.object_layout, world.struct_layout);
        CHECK(layout.Valid());
        CHECK(layout.class_default_object == synth::kCdo);
        CHECK(layout.class_default_object != world.decoy_offset);
        CHECK(layout.confidence > 0.9f);

        // And the accessor must return the object the slot points at.
        const auto klass = zircon::engine::ObjectAt(memory, world.array, 1);
        const auto cdo   = GetClassDefaultObject(memory, layout, klass);
        CHECK(!IsNull(cdo));
        CHECK(Raw(zircon::engine::GetObjectClass(memory, world.object_layout, cdo)) ==
              Raw(klass));
        CHECK(zircon::engine::GetObjectName(memory, world.object_layout, world.pool, cdo)
                  .rfind("Default__", 0) == 0);
    }

    // Too few classes to be sure of anything: refusing is the correct answer, and a
    // layout that claims an offset from a handful of samples would be guessing.
    {
        auto world = BuildObjectWorld(base, 4, false);
        FakeMemory memory(base, world.bytes);
        const auto layout = DeriveClassLayout(memory, world.array, world.pool,
                                              world.object_layout, world.struct_layout);
        CHECK(!layout.Valid());
    }

    // No CDO pointers at all: also a refusal, not a nearby offset that happens to score.
    {
        auto world = BuildObjectWorld(base, 64, true);
        for (int i = 0; i < 64; ++i) {
            const std::uint64_t klass = 0x4000 + static_cast<std::uint64_t>(1 + i * 3) * synth::kObjSize;
            std::uint64_t zero = 0;
            std::memcpy(world.bytes.data() + klass + synth::kCdo, &zero, sizeof(zero));
        }
        FakeMemory memory(base, world.bytes);
        const auto layout = DeriveClassLayout(memory, world.array, world.pool,
                                              world.object_layout, world.struct_layout);
        CHECK(!layout.Valid());
    }
}

// --- extension hooks ------------------------------------------------------------------

// A pool whose entries are XOR-encrypted: what a shipped game with an obfuscated FName
// pool looks like to a reader that does not know the key. Nothing about the layout changes
// -- the header is still two bytes and the characters still follow it -- only the bytes
// are not the bytes.
constexpr std::uint8_t kPoolKey = 0x5A;

SyntheticPool BuildEncryptedPool(Address base, const std::vector<std::string>& names,
                                 std::uint64_t& entries_begin, std::uint64_t& entries_end) {
    auto pool = BuildNamePool(base, names, false);

    // Only the entry region is encrypted; the block pointer array is not, because a game
    // that encrypted its own pointers could not follow them either.
    entries_begin = 0x400;
    entries_end   = 0x2000;
    for (std::uint64_t at = entries_begin; at < entries_end; ++at)
        pool.bytes[at] ^= kPoolKey;

    return pool;
}

// The pool's stride is not a constant, and assuming it is was wrong on a real game.
//
// FF7 Rebirth is a case-preserving build, so its FNameEntry carries a DisplayIndex and
// aligns to 4, not 2. With the stride assumed to be 2 every id still resolved to
// *something* — byte offset id*2 lands inside the block either way — so the pool looked
// like it worked while handing back the wrong names, and the only visible symptom was the
// UObject layout failing its end-to-end check for no stated reason.
//
// That is the failure shape this project exists to avoid, so the stride is derived and
// this is what holds it derived.
// The pre-4.23 name pool is a different shape, not a different set of offsets.
//
// FNamePool packs entries end to end inside big blocks and addresses one by (block, byte
// offset). TNameEntryArray is a two-level table of *pointers*, so an id is (chunk, index)
// and an entry can be anywhere; entries carry no length header and are NUL-terminated.
// Nothing about reading one is shared with the modern pool, which is why they are told
// apart when the pool is found, not anywhere downstream.
// Reading a UE 5.7 UEnum, whose names and values are two separate arrays.
//
// Through 5.6 a UEnum holds one TArray<TPair<FName, int64>>. 5.7 splits it: the names are
// one array, the values another, and each is reached through a pointer whose low bit is
// set as a tag. Two independent 5.7 games agree on this exactly, which is what separated an
// engine change from one studio's fork.
//
// The tag is the part worth pinning. Following those pointers without masking bit 0 lands
// one byte into the array and every FName id afterwards is read from a misaligned offset --
// which does not fail, it produces different names, so nothing downstream would notice.
void TestSplitEnumArrays() {
    using zircon::engine::GetEnumValues;
    using zircon::engine::NamePoolInfo;
    using zircon::engine::UEnumLayout;

    const auto base = static_cast<Address>(0x140000000ull);

    const std::vector<std::string> names = {
        "None", "MOVE_None", "MOVE_Walking", "MOVE_Falling", "MOVE_Flying", "MOVE_MAX"};

    auto pool_image = BuildNamePool(base, names, false);

    // The enum object, and the two arrays it points at, all live in one image.
    constexpr std::uint64_t kEnumAt   = 0x3000;
    constexpr std::uint64_t kNamesAt  = 0x3100;
    constexpr std::uint64_t kValuesAt = 0x3200;
    constexpr int           kNamesField = 0x40;

    std::vector<std::uint8_t> image = pool_image.bytes;
    image.resize(0x4000, 0);

    auto put64 = [&](std::uint64_t at, std::uint64_t v) {
        std::memcpy(image.data() + at, &v, sizeof(v));
    };

    // Entries 1..5 of the pool are the enumerators; "None" at index 0 is not one.
    const int count = 5;
    for (int i = 0; i < count; ++i) {
        put64(kNamesAt  + static_cast<std::uint64_t>(i) * 8, pool_image.entries[i + 1].first);
        put64(kValuesAt + static_cast<std::uint64_t>(i) * 8, static_cast<std::uint64_t>(i * 10));
    }

    // Both pointers carry the tag the engine sets.
    put64(kEnumAt + kNamesField,     (Raw(base) + kNamesAt)  | 1ull);
    put64(kEnumAt + kNamesField + 8, (Raw(base) + kValuesAt) | 1ull);
    put64(kEnumAt + kNamesField + 16, static_cast<std::uint64_t>(count));

    FakeMemory memory(base, image);

    NamePoolInfo pool;
    pool.blocks            = base + pool_image.blocks_offset;
    pool.block_offset_bits = 16;
    pool.stride            = 2;
    pool.len_shift         = 6;

    UEnumLayout layout;
    layout.split_arrays = true;
    layout.names_array  = kNamesField;
    layout.values_array = kNamesField + 8;
    layout.count_at     = kNamesField + 16;

    const auto values = GetEnumValues(memory, layout, pool, base + kEnumAt);

    CHECK(values.size() == static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < values.size() && i < static_cast<std::size_t>(count); ++i) {
        CHECK(values[i].first == names[i + 1]);
        CHECK(values[i].second == static_cast<std::int64_t>(i * 10));
    }

    // An untagged pointer is not this layout, and must not be followed as though it were:
    // reading it anyway would produce names, just the wrong ones.
    {
        auto untagged = image;
        std::uint64_t plain = Raw(base) + kNamesAt;
        std::memcpy(untagged.data() + kEnumAt + kNamesField, &plain, sizeof(plain));

        FakeMemory other(base, untagged);
        CHECK(GetEnumValues(other, layout, pool, base + kEnumAt).empty());
    }
}

void TestNameEntryArrayPool() {
    using zircon::engine::EngineProfile;
    using zircon::engine::FindNamePool;
    using zircon::engine::ResolveName;
    using zircon::engine::SetGlobalResolver;

    const auto base = static_cast<Address>(0x140000000ull);

    // Offsets inside the synthetic image. The characters sit at +0xc, not the +0x10
    // the struct suggests, which is what the real 4.22 target turned out to use — so the
    // derivation has to measure it instead of assuming either.
    constexpr std::uint64_t kEntriesAt = 0x1000;
    constexpr std::uint64_t kChunk0At  = 0x0800;
    constexpr std::uint64_t kTableAt   = 0x0400;
    constexpr int           kCharsAt   = 0x0c;
    constexpr std::uint32_t kPerChunk  = 16384;

    std::vector<std::string> names = {"None"};
    for (int i = 0; i < 200; ++i)
        names.push_back("Entry" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 7), 'z'));

    std::vector<std::uint8_t> image(0x20000, 0);

    auto put64 = [&](std::uint64_t at, std::uint64_t v) {
        std::memcpy(image.data() + at, &v, sizeof(v));
    };

    std::uint64_t cursor = kEntriesAt;
    for (std::size_t i = 0; i < names.size(); ++i) {
        // The entry: a header this reader deliberately does not interpret, then the
        // characters, then a terminator.
        put64(cursor, 0);
        put64(cursor + 4, 0);
        std::memcpy(image.data() + cursor + kCharsAt, names[i].data(), names[i].size());

        put64(kChunk0At + i * 8, Raw(base) + cursor);
        cursor += static_cast<std::uint64_t>(kCharsAt) + names[i].size() + 1;
        cursor = (cursor + 7) & ~7ull;
    }

    put64(kTableAt, Raw(base) + kChunk0At);

    FakeMemory memory(base, image, /*writable*/ true);
    EngineProfile profile;

    SetGlobalResolver(nullptr);
    const auto found = FindNamePool(memory, profile);

    CHECK(found.has_value());
    if (!found) return;

    // It must recognise which pool this is. Reading a TNameEntryArray with the FNamePool
    // reader does not fail, it returns rubbish, so this flag is what keeps the two apart.
    CHECK(found->entry_array);
    CHECK(found->entry_chars == kCharsAt);
    CHECK(found->elements_per_chunk == kPerChunk);
    CHECK(Raw(found->blocks) == Raw(base) + kTableAt);

    // The assertion that matters: ids are sequential here, not byte offsets, and every one
    // resolves to its own name. A wrong chunk size or a wrong character offset still
    // returns text — it just returns the wrong text — so this compares strings.
    int correct = 0;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (ResolveName(memory, *found, static_cast<std::uint32_t>(i)) == names[i]) ++correct;

    CHECK(correct == static_cast<int>(names.size()));

    // An id past the end is not a name, and must not be read as one.
    CHECK(ResolveName(memory, *found, 999999u).empty());
}

void TestPoolStrideIsDerived() {
    using zircon::engine::EngineProfile;
    using zircon::engine::FindNamePool;
    using zircon::engine::ResolveName;
    using zircon::engine::SetGlobalResolver;

    const auto base = static_cast<Address>(0x140000000ull);

    // Enough names that a wrong walk cannot survive by luck, and of varied length so that
    // some of them end exactly on the alignment — which is the only place the reserved
    // byte is observable at all.
    std::vector<std::string> names = {"None"};
    for (int i = 0; i < 120; ++i)
        names.push_back("Name" + std::string(static_cast<std::size_t>(i % 9), 'x') +
                        std::to_string(i));

    struct Case { std::uint32_t stride; std::uint32_t reserved; bool case_preserving; };
    for (const Case& variant : {Case{2, 0, false}, Case{4, 0, true},
                                Case{4, 1, true},  Case{2, 0, true}}) {
        auto synthetic = BuildNamePool(base, names, variant.case_preserving,
                                       variant.stride, variant.reserved);
        FakeMemory memory(base, synthetic.bytes);
        EngineProfile profile;

        const Address blocks = base + synthetic.blocks_offset;
        SetGlobalResolver([blocks](zircon::core::IMemorySource&,
                                   zircon::engine::GlobalCandidates& out) -> bool {
            out.name_pool = blocks;
            return true;
        });

        const auto found = FindNamePool(memory, profile);
        CHECK(found.has_value());
        if (!found) continue;

        CHECK(found->stride == variant.stride);

        // The real assertion: every name resolves through the derived parameters. A wrong
        // stride does not fail here, it returns a different name — so this compares text,
        // never just "did something come back".
        int correct = 0;
        for (const auto& [id, expected] : synthetic.entries)
            if (ResolveName(memory, *found, id) == expected) ++correct;

        CHECK(correct == static_cast<int>(synthetic.entries.size()));
    }

    SetGlobalResolver(nullptr);
}

void TestNameEntryDecoder() {
    using zircon::engine::NamePoolInfo;
    using zircon::engine::ResolveName;
    using zircon::engine::SetNameEntryDecoder;

    const auto base = static_cast<Address>(0x140000000ull);
    const std::vector<std::string> names = {"None", "Actor", "PlayerController"};

    std::uint64_t begin = 0, end = 0;
    auto synthetic = BuildEncryptedPool(base, names, begin, end);
    FakeMemory memory(base, synthetic.bytes);

    NamePoolInfo pool;
    pool.blocks            = base + synthetic.blocks_offset;
    pool.block_offset_bits = 16;
    pool.stride            = 2;
    pool.len_shift         = 6;

    // Without a decoder the entries are noise, and noise must come back as *nothing*. A
    // dumper that returned the decrypted-looking garbage would fill an SDK with it.
    SetNameEntryDecoder(nullptr);
    for (const auto& [id, expected] : synthetic.entries) {
        (void)expected;
        CHECK(ResolveName(memory, pool, id).empty());
    }

    // With one, every name comes back exactly.
    SetNameEntryDecoder([](zircon::core::IMemorySource& source, Address entry,
                           std::uint8_t* out, std::size_t capacity) -> std::size_t {
        const std::size_t want = std::min<std::size_t>(capacity, 512);
        const std::size_t got  = source.Read(entry, out, want);
        for (std::size_t i = 0; i < got; ++i) out[i] ^= kPoolKey;
        return got;
    });

    for (const auto& [id, expected] : synthetic.entries)
        CHECK(ResolveName(memory, pool, id) == expected);

    // A decoder that declines leaves the ordinary path in charge, which on this pool means
    // no name at all.
    SetNameEntryDecoder([](zircon::core::IMemorySource&, Address, std::uint8_t*,
                           std::size_t) -> std::size_t { return 0; });
    CHECK(ResolveName(memory, pool, synthetic.entries[1].first).empty());

    // A decoder that returns plausible-looking rubbish must not get it into the dump: the
    // printable check applies to decoded bytes exactly as it does to read ones.
    SetNameEntryDecoder([](zircon::core::IMemorySource&, Address, std::uint8_t* out,
                           std::size_t capacity) -> std::size_t {
        if (capacity < 16) return 0;
        const std::uint16_t header = static_cast<std::uint16_t>(8u << 6);
        std::memcpy(out, &header, sizeof(header));
        for (std::size_t i = 0; i < 8; ++i) out[2 + i] = static_cast<std::uint8_t>(i + 1);
        return 10;
    });
    CHECK(ResolveName(memory, pool, synthetic.entries[1].first).empty());

    SetNameEntryDecoder(nullptr);
}

void TestGlobalResolver() {
    using zircon::engine::EngineProfile;
    using zircon::engine::FindNamePool;
    using zircon::engine::SetGlobalResolver;

    const auto base = static_cast<Address>(0x140000000ull);
    const std::vector<std::string> names = {"None", "Actor"};

    auto synthetic = BuildNamePool(base, names, false);
    FakeMemory memory(base, synthetic.bytes);
    EngineProfile profile;

    // FakeMemory reports no writable region, so the ordinary scan cannot find anything
    // here. That makes this pair of assertions unambiguous: whatever comes back came from
    // the hook, and whatever does not was refused.
    SetGlobalResolver(nullptr);
    CHECK(!FindNamePool(memory, profile).has_value());

    const Address real = base + synthetic.blocks_offset;
    SetGlobalResolver([real](zircon::core::IMemorySource&,
                             zircon::engine::GlobalCandidates& out) -> bool {
        out.name_pool = real;
        return true;
    });

    const auto found = FindNamePool(memory, profile);
    CHECK(found.has_value());
    if (found) CHECK(Raw(found->blocks) == Raw(real));

    // The property that makes a hook safe: a wrong address is *checked*, not believed. A
    // resolver pointing somewhere with no "None" entry behind it must yield nothing at all
    // and not a pool rooted at the address it named.
    SetGlobalResolver([base](zircon::core::IMemorySource&,
                             zircon::engine::GlobalCandidates& out) -> bool {
        out.name_pool = base + 0x10;   // inside the buffer, but not a block array
        return true;
    });

    const auto rejected = FindNamePool(memory, profile);
    CHECK(!rejected.has_value());

    // Declining is not the same as being wrong: it just means the scan runs.
    SetGlobalResolver([](zircon::core::IMemorySource&,
                         zircon::engine::GlobalCandidates&) -> bool { return false; });
    CHECK(!FindNamePool(memory, profile).has_value());

    SetGlobalResolver(nullptr);
}

} // namespace

int main() {
    SetLogLevel(LogLevel::Error);   // keep expected-failure paths quiet

    TestAddress();
    TestPatternParsing();
    TestPatternMatching();
    TestScanner();
    TestRipRelative();
    TestReadHelpers();
    TestCache();
    TestPeImage();
    TestStaticSourceRejectsLiveObjects();
    TestUnrealDetection();
    TestVersionFingerprint();
    TestNamePool();
    TestClassDefaultObject();
    TestSplitEnumArrays();
    TestNameEntryArrayPool();
    TestPoolStrideIsDerived();
    TestNameEntryDecoder();
    TestGlobalResolver();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
