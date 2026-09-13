#include "emit/Emitter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

namespace zircon::emit::util {
namespace {

// Reserved words, plus the MSVC extensions that would break a generated header. A UE
// property really can be called "class" or "operator".
const std::set<std::string_view> kReserved = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
    "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
    "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
    "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    // MSVC-specific, and genuinely seen in shipped games.
    "__declspec", "__forceinline", "__int8", "__int16", "__int32", "__int64",
    "interface", "NULL", "TRUE", "FALSE",

    // The SDK's own Basic.hpp names. `uint8 uint8;` shadows the type for every later
    // member of the struct, and UE does ship one with a property called "uint8"
    // (FStructSerializerNumericTestStruct).
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64",
    "FName", "FString", "FText", "TArray", "TMap", "TSet", "TWeakObjectPtr",
    "TLazyObjectPtr", "TSoftObjectPtr", "TSoftClassPtr", "TScriptInterface",
    "TSubclassOf", "TOptional", "FDelegate", "FMulticastDelegate", "FFieldPath",
};

} // namespace

std::string SanitizeIdentifier(std::string_view name) {
    std::string out;
    out.reserve(name.size());

    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out.push_back(c);
        } else {
            // Underscore, don't drop. Dropping merges "Foo Bar" and "FooBar" into one
            // colliding name, without saying so.
            out.push_back('_');
        }
    }

    if (out.empty()) out = "_unnamed";
    if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
    if (kReserved.count(out)) out += '_';
    return out;
}

std::string LeafName(std::string_view path) {
    const auto dot = path.find_last_of('.');
    return std::string(dot == std::string_view::npos ? path : path.substr(dot + 1));
}

std::string PackageName(std::string_view path) {
    const auto dot = path.find('.');
    return std::string(dot == std::string_view::npos ? path : path.substr(0, dot));
}

std::string PackageFileStem(std::string_view package) {
    const auto slash = package.find_last_of('/');
    std::string stem(slash == std::string_view::npos ? package : package.substr(slash + 1));
    return SanitizeIdentifier(stem);
}

char CppPrefixFor(const ir::Dump& dump, const ir::Struct& record) {
    // The walker records this whenever it can, which is always for a dump this tool
    // produced. Faster, and more importantly the only answer that survives filtering: the
    // chain below needs ancestors a filter may have thrown away.
    if (record.cpp_prefix != 0) return record.cpp_prefix;

    if (!record.is_class) return 'F';

    // Walk to the root. Anything under AActor takes 'A', an interface 'I', everything else
    // 'U'. Reflection data never states the prefix, so ancestry is all we have.
    //
    // The path index is cached against the dump it came from. Rebuilding per call made this
    // quadratic: ~24M map inserts on a 5000-class dump called once per class, and callers
    // that reach for it per *property* are far worse. Dumps are immutable while emitters
    // run and emitters take one at a time, so caching the last one is safe and sufficient.
    static const ir::Dump* cached_dump = nullptr;
    static std::unordered_map<std::string, const ir::Struct*> by_path;

    if (cached_dump != &dump) {
        by_path.clear();
        for (const auto& package : dump.packages)
            for (const auto& entry : package.classes) by_path[entry.path] = &entry;
        cached_dump = &dump;
    }

    const ir::Struct* current = &record;
    std::set<std::string> seen;

    while (current) {
        if (current->path == "/Script/Engine.Actor") return 'A';
        if (current->path == "/Script/CoreUObject.Interface") return 'I';
        if (current->super.empty()) break;
        if (!seen.insert(current->path).second) break;   // guards a cyclic dump

        const auto it = by_path.find(current->super);
        current = it == by_path.end() ? nullptr : it->second;
    }
    return 'U';
}

bool EnsureDirectory(std::string_view path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (ec && !std::filesystem::is_directory(std::filesystem::path(path))) {
        error = "cannot create directory '" + std::string(path) + "': " + ec.message();
        return false;
    }
    return true;
}

bool WriteFile(std::string_view path, std::string_view contents, std::string& error) {
    const std::filesystem::path target(path);
    if (target.has_parent_path()) {
        if (!EnsureDirectory(target.parent_path().string(), error)) return false;
    }

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open '" + std::string(path) + "' for writing";
        return false;
    }

    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) {
        error = "write failed for '" + std::string(path) + "'";
        return false;
    }
    return true;
}

} // namespace zircon::emit::util
