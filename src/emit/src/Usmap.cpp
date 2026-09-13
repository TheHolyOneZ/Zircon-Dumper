#include "Emitters.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit {
namespace {

// .usmap version 0 ("Initial"). Later versions widen the name length prefix to 16 bits and
// add package-version fields. Emitting 0 keeps the file readable by the widest range of
// tools and nothing here needs what came later.
constexpr std::uint16_t kMagic           = 0x30C4;
constexpr std::uint8_t  kVersion         = 0;
constexpr std::uint8_t  kCompressionNone = 0;

// -1 means absent. Used for a struct with no super.
constexpr std::uint32_t kNoName = 0xFFFFFFFFu;

// Single-byte length prefix in version 0. The format's limit, and nothing we chose.
constexpr std::size_t kMaxNameLength = 255;

// Array dimensions are a single byte in the entry header.
constexpr std::int32_t kMaxArrayDim = 255;

// Counts are uint16 per struct.
constexpr std::int64_t kMaxPropertyCount = 0xFFFF;

// A TypeRef is a tree, and a malformed dump could in principle be cyclic. Real depths like
// TMap<FName, TArray<...>> are tiny, so a low cap costs nothing and bounds the recursion.
constexpr int kMaxTypeDepth = 16;

// EPropertyType, in the order the format defines. On-disk contract: change one and every
// mapping ever produced gets silently reinterpreted.
enum class PropertyType : std::uint8_t {
    Byte              = 0,
    Bool              = 1,
    Int               = 2,
    Float             = 3,
    Object            = 4,
    Name              = 5,
    Delegate          = 6,
    Double            = 7,
    Array             = 8,
    Struct            = 9,
    Str               = 10,
    Text              = 11,
    Interface         = 12,
    MulticastDelegate = 13,
    WeakObject        = 14,
    LazyObject        = 15,
    AssetObject       = 16,
    SoftObject        = 17,
    UInt64            = 18,
    UInt32            = 19,
    UInt16            = 20,
    Int64             = 21,
    Int16             = 22,
    Int8              = 23,
    Map               = 24,
    Set               = 25,
    Enum              = 26,
    FieldPath         = 27,
    Optional          = 28,
    Utf8Str           = 29,
    AnsiStr           = 30,
};

// Little-endian: what UE writes, what every reader expects.
class ByteWriter {
public:
    void U8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }

