# Engineering log

What broke, and what it turned out to mean. Kept because on a tool like this the failures
are worth more than the successes: almost every one of them was a case of something
scoring perfectly against a test that was too weak, which is the failure mode this whole
project is built to avoid.

Ordered roughly by where it sits in the pipeline rather than by when it happened.

---

## Memory, providers and the PE reader

Against the same live process, `--pid` and `--dump` agree on every
module name, base and size. `--file` reports the preferred base rather than the ASLR'd
runtime base, which is correct for an image the loader never mapped.

Verified against Funnel Runners (5.6), FF7 Rebirth (4.26), Atomic Heart (4.27),
Backrooms Escape Together (5.7), Subnautica 2 and Deep Rock Galactic.

Two defects the tests caught, both worth recording:

- `FromBytesAndMask` originally took a `std::string_view` for the byte string, which
  silently truncates `"\x48\x8B\x00\x00"` at the first NUL. Real signatures contain
  zero bytes. The mask length is now authoritative and the bytes come in as a pointer.
- `ReadRva` could not read the PE headers, because they sit below the first section and
  section lookup missed them. The loader maps headers at file offset == RVA, and
  `ReadRva` does the same now.

`PeImage` still does not parse imports or relocations, because nothing has needed them.

---

## Finding GObjects and the name pool

Against Funnel Runners (UE 5.6), with zero manual input:

```
GObjects         0x7ff7c67c70c0   (72562 objects, 2 chunks of 65536)
FNamePool        0x7ff7c66e3590
UObject layout   index +0xc  class +0x10  name +0x18  outer +0x20
confidence       array 96%  pool 95%  layout 95%

Class /Script/Engine.Actor
Class /Script/StormEscape.StormSaveGame
Function /Script/CoreUObject.Object.ExecuteUbergraph
Package /Script/StormEscape
```

72346 of 72562 objects resolve to names, and results are identical from the live
process and from a 6.5 GB minidump of it.

### How discovery works, and why it should survive forks

Nothing here keys off a version number. Each fact is derived from a property that is
structural to the engine rather than incidental to a build:

- **The object array** is found because the object in slot *i* stores *i* in its own
  `InternalIndex`. Checking two dozen spread-out slots both proves the candidate and
  derives the offset, so validation and derivation are one pass.
- **`ClassPrivate` versus `OuterPrivate`** is settled by following each link repeatedly.
  Class converges to a fixed point, because the class of `UClass` is `UClass`. Outer
  terminates at null, because the outermost object is a package.
- **The name pool** is anchored on `"None"`, always the first name interned. The entry
  format — where characters start, how the length is packed — is then derived from that
  known entry rather than assumed.
- **The end-to-end check** is that the class fixed point must be named exactly `"Class"`.
  That one assertion exercises the array, the pool, the name offset and the class offset
  together, and confidence stays below 50% until it passes.

### Defect worth recording

The first working version selected a name offset of `+0x4` — inside the vtable pointer —
and reported **100% confidence** while doing it. The upper 32 bits of a vtable pointer
are near-constant across objects in one module, so reading them as an FName id resolved
to the same valid name for nearly every sample, scoring as well as the real field.

Resolvability is therefore not sufficient to identify a name field. Two things were
added: **diversity** (a real name field yields many distinct names; a coincidence
repeats one), and the `"Class"` fixed-point check as a hard filter. Confidence scoring
was also rebalanced so that failing the end-to-end check cannot produce a high number —
a tool whose premise is not guessing must not report certainty it has not earned.

---

## Walking reflection into the IR

      MinAlignment
      the `can_call` path and so belonged with the injected payload. It does not: P6's
      `ValueReader` reads a CDO's values externally. Done under `--defaults`.

Against Funnel Runners (UE 5.6):

```
$ zircon dump --process StormEscape-Win64-Shipping.exe -o storm.json
packages       232
classes        4902
structs        5574
enums          2026
properties     38879
functions      14632          33.4 MB, 1.8 seconds

$ zircon validate storm.json
round-trip     lossless
```

Re-running against the same target is byte-identical apart from `created_utc`, both
against a live process and against a frozen dump file. A live run and a dump run of the
same game differ only where the game itself has created or destroyed objects since the
dump was captured.

### What P2 had to unlearn

Every derivation in this phase initially selected a *plausible* field rather than the
right one, and each failure had the same shape: a candidate that scores perfectly on a
weak constraint.

| Field | What was selected instead | Why it scored well |
|---|---|---|
| `PropertiesSize` | high half of `FStructBaseChain`'s heap pointer | constant across objects, so perfectly "stable" |
| `MinAlignment` | inheritance depth counter | small integers look like alignments |
| `ChildProperties` | `PropertyLink` | non-null for *more* classes, since it includes inherited properties |
| `Offset_Internal` | a field that is always zero | trivially satisfies `0 <= v < size` for every property |
| subclass slots | one shared `sizeof(FProperty)` | true for object and struct properties, false for `FArrayProperty::Inner` |

The rule the phase settled on: **a derivation needs a positive distinguishing property
plus an independent anchor with a known-correct value.** Internal consistency alone was
wrong every single time it was the only test. In practice that means:

- `PropertiesSize` is anchored on `/Script/CoreUObject.Object` reporting the size of a
  UObject, cross-checked against no class being smaller than the root. It used to demand
  *exactly* the size the object layout implies, which 0.5.0 had to relax: a fork is free to
  append to UObject, and then that figure is wrong while the field is fine.
- `Offset_Internal` must make a struct's own properties *tile* it, not merely fit in it.
- Subclass slots are cross-checked three ways at once: `ObjectProperty` must target a
  `Class`, `StructProperty` a `ScriptStruct`, `ArrayProperty` an `FField` — three
  different target shapes, so agreement is evidence rather than coincidence.
- `MinAlignment`'s storage width is derived, because this build stores it narrower than
  `int32` and reads as 65544 otherwise.

---

## Emitting an SDK

Eight emitters at the time, all pure functions of the IR and therefore testable without
a game. Three more landed in 0.3.0 - see the bottom of this file.

| Format | Output | Status |
|---|---|---|
| `cpp_sdk` | per-package headers + `Basic.hpp` + `SDK.hpp` | parity target, **compiles clean** |
| `usmap` | UE4SS / FModel binary mappings | v0 format, header verified |
| `ida` | IDA Python importing structs + function names | 151k lines, parses |
| `ghidra` | Ghidra Jython equivalent | 96k lines, parses |
| `binja` | Binary Ninja type import (0.3.0) | data table + interpreter |
| `reclass` | ReClass.NET `.rcnet` archive + loose XML | both written |
| `docs` | browsable Markdown API reference | 249 files |
| `graphs` | DOT + Mermaid inheritance graphs | per package + overview |
| `json` | re-emit the IR | trivial, works on partial dumps |

