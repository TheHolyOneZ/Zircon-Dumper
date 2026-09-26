#include "mono/Assembly.h"

#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <unordered_map>

namespace zircon::mono {
namespace {

constexpr std::uint32_t kTableCount = 64;

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at{0};

    bool Ok(std::size_t need) const { return at + need <= data.size(); }

    std::uint8_t U8() {
        if (!Ok(1)) { at = data.size() + 1; return 0; }
        return data[at++];
    }
    std::uint16_t U16() {
        if (!Ok(2)) { at = data.size() + 1; return 0; }
        const std::uint16_t v = static_cast<std::uint16_t>(data[at]) |
                                static_cast<std::uint16_t>(data[at + 1]) << 8;
        at += 2;
        return v;
    }
    std::uint32_t U32() {
        if (!Ok(4)) { at = data.size() + 1; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[at + i]) << (i * 8);
        at += 4;
        return v;
    }
    std::uint64_t U64() {
        std::uint64_t lo = U32();
        std::uint64_t hi = U32();
        return lo | (hi << 32);
    }
    bool Bad() const { return at > data.size(); }
};

std::uint32_t ReadAt32(std::span<const std::uint8_t> data, std::size_t at) {
    if (at + 4 > data.size()) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[at + i]) << (i * 8);
    return v;
}

std::uint16_t ReadAt16(std::span<const std::uint8_t> data, std::size_t at) {
    if (at + 2 > data.size()) return 0;
    return static_cast<std::uint16_t>(data[at]) | static_cast<std::uint16_t>(data[at + 1]) << 8;
}

struct Section {
    std::uint32_t rva{0};
    std::uint32_t vsize{0};
    std::uint32_t raw{0};
    std::uint32_t raw_size{0};
};

struct Image {
    std::vector<std::uint8_t> bytes;
    std::vector<Section>      sections;

    std::size_t Offset(std::uint32_t rva) const {
        for (const auto& s : sections) {
            const std::uint32_t span = std::max(s.vsize, s.raw_size);
            if (rva >= s.rva && rva < s.rva + span) {
                const std::uint32_t delta = rva - s.rva;
                if (delta >= s.raw_size) return 0;
                return s.raw + delta;
            }
        }
        return 0;
    }
};

bool LoadImage(const std::string& path, Image& image, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error = "cannot open " + path; return false; }

    const auto size = static_cast<std::size_t>(in.tellg());
    if (size < 0x100 || size > 512u * 1024 * 1024) {
        error = "not a plausible assembly size";
        return false;
    }
    in.seekg(0);
    image.bytes.resize(size);
    if (!in.read(reinterpret_cast<char*>(image.bytes.data()),
                 static_cast<std::streamsize>(size))) {
        error = "cannot read " + path;
        return false;
    }

    std::span<const std::uint8_t> b{image.bytes};
    if (ReadAt16(b, 0) != 0x5A4D) { error = "no MZ header"; return false; }

    const auto pe = ReadAt32(b, 0x3C);
    if (pe == 0 || pe + 0x18 > size) { error = "bad e_lfanew"; return false; }
    if (ReadAt32(b, pe) != 0x00004550) { error = "no PE signature"; return false; }

    const auto nsect = ReadAt16(b, pe + 6);
    const auto optsz = ReadAt16(b, pe + 20);
    const std::size_t opt = pe + 24;
    const auto magic = ReadAt16(b, opt);
    if (magic != 0x10B && magic != 0x20B) { error = "unknown optional header"; return false; }

    const std::size_t sect = opt + optsz;
    for (std::uint16_t i = 0; i < nsect; ++i) {
        const std::size_t o = sect + static_cast<std::size_t>(i) * 40;
        if (o + 40 > size) break;
        Section s;
        s.vsize    = ReadAt32(b, o + 8);
        s.rva      = ReadAt32(b, o + 12);
        s.raw_size = ReadAt32(b, o + 16);
        s.raw      = ReadAt32(b, o + 20);
        image.sections.push_back(s);
    }

    const std::size_t dirs = opt + (magic == 0x20B ? 112 : 96);
    const auto cor_rva = ReadAt32(b, dirs + 14 * 8);
    if (cor_rva == 0) { error = "not a managed assembly"; return false; }
    return true;
}

struct Streams {
    std::span<const std::uint8_t> tables;
    std::span<const std::uint8_t> strings;
    std::span<const std::uint8_t> blobs;
    std::span<const std::uint8_t> guids;
    std::string runtime_version;
};

