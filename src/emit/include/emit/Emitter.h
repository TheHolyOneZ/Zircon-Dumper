#pragma once

// Emitters render an ir::Dump into some output format. Every one is a pure function of
// the IR: no memory access, no target, no engine knowledge. That is what lets them be
// tested against checked-in fixtures with no game installed, and it is why adding a
// format never touches the engine layer.

#include "ir/Model.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::emit {

struct EmitOptions {
    std::string out_dir{"."};

    // Substring match on package name. Empty means every package.
    std::string package_filter;

    // Some formats are naturally one file (a mapping table), others naturally many (a
    // header per package). Where both make sense, this picks.
    bool single_file{false};

    // Emitters must not invent data. When the dump is partial — a static-image dump has
    // no objects at all — an emitter that needs what is missing refuses instead of
    // producing plausible-looking output from absent data.
    bool allow_partial{false};
};

struct EmitResult {
    std::vector<std::string> files;    // paths actually written
    std::vector<std::string> warnings; // non-fatal problems worth surfacing
    std::string error;                 // empty on success

    bool ok() const { return error.empty(); }
};

// std::function, not a plain pointer, because a plugin's emitter is a C callback
// plus a context pointer, and there is no useful way to smuggle the context through a
// bare function pointer. Called once per emit, so the indirection costs nothing.
using EmitFn = std::function<EmitResult(const ir::Dump&, const EmitOptions&)>;

struct Emitter {
    std::string name;          // "cpp_sdk", used on the command line
    std::string description;
    bool        needs_objects; // refuse a partial dump unless allow_partial
    EmitFn      emit;
};

std::span<const Emitter> Emitters();
const Emitter* FindEmitter(std::string_view name);

// Adds an emitter at runtime — how a plugin gets into `zircon emit --list`. Refused if the
// name is taken, so a plugin cannot quietly shadow a built-in: a user who asks for
// cpp_sdk must get the cpp_sdk they expect.
bool RegisterEmitter(Emitter emitter);

// Shared helpers, so every emitter spells these the same way.
namespace util {

// A C++ identifier that cannot collide with a keyword and contains no characters the
// language rejects. UE names legitimately contain spaces, colons and punctuation.
std::string SanitizeIdentifier(std::string_view name);

// The trailing segment of a UE path: "/Script/Engine.Actor" -> "Actor".
std::string LeafName(std::string_view path);

// The package of a UE path: "/Script/Engine.Actor" -> "/Script/Engine".
std::string PackageName(std::string_view path);

// "/Script/Engine" -> "Engine", suitable for a file name.
std::string PackageFileStem(std::string_view package);

// The UE C++ prefix for a type: 'A' for actors, 'U' for other objects, 'F' for structs,
// 'E' for enums, 'I' for interfaces. Derived by walking the super chain in the dump,
// since nothing in the reflection data states it directly.
char CppPrefixFor(const ir::Dump& dump, const ir::Struct& record);

bool WriteFile(std::string_view path, std::string_view contents, std::string& error);
bool EnsureDirectory(std::string_view path, std::string& error);

} // namespace util
} // namespace zircon::emit
