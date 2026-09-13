#include "Emitters.h"

#include "DisasmCommon.h"

#include <format>
#include <map>
#include <string>
#include <vector>

namespace zircon::emit {
namespace {

using namespace zircon::emit::disasm;

// TArray and friends have no counterpart in the IR's type list, so the script has to
// create them. Worth the trouble: a real shape lets a user follow an allocation instead of
// staring at a byte blob.
struct Helper {
    std::string name;
    std::int32_t size{0};
    std::vector<std::string> members;   // fully formed Python tuples
};

using HelperMap = std::map<std::string, Helper>;

std::string Mangle(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_')
            out.push_back(c);
        else if (!out.empty() && out.back() != '_')
            out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "Opaque" : out;
}

std::string MemberTuple(std::int32_t offset, std::int32_t size, std::string_view name,
                        std::string_view spec, std::string_view comment) {
    return std::format("({}, {}, {}, {}, {})", offset, size, PyQuote(name), spec,
                       PyQuote(comment));
}

// A named block of the right length. Ghidra renders an unnamed byte array fine, but the
// name keeps the member readable and matches what the IDA output shows.
std::string OpaqueHelper(HelperMap& helpers, std::string_view label, std::int32_t size) {
    const std::string name = std::format("Z{}_{}", label, size);
    if (!helpers.count(name)) {
        Helper helper;
        helper.name = name;
        helper.size = size;
        helper.members.push_back(
            MemberTuple(0, size, "pad", std::format("(\"b\", {})", size), ""));
        helpers.emplace(name, std::move(helper));
    }
    return name;
}

std::string SpecFor(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                    std::int32_t size);

std::string ArrayHelper(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                        std::int32_t size) {
    constexpr std::int32_t kCanonicalArraySize = 16;
    if (size != kCanonicalArraySize || type.params.empty())
        return OpaqueHelper(helpers, "TArray", size);

    const ir::TypeRef& element = type.params.front();

    // Only object elements get a typed pointer. By-value would need the pointer to name a
    // type the script may never have created, and wrong beats untyped only in theory.
    std::string pointee = "None";
    std::string label   = "Opaque";
    if (element.kind == ir::TypeKind::ObjectPtr || element.kind == ir::TypeKind::ClassPtr) {
        if (const std::string* name = model.StructName(element.name)) {
            pointee = PyQuote(*name);
            label   = *name;
        }
    } else if (element.kind == ir::TypeKind::Struct) {
        if (const ir::Struct* record = model.Struct(element.name)) {
            if (const std::string* name = model.StructName(element.name)) {
                if (record->size == element.size) {
                    pointee = PyQuote(*name);
                    label   = *name;
                }
            }
        }
    } else {
        label = Mangle(Describe(element));
    }

    const std::string name = "ZTArray_" + Mangle(label);
    if (!helpers.count(name)) {
        Helper helper;
        helper.name = name;
        helper.size = kCanonicalArraySize;
        helper.members.push_back(MemberTuple(0, 8, "Data",
                                             std::format("(\"p\", {})", pointee),
                                             Describe(type)));
        helper.members.push_back(MemberTuple(8, 4, "Num", "(\"i\", 4, True)", ""));
        helper.members.push_back(MemberTuple(12, 4, "Max", "(\"i\", 4, True)", ""));
        helpers.emplace(name, std::move(helper));
    }
    return name;
}

// Empty return means nothing of exactly `size` bytes can be named; the caller then falls
// back to raw bytes instead of shifting every later member.
std::string SpecFor(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                    std::int32_t size) {
    if (size <= 0) return {};

    const auto integer = [&](bool is_signed) -> std::string {
        if (size != 1 && size != 2 && size != 4 && size != 8) return {};
        return std::format("(\"i\", {}, {})", size, is_signed ? "True" : "False");
    };
    const auto named = [&](const std::string& name) {
        return std::format("(\"s\", {})", PyQuote(name));
    };

    switch (type.kind) {
        case ir::TypeKind::Bool:
            return size == 1 ? std::string("(\"bool\",)") : integer(false);

        case ir::TypeKind::Int8:  case ir::TypeKind::Int16:
        case ir::TypeKind::Int32: case ir::TypeKind::Int64:
            return integer(true);

        case ir::TypeKind::UInt8:  case ir::TypeKind::UInt16:
        case ir::TypeKind::UInt32: case ir::TypeKind::UInt64:
            return integer(false);

        case ir::TypeKind::Float:
            return size == 4 ? std::string("(\"f\", 4)") : std::string{};
        case ir::TypeKind::Double:
            return size == 8 ? std::string("(\"f\", 8)") : std::string{};

        case ir::TypeKind::Name: {
            if (size != 8) return named(OpaqueHelper(helpers, "FName", size));
            if (!helpers.count("ZFName")) {
                Helper helper;
                helper.name = "ZFName";
                helper.size = 8;
                helper.members.push_back(
                    MemberTuple(0, 4, "ComparisonIndex", "(\"i\", 4, True)", ""));
                helper.members.push_back(MemberTuple(4, 4, "Number", "(\"i\", 4, True)", ""));
                helpers.emplace("ZFName", std::move(helper));
            }
            return named("ZFName");
        }

        case ir::TypeKind::String: {
            if (size != 16) return named(OpaqueHelper(helpers, "FString", size));
            if (!helpers.count("ZFString")) {
                Helper helper;
                helper.name = "ZFString";
                helper.size = 16;
                helper.members.push_back(
                    MemberTuple(0, 8, "Data", "(\"p\", None)", "wchar_t *"));
                helper.members.push_back(MemberTuple(8, 4, "Num", "(\"i\", 4, True)", ""));
                helper.members.push_back(MemberTuple(12, 4, "Max", "(\"i\", 4, True)", ""));
                helpers.emplace("ZFString", std::move(helper));
            }
            return named("ZFString");
        }

        case ir::TypeKind::Text:
            return named(OpaqueHelper(helpers, "FText", size));

        case ir::TypeKind::Enum: {
            if (const ir::Enum* record = model.EnumAt(type.name)) {
                if (EnumWidth(*record) == size) {
                    if (const std::string* name = model.EnumName(type.name))
                        return std::format("(\"e\", {})", PyQuote(*name));
                }
            }
            return integer(false);
        }

        case ir::TypeKind::Struct: {
            const ir::Struct* record = model.Struct(type.name);
            const std::string* name  = model.StructName(type.name);
            if (record && name && record->size == size) return named(*name);
            return {};
        }

        case ir::TypeKind::ObjectPtr:
        case ir::TypeKind::ClassPtr: {
            if (size != 8) return {};
            if (const std::string* name = model.StructName(type.name))
                return std::format("(\"p\", {})", PyQuote(*name));
            return "(\"p\", None)";
        }

        case ir::TypeKind::WeakPtr:      return named(OpaqueHelper(helpers, "TWeakObjectPtr", size));
        case ir::TypeKind::LazyPtr:      return named(OpaqueHelper(helpers, "TLazyObjectPtr", size));
        case ir::TypeKind::SoftPtr:      return named(OpaqueHelper(helpers, "TSoftObjectPtr", size));
        case ir::TypeKind::SoftClassPtr: return named(OpaqueHelper(helpers, "TSoftClassPtr", size));
        case ir::TypeKind::Interface:    return named(OpaqueHelper(helpers, "TScriptInterface", size));

        case ir::TypeKind::Array:    return named(ArrayHelper(model, helpers, type, size));
        case ir::TypeKind::Set:      return named(OpaqueHelper(helpers, "TSet", size));
        case ir::TypeKind::Map:      return named(OpaqueHelper(helpers, "TMap", size));
        case ir::TypeKind::Optional: return named(OpaqueHelper(helpers, "TOptional", size));

        case ir::TypeKind::Delegate:          return named(OpaqueHelper(helpers, "FDelegate", size));
        case ir::TypeKind::MulticastDelegate: return named(OpaqueHelper(helpers, "FMulticastDelegate", size));
        case ir::TypeKind::FieldPath:         return named(OpaqueHelper(helpers, "TFieldPath", size));

        case ir::TypeKind::Unknown:
            return {};
    }
    return {};
}

std::string SlotTuple(const Model& model, HelperMap& helpers, const MemberSlot& slot,
                      std::size_t& opaque_members) {
    if (slot.base) {
        const std::string* name = model.StructName(slot.base_path);
        const std::string spec = name ? std::format("(\"s\", {})", PyQuote(*name))
                                      : std::format("(\"b\", {})", slot.size);
        return MemberTuple(slot.offset, slot.size, slot.name, spec,
                           "inherited from " + slot.base_path);
    }

    if (slot.padding) {
        return MemberTuple(slot.offset, slot.size, slot.name,
                           std::format("(\"b\", {})", slot.size), "padding");
    }

    if (slot.packed.size() > 1) {
        std::string bits;
        for (const auto* property : slot.packed) {
            if (!bits.empty()) bits += ", ";
            bits += std::format("0x{:02X}={}", property->field_mask, property->name);
        }
        return MemberTuple(slot.offset, 1, slot.name, "(\"i\", 1, False)",
                           "bitfield: " + bits);
    }

    const ir::Property& property = *slot.property;
    const std::int32_t dim = std::max(1, property.array_dim);
    std::int32_t element = property.type.size;
    if (element <= 0 || element * dim != slot.size) element = slot.size / dim;

    const std::string described = Describe(property.type);

    std::string spec;
    if (element > 0 && element * dim == slot.size)
        spec = SpecFor(model, helpers, property.type, element);

    if (spec.empty()) {
        ++opaque_members;
        return MemberTuple(slot.offset, slot.size, slot.name,
                           std::format("(\"b\", {})", slot.size), described + " (opaque)");
    }

    if (dim > 1) spec = std::format("(\"a\", {}, {})", spec, dim);
    return MemberTuple(slot.offset, slot.size, slot.name, spec, described);
}

} // namespace

EmitResult EmitGhidra(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (std::string error = Preflight(dump, options); !error.empty()) {
        result.error = std::move(error);
        return result;
    }

    const Model model = BuildModel(dump, options, result.warnings);

    HelperMap helpers;
    std::size_t opaque_members = 0;

    // Dump order is fine here; Ghidra needs no dependency ordering. A first pass creates
    // every structure at full size, so a member can reference a type whose own members get
    // filled in later and still take up the right space.
    std::vector<std::string> entries;
    entries.reserve(model.types.size());

    for (const auto* record : model.types) {
        const std::string* name = model.StructName(record->path);
        if (!name) continue;

        if (record->size <= 0) {
            result.warnings.push_back(std::format(
                "{}: reported size is {}; skipped, Ghidra cannot create a zero-length "
                "structure", record->path, record->size));
            continue;
        }

        const Layout layout = ComputeLayout(model, *record, result.warnings);

        std::string text = std::format("({}, {}, [\n", PyQuote(*name), layout.total_size);
        for (const auto& slot : layout.slots)
            text += "  " + SlotTuple(model, helpers, slot, opaque_members) + ",\n";
        text += "])";

        entries.push_back(std::move(text));
    }

    if (opaque_members > 0) {
        result.warnings.push_back(std::format(
            "{} members could not be given a named type and were emitted as byte blocks "
            "of the correct size", opaque_members));
    }

    const auto sites = CollectFunctions(dump, options, model, result.warnings);

    // --- assemble ---------------------------------------------------------------------
    std::string python;
    python += "# Generated by Zircon - Unreal Engine reflection toolkit.\n";
    python += std::format("# Source: {}\n",
                          dump.header.source.process.empty() ? "unknown"
                                                             : dump.header.source.process);
    python += std::format("# Engine: {}\n", dump.header.engine.version.empty()
                                                ? "unknown" : dump.header.engine.version);
    python += "#\n";
    python += "# Run inside Ghidra:  Window > Script Manager > Run\n";
    python += "#\n";
    python += "# Jython 2.7, so no f-strings and every print takes a single argument.\n";
    python += "# Addresses are stored relative to the module base and rebased onto this\n";
    python += "# program at run time, so the dump's own ASLR base is irrelevant.\n";
    python += "\n";
    python += "from ghidra.program.model.data import ArrayDataType\n";
    python += "from ghidra.program.model.data import BooleanDataType\n";
    python += "from ghidra.program.model.data import ByteDataType\n";
    python += "from ghidra.program.model.data import CategoryPath\n";
    python += "from ghidra.program.model.data import DataTypeConflictHandler\n";
    python += "from ghidra.program.model.data import DoubleDataType\n";
    python += "from ghidra.program.model.data import EnumDataType\n";
    python += "from ghidra.program.model.data import FloatDataType\n";
    python += "from ghidra.program.model.data import IntegerDataType\n";
    python += "from ghidra.program.model.data import LongLongDataType\n";
    python += "from ghidra.program.model.data import PointerDataType\n";
    python += "from ghidra.program.model.data import ShortDataType\n";
    python += "from ghidra.program.model.data import SignedByteDataType\n";
    python += "from ghidra.program.model.data import StructureDataType\n";
    python += "from ghidra.program.model.data import UnsignedIntegerDataType\n";
    python += "from ghidra.program.model.data import UnsignedLongLongDataType\n";
    python += "from ghidra.program.model.data import UnsignedShortDataType\n";
    python += "from ghidra.program.model.data import VoidDataType\n";
    python += "from ghidra.program.model.symbol import SourceType\n";
    python += "\n";
    python += "CATEGORY = CategoryPath(\"/Zircon\")\n";
    python += "POINTER_SIZE = 8\n";
    python += "\n";

    python += "ZIRCON_ENUMS = [\n";
    for (const auto* record : model.enums) {
        const std::string* name = model.EnumName(record->path);
        if (!name) continue;

        std::string values;
        std::unordered_set<std::string> used;
        for (const auto& value : record->values) {
            std::string label = util::SanitizeIdentifier(value.name);
            // Ghidra rejects a duplicate entry name inside one enum.
            if (!used.insert(label).second) {
                for (int suffix = 1;; ++suffix) {
                    std::string candidate = std::format("{}_{}", label, suffix);
                    if (used.insert(candidate).second) { label = candidate; break; }
                }
            }
            values += std::format("({}, {}), ", PyQuote(label), value.value);
        }
        python += std::format("({}, {}, [{}]),\n", PyQuote(*name), EnumWidth(*record),
                              values);
    }
    python += "]\n\n";

    python += "ZIRCON_HELPERS = [\n";
    for (const auto& [key, helper] : helpers) {
        (void)key;
        python += std::format("({}, {}, [\n", PyQuote(helper.name), helper.size);
        for (const auto& member : helper.members) python += "  " + member + ",\n";
        python += "]),\n";
    }
    python += "]\n\n";

    python += "ZIRCON_STRUCTS = [\n";
    for (const auto& entry : entries) python += entry + ",\n";
    python += "]\n\n";

    python += "ZIRCON_FUNCTIONS = [\n";
    for (const auto& site : sites) {
        python += std::format("({}, {}, {}),\n", site.rva, PyQuote(site.name),
                              PyQuote(site.signature + "    [" + site.path + "]"));
    }
    python += "]\n\n";

    python +=
        "def integer_type(width, is_signed):\n"
        "    if width == 1:\n"
        "        return SignedByteDataType() if is_signed else ByteDataType()\n"
        "    if width == 2:\n"
        "        return ShortDataType() if is_signed else UnsignedShortDataType()\n"
        "    if width == 4:\n"
        "        return IntegerDataType() if is_signed else UnsignedIntegerDataType()\n"
        "    if width == 8:\n"
        "        return LongLongDataType() if is_signed else UnsignedLongLongDataType()\n"
        "    return None\n"
        "\n"
        "def resolve(spec, registry):\n"
        "    tag = spec[0]\n"
        "    if tag == \"b\":\n"
        "        count = spec[1]\n"
        "        if count == 1:\n"
        "            return ByteDataType()\n"
        "        return ArrayDataType(ByteDataType(), count, 1)\n"
        "    if tag == \"i\":\n"
        "        return integer_type(spec[1], spec[2])\n"
        "    if tag == \"f\":\n"
        "        return FloatDataType() if spec[1] == 4 else DoubleDataType()\n"
        "    if tag == \"bool\":\n"
        "        return BooleanDataType()\n"
        "    if tag == \"p\":\n"
        "        target = registry.get(spec[1]) if spec[1] else None\n"
        "        if target is None:\n"
        "            return PointerDataType(VoidDataType(), POINTER_SIZE)\n"
        "        return PointerDataType(target, POINTER_SIZE)\n"
        "    if tag == \"s\" or tag == \"e\":\n"
        "        return registry.get(spec[1])\n"
        "    if tag == \"a\":\n"
        "        element = resolve(spec[1], registry)\n"
        "        if element is None:\n"
        "            return None\n"
        "        return ArrayDataType(element, spec[2], element.getLength())\n"
        "    return None\n"
        "\n"
        "def create_enums(dtm, registry):\n"
        "    for name, width, values in ZIRCON_ENUMS:\n"
        "        enum = EnumDataType(CATEGORY, name, width)\n"
        "        for label, value in values:\n"
        "            try:\n"
        "                enum.add(label, value)\n"
        "            except Exception:\n"
        "                pass\n"
        "        registry[name] = dtm.addDataType(\n"
        "            enum, DataTypeConflictHandler.REPLACE_HANDLER)\n"
        "\n"
        "def create_structs(dtm, registry, tables):\n"
        "    # Every structure is created at its final size first. A member can then refer\n"
        "    # to a type whose own members are filled in later and still take the right\n"
        "    # number of bytes, which removes any need to order definitions.\n"
        "    for table in tables:\n"
        "        for name, size, members in table:\n"
        "            structure = StructureDataType(CATEGORY, name, size)\n"
        "            try:\n"
        "                structure.setPackingEnabled(False)\n"
        "            except Exception:\n"
        "                pass\n"
        "            registry[name] = dtm.addDataType(\n"
        "                structure, DataTypeConflictHandler.REPLACE_HANDLER)\n"
        "\n"
        "def fill_structs(registry, tables):\n"
        "    failures = 0\n"
        "    for table in tables:\n"
        "        for name, size, members in table:\n"
        "            structure = registry.get(name)\n"
        "            if structure is None:\n"
        "                continue\n"
        "            for offset, length, member, spec, comment in members:\n"
        "                data_type = resolve(spec, registry)\n"
        "                if data_type is None:\n"
        "                    failures += 1\n"
        "                    continue\n"
        "                try:\n"
        "                    structure.replaceAtOffset(\n"
        "                        offset, data_type, length, member, comment)\n"
        "                except Exception:\n"
        "                    failures += 1\n"
        "    return failures\n"
        "\n"
        "def apply_functions():\n"
        "    base = currentProgram.getImageBase()\n"
        "    memory = currentProgram.getMemory()\n"
        "    renamed = 0\n"
        "    missing = 0\n"
        "    for rva, name, signature in ZIRCON_FUNCTIONS:\n"
        "        address = base.add(rva)\n"
        "        if not memory.contains(address):\n"
        "            missing += 1\n"
        "            continue\n"
        "        function = getFunctionAt(address)\n"
        "        if function is None:\n"
        "            function = createFunction(address, None)\n"
        "        try:\n"
        "            if function is not None:\n"
        "                function.setName(name, SourceType.USER_DEFINED)\n"
        "            else:\n"
        "                createLabel(address, name, True)\n"
        "            renamed += 1\n"
        "        except Exception:\n"
        "            pass\n"
        "        try:\n"
        "            setPlateComment(address, signature)\n"
        "        except Exception:\n"
        "            pass\n"
        "    print(\"[zircon] renamed %d function(s), %d address(es) outside this program\"\n"
        "          % (renamed, missing))\n"
        "\n"
        "def main():\n"
        "    dtm = currentProgram.getDataTypeManager()\n"
        "    registry = {}\n"
        "    tables = [ZIRCON_HELPERS, ZIRCON_STRUCTS]\n"
        "    transaction = currentProgram.startTransaction(\"Zircon import\")\n"
        "    try:\n"
        "        create_enums(dtm, registry)\n"
        "        create_structs(dtm, registry, tables)\n"
        "        failures = fill_structs(registry, tables)\n"
        "        print(\"[zircon] created %d enum(s) and %d type(s); %d member(s) could \"\n"
        "              \"not be placed\" % (len(ZIRCON_ENUMS), len(registry), failures))\n"
        "        apply_functions()\n"
        "    finally:\n"
        "        currentProgram.endTransaction(transaction, True)\n"
        "    print(\"[zircon] done\")\n"
        "\n"
        "main()\n";

    const std::string path = options.out_dir + "/zircon_ghidra.py";
    std::string error;
    if (!util::WriteFile(path, python, error)) {
        result.error = std::move(error);
        return result;
    }

    result.files.push_back(path);
    return result;
}

} // namespace zircon::emit
