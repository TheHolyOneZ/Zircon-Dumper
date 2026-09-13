// Diff tests. Every case is a synthetic before/after pair, so these run with no game
// installed and assert on exact classification.

#include "diff/Diff.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace zircon;
using namespace zircon::diff;

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

// --- fixtures -----------------------------------------------------------------------

ir::TypeRef Prim(ir::TypeKind kind, std::int32_t size, std::string raw) {
    ir::TypeRef type;
    type.kind = kind;
    type.size = size;
    type.raw  = std::move(raw);
    return type;
}

ir::Property Prop(std::string name, ir::TypeRef type, std::int32_t offset) {
    ir::Property property;
    property.size   = type.size;
    property.name   = std::move(name);
    property.type   = std::move(type);
    property.offset = offset;
    return property;
}

// A small but realistic dump: one package, one class with a base, properties of several
// shapes, a function with a native address, and an enum.
ir::Dump MakeDump() {
    ir::Struct actor;
    actor.name           = "Actor";
    actor.path           = "/Script/Engine.Actor";
    actor.super          = "/Script/CoreUObject.Object";
    actor.is_class       = true;
    actor.size           = 0x2A8;
    actor.alignment      = 8;
    actor.inherited_size = 0x28;
    actor.properties.push_back(Prop("RootComponent",
                                    Prim(ir::TypeKind::ObjectPtr, 8, "ObjectProperty"), 0x150));
    actor.properties.push_back(Prop("NetPriority",
                                    Prim(ir::TypeKind::Float, 4, "FloatProperty"), 0x158));

    ir::Property hidden = Prop("bHidden", Prim(ir::TypeKind::Bool, 1, "BoolProperty"), 0x58);
    hidden.is_bitfield = true;
    hidden.bit_index   = 7;
    hidden.field_mask  = 0x80;
    actor.properties.push_back(hidden);

    ir::Function tick;
    tick.name       = "ReceiveTick";
    tick.flags      = 0x00020400;
    tick.native_rva = 0x40EF000;
    ir::FunctionParam delta;
    delta.name = "DeltaSeconds";
    delta.type = Prim(ir::TypeKind::Float, 4, "FloatProperty");
    tick.params.push_back(delta);
    actor.functions.push_back(tick);

    ir::Enum mode;
    mode.name = "EMovementMode";
    mode.path = "/Script/Engine.EMovementMode";
    mode.underlying = "uint8";
    mode.values.push_back(ir::EnumValue{"MOVE_None", 0});
    mode.values.push_back(ir::EnumValue{"MOVE_Walking", 1});

    ir::Package package;
    package.name = "/Script/Engine";
    package.classes.push_back(std::move(actor));
    package.enums.push_back(std::move(mode));

    ir::Dump dump;
    dump.header.source.process  = "Game-Win64-Shipping.exe";
    dump.header.engine.version  = "5.6";
    dump.header.created_utc     = "2026-01-01T00:00:00Z";
    dump.header.offsets.push_back(ir::DerivedOffset{"UObject.NamePrivate", 0x18});
    dump.packages.push_back(std::move(package));
    return dump;
}

ir::Struct& FirstClass(ir::Dump& dump) { return dump.packages.front().classes.front(); }

const Change* Find(const DiffResult& result, ChangeKind kind, std::string_view member = {}) {
    for (const auto& change : result.changes)
        if (change.kind == kind && (member.empty() || change.member == member))
            return &change;
    return nullptr;
}

std::size_t CountOf(const DiffResult& result, ChangeKind kind) {
    std::size_t n = 0;
    for (const auto& change : result.changes)
        if (change.kind == kind) ++n;
    return n;
}

// --- tests --------------------------------------------------------------------------

void TestIdenticalDumpsHaveNoChanges() {
    const auto dump = MakeDump();
    const auto result = Diff(dump, dump);

    // The single most important property of a diff: comparing a thing to itself must be
    // silent. Any noise here makes every real report untrustworthy.
    CHECK(result.changes.empty());
    CHECK(!result.HasBreakingChanges());
    CHECK(result.stats.types_compared == 1);
    CHECK(result.stats.types_unchanged == 1);
}

void TestMovedOffsetIsCritical() {
    const auto before = MakeDump();
    auto after = MakeDump();
    FirstClass(after).properties[0].offset = 0x158;

    const auto result = Diff(before, after);

    const auto* moved = Find(result, ChangeKind::PropertyMoved, "RootComponent");
    CHECK(moved != nullptr);
    if (moved) {
        CHECK(moved->severity == Severity::Critical);
        CHECK(moved->before == "0x150");
        CHECK(moved->after == "0x158");
        CHECK(moved->detail.find("+8") != std::string::npos);
    }
    CHECK(result.HasBreakingChanges());
    CHECK(result.stats.types_unchanged == 0);

    // A moved offset must not also be reported as removed and added, which would bury it.
    CHECK(CountOf(result, ChangeKind::PropertyRemoved) == 0);
    CHECK(CountOf(result, ChangeKind::PropertyAdded) == 0);
}

