#include "Emitters.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit {
namespace {

// ReClass.NET's own file version, written into the root element.
constexpr std::string_view kFileVersion = "1";

// The entry name inside a .rcnet archive.
constexpr std::string_view kArchiveEntry = "data.xml";

// The invariant this whole emitter is built around. A ReClass class is a flat byte
// sequence, so every node must consume exactly the bytes it claims. One node of the wrong
// width shifts every field below it without a word, and the result looks plausible
// while pointing at the wrong memory.
struct NodeShape {
    std::string_view type;
    std::int32_t     size;
};

std::string XmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Not legal in XML 1.0 even escaped, and UE names have no business
                // carrying them. Drop, instead of emitting a file no parser accepts.
                if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
                break;
        }
    }
    return out;
}

std::string Base64(const std::array<std::uint8_t, 16>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = (i + 1 < bytes.size()) ? bytes[i + 1] : 0u;
        const std::uint32_t c = (i + 2 < bytes.size()) ? bytes[i + 2] : 0u;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;

        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3F] : '=');
    }
    return out;
}

// Derived from the path, never random, so re-running on the same dump gives an identical
// file. Non-deterministic output is a defect here; a random GUID makes every diff noise.
std::string UuidFor(std::string_view path) {
    std::array<std::uint8_t, 16> bytes{};

    std::uint64_t low  = 0xCBF29CE484222325ull;
    std::uint64_t high = 0x9E3779B97F4A7C15ull;
    for (const char c : path) {
        low  = (low ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ull;
        high = (high + static_cast<std::uint8_t>(c)) * 0xFF51AFD7ED558CCDull;
        high ^= high >> 29;
    }

    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i]     = static_cast<std::uint8_t>((low >> (i * 8)) & 0xFF);
        bytes[i + 8] = static_cast<std::uint8_t>((high >> (i * 8)) & 0xFF);
    }
    return Base64(bytes);
}

// Only unambiguous widths get a typed node. Everything else becomes hex bytes of exactly
// the right size: offsets stay correct, and the comment still says what lives there.
bool TypedNodeFor(const ir::Property& property, NodeShape& shape) {
    if (property.is_bitfield) return false;

    const std::int32_t size = property.size;
    switch (property.type.kind) {
        case ir::TypeKind::Bool:   if (size == 1) { shape = {"Bool",   1}; return true; } break;
        case ir::TypeKind::Int8:   if (size == 1) { shape = {"Int8",   1}; return true; } break;
        case ir::TypeKind::UInt8:  if (size == 1) { shape = {"UInt8",  1}; return true; } break;
        case ir::TypeKind::Int16:  if (size == 2) { shape = {"Int16",  2}; return true; } break;
        case ir::TypeKind::UInt16: if (size == 2) { shape = {"UInt16", 2}; return true; } break;
        case ir::TypeKind::Int32:  if (size == 4) { shape = {"Int32",  4}; return true; } break;
        case ir::TypeKind::UInt32: if (size == 4) { shape = {"UInt32", 4}; return true; } break;
        case ir::TypeKind::Int64:  if (size == 8) { shape = {"Int64",  8}; return true; } break;
        case ir::TypeKind::UInt64: if (size == 8) { shape = {"UInt64", 8}; return true; } break;
        case ir::TypeKind::Float:  if (size == 4) { shape = {"Float",  4}; return true; } break;
        case ir::TypeKind::Double: if (size == 8) { shape = {"Double", 8}; return true; } break;
        default: break;
    }
    return false;
}

