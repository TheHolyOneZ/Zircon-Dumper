#include "mono/Assembly.h"
#include "mono/Runtime.h"
#include "mono/Static.h"

#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        ++g_checks;                                                                     \
        if (!(expr)) {                                                                  \
            ++g_failures;                                                               \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);        \
        }                                                                               \
    } while (false)

using namespace zircon;

namespace {

void TestModuleNames() {
    CHECK(mono::LooksLikeMonoModuleName("mono-2.0-bdwgc.dll"));
    CHECK(mono::LooksLikeMonoModuleName("MONO-2.0-SGEN.DLL"));
    CHECK(mono::LooksLikeMonoModuleName("mono.dll"));
    CHECK(mono::LooksLikeMonoModuleName("libmonobdwgc-2.0.so"));
    CHECK(!mono::LooksLikeMonoModuleName("GameAssembly.dll"));
    CHECK(!mono::LooksLikeMonoModuleName("UnityPlayer.dll"));
    CHECK(!mono::LooksLikeMonoModuleName("Mono.Cecil.dll"));
}

void TestEntryPointGroups() {
    CHECK(mono::RequiredEntryPointCount() > 20);
    CHECK(mono::OptionalEntryPointCount() > 20);
}

void TestRefusesRubbish() {
    CHECK(!mono::ReadAssembly("no-such-file-at-all.dll"));

    mono::AssemblySetStats stats;
    const auto none = mono::ReadManagedFolder("no-such-folder-at-all", stats);
    CHECK(none.empty());
    CHECK(stats.files == 0);
}

void TestStaticDumpShape() {
    mono::AssemblyMetadata assembly;
    assembly.name    = "Assembly-CSharp";
    assembly.version = "1.2.3.4";
    assembly.mvid    = "aaaabbbb-cccc-dddd-eeee-ffff00001111";

    mono::TypeRow player;
    player.name       = "Player";
    player.name_space = "Game";
    player.token      = 0x02000042;

    mono::MethodRow hit;
    hit.rva   = 0x2050;
    hit.flags = 0x0006;
    hit.name  = "TakeDamage";
    hit.token = 0x06000011;
    player.methods.push_back(hit);

    mono::FieldRow health;
    health.flags = 0x0006;
    health.name  = "health";
    player.fields.push_back(health);
    assembly.types.push_back(player);

    mono::TypeRow mode;
    mode.name         = "Mode";
    mode.name_space   = "Game";
    mode.is_enum      = true;
    mode.is_valuetype = true;
    mode.underlying   = "int32";
    mode.enum_values  = {{"Idle", 0}, {"Running", 1}, {"Broken", -1}};
    assembly.types.push_back(mode);

    mono::StaticStats stats;
    const auto dump = mono::BuildStaticDump({assembly}, stats);

    CHECK(dump.header.runtime == "mono");
    CHECK(dump.header.partial);
    CHECK(dump.schema_version == ir::kSchemaVersion);
    CHECK(dump.packages.size() == 1);
    if (dump.packages.empty()) return;

    const auto& package = dump.packages.front();
    CHECK(package.name == "Assembly-CSharp");
    CHECK(package.assembly_version == "1.2.3.4");
    CHECK(package.mvid == "aaaabbbb-cccc-dddd-eeee-ffff00001111");
    CHECK(package.classes.size() == 1);
    CHECK(package.enums.size() == 1);

    if (!package.classes.empty()) {
        const auto& record = package.classes.front();
        CHECK(record.path == "Game.Player, Assembly-CSharp");
        CHECK(record.token == 0x02000042);
        CHECK(record.source == "static");
        CHECK(record.functions.size() == 1);
        if (!record.functions.empty()) {
            CHECK(record.functions.front().il_rva == 0x2050);
            CHECK(record.functions.front().native_rva == 0);
        }
        CHECK(record.properties.size() == 1);
        if (!record.properties.empty()) {
            CHECK(record.properties.front().offset_unresolved);
            CHECK(record.properties.front().offset == 0);
        }
    }

    if (!package.enums.empty()) {
        const auto& record = package.enums.front();
        CHECK(record.values.size() == 3);
        CHECK(record.values_resolved);
        CHECK(record.values.back().value == -1);
    }
    CHECK(stats.enum_values == 3);
    CHECK(stats.with_il == 1);
}

void TestMergeFillsWhatLiveCannotKnow() {
    ir::Dump live;
    live.header.runtime = "mono";
    live.header.sources = {"live"};

    ir::Package package;
    package.name = "Assembly-CSharp";

    ir::Struct player;
    player.path = "Game.Player, Assembly-CSharp";
    player.name = "Player";

    ir::Property health;
    health.name   = "health";
    health.offset = 0x18;
    player.properties.push_back(health);

    ir::Function hit;
    hit.name  = "TakeDamage";
    hit.token = 0x06000011;
    player.functions.push_back(hit);
    package.classes.push_back(player);

    ir::Enum mode;
    mode.path = "Game.Mode, Assembly-CSharp";
    mode.name = "Mode";
    mode.values_resolved = false;
    mode.values = {{"Idle", 0}, {"Running", 0}};
    package.enums.push_back(mode);
    live.packages.push_back(package);

    mono::AssemblyMetadata assembly;
    assembly.name = "Assembly-CSharp";
    assembly.mvid = "1111";

    mono::TypeRow row;
    row.name       = "Player";
    row.name_space = "Game";
    mono::MethodRow method;
    method.rva   = 0x2050;
    method.name  = "TakeDamage";
    method.token = 0x06000011;
    row.methods.push_back(method);
    assembly.types.push_back(row);

    mono::TypeRow enum_row;
    enum_row.name         = "Mode";
    enum_row.name_space   = "Game";
    enum_row.is_enum      = true;
    enum_row.is_valuetype = true;
    enum_row.enum_values  = {{"Idle", 0}, {"Running", 7}};
    assembly.types.push_back(enum_row);

    mono::TypeRow only_static;
    only_static.name       = "NeverBuilt";
    only_static.name_space = "Game";
    assembly.types.push_back(only_static);

    mono::StaticStats static_stats;
    const auto from_metadata = mono::BuildStaticDump({assembly}, static_stats);

    mono::MergeStats stats;
    const auto merged = mono::MergeDumps(live, from_metadata, stats);

    CHECK(merged.header.sources.size() == 2);
    CHECK(!merged.header.partial);
    CHECK(stats.matched == 1);
    CHECK(stats.enums_filled == 1);
    CHECK(stats.il_filled == 1);
    CHECK(stats.static_only == 1);

    bool offset_kept = false, il_filled = false, value_filled = false, extra = false;
    for (const auto& p : merged.packages) {
        for (const auto& record : p.classes) {
            if (record.path == "Game.Player, Assembly-CSharp") {
                CHECK(record.source == "both");
                if (!record.properties.empty())
                    offset_kept = record.properties.front().offset == 0x18;
                if (!record.functions.empty())
                    il_filled = record.functions.front().il_rva == 0x2050;
            }
            if (record.path == "Game.NeverBuilt, Assembly-CSharp") extra = true;
        }
        for (const auto& record : p.enums)
            if (record.path == "Game.Mode, Assembly-CSharp")
                value_filled = record.values_resolved && record.values.back().value == 7;
        CHECK(p.mvid == "1111");
    }
    CHECK(offset_kept);
    CHECK(il_filled);
    CHECK(value_filled);
    CHECK(extra);
}

void TestMergeKeepsOnePerPath() {
    ir::Dump live;
    live.header.runtime = "mono";

    ir::Package package;
    package.name = "A";
    ir::Enum shared;
    shared.path = "Game.Thing, A";
    shared.name = "Thing";
    package.enums.push_back(shared);
    live.packages.push_back(package);

    mono::AssemblyMetadata assembly;
    assembly.name = "A";
    mono::TypeRow row;
    row.name         = "Thing";
    row.name_space   = "Game";
    row.is_enum      = true;
    row.is_valuetype = true;
    assembly.types.push_back(row);

    mono::StaticStats static_stats;
    const auto from_metadata = mono::BuildStaticDump({assembly}, static_stats);

    mono::MergeStats stats;
    const auto merged = mono::MergeDumps(live, from_metadata, stats);

    int seen = 0;
    for (const auto& p : merged.packages) {
        for (const auto& r : p.enums)   if (r.path == "Game.Thing, A") ++seen;
        for (const auto& r : p.classes) if (r.path == "Game.Thing, A") ++seen;
        for (const auto& r : p.structs) if (r.path == "Game.Thing, A") ++seen;
    }
    CHECK(seen == 1);
}

void TestNestedNaming() {
    mono::AssemblyMetadata assembly;
    assembly.name = "A";

    mono::TypeRow outer;
    outer.name       = "Outer";
    outer.name_space = "Game";
    assembly.types.push_back(outer);

    mono::TypeRow inner;
    inner.name      = "Inner";
    inner.enclosing = 0;
    assembly.types.push_back(inner);

    mono::TypeRow deepest;
    deepest.name      = "Deepest";
    deepest.enclosing = 1;
    assembly.types.push_back(deepest);

    CHECK(assembly.FullNameOf(0) == "Game.Outer");
    CHECK(assembly.FullNameOf(1) == "Game.Outer.Inner");
    CHECK(assembly.FullNameOf(2) == "Game.Outer.Inner.Deepest");
}

} // namespace

int main() {
    TestModuleNames();
    TestEntryPointGroups();
    TestRefusesRubbish();
    TestStaticDumpShape();
    TestMergeFillsWhatLiveCannotKnow();
    TestMergeKeepsOnePerPath();
    TestNestedNaming();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
