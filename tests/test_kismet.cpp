// Kismet decompiler tests. Bytecode is hand-built, so these run with no game installed.
//
// Unresolved references render as placeholders (`<field 0x...>`), which is deliberate:
// the assertions are about instruction decoding and structure, and a test that also
// needed a live name pool would be testing three things at once.

#include "engine/Kismet.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace zircon;
using namespace zircon::engine;

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

// A memory source that reads nothing. The decoder must still produce structurally correct
// output, with references rendered as placeholders and never invented.
class NoMemory final : public core::IMemorySource {
public:
    std::size_t Read(core::Address, void*, std::size_t) override { return 0; }
    std::span<const core::ModuleInfo> Modules() const override { return {}; }
    std::span<const core::RegionInfo> Regions() const override { return {}; }
    core::Capabilities Caps() const override { return {}; }
    std::string Describe() const override { return "none"; }
};

// Builder for readable test bytecode.
struct Code {
    std::vector<std::uint8_t> bytes;

    Code& Op(std::uint8_t opcode) { bytes.push_back(opcode); return *this; }
    Code& U8(std::uint8_t value)  { bytes.push_back(value); return *this; }

    template <typename T>
    Code& Raw(T value) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
        return *this;
    }

    Code& Ptr(std::uint64_t value) { return Raw<std::uint64_t>(value); }
    Code& I32(std::int32_t value)  { return Raw<std::int32_t>(value); }
    Code& U32(std::uint32_t value) { return Raw<std::uint32_t>(value); }

    Code& Ansi(const char* text) {
        while (*text) bytes.push_back(static_cast<std::uint8_t>(*text++));
        bytes.push_back(0);
        return *this;
    }
};

enum : std::uint8_t {
    EX_LocalVariable = 0x00, EX_InstanceVariable = 0x01, EX_Return = 0x04,
    EX_Jump = 0x06, EX_JumpIfNot = 0x07, EX_Nothing = 0x0B, EX_Let = 0x0F,
    EX_BitFieldConst = 0x10, EX_Unused11 = 0x11, EX_LetBool = 0x14,
    EX_EndFunctionParms = 0x16, EX_Self = 0x17, EX_Context = 0x19,
    EX_FinalFunction = 0x1C, EX_IntConst = 0x1D, EX_FloatConst = 0x1E,
    EX_StringConst = 0x1F, EX_True = 0x27, EX_False = 0x28, EX_TextConst = 0x29,
    EX_StructConst = 0x2F, EX_EndStructConst = 0x30, EX_SetArray = 0x31,
    EX_EndArray = 0x32, EX_DoubleConst = 0x37, EX_EndOfScript = 0x53,
    EX_Tracepoint = 0x5E, EX_CallMath = 0x68,
};

NoMemory g_memory;
ObjectArrayInfo g_array;
NamePoolInfo   g_pool;
UObjectLayout  g_object;
UStructLayout  g_struct;
FPropertyLayout g_property;
SubclassLayout g_subclass;
EngineProfile  g_profile;

ScriptLayout   g_script;

ResolveContext Context(int engine_major = 5) {
    g_profile.major = engine_major;
    g_profile.minor = 0;

    ResolveContext context;
    context.memory          = &g_memory;
    context.array           = &g_array;
    context.pool            = &g_pool;
    context.object_layout   = &g_object;
    context.struct_layout   = &g_struct;
    context.property_layout = &g_property;
    context.subclass_layout = &g_subclass;
    context.profile         = &g_profile;
    return context;
}

// What a licensee build looks like to the decompiler: a profile that knows nothing,
// and a script layout that measured the engine's FVector instead.
ResolveContext MeasuredContext(bool double_vectors) {
    g_profile.major = 0;            // no version string anywhere in the binary
    g_profile.minor = 0;

    g_script.script_array   = 0x20;
    g_script.double_vectors = double_vectors;

    ResolveContext context;
    context.memory          = &g_memory;
    context.array           = &g_array;
    context.pool            = &g_pool;
    context.object_layout   = &g_object;
    context.struct_layout   = &g_struct;
    context.property_layout = &g_property;
    context.subclass_layout = &g_subclass;
    context.profile         = &g_profile;
    context.script_layout   = &g_script;
    return context;
}

std::string Joined(const Decompiled& out) {
    std::string text;
    for (const auto& line : out.lines) { text += line.text; text += "\n"; }
    return text;
}

