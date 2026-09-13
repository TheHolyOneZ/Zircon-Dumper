// Writing values back into a target.
//
// The bitfield case is the reason this file exists. Seven flags share a byte in
// CharacterMovementComponent alone, so a write that sets the byte instead of the bit
// destroys six unrelated settings and nothing reports an error.

#include "core/MemorySource.h"
#include "engine/PropertyLayout.h"
#include "engine/TypeResolver.h"
#include "engine/ValueWriter.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace zircon;
using namespace zircon::core;
using namespace zircon::engine;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("  FAIL line %d: %s\n", line, expression);
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

// Memory that can be written, so a test can assert on the bytes afterwards.
class Fake final : public IMemorySource {
public:
    Fake(Address base, std::size_t size) : base_(base), bytes_(size, 0) {
        modules_.push_back(ModuleInfo{"fake.exe", "", base_, bytes_.size()});
        regions_.push_back(RegionInfo{base_, bytes_.size(),
                                      RegionProtect::Read | RegionProtect::Write, true});
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        if (Raw(addr) < Raw(base_)) return 0;
        const std::uint64_t offset = Raw(addr) - Raw(base_);
        if (offset >= bytes_.size()) return 0;
        const std::size_t available = std::min<std::size_t>(size, bytes_.size() - offset);
        std::memcpy(out, bytes_.data() + offset, available);
        return available;
    }

    bool Write(Address addr, const void* in, std::size_t size) override {
        if (!writes_enabled_) return false;
        if (Raw(addr) < Raw(base_)) return false;
        const std::uint64_t offset = Raw(addr) - Raw(base_);
        if (offset + size > bytes_.size()) return false;
        std::memcpy(bytes_.data() + offset, in, size);
        return true;
    }

    bool EnableWrites(bool enable) override {
        writes_enabled_ = enable;
        return writes_enabled_;
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }
    Capabilities Caps() const override {
        return Capabilities{true, writes_enabled_, false, true};
    }
    std::string Describe() const override { return "fake"; }

    template <typename T>
    void Poke(std::uint64_t offset, T value) {
        std::memcpy(bytes_.data() + offset, &value, sizeof(T));
    }
    template <typename T>
    T Peek(std::uint64_t offset) const {
        T value{};
        std::memcpy(&value, bytes_.data() + offset, sizeof(T));
        return value;
    }

private:
    Address                  base_;
    std::vector<std::uint8_t> bytes_;
    bool                     writes_enabled_{false};
    std::vector<ModuleInfo>  modules_;
    std::vector<RegionInfo>  regions_;
};

// A name pool holding one ANSI entry per name, laid out the way FNamePool blocks are.
struct Pool {
    NamePoolInfo info;
    std::vector<std::uint32_t> ids;
};

constexpr std::uint64_t kBase        = 0x140000000ull;
constexpr std::uint64_t kPoolAt      = 0x1000;
constexpr std::uint64_t kBlockAt     = 0x2000;
constexpr std::uint64_t kClassAt     = 0x3000;   // the property's FFieldClass
constexpr std::uint64_t kPropertyAt  = 0x4000;
constexpr std::uint64_t kObjectAt    = 0x5000;   // the object being edited

// Writes one FNamePool block containing `names` and returns their ids.
Pool BuildPool(Fake& memory, const std::vector<std::string>& names) {
    Pool pool;
    pool.info.blocks            = Address{kBase + kPoolAt};
    pool.info.block_offset_bits = 16;
    pool.info.stride            = 2;
    pool.info.len_shift         = 6;
    pool.info.case_preserving   = false;

    memory.Poke<std::uint64_t>(kPoolAt, kBase + kBlockAt);

    std::uint64_t cursor = kBlockAt;
    for (const auto& name : names) {
        const std::uint64_t offset = cursor - kBlockAt;
        pool.ids.push_back(static_cast<std::uint32_t>(offset / pool.info.stride));

        const std::uint16_t header =
            static_cast<std::uint16_t>((name.size() << pool.info.len_shift) | 0);
        memory.Poke<std::uint16_t>(cursor, header);
        for (std::size_t i = 0; i < name.size(); ++i)
            memory.Poke<std::uint8_t>(cursor + 2 + i, static_cast<std::uint8_t>(name[i]));

        cursor += 2 + name.size();
        cursor = (cursor + 1) & ~1ull;   // entries stay stride-aligned
    }
    return pool;
}

