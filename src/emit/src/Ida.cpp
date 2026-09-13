#include "Emitters.h"

#include "DisasmCommon.h"

#include <format>
#include <map>
#include <string>
#include <vector>

namespace zircon::emit {
namespace {

using namespace zircon::emit::disasm;

// Generated structs are packed and account for every byte. Without the pragma IDA
// re-aligns members and quietly moves them, defeating the only thing these types are for.
constexpr std::string_view kPackPush = "#pragma pack(push, 1)";
constexpr std::string_view kPackPop  = "#pragma pack(pop)";

std::string IntegerOfSize(std::int32_t size, bool is_signed) {
    switch (size) {
        case 1: return is_signed ? "__int8"  : "unsigned __int8";
        case 2: return is_signed ? "__int16" : "unsigned __int16";
        case 4: return is_signed ? "int"     : "unsigned int";
        case 8: return is_signed ? "__int64" : "unsigned __int64";
        default: return {};
    }
}

// Keyed by name, so a given shape gets defined once however many members want it.
using HelperMap = std::map<std::string, std::string>;

std::string Mangle(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out.push_back(c);
        } else if (c == '*') {
            out += "Ptr";
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "Opaque" : out;
}

// Exactly sized placeholder with a readable name, so a member reads `ZTMap_80 Foo;`
// instead of an anonymous byte blob.
std::string OpaqueHelper(HelperMap& helpers, std::string_view label, std::int32_t size) {
    const std::string name = std::format("Z{}_{}", label, size);
    if (!helpers.count(name)) {
        helpers.emplace(name, std::format(
            "struct {} {{ unsigned __int8 pad[{}]; }};", name, size));
    }
    return name;
}

std::string MemberTypeFor(const Model& model, HelperMap& helpers,
                          const ir::TypeRef& type, std::int32_t size);

// Worth modelling properly instead of leaving it opaque. Typing Data lets a user follow the
// allocation in the disassembler and see real elements. C has no templates, so one variant
// per element type.
std::string ArrayHelper(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                        std::int32_t size) {
    constexpr std::int32_t kCanonicalArraySize = 16;   // void* + int32 Num + int32 Max
    if (size != kCanonicalArraySize || type.params.empty())
        return OpaqueHelper(helpers, "TArray", size);

    const ir::TypeRef& element = type.params.front();
    const std::string element_type =
        MemberTypeFor(model, helpers, element, element.size);
    if (element_type.empty()) return OpaqueHelper(helpers, "TArray", size);

    const std::string name = "ZTArray_" + Mangle(element_type);
    if (!helpers.count(name)) {
        // The element is referenced only through a pointer, so a forward declaration is
        // enough and these helpers can be emitted before any struct definition.
        helpers.emplace(name, std::format(
            "struct {} {{ {} *Data; int Num; int Max; }};", name, element_type));
    }
    return name;
}

// A C type of exactly `size` bytes, or empty when none can be named and the caller has to
// fall back to a byte array. Wrong width shifts every following member, so every branch
// checks.
std::string MemberTypeFor(const Model& model, HelperMap& helpers,
                          const ir::TypeRef& type, std::int32_t size) {
    if (size <= 0) return {};

    switch (type.kind) {
        case ir::TypeKind::Bool:
            return size == 1 ? "bool" : IntegerOfSize(size, false);

        case ir::TypeKind::Int8:  case ir::TypeKind::Int16:
        case ir::TypeKind::Int32: case ir::TypeKind::Int64:
            return IntegerOfSize(size, true);

        case ir::TypeKind::UInt8:  case ir::TypeKind::UInt16:
        case ir::TypeKind::UInt32: case ir::TypeKind::UInt64:
            return IntegerOfSize(size, false);

        case ir::TypeKind::Float:
            return size == 4 ? "float" : std::string{};
        case ir::TypeKind::Double:
            return size == 8 ? "double" : std::string{};

        case ir::TypeKind::Name: {
            if (size != 8) return OpaqueHelper(helpers, "FName", size);
            if (!helpers.count("FName")) {
                helpers.emplace("FName",
                    "struct FName { int ComparisonIndex; int Number; };");
            }
            return "FName";
        }

        case ir::TypeKind::String: {
            if (size != 16) return OpaqueHelper(helpers, "FString", size);
            if (!helpers.count("FString")) {
                helpers.emplace("FString",
                    "struct FString { wchar_t *Data; int Num; int Max; };");
            }
            return "FString";
        }

        case ir::TypeKind::Text:
            return OpaqueHelper(helpers, "FText", size);

        case ir::TypeKind::Enum: {
            if (const ir::Enum* record = model.EnumAt(type.name)) {
                if (EnumWidth(*record) == size) {
                    if (const std::string* name = model.EnumName(type.name)) return *name;
                }
            }
            // An enum used at some other width is still a plain integer of that width.
            // Accurate, if less descriptive.
            return IntegerOfSize(size, false);
        }

        case ir::TypeKind::Struct: {
            const ir::Struct* record = model.Struct(type.name);
            const std::string* name  = model.StructName(type.name);
            if (record && name && record->size == size) return *name;
            return {};
        }

        case ir::TypeKind::ObjectPtr:
        case ir::TypeKind::ClassPtr: {
            if (size != 8) return {};
            if (const std::string* name = model.StructName(type.name)) return *name + " *";
            return "void *";
        }

        case ir::TypeKind::WeakPtr:      return OpaqueHelper(helpers, "TWeakObjectPtr", size);
        case ir::TypeKind::LazyPtr:      return OpaqueHelper(helpers, "TLazyObjectPtr", size);
        case ir::TypeKind::SoftPtr:      return OpaqueHelper(helpers, "TSoftObjectPtr", size);
        case ir::TypeKind::SoftClassPtr: return OpaqueHelper(helpers, "TSoftClassPtr", size);
        case ir::TypeKind::Interface:    return OpaqueHelper(helpers, "TScriptInterface", size);

        case ir::TypeKind::Array:        return ArrayHelper(model, helpers, type, size);
        case ir::TypeKind::Set:          return OpaqueHelper(helpers, "TSet", size);
        case ir::TypeKind::Map:          return OpaqueHelper(helpers, "TMap", size);
        case ir::TypeKind::Optional:     return OpaqueHelper(helpers, "TOptional", size);

        case ir::TypeKind::Delegate:          return OpaqueHelper(helpers, "FDelegate", size);
        case ir::TypeKind::MulticastDelegate: return OpaqueHelper(helpers, "FMulticastDelegate", size);
        case ir::TypeKind::FieldPath:         return OpaqueHelper(helpers, "TFieldPath", size);

        case ir::TypeKind::Unknown:
            return {};
    }
    return {};
}

std::string MemberLine(const Model& model, HelperMap& helpers, const MemberSlot& slot,
                       std::size_t& unknown_count) {
    const auto comment = [&](std::string_view text) {
        return text.empty() ? std::string{} : std::format("  // {}", text);
    };

    if (slot.base) {
        const std::string* name = model.StructName(slot.base_path);
        return std::format("  {} {};{}", name ? *name : "void", slot.name,
                           comment(std::format("0x{:04X} inherited", slot.offset)));
    }

    if (slot.padding) {
        return std::format("  unsigned __int8 {}[{}];{}", slot.name, slot.size,
                           comment(std::format("0x{:04X} padding", slot.offset)));
    }

    // A packed byte holds several bools. IDA's struct bitfields are awkward to drive from
    // a declaration, and one member per flag would put several members at the same offset.
    // So: emit the byte once, document the bits.
    if (slot.packed.size() > 1) {
        std::string bits;
        for (const auto* property : slot.packed) {
            if (!bits.empty()) bits += ", ";
            bits += std::format("0x{:02X}={}", property->field_mask, property->name);
        }
        return std::format("  unsigned __int8 {};{}", slot.name,
                           comment(std::format("0x{:04X} bitfield: {}", slot.offset, bits)));
    }

    const ir::Property& property = *slot.property;
    const std::int32_t dim = std::max(1, property.array_dim);
    std::int32_t element = property.type.size;
    if (element <= 0 || element * dim != slot.size) element = slot.size / dim;

    std::string described = Describe(property.type);
    std::string type_text;
    if (element > 0 && element * dim == slot.size)
        type_text = MemberTypeFor(model, helpers, property.type, element);

    if (type_text.empty()) {
        ++unknown_count;
        return std::format("  unsigned __int8 {}[{}];{}", slot.name, slot.size,
                           comment(std::format("0x{:04X} {} (opaque)", slot.offset,
                                               described)));
    }

    const std::string array_suffix = dim > 1 ? std::format("[{}]", dim) : std::string{};

    // A pointer type already ends in '*', so do not insert a second space.
    const std::string spacer = type_text.back() == '*' ? "" : " ";

    return std::format("  {}{}{}{};{}", type_text, spacer, slot.name, array_suffix,
                       comment(std::format("0x{:04X} {}", slot.offset, described)));
}

std::string EnumDeclaration(const ir::Enum& record, const std::string& name,
                            std::unordered_set<std::string>& used_enumerators) {
    const int width = EnumWidth(record);
    const bool is_signed = UnderlyingIsSigned(record.underlying);

    std::string text = std::format("enum {} : {}\n{{\n", name,
                                   IntegerOfSize(width, is_signed));

    for (const auto& value : record.values) {
        std::string label = util::SanitizeIdentifier(value.name);

        // Enumerators share one namespace in C, and UE ships plain names like `Max` in
        // more than one enum.
        if (!used_enumerators.insert(label).second) {
            // Qualifying with the owning enum says which `Max` we mean, but only where the
            // label doesn't already carry it. UE entries usually read
            // `EMovementMode::MOVE_Walking`, and prefixing that again stutters.
            std::string candidate = label.rfind(name, 0) == 0 ? label : name + "__" + label;

            for (int suffix = 1; !used_enumerators.insert(candidate).second; ++suffix)
                candidate = std::format("{}_{}", label, suffix);

            label = candidate;
        }
        text += std::format("  {} = {},\n", label, value.value);
    }

    if (record.values.empty()) {
        // A C enum with no enumerators does not parse.
        text += "  _zircon_empty = 0,\n";
    }

    text += "};";
    return text;
}

void AppendQuotedLines(std::string& python, const std::string& block) {
    std::size_t start = 0;
    while (start <= block.size()) {
        const std::size_t end = block.find('\n', start);
        const std::string_view line =
            std::string_view(block).substr(start, (end == std::string::npos ? block.size() : end) - start);
        python += PyQuote(line);
        python += ",\n";
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

} // namespace

EmitResult EmitIda(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (std::string error = Preflight(dump, options); !error.empty()) {
        result.error = std::move(error);
        return result;
    }

    const Model model = BuildModel(dump, options, result.warnings);
    const std::vector<const ir::Struct*> ordered = TopoSort(model, result.warnings);

    HelperMap helpers;
    std::size_t opaque_members = 0;

    // Definitions first, so every helper type a member wants is registered by assembly time.
    std::vector<std::string> definitions;
    definitions.reserve(ordered.size());

    for (const auto* record : ordered) {
        const std::string* name = model.StructName(record->path);
        if (!name) continue;

        if (record->size <= 0) {
            result.warnings.push_back(std::format(
                "{}: reported size is {}; skipped, a zero-sized type cannot be declared",
                record->path, record->size));
            continue;
        }

        const Layout layout = ComputeLayout(model, *record, result.warnings);

        std::string text = std::format("// {}  (0x{:X} bytes", record->path,
                                       static_cast<unsigned>(layout.total_size));
        if (!record->super.empty()) text += ", extends " + record->super;
        text += ")\n";
        text += std::format("struct {}\n{{\n", *name);

        for (const auto& slot : layout.slots)
            text += MemberLine(model, helpers, slot, opaque_members) + "\n";

        text += "};";
        definitions.push_back(std::move(text));
    }

    if (opaque_members > 0) {
        result.warnings.push_back(std::format(
            "{} members could not be given a named type and were emitted as byte arrays "
            "of the correct size", opaque_members));
    }

    // --- assemble the C blob ---------------------------------------------------------
    std::string c_source;
    c_source += std::string(kPackPush) + "\n\n";

    std::unordered_set<std::string> used_enumerators;
    for (const auto* record : model.enums) {
        const std::string* name = model.EnumName(record->path);
        if (!name) continue;
        c_source += EnumDeclaration(*record, *name, used_enumerators) + "\n\n";
    }

    // Lets pointer members and the TArray helpers name types defined further down.
    for (const auto* record : ordered) {
        if (const std::string* name = model.StructName(record->path))
            c_source += std::format("struct {};\n", *name);
    }
    c_source += "\n";

    for (const auto& [name, definition] : helpers) {
        (void)name;
        c_source += definition + "\n";
    }
    c_source += "\n";

    for (const auto& definition : definitions) c_source += definition + "\n\n";
    c_source += std::string(kPackPop) + "\n";

    // --- assemble the script ---------------------------------------------------------
    const auto sites = CollectFunctions(dump, options, model, result.warnings);

    std::string python;
    python += "# Generated by Zircon - Unreal Engine reflection toolkit.\n";
    python += std::format("# Source: {}\n",
                          dump.header.source.process.empty() ? "unknown"
                                                             : dump.header.source.process);
    python += std::format("# Engine: {}\n", dump.header.engine.version.empty()
                                                ? "unknown" : dump.header.engine.version);
    python += "#\n";
    python += "# Run inside IDA:  File > Script file...\n";
    python += "#\n";
    python += "# Addresses are stored relative to the module base and rebased onto this\n";
    python += "# database at run time, so the ASLR base the dump was taken at is irrelevant.\n";
    python += "\n";
    python += "import idaapi\n";
    python += "import idc\n";
    python += "import ida_bytes\n";
    python += "import ida_funcs\n";
    python += "\n";
    python += "ZIRCON_TYPES = \"\\n\".join([\n";
    AppendQuotedLines(python, c_source);
    python += "])\n\n";

    python += "ZIRCON_FUNCTIONS = [\n";
    for (const auto& site : sites) {
        python += std::format("({}, {}, {}),\n", site.rva, PyQuote(site.name),
                              PyQuote(site.signature + "    [" + site.path + "]"));
    }
    python += "]\n\n";

    python +=
        "def apply_types():\n"
        "    errors = idc.parse_decls(ZIRCON_TYPES, idc.PT_SILENT)\n"
        "    if errors:\n"
        "        print(\"[zircon] %d declaration error(s); some types were not imported\"\n"
        "              % errors)\n"
        "    else:\n"
        "        print(\"[zircon] types imported cleanly\")\n"
        "    return errors\n"
        "\n"
        "def apply_functions():\n"
        "    base = idaapi.get_imagebase()\n"
        "    renamed = 0\n"
        "    missing = 0\n"
        "    for rva, name, signature in ZIRCON_FUNCTIONS:\n"
        "        ea = base + rva\n"
        "        if not ida_bytes.is_loaded(ea):\n"
        "            missing += 1\n"
        "            continue\n"
        "        if ida_funcs.get_func(ea) is None:\n"
        "            ida_funcs.add_func(ea)\n"
        "        if idc.set_name(ea, name, idc.SN_NOWARN | idc.SN_FORCE):\n"
        "            renamed += 1\n"
        "        idc.set_func_cmt(ea, signature, 1)\n"
        "    print(\"[zircon] renamed %d function(s), %d address(es) not present in this \"\n"
        "          \"database\" % (renamed, missing))\n"
        "    return renamed\n"
        "\n"
        "def main():\n"
        "    print(\"[zircon] image base 0x%X\" % idaapi.get_imagebase())\n"
        "    apply_types()\n"
        "    apply_functions()\n"
        "    print(\"[zircon] done\")\n"
        "\n"
        "main()\n";

    const std::string path = options.out_dir + "/zircon_ida.py";
    std::string error;
    if (!util::WriteFile(path, python, error)) {
        result.error = std::move(error);
        return result;
    }

    result.files.push_back(path);
    return result;
}

} // namespace zircon::emit
