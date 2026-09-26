#pragma once

#include "core/MemorySource.h"
#include "core/Types.h"
#include "mono/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zircon::mono {

using core::Address;

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
    Var         = 0x13,
    Array       = 0x14,
    GenericInst = 0x15,
    TypedByRef  = 0x16,
    I           = 0x18, U = 0x19,
    FnPtr       = 0x1B,
    Object      = 0x1C,
    SzArray     = 0x1D,
    MVar        = 0x1E,
    CModReqd    = 0x1F, CModOpt = 0x20,
    Internal    = 0x21,
};

std::string_view ToString(ElementType type);

constexpr bool IsGenericParameter(ElementType type) {
    return type == ElementType::Var || type == ElementType::MVar;
}

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

constexpr std::uint32_t kTypeDefTable = 2;
constexpr std::uint32_t kTypeDefToken = 0x02000000;

constexpr std::int64_t kFieldCountUnknown = -1;

constexpr std::size_t FieldIterationLimit(std::int64_t reported, std::size_t ceiling) {
    if (reported < 0) return ceiling;
    const auto count = static_cast<std::uint64_t>(reported);
    return count > ceiling ? ceiling : static_cast<std::size_t>(count);
}

struct TypeFacts {
    std::string   name;
    ElementType   element{ElementType::End};
    Address       klass{};
    bool          by_ref{false};
};

struct ClassFacts {
    std::string   name;
    std::string   name_space;
    Address       parent{};
    Address       declaring{};
    Address       image{};
    std::uint32_t flags{0};
    std::uint32_t token{0};
    std::int32_t  instance_size{0};
    std::int32_t  value_size{-1};
    std::int32_t  alignment{0};
    bool          is_valuetype{false};
    bool          is_enum{false};
    bool          is_delegate{false};
    bool          is_blittable{false};
};

struct FieldFacts {
    std::string   name;
    Address       type{};
    std::int32_t  offset{-1};
    std::uint32_t flags{0};
};

struct MethodFacts {
    std::string   name;
    Address       return_type{};
    std::uint32_t param_count{0};
    std::uint32_t flags{0};
    std::uint32_t token{0};
};

struct PropertyFacts {
    std::string   name;
    Address       getter{};
    Address       setter{};
    std::uint32_t flags{0};
};

struct AssemblyFacts {
    std::string name;
    std::string version;
    std::string file;
};

class IBridge {
public:
    virtual ~IBridge() = default;

    virtual bool Attach() = 0;

    virtual Address              Domain() = 0;
    virtual std::vector<Address> Assemblies() = 0;
    virtual Address              AssemblyImage(Address assembly) = 0;
    virtual AssemblyFacts        Assembly(Address assembly) = 0;

    virtual std::string   ImageName(Address image) = 0;
    virtual std::uint32_t ImageTypeCount(Address image) = 0;
    virtual Address       ImageType(Address image, std::uint32_t index) = 0;

    virtual ClassFacts           Class(Address klass) = 0;
    virtual std::vector<Address> Fields(Address klass) = 0;
    virtual std::vector<Address> Methods(Address klass) = 0;
    virtual std::vector<Address> Properties(Address klass) = 0;
    virtual std::vector<Address> Interfaces(Address klass) = 0;
    virtual std::vector<Address> NestedTypes(Address klass) = 0;

    virtual FieldFacts    Field(Address field) = 0;
    virtual MethodFacts   Method(Address method) = 0;
    virtual PropertyFacts Property(Address property) = 0;
    virtual TypeFacts     Type(Address type) = 0;
    virtual std::vector<Address> MethodParams(Address method) = 0;

    virtual core::Address ModuleBase() const = 0;
    virtual std::uint64_t ModuleSize() const = 0;
    virtual std::vector<std::string> Evidence() const = 0;
};

core::Result<std::unique_ptr<IBridge>> MakeInProcessBridge(const RuntimeInfo& runtime,
                                                           core::IMemorySource& memory);

} // namespace zircon::mono