struct Fixture {
    Fake            memory{Address{kBase}, 0x8000};
    Pool            pool;
    ObjectArrayInfo array;
    UObjectLayout   object_layout;
    UStructLayout   struct_layout;
    FPropertyLayout property_layout;
    SubclassLayout  subclass_layout;

    // Lays out one property of `type_name` at `value_offset` within the edited object.
    void Build(const std::string& type_name, std::int32_t value_offset,
               std::int32_t element_size) {
        pool = BuildPool(memory, {"None", type_name});

        object_layout.name_offset = 0x18;

        property_layout.class_private    = 0x08;
        property_layout.class_name_offset = 0x00;   // an FFieldClass starts with its FName
        property_layout.next             = 0x18;
        property_layout.name             = 0x20;
        property_layout.array_dim        = 0x30;
        property_layout.element_size     = 0x34;
        property_layout.property_flags   = 0x38;
        property_layout.offset_internal  = 0x44;

        subclass_layout.bool_masks = 0x70;

        memory.Poke<std::uint64_t>(kPropertyAt + 0x08, kBase + kClassAt);
        memory.Poke<std::uint32_t>(kClassAt + 0x00, pool.ids[1]);
        memory.Poke<std::int32_t>(kPropertyAt + 0x34, element_size);
        memory.Poke<std::int32_t>(kPropertyAt + 0x44, value_offset);
    }

    ResolveContext Context() {
        ResolveContext context;
        context.memory          = &memory;
        context.array           = &array;
        context.pool            = &pool.info;
        context.object_layout   = &object_layout;
        context.struct_layout   = &struct_layout;
        context.property_layout = &property_layout;
        context.subclass_layout = &subclass_layout;
        return context;
    }

    Address Property() const { return Address{kBase + kPropertyAt}; }
    Address Object()   const { return Address{kBase + kObjectAt}; }
};

// --- tests ---------------------------------------------------------------------------

void TestRefusesWhenWritesDisabled() {
    Fixture f;
    f.Build("FloatProperty", 0x10, 4);

    const auto result = WritePropertyValue(f.Context(), f.Object(), f.Property(), "1.0");
    CHECK(!result.ok);
    CHECK(result.error.find("not enabled") != std::string::npos);
}

void TestFloat() {
    Fixture f;
    f.Build("FloatProperty", 0x10, 4);
    f.memory.EnableWrites(true);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "1337.5").ok);
    CHECK(f.memory.Peek<float>(kObjectAt + 0x10) == 1337.5f);

    // Text that is not a number leaves the bytes alone.
    CHECK(!WritePropertyValue(f.Context(), f.Object(), f.Property(), "fast").ok);
    CHECK(f.memory.Peek<float>(kObjectAt + 0x10) == 1337.5f);
}

void TestIntegerWidthIsRespected() {
    Fixture f;
    f.Build("ByteProperty", 0x20, 1);
    f.memory.EnableWrites(true);

    // Neighbouring bytes carry a marker, so a write wider than the property shows up.
    f.memory.Poke<std::uint8_t>(kObjectAt + 0x21, 0xAB);
    f.memory.Poke<std::uint8_t>(kObjectAt + 0x22, 0xCD);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "200").ok);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x20) == 200);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x21) == 0xAB);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x22) == 0xCD);

    // 300 does not fit in a byte. Truncating silently to 44 would be the worst outcome,
    // so it is refused and the old value stands.
    const auto overflow = WritePropertyValue(f.Context(), f.Object(), f.Property(), "300");
    CHECK(!overflow.ok);
    CHECK(overflow.error.find("does not fit") != std::string::npos);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x20) == 200);
}

