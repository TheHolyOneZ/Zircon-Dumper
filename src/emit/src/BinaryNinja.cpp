#include "Emitters.h"

#include "DisasmCommon.h"

#include <format>
#include <map>
#include <string>
#include <vector>

namespace zircon::emit {
namespace {

using namespace zircon::emit::disasm;

// BN has no text type parser worth feeding 50k structs to, so this emits a data table plus
// a loop that reads it. Spelling out StructureBuilder calls per type gives you a 40 MB .py
// that takes minutes to import, and the build logic copy-pasted 50k times.
//
// Member kinds, one char each:
//   i/u  int, arg = width       f  float, arg = 4|8      b  bool
//   p    pointer, arg = target name ("" means void*)
//   s    embedded struct, arg = name
//   e    enum, arg = name
// Fifth field is the inline array dim. UE really does use those.

struct Member {
    std::int32_t offset{0};
    std::string  name;
    char         kind{'x'};
    std::string  arg;       // a type name, or a width, depending on kind
    std::int32_t count{1};
};

struct StructDef {
    std::string  name;
    std::int32_t size{0};
    std::int32_t alignment{0};
    std::vector<Member> members;
};

// FName, FString, TArray<T>, and the opaque placeholders. None of these are in the dump,
// so they get synthesised here. std::map so output order is stable across runs.
using HelperMap = std::map<std::string, StructDef>;

std::string Mangle(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "Opaque" : out;
}

// Right-sized placeholder with a name on it. `ZTMap_80 Foo` at least tells you what was
// there; a raw byte blob tells you nothing.
std::string OpaqueStruct(HelperMap& helpers, std::string_view label, std::int32_t size) {
    const std::string name = std::format("Z{}_{}", label, size);
    if (!helpers.count(name)) {
        StructDef def;
        def.name = name;
        def.size = size;
        def.alignment = 1;
        def.members.push_back(Member{0, "pad", 'u', "1", size});
        helpers.emplace(name, std::move(def));
    }
    return name;
}

void EnsureFName(HelperMap& helpers) {
    if (helpers.count("FName")) return;
    StructDef def;
    def.name = "FName";
    def.size = 8;
    def.alignment = 4;
    def.members.push_back(Member{0, "ComparisonIndex", 'i', "4", 1});
    def.members.push_back(Member{4, "Number", 'i', "4", 1});
    helpers.emplace("FName", std::move(def));
}

void EnsureFString(HelperMap& helpers) {
    if (helpers.count("FString")) return;
    StructDef def;
    def.name = "FString";
    def.size = 16;
    def.alignment = 8;
    def.members.push_back(Member{0, "Data", 'p', "", 1});
    def.members.push_back(Member{8, "Num", 'i', "4", 1});
    def.members.push_back(Member{12, "Max", 'i', "4", 1});
    helpers.emplace("FString", std::move(def));
}

void EnsureInterface(HelperMap& helpers) {
    if (helpers.count("FScriptInterface")) return;
    StructDef def;
    def.name = "FScriptInterface";
    def.size = 16;
    def.alignment = 8;
    def.members.push_back(Member{0, "ObjectPointer", 'p', "", 1});
    def.members.push_back(Member{8, "InterfacePointer", 'p', "", 1});
    helpers.emplace("FScriptInterface", std::move(def));
}

Member Classify(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                std::int32_t size, std::size_t& opaque_count);

// Worth doing properly. A typed Data pointer is what lets you follow the allocation in BN
// and actually see elements.
std::string ArrayHelper(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                        std::int32_t size, std::size_t& opaque_count) {
    constexpr std::int32_t kCanonicalArraySize = 16;   // void* Data + int32 Num + int32 Max

    // counted here, not by the caller: an array that keeps its element type isn't opaque
    const auto give_up = [&]() {
        ++opaque_count;
        return OpaqueStruct(helpers, "TArray", size);
    };

    if (size != kCanonicalArraySize || type.params.empty()) return give_up();

    const ir::TypeRef& element = type.params.front();

    // the element's own fallbacks aren't members of this struct, so drop the count
    std::size_t element_fallbacks = 0;
    const Member inner = Classify(model, helpers, element, element.size, element_fallbacks);

    // only bother if the element is nameable. `ZTMap_80 *Data` says nothing a void* doesn't.
    std::string element_name;
    switch (inner.kind) {
        case 's': case 'e': element_name = inner.arg; break;
        case 'i': element_name = "int" + inner.arg; break;
        case 'u': element_name = "uint" + inner.arg; break;
        case 'f': element_name = inner.arg == "4" ? "float" : "double"; break;
        case 'p': element_name = inner.arg.empty() ? "void" : inner.arg; break;
        default: return give_up();
    }

    const std::string name = "ZTArray_" + Mangle(element_name);
    if (!helpers.count(name)) {
        StructDef def;
        def.name = name;
        def.size = kCanonicalArraySize;
        def.alignment = 8;
        // reached through a pointer, so a forward name is fine even if it's defined later
        Member data{0, "Data", 'p', inner.kind == 'p' ? inner.arg : element_name, 1};
        if (inner.kind == 'i' || inner.kind == 'u' || inner.kind == 'f' ||
            inner.kind == 'b') {
            // primitives have no name to reference, so hand the script the encoding instead
            data.arg  = std::string(1, inner.kind) + inner.arg;
        } else if (inner.kind == 's' || inner.kind == 'e') {
            data.arg = inner.arg;
        }
        def.members.push_back(std::move(data));
        def.members.push_back(Member{8,  "Num", 'i', "4", 1});
        def.members.push_back(Member{12, "Max", 'i', "4", 1});
        helpers.emplace(name, std::move(def));
    }
    return name;
}

// A member of exactly `size` bytes. Every branch checks the width, because right idea plus
// wrong size shifts everything after it, and you can't see that in the output.
Member Classify(const Model& model, HelperMap& helpers, const ir::TypeRef& type,
                std::int32_t size, std::size_t& opaque_count) {
    const auto integer = [&](bool is_signed) -> Member {
        if (size == 1 || size == 2 || size == 4 || size == 8)
            return Member{0, {}, is_signed ? 'i' : 'u', std::to_string(size), 1};
        ++opaque_count;
        return Member{0, {}, 's', OpaqueStruct(helpers, "Int", size), 1};
    };
    const auto opaque = [&](std::string_view label) -> Member {
        ++opaque_count;
        return Member{0, {}, 's', OpaqueStruct(helpers, label, size), 1};
    };

    if (size <= 0) return opaque("Empty");

    switch (type.kind) {
        case ir::TypeKind::Bool:
            if (size == 1) return Member{0, {}, 'b', "1", 1};
            return integer(false);

        case ir::TypeKind::Int8: case ir::TypeKind::Int16:
        case ir::TypeKind::Int32: case ir::TypeKind::Int64:
            return integer(true);

        case ir::TypeKind::UInt8: case ir::TypeKind::UInt16:
        case ir::TypeKind::UInt32: case ir::TypeKind::UInt64:
            return integer(false);

        case ir::TypeKind::Float:
            if (size == 4) return Member{0, {}, 'f', "4", 1};
            if (size == 8) return Member{0, {}, 'f', "8", 1};
            return opaque("Float");

        case ir::TypeKind::Double:
            if (size == 8) return Member{0, {}, 'f', "8", 1};
            return opaque("Double");

        case ir::TypeKind::Name:
            if (size != 8) return opaque("FName");
            EnsureFName(helpers);
            return Member{0, {}, 's', "FName", 1};

        case ir::TypeKind::String:
            if (size != 16) return opaque("FString");
            EnsureFString(helpers);
            return Member{0, {}, 's', "FString", 1};

        case ir::TypeKind::Text:
            return opaque("FText");

        case ir::TypeKind::Enum: {
            const std::string* name = model.EnumName(type.name);
            const ir::Enum* record = model.EnumAt(type.name);
            // enum width and property width have to agree. when they don't, trust the
            // property - that's what's actually in memory
            if (name && record && EnumWidth(*record) == size)
                return Member{0, {}, 'e', *name, 1};
            return integer(false);
        }

        case ir::TypeKind::Struct: {
            const std::string* name = model.StructName(type.name);
            const ir::Struct* record = model.Struct(type.name);
            if (name && record && record->size == size)
                return Member{0, {}, 's', *name, 1};
            return opaque("Struct");
        }

        case ir::TypeKind::ObjectPtr:
        case ir::TypeKind::ClassPtr: {
            if (size != 8) return opaque("Ptr");
            const std::string* name = model.StructName(type.name);
            return Member{0, {}, 'p', name ? *name : std::string{}, 1};
        }

        case ir::TypeKind::Interface:
            if (size != 16) return opaque("TScriptInterface");
            EnsureInterface(helpers);
            return Member{0, {}, 's', "FScriptInterface", 1};

        case ir::TypeKind::Array:
            return Member{0, {}, 's',
                          ArrayHelper(model, helpers, type, size, opaque_count), 1};

        case ir::TypeKind::WeakPtr:      return opaque("TWeakObjectPtr");
        case ir::TypeKind::LazyPtr:      return opaque("TLazyObjectPtr");
        case ir::TypeKind::SoftPtr:      return opaque("TSoftObjectPtr");
        case ir::TypeKind::SoftClassPtr: return opaque("TSoftClassPtr");
        case ir::TypeKind::Set:          return opaque("TSet");
        case ir::TypeKind::Map:          return opaque("TMap");
        case ir::TypeKind::Delegate:          return opaque("FDelegate");
        case ir::TypeKind::MulticastDelegate: return opaque("FMulticastDelegate");
        case ir::TypeKind::FieldPath:         return opaque("TFieldPath");
        case ir::TypeKind::Optional:          return opaque("TOptional");
        case ir::TypeKind::Unknown:
            return opaque(Mangle(type.raw.empty() ? "Unknown" : type.raw));
    }
    return opaque("Unknown");
}

Member MemberFor(const Model& model, HelperMap& helpers, const MemberSlot& slot,
                 std::size_t& opaque_count) {
    Member member;
    member.name = slot.name;

    if (slot.base) {
        const std::string* name = model.StructName(slot.base_path);
        if (name) {
            member.kind = 's';
            member.arg  = *name;
            member.offset = slot.offset;
            return member;
        }
    }

    if (slot.padding || slot.property == nullptr || !slot.packed.empty()) {
        // padding, an unusable base, and a byte shared by several bools all end up the
        // same: named bytes, no claim about what's in them. the per-bool masks are already
        // in the dump and in `docs`, and a third copy here is just somewhere to disagree.
        member.kind = 'u';
        member.arg  = "1";
        member.count = slot.size;
        member.offset = slot.offset;
        if (!slot.packed.empty())
            member.name = std::format("bits_{:04X}", static_cast<unsigned>(slot.offset));
        return member;
    }

    const ir::Property& property = *slot.property;
    const std::int32_t dim = std::max(1, property.array_dim);
    const std::int32_t element = dim > 1 && slot.size % dim == 0 ? slot.size / dim
                                                                 : slot.size;

    member = Classify(model, helpers, property.type, element, opaque_count);
    member.name   = slot.name;
    member.offset = slot.offset;
    member.count  = element == slot.size ? 1 : dim;
    return member;
}

void AppendStructTable(std::string& python, const StructDef& def) {
    python += std::format("({}, {}, {}, (", PyQuote(def.name), def.size, def.alignment);
    for (const auto& member : def.members) {
        python += std::format("({},{},{},{},{}),", member.offset, PyQuote(member.name),
                              PyQuote(std::string(1, member.kind)), PyQuote(member.arg),
                              member.count);
    }
    python += ")),\n";
}

} // namespace

EmitResult EmitBinja(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (std::string error = Preflight(dump, options); !error.empty()) {
        result.error = std::move(error);
        return result;
    }

    const Model model = BuildModel(dump, options, result.warnings);

    HelperMap helpers;
    std::size_t opaque_members = 0;
    std::vector<StructDef> defs;
    defs.reserve(model.types.size());

    // No topo sort. The script reserves every name at its final width first, then fills
    // them in, so named refs always resolve. Cycles just work, unlike the C-emitting
    // backends which have to warn about them.
    for (const auto* record : model.types) {
        const std::string* name = model.StructName(record->path);
        if (!name) continue;

        if (record->size <= 0) {
            result.warnings.push_back(std::format(
                "{}: reported size is {}; skipped, a zero-sized type cannot be defined",
                record->path, record->size));
            continue;
        }

        const Layout layout = ComputeLayout(model, *record, result.warnings);

        StructDef def;
        def.name      = *name;
        def.size      = layout.total_size;
        def.alignment = record->alignment > 0 ? record->alignment : 1;
        def.members.reserve(layout.slots.size());
        for (const auto& slot : layout.slots)
            def.members.push_back(MemberFor(model, helpers, slot, opaque_members));

        defs.push_back(std::move(def));
    }

    if (opaque_members > 0) {
        result.warnings.push_back(std::format(
            "{} members could not be given a named type and were defined as byte blocks "
            "of the correct size", opaque_members));
    }

    const auto sites = CollectFunctions(dump, options, model, result.warnings);

    // --- the script ------------------------------------------------------------------
    std::string python;
    python += "# Generated by Zircon - Unreal Engine reflection toolkit.\n";
    python += std::format("# Source: {}\n",
                          dump.header.source.process.empty() ? "unknown"
                                                             : dump.header.source.process);
    python += std::format("# Engine: {}\n", dump.header.engine.version.empty()
                                                ? "unknown" : dump.header.engine.version);
    python += "#\n";
    python += "# Run inside Binary Ninja:  Plugins > Run Script...  (or paste into the\n";
    python += "# Python console, where `bv` is already bound to the open view).\n";
    python += "#\n";
    python += "# Addresses are stored relative to the module base and rebased onto this\n";
    python += "# view at run time, so the ASLR base the dump was taken at is irrelevant.\n";
    python += "\n";
    python += "from binaryninja import (Type, StructureBuilder, EnumerationBuilder,\n";
    python += "                         NamedTypeReferenceClass)\n";
    python += "import binaryninja\n";
    python += "\n";

    python += "ZIRCON_ENUMS = [\n";
    for (const auto* record : model.enums) {
        const std::string* name = model.EnumName(record->path);
        if (!name) continue;
        python += std::format("({}, {}, {}, (", PyQuote(*name), EnumWidth(*record),
                              UnderlyingIsSigned(record->underlying) ? "True" : "False");
        for (const auto& value : record->values)
            python += std::format("({},{}),", PyQuote(value.name), value.value);
        python += ")),\n";
    }
    python += "]\n\n";

    python += "ZIRCON_STRUCTS = [\n";
    for (const auto& [name, def] : helpers) {
        (void)name;
        AppendStructTable(python, def);
    }
    for (const auto& def : defs) AppendStructTable(python, def);
    python += "]\n\n";

    python += "ZIRCON_FUNCTIONS = [\n";
    for (const auto& site : sites) {
        python += std::format("({}, {}, {}),\n", site.rva, PyQuote(site.name),
                              PyQuote(site.signature + "    [" + site.path + "]"));
    }
    python += "]\n\n";

    python +=
        "def _view():\n"
        "    view = globals().get(\"bv\")\n"
        "    if view is None:\n"
        "        view = getattr(binaryninja, \"current_view\", None)\n"
        "    if view is None:\n"
        "        raise RuntimeError(\"no BinaryView: open a binary, then run this script\")\n"
        "    return view\n"
        "\n"
        "def _named(name):\n"
        "    return Type.named_type_reference(\n"
        "        NamedTypeReferenceClass.StructNamedTypeClass, name)\n"
        "\n"
        "def _enum_ref(name):\n"
        "    return Type.named_type_reference(\n"
        "        NamedTypeReferenceClass.EnumNamedTypeClass, name)\n"
        "\n"
        "def _primitive(view, kind, arg):\n"
        "    if kind == \"i\":\n"
        "        return Type.int(int(arg), True)\n"
        "    if kind == \"u\":\n"
        "        return Type.int(int(arg), False)\n"
        "    if kind == \"f\":\n"
        "        return Type.float(int(arg))\n"
        "    if kind == \"b\":\n"
        "        return Type.bool()\n"
        "    return None\n"
        "\n"
        "def _member_type(view, kind, arg):\n"
        "    primitive = _primitive(view, kind, arg)\n"
        "    if primitive is not None:\n"
        "        return primitive\n"
        "    if kind == \"s\":\n"
        "        return _named(arg)\n"
        "    if kind == \"e\":\n"
        "        return _enum_ref(arg)\n"
        "    if kind == \"p\":\n"
        "        if not arg:\n"
        "            return Type.pointer(view.arch, Type.void())\n"
        "        # A pointer target is either a named type or a primitive spelled as its\n"
        "        # own encoding, e.g. \"i4\" — the array helpers need the second form.\n"
        "        inner = _primitive(view, arg[0], arg[1:]) if len(arg) > 1 else None\n"
        "        if inner is None:\n"
        "            inner = _named(arg)\n"
        "        return Type.pointer(view.arch, inner)\n"
        "    return Type.int(1, False)\n"
        "\n"
        "def apply_enums(view):\n"
        "    defined = 0\n"
        "    for name, width, signed, values in ZIRCON_ENUMS:\n"
        "        builder = EnumerationBuilder.create()\n"
        "        for label, value in values:\n"
        "            builder.append(label, value)\n"
        "        try:\n"
        "            view.define_user_type(\n"
        "                name, Type.enumeration_type(view.arch, builder, width, signed))\n"
        "            defined += 1\n"
        "        except Exception as error:\n"
        "            print(\"[zircon] enum %s: %s\" % (name, error))\n"
        "    return defined\n"
        "\n"
        "def _shell(size, alignment):\n"
        "    # Registered at its final width before anything references it. That is what\n"
        "    # makes a cyclic dump — and UE dumps are cyclic — import without ordering.\n"
        "    builder = StructureBuilder.create()\n"
        "    builder.packed = True\n"
        "    builder.width = size\n"
        "    builder.alignment = alignment\n"
        "    return Type.structure_type(builder)\n"
        "\n"
        "def apply_structs(view):\n"
        "    for name, size, alignment, members in ZIRCON_STRUCTS:\n"
        "        try:\n"
        "            view.define_user_type(name, _shell(size, alignment))\n"
        "        except Exception as error:\n"
        "            print(\"[zircon] reserve %s: %s\" % (name, error))\n"
        "\n"
        "    defined = 0\n"
        "    for name, size, alignment, members in ZIRCON_STRUCTS:\n"
        "        builder = StructureBuilder.create()\n"
        "        builder.packed = True\n"
        "        builder.width = size\n"
        "        builder.alignment = alignment\n"
        "        try:\n"
        "            for offset, member, kind, arg, count in members:\n"
        "                member_type = _member_type(view, kind, arg)\n"
        "                if count > 1:\n"
        "                    member_type = Type.array(member_type, count)\n"
        "                builder.insert(offset, member_type, member)\n"
        "            view.define_user_type(name, Type.structure_type(builder))\n"
        "            defined += 1\n"
        "        except Exception as error:\n"
        "            print(\"[zircon] struct %s: %s\" % (name, error))\n"
        "    return defined\n"
        "\n"
        "def apply_functions(view):\n"
        "    base = view.start\n"
        "    renamed = 0\n"
        "    missing = 0\n"
        "    for rva, name, signature in ZIRCON_FUNCTIONS:\n"
        "        address = base + rva\n"
        "        if not view.is_valid_offset(address):\n"
        "            missing += 1\n"
        "            continue\n"
        "        function = view.get_function_at(address)\n"
        "        if function is None:\n"
        "            view.create_user_function(address)\n"
        "            function = view.get_function_at(address)\n"
        "        if function is None:\n"
        "            missing += 1\n"
        "            continue\n"
        "        function.name = name\n"
        "        function.comment = signature\n"
        "        renamed += 1\n"
        "    print(\"[zircon] renamed %d function(s), %d address(es) not in this view\"\n"
        "          % (renamed, missing))\n"
        "    return renamed\n"
        "\n"
        "def main():\n"
        "    view = _view()\n"
        "    print(\"[zircon] image base 0x%X\" % view.start)\n"
        "    print(\"[zircon] %d enum(s) defined\" % apply_enums(view))\n"
        "    print(\"[zircon] %d struct(s) defined\" % apply_structs(view))\n"
        "    apply_functions(view)\n"
        "    view.update_analysis()\n"
        "    print(\"[zircon] done\")\n"
        "\n"
        "main()\n";

    const std::string path = options.out_dir + "/zircon_binja.py";
    std::string error;
    if (!util::WriteFile(path, python, error)) {
        result.error = std::move(error);
        return result;
    }

    result.files.push_back(path);
    return result;
}

} // namespace zircon::emit
