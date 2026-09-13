// Dependency-free test runner for the IR layer. Pure data in, pure data out: nothing
// here touches a process, a file it did not write, or a game.

#include "ir/Json.h"
#include "ir/Model.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace zircon::ir;

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

void TestTypeKindRoundTrip() {
    // Every kind, not a sample. A kind missing from the table would serialise as
    // "unknown" and parse back as Unknown, so the round-trip assertion catches it.
    std::vector<std::string> seen;

    for (const TypeKind kind : kAllTypeKinds) {
        const std::string text{ToString(kind)};
        CHECK(!text.empty());

        const auto parsed = TypeKindFromString(text);
        CHECK(parsed.has_value());
        CHECK(parsed && *parsed == kind);

        // Two kinds sharing a name would round-trip one of them to the other.
        for (const auto& previous : seen) CHECK(previous != text);
        seen.push_back(text);
    }

    CHECK(seen.size() == std::size(kAllTypeKinds));
    CHECK(!TypeKindFromString("not_a_kind").has_value());
    CHECK(!TypeKindFromString("").has_value());

    // Names are the documented lowercase forms, not the C++ spellings.
    CHECK(ToString(TypeKind::MulticastDelegate) == "multicast_delegate");
    CHECK(ToString(TypeKind::SoftClassPtr) == "softclassptr");
    CHECK(ToString(TypeKind::ObjectPtr) == "objectptr");
    CHECK(ToString(TypeKind::UInt64) == "uint64");
}

// A dump exercising every part of the model that has ever been easy to lose in
// serialisation: nested containers, bitfields, negative enum values, high-bit flags.
Dump MakeRichDump() {
    Dump dump;

    dump.header.tool_version = "0.1.0-test";
    dump.header.created_utc  = "2026-09-12T18:00:00Z";
    dump.header.partial      = true;

    dump.header.source.kind        = "external";
    dump.header.source.process     = "Game-Win64-Shipping.exe";
    dump.header.source.main_module = "Game-Win64-Shipping.exe";
    dump.header.source.module_base = 0x7ff7bcf60000ull;
    dump.header.source.image_size  = 171896832;

    dump.header.engine.version              = "5.6";
    dump.header.engine.confidence           = 0.9599999f;
    dump.header.engine.uses_fproperty       = true;
    dump.header.engine.chunked_gobjects     = false;
    dump.header.engine.chunked_name_pool    = true;
    dump.header.engine.case_preserving_name = true;
    dump.header.engine.evidence = {"version string: ++UE5+Release-5.6", "24/24 slots agreed"};

    dump.header.offsets = {{"UObject.InternalIndex", 12}, {"UObject.ClassPrivate", 16}};
    dump.header.globals = {"GObjects=0x98670c0", "GNames=0x9783590"};

    dump.names = {"None", "ByteProperty", "Actor"};

    Package package;
    package.name = "/Script/Engine";

    // TMap<FName, TArray<FVector>> — the deepest container shape the IR supports.
    TypeRef inner_struct;
    inner_struct.kind = TypeKind::Struct;
    inner_struct.name = "/Script/CoreUObject.Vector";
    inner_struct.raw  = "StructProperty";
    inner_struct.size = 24;

    TypeRef inner_array;
    inner_array.kind   = TypeKind::Array;
    inner_array.raw    = "ArrayProperty";
    inner_array.size   = 16;
    inner_array.params = {inner_struct};

    TypeRef key;
    key.kind = TypeKind::Name;
    key.raw  = "NameProperty";
    key.size = 8;

    TypeRef map;
    map.kind   = TypeKind::Map;
    map.raw    = "MapProperty";
    map.size   = 80;
    map.params = {key, inner_array};

    Property mapped;
    mapped.name      = "Lookup";
    mapped.type      = map;
    mapped.offset    = 0x40;
    mapped.size      = 80;
    mapped.array_dim = 1;
    mapped.flags     = 0x8000000000000001ull;   // top bit set on purpose
    mapped.flag_names = {"Edit", "BlueprintVisible"};

    Property flag;
    flag.name        = "bHidden";
    flag.type        = TypeRef{TypeKind::Bool, "", "BoolProperty", {}, 1};
    flag.offset      = 0x94;
    flag.size        = 1;
    flag.is_bitfield = true;
    flag.byte_mask   = 0xFF;
    flag.field_mask  = 0x04;
    flag.bit_index   = 2;

    // A fixed-size array member, which is where a wrong array_dim default shows up.
    Property fixed;
    fixed.name      = "Slots";
    fixed.type      = TypeRef{TypeKind::Int32, "", "IntProperty", {}, 4};
    fixed.offset    = 0xA0;
    fixed.size      = 32;
    fixed.array_dim = 8;

    Function function;
    function.name       = "K2_GetActorLocation";
    function.flags      = 0x04020401u;
    function.flag_names = {"Native", "Public", "BlueprintCallable"};
    function.native_rva   = 0x1234abcdull;
    function.script_size  = 128;

    FunctionParam returned;
    returned.name      = "ReturnValue";
    returned.type      = inner_struct;
    returned.offset    = 0;
    returned.size      = 24;
    returned.is_return = true;

    FunctionParam out_param;
    out_param.name   = "OutHit";
    out_param.type   = inner_struct;
    out_param.offset = 24;
    out_param.size   = 24;
    out_param.is_out = true;
    out_param.is_const = true;

    function.params = {out_param, returned};

    Struct actor;
    actor.name           = "Actor";
    actor.path           = "/Script/Engine.Actor";
    actor.super          = "/Script/CoreUObject.Object";
    actor.is_class       = true;   // set from the containing array on parse
    actor.size           = 680;
    actor.alignment      = 8;
    actor.inherited_size = 40;
    actor.interfaces     = {"/Script/Engine.Interface_AssetUserData"};
    actor.properties     = {mapped, flag, fixed};
    actor.functions      = {function};
    package.classes.push_back(actor);

    Struct vector_struct;
    vector_struct.name = "Vector";
    vector_struct.path = "/Script/CoreUObject.Vector";
    vector_struct.size = 24;
    vector_struct.alignment = 8;
    package.structs.push_back(vector_struct);

    Enum movement;
    movement.name       = "EMovementMode";
    movement.path       = "/Script/Engine.EMovementMode";
    movement.underlying = "uint8";
    movement.is_flags   = true;
    movement.values     = {{"MOVE_None", 0}, {"MOVE_Walking", 1}, {"MOVE_Invalid", -1},
                           {"MOVE_Huge", 9223372036854775807LL}};
    package.enums.push_back(movement);

    dump.packages.push_back(package);
    return dump;
}

