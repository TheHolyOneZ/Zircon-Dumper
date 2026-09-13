#include "engine/Kismet.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>
#include <cstring>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// EExprToken. These values are part of the serialized bytecode and stable for a given
// engine generation. Entries added later just don't appear in older streams.
enum Op : std::uint8_t {
    EX_LocalVariable = 0x00, EX_InstanceVariable = 0x01, EX_DefaultVariable = 0x02,
    EX_Return = 0x04, EX_Jump = 0x06, EX_JumpIfNot = 0x07, EX_Assert = 0x09,
    EX_Nothing = 0x0B, EX_NothingInt32 = 0x0C, EX_Let = 0x0F, EX_BitFieldConst = 0x10,
    EX_ClassContext = 0x12, EX_MetaCast = 0x13, EX_LetBool = 0x14,
    EX_EndParmValue = 0x15, EX_EndFunctionParms = 0x16, EX_Self = 0x17, EX_Skip = 0x18,
    EX_Context = 0x19, EX_Context_FailSilent = 0x1A, EX_VirtualFunction = 0x1B,
    EX_FinalFunction = 0x1C, EX_IntConst = 0x1D, EX_FloatConst = 0x1E,
    EX_StringConst = 0x1F, EX_ObjectConst = 0x20, EX_NameConst = 0x21,
    EX_RotationConst = 0x22, EX_VectorConst = 0x23, EX_ByteConst = 0x24,
    EX_IntZero = 0x25, EX_IntOne = 0x26, EX_True = 0x27, EX_False = 0x28,
    EX_TextConst = 0x29, EX_NoObject = 0x2A, EX_TransformConst = 0x2B,
    EX_IntConstByte = 0x2C, EX_NoInterface = 0x2D, EX_DynamicCast = 0x2E,
    EX_StructConst = 0x2F, EX_EndStructConst = 0x30, EX_SetArray = 0x31,
    EX_EndArray = 0x32, EX_PropertyConst = 0x33, EX_UnicodeStringConst = 0x34,
    EX_Int64Const = 0x35, EX_UInt64Const = 0x36, EX_DoubleConst = 0x37, EX_Cast = 0x38,
    EX_SetSet = 0x39, EX_EndSet = 0x3A, EX_SetMap = 0x3B, EX_EndMap = 0x3C,
    EX_SetConst = 0x3D, EX_EndSetConst = 0x3E, EX_MapConst = 0x3F, EX_EndMapConst = 0x40,
    EX_Vector3fConst = 0x41, EX_StructMemberContext = 0x42,
    EX_LetMulticastDelegate = 0x43, EX_LetDelegate = 0x44,
    EX_LocalVirtualFunction = 0x45, EX_LocalFinalFunction = 0x46,
    EX_LocalOutVariable = 0x48, EX_DeprecatedOp4A = 0x4A, EX_InstanceDelegate = 0x4B,
    EX_PushExecutionFlow = 0x4C, EX_PopExecutionFlow = 0x4D, EX_ComputedJump = 0x4E,
    EX_PopExecutionFlowIfNot = 0x4F, EX_Breakpoint = 0x50, EX_InterfaceContext = 0x51,
    EX_ObjToInterfaceCast = 0x52, EX_EndOfScript = 0x53, EX_CrossInterfaceCast = 0x54,
    EX_InterfaceToObjCast = 0x55, EX_WireTracepoint = 0x5A, EX_SkipOffsetConst = 0x5B,
    EX_AddMulticastDelegate = 0x5C, EX_ClearMulticastDelegate = 0x5D,
    EX_Tracepoint = 0x5E, EX_LetObj = 0x5F, EX_LetWeakObjPtr = 0x60,
    EX_BindDelegate = 0x61, EX_RemoveMulticastDelegate = 0x62,
    EX_CallMulticastDelegate = 0x63, EX_LetValueOnPersistentFrame = 0x64,
    EX_ArrayConst = 0x65, EX_EndArrayConst = 0x66, EX_SoftObjectConst = 0x67,
    EX_CallMath = 0x68, EX_SwitchValue = 0x69, EX_InstrumentationEvent = 0x6A,
    EX_ArrayGetByRef = 0x6B, EX_ClassSparseDataVariable = 0x6C, EX_FieldPathConst = 0x6D,
    EX_AutoRtfmTransact = 0x6E, EX_AutoRtfmStopTransact = 0x6F,
    EX_AutoRtfmAbortIfNot = 0x70,
};

