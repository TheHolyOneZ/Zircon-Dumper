#include "engine/ValueWriter.h"

#include "engine/EnumLayout.h"
#include "engine/NamePool.h"
#include "engine/PropertyLayout.h"
#include "engine/StructLayout.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <vector>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;

std::string Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Accepts 0x-prefixed hex as well as decimal, so a value copied out of the browser with
// "hex" ticked can be pasted straight back in.
bool ParseSigned(std::string_view text, std::int64_t& out) {
    std::string s = Trim(text);
    if (s.empty()) return false;

    int base = 10;
    bool negative = false;
    std::size_t i = 0;
    if (s[i] == '-' || s[i] == '+') { negative = s[i] == '-'; ++i; }
    if (s.size() >= i + 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= s.size()) return false;

    std::uint64_t magnitude = 0;
    const auto* begin = s.data() + i;
    const auto* end = s.data() + s.size();
    const auto result = std::from_chars(begin, end, magnitude, base);
    if (result.ec != std::errc{} || result.ptr != end) return false;

    out = negative ? -static_cast<std::int64_t>(magnitude)
                   : static_cast<std::int64_t>(magnitude);
    return true;
}

bool ParseDouble(std::string_view text, double& out) {
    const std::string s = Trim(text);
    if (s.empty()) return false;
    const auto* end = s.data() + s.size();
    const auto result = std::from_chars(s.data(), end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

bool ParseBool(std::string_view text, bool& out) {
    const std::string s = Trim(text);
    if (EqualsNoCase(s, "true")  || s == "1") { out = true;  return true; }
    if (EqualsNoCase(s, "false") || s == "0") { out = false; return true; }
    return false;
}

// A value has to survive the round trip through the property's actual width. Writing
// 300 into a uint8 silently becomes 44, and the caller would never know.
bool FitsIn(std::int64_t value, int width, bool is_signed) {
    if (width >= 8) return true;
    const int bits = width * 8;
    if (is_signed) {
        const std::int64_t limit = std::int64_t{1} << (bits - 1);
        return value >= -limit && value < limit;
    }
    if (value < 0) return false;
    return value < (std::int64_t{1} << bits);
}

// Structs whose bytes are entirely their members, with nothing else living in them.
//
// FVector is three doubles and writing all three is exactly as safe as writing one. FText
// is a shared pointer to reference-counted state, and a struct holding a TArray owns a
// heap allocation, so the test is not "is it a struct" but "is every member a number this
// writer already handles".
//
// One level deep only. A struct of structs is describable but the text form stops being
// something anyone would type, and a partial write of a nested value is worse than a
// refusal.
struct PlainMember {
    std::string  name;
    Address      field{};
    std::int32_t offset{0};
    std::int32_t size{0};
    bool         is_float{false};
    bool         is_signed{false};
};

bool CollectPlainMembers(const ResolveContext& context, Address structure,
                         std::vector<PlainMember>& out) {
    auto& memory = *context.memory;
    if (IsNull(structure)) return false;

    const auto fields = GetChildProperties(memory, *context.struct_layout,
                                           *context.property_layout, structure);
    if (fields.empty()) return false;

    for (const auto member : fields) {
        const std::string type =
            GetPropertyTypeName(memory, *context.property_layout, *context.pool, member);

        PlainMember entry;
        entry.name   = GetFieldName(memory, *context.property_layout, *context.pool, member);
        entry.field  = member;
        entry.offset = GetPropertyOffset(memory, *context.property_layout, member);
        entry.size   = GetElementSize(memory, *context.property_layout, member);

        if (type == "FloatProperty" || type == "DoubleProperty") {
            entry.is_float = true;
        } else if (type == "IntProperty" || type == "Int64Property" ||
                   type == "Int16Property" || type == "Int8Property") {
            entry.is_signed = true;
        } else if (type == "ByteProperty" || type == "UInt16Property" ||
                   type == "UInt32Property" || type == "UInt64Property") {
            entry.is_signed = false;
        } else {
            return false;   // anything else and the struct is not ours to write
        }

        out.push_back(std::move(entry));
    }
    return true;
}

// Accepts what the reader prints, "{X=1, Y=2, Z=3}", and the shorter "1,2,3" for the same
// members in order. Names win when present, so a partial "{Z=500}" moves one axis and
// leaves the others alone.
bool ParseStructText(std::string_view text, const std::vector<PlainMember>& members,
                     std::vector<std::pair<std::size_t, std::string>>& out,
                     std::string& error) {
    std::string body = Trim(text);
    if (!body.empty() && body.front() == '{' && body.back() == '}')
        body = Trim(body.substr(1, body.size() - 2));

    if (body.empty()) {
        error = "expected a value";
        return false;
    }

    std::vector<std::string> pieces;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i != body.size() && body[i] != ',') continue;
        pieces.push_back(Trim(std::string_view(body).substr(start, i - start)));
        start = i + 1;
    }

    const bool named = pieces.front().find('=') != std::string::npos;

    if (!named) {
        if (pieces.size() != members.size()) {
            error = std::format("expected {} values, got {}", members.size(), pieces.size());
            return false;
        }
        for (std::size_t i = 0; i < pieces.size(); ++i) out.emplace_back(i, pieces[i]);
        return true;
    }

    for (const auto& piece : pieces) {
        const auto equals = piece.find('=');
        if (equals == std::string::npos) {
            error = "mixed named and positional values";
            return false;
        }
        const std::string key = Trim(std::string_view(piece).substr(0, equals));
        const std::string value = Trim(std::string_view(piece).substr(equals + 1));

        const auto it = std::find_if(members.begin(), members.end(),
                                     [&](const PlainMember& m) {
                                         return EqualsNoCase(m.name, key);
                                     });
        if (it == members.end()) {
            error = std::format("no member named '{}'", key);
            return false;
        }
        out.emplace_back(static_cast<std::size_t>(it - members.begin()), value);
    }
    return true;
}