bool LoadStreams(const Image& image, Streams& out, std::string& error) {
    std::span<const std::uint8_t> b{image.bytes};
    const auto pe = ReadAt32(b, 0x3C);
    const std::size_t opt = pe + 24;
    const auto magic = ReadAt16(b, opt);
    const std::size_t dirs = opt + (magic == 0x20B ? 112 : 96);

    const auto cor_rva = ReadAt32(b, dirs + 14 * 8);
    const auto cor = image.Offset(cor_rva);
    if (cor == 0) { error = "CLI header is not in a mapped section"; return false; }

    const auto meta_rva  = ReadAt32(b, cor + 8);
    const auto meta_size = ReadAt32(b, cor + 12);
    const auto meta = image.Offset(meta_rva);
    if (meta == 0 || meta_size == 0 || meta + meta_size > image.bytes.size()) {
        error = "metadata root is out of range";
        return false;
    }

    if (ReadAt32(b, meta) != 0x424A5342) { error = "no BSJB signature"; return false; }

    const auto version_length = ReadAt32(b, meta + 12);
    if (version_length > 256) { error = "implausible version string"; return false; }
    out.runtime_version.assign(reinterpret_cast<const char*>(b.data() + meta + 16),
                               version_length);
    while (!out.runtime_version.empty() && out.runtime_version.back() == '\0')
        out.runtime_version.pop_back();

    std::size_t at = meta + 16 + version_length;
    at += 2;
    const auto stream_count = ReadAt16(b, at);
    at += 2;

    for (std::uint16_t i = 0; i < stream_count && i < 16; ++i) {
        const auto offset = ReadAt32(b, at);
        const auto size   = ReadAt32(b, at + 4);
        at += 8;

        std::string name;
        while (at < image.bytes.size() && b[at] != 0) name.push_back(static_cast<char>(b[at++]));
        ++at;
        at = (at + 3) & ~static_cast<std::size_t>(3);

        if (meta + offset + size > image.bytes.size()) continue;
        const auto span = b.subspan(meta + offset, size);

        if (name == "#~" || name == "#-")   out.tables  = span;
        else if (name == "#Strings")        out.strings = span;
        else if (name == "#Blob")           out.blobs   = span;
        else if (name == "#GUID")           out.guids   = span;
    }

    if (out.tables.empty()) { error = "no table stream"; return false; }
    return true;
}

struct Schema {
    std::array<std::uint32_t, kTableCount> rows{};
    std::array<std::size_t, kTableCount>   start{};
    std::array<std::uint32_t, kTableCount> width{};
    bool wide_strings{false};
    bool wide_guids{false};
    bool wide_blobs{false};

    bool Present(Table t) const { return rows[static_cast<std::uint32_t>(t)] > 0; }
    std::uint32_t Rows(Table t) const { return rows[static_cast<std::uint32_t>(t)]; }
};

std::uint32_t IndexWidth(const Schema& schema, std::initializer_list<Table> candidates,
                         std::uint32_t tag_bits) {
    std::uint32_t largest = 0;
    for (const Table t : candidates) largest = std::max(largest, schema.Rows(t));
    const std::uint32_t ceiling = 1u << (16 - tag_bits);
    return largest < ceiling ? 2u : 4u;
}

std::uint32_t SimpleIndex(const Schema& schema, Table t) {
    return schema.Rows(t) < 0x10000 ? 2u : 4u;
}