const std::map<std::uint8_t, std::string_view>& OpcodeNames() {
    static const std::map<std::uint8_t, std::string_view> names = {
        {EX_LocalVariable, "LocalVariable"}, {EX_InstanceVariable, "InstanceVariable"},
        {EX_DefaultVariable, "DefaultVariable"}, {EX_Return, "Return"},
        {EX_Jump, "Jump"}, {EX_JumpIfNot, "JumpIfNot"}, {EX_Assert, "Assert"},
        {EX_Nothing, "Nothing"}, {EX_NothingInt32, "NothingInt32"}, {EX_Let, "Let"},
        {EX_BitFieldConst, "BitFieldConst"}, {EX_ClassContext, "ClassContext"},
        {EX_MetaCast, "MetaCast"}, {EX_LetBool, "LetBool"},
        {EX_EndParmValue, "EndParmValue"}, {EX_EndFunctionParms, "EndFunctionParms"},
        {EX_Self, "Self"}, {EX_Skip, "Skip"}, {EX_Context, "Context"},
        {EX_Context_FailSilent, "Context_FailSilent"},
        {EX_VirtualFunction, "VirtualFunction"}, {EX_FinalFunction, "FinalFunction"},
        {EX_IntConst, "IntConst"}, {EX_FloatConst, "FloatConst"},
        {EX_StringConst, "StringConst"}, {EX_ObjectConst, "ObjectConst"},
        {EX_NameConst, "NameConst"}, {EX_RotationConst, "RotationConst"},
        {EX_VectorConst, "VectorConst"}, {EX_ByteConst, "ByteConst"},
        {EX_IntZero, "IntZero"}, {EX_IntOne, "IntOne"}, {EX_True, "True"},
        {EX_False, "False"}, {EX_TextConst, "TextConst"}, {EX_NoObject, "NoObject"},
        {EX_TransformConst, "TransformConst"}, {EX_IntConstByte, "IntConstByte"},
        {EX_NoInterface, "NoInterface"}, {EX_DynamicCast, "DynamicCast"},
        {EX_StructConst, "StructConst"}, {EX_EndStructConst, "EndStructConst"},
        {EX_SetArray, "SetArray"}, {EX_EndArray, "EndArray"},
        {EX_PropertyConst, "PropertyConst"},
        {EX_UnicodeStringConst, "UnicodeStringConst"}, {EX_Int64Const, "Int64Const"},
        {EX_UInt64Const, "UInt64Const"}, {EX_DoubleConst, "DoubleConst"},
        {EX_Cast, "Cast"}, {EX_SetSet, "SetSet"}, {EX_EndSet, "EndSet"},
        {EX_SetMap, "SetMap"}, {EX_EndMap, "EndMap"}, {EX_SetConst, "SetConst"},
        {EX_EndSetConst, "EndSetConst"}, {EX_MapConst, "MapConst"},
        {EX_EndMapConst, "EndMapConst"}, {EX_Vector3fConst, "Vector3fConst"},
        {EX_StructMemberContext, "StructMemberContext"},
        {EX_LetMulticastDelegate, "LetMulticastDelegate"},
        {EX_LetDelegate, "LetDelegate"},
        {EX_LocalVirtualFunction, "LocalVirtualFunction"},
        {EX_LocalFinalFunction, "LocalFinalFunction"},
        {EX_LocalOutVariable, "LocalOutVariable"},
        {EX_DeprecatedOp4A, "DeprecatedOp4A"}, {EX_InstanceDelegate, "InstanceDelegate"},
        {EX_PushExecutionFlow, "PushExecutionFlow"},
        {EX_PopExecutionFlow, "PopExecutionFlow"}, {EX_ComputedJump, "ComputedJump"},
        {EX_PopExecutionFlowIfNot, "PopExecutionFlowIfNot"},
        {EX_Breakpoint, "Breakpoint"}, {EX_InterfaceContext, "InterfaceContext"},
        {EX_ObjToInterfaceCast, "ObjToInterfaceCast"}, {EX_EndOfScript, "EndOfScript"},
        {EX_CrossInterfaceCast, "CrossInterfaceCast"},
        {EX_InterfaceToObjCast, "InterfaceToObjCast"},
        {EX_WireTracepoint, "WireTracepoint"}, {EX_SkipOffsetConst, "SkipOffsetConst"},
        {EX_AddMulticastDelegate, "AddMulticastDelegate"},
        {EX_ClearMulticastDelegate, "ClearMulticastDelegate"},
        {EX_Tracepoint, "Tracepoint"}, {EX_LetObj, "LetObj"},
        {EX_LetWeakObjPtr, "LetWeakObjPtr"}, {EX_BindDelegate, "BindDelegate"},
        {EX_RemoveMulticastDelegate, "RemoveMulticastDelegate"},
        {EX_CallMulticastDelegate, "CallMulticastDelegate"},
        {EX_LetValueOnPersistentFrame, "LetValueOnPersistentFrame"},
        {EX_ArrayConst, "ArrayConst"}, {EX_EndArrayConst, "EndArrayConst"},
        {EX_SoftObjectConst, "SoftObjectConst"}, {EX_CallMath, "CallMath"},
        {EX_SwitchValue, "SwitchValue"},
        {EX_InstrumentationEvent, "InstrumentationEvent"},
        {EX_ArrayGetByRef, "ArrayGetByRef"},
        {EX_ClassSparseDataVariable, "ClassSparseDataVariable"},
        {EX_FieldPathConst, "FieldPathConst"},
        {EX_AutoRtfmTransact, "AutoRtfmTransact"},
        {EX_AutoRtfmStopTransact, "AutoRtfmStopTransact"},
        {EX_AutoRtfmAbortIfNot, "AutoRtfmAbortIfNot"},
    };
    return names;
}