bool Contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- tests --------------------------------------------------------------------------

void TestEmptyAndTrivial() {
    const auto context = Context();

    const auto empty = DecompileBytecode(context, {});
    CHECK(empty.lines.empty());
    CHECK(empty.complete);
    CHECK(empty.bytes_total == 0);

    Code code;
    code.Op(EX_Return).Op(EX_Nothing).Op(EX_EndOfScript);
    const auto out = DecompileBytecode(context, code.bytes);
    CHECK(out.complete);
    CHECK(Contains(Joined(out), "return;"));
}

void TestAssignmentAndConstants() {
    const auto context = Context();

    Code code;
    // Let(property, lhs, rhs) with an int constant.
    code.Op(EX_Let).Ptr(0x1111).Op(EX_LocalVariable).Ptr(0x2222).Op(EX_IntConst).I32(42);
    code.Op(EX_LetBool).Op(EX_InstanceVariable).Ptr(0x3333).Op(EX_True);
    code.Op(EX_Let).Ptr(0x4444).Op(EX_LocalVariable).Ptr(0x5555)
        .Op(EX_StringConst).Ansi("hello");
    code.Op(EX_Let).Ptr(0x6666).Op(EX_LocalVariable).Ptr(0x7777)
        .Op(EX_FloatConst).Raw<float>(1.5f);
    code.Op(EX_EndOfScript);

    const auto out = DecompileBytecode(context, code.bytes);
    const std::string text = Joined(out);

    CHECK(out.complete);
    CHECK(Contains(text, "= 42;"));
    CHECK(Contains(text, "= true;"));
    CHECK(Contains(text, "\"hello\""));
    CHECK(Contains(text, "1.5f"));
}

// A measured layout must beat the version number, because on a licensee build there is no
// version number to beat.
//
// The bug that testing a second game turned up. RV There Yet publishes
// "++RideGamejam+rel-1.2" and no UE version at all, so profile.major was 0, so the
// decompiler read three floats where the engine had written three doubles. It did not
// fail: it desynchronised and produced calls with the wrong arguments for 316 functions
// before giving up somewhere downstream. Silent corruption, which is the one outcome this
// project refuses.
void TestVectorPrecisionComesFromMeasurement() {
    constexpr std::uint8_t EX_VectorConst = 0x23;

    // Doubles on the wire, no version to infer from, layout says double: decoded.
    {
        Code code;
        code.Op(EX_Return).Op(EX_VectorConst)
            .Raw<double>(4.0).Raw<double>(5.0).Raw<double>(6.0);
        code.Op(EX_EndOfScript);

        const auto out = DecompileBytecode(MeasuredContext(true), code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "FVector(4, 5, 6)"));
    }

    // Floats on the wire, no version, layout says float: decoded.
    {
        Code code;
        code.Op(EX_Return).Op(EX_VectorConst)
            .Raw<float>(4.0f).Raw<float>(5.0f).Raw<float>(6.0f);
        code.Op(EX_EndOfScript);

        const auto out = DecompileBytecode(MeasuredContext(false), code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "FVector(4, 5, 6)"));
    }

    // The regression itself: doubles on the wire, no version. Without the measurement the
    // profile's 0 would select floats and the value would come back wrong while still
    // claiming to have decoded. The measurement has to be what decides.
    {
        Code code;
        code.Op(EX_Return).Op(EX_VectorConst)
            .Raw<double>(4.0).Raw<double>(5.0).Raw<double>(6.0);
        code.Op(EX_EndOfScript);

        const auto measured = DecompileBytecode(MeasuredContext(true), code.bytes);
        CHECK(Contains(Joined(measured), "FVector(4, 5, 6)"));

        // Same bytes, same absent version, but no layout to consult: the old behaviour,
        // kept only as a fallback, and demonstrably not good enough on its own.
        auto guessing = Context(0);
        const auto guessed = DecompileBytecode(guessing, code.bytes);
        CHECK(!Contains(Joined(guessed), "FVector(4, 5, 6)"));
    }
}