    void U16(std::uint16_t value) {
        U8(static_cast<std::uint8_t>(value & 0xFF));
        U8(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }

    void U32(std::uint32_t value) {
        U16(static_cast<std::uint16_t>(value & 0xFFFF));
        U16(static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
    }

    void Raw(std::string_view bytes) { out_.append(bytes); }

    const std::string& Data() const { return out_; }
    std::size_t Size() const { return out_.size(); }

private:
    std::string out_;
};

// Interns names, hands back indices. Deduplication matters for more than size here: the
// format identifies structs and enums by name index, so two spellings of one name become
// two entries that readers treat as unrelated types.
class NameTable {
public:
    explicit NameTable(EmitResult& result) : result_(&result) {}

    std::uint32_t Add(std::string_view name) {
        if (name.empty()) return kNoName;

        std::string key(name);
        if (key.size() > kMaxNameLength) {
            // Truncation changes identity, so dedupe on the truncated form deliberately.
            // Otherwise two long names sharing a prefix write identical bytes under
            // different indices.
            result_->warnings.push_back("usmap: name longer than " +
                                        std::to_string(kMaxNameLength) +
                                        " bytes truncated: " + key);
            key.resize(kMaxNameLength);
        }

        const auto existing = index_.find(key);
        if (existing != index_.end()) return existing->second;

        const auto id = static_cast<std::uint32_t>(names_.size());
        index_.emplace(key, id);
        names_.push_back(std::move(key));
        return id;
    }

    const std::vector<std::string>& All() const { return names_; }

private:
    std::vector<std::string>                       names_;
    std::unordered_map<std::string, std::uint32_t> index_;
    EmitResult*                                    result_;
};

// The format is flat. Types are identified by leaf name.
std::string TypeLeaf(std::string_view path) {
    return path.empty() ? std::string{} : util::LeafName(path);
}

// `recognised` says whether the mapping is real, so the caller can warn instead of writing
// a plausible-looking wrong type. A mapping file that lies about a type corrupts every
// asset parsed with it.
PropertyType MapKind(ir::TypeKind kind, bool& recognised) {
    recognised = true;
    switch (kind) {
        case ir::TypeKind::Bool:      return PropertyType::Bool;
        case ir::TypeKind::Int8:      return PropertyType::Int8;
        case ir::TypeKind::Int16:     return PropertyType::Int16;
        case ir::TypeKind::Int32:     return PropertyType::Int;
        case ir::TypeKind::Int64:     return PropertyType::Int64;
        case ir::TypeKind::UInt8:     return PropertyType::Byte;
        case ir::TypeKind::UInt16:    return PropertyType::UInt16;
        case ir::TypeKind::UInt32:    return PropertyType::UInt32;
        case ir::TypeKind::UInt64:    return PropertyType::UInt64;
        case ir::TypeKind::Float:     return PropertyType::Float;
        case ir::TypeKind::Double:    return PropertyType::Double;
        case ir::TypeKind::Name:      return PropertyType::Name;
        case ir::TypeKind::String:    return PropertyType::Str;
        case ir::TypeKind::Text:      return PropertyType::Text;
        case ir::TypeKind::Enum:      return PropertyType::Enum;
        case ir::TypeKind::Struct:    return PropertyType::Struct;
        case ir::TypeKind::ObjectPtr: return PropertyType::Object;
        case ir::TypeKind::WeakPtr:   return PropertyType::WeakObject;
        case ir::TypeKind::LazyPtr:   return PropertyType::LazyObject;
        case ir::TypeKind::SoftPtr:   return PropertyType::SoftObject;

        // No distinct soft-class type in the format. UE serialises TSoftClassPtr down the
        // same path as a soft object reference.
        case ir::TypeKind::SoftClassPtr: return PropertyType::SoftObject;

        // Likewise UClass*, as an ordinary object reference.
        case ir::TypeKind::ClassPtr:  return PropertyType::Object;

        case ir::TypeKind::Interface: return PropertyType::Interface;
        case ir::TypeKind::Array:     return PropertyType::Array;
        case ir::TypeKind::Set:       return PropertyType::Set;
        case ir::TypeKind::Map:       return PropertyType::Map;
        case ir::TypeKind::Delegate:  return PropertyType::Delegate;
        case ir::TypeKind::MulticastDelegate: return PropertyType::MulticastDelegate;
        case ir::TypeKind::FieldPath: return PropertyType::FieldPath;
        case ir::TypeKind::Optional:  return PropertyType::Optional;
        case ir::TypeKind::Unknown:   break;
    }
    recognised = false;
    return PropertyType::Byte;
}

// Second chance for a type the IR couldn't classify. The engine's own property class name
// survives in TypeRef::raw, so a build shipping a type this tool predates still maps.
PropertyType MapRaw(std::string_view raw, bool& recognised) {
    recognised = true;
    if (raw == "BoolProperty")        return PropertyType::Bool;
    if (raw == "ByteProperty")        return PropertyType::Byte;
    if (raw == "Int8Property")        return PropertyType::Int8;
    if (raw == "Int16Property")       return PropertyType::Int16;
    if (raw == "IntProperty")         return PropertyType::Int;
    if (raw == "Int64Property")       return PropertyType::Int64;
    if (raw == "UInt16Property")      return PropertyType::UInt16;
    if (raw == "UInt32Property")      return PropertyType::UInt32;
    if (raw == "UInt64Property")      return PropertyType::UInt64;
    if (raw == "FloatProperty")       return PropertyType::Float;
    if (raw == "DoubleProperty")      return PropertyType::Double;
    if (raw == "NameProperty")        return PropertyType::Name;
    if (raw == "StrProperty")         return PropertyType::Str;
    if (raw == "TextProperty")        return PropertyType::Text;
    if (raw == "EnumProperty")        return PropertyType::Enum;
    if (raw == "StructProperty")      return PropertyType::Struct;
    if (raw == "ObjectProperty" || raw == "ObjectPtrProperty" ||
        raw == "ClassProperty"  || raw == "ClassPtrProperty")
        return PropertyType::Object;
    if (raw == "WeakObjectProperty")  return PropertyType::WeakObject;
    if (raw == "LazyObjectProperty")  return PropertyType::LazyObject;
    if (raw == "SoftObjectProperty" || raw == "SoftClassProperty")
        return PropertyType::SoftObject;
    if (raw == "AssetObjectProperty") return PropertyType::AssetObject;
    if (raw == "InterfaceProperty")   return PropertyType::Interface;
    if (raw == "ArrayProperty")       return PropertyType::Array;
    if (raw == "SetProperty")         return PropertyType::Set;
    if (raw == "MapProperty")         return PropertyType::Map;
    if (raw == "DelegateProperty")    return PropertyType::Delegate;
    if (raw == "MulticastDelegateProperty" || raw == "MulticastInlineDelegateProperty" ||
        raw == "MulticastSparseDelegateProperty")
        return PropertyType::MulticastDelegate;
    if (raw == "FieldPathProperty")   return PropertyType::FieldPath;
    if (raw == "OptionalProperty")    return PropertyType::Optional;
    if (raw == "Utf8StrProperty")     return PropertyType::Utf8Str;
    if (raw == "AnsiStrProperty")     return PropertyType::AnsiStr;

    recognised = false;
    return PropertyType::Byte;
}

void WriteType(ByteWriter& writer, NameTable& names, const ir::TypeRef& type,
               EmitResult& result, int depth);

// Something has to be written. The format is positional, so omitting an inner type shifts
// every byte after it and the whole file stops parsing.
void WriteOpaque(ByteWriter& writer, EmitResult& result, std::string_view reason) {
    result.warnings.push_back("usmap: " + std::string(reason) +
                              "; wrote ByteProperty as an opaque placeholder");
    writer.U8(static_cast<std::uint8_t>(PropertyType::Byte));
}

void WriteInner(ByteWriter& writer, NameTable& names, const ir::TypeRef& parent,
                std::size_t index, EmitResult& result, int depth,
                std::string_view what) {
    if (index < parent.params.size()) {
        WriteType(writer, names, parent.params[index], result, depth + 1);
        return;
    }
    WriteOpaque(writer, result, std::string(what) + " of " +
                                (parent.raw.empty() ? std::string("container")
                                                    : parent.raw) + " is unresolved");
}

void WriteType(ByteWriter& writer, NameTable& names, const ir::TypeRef& type,
               EmitResult& result, int depth) {
    if (depth > kMaxTypeDepth) {
        WriteOpaque(writer, result, "type nesting exceeded the depth limit");
        return;
    }

    bool recognised = false;
    PropertyType mapped = MapKind(type.kind, recognised);
    if (!recognised) {
        mapped = MapRaw(type.raw, recognised);
        if (!recognised) {
            WriteOpaque(writer, result,
                        "unrecognised property type '" +
                            (type.raw.empty() ? std::string("<unnamed>") : type.raw) + "'");
            return;
        }
    }

    writer.U8(static_cast<std::uint8_t>(mapped));

    switch (mapped) {
        case PropertyType::Enum: {
            // Inner type is the enum's storage. TEnumAsByte arrives as an IR Enum whose
            // raw is ByteProperty with no underlying param, so supply the byte storage
            // instead of reporting it missing.
            if (!type.params.empty()) {
                WriteType(writer, names, type.params[0], result, depth + 1);
            } else {
                writer.U8(static_cast<std::uint8_t>(PropertyType::Byte));
            }
            writer.U32(names.Add(TypeLeaf(type.name)));
            break;
        }

        case PropertyType::Struct:
            writer.U32(names.Add(TypeLeaf(type.name)));
            break;

        case PropertyType::Array:
        case PropertyType::Set:
        case PropertyType::Optional:
            WriteInner(writer, names, type, 0, result, depth, "element type");
            break;

        case PropertyType::Map:
            WriteInner(writer, names, type, 0, result, depth, "key type");
            WriteInner(writer, names, type, 1, result, depth, "value type");
            break;

        // No payload in version 0. The format records a property's shape and stops there.
        default:
            break;
    }
}

std::string FileStem(const ir::Dump& dump) {
    std::string source = dump.header.source.process;
    if (source.empty()) source = dump.header.source.main_module;
    if (source.empty()) return "mappings";

    const auto dot = source.find_last_of('.');
    if (dot != std::string::npos && dot > 0) source.resize(dot);

    // Keep what a file name can legitimately hold, hyphens in "Game-Win64-Shipping"
    // included. Replace anything a path would choke on.
    std::string stem;
    stem.reserve(source.size());
    for (const char c : source) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        stem.push_back(safe ? c : '_');
    }
    return stem.empty() ? "mappings" : stem;
}

bool PackageIncluded(const ir::Package& package, const EmitOptions& options) {
    return options.package_filter.empty() ||
           package.name.find(options.package_filter) != std::string::npos;
}

} // namespace