WriteResult Fail(std::string reason) { return {false, std::move(reason), {}}; }

WriteResult Ok(std::string written) { return {true, {}, std::move(written)}; }

// Writes exactly `width` bytes of `value`. Little-endian, which is the only layout this
// tool targets.
WriteResult WriteInteger(core::IMemorySource& memory, Address at, std::int64_t value,
                         int width, bool is_signed, std::string_view label) {
    if (!FitsIn(value, width, is_signed))
        return Fail(std::format("{} does not fit in {} ({} byte{})", value, label, width,
                                width == 1 ? "" : "s"));

    std::uint64_t raw = static_cast<std::uint64_t>(value);
    if (!memory.Write(at, &raw, static_cast<std::size_t>(width)))
        return Fail("the write was refused by the target");

    return Ok(std::format("{}", value));
}

const char* PropertyType(const ResolveContext& context, Address field) {
    static thread_local std::string name;
    name = GetPropertyTypeName(*context.memory, *context.property_layout, *context.pool,
                               field);
    return name.c_str();
}

// The kinds whose bytes are entirely owned by the property and carry no bookkeeping.
bool IsWritableTypeName(std::string_view type) {
    static constexpr std::string_view kWritable[] = {
        "BoolProperty",   "Int8Property",  "Int16Property", "IntProperty",
        "Int64Property",  "ByteProperty",  "UInt16Property", "UInt32Property",
        "UInt64Property", "FloatProperty", "DoubleProperty", "EnumProperty",
    };
    return std::find(std::begin(kWritable), std::end(kWritable), type) !=
           std::end(kWritable);
}

bool IsPlainStruct(const ResolveContext& context, Address field) {
    const auto resolved = ResolveType(context, field);
    std::vector<PlainMember> members;
    return CollectPlainMembers(context, resolved.referenced_object, members);
}

} // namespace

bool IsPropertyWritable(const ResolveContext& context, Address field) {
    if (!context.memory || !context.property_layout || !context.pool) return false;

    const std::string type = PropertyType(context, field);
    if (IsWritableTypeName(type)) return true;
    return type == "StructProperty" && IsPlainStruct(context, field);
}

std::vector<std::string> EnumeratorNames(const ResolveContext& context, Address field) {
    if (!context.memory || !context.enum_layout || !context.enum_layout->Valid()) return {};

    const std::string type = PropertyType(context, field);
    if (type != "ByteProperty" && type != "EnumProperty") return {};

    const auto resolved = ResolveType(context, field);
    if (IsNull(resolved.referenced_object)) return {};

    std::vector<std::string> names;
    for (const auto& [name, value] :
         GetEnumValues(*context.memory, *context.enum_layout, *context.pool,
                       resolved.referenced_object)) {
        (void)value;
        const auto scope = name.rfind("::");
        names.push_back(scope == std::string::npos ? name : name.substr(scope + 2));
    }
    return names;
}