void TestRenameKeepsOffsetAndIsNotCritical() {
    const auto before = MakeDump();
    auto after = MakeDump();
    FirstClass(after).properties[0].name = "RootComponentPtr";

    const auto result = Diff(before, after);

    // Same offset, same type: this is a rename, and treating it as removed+added would
    // claim a hardcoded offset died when it did not.
    const auto* renamed = Find(result, ChangeKind::PropertyRenamed);
    CHECK(renamed != nullptr);
    if (renamed) {
        CHECK(renamed->before == "RootComponent");
        CHECK(renamed->after == "RootComponentPtr");
        CHECK(renamed->severity == Severity::Medium);
    }
    CHECK(CountOf(result, ChangeKind::PropertyRemoved) == 0);
    CHECK(CountOf(result, ChangeKind::PropertyAdded) == 0);

    // With detection off, the same input must decompose into removed + added.
    DiffOptions options;
    options.detect_renames = false;
    const auto raw = Diff(before, after, options);
    CHECK(CountOf(raw, ChangeKind::PropertyRenamed) == 0);
    CHECK(CountOf(raw, ChangeKind::PropertyRemoved) == 1);
    CHECK(CountOf(raw, ChangeKind::PropertyAdded) == 1);
}

void TestRenameRequiresSameOffsetAndType() {
    // Same offset but a different type is not a rename: the bytes there mean something
    // else now, which is exactly the case that must stay loud.
    {
        const auto before = MakeDump();
        auto after = MakeDump();
        FirstClass(after).properties[0].name = "Renamed";
        FirstClass(after).properties[0].type = Prim(ir::TypeKind::Int64, 8, "Int64Property");

        const auto result = Diff(before, after);
        CHECK(CountOf(result, ChangeKind::PropertyRenamed) == 0);
        CHECK(CountOf(result, ChangeKind::PropertyRemoved) == 1);
        CHECK(CountOf(result, ChangeKind::PropertyAdded) == 1);
    }

    // Same name change but also moved: not a rename either.
    {
        const auto before = MakeDump();
        auto after = MakeDump();
        FirstClass(after).properties[0].name = "Renamed";
        FirstClass(after).properties[0].offset = 0x200;

        const auto result = Diff(before, after);
        CHECK(CountOf(result, ChangeKind::PropertyRenamed) == 0);
    }
}

void TestTypeChangeAtSameOffset() {
    const auto before = MakeDump();
    auto after = MakeDump();
    after.packages.front().classes.front().properties[1].type =
        Prim(ir::TypeKind::Double, 8, "DoubleProperty");
    after.packages.front().classes.front().properties[1].size = 8;

    const auto result = Diff(before, after);

    // A float becoming a double at the same offset reads plausible garbage instead of
    // failing, so it has to be critical.
    const auto* changed = Find(result, ChangeKind::PropertyTypeChanged, "NetPriority");
    CHECK(changed != nullptr);
    if (changed) CHECK(changed->severity == Severity::Critical);
    CHECK(Find(result, ChangeKind::PropertyResized, "NetPriority") != nullptr);
}

void TestBitfieldMoveIsDetected() {
    const auto before = MakeDump();
    auto after = MakeDump();
    auto& bit = FirstClass(after).properties[2];
    bit.bit_index  = 3;
    bit.field_mask = 0x08;

    const auto result = Diff(before, after);

    // The byte offset did not change, so only a bit-level comparison catches this. Reading
    // the old mask now returns a different flag entirely.
    const auto* moved = Find(result, ChangeKind::PropertyBitMoved, "bHidden");
    CHECK(moved != nullptr);
    if (moved) {
        CHECK(moved->severity == Severity::Critical);
        CHECK(moved->before.find("0x80") != std::string::npos);
        CHECK(moved->after.find("0x08") != std::string::npos);
    }
    CHECK(CountOf(result, ChangeKind::PropertyMoved) == 0);
}