void TestVectorWidthFollowsEngineVersion() {
    // EX_VectorConst is three floats on UE4 and three doubles on UE5. Reading the wrong
    // width does not merely mangle the constant, it desynchronises everything after it,
    // so this is the one place the engine version genuinely changes decoding.
    constexpr std::uint8_t EX_VectorConst = 0x23;

    {
        Code code;
        code.Op(EX_Return).Op(EX_VectorConst)
            .Raw<double>(1.0).Raw<double>(2.0).Raw<double>(3.0);
        code.Op(EX_EndOfScript);

        const auto out = DecompileBytecode(Context(5), code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "FVector(1, 2, 3)"));
    }
    {
        Code code;
        code.Op(EX_Return).Op(EX_VectorConst)
            .Raw<float>(1.0f).Raw<float>(2.0f).Raw<float>(3.0f);
        code.Op(EX_EndOfScript);

        const auto out = DecompileBytecode(Context(4), code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "FVector(1, 2, 3)"));
    }
    {
        // Decoding UE4 bytecode as UE5 must NOT quietly succeed: reading 24 bytes where
        // 12 were written runs off the end, which is exactly the failure the reader is
        // there to turn into an honest stop instead of into fiction.
        Code code;
        code.Op(EX_Return).Op(EX_VectorConst)
            .Raw<float>(1.0f).Raw<float>(2.0f).Raw<float>(3.0f);
        code.Op(EX_EndOfScript);

        const auto out = DecompileBytecode(Context(5), code.bytes);
        CHECK(!out.complete);
    }
}

void TestCallsAndContext() {
    const auto context = Context();

    Code code;
    // this.Foo(1, 2)
    code.Op(EX_Context)
        .Op(EX_Self)
        .U32(0)                       // skip offset
        .Ptr(0)                       // r-value property
        .Op(EX_FinalFunction).Ptr(0xAAAA)
            .Op(EX_IntConst).I32(1)
            .Op(EX_IntConst).I32(2)
            .Op(EX_EndFunctionParms);
    code.Op(EX_EndOfScript);

    const auto out = DecompileBytecode(context, code.bytes);
    const std::string text = Joined(out);
    CHECK(out.complete);
    CHECK(Contains(text, "this."));
    CHECK(Contains(text, "(1, 2)"));

    // A call used purely for its side effect is a statement, not an expression; the
    // statement decoder has to fall back to the expression decoder for it.
    Code bare;
    bare.Op(EX_CallMath).Ptr(0xBBBB).Op(EX_EndFunctionParms).Op(EX_EndOfScript);
    const auto side_effect = DecompileBytecode(context, bare.bytes);
    CHECK(side_effect.complete);
    CHECK(Contains(Joined(side_effect), "()"));
}

void TestControlFlowLabels() {
    const auto context = Context();

    Code code;
    code.Op(EX_JumpIfNot).U32(0x0010).Op(EX_True);   // at 0x00
    while (code.bytes.size() < 0x10) code.Op(EX_Tracepoint);
    code.Op(EX_Return).Op(EX_Nothing);               // at 0x10
    code.Op(EX_EndOfScript);

    const auto out = DecompileBytecode(context, code.bytes);
    const std::string text = Joined(out);

    CHECK(out.complete);
    CHECK(Contains(text, "goto Label_0010;"));
    // A jump target must get a label, or the goto points at nothing a reader can find.
    CHECK(Contains(text, "Label_0010:"));
}

void TestStructConstAndBitfields() {
    const auto context = Context();

    // A struct literal whose members are packed bools. This is the shape that exposed
    // opcode 0x11: it appears exactly where a bitfield constant belongs, and decoding it
    // with BitFieldConst's operands is what makes the stream stay in sync.
    Code code;
    code.Op(EX_Return).Op(EX_StructConst).Ptr(0xCCCC).I32(3)
        .Op(EX_Unused11).Ptr(0xD001).U8(1)
        .Op(EX_Unused11).Ptr(0xD002).U8(0)
        .Op(EX_BitFieldConst).Ptr(0xD003).U8(1)
        .Op(EX_EndStructConst);
    code.Op(EX_EndOfScript);

    const auto out = DecompileBytecode(context, code.bytes);
    const std::string text = Joined(out);

    CHECK(out.complete);
    CHECK(Contains(text, "true"));
    CHECK(Contains(text, "false"));
    // Three members must render, with the terminator consumed and not treated as one.
    CHECK(!Contains(text, "EndStructConst"));
}

