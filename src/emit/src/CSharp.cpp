#include "Emitters.h"

#include <algorithm>
#include <filesystem>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zircon::emit {
namespace {

// A C# source tree, the shape a decompiler leaves behind: one folder per assembly, one file
// per top-level type, namespaces as directories.
//
// What is real here: names, namespaces, base types, interfaces, field types, field offsets,
// method signatures, RVAs, enum values. All of it read from the runtime or the metadata.
//
// What is not: method bodies. A body is IL, and no part of this tool reads IL -- the dump
// holds reflection data. Every emitted body is empty and every file says so at the top. The
// alternative was inventing statements, and a plausible wrong body is worse than no body.
//
// So it does not compile, and is not meant to. It is meant to be read, grepped and diffed,
// which is what the .cs tree out of Il2CppDumper actually gets used for.

const std::set<std::string_view> kKeywords = {
    "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked",
    "class", "const", "continue", "decimal", "default", "delegate", "do", "double", "else",
    "enum", "event", "explicit", "extern", "false", "finally", "fixed", "float", "for",
    "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
    "long", "namespace", "new", "null", "object", "operator", "out", "override", "params",
    "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed",
    "short", "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw",
    "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using",
    "virtual", "void", "volatile", "while",
};

// C# lets you write a keyword as an identifier with @ in front, and IL2CPP assemblies do
// contain them -- obfuscators like the shortest legal names, and those collide.
std::string Identifier(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "_unnamed";
    if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
    if (kKeywords.count(out)) out.insert(out.begin(), '@');
    return out;
}

// Short enough to be a file name, and still its own file name.
//
// An inflated generic carries every type argument in full, nested, so these run to hundreds
// of characters -- one in Road 96 was 190 on its own and the write failed at MAX_PATH. The
// hash is what keeps two truncated names apart, since the part that differs is usually the
// tail that got cut.
std::string Shorten(std::string_view name, std::size_t cap) {
    if (name.size() <= cap) return std::string(name);

    std::uint64_t hash = 1469598103934665603ull;          // FNV-1a
    for (const unsigned char c : name) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return std::format("{}_{:08X}", name.substr(0, cap - 9), static_cast<std::uint32_t>(hash));
}

// Something that can be a file or folder name on Windows. Generic names carry < > and ,
// and a path with those in it is a bad time.
std::string PathSafe(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool bad = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
                         c == '\\' || c == '|' || c == '?' || c == '*' || c == ',' ||
                         static_cast<unsigned char>(c) < 0x20;
        out.push_back(bad ? '_' : c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "_";
    return out;
}

// A file name for one type. Separate from PathSafe, which also names the assembly folder
// and has to leave the spaces in "Cave Crawlers" alone. A type name has no business
// containing one, and List`3 turning into "Foo_T1_ T2_ T3_.cs" is just untidy.
std::string FileStem(std::string_view name) {
    std::string out = PathSafe(name);
    for (char& c : out)
        if (c == ' ') c = '_';

    std::string tidy;
    for (const char c : out) {
        if (c == '_' && !tidy.empty() && tidy.back() == '_') continue;
        tidy.push_back(c);
    }
    while (!tidy.empty() && tidy.back() == '_') tidy.pop_back();
    return tidy.empty() ? std::string("_") : tidy;
}

// The name half of an IL2CPP path. "Game.Boss, Assembly-CSharp" -> "Game.Boss".
std::string_view NamePart(std::string_view path) {
    const auto comma = path.rfind(", ");
    return comma == std::string_view::npos ? path : path.substr(0, comma);
}

// The assembly half, or empty on a path that has none.
std::string_view AssemblyPart(std::string_view path) {
    const auto comma = path.rfind(", ");
    return comma == std::string_view::npos ? std::string_view{} : path.substr(comma + 2);
}

// `1 -> <T>, `2 -> <T1, T2>.
//
// The arity is all there is. IL2CPP keeps generic parameter names in the metadata but the IR
// does not carry them, so these are placeholders and a reader should know it. An inflated
// type ("List<System.Int32>") already has its arguments in the name and comes through here
// untouched.
std::string Ungeneric(std::string_view name) {
    const auto tick = name.find('`');
    if (tick == std::string_view::npos) return std::string(name);

    std::size_t at = tick + 1;
    int arity = 0;
    while (at < name.size() && name[at] >= '0' && name[at] <= '9') {
        arity = arity * 10 + (name[at] - '0');
        ++at;
    }
    if (arity <= 0) return std::string(name.substr(0, tick));

    // Anything after the digits is an argument list the runtime already filled in.
    const auto tail = name.substr(at);
    std::string out(name.substr(0, tick));
    if (!tail.empty() && tail.front() == '<') return out + std::string(tail);

    out += '<';
    for (int i = 0; i < arity; ++i) {
        if (i) out += ", ";
        out += arity == 1 ? "T" : std::format("T{}", i + 1);
    }
    out += '>';
    return out + std::string(tail);
}

// A name as it is written in a declaration.
//
// Identifier() has to run on the name and not on the parameter list: it turns anything that
// is not a letter into an underscore, so Identifier(Ungeneric("Box`1")) gave Box_T_ instead
// of Box<T>. The angle brackets are the one bit of punctuation that has to survive.
std::string DeclarationName(std::string_view name) {
    const std::string full = Ungeneric(name);
    const auto open = full.find('<');
    if (open == std::string::npos) return Identifier(full);

    // A name that starts with < is not generic. The C# compiler names a lambda
    // <EnclosingMethod>b__11_0 and a closure class <>c__DisplayClass12_0, and those angle
    // brackets are part of the name rather than a parameter list.
    if (open == 0) return Identifier(full);

    return Identifier(std::string_view(full).substr(0, open)) + full.substr(open);
}

// Split "A.B.C" on dots that are not inside a generic argument list.
std::vector<std::string> SplitDots(std::string_view text) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (const char c : text) {
        if (c == '<' || c == '[') ++depth;
        if (c == '>' || c == ']') --depth;
        if (c == '.' && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    parts.push_back(current);
    return parts;
}

// Where a type sits: which assembly, which namespace, and the chain of outer types down to
// it. Worked out once per record rather than re-split at every use.
struct Placement {
    std::string assembly;
    std::string name_space;
    std::vector<std::string> nesting;   // outermost first; the last one is this type
};

Placement Place(const ir::Struct& record, std::string_view fallback_assembly) {
    Placement out;
    out.assembly = std::string(AssemblyPart(record.path));
    if (out.assembly.empty()) out.assembly = std::string(fallback_assembly);
    out.name_space = record.name_space;

    std::string_view name = NamePart(record.path);

    // The namespace is a prefix of the name, and the rest is the nesting chain. Trimming it
    // by length beats splitting on dots: a namespace has dots of its own and there is no
    // telling from the text where it stops.
    if (!out.name_space.empty() && name.size() > out.name_space.size() + 1 &&
        name.compare(0, out.name_space.size(), out.name_space) == 0 &&
        name[out.name_space.size()] == '.') {
        name = name.substr(out.name_space.size() + 1);
    }

    out.nesting = SplitDots(name);
    if (out.nesting.empty()) out.nesting.push_back("_unnamed");
    return out;
}

// Everything the emitter needs to know about one record, next to the record.
struct Entry {
    const ir::Struct* record{nullptr};
    const ir::Enum*   enum_record{nullptr};
    Placement         where;
};

// The aliases. The runtime answers with the framework name, and C# spells most of them
// differently -- the file should look like the code it came from.
const std::unordered_map<std::string_view, std::string_view> kAliases = {
    {"System.Void", "void"},       {"System.Object", "object"},
    {"System.Boolean", "bool"},    {"System.Char", "char"},
    {"System.SByte", "sbyte"},     {"System.Byte", "byte"},
    {"System.Int16", "short"},     {"System.UInt16", "ushort"},
    {"System.Int32", "int"},       {"System.UInt32", "uint"},
    {"System.Int64", "long"},      {"System.UInt64", "ulong"},
    {"System.Single", "float"},    {"System.Double", "double"},
    {"System.Decimal", "decimal"}, {"System.String", "string"},
    {"System.IntPtr", "IntPtr"},   {"System.UIntPtr", "UIntPtr"},
};

// Empty when there is no alias for it.
std::string_view Alias(std::string_view full_name) {
    const auto it = kAliases.find(full_name);
    return it == kAliases.end() ? std::string_view{} : it->second;
}

// A type reference as C# spells it.
//
// Prefers the path the dump recorded over the kind, because on an IL2CPP dump the path is
// the real name and the kind is this IR's approximation of it. Falls back to the kind for
// Unreal dumps and for anything the walk could not resolve.
std::string TypeName(const ir::TypeRef& type);

// Splits "A, B<C, D>, E" on the commas between arguments, not the ones inside them.
std::vector<std::string> SplitArgs(std::string_view text) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (const char c : text) {
        if (c == '<' || c == '[') ++depth;
        if (c == '>' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

std::string ShortName(std::string_view path) {
    auto name = NamePart(path);

    // An array of a type the alias table knows: System.Int32[][] is int[][]. Peeled off
    // first, or the lookup misses and the element keeps its framework name.
    std::string suffix;
    while (name.size() >= 2 && name.substr(name.size() - 2) == "[]") {
        suffix += "[]";
        name.remove_suffix(2);
    }

    if (const auto alias = Alias(name); !alias.empty()) return std::string(alias) + suffix;
    if (!suffix.empty()) return ShortName(name) + suffix;

    const auto parts = SplitDots(name);
    std::string leaf = Ungeneric(parts.empty() ? std::string(name) : parts.back());

    // Recurse into the arguments. Without this a Dictionary of two namespaced types was
    // three lines wide for no information.
    const auto open = leaf.find('<');
    if (open == std::string::npos || leaf.back() != '>') return leaf;

    std::string out = leaf.substr(0, open) + "<";
    const auto args = SplitArgs(std::string_view(leaf).substr(open + 1,
                                                              leaf.size() - open - 2));
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) out += ", ";
        std::string_view arg = args[i];
        while (!arg.empty() && arg.front() == ' ') arg.remove_prefix(1);
        out += ShortName(arg);
    }
    return out + ">";
}

std::string Inner(const ir::TypeRef& type, std::size_t index) {
    return index < type.params.size() ? TypeName(type.params[index]) : "object";
}

std::string TypeName(const ir::TypeRef& type) {
    switch (type.kind) {
        case ir::TypeKind::Bool:   return "bool";
        case ir::TypeKind::Int8:   return "sbyte";
        case ir::TypeKind::Int16:  return "short";
        case ir::TypeKind::Int32:  return "int";
        case ir::TypeKind::Int64:  return "long";
        case ir::TypeKind::UInt8:  return "byte";
        case ir::TypeKind::UInt16: return "ushort";
        case ir::TypeKind::UInt32: return "uint";
        case ir::TypeKind::UInt64: return "ulong";
        case ir::TypeKind::Float:  return "float";
        case ir::TypeKind::Double: return "double";
        case ir::TypeKind::Name:
        case ir::TypeKind::String:
        case ir::TypeKind::Text:   return "string";

        case ir::TypeKind::Enum:
        case ir::TypeKind::Struct:
        case ir::TypeKind::ObjectPtr:
        case ir::TypeKind::ClassPtr:
        case ir::TypeKind::Interface:
            if (!type.name.empty()) return ShortName(type.name);
            break;

        // Unreal's pointer wrappers. No C# spelling, so they keep the engine's own and read
        // as what they are rather than as something they are not.
        case ir::TypeKind::WeakPtr:      return "TWeakObjectPtr<" + Inner(type, 0) + ">";
        case ir::TypeKind::LazyPtr:      return "TLazyObjectPtr<" + Inner(type, 0) + ">";
        case ir::TypeKind::SoftPtr:      return "TSoftObjectPtr<" + Inner(type, 0) + ">";
        case ir::TypeKind::SoftClassPtr: return "TSoftClassPtr<" + Inner(type, 0) + ">";

        case ir::TypeKind::Array:    return Inner(type, 0) + "[]";
        case ir::TypeKind::Set:      return "HashSet<" + Inner(type, 0) + ">";
        case ir::TypeKind::Map:      return "Dictionary<" + Inner(type, 0) + ", " +
                                            Inner(type, 1) + ">";
        case ir::TypeKind::Optional: return Inner(type, 0) + "?";

        case ir::TypeKind::Delegate:
        case ir::TypeKind::MulticastDelegate:
            return type.name.empty() ? "Delegate" : ShortName(type.name);

        case ir::TypeKind::FieldPath: return "FieldPath";
        case ir::TypeKind::Unknown:   break;
    }

    if (!type.name.empty()) return ShortName(type.name);

    // The engine's own word for it, when that is a name. A static read puts a sentence
    // there instead -- "<from the binary>" -- and sanitising a sentence into an identifier
    // produced `_from_the_binary_`, which looks like a type somebody declared. The sentence
    // goes in the comment beside the field instead.
    if (!type.raw.empty()) {
        if (const auto alias = Alias(type.raw); !alias.empty()) return std::string(alias);
        const char first = type.raw.front();
        if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_')
            return Identifier(type.raw);
    }
    return "object";
}

std::string Indent(int depth) { return std::string(static_cast<std::size_t>(depth) * 4, ' '); }

// is_interface, checked against what the rest of the record says.
//
// Dumps written before 0.8.0 read this flag through an int-returning signature, and the
// runtime only sets the low byte, so it is true about a third of the time at random --
// UnityEngine.Vector3 and every compiler-generated closure class came out as interfaces.
// An interface has no base type and no instance fields, so a record with either is not one
// whatever the flag says. A dump taken since the fix passes this anyway.
bool ReallyInterface(const ir::Struct& record) {
    if (!record.is_interface) return false;
    if (record.is_valuetype) return false;
    if (!record.super.empty()) return false;
    for (const auto& field : record.properties)
        if (!field.is_static) return false;
    return true;
}

// Whatever the runtime said, rendered as the modifiers that go in front of a declaration.
std::string Modifiers(const ir::Struct& record) {
    std::string out = "public ";
    if (record.is_valuetype) return out;         // a struct is never abstract
    if (ReallyInterface(record)) return out;

    // is_abstract came through the same broken signature, so it gets the same treatment: a
    // type with fields of its own is not abstract in any way worth printing.
    if (record.is_abstract && record.properties.empty()) out += "abstract ";
    return out;
}

std::string_view Keyword(const ir::Struct& record) {
    // Value type first. It is the one the runtime answers directly, and a type that claims
    // to be both is a dump from before the flags were read correctly.
    if (record.is_valuetype) return "struct";
    if (ReallyInterface(record)) return "interface";
    return record.is_class ? "class" : "struct";
}

// The ": Base, IFoo, IBar" part.
std::string Bases(const ir::Struct& record) {
    std::vector<std::string> names;

    // Every class derives from object and every struct from ValueType, and C# writes
    // neither. Left in, half the tree carried a base that says nothing.
    const auto base = ShortName(record.super);
    if (!record.super.empty() && base != "object" && base != "ValueType")
        names.push_back(base);
    for (const auto& iface : record.interfaces) names.push_back(ShortName(iface));
    if (names.empty()) return {};

    std::string out = " : ";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

struct Context {
    bool offsets_known{true};   // false on a static dump: the numbers live in the binary
};

// The IR names these after its own TypeKind. An enum declared ": int32" is not C#.
std::string_view Underlying(std::string_view name) {
    if (name == "int8")   return "sbyte";
    if (name == "int16")  return "short";
    if (name == "int32")  return "int";
    if (name == "int64")  return "long";
    if (name == "uint8")  return "byte";
    if (name == "uint16") return "ushort";
    if (name == "uint32") return "uint";
    if (name == "uint64") return "ulong";
    if (name == "bool")   return "byte";      // an enum cannot sit on bool
    return name.empty() ? std::string_view("int") : name;
}

void WriteEnum(std::string& out, const ir::Enum& record, int depth) {
    const auto pad = Indent(depth);

    if (!record.values_resolved) {
        out += std::format("{}// Values were not readable from this build, so the names are\n"
                           "{}// here without them rather than with invented ones.\n",
                           pad, pad);
    }
    if (record.is_flags) out += std::format("{}[Flags]\n", pad);

    out += std::format("{}public enum {} : {}\n{}{{\n", pad,
                       Identifier(ShortName(record.path.empty() ? record.name : record.path)),
                       Underlying(record.underlying), pad);

    for (const auto& value : record.values) {
        if (record.values_resolved)
            out += std::format("{}    {} = {},\n", pad, Identifier(value.name), value.value);
        else
            out += std::format("{}    {},\n", pad, Identifier(value.name));
    }
    out += std::format("{}}}\n", pad);
}

void WriteField(std::string& out, const ir::Property& field, const Context& context,
                int depth) {
    const auto pad = Indent(depth);

    std::string note;
    if (!context.offsets_known) {
        // Checked first. Read statically, every field is unresolved for one reason, and it
        // is not the open-generic one.
        note = "type and offset are in the binary, not in the metadata";
    } else if (field.is_static) {
        note = "static field";
    } else if (field.offset_unresolved) {
        note = "offset unknown: an open generic has no laid-out fields";
    } else {
        note = std::format("0x{:X}", field.offset);
        if (field.boxed_offset >= 0 && field.boxed_offset != field.offset)
            note += std::format(" (0x{:X} boxed)", field.boxed_offset);
    }
    if (field.is_bitfield) note += std::format(", bit {}", field.bit_index);
    if (context.offsets_known && field.type.kind == ir::TypeKind::Unknown &&
        !field.type.raw.empty() && field.type.raw.front() == '<')
        note += std::format(", type {}", field.type.raw);

    out += std::format("{}public {}{} {}; // {}\n", pad, field.is_static ? "static " : "",
                       TypeName(field.type), Identifier(field.name), note);
}

void WriteAccessor(std::string& out, const ir::Accessor& accessor, int depth) {
    const auto pad = Indent(depth);
    std::string body = " { ";
    if (!accessor.getter.empty()) body += "get; ";
    if (!accessor.setter.empty()) body += "set; ";
    body += "}";
    out += std::format("{}public {} {}{}\n", pad, TypeName(accessor.type),
                       Identifier(accessor.name), body);
}

void WriteMethod(std::string& out, const ir::Function& method, std::string_view owner,
                 int depth) {
    const auto pad = Indent(depth);

    std::string returns = "void";
    std::string params;
    for (const auto& param : method.params) {
        if (param.is_return) {
            returns = TypeName(param.type);
            continue;
        }
        if (!params.empty()) params += ", ";
        if (param.is_out) params += "out ";
        params += std::format("{} {}", TypeName(param.type), Identifier(param.name));
    }

    // The runtime calls a constructor ".ctor". Sanitised, that became _ctor with a void
    // return, which is neither what it is called nor how it is written.
    const bool constructor  = method.name == ".ctor";
    const bool static_setup = method.name == ".cctor";
    if (constructor || static_setup) returns.clear();

    std::string note;
    if (method.native_rva)
        note = std::format(" // RVA: 0x{:X}", method.native_rva);
    if (method.shared_body)
        note += note.empty() ? " // shared body: more than one method is at this address"
                             : ", shared body";

    if (constructor || static_setup) {
        out += std::format("{}public {}{}({}){{ }}{}\n", pad, static_setup ? "static " : "",
                           DeclarationName(owner), params, note);
        return;
    }
    out += std::format("{}public {} {}({}){{ }}{}\n", pad, returns,
                       DeclarationName(method.name), params, note);
}

void WriteType(std::string& out, const Entry& entry,
               const std::unordered_map<std::string, std::vector<const Entry*>>& nested_of,
               const Context& context, int depth);

void WriteMembers(std::string& out, const Entry& entry,
                  const std::unordered_map<std::string, std::vector<const Entry*>>& nested_of,
                  const Context& context, int depth) {
    const auto& record = *entry.record;

    for (const auto& field : record.properties) WriteField(out, field, context, depth);
    if (!record.properties.empty() && !record.accessors.empty()) out += "\n";
    for (const auto& accessor : record.accessors) WriteAccessor(out, accessor, depth);
    if ((!record.properties.empty() || !record.accessors.empty()) && !record.functions.empty())
        out += "\n";
    for (const auto& method : record.functions)
        WriteMethod(out, method, entry.where.nesting.back(), depth);

    const auto it = nested_of.find(record.path);
    if (it == nested_of.end()) return;
    for (const auto* child : it->second) {
        out += "\n";
        WriteType(out, *child, nested_of, context, depth);
    }
}

void WriteType(std::string& out, const Entry& entry,
               const std::unordered_map<std::string, std::vector<const Entry*>>& nested_of,
               const Context& context, int depth) {
    if (entry.enum_record) {
        WriteEnum(out, *entry.enum_record, depth);
        return;
    }
    const auto& record = *entry.record;
    const auto pad = Indent(depth);

    std::string facts;
    if (record.token) facts += std::format("Token: 0x{:08X}  ", record.token);
    if (record.size)  facts += std::format("Size: 0x{:X}  ", record.size);
    if (record.vtable_rva) facts += std::format("VTable: 0x{:X}  ", record.vtable_rva);
    if (!record.source.empty()) facts += std::format("Read from: {}", record.source);
    if (!facts.empty()) out += std::format("{}// {}\n", pad, facts);

    if (record.explicit_layout)
        out += std::format("{}[StructLayout(LayoutKind.Explicit)]\n", pad);

    out += std::format("{}{}{} {}{}\n{}{{\n", pad, Modifiers(record), Keyword(record),
                       DeclarationName(entry.where.nesting.back()), Bases(record), pad);
    WriteMembers(out, entry, nested_of, context, depth + 1);
    out += std::format("{}}}\n", pad);
}

std::string Join(std::string_view dir, std::string_view leaf) {
    std::filesystem::path path(dir);
    path /= leaf;
    return path.lexically_normal().string();
}

} // namespace

EmitResult EmitCSharp(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    // Unreal reflection has no C# in it. cpp_sdk is the tree for those, and printing a
    // UClass as a C# class is the same mistake in the other direction.
    if (dump.header.runtime == "unreal") {
        result.error = "this is an Unreal dump, and its types are C++ rather than C#; "
                       "emit cpp_sdk writes the header tree for those";
        return result;
    }

    // A static read gets its type system from global-metadata.dat, and field offsets are
    // not in that file -- they live in the binary. Printing 0 would read as "first field".
    //
    // source.kind is the one that has always been written. header.sources only reached the
    // static path late in 0.7.0, so a dump from before that has the kind and nothing else.
    const bool static_only =
        dump.header.source.kind == "static" ||
        (dump.header.sources.size() == 1 && dump.header.sources.front() == "static");

    Context context;
    context.offsets_known = !static_only;

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

    // Place everything first. A type's file depends on its nesting, and a nested type is
    // reached from its outer one rather than from a file of its own.
    std::vector<Entry> entries;
    for (const auto* package : packages) {
        std::string fallback = package->name;
        if (fallback.size() > 4 && fallback.compare(fallback.size() - 4, 4, ".dll") == 0)
            fallback.resize(fallback.size() - 4);

        const auto take = [&](const ir::Struct& record) {
            entries.push_back(Entry{&record, nullptr, Place(record, fallback)});
        };
        for (const auto& record : package->classes) take(record);
        for (const auto& record : package->structs) take(record);

        for (const auto& record : package->enums) {
            ir::Struct shim;                       // enums are placed the same way
            shim.path = record.path;
            shim.name = record.name;
            Entry entry;
            entry.enum_record = &record;
            entry.where = Place(shim, fallback);
            entries.push_back(entry);
        }
    }

    // Which types live inside which. The outer type's path is this one's minus the last
    // segment, so a lookup by path finds it without another pass over the dump.
    std::unordered_map<std::string, const Entry*> by_path;
    for (const auto& entry : entries) {
        const auto& path = entry.record ? entry.record->path : entry.enum_record->path;
        by_path.emplace(path, &entry);
    }

    std::unordered_map<std::string, std::vector<const Entry*>> nested_of;
    std::unordered_set<const Entry*> is_nested;
    for (const auto& entry : entries) {
        if (entry.where.nesting.size() < 2) continue;

        std::string outer;
        for (std::size_t i = 0; i + 1 < entry.where.nesting.size(); ++i) {
            if (i) outer += '.';
            outer += entry.where.nesting[i];
        }
        if (!entry.where.name_space.empty()) outer = entry.where.name_space + "." + outer;
        if (!entry.where.assembly.empty()) outer += ", " + entry.where.assembly;

        // No outer type in this dump means the filter kept the inner one and dropped its
        // parent. It gets a file of its own rather than disappearing.
        if (!by_path.count(outer)) continue;
        nested_of[outer].push_back(&entry);
        is_nested.insert(&entry);
    }

    std::string banner = std::format(
        "// Written by Zircon {} from {} reflection data.\n"
        "//\n"
        "// Names, types, offsets, RVAs and enum values are what the game reported. Method\n"
        "// bodies are empty: a body is IL, and this dump holds reflection data rather than\n"
        "// IL. Nothing here was guessed, and nothing here compiles.\n",
        dump.header.tool_version.empty() ? "?" : dump.header.tool_version,
        dump.header.runtime);

    if (static_only && dump.header.runtime == "mono") {
        banner +=
            "//\n"
            "// Read from the game's own managed assemblies with the game not running. Names,\n"
            "// namespaces, tokens, base types, interfaces, enum values and IL RVAs are exact:\n"
            "// all of it is ECMA-335 metadata. Field offsets are absent, because the CLI does\n"
            "// not store them -- the runtime lays a type out the first time it is used. Dump\n"
            "// the game live, or with --mode dual, for offsets.\n";
    } else if (static_only) {
        banner +=
            "//\n"
            "// Read from global-metadata.dat with the game not running. Names, namespaces and\n"
            "// tokens are exact. Field types, field offsets and method addresses are absent,\n"
            "// because they are in the binary rather than in that file -- and every type is\n"
            "// written as a class, because struct and enum come from a type's parent, which is\n"
            "// also in the binary. Dump the game live, or with --mode dual, for those.\n";
    }

    std::string error;
    std::unordered_set<std::string> written;
    int skipped_nested = 0;

    for (const auto& entry : entries) {
        if (is_nested.count(&entry)) {
            ++skipped_nested;
            continue;
        }

        std::filesystem::path folder(options.out_dir);
        folder /= PathSafe(entry.where.assembly.empty() ? "unknown" : entry.where.assembly);
        for (const auto& part : SplitDots(entry.where.name_space))
            if (!part.empty()) folder /= PathSafe(part);

        if (!util::EnsureDirectory(folder.string(), error)) {
            result.error = error;
            return result;
        }

        // Two types can want one file: Foo and Foo`1 differ by a backtick that a file name
        // cannot keep. Numbered rather than silently overwritten.
        std::string stem = Shorten(FileStem(Ungeneric(entry.where.nesting.back())), 80);
        std::string file = Join(folder.string(), stem + ".cs");
        for (int suffix = 2; !written.insert(file).second; ++suffix)
            file = Join(folder.string(), std::format("{}_{}.cs", stem, suffix));

        std::string text = banner;
        text += "\n";
        if (!entry.where.name_space.empty())
            text += std::format("namespace {}\n{{\n", entry.where.name_space);

        const int depth = entry.where.name_space.empty() ? 0 : 1;
        WriteType(text, entry, nested_of, context, depth);

        if (!entry.where.name_space.empty()) text += "}\n";

        if (!util::WriteFile(file, text, error)) {
            result.error = error;
            return result;
        }
        result.files.push_back(file);
    }

    if (!context.offsets_known)
        result.warnings.emplace_back(
            "this dump was read from metadata alone, so field offsets and method RVAs are "
            "absent and the files say so where they would have gone");
    if (skipped_nested > 0)
        result.warnings.push_back(std::format(
            "{} nested types are inside their outer type's file rather than in files of "
            "their own, which is where C# puts them", skipped_nested));

    return result;
}

} // namespace zircon::emit
