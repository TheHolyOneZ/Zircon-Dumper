#include "Emitters.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit {
namespace {

// Type stubs, one module per UE package, for driving a game from Python - pymem,
// frida-python, whatever you already use. Completion on every class, offsets in a
// class-level dict, enums as real IntEnums.
//
// Stubs, not importable modules. Nothing here ever executes, which is the only reason
// the circular imports between packages are fine - UE's dependency graph has cycles and
// nothing would untangle them.

const std::set<std::string_view> kKeywords = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
    "continue", "def", "del", "elif", "else", "except", "finally", "for", "from",
    "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass",
    "raise", "return", "try", "while", "with", "yield",
    // not keywords, but shadowing any of these in a generated module is asking for it
    "int", "float", "bool", "str", "bytes", "list", "dict", "set", "tuple", "type",
    "object", "property", "self", "cls",
};

std::string PyIdentifier(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "_unnamed";
    if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
    if (kKeywords.count(out)) out += '_';
    return out;
}

std::string PyString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const unsigned char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n' || c == '\r' || c == '\t') { out.push_back(' '); continue; }
        if (c < 0x20) continue;
        out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
    return out;
}

// goes in a comment, so flatten anything that would end the line early
std::string OneLine(std::string_view text, std::size_t cap = 72) {
    std::string out;
    for (const char c : text) {
        if (c == '\n' || c == '\r') { out += " "; continue; }
        out.push_back(c);
    }
    if (out.size() > cap) { out.resize(cap - 3); out += "..."; }
    return out;
}

struct Named {
    std::string module;      // "Engine"
    std::string identifier;  // "AActor"
};

struct Index {
    std::unordered_map<std::string, Named> types;   // struct/class path -> name
    std::unordered_map<std::string, Named> enums;
};

// UE's prefixes give a flat namespace and dodge the class/struct/enum collisions you get
// from bare leaf names. Two packages can both declare `Foo`; Python modules keep those
// apart, so only collisions inside one module need a counter.
std::string PrefixedName(const ir::Dump& dump, const ir::Struct& record) {
    const char prefix = util::CppPrefixFor(dump, record);
    return std::string(1, prefix) + PyIdentifier(util::LeafName(record.path));
}

std::string ModuleFor(std::string_view package) {
    return PyIdentifier(util::PackageFileStem(package));
}

// What the value looks like once you've read it. Loose on purpose for the kinds nothing
// reads safely - calling a delegate handle an `int` would just be wrong.
std::string Annotation(const Index& index, const ir::TypeRef& type) {
    const auto named = [&](const std::unordered_map<std::string, Named>& table,
                           const std::string& path) -> std::string {
        const auto it = table.find(path);
        if (it == table.end()) return "Any";
        // quoted - the target might be defined further down this module, or reached through an
        // import cycle. checkers resolve string annotations lazily.
        return "\"" + it->second.identifier + "\"";
    };
    const auto inner = [&](std::size_t i) -> std::string {
        return i < type.params.size() ? Annotation(index, type.params[i])
                                      : std::string("Any");
    };

    switch (type.kind) {
        case ir::TypeKind::Bool:   return "bool";
        case ir::TypeKind::Int8:  case ir::TypeKind::Int16:
        case ir::TypeKind::Int32: case ir::TypeKind::Int64:
        case ir::TypeKind::UInt8:  case ir::TypeKind::UInt16:
        case ir::TypeKind::UInt32: case ir::TypeKind::UInt64:
            return "int";
        case ir::TypeKind::Float: case ir::TypeKind::Double:
            return "float";
        case ir::TypeKind::Name:   return "str";
        case ir::TypeKind::String: return "str";
        case ir::TypeKind::Text:   return "str";
        case ir::TypeKind::Enum:   return named(index.enums, type.name);
        case ir::TypeKind::Struct: return named(index.types, type.name);
        case ir::TypeKind::ObjectPtr:
        case ir::TypeKind::ClassPtr:
        case ir::TypeKind::WeakPtr: case ir::TypeKind::LazyPtr:
        case ir::TypeKind::SoftPtr: case ir::TypeKind::SoftClassPtr: {
            const std::string target = named(index.types, type.name);
            return target == "Any" ? "Any" : "Optional[" + target + "]";
        }
        case ir::TypeKind::Interface: return "Any";
        case ir::TypeKind::Array:     return "List[" + inner(0) + "]";
        case ir::TypeKind::Set:       return "Set[" + inner(0) + "]";
        case ir::TypeKind::Map:       return "Dict[" + inner(0) + ", " + inner(1) + "]";
        case ir::TypeKind::Optional:  return "Optional[" + inner(0) + "]";
        case ir::TypeKind::Delegate:
        case ir::TypeKind::MulticastDelegate:
        case ir::TypeKind::FieldPath:
        case ir::TypeKind::Unknown:
            return "Any";
    }
    return "Any";
}