std::uint32_t RowWidth(const Schema& schema, Table table) {
    const auto str  = schema.wide_strings ? 4u : 2u;
    const auto blob = schema.wide_blobs ? 4u : 2u;
    const auto guid = schema.wide_guids ? 4u : 2u;

    const auto type_def_or_ref =
        IndexWidth(schema, {Table::TypeDef, Table::TypeRef, Table::TypeSpec}, 2);
    const auto has_constant =
        IndexWidth(schema, {Table::Field, Table::Param, Table::Property}, 2);
    const auto has_semantics = IndexWidth(schema, {Table::Event, Table::Property}, 1);
    const auto has_custom_attribute = IndexWidth(
        schema, {Table::MethodDef, Table::Field, Table::TypeRef, Table::TypeDef, Table::Param,
                 Table::InterfaceImpl, Table::MemberRef, Table::Module, Table::DeclSecurity,
                 Table::Property, Table::Event, Table::StandAloneSig, Table::ModuleRef,
                 Table::TypeSpec, Table::Assembly, Table::AssemblyRef, Table::File,
                 Table::ExportedType, Table::ManifestResource, Table::GenericParam,
                 Table::GenericParamConstraint, Table::MethodSpec}, 5);
    const auto custom_attribute_type =
        IndexWidth(schema, {Table::MethodDef, Table::MemberRef}, 3);
    const auto has_decl_security =
        IndexWidth(schema, {Table::TypeDef, Table::MethodDef, Table::Assembly}, 2);
    const auto member_forwarded = IndexWidth(schema, {Table::Field, Table::MethodDef}, 1);
    const auto implementation =
        IndexWidth(schema, {Table::File, Table::AssemblyRef, Table::ExportedType}, 2);
    const auto resolution_scope = IndexWidth(
        schema, {Table::Module, Table::ModuleRef, Table::AssemblyRef, Table::TypeRef}, 2);
    const auto method_def_or_ref = IndexWidth(schema, {Table::MethodDef, Table::MemberRef}, 1);
    const auto type_or_method_def = IndexWidth(schema, {Table::TypeDef, Table::MethodDef}, 1);
    const auto has_field_marshal = IndexWidth(schema, {Table::Field, Table::Param}, 1);
    const auto member_ref_parent = IndexWidth(
        schema, {Table::TypeDef, Table::TypeRef, Table::ModuleRef, Table::MethodDef,
                 Table::TypeSpec}, 3);

    switch (table) {
        case Table::Module:    return 2 + str + 3 * guid;
        case Table::TypeRef:   return resolution_scope + 2 * str;
        case Table::TypeDef:
            return 4 + 2 * str + type_def_or_ref + SimpleIndex(schema, Table::Field) +
                   SimpleIndex(schema, Table::MethodDef);
        case Table::Field:     return 2 + str + blob;
        case Table::MethodDef:
            return 4 + 2 + 2 + str + blob + SimpleIndex(schema, Table::Param);
        case Table::Param:     return 2 + 2 + str;
        case Table::InterfaceImpl:
            return SimpleIndex(schema, Table::TypeDef) + type_def_or_ref;
        case Table::MemberRef: return member_ref_parent + str + blob;
        case Table::Constant:  return 1 + 1 + has_constant + blob;
        case Table::CustomAttribute:
            return has_custom_attribute + custom_attribute_type + blob;
        case Table::FieldMarshal:  return has_field_marshal + blob;
        case Table::DeclSecurity:  return 2 + has_decl_security + blob;
        case Table::ClassLayout:   return 2 + 4 + SimpleIndex(schema, Table::TypeDef);
        case Table::FieldLayout:   return 4 + SimpleIndex(schema, Table::Field);
        case Table::StandAloneSig: return blob;
        case Table::EventMap:
            return SimpleIndex(schema, Table::TypeDef) + SimpleIndex(schema, Table::Event);
        case Table::Event:         return 2 + str + type_def_or_ref;
        case Table::PropertyMap:
            return SimpleIndex(schema, Table::TypeDef) + SimpleIndex(schema, Table::Property);
        case Table::Property:      return 2 + str + blob;
        case Table::MethodSemantics:
            return 2 + SimpleIndex(schema, Table::MethodDef) + has_semantics;
        case Table::MethodImpl:
            return SimpleIndex(schema, Table::TypeDef) + 2 * method_def_or_ref;
        case Table::ModuleRef:     return str;
        case Table::TypeSpec:      return blob;
        case Table::ImplMap:       return 2 + member_forwarded + str +
                                          SimpleIndex(schema, Table::ModuleRef);
        case Table::FieldRva:      return 4 + SimpleIndex(schema, Table::Field);
        case Table::Assembly:      return 4 + 4 * 2 + 4 + blob + 2 * str;
        case Table::AssemblyRef:   return 4 * 2 + 4 + blob + 2 * str + blob;
        case Table::File:          return 4 + str + blob;
        case Table::ExportedType:  return 4 + 4 + 2 * str + implementation;
        case Table::ManifestResource: return 4 + 4 + str + implementation;
        case Table::NestedClass:   return 2 * SimpleIndex(schema, Table::TypeDef);
        case Table::GenericParam:  return 2 + 2 + type_or_method_def + str;
        case Table::MethodSpec:    return method_def_or_ref + blob;
        case Table::GenericParamConstraint:
            return SimpleIndex(schema, Table::GenericParam) + type_def_or_ref;
        default: return 0;
    }
}

