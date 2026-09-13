# Changelog

Notable changes per release. Dates are when the work landed, not when it was tagged.

## 0.2.0 — 2026-09-13

### ProcessEvent, found by asking a question with a known answer

The SDK can call the game without anyone wiring anything up.

`UObject::ProcessEvent` is the entry point every reflected call goes through, and it is a
virtual. Its vtable slot is the one thing about calling that the reflection data does not
record, so unlike every other offset here it cannot be read. It can be confirmed.

UE ships a pure function whose answer is known before it runs. Call a candidate slot with
`KismetMathLibrary::Add_IntInt(2, 3)` and the slot that produces 5 is ProcessEvent. Twice,
with different numbers: a slot that writes nothing leaves the block zeroed, and a single
match against an expected zero would pass. The parameter offsets come from the same
reflection data as everything else rather than from a layout assumed here.

Found at slot 76 on a UE 5.6 game, twice, on two separate launches.

The cost is real. Reaching slot 76 means calling 74 virtuals that are not it, with two
pointers they were not expecting. Candidates outside executable memory are skipped, the
first slots are never tried because a destructor lives there, and the calls are guarded,
but the game generally dies a few seconds later. So it is opt-in twice over: nothing
happens unless a marker file sits beside the DLL, and the log says plainly what is about
to happen.

It only needs doing once. The probe runs before the dump, so the slot lands in the dump
header as `UObject.ProcessEvent`, and `emit cpp_sdk` bakes it into `Basic.hpp`:

```cpp
inline constexpr int kProcessEventSlot = 76;
```

`BindToZirconPayload()` then fills in both hooks, `FindFunction` from the payload's export
and `ProcessEvent` from each object's own vtable. An SDK generated from that dump calls
into the game with one line and no probing.

### The payload writes a log

`AllocConsole` gives the payload a window a fullscreen game covers, that some games
prevent outright, and that is gone when the process exits. Everything now also goes to
`zircon-out/zircon.log`, flushed per line, because the interesting log is the one written
by a payload that then took the game down with it.

The first version used `fopen_s`, which on MSVC opens exclusively, so nothing could read
the log while it was being written. Watching a payload work is most of the reason the file
exists. It uses `_fsopen` with `_SH_DENYWR` now.

### UE 5.3 verified, and the FProperty boundary pinned

Ready Or Not, which was already installed and which Dumper-7 identifies as 5.3.2. It
carries no engine version string at all, so the layout came entirely from memory.

```
201090 objects, 1406 packages, 6327 classes, 49946 properties, 25517 functions
SDK           259 headers, 45832 static_asserts, 0 errors, 0 warnings
bytecode      7700/7700 = 100.00%  (2173747 bytes, the largest tested)
defaults      32007
L5            10731 / 10731 types byte-identical against a 5.5 GB minidump
```

It also answered a question that had been open since Dark Pals. `FProperty` moved from
`next +0x20, offset +0x4c` to `next +0x18, offset +0x44` somewhere between 5.2 and 5.5,
and 5.3 already has the new shape. So the change landed in 5.3, and 5.4 is now bracketed
by two versions that agree with each other rather than two that differ.

### A second game found a second name collision

`TRANSPARENT` is an enumerator in `ERaMaterialName` and a macro in wingdi.h. The rename
list added for the previous game did not have it, because no list written against one game
predicts the next.

Names not on the rename list now get a guarded `#undef` as well, generated for every
identifier the SDK emits and firing only when the name really is a macro on whichever
Windows SDK is compiling. 165,000 preprocessor directives for a large game, costing about
two and a half seconds.

The rename list stays for the names whose loss would hurt the caller: undefining `TRUE`,
`RGB` or `SendMessage` fixes the header and breaks the program including it.

### Editing plain structs

`FVector`, `FRotator`, `FQuat`, `FLinearColor` and anything else whose members are all
numbers can be written now, which is what makes a teleport possible.

```
--set GravityDirection={Z=-2}      names, and only the ones you mention
--set GravityDirection=0,1,-3      positional, in declaration order
```

Names are matched without case, and a partial write leaves the members it did not name
alone. Everything is parsed before anything is written: a struct half-updated because the
third value was a typo is worse than one not updated at all.

The test is what would catch that regressing, and it was verified to by making the writer
write as it parsed. Three assertions fail.

A struct is only writable when every member is a number the writer already handles. One
holding a pointer, an `FName` or a nested struct is refused and says which: those carry
state a byte write corrupts, and `AttachmentReplication` is not `FVector` with more
fields.

### `zircon find`

Finding objects by what they hold, which is the question a dump on its own cannot answer.