// A member name is not unique within a type, and a diff that assumes it is invents changes.
//
// FF7 Rebirth ships a Blueprint class with two properties both named "Light", at different
// offsets and of different types. Keyed by name alone the map kept only the last of them,
// so the first compared against the wrong member and diffing that dump *against itself*
// reported a critical move and a type change. A diff tool that cannot recognise a file as
// equal to itself is worse than no diff tool.
void TestDuplicateMemberNames() {
    auto dump = MakeDump();

    ir::Property second;
    second.name   = FirstClass(dump).properties[0].name;   // deliberately the same name
    second.offset = FirstClass(dump).properties[0].offset + 0x20;
    second.size   = 8;
    second.type.kind = ir::TypeKind::ObjectPtr;
    second.type.name = "/Script/Engine.Texture2D";
    second.type.raw  = "ObjectProperty";
    FirstClass(dump).properties.push_back(second);

    // The invariant that matters: a dump is equal to itself, duplicates and all.
    {
        const auto result = Diff(dump, dump);
        CHECK(result.changes.empty());
        CHECK(!result.HasBreakingChanges());
    }

    // And the duplicates must not be reported as added or removed either way round.
    {
        const auto plain = MakeDump();
        const auto grew  = Diff(plain, dump);

        // Exactly one property appeared; the original keeps its identity and is not
        // reported as replaced.
        CHECK(CountOf(grew, ChangeKind::PropertyAdded) == 1);
        CHECK(CountOf(grew, ChangeKind::PropertyMoved) == 0);
        CHECK(CountOf(grew, ChangeKind::PropertyTypeChanged) == 0);

        const auto shrank = Diff(dump, plain);
        CHECK(CountOf(shrank, ChangeKind::PropertyRemoved) == 1);
        CHECK(CountOf(shrank, ChangeKind::PropertyMoved) == 0);
    }

    // A real change to the *second* duplicate must still be caught: pairing by occurrence
    // has to preserve detection, not merely silence it.
    {
        auto moved = dump;
        FirstClass(moved).properties.back().offset += 0x40;

        const auto result = Diff(dump, moved);
        CHECK(CountOf(result, ChangeKind::PropertyMoved) == 1);
    }
}

void TestDefaultValueChanges() {
    // A patch that moves nothing and changes how the game behaves. Nothing structural
    // differs here, so every other comparison in the diff stays silent and this is the
    // only thing that reports.
    {
        auto before = MakeDump();
        auto after  = MakeDump();
        FirstClass(before).properties[0].default_value = "2.5";
        FirstClass(after).properties[0].default_value  = "3.75";

        const auto result = Diff(before, after);

        const auto* changed = Find(result, ChangeKind::PropertyDefaultChanged,
                                   FirstClass(after).properties[0].name);
        CHECK(changed != nullptr);
        if (changed) {
            CHECK(changed->before == "2.5");
            CHECK(changed->after == "3.75");
            // It breaks no code, so it must never gate a build.
            CHECK(changed->severity == Severity::Low);
        }
        CHECK(!result.HasBreakingChanges());
    }

    // Defaults are opt-in. One dump having them and the other not is a difference in how
    // the dumps were taken, not a change in the game, and reporting it would bury a real
    // report under one row per property the first time someone passed --defaults.
    {
        auto before = MakeDump();
        auto after  = MakeDump();
        FirstClass(after).properties[0].default_value = "3.75";

        const auto result = Diff(before, after);
        CHECK(CountOf(result, ChangeKind::PropertyDefaultChanged) == 0);
    }
    {
        auto before = MakeDump();
        auto after  = MakeDump();
        FirstClass(before).properties[0].default_value = "2.5";

        const auto result = Diff(before, after);
        CHECK(CountOf(result, ChangeKind::PropertyDefaultChanged) == 0);
    }

    // The same value on both sides is not a change.
    {
        auto before = MakeDump();
        auto after  = MakeDump();
        FirstClass(before).properties[0].default_value = "2.5";
        FirstClass(after).properties[0].default_value  = "2.5";

        const auto result = Diff(before, after);
        CHECK(CountOf(result, ChangeKind::PropertyDefaultChanged) == 0);
    }
}

void TestFunctionChanges() {
    const auto before = MakeDump();

    {
        auto after = MakeDump();
        FirstClass(after).functions[0].native_rva = 0x40EF100;
        const auto result = Diff(before, after);

        const auto* moved = Find(result, ChangeKind::FunctionMoved, "ReceiveTick");
        CHECK(moved != nullptr);
        if (moved) CHECK(moved->severity == Severity::High);
    }

    {
        auto after = MakeDump();
        ir::FunctionParam extra;
        extra.name = "bForce";
        extra.type = Prim(ir::TypeKind::Bool, 1, "BoolProperty");
        FirstClass(after).functions[0].params.push_back(extra);

        const auto result = Diff(before, after);
        CHECK(Find(result, ChangeKind::FunctionSignatureChanged, "ReceiveTick") != nullptr);
    }

    {
        // A script-only function has no address in either dump; 0 -> 0 is not a move.
        auto without = MakeDump();
        FirstClass(without).functions[0].native_rva = 0;
        const auto result = Diff(without, without);
        CHECK(CountOf(result, ChangeKind::FunctionMoved) == 0);
    }

    {
        auto after = MakeDump();
        FirstClass(after).functions.clear();
        const auto result = Diff(before, after);
        const auto* gone = Find(result, ChangeKind::FunctionRemoved, "ReceiveTick");
        CHECK(gone != nullptr);
        if (gone) CHECK(gone->severity == Severity::Critical);
    }
}

