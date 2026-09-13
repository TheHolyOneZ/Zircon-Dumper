# Architecture

How the code is laid out and why. For what the tool does, see the README.

## Layering

```
  app/      CLI shell            dll/   injected payload     gui/  browser
                \                  |                          /
                 +---------- emit/  +  diff/ -----------------+
                                \      /
                                  ir/                <- pure data, links nothing
                                   ^
                              engine/                <- Unreal reflection
                                   ^
                                core/                <- memory, PE, patterns
```

Arrows point one way. `core/` knows nothing about Unreal. `engine/` knows nothing about
output formats. `ir/` knows nothing about anything. `emit/` and `diff/` see the IR and
never touch a process.

This is enforced by the CMake targets rather than by convention: `zircon_ir` links
nothing at all, and `zircon_emit` and `zircon_diff` link only `zircon_ir`. Violating the
rule does not compile.

## core/

### IMemorySource

Every read in the entire program goes through one interface:

```cpp
class IMemorySource {
public:
    virtual ~IMemorySource() = default;

    // Returns bytes actually read. Short reads are legal and callers must handle them:
    // page boundaries in a live process, unmapped tails in a dump.
    virtual std::size_t Read(Address addr, void* out, std::size_t size) = 0;

    // Only Internal and External implement this, and it is off unless asked for.
    virtual bool Write(Address, const void*, std::size_t) { return false; }

    virtual std::span<const ModuleInfo> Modules() const = 0;
    virtual std::span<const RegionInfo> Regions() const = 0;
    virtual Capabilities                Caps()    const = 0;
    virtual std::string                 Describe() const = 0;
};
```

Four providers implement it: `Internal` (injected), `External` (OpenProcess + RPM),
`Dump` (a full-memory minidump) and `Static` (a PE on disk). Because the reflection
walker is written against the interface, all four get every feature for free, and a dump
taken through one can be compared against a dump taken through another. That comparison
is the strongest correctness check the project has.

`Capabilities` is how the layers above ask what a target can do, instead of testing which
provider they were handed:

```cpp
struct Capabilities {
    bool live_objects;       // GObjects is populated (false for Static)
    bool writable;
    bool can_call;           // Internal only: we can invoke game functions
    bool full_address_space;
};
```

Typed helpers (`Read<T>`, `ReadArray<T>`, `ReadCString`) are free functions over the
interface, never virtuals. One virtual call per read is already the bottleneck in
External mode.

A page cache sits in front of every provider. Without it a full walk issues millions of
tiny reads, which is tolerable in-process and ruinous across a process boundary.

### The rest

`PeImage` parses headers, sections and exports from any provider, so the same code reads
a mapped module and a file on disk. `PatternScanner` searches a module or the whole
address space. `Injector` loads the payload, and refuses when anti-cheat is present.

## engine/

This is where all the Unreal knowledge lives, and all of it is derived at runtime.

**EngineProfile** identifies the engine generation: FProperty vs UProperty, `FNamePool`
vs `TNameEntryArray`, chunked vs fixed object array, case-preserving names. A version
string in `.rdata` is used as corroboration when one exists, never as the source of
truth — plenty of shipped games have none, and licensee builds carry their own.

**ObjectArray** finds `GObjects`. The anchor is that the object in slot *i* stores *i* in
its own `InternalIndex`, which nothing else in the address space satisfies across a whole
array.

**NamePool** finds the name pool and derives its stride, block-offset bits and entry
shape. The anchor is that block 0 begins with `None`, always the first name interned.

**ObjectLayout / StructLayout / PropertyLayout / ClassLayout / FunctionLayout /
EnumLayout** derive the member offsets of each engine type. Each one reports a confidence
value and the evidence behind it, and every conclusion is recorded in the dump header.

**TypeResolver** turns a property object into a `TypeRef`. **ValueReader** reads a
property's current value, or its default out of the class default object.

**Kismet** decodes bytecode: an opcode table, then a linear walk, then enough
control-flow reconstruction to print something readable. An unknown opcode stops the walk
and says where, because a misaligned decode produces confident nonsense.

**DumpBuilder** walks everything above into the IR.

### The rule these all follow

A field that scores perfectly on a weak constraint is not the right field. Every
derivation needs a positive distinguishing property *and* an independent anchor whose
correct value is known ahead of time. Internal consistency on its own has been wrong
every time it was the only test — six times, each from real game data. The README lists
the six.

The corollary: never emit plausible-but-wrong output. An unresolvable type becomes an
opaque array of the right size, an unknown opcode stops the walk, an ambiguous match is
refused. A missing name is visible; a member at the wrong offset is not.

## ir/

`Model.h` is the contract. Plain structs, no behaviour, no dependencies. The JSON reader
and writer are hand-rolled — the shapes involved are simple, and a dependency here would
be inherited by every consumer of the IR.

`zircon validate` parses a real dump and re-emits it, checking the result is identical.
Synthetic fixtures cannot cover what a 30 MB dump of a shipped game contains, so that
round-trip is what actually proves the serializer.

The dump's exact structure is documented in the README under "What the dump contains".

### Why the IR is the contract

Emitters are pure functions of it, so they are testable with no game running. Diffing is
IR to IR, so it works across providers and across builds. Anyone can write tooling in any
language against `dump.json` without touching this C++ at all.

`TypeRef` has an `unknown` kind, and that is load-bearing rather than an oversight. An
unrecognised property class keeps its raw class name and its size, so a dump never fails
wholesale because a game shipped a custom property type.

## emit/ and diff/

One file per output format, all of them pure functions from the IR. Adding a format means
adding a file and registering it; it never means touching `engine/`.

`diff/` classifies changes between two dumps by what they break, and renders text, JSON or
Markdown. The exit code carries the verdict so a build script can gate on it.

## plugin/

The host side of the C ABI in `include/zircon/plugin.h`. Plugins add formats, and can
supply an `FName` decoder or a global resolver for targets that deriving alone will not
reach.

Fields are addressed by name rather than through typed getters. That is the one decision
in the ABI worth knowing about: adding a field to the IR is then not an ABI break, and
nodes become introspectable, which is what makes the Lua binding possible at all.

Nothing a plugin returns is trusted. A resolver says where `GObjects` might be; the
address still has to pass the `InternalIndex` check or it is dropped and the scan runs. A
decoder's output still has to pass the same length and printable checks as any other
name. A hook supplies an input, never an answer.

See `PLUGINS.md` for the authoring side.