// For anything emitted as hex bytes this comment is the only place the real type survives,
// so carry the container shape and not just the outer name.
std::string DescribeType(const ir::TypeRef& type) {
    const auto leaf = [](const std::string& path) {
        return path.empty() ? std::string("?") : util::LeafName(path);
    };

    switch (type.kind) {
        case ir::TypeKind::Array:
            return "TArray<" + (type.params.empty() ? std::string("?")
                                                    : DescribeType(type.params[0])) + ">";
        case ir::TypeKind::Set:
            return "TSet<" + (type.params.empty() ? std::string("?")
                                                  : DescribeType(type.params[0])) + ">";
        case ir::TypeKind::Optional:
            return "TOptional<" + (type.params.empty() ? std::string("?")
                                                       : DescribeType(type.params[0])) + ">";
        case ir::TypeKind::Map:
            return "TMap<" + (type.params.size() > 0 ? DescribeType(type.params[0])
                                                     : std::string("?")) + ", " +
                   (type.params.size() > 1 ? DescribeType(type.params[1])
                                           : std::string("?")) + ">";
        case ir::TypeKind::Struct:       return "F" + leaf(type.name);
        case ir::TypeKind::Enum:         return leaf(type.name);
        case ir::TypeKind::ObjectPtr:    return leaf(type.name) + "*";
        case ir::TypeKind::ClassPtr:     return "TSubclassOf<" + leaf(type.name) + ">";
        case ir::TypeKind::WeakPtr:      return "TWeakObjectPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::LazyPtr:      return "TLazyObjectPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::SoftPtr:      return "TSoftObjectPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::SoftClassPtr: return "TSoftClassPtr<" + leaf(type.name) + ">";
        case ir::TypeKind::Interface:    return "TScriptInterface<" + leaf(type.name) + ">";
        case ir::TypeKind::Name:         return "FName";
        case ir::TypeKind::String:       return "FString";
        case ir::TypeKind::Text:         return "FText";
        case ir::TypeKind::Bool:         return "bool";
        case ir::TypeKind::Int8:         return "int8";
        case ir::TypeKind::Int16:        return "int16";
        case ir::TypeKind::Int32:        return "int32";
        case ir::TypeKind::Int64:        return "int64";
        case ir::TypeKind::UInt8:        return "uint8";
        case ir::TypeKind::UInt16:       return "uint16";
        case ir::TypeKind::UInt32:       return "uint32";
        case ir::TypeKind::UInt64:       return "uint64";
        case ir::TypeKind::Float:        return "float";
        case ir::TypeKind::Double:       return "double";
        case ir::TypeKind::Delegate:     return "FDelegate";
        case ir::TypeKind::MulticastDelegate: return "FMulticastDelegate";
        case ir::TypeKind::FieldPath:    return "TFieldPath";
        case ir::TypeKind::Unknown:      break;
    }
    return type.raw.empty() ? "unknown" : type.raw;
}

class XmlBuilder {
public:
    void Line(int indent, std::string_view text) {
        out_.append(static_cast<std::size_t>(indent) * 2, ' ');
        out_.append(text);
        out_.push_back('\n');
    }

    const std::string& Data() const { return out_; }

private:
    std::string out_;
};

// Largest nodes first. The offset goes in the name so every node in a class is uniquely
// identifiable, which matters the moment a user edits one by hand.
void EmitPadding(XmlBuilder& xml, std::int32_t at, std::int32_t bytes,
                 std::string_view comment) {
    struct Chunk { std::int32_t size; std::string_view type; };
    static constexpr Chunk kChunks[] = {
        {8, "Hex64"}, {4, "Hex32"}, {2, "Hex16"}, {1, "Hex8"},
    };

    std::int32_t cursor = at;
    std::int32_t left   = bytes;

    for (const auto& chunk : kChunks) {
        while (left >= chunk.size) {
            xml.Line(3, "<node type=\"" + std::string(chunk.type) + "\" name=\"pad_" +
                            std::to_string(cursor) + "\" comment=\"" +
                            XmlEscape(comment) + "\" />");
            cursor += chunk.size;
            left   -= chunk.size;
        }
    }
}

// CRC-32 (IEEE) for the archive entry. Computed on the fly to keep the emitter free of
// global state.
std::uint32_t Crc32(std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char raw : data) {
        crc ^= static_cast<std::uint8_t>(raw);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

void PutU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void PutU32(std::string& out, std::uint32_t value) {
    PutU16(out, static_cast<std::uint16_t>(value & 0xFFFF));
    PutU16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFF));
}

