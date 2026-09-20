#include "Emitters.h"

#include <algorithm>
#include <vector>

namespace zircon::emit {
namespace {

// This order is what `zircon emit --list` prints, so it's the order a reader meets them
// in: parity target first, then machine formats, then documentation. Plugins append, so
// built-ins always come first. What shipped with the tool, then what this machine has.
std::vector<Emitter>& Registry() {
    static std::vector<Emitter> registry = [] {
        std::vector<Emitter> built_in;

        // Reserve once. Growing later invalidates every pointer FindEmitter has handed
        // out, and the CLI holds one across a whole emit.
        built_in.reserve(256);
        built_in.push_back({"cpp_sdk", "C++ SDK headers with static_assert offset checks", true, &EmitCppSdk});
        built_in.push_back({"csharp", "C# source tree: one folder per assembly, one file per type", true, &EmitCSharp});
        built_in.push_back({"usmap", "UE4SS / FModel .usmap mappings", true, &EmitUsmap});
        built_in.push_back({"ida", "IDA Pro Python script importing types", true, &EmitIda});
        built_in.push_back({"ghidra", "Ghidra Python script importing types", true, &EmitGhidra});
        built_in.push_back({"binja", "Binary Ninja Python script importing types", true, &EmitBinja});
        built_in.push_back({"reclass", "ReClass.NET node file", true, &EmitReClass});
        built_in.push_back({"docs", "Browsable Markdown API reference", true, &EmitDocs});
        built_in.push_back({"frida_js", "Frida JavaScript bindings with live property accessors", true, &EmitFridaJs});
        built_in.push_back({"python_stubs", "Python .pyi type stubs for the whole type system", true, &EmitPythonStubs});
        built_in.push_back({"graphs", "Inheritance graphs in DOT and Mermaid", true, &EmitGraphs});
        built_in.push_back({"json", "Re-emit the IR as JSON", false, &EmitJson});
        return built_in;
    }();
    return registry;
}

} // namespace

std::span<const Emitter> Emitters() { return Registry(); }

const Emitter* FindEmitter(std::string_view name) {
    const auto& registry = Registry();
    const auto it = std::find_if(registry.begin(), registry.end(),
                                 [&](const Emitter& e) { return e.name == name; });
    return it == registry.end() ? nullptr : &*it;
}

bool RegisterEmitter(Emitter emitter) {
    if (emitter.name.empty() || !emitter.emit) return false;
    if (FindEmitter(emitter.name)) return false;

    // Past the reservation the vector reallocates and dangles those pointers. Refuse.
    auto& registry = Registry();
    if (registry.size() == registry.capacity()) return false;

    registry.push_back(std::move(emitter));
    return true;
}

} // namespace zircon::emit