Against Funnel Runners (UE 5.6):

```
$ zircon emit cpp_sdk storm.json -o out
234 files written

$ cl /std:c++20 /c SDK.hpp
0 errors      46225 static_asserts, all passing
```

233 headers, 160k lines, compiled in 3.8 seconds. Every member carries a `static_assert`
on its offset, so the SDK compiling *is* the proof that P1 and P2 derived the layout
correctly — a wrong offset anywhere upstream is a compiler error here rather than a
silent misread at runtime.

Deferred at the time: `frida_js` and `python_stubs`. Both landed in 0.3.0, and one of
them did teach something - see below.

### What compiling 46k assertions found

The SDK went from 201 errors to 0 in six steps, and every one was a real defect rather
than a cosmetic fix:

| Errors | Cause |
|---|---|
| 201 | package headers included only `Basic.hpp`, so a base class in another package was undefined |
| 102 | `Enum::underlying` was never populated, so every enum emitted as `uint8`; a 4-byte enum shifted every member after it |
| 17 | `Basic.hpp` hardcoded container sizes — `FText` is 16 bytes in this build, not 24 |
| 4 | `FEnumProperty::UnderlyingProp` resolved to a generic link pointer, making a property's *successor* its underlying type |
| 2 | a struct with a property literally named `uint8`, shadowing the SDK's own typedef |
| 0 | `alignas` rounded `sizeof` up on a 142-byte struct declared at alignment 4 |

Two of those were engine-layer bugs that only an emitter could expose, which is the
argument for the milestone being "it compiles" rather than "it produced files".

The general principle the phase settled on: **a rendering whose C++ size differs from the
size the engine reports is degraded to an opaque byte array.** Losing a type name is
visible in the output; a member at the wrong offset is not, and corrupts everything after
it. Container sizes are now measured from the dump rather than hardcoded, since every
property using one reports its element size.

### Known gaps in the IR, surfaced by the emitters

Recorded here when P3 was written, and all closed afterwards — see "Closing the gap list"
at the end of this document for what each turned out to be.

- ~~`Function::vtable_index` is modelled but never populated.~~ Not derivable; removed,
  and replaced with `Struct::vtable_rva`, which is.
- ~~`Struct::interfaces` is modelled but the walker does not fill it.~~ Derived.
- ~~`Property::flag_names` is empty, so the docs Flags column shows a raw hex word.~~ 41
  names, no unnamed bits left.
- ~~`util::CppPrefixFor` silently demotes every actor to `U` under a `--filter`.~~ Fixed by
  recording the prefix in the walker, where the whole chain is visible.

---

## Diffing two builds

```
$ zircon diff yesterday.json live.json
10476 of 10476 types present in both are byte-identical (100.0%)
no differences                                        # live process vs a 6.5 GB minidump

$ zircon diff before.json after.json --breaking
critical   154
high       322
/Script/Engine.Actor
  critical  property_moved  NetPriority  0x190 -> 0x198   (moved +8 bytes)
```

### Severity is "what breaks", not "what changed"

A game update adds hundreds of classes and moves a handful of offsets. Sorting by
novelty buries the second under the first, so changes are graded by their effect on code
that already exists:

| | |
|---|---|
| **critical** | reads through this now return the wrong bytes, silently — moved, resized, retyped, bit moved, removed |
| **high** | still resolvable, but broken until updated — type resized, function relocated, signature or enum value changed |
| **medium** | renames, engine version, a core layout offset moving |
| **low** | flag changes |
| **info** | additions; new surface cannot break what already worked |

Two consequences worth stating. Additions are suppressible in one flag (`--breaking`),
because they dominate the raw count and never matter. And a bitfield moving *within* its
byte is critical despite the offset being unchanged — an offset-only comparison misses
it, and reading the old mask then returns a different flag that looks perfectly valid.

### Rename detection

A property that disappears while another appears at the same offset with the same type
is a rename, and reporting it as removed+added claims a hardcoded offset died when it did
not. Both conditions are required: a same-offset replacement of a *different* type is not
a rename, it is the case that most needs to stay loud.

Validated on real data. In a simulated patch that renamed four properties, three were
detected; the fourth had also moved, so the heuristic correctly refused to call it a
rename and reported it as removed plus added.

### The diff found a bug in the dumper

Its first run against real data — a live process versus a minidump of the same process —
reported all 14632 functions as relocated, every one from the same source address. That
was not a game change; it was Zircon deriving `UFunction::Func` at `+0xE0` from the dump
and `+0xD8` from the live process.

Two defects behind it, both now fixed:

- **Ranking `Func` by "points into executable memory" is not selective enough.** A
  UFunction holds more than one code pointer, and a slot holding the same shared thunk
  for every function satisfies that test for 100% of samples, beating the real field
  where a few entries are null. Selection now ranks by *distinct* targets: what
  characterises `Func` is pointing somewhere different per function.

- **Page protection is not a usable signal across sources.** A minidump captured without
  `MiniDumpWithFullMemoryInfo` records no protection, so the Dump provider reports every
  region as executable (a deliberate P0 choice, since claiming read-only would be worse)
  and the test became vacuously true. Executability is now read from the loaded modules'
  PE section headers, which answer the same question identically for a live process, a
  dump and a static image.

After both fixes, the same target derives the same layout from either source and the
diff is clean. This is the argument for building the diff at all: it is the only part of
the tool that can catch the tool disagreeing with itself.

---

## Kismet bytecode

Against Funnel Runners (UE 5.6):

```
2871 functions carry bytecode (1156611 bytes); 2871 decoded fully (100.0%)

/Game/.../PC_Lobby_BP_C.ExecuteUbergraph_PC_Lobby_BP   // 473 bytes of bytecode
  000A  FlushPlayerInput(this);
  0015  CallFunc_GetLocalPlayerSubsystem_ReturnValue =
            GetLocalPlayerSubsystem(this, /Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem);
  0058  if (!(K2Node_CustomEvent_bEnable)) goto Label_0103;
  0084  Temp_struct_Variable_1 = ModifyContextOptions{true, false, false};
  00C2  CallFunc_....AddMappingContext(/Game/.../IMC_LockerView, 0, Temp_struct_Variable_1);
```

Control flow is rendered as labelled `goto` rather than reconstructed into `if`/`while`.
That is deliberate: goto form is correct for any control-flow graph, and inventing
structure that does not match the real graph would be the same failure this project has
spent five phases avoiding. Structuring is a later refinement, not a correctness fix.