void TestAdditionsAreNotBreaking() {
    const auto before = MakeDump();
    auto after = MakeDump();

    ir::Struct fresh;
    fresh.name = "NewThing";
    fresh.path = "/Script/Engine.NewThing";
    fresh.is_class = true;
    fresh.size = 0x40;
    after.packages.front().classes.push_back(std::move(fresh));
    FirstClass(after).properties.push_back(
        Prop("NewProp", Prim(ir::TypeKind::Int32, 4, "IntProperty"), 0x2A0));

    const auto result = Diff(before, after);
    CHECK(Find(result, ChangeKind::TypeAdded) != nullptr);
    CHECK(Find(result, ChangeKind::PropertyAdded, "NewProp") != nullptr);

    // New surface cannot break code that already worked.
    CHECK(!result.HasBreakingChanges());

    DiffOptions options;
    options.include_additions = false;
    const auto filtered = Diff(before, after, options);
    CHECK(CountOf(filtered, ChangeKind::TypeAdded) == 0);
    CHECK(CountOf(filtered, ChangeKind::PropertyAdded) == 0);
}

void TestEnumAndHeaderChanges() {
    const auto before = MakeDump();

    {
        auto after = MakeDump();
        after.packages.front().enums.front().values[1].value = 2;
        const auto result = Diff(before, after);
        const auto* changed = Find(result, ChangeKind::EnumValueChanged, "MOVE_Walking");
        CHECK(changed != nullptr);
        if (changed) {
            CHECK(changed->severity == Severity::High);
            CHECK(changed->before == "1" && changed->after == "2");
        }
    }

    {
        auto after = MakeDump();
        after.header.engine.version = "5.7";
        const auto result = Diff(before, after);
        CHECK(Find(result, ChangeKind::EngineVersionChanged) != nullptr);
    }

    {
        // A core layout offset moving explains a whole report at once, so it is called
        // out, not left to be inferred from the damage.
        auto after = MakeDump();
        after.header.offsets[0].value = 0x20;
        const auto result = Diff(before, after);
        const auto* moved = Find(result, ChangeKind::LayoutOffsetChanged, "UObject.NamePrivate");
        CHECK(moved != nullptr);
        if (moved) CHECK(moved->before == "0x18" && moved->after == "0x20");
    }
}

void TestSeverityOrderingAndFilter() {
    const auto before = MakeDump();
    auto after = MakeDump();
    FirstClass(after).properties[0].offset = 0x160;   // critical
    FirstClass(after).functions[0].flags = 0x1;       // low
    FirstClass(after).size = 0x2B0;                   // high

    const auto result = Diff(before, after);
    CHECK(result.changes.size() >= 3);

    // Most severe first: a report that opens with flag noise buries the thing that broke.
    for (std::size_t i = 1; i < result.changes.size(); ++i)
        CHECK(result.changes[i - 1].severity >= result.changes[i].severity);

    DiffOptions options;
    options.minimum = Severity::High;
    const auto serious = Diff(before, after, options);
    for (const auto& change : serious.changes) CHECK(change.severity >= Severity::High);
    CHECK(CountOf(serious, ChangeKind::FunctionFlagsChanged) == 0);
}

void TestPackageFilter() {
    auto before = MakeDump();
    auto after  = MakeDump();

    ir::Package other;
    other.name = "/Script/Other";
    ir::Struct record;
    record.name = "Thing";
    record.path = "/Script/Other.Thing";
    record.is_class = true;
    record.size = 0x10;
    other.classes.push_back(record);
    before.packages.push_back(other);

    other.classes.front().size = 0x20;
    after.packages.push_back(other);

    const auto all = Diff(before, after);
    CHECK(Find(all, ChangeKind::TypeResized) != nullptr);

    DiffOptions options;
    options.package_filter = "/Script/Engine";
    const auto scoped = Diff(before, after, options);
    CHECK(CountOf(scoped, ChangeKind::TypeResized) == 0);
}