EmitResult EmitUsmap(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "usmap needs a complete dump; this one is partial "
                       "(pass allow_partial to emit anyway)";
        return result;
    }

    NameTable names(result);
    ByteWriter enums_body;
    ByteWriter structs_body;

    std::uint32_t enum_count   = 0;
    std::uint32_t struct_count = 0;

    // usmap is a flat namespace keyed by leaf name, so two packages defining the same
    // leaf collapse into one entry. Readers cannot tell them apart, so the collision is
    // reported rather than hidden.
    std::unordered_set<std::string> seen_type_names;
    auto note_collision = [&](const std::string& leaf, const std::string& path,
                              const char* what) {
        if (!seen_type_names.insert(leaf).second) {
            result.warnings.push_back(std::string("usmap: duplicate ") + what + " name '" +
                                      leaf + "' (from " + path +
                                      "); the format is flat and readers will see one entry");
        }
    };

    for (const auto& package : dump.packages) {
        if (!PackageIncluded(package, options)) continue;

        for (const auto& record : package.enums) {
            const std::string leaf = record.name.empty() ? TypeLeaf(record.path)
                                                         : record.name;
            if (leaf.empty()) {
                result.warnings.push_back("usmap: skipped an enum with no name in " +
                                          package.name);
                continue;
            }
            note_collision(leaf, record.path, "enum");

            if (record.values.size() > 0xFF) {
                result.warnings.push_back("usmap: enum '" + leaf + "' has " +
                                          std::to_string(record.values.size()) +
                                          " values; the format stores at most 255, extras dropped");
            }
            const auto value_count =
                static_cast<std::uint8_t>(std::min<std::size_t>(record.values.size(), 0xFF));

            enums_body.U32(names.Add(leaf));
            enums_body.U8(value_count);
            for (std::uint8_t i = 0; i < value_count; ++i)
                enums_body.U32(names.Add(record.values[i].name));

            ++enum_count;
        }
    }

    // Classes and script structs share the struct table: UE serialises both through the
    // same schema machinery, and a mapping file that omitted one would fail to parse half
    // of any asset.
    for (const auto& package : dump.packages) {
        if (!PackageIncluded(package, options)) continue;

        for (const auto* list : {&package.classes, &package.structs}) {
            for (const auto& record : *list) {
                const std::string leaf = record.name.empty() ? TypeLeaf(record.path)
                                                             : record.name;
                if (leaf.empty()) {
                    result.warnings.push_back("usmap: skipped a struct with no name in " +
                                              package.name);
                    continue;
                }
                note_collision(leaf, record.path, "struct");

                // Property slots, counting an inline array as the number of elements it
                // occupies, because the schema index advances per element.
                std::int64_t slots = 0;
                for (const auto& property : record.properties)
                    slots += std::max<std::int32_t>(property.array_dim, 1);

                if (slots > kMaxPropertyCount) {
                    result.warnings.push_back("usmap: struct '" + leaf + "' has " +
                                              std::to_string(slots) +
                                              " property slots; clamped to 65535");
                    slots = kMaxPropertyCount;
                }
                const auto entry_count = std::min<std::size_t>(record.properties.size(),
                                                               kMaxPropertyCount);

                structs_body.U32(names.Add(leaf));
                structs_body.U32(record.super.empty() ? kNoName
                                                      : names.Add(TypeLeaf(record.super)));
                structs_body.U16(static_cast<std::uint16_t>(slots));
                structs_body.U16(static_cast<std::uint16_t>(entry_count));

                std::int64_t schema_index = 0;
                for (std::size_t i = 0; i < entry_count; ++i) {
                    const auto& property = record.properties[i];

                    std::int32_t dim = std::max<std::int32_t>(property.array_dim, 1);
                    if (dim > kMaxArrayDim) {
                        result.warnings.push_back(
                            "usmap: '" + leaf + "." + property.name + "' has array dim " +
                            std::to_string(dim) + "; the format stores one byte, clamped to 255");
                        dim = kMaxArrayDim;
                    }

                    structs_body.U16(static_cast<std::uint16_t>(
                        std::min<std::int64_t>(schema_index, kMaxPropertyCount)));
                    structs_body.U8(static_cast<std::uint8_t>(dim));
                    structs_body.U32(names.Add(property.name));
                    WriteType(structs_body, names, property.type, result, 0);

                    schema_index += dim;
                }

                ++struct_count;
            }
        }
    }

    // The name table is written first but is only complete once everything that
    // references a name has been serialised, so the body is assembled last.
    ByteWriter body;
    body.U32(static_cast<std::uint32_t>(names.All().size()));
    for (const auto& name : names.All()) {
        body.U8(static_cast<std::uint8_t>(name.size()));
        body.Raw(name);
    }

    body.U32(enum_count);
    body.Raw(enums_body.Data());

    body.U32(struct_count);
    body.Raw(structs_body.Data());

    ByteWriter file;
    file.U16(kMagic);
    file.U8(kVersion);
    file.U8(kCompressionNone);

    // With no compression the two sizes are equal, but both must carry the real payload
    // length: a reader allocates from the decompressed size and bounds its read with the
    // compressed size, so a zero in either truncates the file to nothing.
    const auto payload = static_cast<std::uint32_t>(body.Size());
    file.U32(payload);
    file.U32(payload);
    file.Raw(body.Data());

    const std::string path = options.out_dir + "/" + FileStem(dump) + ".usmap";
    std::string error;
    if (!util::WriteFile(path, file.Data(), error)) {
        result.error = error;
        return result;
    }

    result.files.push_back(path);
    return result;
}

} // namespace zircon::emit