void TestHexAccepted() {
    Fixture f;
    f.Build("IntProperty", 0x30, 4);
    f.memory.EnableWrites(true);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "0x1F4").ok);
    CHECK(f.memory.Peek<std::int32_t>(kObjectAt + 0x30) == 500);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "-42").ok);
    CHECK(f.memory.Peek<std::int32_t>(kObjectAt + 0x30) == -42);
}

// The one that matters. Seven flags share a byte; setting one must leave the rest alone.
void TestBitfieldPreservesNeighbours() {
    Fixture f;
    f.Build("BoolProperty", 0x40, 1);
    f.memory.EnableWrites(true);

    // FBoolProperty keeps FieldSize, ByteOffset, ByteMask, FieldMask as four bytes.
    // This property owns bit 2.
    f.memory.Poke<std::uint8_t>(kPropertyAt + 0x70 + 2, 0x04);   // ByteMask
    f.memory.Poke<std::uint8_t>(kPropertyAt + 0x70 + 3, 0x04);   // FieldMask

    // Every other flag in the byte is set.
    f.memory.Poke<std::uint8_t>(kObjectAt + 0x40, 0xFB);   // 1111 1011, bit 2 clear

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "true").ok);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x40) == 0xFF);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "false").ok);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x40) == 0xFB);

    // A whole-byte write would have produced 0x01 and 0x00 here, wiping the other seven.
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x40) != 0x00);
}

void TestNativeBoolOwnsItsByte() {
    Fixture f;
    f.Build("BoolProperty", 0x50, 1);
    f.memory.EnableWrites(true);

    // A full mask means the property owns the byte outright.
    f.memory.Poke<std::uint8_t>(kPropertyAt + 0x70 + 2, 0xFF);
    f.memory.Poke<std::uint8_t>(kPropertyAt + 0x70 + 3, 0xFF);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "true").ok);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x50) == 1);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "0").ok);
    CHECK(f.memory.Peek<std::uint8_t>(kObjectAt + 0x50) == 0);

    CHECK(!WritePropertyValue(f.Context(), f.Object(), f.Property(), "yes").ok);
}

// A StructProperty pointing at a UScriptStruct with three float members, the shape of
// FVector. Enough for the writer to walk, which is the part worth testing: a struct write
// touches several offsets and a wrong one lands in the neighbour.
struct StructFixture {
    Fake            memory{Address{kBase}, 0x9000};
    Pool            pool;
    ObjectArrayInfo array;
    UObjectLayout   object_layout;
    UStructLayout   struct_layout;
    FPropertyLayout property_layout;
    SubclassLayout  subclass_layout;

    static constexpr std::uint64_t kScriptStructAt = 0x6000;
    static constexpr std::uint64_t kMemberAt[3]    = {0x6100, 0x6200, 0x6300};
    static constexpr std::uint64_t kFloatClassAt   = 0x6400;
    static constexpr std::int32_t  kValueOffset    = 0x80;

    // ResolveType will not follow FStructProperty::Struct unless the target is a live
    // object of class ScriptStruct, which is the check that stops a stray pointer being
    // read as a type. So the fixture needs a real slot in a real array.
    static constexpr std::uint64_t kScriptStructClassAt = 0x6500;
    static constexpr std::uint64_t kSlotsAt             = 0x7000;
    static constexpr std::uint64_t kChunkTableAt        = 0x7100;
    static constexpr std::uint64_t kArrayAt             = 0x7200;

