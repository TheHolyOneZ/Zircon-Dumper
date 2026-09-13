#include "engine/ValueReader.h"
#include "engine/EnumLayout.h"
#include "core/Log.h"

#include <algorithm>
#include <format>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr const char* kUnreadable = "<unreadable>";

template <typename T>
std::string Scalar(core::IMemorySource& memory, Address at, bool hex) {
    T value{};
    if (!core::ReadInto(memory, at, value)) return kUnreadable;

    if constexpr (std::is_floating_point_v<T>) {
        return std::format("{}", value);
    } else {
        if (hex) return std::format("0x{:X}", static_cast<std::uint64_t>(value));
        return std::format("{}", value);
    }
}

std::string Printable(std::string_view text, int limit) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    int written = 0;
    for (const char c : text) {
        if (written >= limit) { out += "..."; break; }
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 32) out += '.';
        else out.push_back(c);
        ++written;
    }
    out.push_back('"');
    return out;
}

std::string ValueAt(const ResolveContext& context, Address object, Address field,
                    const ValueFormat& format, int depth);

// Rendered by walking the struct's own properties. Only readable to a shallow depth; past
// that the line runs off the screen and says nothing.
std::string StructValue(const ResolveContext& context, Address at, Address structure,
                        const std::string& path, const ValueFormat& format, int depth) {
    auto& memory = *context.memory;

    if (IsNull(structure) || depth >= format.max_depth)
        return path.empty() ? "{...}" : std::format("{{{}}}", path.substr(path.find_last_of('.') + 1));

    const auto fields = GetChildProperties(memory, *context.struct_layout,
                                           *context.property_layout, structure);
    if (fields.empty()) return "{}";

    std::string out = "{";
    int shown = 0;
    for (const auto member : fields) {
        if (shown >= format.max_array_items) { out += ", ..."; break; }
        if (shown) out += ", ";

        const std::string name = GetFieldName(memory, *context.property_layout,
                                              *context.pool, member);
        out += std::format("{}={}", name, ValueAt(context, at, member, format, depth + 1));
        ++shown;
    }
    return out + "}";
}