```
> zircon find --pid 1234 --where MaxWalkSpeed>500 -n 4
OBJECT                                               PROPERTY        VALUE
/Script/Engine.Default__CharacterMovementComponent   MaxWalkSpeed    777
/Script/Engine.Default__Character.CharMoveComp       MaxWalkSpeed    600
```

`=`, `!=`, `<`, `>`, `<=`, `>=`. Values that are not numbers compare as the text the
reader produced, so `--where MovementMode=MOVE_Falling` works; asking for `<` on one of
those is refused rather than silently comparing strings. `-f` narrows by class first, and
the exit code is 3 when nothing matched.

### The SDK compiles next to `windows.h`

It did not, and nobody had noticed because the compile test used a translation unit with
nothing else in it. A cheat DLL includes `windows.h`, and doing that first produced 97
errors.

The preprocessor was rewriting reflected names before the compiler saw them. `PF_MAX` is
`AF_MAX` in winsock, so `EPixelFormat`'s last entry became `32 = 94`. `min` and `max` are
macros, so `int32 min(int32 A, int32 B)` stopped being a declaration.

Measured rather than guessed at: of the 66,114 identifiers emitted for a UE 5.6 game, 16
are macros on the Windows 10.0.26100 headers. `min`, `max`, `RGB`, `TRUE`, `FALSE`,
`PF_MAX`, `PlaySound`, `DrawText`, `GetObject`, `GetMessage`, `SendMessage`,
`GetCommandLine`, `GetCurrentTime`, `GetDiskFreeSpace`, `ReportEvent`, `UpdateResource`.

The SDK renames its own identifier, so `min` is emitted as `min_`. The first attempt
undefined the macros instead, which fixed the header and would have broken the caller:
`TRUE`, `RGB` and `SendMessage` are names their code is entitled to keep. It also cost two
seconds of compile time for 28,000 preprocessor directives that did nothing.

### The payload resolves functions for a generated SDK

`zircon.dll` exports `zircon_find_object`, and the SDK's `ZirconSDK::BindToZirconPayload()`
picks it up when the payload is loaded in the same process. Turning a path into a
UFunction is the object walk the payload has already done, so it is not work worth making
anyone repeat.

`ProcessEvent` stays the caller's one line. It is a virtual on `UObject` whose vtable index
the reflection data does not record, and guessing it would call something arbitrary on a
live object.

The helper is only declared when `<windows.h>` has already been included. The SDK will not
include it: several hundred macros arrive with it, and the section above is what happens
when they meet reflected names.

### `zircon write`

Editing was the one thing the browser could do that the command line could not, which made
it the odd feature out.

```
> zircon write --pid 1234 -f /Script/Engine.CharacterMovementComponent --set MaxWalkSpeed=777
object            /Script/Engine.CharacterMovementComponent
property          MaxWalkSpeed
was               9999
now               777
```

It resolves the same way `read` does, so naming a class writes its defaults, and an
inherited property can be named without qualifying it. The old value is printed because
the fastest way to undo a mistake is to have been shown what it replaced.

Refusals carry the reason: `StructProperty cannot be written safely`, `300 does not fit in
ByteProperty (1 byte)`, `has no property named 'NoSuchThing'`. Enums take a name, so
`--set MovementMode=MOVE_Flying` works.

If the value reads back as something other than what was written, that is reported too:
the game may own the field, and freezing in the browser is the answer.

### Freezing a value against the game

Editing a field the game owns achieves nothing: the next tick writes it back before the
row can even be re-read. Freezing holds it.

Click the dot beside any editable value and it turns into an asterisk. From then on the
value is re-applied every 30ms, which beats a 60Hz tick. Frozen entries are keyed by
address, so they survive selecting another object, and the list of what is held sits under
the table with a release button each.

Proven on a live UE 5.6 game by writing over a frozen field from a separate process:

```
frozen at 600     before write: 600   just written: 9999   after 600ms: 600
freeze released   before write: 600   just written: 9999   after 600ms: 9999
```

The second line is the control. Without it the first only shows that something wrote 600
at some point, not that freezing is what did it.

The interval is fixed rather than tied to the refresh slider, which someone could set to
two seconds and then wonder why freezing had stopped working. An entry that fails to write
for about a second is dropped, since that normally means the object is gone.

### Callable function wrappers in the C++ SDK

The last place Dumper-7's SDK was ahead. Classes now carry their reflected functions as
methods, with a parameter block per function laid out at the engine's own offsets:

```cpp
inline class APawn* UPawnMovementComponent::GetPawnOwner() {
    static void* function = nullptr;
    UPawnMovementComponent_GetPawnOwner_Params params{};
    ZirconSDK::Call(this, function, "/Script/Engine.PawnMovementComponent.GetPawnOwner", &params);
    return params.ReturnValue;
}
```