class Metadata {
public:
    Metadata(const Image& image, const Streams& streams)
        : image_(image), streams_(streams) {}

    bool Parse(std::string& error) {
        auto t = streams_.tables;
        if (t.size() < 24) { error = "table stream is too small"; return false; }

        const std::uint8_t heap_sizes = t[6];
        schema_.wide_strings = (heap_sizes & 0x01) != 0;
        schema_.wide_guids   = (heap_sizes & 0x02) != 0;
        schema_.wide_blobs   = (heap_sizes & 0x04) != 0;

        std::uint64_t valid = 0;
        for (int i = 0; i < 8; ++i) valid |= static_cast<std::uint64_t>(t[8 + i]) << (i * 8);

        std::size_t at = 24;
        for (std::uint32_t i = 0; i < kTableCount; ++i) {
            if (!(valid & (1ull << i))) continue;
            if (at + 4 > t.size()) { error = "row counts run past the stream"; return false; }
            schema_.rows[i] = ReadAt32(t, at);
            at += 4;
        }

        for (std::uint32_t i = 0; i < kTableCount; ++i) {
            if (schema_.rows[i] == 0) continue;
            schema_.width[i] = RowWidth(schema_, static_cast<Table>(i));
            if (schema_.width[i] == 0) {
                error = std::format("table 0x{:02X} has no known shape", i);
                return false;
            }
        }

        for (std::uint32_t i = 0; i < kTableCount; ++i) {
            if (schema_.rows[i] == 0) continue;
            schema_.start[i] = at;
            at += static_cast<std::size_t>(schema_.rows[i]) * schema_.width[i];
            if (at > t.size()) {
                error = std::format("table 0x{:02X} runs past the stream", i);
                return false;
            }
        }
        return true;
    }

    const Schema& schema() const { return schema_; }

    std::size_t RowAt(Table table, std::uint32_t index) const {
        const auto id = static_cast<std::uint32_t>(table);
        return schema_.start[id] + static_cast<std::size_t>(index) * schema_.width[id];
    }

    std::uint32_t Field(Table table, std::uint32_t index, std::uint32_t byte_offset,
                        std::uint32_t width) const {
        const auto at = RowAt(table, index) + byte_offset;
        if (width == 2) return ReadAt16(streams_.tables, at);
        if (width == 4) return ReadAt32(streams_.tables, at);
        if (width == 1 && at < streams_.tables.size()) return streams_.tables[at];
        return 0;
    }

    std::string String(std::uint32_t offset) const {
        if (offset >= streams_.strings.size()) return {};
        const char* begin = reinterpret_cast<const char*>(streams_.strings.data()) + offset;
        const std::size_t room = streams_.strings.size() - offset;
        std::size_t length = 0;
        while (length < room && begin[length] != 0) ++length;
        return std::string(begin, length);
    }

    std::span<const std::uint8_t> Blob(std::uint32_t offset) const {
        if (offset >= streams_.blobs.size()) return {};
        std::size_t at = offset;
        std::uint32_t length = 0;
        const std::uint8_t first = streams_.blobs[at];
        if ((first & 0x80) == 0) { length = first & 0x7F; at += 1; }
        else if ((first & 0xC0) == 0x80) {
            length = ((first & 0x3Fu) << 8) | streams_.blobs[at + 1];
            at += 2;
        } else {
            length = ((first & 0x1Fu) << 24) |
                     (static_cast<std::uint32_t>(streams_.blobs[at + 1]) << 16) |
                     (static_cast<std::uint32_t>(streams_.blobs[at + 2]) << 8) |
                     streams_.blobs[at + 3];
            at += 4;
        }
        if (at + length > streams_.blobs.size()) return {};
        return streams_.blobs.subspan(at, length);
    }

    std::string Guid(std::uint32_t index) const {
        if (index == 0) return {};
        const std::size_t at = static_cast<std::size_t>(index - 1) * 16;
        if (at + 16 > streams_.guids.size()) return {};
        const auto* g = streams_.guids.data() + at;
        return std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
                           "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                           ReadAt32(streams_.guids, at), ReadAt16(streams_.guids, at + 4),
                           ReadAt16(streams_.guids, at + 6),
                           g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    }

    std::uint32_t StringWidth() const { return schema_.wide_strings ? 4u : 2u; }
    std::uint32_t BlobWidth()   const { return schema_.wide_blobs ? 4u : 2u; }
    std::uint32_t GuidWidth()   const { return schema_.wide_guids ? 4u : 2u; }

private:
    const Image&   image_;
    const Streams& streams_;
    Schema         schema_{};
};