std::string ValueAt(const ResolveContext& context, Address object, Address field,
                    const ValueFormat& format, int depth) {
    auto& memory = *context.memory;
    const auto& properties = *context.property_layout;

    const std::int32_t offset = GetPropertyOffset(memory, properties, field);
    const Address at = object + static_cast<std::uint64_t>(offset);

    const std::string type = GetPropertyTypeName(memory, properties, *context.pool, field);
    const auto resolved = ResolveType(context, field);

    // Mask bitfields out of their byte. Read it whole and any of the eight packed flags
    // reports 1.
    if (type == "BoolProperty") {
        const auto bits = ResolveBitfield(context, field);
        std::uint8_t raw{};
        if (!core::ReadInto(memory, at, raw)) return kUnreadable;
        if (bits.is_bitfield) return (raw & bits.field_mask) ? "true" : "false";
        return raw ? "true" : "false";
    }

    if (type == "Int8Property")    return Scalar<std::int8_t>(memory, at, format.hex_integers);
    if (type == "Int16Property")   return Scalar<std::int16_t>(memory, at, format.hex_integers);
    if (type == "IntProperty")     return Scalar<std::int32_t>(memory, at, format.hex_integers);
    if (type == "Int64Property")   return Scalar<std::int64_t>(memory, at, format.hex_integers);
    if (type == "UInt16Property")  return Scalar<std::uint16_t>(memory, at, format.hex_integers);
    if (type == "UInt32Property")  return Scalar<std::uint32_t>(memory, at, format.hex_integers);
    if (type == "UInt64Property")  return Scalar<std::uint64_t>(memory, at, format.hex_integers);
    if (type == "FloatProperty")   return Scalar<float>(memory, at, false);
    if (type == "DoubleProperty")  return Scalar<double>(memory, at, false);

    if (type == "NameProperty") {
        std::uint32_t id{};
        std::int32_t number{};
        if (!core::ReadInto(memory, at, id)) return kUnreadable;
        core::ReadInto(memory, at + 4, number);
        const std::string name = ResolveFName(memory, *context.pool, id, number);
        return name.empty() ? std::format("<name {}>", id) : name;
    }

    if (type == "StrProperty") {
        const std::string text = ReadFString(memory, at, format.max_string);
        return Printable(text, format.max_string);
    }

    if (type == "TextProperty") return "<FText>";

    if (type == "ByteProperty" || type == "EnumProperty") {
        // Width follows the underlying property. A fixed size gets the 4-byte enums this
        // engine uses freely wrong.
        std::int64_t value = 0;
        std::int32_t width = GetElementSize(memory, properties, field);
        if (width == 8)      { std::int64_t v{}; if (!core::ReadInto(memory, at, v)) return kUnreadable; value = v; }
        else if (width == 4) { std::int32_t v{}; if (!core::ReadInto(memory, at, v)) return kUnreadable; value = v; }
        else if (width == 2) { std::int16_t v{}; if (!core::ReadInto(memory, at, v)) return kUnreadable; value = v; }
        else                 { std::uint8_t v{}; if (!core::ReadInto(memory, at, v)) return kUnreadable; value = v; }

        // Nobody wants to read a number here. Resolving against the UEnum needs the enum
        // layout, which is optional in this context, so the numeric form stays as a
        // fallback instead of the display being impossible without it.
        if (context.enum_layout && context.enum_layout->Valid() &&
            !IsNull(resolved.referenced_object)) {
            for (const auto& [name, entry] :
                 GetEnumValues(memory, *context.enum_layout, *context.pool,
                               resolved.referenced_object)) {
                if (entry != value) continue;

                // UE stores entries fully qualified, so the scope is redundant.
                const auto scope = name.rfind("::");
                return scope == std::string::npos ? name : name.substr(scope + 2);
            }
        }
        return std::format("{}", value);
    }

    if (type == "ObjectProperty" || type == "ObjectPtrProperty" ||
        type == "ClassProperty"  || type == "WeakObjectProperty") {
        Address target{};
        if (!core::ReadInto(memory, at, target)) return kUnreadable;
        if (IsNull(target)) return "nullptr";

        const std::string path = GetObjectPathName(memory, *context.object_layout,
                                                   *context.pool, target);
        return path.empty() ? std::format("0x{:X}", Raw(target)) : path;
    }

    if (type == "StructProperty")
        return StructValue(context, at, resolved.referenced_object, resolved.referenced,
                           format, depth);

    if (type == "ArrayProperty") {
        Address data{};
        std::int32_t count{};
        if (!core::ReadInto(memory, at, data) || !core::ReadInto(memory, at + 8, count))
            return kUnreadable;
        if (count < 0 || count > (1 << 22)) return "<bad array>";
        if (count == 0) return "[]";
        return std::format("[{} item{}]", count, count == 1 ? "" : "s");
    }

    if (type == "MapProperty" || type == "SetProperty") {
        // Sparse arrays with a hash index. The element count isn't at a fixed offset in
        // any way worth guessing at, so report the shape instead of a number that might
        // be wrong.
        return type == "MapProperty" ? "<TMap>" : "<TSet>";
    }

    if (type.find("Delegate") != std::string::npos) return "<delegate>";

    return std::format("<{}>", type.empty() ? "unknown" : type);
}

} // namespace

std::string ReadFString(core::IMemorySource& memory, Address address, int max_chars) {
    Address data{};
    std::int32_t count{};
    if (!core::ReadInto(memory, address, data)) return {};
    if (!core::ReadInto(memory, address + 8, count)) return {};

    if (IsNull(data) || count <= 0 || count > (1 << 20)) return {};

    // The stored count includes the terminator.
    const int chars = std::min(count - 1, max_chars);
    if (chars <= 0) return {};

    std::vector<std::uint16_t> wide(static_cast<std::size_t>(chars));
    const std::size_t want = wide.size() * sizeof(std::uint16_t);
    if (memory.Read(data, wide.data(), want) != want) return {};

    std::string out;
    out.reserve(wide.size());
    for (const std::uint16_t c : wide) {
        if (c == 0) break;
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

std::string ReadPropertyValue(const ResolveContext& context, Address object, Address field,
                              const ValueFormat& format) {
    if (IsNull(object) || IsNull(field)) return kUnreadable;
    return ValueAt(context, object, field, format, 0);
}

} // namespace zircon::engine
