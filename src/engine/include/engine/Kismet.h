#pragma once

// Kismet bytecode: disassembly and decompilation.
//
// UE compiles Blueprints to a stack machine whose instruction stream is already a tree:
// most opcodes consume sub-expressions inline rather than operands from a stack. That
// means "disassembly" naturally yields an expression tree, and rendering it as pseudo-C++
// is a matter of formatting rather than of control-flow recovery — except for jumps,
// which are handled separately.
//
// The governing rule, as everywhere else in Zircon: never emit plausible-but-wrong
// output. An opcode this build does not understand stops the walk and marks the function
// incomplete, because past an unknown operand length every subsequent byte is misread and
// the result would look like valid code while being fiction.

#include "core/MemorySource.h"
#include "engine/TypeResolver.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zircon::engine {

// UStruct::Script, a TArray<uint8>. Present on every UStruct but only non-empty on
// UFunctions that carry Blueprint code.
struct ScriptLayout {
    int script_array{-1};

    // Whether the bytecode's vector, rotator and transform constants carry doubles.
    //
    // UE5's Large World Coordinates widened FVector from three floats to three doubles,
    // and EX_VectorConst with it. Get this wrong and the instruction stream desyncs at the
    // first vector literal: every later operand is read at the wrong offset, so the
    // decompiler does not fail loudly, it produces a call with the wrong arguments and
    // then gives up somewhere downstream.
    //
    // This used to be decided by the engine's major version, which is exactly the thing a
    // licensee build does not publish — and a game that renames its version string got
    // float widths on a double-precision engine. It is now measured; see
    // DeriveScriptLayout.
    bool double_vectors{true};

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return script_array >= 0; }
};

ScriptLayout DeriveScriptLayout(core::IMemorySource& memory,
                                const ObjectArrayInfo& array,
                                const NamePoolInfo& pool,
                                const UObjectLayout& object_layout,
                                const UStructLayout& struct_layout);

std::vector<std::uint8_t> GetScriptBytes(core::IMemorySource& memory,
                                         const ScriptLayout& layout,
                                         core::Address function);

// One rendered statement, tagged with the bytecode offset it started at so output can be
// cross-referenced against a raw disassembly.
struct ScriptLine {
    std::uint32_t offset{};
    int           depth{};     // indentation, from context and jump structure
    std::string   text;
};

struct Decompiled {
    std::vector<ScriptLine> lines;

    std::uint32_t bytes_total{};
    std::uint32_t bytes_decoded{};

    // False when an opcode was not understood. The lines produced before that point are
    // still correct; everything after is unknown, and is deliberately not guessed at.
    bool complete{true};
    std::string stop_reason;

    // Opcodes encountered that this build does not decode, for reporting rather than for
    // the reader.
    std::vector<std::uint8_t> unknown_opcodes;
};

// Renders a UFunction's bytecode. `function` is the UFunction object; names of referenced
// objects and properties are resolved through the context.
Decompiled DecompileFunction(const ResolveContext& context, const ScriptLayout& layout,
                             core::Address function);

// The same decoder over a caller-supplied buffer. Separated so the decoder can be tested
// against hand-built bytecode without a process, a name pool or an object array — the
// structure of the output is asserted, and unresolved references simply render as
// placeholders.
Decompiled DecompileBytecode(const ResolveContext& context,
                             const std::vector<std::uint8_t>& code);

// Raw opcode names, for diagnostics and for a disassembly listing.
std::string_view OpcodeName(std::uint8_t opcode);

} // namespace zircon::engine
