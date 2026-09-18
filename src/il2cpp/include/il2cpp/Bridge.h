#pragma once

// The questions the walk asks a runtime. Nothing about how they get answered.
//
// Mode 1 answers them by calling the exported C API from inside the process. That's the only
// way to get il2cpp_field_get_offset to say anything: it's a function, not a field, and
// reading memory won't run it. So the IL2CPP path injects where the Unreal path never has to.
//
// Everything above this header is written against IBridge rather than the C API, so the two
// providers still to come (minidump reader, external no-inject reader) slot in under the
// same walk instead of forking it.

#include "core/MemorySource.h"
#include "il2cpp/Runtime.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zircon::il2cpp {

using core::Address;

// ECMA-335 element types, which is what il2cpp_type_get_type returns. These belong to the
// CLI standard, not Unity, so unlike a metadata version they don't move.
enum class ElementType : std::uint8_t {
    End         = 0x00,
    Void        = 0x01,
    Boolean     = 0x02,
    Char        = 0x03,
    I1          = 0x04, U1 = 0x05,
    I2          = 0x06, U2 = 0x07,
    I4          = 0x08, U4 = 0x09,
    I8          = 0x0A, U8 = 0x0B,
    R4          = 0x0C, R8 = 0x0D,
    String      = 0x0E,
    Ptr         = 0x0F,
    ByRef       = 0x10,
    ValueType   = 0x11,
    Class       = 0x12,
    Var         = 0x13,     // !T   -- a class's own generic parameter
    Array       = 0x14,     // multi-dimensional
    GenericInst = 0x15,
    TypedByRef  = 0x16,
    I           = 0x18, U = 0x19,
    FnPtr       = 0x1B,
    Object      = 0x1C,
    SzArray     = 0x1D,     // T[]
    MVar        = 0x1E,     // !!T  -- a method's own generic parameter
    CModReqd    = 0x1F, CModOpt = 0x20,
    Internal    = 0x21,
};

std::string_view ToString(ElementType type);

// A generic parameter that hasn't been given a value. A field of this type has no offset
// worth recording (the open-generic trap).
constexpr bool IsGenericParameter(ElementType type) {
    return type == ElementType::Var || type == ElementType::MVar;
}

// Element types that stand for a class the runtime already has.
//
// The rest don't, and the difference matters. Asked for the class behind a pointer type,
// IL2CPP goes and *builds* one -- allocating, taking locks -- and an older runtime asked to
// do that from an injected thread takes the process down. Reading is safe; asking the
// runtime to construct something isn't.
constexpr bool NamesAClass(ElementType type) {
    switch (type) {
        case ElementType::Class:
        case ElementType::Object:
        case ElementType::ValueType:
        case ElementType::GenericInst:
        case ElementType::SzArray:
        case ElementType::Array:
            return true;
        default:
            return false;
    }
}

struct TypeFacts {
    std::string   name;              // "System.Collections.Generic.List`1<System.Int32>"
    ElementType   element{ElementType::End};
    Address       klass{};           // the class, or an array's element class
    bool          by_ref{false};
    bool          is_pointer{false};
    std::uint32_t attrs{0};
};

struct ClassFacts {
    std::string   name;              // "List`1"
    std::string   name_space;        // "System.Collections.Generic"
    Address       parent{};
    Address       declaring{};       // outer type of a nested type
    Address       image{};
    std::uint32_t flags{0};          // TypeAttributes, verbatim
    std::uint32_t token{0};          // metadata token, 0 when unavailable
    std::int32_t  instance_size{0};  // boxed size, header included
    std::int32_t  value_size{-1};    // unboxed size, or -1 when not a value type
    std::int32_t  rank{0};           // array rank, 0 when not an array
    bool          is_valuetype{false};
    bool          is_enum{false};
    bool          is_interface{false};
    bool          is_abstract{false};
    bool          is_generic{false}; // an open definition: List`1, not List<int>
    bool          is_inflated{false};// a concrete instantiation: List<int>
};

struct FieldFacts {
    std::string   name;
    Address       type{};
    std::int32_t  offset{-1};
    std::uint32_t flags{0};          // FieldAttributes, verbatim
    bool          is_literal{false}; // a const. no storage, so no offset either
};