std::string ElementName(std::uint8_t element) {
    switch (element) {
        case 0x02: return "bool";
        case 0x03: return "char";
        case 0x04: return "int8";
        case 0x05: return "uint8";
        case 0x06: return "int16";
        case 0x07: return "uint16";
        case 0x08: return "int32";
        case 0x09: return "uint32";
        case 0x0A: return "int64";
        case 0x0B: return "uint64";
        default:   return "int32";
    }
}

std::int64_t DecodeConstant(std::uint8_t type, std::span<const std::uint8_t> blob) {
    const auto need = [&](std::size_t n) { return blob.size() >= n; };
    switch (type) {
        case 0x02: return need(1) ? (blob[0] ? 1 : 0) : 0;
        case 0x04: return need(1) ? static_cast<std::int8_t>(blob[0]) : 0;
        case 0x05: return need(1) ? blob[0] : 0;
        case 0x03:
        case 0x07: return need(2) ? ReadAt16(blob, 0) : 0;
        case 0x06: return need(2) ? static_cast<std::int16_t>(ReadAt16(blob, 0)) : 0;
        case 0x08: return need(4) ? static_cast<std::int32_t>(ReadAt32(blob, 0)) : 0;
        case 0x09: return need(4) ? ReadAt32(blob, 0) : 0;
        case 0x0A:
        case 0x0B: {
            if (!need(8)) return 0;
            std::uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(blob[i]) << (i * 8);
            return static_cast<std::int64_t>(v);
        }
        default: return 0;
    }
}

} // namespace

std::string AssemblyMetadata::FullNameOf(std::size_t index) const {
    if (index >= types.size()) return {};
    const auto& row = types[index];

    std::string nested = row.name;
    std::int32_t outer = row.enclosing;
    int depth = 0;
    while (outer >= 0 && static_cast<std::size_t>(outer) < types.size() && depth++ < 16) {
        nested = types[static_cast<std::size_t>(outer)].name + "." + nested;
        outer = types[static_cast<std::size_t>(outer)].enclosing;
    }

    std::size_t root = index;
    depth = 0;
    while (types[root].enclosing >= 0 && depth++ < 16)
        root = static_cast<std::size_t>(types[root].enclosing);

    const std::string& space = types[root].name_space;
    return space.empty() ? nested : space + "." + nested;
}