### An opcode that is not in the documentation

Four functions stopped on opcode `0x11`, which the published `EExprToken` lists as unused.
It appeared only inside `EX_StructConst`, exactly where a packed bool belongs, and
`FModifyContextOptions` - one of the affected types - is three `uint8 : 1` members.

Decoding it with `EX_BitFieldConst` operands (an `FProperty*` then a byte) took the corpus
from 99.9% to **100.0%**, and the affected literals render as
`ModifyContextOptions{true, false, false}`: three bools where three bools belong. Two
independent confirmations - a wrong operand length would have relocated the desync rather
than removed it, and a wrong interpretation would not have produced the right number of
values of the right kind. Recorded as established by observation, not assumed.

### The decompiler found a much larger bug

Its first run over a full dump produced nothing, because the dump contained no Blueprint
functions. It contained no Blueprint *classes* either.

`BuildDump` classified objects by comparing their class name to `"Class"`. A Blueprint
class reports `BlueprintGeneratedClass`, a widget `WidgetBlueprintGeneratedClass`, an anim
graph `AnimBlueprintGeneratedClass` - none of which match. **Every Blueprint type in every
game had been silently missing since P2**, unnoticed because the dump looked complete and
the SDK compiled.

Objects are now classified by walking the meta-class chain, which is what they *are*
rather than what they are called. On the same target:

| | before | after |
|---|---:|---:|
| packages | 232 | **618** |
| classes | 4902 | **5252** |
| properties | 38879 | **48692** |
| functions | 14632 | **17698** |

Nearly ten thousand properties and three thousand functions were absent. The SDK still
compiles with all of them: 620 headers, 56396 static_asserts, zero errors.

Two consequences of including Blueprint types, both fixed:

- **`alignas` had to go.** A Blueprint class typically appends one bool to a native base,
  giving a size that is not a multiple of its alignment. `alignas` cannot be selectively
  dropped to fix that, because a type is at least as aligned as its base *and* its members
  - `alignas(1)` lowers nothing. Under `#pragma pack(1)` every generated type is already
  1-aligned and `sizeof` is exactly the bytes emitted, so the declaration no longer
  restates alignment; the comment on each type carries it instead.

- **An unresolved container element produced `TArray<>`**, which does not compile, losing a
  whole header over one element type. A container size does not depend on its element, so
  it now renders as `TArray<FUnresolved>`.

### Determinism, again

With Blueprint types included, two consecutive dumps of one live process disagreed on 4
types. The cause was 98 functions whose names do not resolve and 92 duplicate names within
a type: keyed by name alone they collapsed, and which one survived depended on hash
iteration order. Unnamed functions now carry a stable identity from their FName id, and
the diff matches by name *and* occurrence. Back to 10850 of 10850 types byte-identical.

---

## The live browser

Revised twice. The original plan was a TUI; that became Dear ImGui + D3D11. The second
revision is the one worth recording: **the in-game overlay was dropped deliberately.**