// .rcnet is a zip around data.xml, so building one here makes the output directly openable
// instead of something the user repackages by hand. Stored entries need no deflate, so this
// is the archive structure and nothing else.
std::string MakeArchive(std::string_view entry_name, std::string_view payload) {
    const auto crc  = Crc32(payload);
    const auto size = static_cast<std::uint32_t>(payload.size());
    const auto name_length = static_cast<std::uint16_t>(entry_name.size());

    std::string out;

    PutU32(out, 0x04034B50u);          // local file header
    PutU16(out, 20);                   // version needed
    PutU16(out, 0);                    // flags
    PutU16(out, 0);                    // stored
    PutU16(out, 0);                    // mod time
    PutU16(out, 0x0021);               // mod date: 1980-01-01, the epoch of the format
    PutU32(out, crc);
    PutU32(out, size);
    PutU32(out, size);
    PutU16(out, name_length);
    PutU16(out, 0);                    // extra length
    out.append(entry_name);
    out.append(payload);

    const auto central_offset = static_cast<std::uint32_t>(out.size());

    PutU32(out, 0x02014B50u);          // central directory header
    PutU16(out, 20);                   // version made by
    PutU16(out, 20);                   // version needed
    PutU16(out, 0);
    PutU16(out, 0);
    PutU16(out, 0);
    PutU16(out, 0x0021);
    PutU32(out, crc);
    PutU32(out, size);
    PutU32(out, size);
    PutU16(out, name_length);
    PutU16(out, 0);                    // extra
    PutU16(out, 0);                    // comment
    PutU16(out, 0);                    // disk number
    PutU16(out, 0);                    // internal attributes
    PutU32(out, 0);                    // external attributes
    PutU32(out, 0);                    // offset of local header
    out.append(entry_name);

    const auto central_size = static_cast<std::uint32_t>(out.size()) - central_offset;

    PutU32(out, 0x06054B50u);          // end of central directory
    PutU16(out, 0);
    PutU16(out, 0);
    PutU16(out, 1);
    PutU16(out, 1);
    PutU32(out, central_size);
    PutU32(out, central_offset);
    PutU16(out, 0);                    // comment length

    return out;
}

std::string FileStem(const ir::Dump& dump) {
    std::string source = dump.header.source.process;
    if (source.empty()) source = dump.header.source.main_module;
    if (source.empty()) return "reclass";

    const auto dot = source.find_last_of('.');
    if (dot != std::string::npos && dot > 0) source.resize(dot);

    std::string stem;
    stem.reserve(source.size());
    for (const char c : source) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        stem.push_back(safe ? c : '_');
    }
    return stem.empty() ? "reclass" : stem;
}

bool PackageIncluded(const ir::Package& package, const EmitOptions& options) {
    return options.package_filter.empty() ||
           package.name.find(options.package_filter) != std::string::npos;
}

} // namespace