void TestDumpRoundTrip() {
    const Dump original = MakeRichDump();

    for (const bool pretty : {true, false}) {
        const std::string text = WriteJsonString(original, pretty);
        auto parsed = ParseJson(text);

        CHECK(parsed.ok());
        if (!parsed.ok()) {
            std::fprintf(stderr, "  parse error: %s at %zu\n",
                         parsed.error().message.c_str(), parsed.error().offset);
            continue;
        }
        CHECK(parsed.value() == original);

        // Writing what we parsed must produce the same bytes, or the format is not a
        // fixed point and diffing two dumps would show spurious changes.
        CHECK(WriteJsonString(parsed.value(), pretty) == text);
    }

    // The fields most likely to be damaged without anything noticing.
    const std::string text = WriteJsonString(original, true);
    auto parsed = ParseJson(text);
    CHECK(parsed.ok());
    if (!parsed.ok()) return;

    const Dump& round_tripped = parsed.value();
    const Struct& actor = round_tripped.packages.at(0).classes.at(0);

    CHECK(actor.properties.at(0).flags == 0x8000000000000001ull);
    CHECK(actor.properties.at(0).type.params.at(1).params.at(0).name ==
          "/Script/CoreUObject.Vector");
    CHECK(actor.properties.at(1).is_bitfield);
    CHECK(actor.properties.at(1).field_mask == 0x04);
    CHECK(actor.properties.at(2).array_dim == 8);
    CHECK(actor.functions.at(0).native_rva == 0x1234abcdull);
    CHECK(round_tripped.packages.at(0).enums.at(0).values.at(2).value == -1);
    CHECK(round_tripped.packages.at(0).enums.at(0).values.at(3).value ==
          9223372036854775807LL);
    CHECK(round_tripped.header.engine.confidence == original.header.engine.confidence);
    CHECK(round_tripped.header.source.module_base == 0x7ff7bcf60000ull);

    // Counting helpers should agree with what went in.
    CHECK(round_tripped.TotalClasses() == 1);
    CHECK(round_tripped.TotalStructs() == 1);
    CHECK(round_tripped.TotalEnums() == 1);
    CHECK(round_tripped.TotalProperties() == 3);
    CHECK(round_tripped.TotalFunctions() == 1);
    CHECK(round_tripped.FindPackage("/Script/Engine") != nullptr);
    CHECK(round_tripped.FindPackage("/Script/Nope") == nullptr);
}