An overlay means hooking the game's swap chain `Present` and writing a trampoline over
code the game owns. `docs/SCOPE.md` rules that out ("Game modification. This tool reads.
Writing to a live process is limited to what the `Internal` provider needs to call
reflection functions"), and a Present hook is not something the Internal provider needs.
So the injected payload opens a window of its own instead. That costs nothing — it is the
same `Browser`, the same `RunLoop` — and it works whether the game renders with D3D11,
D3D12 or Vulkan, which a D3D11 Present hook would not.

`src/gui/Browser.cpp` is host-agnostic: it draws into whatever ImGui context is current
and knows nothing about windows or devices. `src/gui/Host.cpp` supplies the window, the
device and the frame loop. Two shells call into it:

| Shell | Entry | Target |
|---|---|---|
| `zircon-gui.exe` | `RunBrowserWindow(pid)` | External provider, attaches by pid |
| `zircon.dll` | `RunBrowserWindow(memory, reflection)` | Internal provider, already derived |

The payload hands over a target it has already opened and reflected, so the browser does
not repeat several seconds of derivation the payload just finished.

### What it shows

- Scored process picker (the same `DetectUnrealProcesses` the CLI uses), or `--attach <pid>`
- Every derived offset behind one tooltip, so the UI is as auditable as the dump
- 72k-object list, filtered live, drawn through `ImGuiListClipper`
- Property inspector: offset, resolved type, name, live value, grouped by declaring class
- Kismet pseudo-code for the selected function
- Values re-read on a timer, not per frame — a deep struct member costs many reads, and at
  60 fps that is thousands of syscalls a second for data no one can read that fast

### Defects found and fixed in this phase

**A class showed no properties at all.** The inspector read every selection as an
instance: it took `GetObjectClass(selection)` and walked that. For `/Script/Engine.PlayerController`
that is `UClass`, which declares almost no reflected properties, so the panel was empty.
A class is not an instance. The fix required a member nothing had derived yet.

**`UClass::ClassDefaultObject`, derived** (`src/engine/src/ClassLayout.cpp`). The CDO is
the one real instance of a class the engine keeps, and it is where every default value
lives. It is identified by a property no other pointer in `UClass` has: *the object it
points at reports this very class as its class*. `SuperStruct` points at a different
class, `ClassWithin` at `UObject`, the linked lists at properties and functions — none of
them close that loop.

That loop alone would have been the recurring bug a seventh time: it is pure internal
consistency. So it is paired with two facts that have known-correct values — the target
must be a live entry of the object array (its slot index round-trips), and the engine
names every CDO `Default__<ClassName>`. Measured on the UE 5.6 target: `+0x110`, 300 of
300 sampled classes closing the loop, 300 of 300 named `Default__*`.

The proof that it is right is not the score, it is the output. Reading
`/Script/Engine.PlayerController` through it gives `InputYawScale = 2.5`,
`InputPitchScale = -2.5`, `InputRollScale = 1`, `bAutoManageActiveCameraTarget = true`,
`CheatClass = /Script/Engine.CheatManager` — the documented UE defaults, none of which
Zircon has any table of.

**Enum values printed as numbers.** `DefaultMouseCursor = 1` rather than `Default`.
`ValueReader` had the enum's *path* from `ResolveType` and was looking the object back up
with `FindObjectByPath` — a linear scan of all 72562 objects, per property read, which
also silently failed whenever the path did not match exactly. But `ResolveType` had the
enum's address in hand and threw it away. `ResolvedType` now carries `referenced_object`
alongside `referenced`, and the reader follows the pointer. Same fix for struct members,
which were doing the same scan. `zircon read` on `PlayerController` went from 1 in 4
enums resolved to all of them, and from minutes to 1.5s.

**`GetClassName` is a `<windows.h>` macro.** It expands to `GetClassNameW`, so including
`<windows.h>` in a file that calls `engine::GetClassName` silently rewrites the call into
a window-manager call. Caught by the compiler here; worth remembering because the failure
mode when it *does* compile is invisible.

### Also landed

- `zircon inject --pid <n>` (`src/core/src/Injector.cpp`) — `VirtualAllocEx` +
  `WriteProcessMemory` + `CreateRemoteThread(LoadLibraryW)`. Deliberately the plainest
  possible loader; manual mapping or unlinking the module is the detection bypass the
  scope rules out. It refuses outright when the target has anti-cheat loaded, because
  every Zircon feature except calling game functions works externally.
- The payload does real work. It was still printing "reflection dumping lands in P2";
  it now runs `Reflect`, `BuildDump` with script, and the `cpp_sdk`, `usmap` and `json`
  emitters into `zircon-out/` next to the DLL, then opens the browser.
- DPI scaling. A table of 48692 offsets at 96-DPI metrics on a 4K panel is unreadable.

### The test that bites

`TestClassDefaultObject` builds a synthetic object world — a meta-class, 64 classes, each
with a CDO and one ordinary instance — at offsets that match no real engine build, so a
derivation cannot pass by having memorised UE 5.6's numbers.

The decoy in it is the important part. It sits at a *lower* offset than the real CDO slot
and points at an ordinary instance of the class. That instance is live, and it names this
very class as its class, so it closes exactly the same loop the CDO does, for all 64
classes, perfectly. Only the `Default__` name separates them.

Verified to bite: deleting the name check from `ClassLayout.cpp` makes the derivation pick
the decoy and five assertions fail. With it, 149 checks pass.

**Milestone:** attached to a live UE 5.6 game, indexed 72346 objects, selected
`/Script/Engine.PlayerController`, and read its defaults — correct values, resolved enum
names, masked bitfields, resolved object paths — in a standalone window and from inside
the game process.

---

## Shells

Three products over one core, because the `IMemorySource` split makes each shell thin.

| Shell | Target | Status |
|---|---|---|
| `zircon.exe` | CLI | done — the engine; the others are veneer |
| `zircon.dll` | injectable payload | done — dumps, emits, and opens the browser in-process |
| `zircon-gui.exe` | GUI | done — Dear ImGui browser over the External provider |

Injection is a capability upgrade, not the normal path: it is only required for
`can_call` (CDO construction, `StaticFindObject`, invoking `UFunction`s). Walking
`GObjects`, reading names and emitting a full SDK all work externally, with no code in
the game's address space.

`zircon inject --pid X` loads the payload. It refuses when the target has anti-cheat
loaded; see P6.

---

## The plugin ABI

A plugin is a DLL exporting `zircon_plugin_main`. It can add output formats, and it can
teach Zircon how to read a target that deriving alone will not reach. `docs/PLUGINS.md` is
the authoring guide; `include/zircon/plugin.h` is the whole interface.

Loading is opt-in — `--plugins <dir>`, or `ZIRCON_PLUGINS`. A dumper that silently ran
whatever DLL was sitting next to it would be a poor tool to hand someone.

### Fields are addressed by name, and that is the whole design

There are no per-field getters. Every IR node is a `ZnNode` and every field is reached by
name:

```c
zn->str(record, "name", &text, &len);
zn->len(record, "properties", &count);
zn->at (record, "properties", i, &property);
```

With typed getters — `zn_struct_size`, `zn_property_offset` — the ABI freezes around the
schema: every new IR field is a new export, and the IR is this project's contract and is
going to grow. By name, adding a field is not an ABI change at all. Old plugins never ask
for it; a new plugin asking an old host gets `ZN_ERR_NO_FIELD`, which is a defined answer
rather than undefined behaviour.

It also makes nodes introspectable (`field_count` / `field_name` / `field_type`), which is
what makes a scripting binding possible at all: the Lua host exposes nodes as ordinary
tables without knowing a single field name.

The cost is a string compare per access. For a boundary crossed by scripts, that is the
right trade — 48 692 properties through it in 1.6 seconds.

### Lua, as an ordinary plugin

Lua 5.4 is vendored, and the Lua host is **not** built into the tool. It is a plugin like
any other, using nothing but the public `ZnApi`. That is deliberate: it means the ABI is
exercised end to end by something real rather than by a test that knows where the bodies
are buried. If a field were missing from the node tables, or the path check in
`write_file` were wrong, the Lua host would stop working.

A script returns `{name, description, emit}` and gets dropped next to the DLL. `dump` is a
live view over the host's IR, not a copy — indexing calls back through the ABI — so opening
a 46 MB dump is free and a script pays only for what it reads.

`plugins/padding.lua` is the example worth reading. It reports bytes in every class that no
reflected property accounts for, which no built-in emitter does, in eighty lines of script.
On the UE 5.6 target: 828 classes with 82 685 unexplained bytes, with a further 155 837
bytes correctly identified as alignment and left out.

### The two engine hooks, and why neither is trusted

`ZN_ABI_MINOR` 1 added a name-entry decoder (for encrypted `FName` pools) and a global
resolver (for a `GObjects` that cannot be scanned for). Both are narrow, and both are
*hints*:

- A resolver's `GObjects` goes through the identical `evaluate()` the scan uses — the
  object in slot *i* must store *i* in its `InternalIndex`. Its `FNamePool` must still have
  the `None` entry at the start of block 0.
- A decoder's output goes through the identical parse — the length bound, the printable
  test. A broken decoder can fail to produce a name. It cannot put nonsense into a dump.

A hook says *where* to look. It is never believed about what is there. That is the only
shape an extension point can take in a tool whose entire value is that its output is
correct, and it is the same rule the six derivation bugs taught: a plausible input is not
a correct one until something independent confirms it.

Wiring the resolver required pulling the object-array scan's loop body into an `evaluate`
lambda so the hint and the scan share one code path. Verified neutral: same addresses, and
a dump byte-identical to the one before the refactor across all 10 850 types.

### Tests

`tests/test_plugin.cpp` — 280 checks. The one that matters is the round-trip: it builds a
`Dump` in which no field holds its default, reads the whole thing back through nothing but
`ZnApi` calls, rebuilds a `Dump` from what came out, and asserts the two are equal.

That is a real completeness check, not a spot check. A table of getters is exactly the kind
of thing that silently falls behind the type it describes; this is what stops it. Verified
to bite: deleting `Property.bit_index` from the node table makes four reads fail with
`ZN_ERR_NO_FIELD` and the equality assertion fail with it.

The rest cover the error contract (unknown field, wrong type, out of range, null — four
distinct codes, because a typo and a misunderstanding are different mistakes), and the
write sandbox: `../escaped.txt`, `nested/../../escaped.txt`, `C:/Windows/...`, `/etc/passwd`
and `""` are all refused, and nothing refused is written or counted.

`tests/test_core.cpp` gained the hook tests: a synthetic XOR-encrypted name pool that
resolves to nothing without a decoder and exactly right with one; a decoder returning
plausible rubbish that still cannot get it into a name; and a resolver whose wrong address
is *rejected* rather than believed. Verified to bite: removing the validation from the
resolver path makes that last assertion fail.

### Verified

Both examples in `docs/PLUGINS.md` were run verbatim rather than written from memory. The
C one compiles against nothing but `include/zircon/plugin.h`, `/W4` clean, loads, and
reports 618 packages. The guardrails were checked end to end too: a script claiming the
name `cpp_sdk` is refused and the built-in still answers, and a script writing to
`../../../escaped.txt` is refused with no file anywhere.

**Milestone:** a third-party emitter written entirely in script produces valid output —
48 692 CSV rows from `plugins/csv.lua` in 1.6 s, with no part of Zircon rebuilt.

---

## Closing out the known gaps

With P0–P7 finished, what remained was the list of things the emitters had been asking for
and the walker had never supplied. Measured against a real dump first, because three of the
four were believed to be harder than they were:

```
cdo_rva non-zero        0
vtable_index populated  0
interfaces non-empty    0
property flag_names     0
```

### Defaults were never blocked

Reading default values was parked early on, with the reasoning that it needed the
`can_call` path and would therefore have to wait for the injectable payload. That was
simply wrong, and building the browser is what exposed it: `ValueReader` reads a class
default object's values **externally**, with no calling at all. The work had been sitting
behind a constraint that did not exist.

`--defaults` now walks every class's default object and records each property's value as a
rendered string. On the UE 5.6 target: **27 065 of 27 065 class properties**, in 2 seconds.
Struct properties get none, correctly — a struct has no default object.

A rendered string rather than a typed value on purpose. A typed one would need the IR to
model every container and struct shape a default can take, and what a reader wants from a
default is to read it.

It reached the emitters too: 12 255 defaults in the generated SDK's comments
(`float InputYawScale; // 0x0540(0x0004) = 2.5`), a Default column in the docs, and a new
`PropertyDefaultChanged` in the diff — a patch that moves nothing and changes how the game
behaves used to produce an empty report.

**A defect this surfaced.** `Reflection::Context()` never wired up `enum_layout`, while the
CLI's own session did. So an enum default read as `3` through the dump path and
`ECC_Visibility` through the live path — the same property resolving differently depending
on which entry point asked. Exactly the class of disagreement the diff tool exists to
catch, found here by reading the output instead.

### Two fields that could never be filled

**`Struct::cdo_rva`** — removed. A CDO is heap-allocated by the class constructor at
runtime, so it has no module-relative address, and an absolute one would be a stale heap
pointer the moment a dump is saved. It was structurally always zero, and an always-zero
field is a trap for whoever tries to use it next.

**`Function::vtable_index`** — removed. Unreal's reflection data does not record one:
`UFunction::Func` points at the generated exec thunk, not at the virtual method, and
nothing in the object graph names a virtual slot. Recovering it would mean binary analysis
with nothing independent to confirm the answer against.

Replaced with something that *is* derivable and is usually what the question was really
asking: **`Struct::vtable_rva`**, read from the first pointer of the CDO. Unlike the CDO's
own address this one is stable — a vtable lives in the image's read-only data — and it is
accepted only when it lands inside the main module. 5 240 of 5 252 classes, 3 755 distinct
tables, with Blueprint classes correctly sharing their native parent's.

Removing both from the plugin node tables needed no ABI bump, which is the name-addressed
design paying for itself: a plugin asking for a field the host no longer has gets
`ZN_ERR_NO_FIELD`, the defined answer, rather than a broken vtable slot.

### `Property::flag_names`

41 names, from the raw `EPropertyFlags` word. This is a table, which everywhere else in
this project would be the wrong answer — but there is nothing to derive: no amount of
looking at a running game recovers the word "BlueprintReadOnly" from bit 4. The numeric
flags stay in the IR verbatim, so a build whose meanings differ loses nothing.

Anything unrecognised comes back as `Unknown(0x...)` rather than being dropped, and that
immediately earned its keep: bit 56 was set on 2 980 properties and missing from the first
table. It is UE5's `CPF_TObjectPtr`, and every property carrying it was an object pointer.
Now named; zero unnamed bits remain.

### `Struct::interfaces`

`UClass::Interfaces` derived at `+0x1d8`, stride 16. What makes it findable is not its
shape — `UClass` holds several `TArray`s — but what its elements point at: every element's
first pointer is a class that inherits `/Script/CoreUObject.Interface`. `ClassReps`, the
other pointer-plus-int32 array in `UClass`, holds `FProperty` pointers, which are not
array objects at all, so the two do not compete. The anchor with a known-correct value is
the `Interface` class itself, found by name through the pool.

One class whose array does not hold interfaces disqualifies an offset outright. A field
that is right for most classes and wrong for one is not the field; it is a coincidence with
a bad case.

400 classes, 208 distinct interfaces, and the spot-checks are exactly right against real
UE: `UStaticMesh` lists `Interface_CollisionDataProvider`, `Interface_AssetUserData` and
`Interface_AsyncCompilation` — all three, no more; `UActorComponent` lists
`Interface_AssetUserData`; `USkinnedMeshComponent` lists `LODSyncInterface`; `AActor`
lists none.

### The filtered-SDK bug

`util::CppPrefixFor` inferred `A` and `I` by walking the super chain to
`/Script/Engine.Actor`. A `--filter` that excluded that package removed the ancestors to
walk, so **every actor silently became `U`** — a wrong answer that looked like a normal one.

Fixed where the data actually is: the walker records `Struct::cpp_prefix`, because it sees
the live chain regardless of what the filter keeps. Verified on a `--filter StormEscape`
dump that does not contain `/Script/Engine.Actor` at all: 174 classes correctly `A`, 14
correctly `I`, where previously all 655 would have been `U`. The old walk is kept as a
fallback for dumps that predate the field, and produces an identical census on an
unfiltered dump.

### A limit of the round-trip test, found the hard way

Adding `Property::default_value` to the IR did not fail the plugin round-trip test, even
though the node tables had not been updated — because the test's *reconstruction* did not
read the new field either. The test catches a field the tables lose, but only among the
fields it reads, so it is exactly as complete as its reader is. Both were then updated, and
removing the entry from the table now fails six assertions.

Recorded because the P7 entry above calls that test "a real completeness check", and it is
one only if adding a field to the IR means adding it in three places: `Model.h`, the node
table, and the test's reader. Nothing enforces that automatically; C++ cannot reflect over
its own members, which is the reason the tables exist at all.

### Verified

All six suites green, 1 424 checks. Two regressions injected and confirmed to bite:
reporting a default change when only one side captured defaults fails two assertions, and
the earlier hook and CDO tests still fail when their anchors are removed. SDK regenerated:
619 headers, 56 396 `static_assert`s, **0 errors**, and every one of the eight emitters runs
on a dump carrying defaults.

---

## Things that held throughout

- **Tests.** Emitters get golden-file tests against checked-in IR fixtures. Core gets
  unit tests. The plugin ABI gets a round-trip test that fails if the node tables fall
  behind the IR. The engine layer gets tests against small checked-in dump fixtures so CI
  never needs a game installed. Run with `ctest --test-dir build -C Release`.
- **No silent guessing.** Anything derived carries a confidence value and is reported.
- **Partial results beat failures.** An unknown property type becomes `unknown`, not a
  crash. A missing global degrades the dump, it does not abort it.

---

## 0.3.0 — the three deferred emitters

### `binja`, and why it looks nothing like `ida` or `ghidra`

Those two write a type out in the host's own language: a C declaration for IDA, a
`DataType` call for Ghidra. Doing the same for Binary Ninja means a `StructureBuilder`
sequence per type, and on a 5 000-class dump that is roughly 40 MB of Python that its
interpreter has to parse before anything happens. It is also the same six lines copied
fifty thousand times, which is where a mistake gets to hide.

So it writes a table and one loop. Every member is a five-tuple `(offset, name, kind, arg,
count)` with a one-character kind, and forty lines at the bottom of the script turn that
into types. The build logic exists once, where it can be read.

The other thing that fell out of it: no topological sort. The script reserves every type
name at its final width first and fills members in afterwards, so a named reference always
resolves. UE dumps contain reference cycles, and the two C-emitting backends have to warn
about them because a C declaration cannot be written out of order. Here they cost nothing.

### The Frida blob is JSON, not a JS object

Emitting the type table as a JavaScript object literal is the obvious thing and it is
slow: V8 has a dedicated JSON parser and the object-literal path goes through the full
parser. So the data is a string that goes through `JSON.parse`.

That string then has to sit inside a JS literal, and the first version put raw newlines in
it — which is a syntax error, not a warning. Escaping them to `\n` would have produced a
single 30 MB line that no editor opens. The literal closes and reopens instead:

```js
var DATA = JSON.parse('{"meta":...' +
'"/Script/Engine.Actor":{...}' +
...);
```

One type per source line, one string to the parser.

`Zircon.base` also had to become lazy. The first version resolved it with
`Module.findBaseAddress` at load time, which made the file unloadable outside Frida —
including from node, which is where the accessors were then tested. Everything above that
line is plain data, and querying an offset from a build script is a reasonable thing to
want.

**Verified against a fake buffer.** The generated runtime was loaded in node over a
`Buffer` with a NativePointer stand-in: fifteen checks, covering the bit-preserving write
(set one bool, the other six in the byte survive), enum round-trip by name, a nested
struct's members landing at the outer offset plus the inner one, an inherited property read
through a derived class, and `FString` refusing to be written. That is the kind of check
the C++ suite cannot do, because it cannot run JavaScript.

### Two guards, neither test able to fail

`None` is an enumerator in `EGizmoElements` and a keyword in Python, so the stub emitter
renames it `None_`. The check existed twice: once in `PyIdentifier`, once again after the
`EFoo::Bar` leaf was stripped in the enum loop.

Removing either one left the other standing, so the test passed both times it should have
failed. The stripping now happens *before* sanitising rather than after, which leaves one
guard, and removing it fails two assertions.

Recorded because it is the same shape as the round-trip limitation noted earlier in this
file: a test is exactly as complete as the thing it exercises, and redundant code makes it
quietly less complete than it looks.

### What the linter caught the first time it met a real game

`validate --strict` was built against fixtures and ten hand-broken dumps. The first shipped
game it saw came back with 176 errors, and both causes were real.

**The name check, again.** `TypeResolver` refuses a referenced object unless it is the kind
expected, which is right, and it was testing that with a string compare against the class
name. A Blueprint class's class is `BlueprintGeneratedClass`; a Blueprint struct's is
`UserDefinedStruct`. Neither equals `"Class"` or `"ScriptStruct"`, so 489 object properties
and 43 struct properties came back with no type at all.

`ClassifyObject` exists precisely for this and `StructLayout.h` carries a comment saying
so, including that the mistake had already been made once and gone unnoticed until the
bytecode decompiler found functions the dump did not contain. It was still being made one
file away. The lesson is not "use ClassifyObject" — that was already written down. It is
that writing the lesson down does not find the places still doing it, and a check that
reads the finished artefact does.

Measured as a strict gain, not a change: 532 references recovered, 0 offsets moved, 0 sizes
changed, 0 properties renamed to anything other than from empty.

**The enum width, half-fixed.** 0.2.0 found that `Enum::underlying` was `uint8` for enums
whose values need more, and fixed it in the emitters — the SDK widens at emit time and
compiles. The IR kept saying `uint8`. That is invisible while the built-in emitters are the only
consumers, and wrong for a plugin, a script, or the two new emitters in this release.

The fix belongs where the data is: `DumpBuilder` widens `underlying` to whatever the values
need, because the values come straight from `UEnum::Names` and the width was a fallback.
22 enums on that game, 0 enum values changed.

Both of these had been reproduced identically by all three memory providers for months. L5
— external, injected and minidump agreeing byte for byte — cannot see a defect they all
share, and that is the argument for L6 being a separate level rather than a nicer way of
saying the same thing.

### The SDK's "zero warnings" had quietly stopped being true

Every type in the SDK is `struct`. Every elaborated specifier in a function signature said
`class`. MSVC raises C4099 per occurrence: 27,334 on a UE 5.6 game.

Never an error, never a layout problem, and the compile test had been checking for errors.
"0 errors, 0 warnings" went in the README when both were true and only the first half was
being re-measured afterwards. Fixed in the seven places that render a reference plus one
hand-written `class UClass*` in `Basic.hpp`; `/W3` is clean again.

Worth keeping because of what it says about the test: a claim is only maintained if
something re-checks the whole of it, and a compile test that greps for `error` will watch
a warning count go from 0 to 27,334 without comment.

## 0.4.0 — a cache with no way to say "that's old now"

### The bug the fixtures could never have caught

Reported from outside: a 0.3.0 GUI dump of Funnel Runners where 36 of 12,856 types came out
with a package belonging to an unrelated asset —
`BP_SqWaterTower_Destr_C` in
`/Game/RuralGasStation/Textures/T_CoffeeMachine_OcclusionRoughnessMetallic`. A Blueprint
class cannot live inside a texture, so the outer pointer was wrong, not the name lookup.

The CLI didn't reproduce it. Neither did `--names`, which the report mentioned and which was
the obvious variable to isolate first. 12,876 types, 0 mismatches. Ran the GUI with the same
boxes ticked and got 0 as well.

What made it appear was doing what a person does with a live browser: attach, look at things
for a while, keep the window open, dump later. Specifically — attach, filter to
`BP_SqWaterTower_Destr` and click through sixteen of them, load into a match, then dump.

```
_C classes with a foreign package       14   (8 of them not even a valid name)
properties with no name                 31
array_dim in the hundreds of millions    3
```

Same build, same game, same clicks, with the fix: 0, 0, 0.

### What it actually was

`CachedMemorySource` — 64 MiB, direct-mapped, 4 KiB pages. Written for one reason: External
mode issues millions of small reads and every one is a syscall, which puts an uncached full
walk about two orders of magnitude behind Internal.

Its entire invalidation story:

```cpp
bool Write(Address addr, const void* in, std::size_t size) override {
    const bool ok = inner_->Write(addr, in, size);
    if (ok) InvalidateRange(addr, size);   // and that is all of it
    return ok;
}
```

A page enters a slot and stays until something else hashes to that slot. There is no age, no
generation counter, and `IMemorySource` had no method a caller could use to ask for fresh
bytes even if it wanted to.

For the CLI that is not a bug, it's the right design. Open, derive, walk, exit — a few
seconds, during which the target barely moves.

The GUI builds one cache at attach and holds it until detach, which turns the same code into
two different defects:

**The live object browser was not live.** `RefreshRows` re-reads the selected object's
properties on a timer, so the refresh slider was choosing how often to re-read *the cache*.
Values did sometimes move, which is why nobody caught it — direct-mapped means a page gets
evicted whenever something conflicts for its slot, so the display was a mix of live values
and values frozen at whenever that page was first touched. Intermittently-correct is worse
than broken; broken gets reported.

**A dump taken later walked the graph through pages put there by browsing.** UE recycles
objects across a GC. An `Outer` pointer cached before a level change resolves, after it,
against whatever now owns that memory — which is exactly how a Blueprint class ends up
claiming a texture. 14 of 12,847 is 0.1%, which is small enough to look like noise and large
enough to be wrong.

### The one already in this file

The `--defaults` entry above records an enum default reading as `3` through the dump path and
`ECC_Visibility` through the live path, attributed to `Reflection::Context()` not wiring up
`enum_layout`. That wiring was genuinely missing and fixing it was correct. But two entry
points disagreeing about the same property, one of them long-lived and one of them not, is
this bug's signature, and it was sitting in the log unrecognised.

### The fix, and why it is not expensive

`IMemorySource::Invalidate()` — virtual, default no-op, so every provider except the cache
ignores it. The cache fills its tag array with the invalid tag and forwards down the chain.

The GUI calls it before a dump, before a reindex, and on every value refresh. Dropping 64 MiB
of cache four times a second reads worse than it runs: a selected object is a few hundred
properties, so a refresh is a few hundred page reads — which is what the uncached read would
have cost, and what the browser was supposed to be paying all along. The refresh slider
bounds it.

`test_core.cpp` pins the behaviour with a fake source that changes its bytes behind the
cache's back, the way a running target does. Removing the fix fails four checks.

### What is still wrong

The GUI takes `num_elements` from `Reflect()` at attach and never re-reads it, so its object
table is sized to the world as it was then. Objects created since are not in it, and Reindex
can't find them — it re-reads every slot it knows about, but it doesn't know about new ones.
Nothing reads *wrong*, there is just less of it than there should be. Re-reading the count
means storing where in `FUObjectArray` it was found, which `ObjectArrayInfo` currently
doesn't carry.

### The general point

Every correctness argument this project makes — L5 cross-provider agreement, L6
self-consistency — compares *outputs*. All three providers sit behind the same cache with
the same policy, so a defect in the caching layer is reproduced faithfully by all of them and
agreement stays perfect while the answer is wrong. Same shape as the enum-width bug in 0.3.0,
one layer lower.

And the condition needed a person: attach, browse, wait, change level, dump. No fixture has a
clock, and nothing in CI keeps a memory source alive long enough for the target to move
underneath it.

## 0.5.0 — the first target that defeated the derivation

### Two bugs, one assumption

Atomic Heart is a UE 4.27 build that appends to `UObject`. Every anchor in this project that
said "OuterPrivate is the last member of UObject" was wrong on it, and that phrasing appears
in two files.

**`StructLayout.cpp` refused to derive.** `PropertiesSize` is kept only if
`/Script/CoreUObject.Object` reports exactly `outer_offset + 8` there. Real figure 48,
expected 40, no candidate survived, `properties_size` stayed `-1`, and `MinAlignment` was
never attempted because it is derived inside the success branch. Two `-1`s, one cause.

**`FunctionLayout.cpp` derived the wrong thing and said nothing.** `UField::Next` is found by
walking `Children` and keeping the offset whose chains "stay in the object array and
terminate". A field that reads null everywhere terminates every chain immediately and scores
a perfect hundred percent. The scan started at the same stock `sizeof(UObject)` and took the
*first* offset that passed, so it settled on a field that links nothing. Every Children list
came out one entry long: 4,763 classes, 1,640 functions, `validate --strict` clean.

The second is the one worth sitting with. The first failed loudly and cost a dump. The second
produced a dump that lints clean, round-trips, diffs, and is missing 85% of its functions.

### What the build actually looks like

```
0x30  sizeof(UObject) = 48                       8 past where the members end
0x30  UField::Next
0x38  FStructBaseChain::StructBaseChainArray     UE5, in a 4.27 build
0x40  FStructBaseChain::NumStructBasesInChainMinusOne
0x48  SuperStruct   0x50 Children   0x58 ChildProperties
0x60  PropertiesSize = 48   0x64 MinAlignment = 8
```

Read out of the process rather than inferred. The depth counter at `+0x40` is 0 for `Object`,
1 for `Actor`, 2 for `Struct`, 3 for `Class` — `NumStructBasesInChainMinusOne` and nothing
else could produce that sequence.

### Why the obvious fix was wrong

The issue report proposed anchoring on `super_struct - 8`, reasoning that `UField` is
`UObject` plus one pointer so `offsetof(Next) == sizeof(UObject)`. The reasoning is sound;
the arithmetic assumes `SuperStruct` sits immediately after `Next`, and on this build the
base chain sits in between. `0x48 - 8` is 64; the answer is 48. It would have missed and
failed closed in exactly the same way.

That is worth recording because the proposal looked obviously correct, was written by
someone with the offsets in front of them, and was still wrong. The number was inferred from
deltas rather than read, and the delta had two causes that were assumed to be one.

### The rule that replaced it

Exact figure first, so every target that already worked takes the identical path and cannot
drift. Only on no match does the fallback run, anchored on properties that do not reference
UObject's tail at all:

- at least `outer_offset + 8`, because those members demonstrably exist
- 8-aligned, because UObject holds pointers
- no class smaller than the root, because everything derives from UObject
- an alignment in the following dword

Four constraints, none of which care what a fork appended. `UField::Next` then anchors on the
measured `sizeof(UObject)` — the report's reasoning, applied in the direction where it holds,
with the number read instead of inferred — and its chains must *link*, not merely terminate.

### Proving nothing else moved

A change to `DeriveStructLayout` touches every target, so "the new game works" is not
evidence. Six games were dumped twice against the same live process, once with 0.4.0 and once
with this build:

```
HRDINA                      4.22    3,172 types    no differences
Nightmare Kart              4.25    3,745 types    no differences
Peepo Island                5.0     6,059 types    no differences
Mizeria                     5.2     6,370 types    no differences
Ready or Not                5.3    10,731 types    no differences
Backrooms: Escape Together  5.7    13,599 types    no differences
```

43,676 types, both property models, both name pools, 100% byte-identical on every one.

### The shape of the mistake

Both bugs are the same shape as the ones already in this file: **a test that a wrong answer
passes trivially.** The `ClassifyObject` bug compared a class name that Blueprint types never
match. The CDO decoy passes a self-consistency check perfectly. Here, a field full of zeroes
satisfies "chains terminate" better than the real field does, because it terminates sooner.

The pattern is that a constraint phrased as an absence — does not loop, does not leave the
array, does not disagree — is satisfied best by a field containing nothing. Every such check
needs a paired constraint phrased as a presence. "Terminates" needed "and links". "Reports a
plausible size" needed "and nothing is smaller than the root".

### What the shader theory was worth

The first hypothesis was that the dump had been taken while shaders compiled. It was worth
checking and the first run could not rule it out, because shaders were genuinely at 86% at
the time. Waiting for 100% changed nothing: identical failure, identical offsets.

The useful part is that the screenshot caught it. A run that *looks* controlled and is not is
how a wrong conclusion gets published, and nothing in the log would have said so.

---

## 0.6.0 — a second runtime, and three lessons that generalise

Unity IL2CPP support lives in `src/il2cpp/` and is documented in `docs/IL2CPP.md`. What
belongs here is the part that is not about Unity at all.

### The ninth time a wrong answer scored perfectly

`MethodInfo` holds two or three code pointers, and exactly one of them is the method's
compiled body. The others are the invoker thunk and, since Unity 2021.2, a second body
pointer for value-type adjustors.

The invoker is the best decoy this project has produced. It sits immediately beside the real
field. It points into executable memory 100% of the time. It appears in the exception
directory. Every test of the form "does this look like a pointer to code" it passes perfectly,
because it *is* a pointer to code.

What separates it is not a property of pointers but a property of invokers: **one thunk serves
every method of a given signature shape.** So it repeats across a sample where a body does
not, and it stays constant inside one argument shape where a body varies. That is the positive
distinguishing property, and the exception directory — written by the linker from a source the
runtime knows nothing about — is the independent anchor.

Both are expressed as comparisons between two measured numbers rather than as thresholds
either has to clear, which matters: the absolute figures move with how varied the sample is,
and the ordering does not.

### A threshold that was wrong in kind, not in value

The first version gated a slot on "at least 90% of its values are function entry points in the
exception directory". On the first real game that rejected all three code slots.

The instinct is to lower the number. The number was not the problem. A leaf function — no
frame, no calls, nothing to unwind — is entitled to have no `.pdata` entry at all, and IL2CPP
emits enormous numbers of one-line accessors that are exactly that. Measured, code slots score
between 20% and 80%, and the 20% is the invoker slot, which is almost entirely leaves.

The check was asking the wrong question. "Is this a function entry point" is a fine
corroboration and a bad gate; "does this land in an executable section" is the gate, because a
pointer into the metadata blob fails it outright and a code pointer passes it always.

The general form: **when a constraint rejects everything, ask what it is actually measuring
before adjusting what it accepts.**

### A guard that was worse than the fault it caught

Reading a C# const meant calling into the runtime, and a const has no storage of its own, so
the call might walk off the end of something. The obvious defence is `__try`/`__except`.

It is not a defence. The runtime takes a lock on the way in, and unwinding out of the middle
of it leaves that lock held. The process does not die at the fault — it dies a little later,
somewhere unrelated, with nothing in the log connecting the two. That is strictly worse than
the crash, because the crash at least says where it was.

**A structured-exception guard around a call into someone else's runtime is not a safety net.**
It is safe around a copy that holds no locks, which is exactly where `InternalMemorySource`
already uses one. So the read goes through the memory provider with its bounds checked first,
and the runtime is asked only where it cannot — unguarded, honestly, with a switch to turn it
off for a build that does not survive it.

The postscript is worth having too: this guard was removed on the theory that it was crashing
Road 96, and it was not. The theory was tested — the target dies with const reading switched
off entirely — and the real cause turned out to be three separate things, none of them this.
Removing it was still right. A hypothesis being wrong does not make the code it condemned
correct.

### The linter earning its keep twice in one afternoon

`validate --strict` over the first Unity dump reported 34,315 errors.

Ten thousand of them were the linter's own fault: it was applying instance-layout rules to
static fields, which live in a different block entirely, and to members whose offset is
explicitly marked unresolved. It also treated two members at one offset as a contradiction,
which it is, unless the type declared an explicit layout — which is how C# writes a union.

The rest were real, and one of them was the good one: **`Vector3.z` runs past the end of
`Vector3`.** The type's size had been recorded unboxed, because that is what a struct
declaration is, while its field offsets were recorded boxed, because that is what the runtime
reports. Both numbers were right and they were measured from different origins, so nothing in
the record could be compared with anything else in it.

Nothing in the walk could have found that. Only reading the finished output back and checking
it against itself could, which is the whole argument for having the linter at all.

Zero errors now, on a 49,572-type dump.