    void Build() {
        pool = BuildPool(memory, {"None", "StructProperty", "FloatProperty", "X", "Y", "Z",
                                  "ScriptStruct", "Vector"});

        object_layout.index_offset = 0x0C;
        object_layout.class_offset = 0x10;
        object_layout.name_offset  = 0x18;
        object_layout.outer_offset = 0x20;

        // One slot, holding the ScriptStruct.
        memory.Poke<std::uint64_t>(kSlotsAt, kBase + kScriptStructAt);
        memory.Poke<std::uint64_t>(kChunkTableAt, kBase + kSlotsAt);
        memory.Poke<std::uint64_t>(kArrayAt, kBase + kChunkTableAt);

        array.inner              = Address{kBase + kArrayAt};
        array.gobjects           = Address{kBase + kArrayAt};
        array.chunked            = true;
        array.num_elements       = 1;
        array.max_elements       = 1;
        array.num_chunks         = 1;
        array.max_chunks         = 1;
        array.elements_per_chunk = 1;
        array.item_size          = 24;
        array.index_offset       = 0x0C;
        array.confidence         = 1.0f;

        // The struct object: index 0, and a class whose name is ScriptStruct.
        memory.Poke<std::int32_t>(kScriptStructAt + 0x0C, 0);
        memory.Poke<std::uint64_t>(kScriptStructAt + 0x10, kBase + kScriptStructClassAt);
        memory.Poke<std::uint32_t>(kScriptStructAt + 0x18, pool.ids[7]);   // "Vector"
        memory.Poke<std::uint32_t>(kScriptStructClassAt + 0x18, pool.ids[6]);  // ScriptStruct
        memory.Poke<std::uint64_t>(kScriptStructClassAt + 0x10,
                                   kBase + kScriptStructClassAt);          // class of class

        property_layout.class_private     = 0x08;
        property_layout.class_name_offset = 0x00;
        property_layout.next              = 0x18;
        property_layout.name              = 0x20;
        property_layout.element_size      = 0x34;
        property_layout.offset_internal   = 0x44;

        struct_layout.child_properties = 0x50;
        subclass_layout.struct_struct  = 0x78;

        // The outer property: a StructProperty at kValueOffset pointing at the struct.
        memory.Poke<std::uint64_t>(kPropertyAt + 0x08, kBase + kClassAt);
        memory.Poke<std::uint32_t>(kClassAt + 0x00, pool.ids[1]);        // "StructProperty"
        memory.Poke<std::int32_t>(kPropertyAt + 0x34, 12);
        memory.Poke<std::int32_t>(kPropertyAt + 0x44, kValueOffset);
        memory.Poke<std::uint64_t>(kPropertyAt + 0x78, kBase + kScriptStructAt);

        // The struct, and its three members chained through Next.
        memory.Poke<std::uint64_t>(kScriptStructAt + 0x50, kBase + kMemberAt[0]);
        memory.Poke<std::uint32_t>(kFloatClassAt + 0x00, pool.ids[2]);   // "FloatProperty"

        for (int i = 0; i < 3; ++i) {
            const std::uint64_t at = kMemberAt[i];
            memory.Poke<std::uint64_t>(at + 0x08, kBase + kFloatClassAt);
            memory.Poke<std::uint64_t>(at + 0x18,
                                       i < 2 ? kBase + kMemberAt[i + 1] : 0ull);
            memory.Poke<std::uint32_t>(at + 0x20, pool.ids[3 + i]);      // X, Y, Z
            memory.Poke<std::int32_t>(at + 0x34, 4);
            memory.Poke<std::int32_t>(at + 0x44, i * 4);
        }
    }

    ResolveContext Context() {
        ResolveContext context;
        context.memory          = &memory;
        context.array           = &array;
        context.pool            = &pool.info;
        context.object_layout   = &object_layout;
        context.struct_layout   = &struct_layout;
        context.property_layout = &property_layout;
        context.subclass_layout = &subclass_layout;
        return context;
    }

    Address Property() const { return Address{kBase + kPropertyAt}; }
    Address Object()   const { return Address{kBase + kObjectAt}; }

    float Member(int index) const {
        return memory.Peek<float>(kObjectAt + static_cast<std::uint64_t>(kValueOffset) +
                                  static_cast<std::uint64_t>(index) * 4);
    }
};