void TestDeterministicOrder() {
    const auto before = MakeDump();
    auto after = MakeDump();
    for (int i = 0; i < 20; ++i)
        FirstClass(after).properties.push_back(
            Prop("Extra" + std::to_string(i), Prim(ir::TypeKind::Int32, 4, "IntProperty"),
                 0x300 + i * 4));

    // The indices are hash maps, so without an explicit sort the report order would vary
    // between runs and two diffs of the same pair could not be compared.
    const auto first  = Diff(before, after);
    const auto second = Diff(before, after);
    CHECK(first.changes.size() == second.changes.size());
    for (std::size_t i = 0; i < first.changes.size(); ++i) {
        CHECK(first.changes[i].kind == second.changes[i].kind);
        CHECK(first.changes[i].path == second.changes[i].path);
        CHECK(first.changes[i].member == second.changes[i].member);
    }
}

void TestReportsRender() {
    const auto before = MakeDump();
    auto after = MakeDump();
    FirstClass(after).properties[0].offset = 0x158;
    FirstClass(after).properties[1].name = "NetPriorityValue";
    FirstClass(after).functions[0].native_rva = 0x40EF200;

    const auto result = Diff(before, after);

    const std::string text = RenderText(result, before, after, false);
    CHECK(text.find("RootComponent") != std::string::npos);
    CHECK(text.find("critical") != std::string::npos);
    // Colour must be opt-in: escape codes in a redirected file corrupt it downstream.
    CHECK(text.find("\033[") == std::string::npos);
    CHECK(RenderText(result, before, after, true).find("\033[") != std::string::npos);

    const std::string json = RenderJson(result);
    CHECK(json.find("\"breaking\": true") != std::string::npos);
    CHECK(json.find("\"property_moved\"") != std::string::npos);
    CHECK(json.front() == '{');

    const std::string markdown = RenderMarkdown(result, before, after);
    CHECK(markdown.find("# Migration report") != std::string::npos);
    CHECK(markdown.find("Moved offsets") != std::string::npos);
    CHECK(markdown.find("| +8 |") != std::string::npos);   // the delta column
    CHECK(markdown.find("Renamed, offset unchanged") != std::string::npos);
    CHECK(markdown.find("Functions that moved") != std::string::npos);

    // A clean diff must say so out loud, and skip the empty tables.
    const auto clean = Diff(before, before);
    CHECK(RenderMarkdown(clean, before, before).find("No breaking changes") !=
          std::string::npos);
    CHECK(RenderText(clean, before, before, false).find("no differences") !=
          std::string::npos);
}

void TestPipeInNameDoesNotBreakMarkdown() {
    auto before = MakeDump();
    auto after  = MakeDump();
    FirstClass(before).properties[0].name = "Weird|Name";
    FirstClass(after).properties[0].name  = "Weird|Name";
    FirstClass(after).properties[0].offset = 0x160;

    const auto result = Diff(before, after);
    const std::string markdown = RenderMarkdown(result, before, after);

    // An unescaped pipe just ends the table cell, so the row renders wrong rather
    // than failing visibly.
    CHECK(markdown.find("Weird\\|Name") != std::string::npos);
}

void TestEmptyDumps() {
    const ir::Dump empty;
    const auto result = Diff(empty, empty);
    CHECK(result.changes.empty());
    CHECK(!result.HasBreakingChanges());
    CHECK(RenderJson(result).find("\"changes\": []") != std::string::npos);

    // Everything disappearing is the worst case a real diff can report, and it must not
    // crash on the empty side or under-report what is gone.
    const auto populated = MakeDump();
    const auto wiped = Diff(populated, empty);
    CHECK(CountOf(wiped, ChangeKind::TypeRemoved) == 1);
    CHECK(CountOf(wiped, ChangeKind::EnumRemoved) == 1);
    CHECK(CountOf(wiped, ChangeKind::PackageRemoved) == 1);
    CHECK(wiped.HasBreakingChanges());
}

} // namespace

int main() {
    TestIdenticalDumpsHaveNoChanges();
    TestMovedOffsetIsCritical();
    TestRenameKeepsOffsetAndIsNotCritical();
    TestRenameRequiresSameOffsetAndType();
    TestTypeChangeAtSameOffset();
    TestBitfieldMoveIsDetected();
    TestDuplicateMemberNames();
    TestDefaultValueChanges();
    TestFunctionChanges();
    TestAdditionsAreNotBreaking();
    TestEnumAndHeaderChanges();
    TestSeverityOrderingAndFilter();
    TestPackageFilter();
    TestDeterministicOrder();
    TestReportsRender();
    TestPipeInNameDoesNotBreakMarkdown();
    TestEmptyDumps();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