void TestHighBitFlags() {
    // A uint64 routed through a double loses precision above 2^53, and EPropertyFlags
    // genuinely uses the high bits, so this is a real corruption risk and not a
    // theoretical one.
    const std::uint64_t values[] = {
        0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull,
        0x0020000000000001ull,
        9007199254740993ull,        // 2^53 + 1, the first integer a double cannot hold
    };

    for (const std::uint64_t value : values) {
        Dump dump;
        Package package;
        package.name = "/Script/Test";

        Property property;
        property.name  = "Flagged";
        property.flags = value;

        Struct record;
        record.name = "T";
        record.path = "/Script/Test.T";
        record.properties.push_back(property);
        package.classes.push_back(record);
        dump.packages.push_back(package);

        auto parsed = ParseJson(WriteJsonString(dump, false));
        CHECK(parsed.ok());
        if (parsed.ok())
            CHECK(parsed.value().packages.at(0).classes.at(0).properties.at(0).flags == value);
    }
}

void TestStringEscaping() {
    Dump dump;
    dump.header.tool_version = "quote:\" backslash:\\ newline:\n tab:\t cr:\r";
    dump.names = {
        "/Script/Engine.Actor",                 // slashes must survive unescaped
        "Name_With\"Quote",
        "Back\\slash",
        "Line\nBreak",
        std::string("Nul\x01Control"),
        "Unicode\xE2\x9C\x93",                  // UTF-8 check mark passes through
    };

    const std::string text = WriteJsonString(dump, true);

    // Slashes are legal escaped, but escaping them would bloat every UE path in the dump.
    CHECK(text.find("\\/") == std::string::npos);
    CHECK(text.find("/Script/Engine.Actor") != std::string::npos);

    auto parsed = ParseJson(text);
    CHECK(parsed.ok());
    if (parsed.ok()) CHECK(parsed.value() == dump);

    // Escapes we never emit must still parse, since dumps may be hand-edited.
    auto escaped = ParseJson(R"({"names":["a\/b","Aé😀"],"packages":[]})");
    CHECK(escaped.ok());
    if (escaped.ok()) {
        CHECK(escaped.value().names.at(0) == "a/b");
        CHECK(escaped.value().names.at(1) == "A\xC3\xA9\xF0\x9F\x98\x80");
    }
}

void TestMalformedInput() {
    struct Case {
        const char* text;
        const char* what;
    };
    const Case cases[] = {
        {"",                              "empty input"},
        {"{",                             "truncated object"},
        {"[",                             "truncated array"},
        {R"({"names":["a",]})",            "trailing comma in array"},
        {R"({"names":["a",,"b"]})",        "double comma"},
        {R"({"names":["unterminated})",    "unterminated string"},
        {R"({"names":["bad\qescape"]})",   "unrecognised escape"},
        {R"({"names":"not an array"})",    "wrong type for names"},
        {R"({"packages":[{"name":5}]})",   "wrong type for package name"},
        {R"({"schema_version":"one"})",    "wrong type for schema_version"},
        {R"({"packages":[{"classes":[{"properties":[{"type":{"kind":"nope"}}]}]}]})",
                                           "unrecognised type kind"},
        {R"({} trailing)",                 "trailing content"},
        {R"({"a":1)",                      "unterminated object after member"},
        {R"({"names":[01]})",              "leading zero in number"},
        {R"({"names":[1.]})",              "missing fraction digits"},
        {R"({"names":[1e]})",              "missing exponent digits"},
        {"{'single':1}",                   "single-quoted key"},
        {R"({"x":tru})",                   "truncated literal"},
        {R"({"header":"not an object"})",  "wrong type for header"},
        {R"({"packages":[{"name":"p","classes":[{"size":"zz"}]}]})", "bad number"},
    };

    for (const auto& test : cases) {
        auto parsed = ParseJson(test.text);
        CHECK(!parsed.ok());
        if (parsed.ok()) {
            std::fprintf(stderr, "  expected failure for %s\n", test.what);
            continue;
        }
        // An error with no message is useless to whoever has to fix the file.
        CHECK(!parsed.error().message.empty());
        CHECK(parsed.error().offset <= std::string_view(test.text).size());
    }
}