std::string Join(std::string_view dir, std::string_view leaf) {
    std::filesystem::path path(dir);
    path /= leaf;
    return path.lexically_normal().string();
}

} // namespace

EmitResult EmitPythonStubs(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "the dump is partial (no object data), so there are no types to "
                       "describe; pass allow_partial to emit anyway";
        return result;
    }

    std::vector<const ir::Package*> packages;
    for (const auto& package : dump.packages) {
        if (!options.package_filter.empty() &&
            package.name.find(options.package_filter) == std::string::npos)
            continue;
        packages.push_back(&package);
    }

    if (packages.empty()) {
        result.error = options.package_filter.empty()
            ? "the dump contains no packages"
            : "package filter '" + options.package_filter + "' matched no packages";
        return result;
    }

    // two packages can sanitise down to the same module name, so a module collects whatever
    // lands in it rather than getting written once per package
    std::map<std::string, std::vector<const ir::Package*>> modules;
    for (const auto* package : packages) modules[ModuleFor(package->name)].push_back(package);

    // name everything first. a property in the first module routinely points at a class in
    // the last one.
    Index index;
    std::map<std::string, std::unordered_set<std::string>> claimed;   // module -> names

    const auto claim = [&](const std::string& module, std::string candidate) {
        auto& used = claimed[module];
        if (used.insert(candidate).second) return candidate;
        for (int suffix = 2;; ++suffix) {
            std::string numbered = std::format("{}_{}", candidate, suffix);
            if (used.insert(numbered).second) return numbered;
        }
    };

    for (const auto& [module, in_module] : modules) {
        for (const auto* package : in_module) {
            for (const auto& record : package->classes)
                index.types[record.path] = Named{module, claim(module, PrefixedName(dump, record))};
            for (const auto& record : package->structs)
                index.types[record.path] = Named{module, claim(module, PrefixedName(dump, record))};
            for (const auto& record : package->enums) {
                std::string leaf = PyIdentifier(util::LeafName(record.path));
                if (leaf.empty() || leaf[0] != 'E') leaf = "E" + leaf;
                index.enums[record.path] = Named{module, claim(module, leaf)};
            }
        }
    }

    std::size_t stub_classes = 0;
    std::size_t stub_enums   = 0;
    const std::string package_dir = Join(options.out_dir, "zircon_sdk");

    for (const auto& [module, in_module] : modules) {
        std::string text;
        text += "# Generated by Zircon - Unreal Engine reflection toolkit.\n";
        text += std::format("# Package: {}\n",
                            in_module.size() == 1 ? in_module.front()->name
                                                  : std::string("several"));
        text += "#\n";
        text += "# Type stubs. Not importable at runtime, and not meant to be: offsets in\n";
        text += "# __offsets__ are what you feed your own reader.\n";
        text += "\n";
        text += "from enum import IntEnum\n";
        text += "from typing import Any, ClassVar, Dict, List, Optional, Set\n";

        // only what this module actually mentions. cycles between these are fine, a stub never
        // runs.
        std::map<std::string, std::set<std::string>> imports;   // module -> names
        const auto note_import = [&](const std::string& path) {
            auto it = index.types.find(path);
            if (it != index.types.end() && it->second.module != module) {
                imports[it->second.module].insert(it->second.identifier);
                return;
            }
            auto other = index.enums.find(path);
            if (other != index.enums.end() && other->second.module != module)
                imports[other->second.module].insert(other->second.identifier);
        };

        std::function<void(const ir::TypeRef&)> walk = [&](const ir::TypeRef& type) {
            if (!type.name.empty()) note_import(type.name);
            for (const auto& param : type.params) walk(param);
        };

        for (const auto* package : in_module) {
            for (const auto* list : {&package->classes, &package->structs}) {
                for (const auto& record : *list) {
                    if (!record.super.empty()) note_import(record.super);
                    for (const auto& property : record.properties) walk(property.type);
                    for (const auto& function : record.functions)
                        for (const auto& param : function.params) walk(param.type);
                }
            }
        }

        for (const auto& [from, names] : imports) {
            text += "from ." + from + " import ";
            bool first = true;
            for (const auto& name : names) {
                if (!first) text += ", ";
                first = false;
                text += name;
            }
            text += "\n";
        }
        text += "\n";

        for (const auto* package : in_module) {
            for (const auto& record : package->enums) {
                const auto it = index.enums.find(record.path);
                if (it == index.enums.end()) continue;
                ++stub_enums;

                text += std::format("class {}(IntEnum):\n", it->second.identifier);
                text += std::format("    \"\"\"{}\"\"\"\n", record.path);
                text += std::format("    __path__: ClassVar[str] = {}\n",
                                    PyString(record.path));

                std::unordered_set<std::string> used;
                for (const auto& value : record.values) {
                    // UE writes some enumerators as `EFoo::Bar`, so keep the leaf.
                    // Strip first, sanitise second. The other way round the leaf
                    // skips the keyword check and you get `None = 0`, which isn't
                    // a name clash, it's a stub that won't parse.
                    std::string_view raw = value.name;
                    const auto colons = raw.rfind("::");
                    if (colons != std::string_view::npos && colons + 2 < raw.size())
                        raw = raw.substr(colons + 2);

                    std::string label = PyIdentifier(raw);
                    if (label.empty()) continue;
                    if (!used.insert(label).second) continue;   // IntEnum rejects a repeat
                    text += std::format("    {} = {}\n", label, value.value);
                }
                if (record.values.empty()) text += "    ...\n";
                text += "\n";
            }

            for (const auto* list : {&package->classes, &package->structs}) {
                const bool is_class = list == &package->classes;
                for (const auto& record : *list) {
                    const auto it = index.types.find(record.path);
                    if (it == index.types.end()) continue;
                    ++stub_classes;

                    std::string base = "object";
                    if (!record.super.empty()) {
                        const auto super = index.types.find(record.super);
                        if (super != index.types.end()) base = super->second.identifier;
                    }

                    text += std::format("class {}({}):\n", it->second.identifier, base);
                    text += std::format("    \"\"\"{}  ({} bytes{})\"\"\"\n", record.path,
                                        record.size,
                                        is_class ? ", class" : ", struct");
                    text += std::format("    __path__: ClassVar[str] = {}\n",
                                        PyString(record.path));
                    text += std::format("    __size__: ClassVar[int] = {}\n", record.size);
                    if (record.alignment > 0)
                        text += std::format("    __align__: ClassVar[int] = {}\n",
                                            record.alignment);
                    if (record.vtable_rva != 0)
                        text += std::format("    __vtable_rva__: ClassVar[int] = 0x{:X}\n",
                                            record.vtable_rva);

                    // declared here only. inherited ones come off the base class,
                    // the way Python already resolves attributes.
                    text += "    __offsets__: ClassVar[Dict[str, int]] = {";
                    bool first = true;
                    for (const auto& property : record.properties) {
                        if (!first) text += ", ";
                        first = false;
                        text += std::format("{}: {}", PyString(property.name),
                                            property.offset);
                    }
                    text += "}\n";

                    if (!record.interfaces.empty()) {
                        text += "    __interfaces__: ClassVar[List[str]] = [";
                        bool first_interface = true;
                        for (const auto& interface_path : record.interfaces) {
                            if (!first_interface) text += ", ";
                            first_interface = false;
                            text += PyString(interface_path);
                        }
                        text += "]\n";
                    }

                    std::unordered_set<std::string> members;
                    members.insert("__path__");
                    members.insert("__size__");
                    members.insert("__offsets__");

                    for (const auto& property : record.properties) {
                        std::string name = PyIdentifier(property.name);
                        if (!members.insert(name).second) {
                            for (int suffix = 2;; ++suffix) {
                                std::string candidate = std::format("{}_{}", name, suffix);
                                if (members.insert(candidate).second) { name = candidate; break; }
                            }
                        }

                        std::string annotation = Annotation(index, property.type);
                        if (property.array_dim > 1) annotation = "List[" + annotation + "]";

                        std::string comment = std::format("  # 0x{:04X}(0x{:04X})",
                                                          static_cast<unsigned>(property.offset),
                                                          static_cast<unsigned>(property.size));
                        if (property.is_bitfield)
                            comment += std::format(" bit {}", property.bit_index);
                        if (!property.default_value.empty())
                            comment += " = " + OneLine(property.default_value);

                        text += std::format("    {}: {}{}\n", name, annotation, comment);
                    }

                    for (const auto& function : record.functions) {
                        std::string name = PyIdentifier(function.name);
                        if (!members.insert(name).second) continue;

                        std::string params;
                        std::string returns = "None";
                        for (const auto& param : function.params) {
                            if (param.is_return) {
                                returns = Annotation(index, param.type);
                                continue;
                            }
                            params += ", " + PyIdentifier(param.name) + ": " +
                                      Annotation(index, param.type);
                        }

                        std::string comment;
                        if (function.native_rva != 0)
                            comment = std::format("  # rva 0x{:X}", function.native_rva);
                        else if (function.script_size > 0)
                            comment = std::format("  # {} bytes of bytecode",
                                                  function.script_size);

                        text += std::format("    def {}(self{}) -> {}: ...{}\n",
                                            name, params, returns, comment);
                    }

                    if (record.properties.empty() && record.functions.empty())
                        text += "    ...\n";
                    text += "\n";
                }
            }
        }

        const std::string path = Join(package_dir, module + ".pyi");
        std::string error;
        if (!util::WriteFile(path, text, error)) {
            result.error = std::move(error);
            return result;
        }
        result.files.push_back(path);
    }

    // __init__ re-exports the lot, so `from zircon_sdk import AActor` works without knowing
    // which UE package Actor lives in.
    std::string init;
    init += "# Generated by Zircon - Unreal Engine reflection toolkit.\n";
    init += std::format("# Source: {}\n", dump.header.source.process.empty()
                                              ? "unknown" : dump.header.source.process);
    init += std::format("# Engine: {}\n", dump.header.engine.version.empty()
                                              ? "unknown" : dump.header.engine.version);
    init += std::format("# {} classes and structs, {} enums, {} module(s).\n",
                        stub_classes, stub_enums, modules.size());
    init += "\n";
    init += std::format("TOOL_VERSION: str = {}\n", PyString(dump.header.tool_version));
    init += std::format("ENGINE_VERSION: str = {}\n", PyString(dump.header.engine.version));
    init += std::format("PROCESS: str = {}\n", PyString(dump.header.source.process));
    init += "\n";
    init += "OFFSETS: dict[str, int] = {\n";
    for (const auto& offset : dump.header.offsets)
        init += std::format("    {}: {},\n", PyString(offset.name), offset.value);
    init += "}\n\n";

    for (const auto& [module, in_module] : modules) {
        (void)in_module;
        init += std::format("from .{} import *\n", module);
    }

    const std::string init_path = Join(package_dir, "__init__.pyi");
    std::string error;
    if (!util::WriteFile(init_path, init, error)) {
        result.error = std::move(error);
        return result;
    }
    result.files.push_back(init_path);

    // marks this as a stub-only package. without it a type checker goes looking for a .py
    // next to the .pyi and gives up.
    const std::string marker = Join(package_dir, "py.typed");
    if (!util::WriteFile(marker, "", error)) {
        result.error = std::move(error);
        return result;
    }
    result.files.push_back(marker);

    if (stub_classes == 0)
        result.warnings.push_back("no classes or structs matched; the stubs are empty");

    return result;
}

} // namespace zircon::emit