core::Result<AssemblyMetadata> ReadAssembly(const std::string& path) {
    Image image;
    std::string error;
    if (!LoadImage(path, image, error)) return core::Error{error, 3};

    Streams streams;
    if (!LoadStreams(image, streams, error)) return core::Error{error, 3};

    Metadata meta(image, streams);
    if (!meta.Parse(error)) return core::Error{error, 3};

    const auto& schema = meta.schema();
    AssemblyMetadata out;
    out.file = path;
    out.runtime_version = streams.runtime_version;

    const auto str = meta.StringWidth();
    const auto blob = meta.BlobWidth();
    const auto guid = meta.GuidWidth();

    if (schema.Present(Table::Module)) {
        out.name = meta.String(meta.Field(Table::Module, 0, 2, str));
        out.mvid = meta.Guid(meta.Field(Table::Module, 0, 2 + str, guid));
        if (out.name.size() > 4 && out.name.compare(out.name.size() - 4, 4, ".dll") == 0)
            out.name.resize(out.name.size() - 4);
    }

    if (schema.Present(Table::Assembly)) {
        const auto major = meta.Field(Table::Assembly, 0, 4, 2);
        const auto minor = meta.Field(Table::Assembly, 0, 6, 2);
        const auto build = meta.Field(Table::Assembly, 0, 8, 2);
        const auto rev   = meta.Field(Table::Assembly, 0, 10, 2);
        out.version = std::format("{}.{}.{}.{}", major, minor, build, rev);

        const auto name_at = 4 + 8 + 4 + blob;
        const auto asm_name = meta.String(meta.Field(Table::Assembly, 0, name_at, str));
        if (!asm_name.empty()) out.name = asm_name;
    }

    const auto resolution_scope = IndexWidth(
        schema, {Table::Module, Table::ModuleRef, Table::AssemblyRef, Table::TypeRef}, 2);
    const auto assembly_ref_name_at = 8 + 4 + blob;

    const auto TypeRefName = [&](std::uint32_t row, int depth) -> std::string {
        std::string result;
        std::uint32_t at = row;
        for (int guard = 0; guard <= depth; ++guard) {
            if (at < 1 || at > schema.Rows(Table::TypeRef)) return {};
            const auto name = meta.String(meta.Field(Table::TypeRef, at - 1,
                                                     resolution_scope, str));
            const auto space = meta.String(meta.Field(Table::TypeRef, at - 1,
                                                      resolution_scope + str, str));
            std::string full = space.empty() ? name : space + "." + name;
            result = result.empty() ? full : full + "." + result;

            const auto scope = meta.Field(Table::TypeRef, at - 1, 0, resolution_scope);
            const std::uint32_t tag = scope & 0x3;
            const std::uint32_t target = scope >> 2;

            if (tag == 3) { at = target; continue; }
            if (tag == 2 && schema.Present(Table::AssemblyRef) && target >= 1 &&
                target <= schema.Rows(Table::AssemblyRef)) {
                const auto owner = meta.String(
                    meta.Field(Table::AssemblyRef, target - 1, assembly_ref_name_at, str));
                if (!owner.empty()) result += ", " + owner;
            }
            return result;
        }
        return result;
    };

    const auto type_rows   = schema.Rows(Table::TypeDef);
    const auto field_rows  = schema.Rows(Table::Field);
    const auto method_rows = schema.Rows(Table::MethodDef);
    const auto param_rows  = schema.Rows(Table::Param);

    const auto type_def_or_ref =
        IndexWidth(schema, {Table::TypeDef, Table::TypeRef, Table::TypeSpec}, 2);
    const auto field_index  = SimpleIndex(schema, Table::Field);
    const auto method_index = SimpleIndex(schema, Table::MethodDef);
    const auto param_index  = SimpleIndex(schema, Table::Param);
    const auto type_index   = SimpleIndex(schema, Table::TypeDef);

    out.types.reserve(type_rows);

    std::vector<std::uint32_t> first_field(type_rows + 1, 0);
    std::vector<std::uint32_t> first_method(type_rows + 1, 0);

    for (std::uint32_t i = 0; i < type_rows; ++i) {
        TypeRow row;
        row.flags      = meta.Field(Table::TypeDef, i, 0, 4);
        row.name       = meta.String(meta.Field(Table::TypeDef, i, 4, str));
        row.name_space = meta.String(meta.Field(Table::TypeDef, i, 4 + str, str));
        row.extends    = meta.Field(Table::TypeDef, i, 4 + 2 * str, type_def_or_ref);
        row.token      = 0x02000000u | (i + 1);

        first_field[i]  = meta.Field(Table::TypeDef, i, 4 + 2 * str + type_def_or_ref,
                                     field_index);
        first_method[i] = meta.Field(Table::TypeDef, i,
                                     4 + 2 * str + type_def_or_ref + field_index,
                                     method_index);

        row.is_interface = (row.flags & 0x20) != 0;
        row.is_abstract  = (row.flags & 0x80) != 0;
        out.types.push_back(std::move(row));
    }
    first_field[type_rows]  = field_rows + 1;
    first_method[type_rows] = method_rows + 1;

    std::unordered_map<std::uint32_t, std::size_t> type_of_field;
    std::unordered_map<std::uint32_t, std::size_t> type_of_method;

    for (std::uint32_t i = 0; i < type_rows; ++i) {
        const std::uint32_t f_begin = first_field[i];
        const std::uint32_t f_end   = (i + 1 < type_rows) ? first_field[i + 1] : field_rows + 1;
        for (std::uint32_t f = f_begin; f < f_end && f >= 1 && f <= field_rows; ++f) {
            FieldRow field;
            field.flags     = static_cast<std::uint16_t>(meta.Field(Table::Field, f - 1, 0, 2));
            field.name      = meta.String(meta.Field(Table::Field, f - 1, 2, str));
            field.signature = meta.Field(Table::Field, f - 1, 2 + str, blob);
            type_of_field.emplace(f, i);
            out.types[i].fields.push_back(std::move(field));
        }

        const std::uint32_t m_begin = first_method[i];
        const std::uint32_t m_end   = (i + 1 < type_rows) ? first_method[i + 1] : method_rows + 1;
        for (std::uint32_t m = m_begin; m < m_end && m >= 1 && m <= method_rows; ++m) {
            MethodRow method;
            method.rva        = meta.Field(Table::MethodDef, m - 1, 0, 4);
            method.impl_flags = static_cast<std::uint16_t>(
                meta.Field(Table::MethodDef, m - 1, 4, 2));
            method.flags = static_cast<std::uint16_t>(meta.Field(Table::MethodDef, m - 1, 6, 2));
            method.name  = meta.String(meta.Field(Table::MethodDef, m - 1, 8, str));
            method.signature = meta.Field(Table::MethodDef, m - 1, 8 + str, blob);
            method.first_param = meta.Field(Table::MethodDef, m - 1, 8 + str + blob,
                                            param_index);
            method.token = 0x06000000u | m;
            type_of_method.emplace(m, i);
            out.types[i].methods.push_back(std::move(method));
        }
    }

    for (std::uint32_t i = 0; i < type_rows; ++i) {
        auto& methods = out.types[i].methods;
        for (std::size_t k = 0; k < methods.size(); ++k) {
            const std::uint32_t begin = methods[k].first_param;
            std::uint32_t end = param_rows + 1;
            if (k + 1 < methods.size()) end = methods[k + 1].first_param;
            else if (i + 1 < type_rows && !out.types[i + 1].methods.empty())
                end = out.types[i + 1].methods.front().first_param;
            methods[k].param_count = end > begin ? end - begin : 0;
        }
    }

    if (schema.Present(Table::NestedClass)) {
        const auto rows = schema.Rows(Table::NestedClass);
        for (std::uint32_t i = 0; i < rows; ++i) {
            const auto nested    = meta.Field(Table::NestedClass, i, 0, type_index);
            const auto enclosing = meta.Field(Table::NestedClass, i, type_index, type_index);
            if (nested >= 1 && nested <= type_rows && enclosing >= 1 && enclosing <= type_rows)
                out.types[nested - 1].enclosing = static_cast<std::int32_t>(enclosing - 1);
        }
    }

    if (schema.Present(Table::InterfaceImpl)) {
        const auto rows = schema.Rows(Table::InterfaceImpl);
        for (std::uint32_t i = 0; i < rows; ++i) {
            const auto klass = meta.Field(Table::InterfaceImpl, i, 0, type_index);
            const auto coded = meta.Field(Table::InterfaceImpl, i, type_index, type_def_or_ref);
            if (klass < 1 || klass > type_rows) continue;

            const std::uint32_t tag = coded & 0x3;
            const std::uint32_t row = coded >> 2;
            std::string name;
            if (tag == 0 && row >= 1 && row <= type_rows) {
                name = out.FullNameOf(row - 1);
                if (!out.name.empty()) name += ", " + out.name;
            } else if (tag == 1) {
                name = TypeRefName(row, 8);
            }
            if (!name.empty()) out.types[klass - 1].interfaces.push_back(std::move(name));
        }
    }

    if (schema.Present(Table::PropertyMap) && schema.Present(Table::Property)) {
        const auto map_rows  = schema.Rows(Table::PropertyMap);
        const auto prop_rows = schema.Rows(Table::Property);
        const auto prop_index = SimpleIndex(schema, Table::Property);

        std::unordered_map<std::uint32_t, std::string> getter_of;
        std::unordered_map<std::uint32_t, std::string> setter_of;
        if (schema.Present(Table::MethodSemantics)) {
            const auto sem_rows = schema.Rows(Table::MethodSemantics);
            const auto has_semantics = IndexWidth(schema, {Table::Event, Table::Property}, 1);
            for (std::uint32_t i = 0; i < sem_rows; ++i) {
                const auto semantics = meta.Field(Table::MethodSemantics, i, 0, 2);
                const auto method    = meta.Field(Table::MethodSemantics, i, 2, method_index);
                const auto assoc     = meta.Field(Table::MethodSemantics, i, 2 + method_index,
                                                  has_semantics);
                if ((assoc & 0x1) != 1) continue;
                const std::uint32_t property = assoc >> 1;
                if (method < 1 || method > method_rows) continue;
                const auto name = meta.String(meta.Field(Table::MethodDef, method - 1, 8, str));
                if (semantics & 0x2) getter_of[property] = name;
                if (semantics & 0x1) setter_of[property] = name;
            }
        }

        for (std::uint32_t i = 0; i < map_rows; ++i) {
            const auto owner = meta.Field(Table::PropertyMap, i, 0, type_index);
            const auto begin = meta.Field(Table::PropertyMap, i, type_index, prop_index);
            std::uint32_t end = prop_rows + 1;
            if (i + 1 < map_rows)
                end = meta.Field(Table::PropertyMap, i + 1, type_index, prop_index);
            if (owner < 1 || owner > type_rows) continue;

            for (std::uint32_t p = begin; p < end && p >= 1 && p <= prop_rows; ++p) {
                PropertyRow property;
                property.flags = static_cast<std::uint16_t>(
                    meta.Field(Table::Property, p - 1, 0, 2));
                property.name = meta.String(meta.Field(Table::Property, p - 1, 2, str));
                property.signature = meta.Field(Table::Property, p - 1, 2 + str, blob);
                if (const auto it = getter_of.find(p); it != getter_of.end())
                    property.getter = it->second;
                if (const auto it = setter_of.find(p); it != setter_of.end())
                    property.setter = it->second;
                out.types[owner - 1].properties.push_back(std::move(property));
            }
        }
    }

    for (auto& row : out.types) {
        if (row.is_interface) continue;
        const std::uint32_t tag = row.extends & 0x3;
        const std::uint32_t at  = row.extends >> 2;
        if (tag == 1 && schema.Present(Table::TypeRef) && at >= 1 &&
            at <= schema.Rows(Table::TypeRef)) {
            const auto n  = meta.String(meta.Field(Table::TypeRef, at - 1, resolution_scope,
                                                   str));
            const auto ns = meta.String(meta.Field(Table::TypeRef, at - 1,
                                                   resolution_scope + str, str));
            if (ns == "System" && n == "Enum")      { row.is_enum = true; row.is_valuetype = true; }
            else if (ns == "System" && n == "ValueType") row.is_valuetype = true;
        }
    }

    if (schema.Present(Table::Constant)) {
        const auto rows = schema.Rows(Table::Constant);
        const auto has_constant =
            IndexWidth(schema, {Table::Field, Table::Param, Table::Property}, 2);

        std::unordered_map<std::uint32_t, std::pair<std::uint8_t, std::uint32_t>> by_field;
        for (std::uint32_t i = 0; i < rows; ++i) {
            const auto kind   = static_cast<std::uint8_t>(meta.Field(Table::Constant, i, 0, 1));
            const auto parent = meta.Field(Table::Constant, i, 2, has_constant);
            const auto value  = meta.Field(Table::Constant, i, 2 + has_constant, blob);
            if ((parent & 0x3) != 0) continue;
            by_field.emplace(parent >> 2, std::make_pair(kind, value));
        }

        for (std::uint32_t f = 1; f <= field_rows; ++f) {
            const auto owner = type_of_field.find(f);
            if (owner == type_of_field.end()) continue;
            auto& row = out.types[owner->second];
            if (!row.is_enum) continue;

            const auto flags = static_cast<std::uint16_t>(meta.Field(Table::Field, f - 1, 0, 2));
            if (!(flags & 0x40)) {
                if (row.underlying.empty()) {
                    const auto sig = meta.Blob(meta.Field(Table::Field, f - 1, 2 + str, blob));
                    if (sig.size() >= 2) row.underlying = ElementName(sig[1]);
                }
                continue;
            }

            const auto name = meta.String(meta.Field(Table::Field, f - 1, 2, str));
            const auto it = by_field.find(f);
            if (it == by_field.end()) {
                row.enum_values_resolved = false;
                row.enum_values.emplace_back(name, 0);
                continue;
            }
            row.enum_values.emplace_back(name,
                                         DecodeConstant(it->second.first,
                                                        meta.Blob(it->second.second)));
        }
    }

    for (auto& row : out.types)
        if (row.is_enum && row.underlying.empty()) row.underlying = "int32";

    return out;
}

std::vector<AssemblyMetadata> ReadManagedFolder(const std::string& folder,
                                                AssemblySetStats& stats) {
    std::vector<AssemblyMetadata> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return out;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".dll") continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    stats.files = files.size();

    for (const auto& file : files) {
        auto read = ReadAssembly(file.string());
        if (!read) {
            ++stats.refused;
            stats.refusals.push_back(
                std::format("{}: {}", file.filename().string(), read.error().message));
            continue;
        }
        ++stats.read;
        stats.types += read.value().types.size();
        for (const auto& type : read.value().types)
            stats.enum_values += type.enum_values.size();
        out.push_back(std::move(read.value()));
    }
    return out;
}

} // namespace zircon::mono