17,419 of the game's 17,441 reflected functions on a UE 5.6 target, against Dumper-7's
17,319 for the same game. The SDK compiles with zero errors and zero warnings.

UE dispatches a reflected call through `UObject::ProcessEvent`, which is virtual, and its
vtable index is not in the reflection data. Deriving it would mean binary analysis with
nothing independent to check the answer against. So the SDK names the one thing it cannot
know and asks for it once, in `Basic.hpp`:

```cpp
ZirconSDK::FindFunction = ...;   // resolve a UFunction by full path
ZirconSDK::ProcessEvent = ...;   // object, function, parameter block
```

Injected, both are a few lines. Each wrapper resolves its UFunction once and caches it,
and the function is addressed by full object path, which the dump does have.

Bodies and parameter blocks live in one `Functions.hpp`, included after every package, so
a parameter type is complete whichever package the engine put it in. The declarations stay
inside their class, where an incomplete type is all a declaration needs.

The first attempt emitted only 13,727 of them, skipping any function whose signature named
a by-value type from another package. That was built on a wrong belief about the emitter:
package headers were assumed to include only `Basic.hpp`, when they have always included
their value dependencies. What they did not include were packages reached only through a
function signature, since the dependency walk looked at properties alone. It walks
signatures now, and the restriction is gone.

### UFunction flags were reading a pointer

Found while marking static functions in the new wrappers: every function in the game
reported `Event | Static | Protected`.

`UFunction::FunctionFlags` was derived on one constraint, that exactly one of Public,
Private and Protected is set. The low half of a heap pointer a few bytes away satisfied it
for 15,884 of 17,441 functions, because those pointers all came from one region and shared
the same bit there. The offset resolved to `+0xe0`, which is past `Func`.

This is the failure this project has a rule about, and it is the seventh time: a field that
scores perfectly on a weak constraint is not the right field. Diversity is now the
independent check. Real flags differ across functions, since a native getter, a Blueprint
event and a replicated RPC have little in common, so a candidate where one value covers
half the target is rejected.

Flags now derive to `+0xb0`, and the most common value covers 13.2% instead of 91%.
`IsMoveInputIgnored` reads `Native, Public`; it previously read `Event, Static, Protected`.

Offsets were never affected, so no SDK was ever wrong about layout. What was wrong is
every consumer of `flag_names`: the docs emitter, the diff's severity reasoning, and
anything filtering on Static or Native.

### Live value editing

The browser could show what a property holds. Now it can change it.

Tick **Allow edits** in the top bar and a Value cell becomes editable. Numbers, bools and
enums, written into the running game and read back straight afterwards. If the game owns
the field and overwrites it on the next tick, the row snapping back is the answer worth
seeing.

Three constraints, each enforced in code rather than described in docs:

- **Off until switched on**, every session, and the switch reaches the memory source.
  While it is off `IMemorySource::Write` refuses at the bottom of the stack, so nothing
  higher up can write by mistake.
- **The write covers exactly the property's width.** 300 into a `uint8` is refused, not
  stored as 44.
- **A packed bool changes one bit.** `CharacterMovementComponent` puts seven flags in
  `0x02E8` alone; writing the byte whole would clear six unrelated settings silently.

Containers, strings, structs and object pointers are refused. Their memory carries
allocator state beside the value, and a byte write corrupts it with no immediate symptom.
Those cells stay plain text instead of offering an edit that would fail.

The external provider opens its handle without `PROCESS_VM_WRITE`, which is right for a
tool that only reads. `EnableWrites` reopens it with write access, so a session that never
enables editing never holds a handle capable of writing. `Write` also lifts page
protection for the write and restores it.

Proven against a live UE 5.6 game: `MaxWalkSpeed` 600 to 1337, confirmed from a separate
process, with the properties either side of it untouched.

`tests/test_write.cpp` covers the byte-level behaviour and was verified to fail when the
masked write is replaced with a whole-byte one.

### Dump from the GUI

A **Dump…** button, offering the same choices the CLI has: bytecode, defaults and name
pool, which of the eight formats to write, and where. It runs on a worker thread, because
walking seventy thousand objects takes seconds and a frozen window looks like a crash.

While a dump runs the UI reads the target zero times and says so. One reader at a time,
enforced by not having a second one.

### `zircon install`

Adds the folder it is sitting in to the user PATH, so `zircon` works from anywhere.
`HKCU` only, no elevation, and `zircon uninstall` removes it. Existing PATH entries are
never rewritten.

### Enum underlying types are wide enough for their values