void TestTextConstForms() {
    const auto context = Context();

    {
        Code code;
        code.Op(EX_Return).Op(EX_TextConst).U8(0);              // Empty
        code.Op(EX_EndOfScript);
        const auto out = DecompileBytecode(context, code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "FText()"));
    }
    {
        Code code;
        code.Op(EX_Return).Op(EX_TextConst).U8(3)              // LiteralString
            .Op(EX_StringConst).Ansi("hi");
        code.Op(EX_EndOfScript);
        const auto out = DecompileBytecode(context, code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "FText::FromString(\"hi\")"));
    }
    {
        Code code;
        code.Op(EX_Return).Op(EX_TextConst).U8(1)              // LocalizedText
            .Op(EX_StringConst).Ansi("source")
            .Op(EX_StringConst).Ansi("ns")
            .Op(EX_StringConst).Ansi("key");
        code.Op(EX_EndOfScript);
        const auto out = DecompileBytecode(context, code.bytes);
        CHECK(out.complete);
        CHECK(Contains(Joined(out), "NSLOCTEXT"));
    }
    {
        // An unrecognised literal type must stop. Guessing a payload length is fiction.
        Code code;
        code.Op(EX_Return).Op(EX_TextConst).U8(99);
        code.Op(EX_EndOfScript);
        const auto out = DecompileBytecode(context, code.bytes);
        CHECK(!out.complete);
    }
}

void TestSetArray() {
    const auto context = Context();

    Code code;
    code.Op(EX_SetArray).Op(EX_LocalVariable).Ptr(0x1)
        .Op(EX_IntConst).I32(7).Op(EX_IntConst).I32(8).Op(EX_EndArray);
    code.Op(EX_EndOfScript);

    const auto out = DecompileBytecode(context, code.bytes);
    CHECK(out.complete);
    CHECK(Contains(Joined(out), "{7, 8}"));
}

void TestTruncatedInputStopsHonestly() {
    const auto context = Context();

    // An operand cut short must produce an incomplete result with a reason, never a line
    // built from whatever followed in memory.
    Code code;
    code.Op(EX_Return).Op(EX_IntConst).U8(1).U8(2);   // two bytes of a four-byte int
    const auto out = DecompileBytecode(context, code.bytes);

    CHECK(!out.complete);
    CHECK(!out.stop_reason.empty());
    CHECK(out.bytes_decoded <= out.bytes_total);
}

void TestUnknownOpcodeStopsAndIsReported() {
    const auto context = Context();

    Code code;
    code.Op(EX_Tracepoint).Op(0xF3);   // 0xF3 is not an opcode in any version
    const auto out = DecompileBytecode(context, code.bytes);

    CHECK(!out.complete);
    CHECK(!out.unknown_opcodes.empty());
    if (!out.unknown_opcodes.empty()) CHECK(out.unknown_opcodes[0] == 0xF3);
    // Past an unknown operand length every later byte is misread, so nothing may be
    // emitted after the stop.
    CHECK(Contains(out.stop_reason, "0xF3"));
}

void TestNoRunawayOnHostileInput() {
    const auto context = Context();

    // A stream that is all context opcodes would recurse forever without a depth cap.
    std::vector<std::uint8_t> nested(4096, 0x19);   // EX_Context
    const auto out = DecompileBytecode(context, nested);
    CHECK(!out.complete);

    // Unterminated parameter lists and struct literals must terminate too.
    std::vector<std::uint8_t> unterminated(2048, 0x1D);   // EX_IntConst, never ends
    const auto second = DecompileBytecode(context, unterminated);
    CHECK(second.bytes_decoded <= second.bytes_total);
}

void TestOpcodeNames() {
    CHECK(OpcodeName(0x04) == "Return");
    CHECK(OpcodeName(0x53) == "EndOfScript");
    CHECK(OpcodeName(0x68) == "CallMath");
    CHECK(OpcodeName(0xF3) == "Unknown");
}

} // namespace

int main() {
    TestVectorPrecisionComesFromMeasurement();
    TestEmptyAndTrivial();
    TestAssignmentAndConstants();
    TestVectorWidthFollowsEngineVersion();
    TestCallsAndContext();
    TestControlFlowLabels();
    TestStructConstAndBitfields();
    TestTextConstForms();
    TestSetArray();
    TestTruncatedInputStopsHonestly();
    TestUnknownOpcodeStopsAndIsReported();
    TestNoRunawayOnHostileInput();
    TestOpcodeNames();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
