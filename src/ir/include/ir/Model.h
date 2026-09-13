#pragma once

// The intermediate representation. This is the contract of the whole tool: the engine
// layer produces it, every emitter consumes it, and diffing compares two of them.
//
// It is deliberately free of engine types and of memory concerns. Nothing here knows
// what an FProperty is or which process it came from, which is what lets emitters be
// tested against checked-in fixtures with no game running.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::ir {

inline constexpr int kSchemaVersion = 1;

// ---------------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------------

enum class TypeKind {
    Unknown,        // unrecognised property class; `raw` carries the engine's own name
    Bool,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double,
    Name, String, Text,
    Enum,           // `name` = enum path
    Struct,         // `name` = struct path
    ObjectPtr,      // `name` = class path
    WeakPtr, LazyPtr, SoftPtr, SoftClassPtr, ClassPtr,
    Interface,
    Array,          // params[0] = element
    Set,            // params[0] = element
    Map,            // params[0] = key, params[1] = value
    Delegate, MulticastDelegate,
    FieldPath,
    Optional,       // params[0] = value
};

std::string_view ToString(TypeKind kind);
std::optional<TypeKind> TypeKindFromString(std::string_view text);

// Every kind, so tests can assert the string mapping is total instead of spot-checking
// a few. ToString switches without a `default:` label, so MSVC raises C4062 when an
// enumerator is added and not handled — under this project's zero-warning policy that
// turns a forgotten kind into a build failure.
inline constexpr TypeKind kAllTypeKinds[] = {
    TypeKind::Unknown,
    TypeKind::Bool,
    TypeKind::Int8,   TypeKind::Int16,  TypeKind::Int32,  TypeKind::Int64,
    TypeKind::UInt8,  TypeKind::UInt16, TypeKind::UInt32, TypeKind::UInt64,
    TypeKind::Float,  TypeKind::Double,
    TypeKind::Name,   TypeKind::String, TypeKind::Text,
    TypeKind::Enum,   TypeKind::Struct,
    TypeKind::ObjectPtr,
    TypeKind::WeakPtr, TypeKind::LazyPtr, TypeKind::SoftPtr,
    TypeKind::SoftClassPtr, TypeKind::ClassPtr,
    TypeKind::Interface,
    TypeKind::Array,  TypeKind::Set,    TypeKind::Map,
    TypeKind::Delegate, TypeKind::MulticastDelegate,
    TypeKind::FieldPath,
    TypeKind::Optional,
};

struct TypeRef {
    TypeKind    kind{TypeKind::Unknown};
    std::string name;               // path of the referenced enum/struct/class, if any

    // The engine's own property class name, e.g. "ObjectProperty". Always populated,
    // including for Unknown, so a dump never loses information just because this build
    // of the tool did not recognise a type.
    std::string raw;

    std::vector<TypeRef> params;    // container element / key / value types

    // Byte size of one element as the engine reports it. Preserved, never inferred
    // because a licensee build can change it.
    std::int32_t size{0};

    // Defined out of line: TypeRef contains a vector of itself, so a defaulted
    // comparison would be evaluated while the type is still incomplete.
    bool operator==(const TypeRef& other) const;
    bool operator!=(const TypeRef& other) const { return !(*this == other); }
};

// ---------------------------------------------------------------------------------
// Members
// ---------------------------------------------------------------------------------

struct Property {
    std::string  name;
    TypeRef      type;
    std::int32_t offset{0};
    std::int32_t size{0};          // total bytes occupied: element size * array_dim
    std::int32_t array_dim{1};
    std::uint64_t flags{0};        // EPropertyFlags, verbatim
    std::vector<std::string> flag_names;

    // Bitfields. A BoolProperty that is a real bool has no mask; one packed into a byte
    // carries the mask the engine uses, which emitters need to reproduce the layout.
    bool          is_bitfield{false};
    std::uint8_t  byte_mask{0};
    std::uint8_t  field_mask{0};
    std::int32_t  bit_index{-1};

    // The value this property has in its class's default object, rendered for reading:
    // "2.5", "ECC_Visibility", "nullptr", "{X=0, Y=0, Z=0}". Empty when defaults were not
    // captured (they are opt-in) or when the class has no default object.
    //
    // A rendered string, not a typed value, on purpose. A typed one would need the
    // IR to model every container and struct shape a default can take, and what a reader
    // actually wants from a default is to read it. Anything that needs the bytes has the
    // CDO's address.
    std::string   default_value;

    bool operator==(const Property&) const = default;
};

struct FunctionParam {
    std::string name;
    TypeRef     type;
    std::int32_t offset{0};
    std::int32_t size{0};
    bool is_return{false};
    bool is_out{false};
    bool is_const{false};

    bool operator==(const FunctionParam&) const = default;
};

// One decompiled statement. The bytecode offset travels with the text so a reader can
// cross-reference it against a raw disassembly, and so a diff can tell a statement that
// moved from one that changed.
struct ScriptStatement {
    std::uint32_t offset{0};
    std::int32_t  depth{0};
    std::string   text;

    bool operator==(const ScriptStatement&) const = default;
};

struct Function {
    std::string   name;
    std::uint32_t flags{0};            // EFunctionFlags, verbatim
    std::vector<std::string> flag_names;

    std::vector<FunctionParam> params; // includes the return value, flagged as such