void TestDepthCap() {
    // Must reject before the stack runs out.
    std::string deep;
    deep.reserve(kMaxJsonDepth * 40);
    const int levels = kMaxJsonDepth * 20;
    for (int i = 0; i < levels; ++i) deep += '[';
    for (int i = 0; i < levels; ++i) deep += ']';

    auto parsed = ParseJson(deep);
    CHECK(!parsed.ok());
    if (!parsed.ok())
        CHECK(parsed.error().message.find("nesting") != std::string::npos);

    // Nesting just inside the cap must still be accepted, or the cap is too aggressive
    // to represent legitimately nested container types.
    std::string shallow = R"({"packages":[{"name":"p","classes":[{"name":"c","path":"p.c",)"
                          R"("properties":[{"name":"x","type":)";
    const int nested = 8;
    for (int i = 0; i < nested; ++i) shallow += R"({"kind":"array","params":[)";
    shallow += R"({"kind":"int32"})";
    for (int i = 0; i < nested; ++i) shallow += "]}";
    shallow += "}]}]}]}";

    auto ok = ParseJson(shallow);
    CHECK(ok.ok());
    if (!ok.ok())
        std::fprintf(stderr, "  nested-but-legal parse failed: %s\n",
                     ok.error().message.c_str());
}

void TestEmptyDump() {
    const Dump empty;
    const std::string text = WriteJsonString(empty, true);

    auto parsed = ParseJson(text);
    CHECK(parsed.ok());
    if (parsed.ok()) {
        CHECK(parsed.value() == empty);
        CHECK(parsed.value().schema_version == kSchemaVersion);
        CHECK(parsed.value().packages.empty());
        CHECK(parsed.value().TotalProperties() == 0);
    }

    // Defaults that are not zero must survive being omitted from the output.
    CHECK(text.find("array_dim") == std::string::npos);
    auto minimal = ParseJson(R"({"packages":[{"name":"p","classes":[{"name":"c",)"
                             R"("path":"p.c","properties":[{"name":"x"}]}]}]})");
    CHECK(minimal.ok());
    if (minimal.ok()) {
        const Property& property =
            minimal.value().packages.at(0).classes.at(0).properties.at(0);
        CHECK(property.array_dim == 1);     // not 0
        CHECK(property.bit_index == -1);    // not 0
        CHECK(!property.is_bitfield);
    }
}

void TestFileRoundTrip() {
    const Dump original = MakeRichDump();
    const std::string path = "zircon_ir_roundtrip.json";

    std::string error;
    CHECK(WriteJsonFile(original, path, error, true));
    CHECK(error.empty());

    auto parsed = ReadJsonFile(path);
    CHECK(parsed.ok());
    if (parsed.ok()) CHECK(parsed.value() == original);

    auto missing = ReadJsonFile("zircon_no_such_file_here.json");
    CHECK(!missing.ok());

    std::remove(path.c_str());
}

} // namespace

int main() {
    TestTypeKindRoundTrip();
    TestDumpRoundTrip();
    TestHighBitFlags();
    TestStringEscaping();
    TestMalformedInput();
    TestDepthCap();
    TestEmptyDump();
    TestFileRoundTrip();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