WriteResult WritePropertyValue(const ResolveContext& context, Address object,
                               Address field, std::string_view text) {
    if (!context.memory || !context.property_layout || !context.pool)
        return Fail("no target");

    auto& memory = *context.memory;
    if (!memory.Caps().writable)
        return Fail("writes are not enabled on this target");

    const auto& properties = *context.property_layout;
    const std::int32_t offset = GetPropertyOffset(memory, properties, field);
    const Address at = object + static_cast<std::uint64_t>(offset);
    const std::int32_t width = GetElementSize(memory, properties, field);

    const std::string type = GetPropertyTypeName(memory, properties, *context.pool, field);

    if (type == "StructProperty") {
        const auto resolved = ResolveType(context, field);
        std::vector<PlainMember> members;
        if (!CollectPlainMembers(context, resolved.referenced_object, members))
            return Fail("only structs whose members are all numbers can be written");

        std::vector<std::pair<std::size_t, std::string>> assignments;
        std::string error;
        if (!ParseStructText(text, members, assignments, error)) return Fail(error);

        // Parse everything before writing anything. A struct half-updated because the
        // third value was a typo is worse than one not updated at all.
        struct Pending { Address at; std::int32_t size; bool is_float; std::int64_t whole;
                         double real; };
        std::vector<Pending> pending;

        for (const auto& [index, value] : assignments) {
            const PlainMember& member = members[index];
            Pending item;
            item.at       = at + static_cast<std::uint64_t>(member.offset);
            item.size     = member.size;
            item.is_float = member.is_float;

            if (member.is_float) {
                if (!ParseDouble(value, item.real))
                    return Fail(std::format("{}: expected a number", member.name));
            } else {
                if (!ParseSigned(value, item.whole))
                    return Fail(std::format("{}: expected a whole number", member.name));
                if (!FitsIn(item.whole, member.size, member.is_signed))
                    return Fail(std::format("{}: {} does not fit in {} byte{}",
                                            member.name, item.whole, member.size,
                                            member.size == 1 ? "" : "s"));
            }
            pending.push_back(item);
        }

        for (const auto& item : pending) {
            bool ok = false;
            if (item.is_float && item.size == 4) {
                const float value = static_cast<float>(item.real);
                ok = memory.Write(item.at, &value, sizeof(value));
            } else if (item.is_float) {
                ok = memory.Write(item.at, &item.real, sizeof(item.real));
            } else {
                const std::uint64_t raw = static_cast<std::uint64_t>(item.whole);
                ok = memory.Write(item.at, &raw, static_cast<std::size_t>(item.size));
            }
            if (!ok) return Fail("the write was refused by the target");
        }

        return Ok(std::string(Trim(text)));
    }

    if (!IsWritableTypeName(type))
        return Fail(std::format("{} cannot be written safely", type));

    if (type == "BoolProperty") {
        bool value{};
        if (!ParseBool(text, value)) return Fail("expected true or false");

        const auto bits = ResolveBitfield(context, field);
        if (bits.is_bitfield) {
            // Read, flip one bit, write back. Writing the byte whole would clear the
            // other seven flags packed beside it.
            std::uint8_t raw{};
            if (!core::ReadInto(memory, at, raw))
                return Fail("could not read the byte the flag is packed into");

            const std::uint8_t updated = value ? static_cast<std::uint8_t>(raw | bits.field_mask)
                                               : static_cast<std::uint8_t>(raw & ~bits.field_mask);
            if (!memory.Write(at, &updated, 1))
                return Fail("the write was refused by the target");
            return Ok(value ? "true" : "false");
        }

        const std::uint8_t raw = value ? 1 : 0;
        if (!memory.Write(at, &raw, 1)) return Fail("the write was refused by the target");
        return Ok(value ? "true" : "false");
    }

    if (type == "FloatProperty") {
        double parsed{};
        if (!ParseDouble(text, parsed)) return Fail("expected a number");
        const float value = static_cast<float>(parsed);
        if (!memory.Write(at, &value, sizeof(value)))
            return Fail("the write was refused by the target");
        return Ok(std::format("{}", value));
    }

    if (type == "DoubleProperty") {
        double value{};
        if (!ParseDouble(text, value)) return Fail("expected a number");
        if (!memory.Write(at, &value, sizeof(value)))
            return Fail("the write was refused by the target");
        return Ok(std::format("{}", value));
    }

    if (type == "ByteProperty" || type == "EnumProperty") {
        std::int64_t value = 0;
        if (!ParseSigned(text, value)) {
            // Not a number, so try it as an enumerator. Matching on the short name is
            // what the reader prints, and the fully qualified form is accepted too.
            const auto resolved = ResolveType(context, field);
            bool matched = false;
            if (context.enum_layout && context.enum_layout->Valid() &&
                !IsNull(resolved.referenced_object)) {
                for (const auto& [name, entry] :
                     GetEnumValues(memory, *context.enum_layout, *context.pool,
                                   resolved.referenced_object)) {
                    const auto scope = name.rfind("::");
                    const std::string leaf =
                        scope == std::string::npos ? name : name.substr(scope + 2);
                    if (!EqualsNoCase(Trim(text), leaf) && !EqualsNoCase(Trim(text), name))
                        continue;
                    value = entry;
                    matched = true;
                    break;
                }
            }
            if (!matched) return Fail("expected a number or an enumerator name");
        }
        return WriteInteger(memory, at, value, width > 0 ? width : 1, false, type);
    }

    std::int64_t value = 0;
    if (!ParseSigned(text, value)) return Fail("expected a whole number");

    const bool is_signed = type == "Int8Property" || type == "Int16Property" ||
                           type == "IntProperty"  || type == "Int64Property";
    return WriteInteger(memory, at, value, width > 0 ? width : 4, is_signed, type);
}

} // namespace zircon::engine