Found by emitting an SDK for the same game as Dumper-7 and compiling both.

`UEnum` does not always state a width, so the IR falls back to `uint8`. A flags enum built
from bit positions overflows that at once: `ETransformGizmoSubElements` reaches 524287.
MSVC raised C4369 on 133 enumerators and clamped each one, leaving an SDK that compiled
while comparing against the wrong number.

The width is now computed from the values, signed when any value is negative, and never
narrowed below what the IR recorded. SDK output on a UE 5.6 game went from 133 warnings
to none, with offsets unchanged.

### One version string

The version was typed out by hand in three places: the CLI, the dump header, and the
`VERSIONINFO` block in the resource file. The dump header still said `0.1.0-dev` while the
CLI said `0.2.0-dev`, so a dump generated by 0.2.0 announced itself as 0.1.0 and every
generated SDK carried that into its banner. Nothing compares them, so nothing noticed.

The first fix caught two of the three and left the resource file, which then reported the
wrong version on the binaries themselves. All three now come from `project()` through
CMake, and `-DZIRCON_RELEASE=ON` drops the `-dev` suffix. That flag is the only difference
between a local build and a released one.

### Colour, and a CLI that reads like one

One place decides whether colour is on: TTY, `NO_COLOR`, `--color`/`--no-color`, and
whether the Windows console can render escapes. The injected payload's console uses the
same path.

The help screen showed an empty version string and listed eighteen commands as one flat
wall; it is grouped by task now, with pasteable examples. Results print as a dim label and
a value. `emit` printed 71 identical warnings for one unrepresentable property type, which
pushed the actual result off the screen; identical warnings are collapsed with a count,
eight distinct are shown, and `-v` prints them all.

### The browser looks like a tool

It was running on ImGui's 13px bitmap font. Segoe UI for prose, Cascadia Mono for the
columns that are really numbers: offsets, addresses and decompiled script, where fixed
pitch is not cosmetic because the nesting is leading spaces.

A real palette instead of two overrides on the default dark theme, a dark title bar
through DWM, one-pixel frame borders so an unchecked checkbox is visible, and a process
picker that names the project rather than only the executable.

### Icon and version resources

The binaries had no icon and no version block. `res/zircon.ico` is built at ten sizes, 16
through 256. One `VERSIONINFO` block is shared by the three thin `.rc` files, so the
version cannot drift between them.

### Fixes

- Font stack imbalance in the derived-offsets tooltip: a scope popped the font after
  `EndTooltip` and tripped ImGui's assert. The same shape was fixed in the dump log.
- The Value cell used a `Text` item, which is only as wide as its glyphs, so clicking the
  space past a short number did nothing. It is a `Selectable` now, which also shows on
  hover that the row is editable.
- `zircon inject` pointed users at `docs/ROADMAP.md`, a file that no longer exists.

### Packaging

The MSVC runtime is linked statically, so a release runs on a clean Windows install
without the Visual C++ redistributable. It matters most for the payload: an injected DLL
importing `MSVCP140.dll` lands in a process that may already have a different version
loaded.

### Documentation

Apache-2.0 with a `NOTICE` for the vendored ImGui and Lua. `docs/TEST_TARGETS.md` removed:
it was an inventory of one machine, and the two reusable findings in it moved into
`UE-Test.md`. `ARCHITECTURE.md` rewritten, having drifted into describing emitters and
files that were never built. `ROADMAP.md` became `ENGINEERING-LOG.md`.

The README is split into a user guide and a developer guide, and every screenshot now has
a text transcript beside it.

**Scope changed.** "Zircon reads" was true and is not any more. The line now sits between
data and code: editing a reflected value is data the engine already describes and changes
itself, while patching instructions, hooks and trampolines stay out, as does anti-cheat
evasion.

---

## 0.1.0 — 2026-09-13

First release. Phases P0 through P7 complete.

- Four memory providers behind one interface: injected, external, minidump, and a PE on
  disk. The reflection walker is written once.
- Every offset derived at runtime from a structural invariant. No engine-version table
  anywhere in the codebase.
- Eight output formats: `cpp_sdk`, `usmap`, `ida`, `ghidra`, `reclass`, `docs`, `graphs`,
  `json`.
- Kismet bytecode decompiler, roughly ninety opcodes.
- Build-to-build diffing, 27 change kinds graded by what they break, exit code 8 when
  something breaking changed.
- Live object browser, standalone and in-process.
- Plugin ABI in C, with Lua vendored and hosted as an ordinary plugin.
- Class default object values read externally, into the SDK, the docs and the diff.

Verified against fifteen games from UE 4.22 to UE 5.7, fourteen of them to byte-identical
agreement between independent memory providers.
