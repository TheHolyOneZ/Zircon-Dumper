#include "engine/TypeResolver.h"
#include "core/Log.h"

#include <algorithm>
#include <format>
#include <map>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr int kMaxSubclassProbe = 0x100;
constexpr int kMaxTypeDepth     = 8;   // TMap<FName, TArray<TMap<...>>> bottoms out fast

bool Readable(core::IMemorySource& memory, Address addr, std::size_t size = 8) {
    if (IsNull(addr) || Raw(addr) < 0x10000 || Raw(addr) >= 0x7FFFFFFFFFFFull) return false;
    std::uint8_t probe[64];
    const std::size_t want = std::min(size, sizeof(probe));
    return memory.Read(addr, probe, want) == want;
}

bool IsArrayObject(core::IMemorySource& memory, const ObjectArrayInfo& array, Address candidate) {
    if (IsNull(candidate)) return false;
    std::int32_t index{};
    if (!core::ReadInto(memory, candidate + array.index_offset, index)) return false;
    if (index < 0 || index >= array.num_elements) return false;
    return Raw(ObjectAt(memory, array, index)) == Raw(candidate);
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

SubclassLayout DeriveSubclassLayout(core::IMemorySource& memory,
                                    const ObjectArrayInfo& array,
                                    const NamePoolInfo& pool,
                                    const UObjectLayout& object_layout,
                                    const UStructLayout& struct_layout,
                                    const FPropertyLayout& property_layout) {
    SubclassLayout layout;
    if (!property_layout.Valid()) return layout;

    // Group by reflected type, so each slot is derived only from properties that actually
    // have it. Differing base depths get discovered instead of assumed away.
    std::map<std::string, std::vector<Address>> by_type;
    constexpr std::size_t kPerType = 200;

    for (std::int32_t index = 0; index < array.num_elements; ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;

        const std::string kind = GetClassName(memory, object_layout, pool, object);
        if (kind != "Class" && kind != "ScriptStruct") continue;

        for (const auto field :
             GetChildProperties(memory, struct_layout, property_layout, object)) {
            const std::string type = GetPropertyTypeName(memory, property_layout, pool, field);
            if (type.empty()) continue;
            auto& bucket = by_type[type];
            if (bucket.size() < kPerType) bucket.push_back(field);
        }
    }

    // Nothing before the last known FProperty member can belong to a subclass.
    const int search_from =
        (std::max({property_layout.offset_internal, property_layout.element_size,
                   property_layout.array_dim, property_layout.property_flags + 4}) + 4 + 7) & ~7;

    auto samples_for = [&](std::initializer_list<const char*> types) {
        std::vector<Address> all;
        for (const char* type : types) {
            const auto it = by_type.find(type);
            if (it != by_type.end()) all.insert(all.end(), it->second.begin(), it->second.end());
        }
        return all;
    };

    // The offset where `predicate` holds for nearly every sample. Lowest qualifying one,
    // which matters because a pointer member is often followed by another of the same shape
    // (KeyProp then ValueProp) and the declared one comes first. `skip_first` takes the
    // second instead.
    auto derive_slot = [&](const char* label, const std::vector<Address>& samples,
                           auto predicate, int skip_first = 0) -> int {
        if (samples.size() < 8) return -1;

        int qualifying = 0;
        for (int offset = search_from; offset <= kMaxSubclassProbe; offset += 8) {
            int hits = 0;
            for (const auto field : samples)
                if (predicate(core::ReadOr<Address>(memory, field + offset))) ++hits;

            if (hits * 10 < static_cast<int>(samples.size()) * 9) continue;
            if (qualifying++ < skip_first) continue;

            layout.evidence.push_back(std::format("{} at +{:#x} ({}/{} samples)", label,
                                                  offset, hits, samples.size()));
            return offset;
        }
        return -1;
    };

    auto is_object_of_kind = [&](std::string_view expected) {
        return [&memory, &array, &object_layout, &pool, expected](Address target) {
            if (!IsArrayObject(memory, array, target)) return false;
            return GetClassName(memory, object_layout, pool, target) == expected;
        };
    };

    // Whether a pointer names another property.
    //
    // Which side of the object array it has to be on depends on the era, and this is the
    // test that ends up backwards if you forget. An FField lives *outside* the array, which
    // is how ChildProperties was told apart from Children in the first place; a UProperty
    // is a UObject and so sits *in* it. Requiring "not in the array" unconditionally made
    // every container on a 4.24-or-earlier target resolve its element type to nothing.
    auto is_property_field = [&](Address target) {
        if (IsNull(target)) return false;
        if (IsArrayObject(memory, array, target) != property_layout.uproperty) return false;
        if (!Readable(memory, target, 32)) return false;
        return EndsWith(GetPropertyTypeName(memory, property_layout, pool, target), "Property");
    };

    // UnderlyingProp is specifically a numeric property, and any-FProperty* isn't
    // selective enough. An FProperty carries several generic link pointers (PropertyLinkNext
    // and friends) that satisfy the looser test, and the search then resolves the *next
    // property in the owning struct* as the enum's underlying type.
    auto is_numeric_property = [&](Address target) {
        if (!is_property_field(target)) return false;
        const std::string type = GetPropertyTypeName(memory, property_layout, pool, target);
        return type == "ByteProperty"  || type == "Int8Property"  ||
               type == "Int16Property" || type == "IntProperty"   ||
               type == "Int64Property" || type == "UInt16Property" ||
               type == "UInt32Property" || type == "UInt64Property";
    };

    // All of them inherit PropertyClass from FObjectPropertyBase, so pooling gives one
    // larger and more reliable sample set.
    layout.object_property_class = derive_slot(
        "FObjectPropertyBase::PropertyClass",
        samples_for({"ObjectProperty", "ObjectPtrProperty", "WeakObjectProperty",
                     "LazyObjectProperty", "SoftObjectProperty", "ClassProperty",
                     "SoftClassProperty"}),
        is_object_of_kind("Class"));

    // MetaClass is the *second* UClass* in a class property. Skipping the first is what
    // tells TSubclassOf<What> apart from the container class.
    layout.class_meta_class = derive_slot(
        "FClassProperty::MetaClass", samples_for({"ClassProperty", "SoftClassProperty"}),
        is_object_of_kind("Class"), /*skip_first=*/1);

    layout.struct_struct = derive_slot("FStructProperty::Struct",
                                       samples_for({"StructProperty"}),
                                       is_object_of_kind("ScriptStruct"));

    layout.array_inner = derive_slot("FArrayProperty::Inner",
                                     samples_for({"ArrayProperty"}), is_property_field);
    layout.set_element = derive_slot("FSetProperty::ElementProp",
                                     samples_for({"SetProperty"}), is_property_field);
    layout.map_key     = derive_slot("FMapProperty::KeyProp",
                                     samples_for({"MapProperty"}), is_property_field);
    layout.map_value   = derive_slot("FMapProperty::ValueProp",
                                     samples_for({"MapProperty"}), is_property_field, 1);

    layout.enum_underlying = derive_slot("FEnumProperty::UnderlyingProp",
                                         samples_for({"EnumProperty"}), is_numeric_property);
    layout.enum_enum       = derive_slot("FEnumProperty::Enum",
                                         samples_for({"EnumProperty"}),
                                         is_object_of_kind("Enum"));
    layout.byte_enum       = derive_slot("FByteProperty::Enum",
                                         samples_for({"ByteProperty"}),
                                         is_object_of_kind("Enum"));
    layout.interface_class = derive_slot("FInterfaceProperty::InterfaceClass",
                                         samples_for({"InterfaceProperty"}),
                                         is_object_of_kind("Class"));
    layout.delegate_signature = derive_slot(
        "FDelegateProperty::SignatureFunction",
        samples_for({"DelegateProperty", "MulticastDelegateProperty",
                     "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty"}),
        is_object_of_kind("Function"));

    // FBoolProperty stores FieldSize, ByteOffset, ByteMask, FieldMask. Native bools carry
    // full masks, packed bitfields a single bit. Demanding *both* shapes turn up is what
    // proves this is the mask block and not just four small-looking bytes.
    {
        const auto bools = samples_for({"BoolProperty"});
        auto single_bit_or_full = [](std::uint8_t value) {
            return value == 0xFF || (value != 0 && (value & (value - 1)) == 0);
        };

        for (int offset = search_from; offset <= kMaxSubclassProbe && layout.bool_masks < 0;
             offset += 4) {
            if (bools.size() < 8) break;

            int valid = 0, packed = 0, full = 0;
            for (const auto field : bools) {
                const auto byte_mask  = core::ReadOr<std::uint8_t>(memory, field + offset + 2);
                const auto field_mask = core::ReadOr<std::uint8_t>(memory, field + offset + 3);
                if (!single_bit_or_full(byte_mask) || !single_bit_or_full(field_mask)) continue;
                ++valid;
                if (field_mask == 0xFF) ++full; else ++packed;
            }

            if (valid * 10 >= static_cast<int>(bools.size()) * 9 && packed > 0 && full > 0) {
                layout.bool_masks = offset;
                layout.evidence.push_back(std::format(
                    "FBoolProperty masks at +{:#x}: {} packed bitfields and {} native bools "
                    "among {} samples", offset, packed, full, bools.size()));
            }
        }
    }

    int resolved = 0;
    for (const int slot : {layout.object_property_class, layout.class_meta_class,
                           layout.struct_struct, layout.array_inner, layout.set_element,
                           layout.map_key, layout.map_value, layout.enum_underlying,
                           layout.enum_enum, layout.byte_enum, layout.interface_class,
                           layout.delegate_signature, layout.bool_masks})
        if (slot >= 0) ++resolved;

    layout.confidence = layout.Valid() ? std::min(0.98f, 0.5f + 0.04f * resolved) : 0.0f;

    core::LogInfo("subclass slots: object +{:#x}, struct +{:#x}, array inner +{:#x}, "
                  "bool masks +{:#x} ({} of 13 resolved)",
                  layout.object_property_class, layout.struct_struct, layout.array_inner,
                  layout.bool_masks, resolved);
    return layout;
}

namespace {

ResolvedType ResolveTypeInner(const ResolveContext& context, Address field, int depth) {
    ResolvedType type;
    if (IsNull(field) || depth > kMaxTypeDepth) return type;

    auto& memory = *context.memory;
    const auto& slots = *context.subclass_layout;

    type.raw  = GetPropertyTypeName(memory, *context.property_layout, *context.pool, field);
    type.size = GetElementSize(memory, *context.property_layout, field);

    // Only resolves when the target really is the expected kind. Silence beats a wrong
    // name here, since an unresolved reference is visible in the output and a confidently
    // wrong one isn't.
    auto referenced_object = [&](int slot, std::string_view expected_kind) -> std::string {
        if (slot < 0) return {};
        const auto target = core::ReadOr<Address>(memory, field + slot);
        if (!IsArrayObject(memory, *context.array, target)) return {};
        if (!expected_kind.empty() &&
            GetClassName(memory, *context.object_layout, *context.pool, target) != expected_kind)
            return {};
        type.referenced_object = target;
        return GetObjectPathName(memory, *context.object_layout, *context.pool, target);
    };

    auto inner_property = [&](int slot) -> ResolvedType {
        if (slot < 0) return {};
        const auto inner = core::ReadOr<Address>(memory, field + slot);
        if (IsNull(inner)) return {};

        // Opposite senses per era; see is_property_field in the derivation above.
        if (IsArrayObject(memory, *context.array, inner) !=
            context.property_layout->uproperty)
            return {};

        return ResolveTypeInner(context, inner, depth + 1);
    };

    const std::string& raw = type.raw;

    if (raw == "ObjectProperty" || raw == "WeakObjectProperty" || raw == "LazyObjectProperty" ||
        raw == "SoftObjectProperty" || raw == "ObjectPtrProperty") {
        type.referenced = referenced_object(slots.object_property_class, "Class");
    } else if (raw == "ClassProperty" || raw == "SoftClassProperty") {
        // MetaClass gives TSubclassOf<What>. PropertyClass is only ever UClass itself.
        type.referenced = referenced_object(slots.class_meta_class, "Class");
        if (type.referenced.empty()) {
            type.referenced_object = {};
            type.referenced = referenced_object(slots.object_property_class, "Class");
        }
    } else if (raw == "StructProperty") {
        type.referenced = referenced_object(slots.struct_struct, "ScriptStruct");
    } else if (raw == "EnumProperty") {
        type.referenced = referenced_object(slots.enum_enum, "Enum");
        type.params.push_back(inner_property(slots.enum_underlying));
    } else if (raw == "ByteProperty") {
        // Null for a plain uint8.
        type.referenced = referenced_object(slots.byte_enum, "Enum");
    } else if (raw == "ArrayProperty") {
        type.params.push_back(inner_property(slots.array_inner));
    } else if (raw == "SetProperty") {
        type.params.push_back(inner_property(slots.set_element));
    } else if (raw == "OptionalProperty") {
        type.params.push_back(inner_property(slots.array_inner));
    } else if (raw == "MapProperty") {
        type.params.push_back(inner_property(slots.map_key));
        type.params.push_back(inner_property(slots.map_value));
    } else if (raw == "InterfaceProperty") {
        type.referenced = referenced_object(slots.interface_class, "Class");
    } else if (raw == "DelegateProperty" || raw == "MulticastDelegateProperty" ||
               raw == "MulticastInlineDelegateProperty" ||
               raw == "MulticastSparseDelegateProperty") {
        type.referenced = referenced_object(slots.delegate_signature, "Function");
    }

    return type;
}

} // namespace

ResolvedType ResolveType(const ResolveContext& context, Address field) {
    return ResolveTypeInner(context, field, 0);
}

BitfieldInfo ResolveBitfield(const ResolveContext& context, Address field) {
    BitfieldInfo info;
    if (IsNull(field) || context.subclass_layout->bool_masks < 0) return info;

    auto& memory = *context.memory;
    if (GetPropertyTypeName(memory, *context.property_layout, *context.pool, field) !=
        "BoolProperty")
        return info;

    const int at = context.subclass_layout->bool_masks;
    info.byte_mask  = core::ReadOr<std::uint8_t>(memory, field + at + 2);
    info.field_mask = core::ReadOr<std::uint8_t>(memory, field + at + 3);

    // A native bool owns its byte and has a full mask. Anything else is packed, and
    // emitters have to reproduce the exact bit or the struct layout comes out wrong.
    if (info.field_mask != 0 && info.field_mask != 0xFF) {
        info.is_bitfield = true;
        for (int bit = 0; bit < 8; ++bit) {
            if (info.field_mask & (1u << bit)) { info.bit_index = bit; break; }
        }
    }
    return info;
}

std::string DescribeType(const ResolvedType& type) {
    auto leaf = [](const std::string& path) {
        const auto dot = path.find_last_of('.');
        return dot == std::string::npos ? path : path.substr(dot + 1);
    };

    const std::string& raw = type.raw;

    if (raw == "ArrayProperty" && !type.params.empty())
        return "TArray<" + DescribeType(type.params[0]) + ">";
    if (raw == "SetProperty" && !type.params.empty())
        return "TSet<" + DescribeType(type.params[0]) + ">";
    if (raw == "OptionalProperty" && !type.params.empty())
        return "TOptional<" + DescribeType(type.params[0]) + ">";
    if (raw == "MapProperty" && type.params.size() == 2)
        return "TMap<" + DescribeType(type.params[0]) + ", " + DescribeType(type.params[1]) + ">";

    if (raw == "ObjectProperty" || raw == "ObjectPtrProperty")
        return type.referenced.empty() ? "UObject*" : leaf(type.referenced) + "*";
    if (raw == "WeakObjectProperty")  return "TWeakObjectPtr<" + leaf(type.referenced) + ">";
    if (raw == "LazyObjectProperty")  return "TLazyObjectPtr<" + leaf(type.referenced) + ">";
    if (raw == "SoftObjectProperty")  return "TSoftObjectPtr<" + leaf(type.referenced) + ">";
    if (raw == "ClassProperty")       return "TSubclassOf<" + leaf(type.referenced) + ">";
    if (raw == "SoftClassProperty")   return "TSoftClassPtr<" + leaf(type.referenced) + ">";
    if (raw == "InterfaceProperty")   return "TScriptInterface<" + leaf(type.referenced) + ">";
    if (raw == "StructProperty")
        return type.referenced.empty() ? "struct" : "F" + leaf(type.referenced);
    if (raw == "EnumProperty" || (raw == "ByteProperty" && !type.referenced.empty()))
        return leaf(type.referenced);

    if (raw == "BoolProperty")      return "bool";
    if (raw == "ByteProperty")      return "uint8";
    if (raw == "Int8Property")      return "int8";
    if (raw == "Int16Property")     return "int16";
    if (raw == "IntProperty")       return "int32";
    if (raw == "Int64Property")     return "int64";
    if (raw == "UInt16Property")    return "uint16";
    if (raw == "UInt32Property")    return "uint32";
    if (raw == "UInt64Property")    return "uint64";
    if (raw == "FloatProperty")     return "float";
    if (raw == "DoubleProperty")    return "double";
    if (raw == "NameProperty")      return "FName";
    if (raw == "StrProperty")       return "FString";
    if (raw == "TextProperty")      return "FText";
    if (raw == "FieldPathProperty") return "TFieldPath";

    if (raw == "DelegateProperty")  return "FDelegate";
    if (raw == "MulticastDelegateProperty" || raw == "MulticastInlineDelegateProperty" ||
        raw == "MulticastSparseDelegateProperty")
        return "FMulticastDelegate";

    // Unknown property classes keep their engine name. Emitters decide how to render an
    // opaque type of the right size.
    return raw.empty() ? "unknown" : raw;
}

} // namespace zircon::engine