// Bounds-checked cursor over the bytecode. Every read goes through it, so running off the
// end sets a failure flag instead of reading adjacent memory. Which matters: a misdecoded
// operand length is exactly how a decompiler starts producing fiction.
class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& code) : code_(code) {}

    bool Ok() const { return failure_.empty(); }
    const std::string& Failure() const { return failure_; }
    std::uint32_t Pos() const { return pos_; }
    bool AtEnd() const { return pos_ >= code_.size(); }
    std::size_t Size() const { return code_.size(); }

    void Fail(std::string why) {
        if (failure_.empty()) failure_ = std::move(why);
    }

    // Only ever steps back to a position already read. That's the one-byte lookahead and
    // the statement/expression fallback.
    void Seek(std::uint32_t pos) {
        if (pos > code_.size()) { Fail("seek past the end of the bytecode"); return; }
        pos_ = pos;
    }

    std::uint8_t Byte() {
        if (pos_ >= code_.size()) { Fail("ran past the end of the bytecode"); return 0; }
        return code_[pos_++];
    }

    template <typename T>
    T Read() {
        static_assert(std::is_trivially_copyable_v<T>);
        if (pos_ + sizeof(T) > code_.size()) {
            Fail("ran past the end of the bytecode");
            pos_ = static_cast<std::uint32_t>(code_.size());
            return T{};
        }
        T value{};
        std::memcpy(&value, code_.data() + pos_, sizeof(T));
        pos_ += static_cast<std::uint32_t>(sizeof(T));
        return value;
    }

    std::string AnsiString() {
        std::string out;
        while (pos_ < code_.size()) {
            const char c = static_cast<char>(code_[pos_++]);
            if (c == '\0') return out;
            out.push_back(c);
        }
        Fail("unterminated string constant");
        return out;
    }

    std::string WideString() {
        std::string out;
        while (pos_ + 1 < code_.size()) {
            std::uint16_t c{};
            std::memcpy(&c, code_.data() + pos_, 2);
            pos_ += 2;
            if (c == 0) return out;
            // Overwhelmingly ASCII. Escape the rest; don't guess an encoding.
            if (c < 0x80) out.push_back(static_cast<char>(c));
            else out += std::format("\\u{:04x}", c);
        }
        Fail("unterminated wide string constant");
        return out;
    }

private:
    const std::vector<std::uint8_t>& code_;
    std::uint32_t pos_{0};
    std::string failure_;
};

std::string Quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

