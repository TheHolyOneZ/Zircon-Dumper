# Zircon — Scope

## What it is

A reflection extraction and analysis toolkit for Unreal Engine games. It reads a UE
process (or a dump, or a binary on disk), reconstructs the engine's reflection data
into a versioned intermediate representation, and emits that IR into as many useful
formats as we can support.

It is not "a better Dumper-7". Dumper-7 is one feature of this tool (the C++ SDK
emitter), reached through a pipeline that also produces diffs, disassembler types,
mappings, decompiled script bytecode, and a live inspector.

## Design decisions (settled)

| Decision | Choice |
|---|---|
| Memory access | `IMemorySource` abstraction, four providers shipped up front |
| Engine coverage | UE4.20 → UE5.5, runtime fingerprint + offset auto-derivation |
| Output strategy | Reflection → IR → N emitters; IR is the contract |
| Language / toolchain | C++20, MSVC (VS2022), CMake + Ninja, x64 Windows first |
| Scripting | Python for optional post-processing only, never in the hot path |

## The four memory providers

Every read in the reflection layer goes through `IMemorySource`. The walker is written
once; the mode is a runtime choice.

| Provider | Backing | Can enumerate live objects? | Notes |
|---|---|---|---|
| `Internal` | injected DLL, direct pointers | yes | fastest; can also *call* game functions (CDO construction, `StaticFindObject`) |
| `External` | `OpenProcess` + `NtReadVirtualMemory` | yes | no injection; a crash in our code cannot take the game down |
| `Dump` | full-memory minidump on disk | yes, if captured with `MiniDumpWithFullMemory` | offline, reproducible, shareable — dump once, analyse forever |
| `Static` | PE file on disk, sections mapped | **no** | see caveat below |

### Static-mode caveat (important, not a bug)

`GObjects` and the `FName` pool are populated at runtime. A PE on disk has empty
`.data`/`.bss`, so static mode **cannot** enumerate objects or resolve names. What it
*can* do, and what it is for:

- locate `GObjects` / `GNames` / `GWorld` **addresses** by pattern scan, so a later
  live/dump run starts with known-good globals instead of re-scanning
- validate signatures across game builds without launching anything
- feed the disassembler emitters (section layout, imports, exports, relocations)
- diff two game executables at the binary level before you even attach

Static mode produces a *partial* IR, explicitly flagged as such in the dump header.
Any emitter that needs object data will refuse a partial IR rather than emit garbage.

## Feature matrix

Legend: ✅ planned · ⭐ beyond anything in Dumper-7 · ➖ out of scope

| Capability | Dumper-7 | Zircon |
|---|---|---|
| Internal (injected) dumping | yes | ✅ |
| External (no injection) dumping | no | ⭐ |
| Offline dump-file analysis | no | ⭐ |
| Static PE analysis | no | ⭐ |
| `GObjects` / `GNames` auto-discovery | yes | ✅ |
| Chunked **and** fixed `FUObjectArray` | yes | ✅ |
| `UProperty` (≤4.24) and `FProperty` (≥4.25) | yes | ✅ |
| Engine version fingerprinting | partial | ⭐ explicit, reported in dump header |
| Offset auto-derivation | heuristic, hardcoded | ⭐ derivation + confidence score + manual override file |
| Encrypted / custom `FName` decode | no | ⭐ pluggable decoder hooks |
| C++ SDK headers | yes | ✅ parity target |
| Padding members + `static_assert` verification | yes | ✅ |
| Bitfield reconstruction | partial | ⭐ full, with mask/offset preserved in IR |
| `.usmap` mappings (UE4SS / FModel) | some builds | ✅ |
| Machine-readable JSON IR | no | ⭐ the core contract |
| IDA / Ghidra / Binary Ninja type import | no | ⭐ |
| ReClass.NET node files | no | ⭐ |
| Frida JS bindings | no | ⭐ |
| Python type stubs | no | ⭐ |
| Markdown API docs | no | ⭐ |
| Inheritance / interface graphs (dot, mermaid) | no | ⭐ |
| CDO default-value extraction | no | ⭐ |
| `UFunction` native address + vtable index | partial | ⭐ |
| Kismet script bytecode disassembly | no | ⭐ |
| Kismet → pseudo-C++ / pseudo-Blueprint decompile | no | ⭐ |
| Console command dump (`IConsoleManager`) | no | ⭐ |
| Build-to-build dump diffing | no | ⭐ |
| Live interactive object browser | no | ⭐ |
| Plugin API for custom emitters | no | ⭐ |
| Non-Windows hosts | no | ➖ not in v1 |
| Anti-cheat evasion | no | ➖ explicitly excluded |

## Non-goals

- **Anti-cheat evasion / detection bypass.** Handling unusual, packed, or encrypted
  memory layouts is in scope because it is a reverse-engineering problem. Defeating
  protection to gain access we do not otherwise have is not.
- **Game modification.** This tool reads. Writing to a live process is limited to what
  the `Internal` provider needs to call reflection functions, and is off by default.
- **Non-UE engines.**
- **Overlaying the game's own rendering.** The injected payload opens its own window
  rather than hooking the swap chain's `Present`. A Present hook means writing a
  trampoline over code the game owns, which is the rule above; a separate window is the
  same UI, works whatever the game renders with, and touches nothing.

## Success criteria

1. Point it at any UE4.20–UE5.5 game and get a complete `dump.json` with no manual
   offset input.
2. The generated C++ SDK compiles clean with zero `static_assert` failures.
3. `zircon diff old.json new.json` after a game patch tells you exactly what broke.
4. A `UFunction` with script bytecode comes back as readable pseudo-code.
5. Every one of the above works identically from a live process and from a dump file.