EmitResult EmitReClass(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "reclass needs a complete dump; this one is partial "
                       "(pass allow_partial to emit anyway)";
        return result;
    }

    XmlBuilder xml;
    xml.Line(0, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    xml.Line(0, "<reclass type=\"ReClass.NET\" version=\"" + std::string(kFileVersion) +
                    "\" platform=\"x64\">");
    xml.Line(1, "<custom_data />");

    // No enums, deliberately. There's no way for this emitter to verify against
    // ReClass.NET's enum schema, and a section it rejects makes the whole file unloadable.
    // Enum-typed fields carry their type name in the node comment instead.
    xml.Line(1, "<enums />");
    xml.Line(1, "<classes>");

    std::size_t emitted = 0;

    for (const auto& package : dump.packages) {
        if (!PackageIncluded(package, options)) continue;

        for (const auto* list : {&package.classes, &package.structs}) {
            for (const auto& record : *list) {
                if (record.size <= 0) {
                    result.warnings.push_back("reclass: skipped '" + record.path +
                                              "' because it reports no size");
                    continue;
                }

                const char prefix = util::CppPrefixFor(dump, record);
                const std::string display =
                    prefix + util::SanitizeIdentifier(
                                 record.name.empty() ? util::LeafName(record.path)
                                                     : record.name);

                std::string header_comment = record.path;
                if (!record.super.empty()) header_comment += " : " + record.super;

                xml.Line(2, "<class name=\"" + XmlEscape(display) + "\" uuid=\"" +
                                XmlEscape(UuidFor(record.path)) + "\" address=\"0\" comment=\"" +
                                XmlEscape(header_comment) + "\">");

                // Group by offset first. Bitfields legitimately share a byte, and a node
                // per property would consume that byte several times over, shifting
                // everything below it.
                std::vector<const ir::Property*> ordered;
                ordered.reserve(record.properties.size());
                for (const auto& property : record.properties) ordered.push_back(&property);

                std::stable_sort(ordered.begin(), ordered.end(),
                                 [](const ir::Property* a, const ir::Property* b) {
                                     return a->offset < b->offset;
                                 });

                std::unordered_set<std::string> used_names;
                std::int32_t cursor = 0;

                for (std::size_t i = 0; i < ordered.size();) {
                    const std::int32_t at = ordered[i]->offset;

                    std::size_t group_end = i;
                    std::int32_t width    = 0;
                    std::string  names;
                    while (group_end < ordered.size() && ordered[group_end]->offset == at) {
                        const auto* property = ordered[group_end];
                        width = std::max(width, std::max<std::int32_t>(property->size, 1));
                        if (!names.empty()) names += ", ";
                        names += property->name + " : " + DescribeType(property->type);
                        if (property->is_bitfield)
                            names += " (bit " + std::to_string(property->bit_index) + ")";
                        ++group_end;
                    }

                    if (at < cursor) {
                        // Overlaps something already written. Better reported than
                        // emitted, since it would desynchronise every later offset.
                        result.warnings.push_back(
                            "reclass: '" + record.path + "' has members at offset " +
                            std::to_string(at) + " overlapping earlier ones; skipped (" +
                            names + ")");
                        i = group_end;
                        continue;
                    }

                    if (at > record.size) break;
                    if (at > cursor) EmitPadding(xml, cursor, at - cursor, "padding");

                    width = std::min(width, record.size - at);
                    if (width <= 0) { i = group_end; continue; }

                    std::string node_name =
                        util::SanitizeIdentifier(ordered[i]->name.empty()
                                                     ? "field_" + std::to_string(at)
                                                     : ordered[i]->name);
                    while (!used_names.insert(node_name).second)
                        node_name += "_";

                    NodeShape shape{};
                    const bool typed = group_end - i == 1 &&
                                       TypedNodeFor(*ordered[i], shape) &&
                                       shape.size == width;

                    if (typed) {
                        xml.Line(3, "<node type=\"" + std::string(shape.type) + "\" name=\"" +
                                        XmlEscape(node_name) + "\" comment=\"" +
                                        XmlEscape(names) + "\" />");
                    } else {
                        // Hex bytes, exact width. The comment does the explaining.
                        EmitPadding(xml, at, width, names);
                    }

                    cursor = at + width;
                    i = group_end;
                }

                if (cursor < record.size)
                    EmitPadding(xml, cursor, record.size - cursor, "tail padding");

                xml.Line(2, "</class>");
                ++emitted;
            }
        }
    }

    xml.Line(1, "</classes>");
    xml.Line(0, "</reclass>");

    if (emitted == 0) {
        result.error = "reclass: nothing to emit (no classes or structs matched)";
        return result;
    }

    const std::string stem = FileStem(dump);

    const std::string xml_path = options.out_dir + "/" + stem + ".reclass.xml";
    std::string error;
    if (!util::WriteFile(xml_path, xml.Data(), error)) {
        result.error = error;
        return result;
    }
    result.files.push_back(xml_path);

    // ReClass.NET opens the archive directly. The loose XML stays alongside it because a
    // readable file is what makes a schema problem diagnosable.
    const std::string archive_path = options.out_dir + "/" + stem + ".rcnet";
    if (!util::WriteFile(archive_path, MakeArchive(kArchiveEntry, xml.Data()), error)) {
        result.error = error;
        return result;
    }
    result.files.push_back(archive_path);

    return result;
}

} // namespace zircon::emit