std::string Leaf(const std::string& path) {
    const auto dot = path.find_last_of('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}

class Decompiler {
public:
    Decompiler(const ResolveContext& context, Reader& reader, Decompiled& out)
        : context_(context), reader_(reader), out_(out) {
        // EX_VectorConst carries three floats on UE4 and three doubles on UE5. The wrong
        // width doesn't merely mangle the constant, it desynchronises every instruction
        // after it.
        //
        // Measured from the engine's own FVector, never inferred from a version number. The
        // profile is a fallback for callers that supplied no script layout at all.
        doubles_ = context.script_layout
                     ? context.script_layout->double_vectors
                     : (!context.profile || context.profile->major >= 5);
    }

    // Renders one expression, consuming exactly its operands.
    std::string Expr(int depth = 0) {
        if (!reader_.Ok()) return "<error>";
        if (depth > kMaxDepth) {
            reader_.Fail("expression nesting is implausibly deep");
            return "<error>";
        }
        if (reader_.AtEnd()) {
            reader_.Fail("expression ran past the end of the bytecode");
            return "<error>";
        }

        const std::uint8_t opcode = reader_.Byte();
        switch (opcode) {
            // --- variables ------------------------------------------------------------
            case EX_LocalVariable:
            case EX_InstanceVariable:
            case EX_DefaultVariable:
            case EX_LocalOutVariable:
            case EX_ClassSparseDataVariable:
                return PropertyName(reader_.Read<std::uint64_t>());

            case EX_Self:        return "this";
            case EX_Nothing:     return "";
            case EX_NothingInt32: reader_.Read<std::int32_t>(); return "";
            case EX_NoObject:    return "nullptr";
            case EX_NoInterface: return "nullptr";
            case EX_True:        return "true";
            case EX_False:       return "false";
            case EX_IntZero:     return "0";
            case EX_IntOne:      return "1";

            // --- constants ------------------------------------------------------------
            case EX_IntConst:     return std::to_string(reader_.Read<std::int32_t>());
            case EX_Int64Const:   return std::to_string(reader_.Read<std::int64_t>());
            case EX_UInt64Const:  return std::to_string(reader_.Read<std::uint64_t>());
            case EX_ByteConst:
            case EX_IntConstByte: return std::to_string(static_cast<int>(reader_.Byte()));
            case EX_FloatConst:   return std::format("{}f", reader_.Read<float>());
            case EX_DoubleConst:  return std::format("{}", reader_.Read<double>());
            case EX_SkipOffsetConst:
                return std::format("0x{:X}", reader_.Read<std::uint32_t>());

            case EX_StringConst:        return Quote(reader_.AnsiString());
            case EX_UnicodeStringConst: return Quote(reader_.WideString());
            case EX_NameConst:          return std::format("FName({})", ScriptName());
            case EX_ObjectConst:        return ObjectName(reader_.Read<std::uint64_t>());
            case EX_PropertyConst:      return PropertyName(reader_.Read<std::uint64_t>());

            case EX_SoftObjectConst:    return std::format("SoftObject({})", Expr(depth + 1));

            case EX_TextConst: {
                // EBlueprintTextLiteralType picks the payload, and each payload string is
                // itself a nested string expression.
                enum : std::uint8_t {
                    kEmpty = 0, kLocalizedText = 1, kInvariantText = 2,
                    kLiteralString = 3, kStringTableEntry = 4,
                };
                const std::uint8_t kind = reader_.Byte();
                switch (kind) {
                    case kEmpty:
                        return "FText()";
                    case kLocalizedText: {
                        const std::string source = Expr(depth + 1);
                        const std::string ns     = Expr(depth + 1);
                        const std::string key    = Expr(depth + 1);
                        return std::format("NSLOCTEXT({}, {}, {})", ns, key, source);
                    }
                    case kInvariantText:
                        return std::format("FText::AsCultureInvariant({})", Expr(depth + 1));
                    case kLiteralString:
                        return std::format("FText::FromString({})", Expr(depth + 1));
                    case kStringTableEntry: {
                        ObjectName(reader_.Read<std::uint64_t>());   // string table asset
                        const std::string table = Expr(depth + 1);
                        const std::string key   = Expr(depth + 1);
                        return std::format("FText::FromStringTable({}, {})", table, key);
                    }
                    default:
                        reader_.Fail(std::format("unknown text literal type {}", kind));
                        return "<error>";
                }
            }

            case EX_FieldPathConst:
                return std::format("FieldPath({})", Expr(depth + 1));

            case EX_BitFieldConst:
            case 0x11: {
                // The published EExprToken lists 0x11 as unused, but this build emits it
                // exactly where a packed bool constant belongs inside a struct literal.
                // Decoding it with BitFieldConst's operands (FProperty*, then a byte) makes
                // every affected function decode cleanly to its end. Observation, not
                // assumption.
                const std::string property = PropertyName(reader_.Read<std::uint64_t>());
                const int value = static_cast<int>(reader_.Byte());
                return value ? "true" : "false";
            }

            case EX_VectorConst: {
                // Three components; width per engine generation.
                if (doubles_) {
                    const double x = reader_.Read<double>();
                    const double y = reader_.Read<double>();
                    const double z = reader_.Read<double>();
                    return std::format("FVector({}, {}, {})", x, y, z);
                }
                const float x = reader_.Read<float>();
                const float y = reader_.Read<float>();
                const float z = reader_.Read<float>();
                return std::format("FVector({}, {}, {})", x, y, z);
            }
            case EX_Vector3fConst: {
                const float x = reader_.Read<float>();
                const float y = reader_.Read<float>();
                const float z = reader_.Read<float>();
                return std::format("FVector3f({}, {}, {})", x, y, z);
            }
            case EX_RotationConst: {
                if (doubles_) {
                    const double p = reader_.Read<double>();
                    const double y = reader_.Read<double>();
                    const double r = reader_.Read<double>();
                    return std::format("FRotator({}, {}, {})", p, y, r);
                }
                const float p = reader_.Read<float>();
                const float y = reader_.Read<float>();
                const float r = reader_.Read<float>();
                return std::format("FRotator({}, {}, {})", p, y, r);
            }
            case EX_TransformConst: {
                // Rotation (4), translation (3), scale (3).
                const int components = 10;
                for (int i = 0; i < components; ++i) {
                    if (doubles_) reader_.Read<double>(); else reader_.Read<float>();
                }
                return "FTransform(...)";
            }

            case EX_StructConst: {
                const std::string name = ObjectName(reader_.Read<std::uint64_t>());
                reader_.Read<std::int32_t>();          // serialized size, not needed
                std::string out = Leaf(name) + "{";
                bool first = true;
                while (reader_.Ok() && Peek() != EX_EndStructConst) {
                    if (!first) out += ", ";
                    first = false;
                    out += Expr(depth + 1);
                }
                reader_.Byte();                        // EX_EndStructConst
                return out + "}";
            }

            case EX_ArrayConst: {
                PropertyName(reader_.Read<std::uint64_t>());
                reader_.Read<std::int32_t>();          // element count
                std::string out = "{";
                bool first = true;
                while (reader_.Ok() && Peek() != EX_EndArrayConst) {
                    if (!first) out += ", ";
                    first = false;
                    out += Expr(depth + 1);
                }
                reader_.Byte();
                return out + "}";
            }

            case EX_SetConst: {
                PropertyName(reader_.Read<std::uint64_t>());
                reader_.Read<std::int32_t>();
                std::string out = "TSet{";
                bool first = true;
                while (reader_.Ok() && Peek() != EX_EndSetConst) {
                    if (!first) out += ", ";
                    first = false;
                    out += Expr(depth + 1);
                }
                reader_.Byte();
                return out + "}";
            }

            case EX_MapConst: {
                PropertyName(reader_.Read<std::uint64_t>());   // key property
                PropertyName(reader_.Read<std::uint64_t>());   // value property
                reader_.Read<std::int32_t>();
                std::string out = "TMap{";
                bool first = true;
                while (reader_.Ok() && Peek() != EX_EndMapConst) {
                    if (!first) out += ", ";
                    first = false;
                    const std::string key = Expr(depth + 1);
                    const std::string value = Expr(depth + 1);
                    out += std::format("{}: {}", key, value);
                }
                reader_.Byte();
                return out + "}";
            }

            // --- calls ----------------------------------------------------------------
            case EX_FinalFunction:
            case EX_LocalFinalFunction:
            case EX_CallMath: {
                const std::string name = ObjectName(reader_.Read<std::uint64_t>());
                return std::format("{}({})", Leaf(name), Params(depth));
            }
            case EX_VirtualFunction:
            case EX_LocalVirtualFunction: {
                const std::string name = ScriptName();
                return std::format("{}({})", name, Params(depth));
            }
            case EX_CallMulticastDelegate: {
                ObjectName(reader_.Read<std::uint64_t>());     // signature function
                const std::string target = Expr(depth + 1);
                return std::format("{}.Broadcast({})", target, Params(depth));
            }

            // --- context --------------------------------------------------------------
            case EX_Context:
            case EX_Context_FailSilent:
            case EX_ClassContext: {
                const std::string target = Expr(depth + 1);
                reader_.Read<std::uint32_t>();                 // skip offset if null
                PropertyName(reader_.Read<std::uint64_t>());   // r-value property
                const std::string member = Expr(depth + 1);
                const char* arrow = opcode == EX_Context_FailSilent ? "?." : ".";
                return std::format("{}{}{}", target, arrow, member);
            }
            case EX_InterfaceContext:
                return Expr(depth + 1);
            case EX_StructMemberContext: {
                const std::string member = PropertyName(reader_.Read<std::uint64_t>());
                const std::string owner  = Expr(depth + 1);
                return std::format("{}.{}", owner, member);
            }

            // --- casts ----------------------------------------------------------------
            case EX_DynamicCast:
            case EX_MetaCast: {
                const std::string target = ObjectName(reader_.Read<std::uint64_t>());
                return std::format("Cast<{}>({})", Leaf(target), Expr(depth + 1));
            }
            case EX_ObjToInterfaceCast:
            case EX_CrossInterfaceCast:
            case EX_InterfaceToObjCast: {
                const std::string target = ObjectName(reader_.Read<std::uint64_t>());
                return std::format("Cast<{}>({})", Leaf(target), Expr(depth + 1));
            }
            case EX_Cast: {
                const int kind = reader_.Byte();
                return std::format("Cast<{}>({})", kind, Expr(depth + 1));
            }

            // --- containers -----------------------------------------------------------
            case EX_ArrayGetByRef: {
                const std::string array = Expr(depth + 1);
                const std::string index = Expr(depth + 1);
                return std::format("{}[{}]", array, index);
            }

            case EX_SwitchValue: {
                const std::uint16_t cases = reader_.Read<std::uint16_t>();
                reader_.Read<std::uint32_t>();                 // end offset
                const std::string subject = Expr(depth + 1);

                std::string out = std::format("Switch({}", subject);
                for (std::uint16_t i = 0; i < cases && reader_.Ok(); ++i) {
                    const std::string label = Expr(depth + 1);
                    reader_.Read<std::uint32_t>();             // next case offset
                    const std::string value = Expr(depth + 1);
                    out += std::format(", {} => {}", label, value);
                }
                out += std::format(", default => {})", Expr(depth + 1));
                return out;
            }

            case EX_InstanceDelegate:
                return std::format("Delegate({})", ScriptName());

            default:
                // Everything else is a statement, not an expression, or is an opcode this
                // build does not decode. Either way the caller must not continue blind.
                reader_.Fail(std::format("unexpected opcode 0x{:02X} ({}) in expression",
                                         opcode, OpcodeName(opcode)));
                out_.unknown_opcodes.push_back(opcode);
                return "<error>";
        }
    }

    // Renders one statement. Returns false when the walk must stop.
    bool Statement(int depth) {
        if (!reader_.Ok() || reader_.AtEnd()) return false;

        const std::uint32_t at = reader_.Pos();
        const std::uint8_t opcode = reader_.Byte();

        switch (opcode) {
            // Debug markers carry no semantics and would only add noise.
            case EX_Tracepoint:
            case EX_WireTracepoint:
            case EX_Breakpoint:
            case EX_DeprecatedOp4A:
                return true;
            case EX_InstrumentationEvent:
                reader_.Byte();
                return true;

            case EX_Nothing:
                return true;

            case EX_EndOfScript:
                Line(at, depth, "// end of script");
                return false;

            case EX_Return: {
                const std::string value = Expr();
                Line(at, depth, value.empty() ? "return;" : std::format("return {};", value));
                return true;
            }

            case EX_Jump: {
                const std::uint32_t target = reader_.Read<std::uint32_t>();
                targets_.insert(target);
                Line(at, depth, std::format("goto Label_{:04X};", target));
                return true;
            }
            case EX_JumpIfNot: {
                const std::uint32_t target = reader_.Read<std::uint32_t>();
                targets_.insert(target);
                const std::string condition = Expr();
                Line(at, depth, std::format("if (!({})) goto Label_{:04X};", condition, target));
                return true;
            }
            case EX_ComputedJump: {
                const std::string target = Expr();
                Line(at, depth, std::format("goto [{}];", target));
                return true;
            }
            case EX_PushExecutionFlow: {
                const std::uint32_t target = reader_.Read<std::uint32_t>();
                targets_.insert(target);
                Line(at, depth, std::format("PushFlow(Label_{:04X});", target));
                return true;
            }
            case EX_PopExecutionFlow:
                Line(at, depth, "PopFlow();");
                return true;
            case EX_PopExecutionFlowIfNot: {
                const std::string condition = Expr();
                Line(at, depth, std::format("if (!({})) PopFlow();", condition));
                return true;
            }

            case EX_Let: {
                PropertyName(reader_.Read<std::uint64_t>());
                const std::string lhs = Expr();
                const std::string rhs = Expr();
                Line(at, depth, std::format("{} = {};", lhs, rhs));
                return true;
            }
            case EX_LetBool:
            case EX_LetObj:
            case EX_LetWeakObjPtr:
            case EX_LetDelegate:
            case EX_LetMulticastDelegate: {
                const std::string lhs = Expr();
                const std::string rhs = Expr();
                Line(at, depth, std::format("{} = {};", lhs, rhs));
                return true;
            }
            case EX_LetValueOnPersistentFrame: {
                const std::string destination = PropertyName(reader_.Read<std::uint64_t>());
                const std::string value = Expr();
                Line(at, depth, std::format("PersistentFrame.{} = {};", destination, value));
                return true;
            }

            case EX_AddMulticastDelegate: {
                const std::string target = Expr();
                const std::string value = Expr();
                Line(at, depth, std::format("{} += {};", target, value));
                return true;
            }
            case EX_RemoveMulticastDelegate: {
                const std::string target = Expr();
                const std::string value = Expr();
                Line(at, depth, std::format("{} -= {};", target, value));
                return true;
            }
            case EX_ClearMulticastDelegate: {
                const std::string target = Expr();
                Line(at, depth, std::format("{}.Clear();", target));
                return true;
            }
            case EX_BindDelegate: {
                const std::string name = ScriptName();
                const std::string delegate = Expr();
                const std::string object = Expr();
                Line(at, depth, std::format("{}.Bind({}, {});", delegate, object, name));
                return true;
            }

            case EX_SetArray: {
                const std::string target = Expr();
                std::string values;
                while (reader_.Ok() && Peek() != EX_EndArray) {
                    if (!values.empty()) values += ", ";
                    values += Expr();
                }
                reader_.Byte();
                Line(at, depth, std::format("{} = {{{}}};", target, values));
                return true;
            }
            case EX_SetSet: {
                const std::string target = Expr();
                reader_.Read<std::int32_t>();
                std::string values;
                while (reader_.Ok() && Peek() != EX_EndSet) {
                    if (!values.empty()) values += ", ";
                    values += Expr();
                }
                reader_.Byte();
                Line(at, depth, std::format("{} = TSet{{{}}};", target, values));
                return true;
            }
            case EX_SetMap: {
                const std::string target = Expr();
                reader_.Read<std::int32_t>();
                std::string values;
                while (reader_.Ok() && Peek() != EX_EndMap) {
                    if (!values.empty()) values += ", ";
                    const std::string key = Expr();
                    const std::string value = Expr();
                    values += std::format("{}: {}", key, value);
                }
                reader_.Byte();
                Line(at, depth, std::format("{} = TMap{{{}}};", target, values));
                return true;
            }

            case EX_Assert: {
                reader_.Read<std::uint16_t>();     // line number
                reader_.Byte();                    // debug mode
                const std::string condition = Expr();
                Line(at, depth, std::format("assert({});", condition));
                return true;
            }
            case EX_Skip: {
                reader_.Read<std::uint32_t>();
                const std::string expression = Expr();
                Line(at, depth, std::format("{};", expression));
                return true;
            }

            case EX_AutoRtfmTransact: {
                reader_.Read<std::int32_t>();
                reader_.Read<std::uint32_t>();
                Line(at, depth, "// AutoRTFM transaction");
                return true;
            }
            case EX_AutoRtfmStopTransact: {
                reader_.Read<std::int32_t>();
                reader_.Byte();
                return true;
            }
            case EX_AutoRtfmAbortIfNot: {
                const std::string condition = Expr();
                Line(at, depth, std::format("AutoRtfmAbortIfNot({});", condition));
                return true;
            }

            default: {
                // Not a statement opcode: rewind one byte and let the expression decoder
                // try, which covers a bare call used for its side effect.
                reader_.Seek(at);
                const std::string expression = Expr();
                if (!reader_.Ok()) return false;
                if (!expression.empty()) Line(at, depth, expression + ";");
                return true;
            }
        }
    }

    const std::set<std::uint32_t>& Targets() const { return targets_; }

private:
    static constexpr int kMaxDepth = 48;

    std::uint8_t Peek() {
        if (reader_.AtEnd()) { reader_.Fail("expected a terminator"); return 0; }
        const std::uint32_t at = reader_.Pos();
        const std::uint8_t value = reader_.Byte();
        reader_.Seek(at);
        return value;
    }

    void Line(std::uint32_t at, int depth, std::string text) {
        out_.lines.push_back(ScriptLine{at, depth, std::move(text)});
    }

    // FScriptName: comparison id, display id, number.
    std::string ScriptName() {
        const std::uint32_t comparison = reader_.Read<std::uint32_t>();
        reader_.Read<std::uint32_t>();
        const std::int32_t number = reader_.Read<std::int32_t>();
        std::string name = ResolveFName(*context_.memory, *context_.pool, comparison, number);
        return name.empty() ? std::format("<name {}>", comparison) : name;
    }

    std::string ObjectName(std::uint64_t pointer) {
        if (pointer == 0) return "nullptr";
        const auto object = static_cast<Address>(pointer);
        std::string path = GetObjectPathName(*context_.memory, *context_.object_layout,
                                             *context_.pool, object);
        return path.empty() ? std::format("<object 0x{:X}>", pointer) : path;
    }

    std::string PropertyName(std::uint64_t pointer) {
        if (pointer == 0) return "<null>";
        const auto field = static_cast<Address>(pointer);
        std::string name = GetFieldName(*context_.memory, *context_.property_layout,
                                        *context_.pool, field);
        return name.empty() ? std::format("<field 0x{:X}>", pointer) : name;
    }

    std::string Params(int depth) {
        std::string out;
        while (reader_.Ok()) {
            const std::uint8_t next = Peek();
            if (!reader_.Ok()) break;
            if (next == EX_EndFunctionParms) { reader_.Byte(); break; }

            const std::string argument = Expr(depth + 1);
            if (!reader_.Ok()) break;
            if (argument.empty()) continue;        // EX_Nothing: an omitted optional
            if (!out.empty()) out += ", ";
            out += argument;
        }
        return out;
    }

    const ResolveContext& context_;
    Reader& reader_;
    Decompiled& out_;
    std::set<std::uint32_t> targets_;
    bool doubles_{true};
};

} // namespace

std::string_view OpcodeName(std::uint8_t opcode) {
    const auto& names = OpcodeNames();
    const auto it = names.find(opcode);
    return it == names.end() ? "Unknown" : it->second;
}

ScriptLayout DeriveScriptLayout(core::IMemorySource& memory,
                                const ObjectArrayInfo& array,
                                const NamePoolInfo& pool,
                                const UObjectLayout& object_layout,
                                const UStructLayout& struct_layout) {
    ScriptLayout layout;
    if (!struct_layout.Valid() || struct_layout.min_alignment < 0) return layout;

    // Ask the engine how wide its own FVector is, rather than inferring it from a version
    // number. /Script/CoreUObject.Vector is 12 bytes of float on UE4 and 24 bytes of
    // double on UE5, and the reflection data states that size outright.
    //
    // Vector2D is checked as well, independently: 8 bytes against 16. One struct having an
    // unexpected size could be a licensee change to that struct; both agreeing is the
    // engine's precision model. If they disagree, or neither is present, the value is left
    // at its UE5 default and said so — a wrong guess here is silent corruption, so it is
    // worth a line in the log.
    {
        const auto vector = FindObjectByPath(memory, array, object_layout, pool,
                                             "/Script/CoreUObject.Vector");
        const auto vector2d = FindObjectByPath(memory, array, object_layout, pool,
                                               "/Script/CoreUObject.Vector2D");

        const std::int32_t v_size  = IsNull(vector)   ? 0 : GetPropertiesSize(memory, struct_layout, vector);
        const std::int32_t v2_size = IsNull(vector2d) ? 0 : GetPropertiesSize(memory, struct_layout, vector2d);

        const bool v_double  = v_size == 24;
        const bool v_float   = v_size == 12;
        const bool v2_double = v2_size == 16;
        const bool v2_float  = v2_size == 8;

        if ((v_double && v2_double) || (v_double && v2_size == 0)) {
            layout.double_vectors = true;
            layout.evidence.push_back(std::format(
                "double-precision bytecode constants: FVector is {} bytes, FVector2D {}",
                v_size, v2_size));
        } else if ((v_float && v2_float) || (v_float && v2_size == 0)) {
            layout.double_vectors = false;
            layout.evidence.push_back(std::format(
                "single-precision bytecode constants: FVector is {} bytes, FVector2D {}",
                v_size, v2_size));
        } else {
            core::LogWarn("could not measure FVector precision (FVector {} bytes, FVector2D "
                          "{}); assuming double. A wrong answer here desyncs bytecode at the "
                          "first vector literal.", v_size, v2_size);
            layout.evidence.push_back("FVector precision unmeasured; assumed double");
        }

        core::LogInfo("bytecode constants: {}-precision vectors (FVector {} bytes)",
                      layout.double_vectors ? "double" : "single", v_size);
    }

    // Collect every function first, then thin evenly. Striding the *index* and stopping
    // at a cap does not work: engine functions are dense in the low indices, so the cap
    // fills long before /Game/ is reached and the sample contains no Blueprint function
    // at all — which makes Script indistinguishable from any other always-empty TArray.
    std::vector<Address> all;
    for (std::int32_t index = 0; index < array.num_elements; ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;
        if (GetClassName(memory, object_layout, pool, object) != "Function") continue;
        all.push_back(object);
    }

    constexpr std::size_t kSample = 800;
    std::vector<Address> functions;
    if (all.size() <= kSample) {
        functions = std::move(all);
    } else {
        const std::size_t step = all.size() / kSample;
        for (std::size_t i = 0; i < all.size() && functions.size() < kSample; i += step)
            functions.push_back(all[i]);
    }

    if (functions.size() < 32) return layout;

    // Script is a TArray<uint8> declared right after MinAlignment. It is identified by
    // the shape of a TArray plus one fact no unrelated field satisfies: native functions
    // have an empty script while Blueprint functions have a non-empty one, so a real
    // Script field shows *both* across a sample. A field that is always empty, or always
    // non-empty, is something else.
    const int search_from = (struct_layout.min_alignment + 4 + 7) & ~7;

    for (int offset = search_from; offset <= search_from + 0x40; offset += 8) {
        int empty = 0, populated = 0, sane = 0;

        for (const auto function : functions) {
            const auto data  = core::ReadOr<Address>(memory, function + offset);
            const auto count = core::ReadOr<std::int32_t>(memory, function + offset + 8);
            const auto capacity = core::ReadOr<std::int32_t>(memory, function + offset + 12);

            if (count < 0 || capacity < count) continue;
            constexpr std::int32_t kMaxScript = 4 << 20;
            if (count > kMaxScript) continue;

            if (count == 0) { ++empty; ++sane; continue; }
            if (IsNull(data)) continue;

            // The first byte must be a known opcode; random heap data rarely is.
            std::uint8_t first{};
            if (!core::ReadInto(memory, data, first)) continue;
            if (OpcodeName(first) == "Unknown") continue;

            ++populated;
            ++sane;
        }

        const int total = static_cast<int>(functions.size());
        core::LogDebug("script probe +{:#x}: {} sane, {} populated, {} empty of {}",
                       offset, sane, populated, empty, total);

        if (sane >= total * 9 / 10 && populated > total / 50 && empty > total / 50) {
            layout.script_array = offset;
            layout.confidence   = 0.9f;
            layout.evidence.push_back(std::format(
                "UStruct::Script at +{:#x}: {} functions carry bytecode starting with a "
                "known opcode and {} are empty, out of {}", offset, populated, empty, total));
            break;
        }
    }

    if (!layout.Valid())
        core::LogWarn("could not identify UStruct::Script");
    else
        core::LogInfo("UStruct::Script at +{:#x}", layout.script_array);

    return layout;
}

std::vector<std::uint8_t> GetScriptBytes(core::IMemorySource& memory,
                                         const ScriptLayout& layout, Address function) {
    std::vector<std::uint8_t> code;
    if (IsNull(function) || !layout.Valid()) return code;

    const auto data  = core::ReadOr<Address>(memory, function + layout.script_array);
    const auto count = core::ReadOr<std::int32_t>(memory, function + layout.script_array + 8);

    constexpr std::int32_t kMaxScript = 4 << 20;
    if (IsNull(data) || count <= 0 || count > kMaxScript) return code;

    code.resize(static_cast<std::size_t>(count));
    const std::size_t got = memory.Read(data, code.data(), code.size());
    code.resize(got);
    return code;
}

Decompiled DecompileFunction(const ResolveContext& context, const ScriptLayout& layout,
                             Address function) {
    return DecompileBytecode(context, GetScriptBytes(*context.memory, layout, function));
}

Decompiled DecompileBytecode(const ResolveContext& context,
                             const std::vector<std::uint8_t>& code) {
    Decompiled out;
    out.bytes_total = static_cast<std::uint32_t>(code.size());
    if (code.empty()) return out;

    Reader reader(code);
    Decompiler decompiler(context, reader, out);

    while (reader.Ok() && !reader.AtEnd()) {
        if (!decompiler.Statement(0)) break;
    }

    out.bytes_decoded = reader.Pos();
    if (!reader.Ok()) {
        out.complete    = false;
        out.stop_reason = reader.Failure();
    }

    // Labels are inserted after the fact because a jump target is only known once the
    // instruction referencing it has been read.
    if (!decompiler.Targets().empty()) {
        std::vector<ScriptLine> merged;
        merged.reserve(out.lines.size() + decompiler.Targets().size());

        for (const auto& line : out.lines) {
            if (decompiler.Targets().count(line.offset))
                merged.push_back(ScriptLine{line.offset, 0,
                                            std::format("Label_{:04X}:", line.offset)});
            merged.push_back(line);
        }
        out.lines = std::move(merged);
    }

    return out;
}

} // namespace zircon::engine