void TestPlainStructIsWritable() {
    StructFixture f;
    f.Build();
    f.memory.EnableWrites(true);

    CHECK(IsPropertyWritable(f.Context(), f.Property()));

    // Positional, in declaration order.
    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "1,2,3").ok);
    CHECK(f.Member(0) == 1.0f);
    CHECK(f.Member(1) == 2.0f);
    CHECK(f.Member(2) == 3.0f);

    // Named, and partial: the members not mentioned keep what they had.
    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "{Z=-9}").ok);
    CHECK(f.Member(0) == 1.0f);
    CHECK(f.Member(1) == 2.0f);
    CHECK(f.Member(2) == -9.0f);

    // Braces are optional, and the names are matched without case.
    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "y=7").ok);
    CHECK(f.Member(1) == 7.0f);
}

void TestStructWriteIsAllOrNothing() {
    StructFixture f;
    f.Build();
    f.memory.EnableWrites(true);

    CHECK(WritePropertyValue(f.Context(), f.Object(), f.Property(), "4,5,6").ok);

    // The third value is not a number. Writing the first two and then failing would leave
    // the struct in a state that was never asked for, so nothing is written at all.
    const auto bad = WritePropertyValue(f.Context(), f.Object(), f.Property(), "7,8,nine");
    CHECK(!bad.ok);
    CHECK(f.Member(0) == 4.0f);
    CHECK(f.Member(1) == 5.0f);
    CHECK(f.Member(2) == 6.0f);

    // Same for a member that does not exist, and for the wrong number of values.
    CHECK(!WritePropertyValue(f.Context(), f.Object(), f.Property(), "{W=1}").ok);
    CHECK(!WritePropertyValue(f.Context(), f.Object(), f.Property(), "1,2").ok);
    CHECK(f.Member(0) == 4.0f);
    CHECK(f.Member(2) == 6.0f);
}

void TestContainersAreRefused() {
    // A TArray has a heap pointer and two counts. Writing text over it corrupts the
    // allocator, and the game keeps running until it does not.
    for (const char* type : {"ArrayProperty", "StrProperty", "MapProperty",
                             "ObjectProperty"}) {
        Fixture f;
        f.Build(type, 0x60, 16);
        f.memory.EnableWrites(true);

        const auto result = WritePropertyValue(f.Context(), f.Object(), f.Property(), "0");
        CHECK(!result.ok);
        CHECK(result.error.find("cannot be written safely") != std::string::npos);
        CHECK(!IsPropertyWritable(f.Context(), f.Property()));
    }

    // A struct is refused for its own reason, and says so: some are writable and the
    // message should say why this one is not.
    {
        Fixture f;
        f.Build("StructProperty", 0x60, 16);
        f.memory.EnableWrites(true);

        const auto result = WritePropertyValue(f.Context(), f.Object(), f.Property(), "0");
        CHECK(!result.ok);
        CHECK(result.error.find("all numbers") != std::string::npos);
        CHECK(!IsPropertyWritable(f.Context(), f.Property()));
    }
}

void TestWritableReportingMatchesBehaviour() {
    for (const char* type : {"BoolProperty", "IntProperty", "FloatProperty",
                             "DoubleProperty", "ByteProperty", "Int64Property"}) {
        Fixture f;
        f.Build(type, 0x10, 4);
        CHECK(IsPropertyWritable(f.Context(), f.Property()));
    }
}

} // namespace

int main() {
    std::printf("value writer\n");
    TestRefusesWhenWritesDisabled();
    TestFloat();
    TestIntegerWidthIsRespected();
    TestHexAccepted();
    TestBitfieldPreservesNeighbours();
    TestNativeBoolOwnsItsByte();
    TestPlainStructIsWritable();
    TestStructWriteIsAllOrNothing();
    TestContainersAreRefused();
    TestWritableReportingMatchesBehaviour();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
