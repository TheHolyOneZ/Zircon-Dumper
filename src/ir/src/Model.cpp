#include "ir/Model.h"

#include <algorithm>

namespace zircon::ir {
namespace {

// Single source of truth for the text form. Both directions read this table, so a name
// can't drift between writing and parsing and leave us with dumps we can't read back.
struct KindName {
    TypeKind         kind;
    std::string_view text;
};

constexpr KindName kKindNames[] = {
    {TypeKind::Unknown,           "unknown"},
    {TypeKind::Bool,              "bool"},
    {TypeKind::Int8,              "int8"},
    {TypeKind::Int16,             "int16"},
    {TypeKind::Int32,             "int32"},
    {TypeKind::Int64,             "int64"},
    {TypeKind::UInt8,             "uint8"},
    {TypeKind::UInt16,            "uint16"},
    {TypeKind::UInt32,            "uint32"},
    {TypeKind::UInt64,            "uint64"},
    {TypeKind::Float,             "float"},
    {TypeKind::Double,            "double"},
    {TypeKind::Name,              "name"},
    {TypeKind::String,            "string"},
    {TypeKind::Text,              "text"},
    {TypeKind::Enum,              "enum"},
    {TypeKind::Struct,            "struct"},
    {TypeKind::ObjectPtr,         "objectptr"},
    {TypeKind::WeakPtr,           "weakptr"},
    {TypeKind::LazyPtr,           "lazyptr"},
    {TypeKind::SoftPtr,           "softptr"},
    {TypeKind::SoftClassPtr,      "softclassptr"},
    {TypeKind::ClassPtr,          "classptr"},
    {TypeKind::Interface,         "interface"},
    {TypeKind::Array,             "array"},
    {TypeKind::Set,               "set"},
    {TypeKind::Map,               "map"},
    {TypeKind::Delegate,          "delegate"},
    {TypeKind::MulticastDelegate, "multicast_delegate"},
    {TypeKind::FieldPath,         "fieldpath"},
    {TypeKind::Optional,          "optional"},
};

static_assert(std::size(kKindNames) == std::size(kAllTypeKinds),
              "kKindNames and kAllTypeKinds must cover the same set of kinds");

} // namespace

std::string_view ToString(TypeKind kind) {
    // No `default:` label, deliberately. MSVC raises C4062 for an unhandled enumerator,
    // so at /W4 with a zero-warning policy a newly added kind fails the build instead of
    // quietly serialising as "unknown".
    switch (kind) {
        case TypeKind::Unknown:           return "unknown";
        case TypeKind::Bool:              return "bool";
        case TypeKind::Int8:              return "int8";
        case TypeKind::Int16:             return "int16";
        case TypeKind::Int32:             return "int32";
        case TypeKind::Int64:             return "int64";
        case TypeKind::UInt8:             return "uint8";
        case TypeKind::UInt16:            return "uint16";
        case TypeKind::UInt32:            return "uint32";
        case TypeKind::UInt64:            return "uint64";
        case TypeKind::Float:             return "float";
        case TypeKind::Double:            return "double";
        case TypeKind::Name:              return "name";
        case TypeKind::String:            return "string";
        case TypeKind::Text:              return "text";
        case TypeKind::Enum:              return "enum";
        case TypeKind::Struct:            return "struct";
        case TypeKind::ObjectPtr:         return "objectptr";
        case TypeKind::WeakPtr:           return "weakptr";
        case TypeKind::LazyPtr:           return "lazyptr";
        case TypeKind::SoftPtr:           return "softptr";
        case TypeKind::SoftClassPtr:      return "softclassptr";
        case TypeKind::ClassPtr:          return "classptr";
        case TypeKind::Interface:         return "interface";
        case TypeKind::Array:             return "array";
        case TypeKind::Set:               return "set";
        case TypeKind::Map:               return "map";
        case TypeKind::Delegate:          return "delegate";
        case TypeKind::MulticastDelegate: return "multicast_delegate";
        case TypeKind::FieldPath:         return "fieldpath";
        case TypeKind::Optional:          return "optional";
    }
    return "unknown";
}

std::optional<TypeKind> TypeKindFromString(std::string_view text) {
    const auto it = std::find_if(std::begin(kKindNames), std::end(kKindNames),
                                 [&](const KindName& entry) { return entry.text == text; });
    if (it == std::end(kKindNames)) return std::nullopt;
    return it->kind;
}

bool TypeRef::operator==(const TypeRef& other) const {
    return kind == other.kind && name == other.name && raw == other.raw &&
           size == other.size && params == other.params;
}

const Package* Dump::FindPackage(std::string_view name) const {
    const auto it = std::find_if(packages.begin(), packages.end(),
                                 [&](const Package& package) { return package.name == name; });
    return it == packages.end() ? nullptr : &*it;
}

std::size_t Dump::TotalClasses() const {
    std::size_t total = 0;
    for (const auto& package : packages) total += package.classes.size();
    return total;
}

std::size_t Dump::TotalStructs() const {
    std::size_t total = 0;
    for (const auto& package : packages) total += package.structs.size();
    return total;
}

std::size_t Dump::TotalEnums() const {
    std::size_t total = 0;
    for (const auto& package : packages) total += package.enums.size();
    return total;
}

std::size_t Dump::TotalProperties() const {
    std::size_t total = 0;
    for (const auto& package : packages) {
        for (const auto& klass : package.classes)  total += klass.properties.size();
        for (const auto& record : package.structs) total += record.properties.size();
    }
    return total;
}

std::size_t Dump::TotalFunctions() const {
    std::size_t total = 0;
    for (const auto& package : packages) {
        for (const auto& klass : package.classes)  total += klass.functions.size();
        for (const auto& record : package.structs) total += record.functions.size();
    }
    return total;
}

} // namespace zircon::ir