    // Address of the native implementation, as an offset from the main module base.
    // Module-relative so a dump stays meaningful across runs with different ASLR.
    std::uint64_t native_rva{0};

    // There is deliberately no vtable index here. Unreal's reflection data does not record
    // one: UFunction::Func points at the generated exec thunk, not at the virtual method,
    // and nothing in the object graph names a virtual slot. Recovering it would mean
    // binary analysis with nothing independent to confirm the answer against, which is the
    // kind of plausible-but-unverified guess this tool refuses to emit. Struct::vtable_rva
    // gives the table itself, which is what the question is usually really asking.

    std::int32_t  script_size{0};      // bytes of Kismet bytecode

    // Decompiled statements. Empty unless the dump was taken with script decompilation
    // enabled, which is opt-in because it roughly doubles a large dump.
    std::vector<ScriptStatement> script;

    // False when an opcode was not understood. The statements present are still correct;
    // everything past the stop is deliberately absent, never guessed.
    bool script_complete{true};

    bool operator==(const Function&) const = default;
};

// ---------------------------------------------------------------------------------
// Types that own members
// ---------------------------------------------------------------------------------

struct EnumValue {
    std::string  name;
    std::int64_t value{0};

    bool operator==(const EnumValue&) const = default;
};

struct Enum {
    std::string  name;
    std::string  path;
    std::string  underlying{"uint8"};
    bool         is_flags{false};
    std::vector<EnumValue> values;

    bool operator==(const Enum&) const = default;
};

// One class or script struct. Both are UStructs and share a shape, so they share a
// record; `is_class` distinguishes them for emitters that care.
struct Struct {
    std::string  name;
    std::string  path;             // "/Script/Engine.Actor"
    std::string  super;            // path of the base, empty at the root
    bool         is_class{false};

    std::int32_t size{0};
    std::int32_t alignment{0};
    std::int32_t inherited_size{0};   // where this type's own members start

    // 'A', 'U', 'I' or 'F' — the prefix UE's own headers give this type. Recorded rather
    // than inferred by emitters, because inferring it means walking the super chain, and a
    // filtered dump does not contain the ancestors to walk. The walker always can: it sees
    // the live chain regardless of what the filter keeps. 0 means unknown.
    char cpp_prefix{0};

    // Module-relative address of this class's vtable, or 0.
    //
    // Unlike the CDO's own address this one is stable: a vtable is emitted into the
    // image's read-only data, so the RVA means the same thing on every run and in every
    // dump of the same build. It is read from the first pointer of the class default
    // object and only accepted when it lands inside the main module — a CDO that has not
    // been constructed, or a class with no virtual functions, gives 0 and not a guess.
    std::uint64_t vtable_rva{0};

    std::vector<std::string> interfaces;
    std::vector<Property>    properties;   // declared here only, never inherited
    std::vector<Function>    functions;

    // There is deliberately no address for the class default object here. A CDO is
    // heap-allocated by the class constructor at runtime, so it has no module-relative
    // address, and an absolute one would be a stale heap pointer the moment the dump is
    // saved or shared. What a reader wants from the CDO is its *values*, and those are in
    // Property::default_value.

    bool operator==(const Struct&) const = default;
};

struct Package {
    std::string name;              // "/Script/Engine"
    std::vector<Struct> classes;
    std::vector<Struct> structs;
    std::vector<Enum>   enums;

    bool operator==(const Package&) const = default;
};

// ---------------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------------

struct DerivedOffset {
    std::string  name;             // "UObject.InternalIndex"
    std::int32_t value{0};

    bool operator==(const DerivedOffset&) const = default;
};

struct EngineInfo {
    std::string version;           // "5.6", or empty when unknown
    float       confidence{0.0f};
    bool        uses_fproperty{true};
    bool        chunked_gobjects{true};
    bool        chunked_name_pool{true};
    bool        case_preserving_name{false};
    std::vector<std::string> evidence;

    bool operator==(const EngineInfo&) const = default;
};

struct SourceInfo {
    std::string   kind;            // "internal" | "external" | "dump" | "static"
    std::string   process;
    std::string   main_module;
    std::uint64_t module_base{0};
    std::uint64_t image_size{0};

    bool operator==(const SourceInfo&) const = default;
};

struct Header {
    std::string   tool_version;
    std::string   created_utc;

    // True when the source could not supply everything, e.g. a static PE with no live
    // objects. Emitters that need object data refuse a partial dump instead of
    // producing plausible-looking output from missing data.
    bool          partial{false};

    SourceInfo    source;
    EngineInfo    engine;
    std::vector<DerivedOffset> offsets;
    std::vector<std::string>   globals;   // "GObjects=0x...", module-relative where known

    bool operator==(const Header&) const = default;
};

struct Dump {
    // Top level, not inside Header, matching docs/ARCHITECTURE.md and the
    // usual convention that a reader can check the version before trusting anything
    // else in the document.
    int                      schema_version{kSchemaVersion};

    Header                   header;
    std::vector<std::string> names;       // optional full FName pool
    std::vector<Package>     packages;

    const Package* FindPackage(std::string_view name) const;
    std::size_t TotalClasses() const;
    std::size_t TotalStructs() const;
    std::size_t TotalEnums() const;
    std::size_t TotalProperties() const;
    std::size_t TotalFunctions() const;

    bool operator==(const Dump&) const = default;
};

} // namespace zircon::ir