struct MethodFacts {
    std::string   name;
    Address       return_type{};
    std::uint32_t param_count{0};
    std::uint32_t flags{0};          // MethodAttributes, verbatim
    std::uint32_t token{0};
    bool          is_instance{true};
    bool          is_generic{false};
    bool          is_inflated{false};

    // Where the compiled body starts, in the target. Zero when MethodInfo's layout couldn't
    // be derived; reported, not guessed.
    Address       body{};
};

struct PropertyFacts {
    std::string   name;
    Address       getter{};
    Address       setter{};
    std::uint32_t flags{0};
};

class IBridge {
public:
    virtual ~IBridge() = default;

    // Joins the calling thread to the runtime. Touching a managed object from a thread the
    // GC has never heard of is how an injected dumper takes the game down.
    virtual bool Attach() = 0;

    // Bytes between the start of a boxed object and its first field. Field offsets are
    // reported from the boxed start even for structs, so a struct's real layout sits this
    // much lower. Asked, not assumed to be 0x10.
    virtual std::int32_t ObjectHeaderSize() const = 0;

    virtual Address              Domain() = 0;
    virtual std::vector<Address> Assemblies() = 0;
    virtual Address              AssemblyImage(Address assembly) = 0;

    // Every class the runtime currently holds, inflated generics included. List`1 is in the
    // metadata; List<int> is made at runtime and only shows up here. Empty when the build
    // doesn't export il2cpp_class_for_each.
    virtual std::vector<Address> AllClasses() = 0;

    virtual std::string  ImageName(Address image) = 0;
    virtual std::size_t  ImageClassCount(Address image) = 0;
    virtual Address      ImageClass(Address image, std::size_t index) = 0;

    virtual ClassFacts           Class(Address klass) = 0;
    virtual std::vector<Address> Fields(Address klass) = 0;
    virtual std::vector<Address> Methods(Address klass) = 0;
    virtual std::vector<Address> Properties(Address klass) = 0;
    virtual std::vector<Address> NestedTypes(Address klass) = 0;
    virtual std::vector<Address> Interfaces(Address klass) = 0;
    virtual Address              ClassType(Address klass) = 0;
    virtual Address              EnumBaseType(Address klass) = 0;

    virtual FieldFacts    Field(Address field) = 0;
    virtual MethodFacts   Method(Address method) = 0;
    virtual PropertyFacts Property(Address property) = 0;
    virtual TypeFacts     Type(Address type) = 0;

    virtual std::string ParamName(Address method, std::uint32_t index) = 0;
    virtual Address     ParamType(Address method, std::uint32_t index) = 0;

    // Reads a const's value into `out`. False when this build can't answer, in which case
    // the enum keeps its names and says the values are unresolved.
    virtual bool LiteralValue(Address field, void* out, std::size_t size) = 0;

    // Where the runtime module is loaded, so a body address can be stored as an RVA. A VA
    // means nothing once the game restarts.
    virtual Address       ModuleBase() const = 0;
    virtual std::uint64_t ModuleSize() const = 0;

    // How the method body address was arrived at, for the dump header.
    virtual std::vector<std::string> Evidence() const = 0;

    // What was worked out about the runtime's own structures, for the dump header. Same
    // idea as the UObject offsets on the Unreal side: a reader can check the derivation.
    virtual std::vector<std::pair<std::string, std::int32_t>> Derived() const = 0;

    // Resolved entry points as "name=0xRVA", module-relative. An export map of the runtime.
    virtual std::vector<std::string> EntryPoints() const = 0;
};

// The injected provider. Fails when the source can't call into the target -- every answer
// here is a function call.
//
// `read_consts`: whether to ask the runtime for a const's value when we can't read it
// safely ourselves. Without it an enum comes back named but unnumbered. It's also the one
// call here that makes the runtime do work rather than answer, so it can be turned off for
// a build that doesn't survive it.
core::Result<std::unique_ptr<IBridge>> MakeInProcessBridge(const RuntimeInfo& runtime,
                                                           core::IMemorySource& memory,
                                                           bool read_consts = true);

} // namespace zircon::il2cpp
