# The Unity IL2CPP backend

How Zircon dumps a Unity game, what it derives, what it refuses, and what it has been run
against. The Unreal equivalent is `docs/ENGINEERING-LOG.md` and `docs/UE-Test.md`; this is the
same material for `src/il2cpp/`.

---

## The problem with every other IL2CPP dumper

Unity's IL2CPP pipeline compiles C# to C++ to native. The type information ends up in two
places: `global-metadata.dat`, and registration structures in the binary. Both change shape
between metadata versions — 16, 19 through 24.5, 27.x, 29.x, 31.x, and whatever ships next.

So every tool built on parsing them carries a table of struct layouts per version. Each Unity
release breaks them until someone hand-adds the new layout, and a game that encrypts its
metadata stops them outright.

That is the same version-table dependency this project refuses for Unreal, and the answer
here is better than the Unreal one.

## Ask the runtime

`GameAssembly.dll` **exports the IL2CPP embedding C API by name**. It is a public contract
Unity has kept stable since 5.x, and it will answer questions about the runtime it belongs to:

```
il2cpp_domain_get_assemblies   il2cpp_class_get_fields    il2cpp_field_get_offset
il2cpp_assembly_get_image      il2cpp_class_get_methods   il2cpp_method_get_param
il2cpp_image_get_class_count   il2cpp_class_get_name      il2cpp_type_get_name
```

So: don't parse the metadata. Walk domain → assemblies → images → classes → members, and take
every offset from `il2cpp_field_get_offset`.

Two consequences worth stating plainly:

- **No per-version knowledge anywhere.** Unity 5.3 through whatever ships next year, one code
  path. There is no version table in `src/il2cpp/` and there will not be one.
- **Encrypted or scrambled metadata stops mattering.** By the time the process is running, the
  runtime has already decrypted it. This is the single biggest practical difference from
  Il2CppDumper and its descendants.

The one real failure mode is a build that strips or renames its exports, and that is reported
by name rather than guessed around.

### Measured before anything was written

Ten installed games, oldest Unity to newest:

| game | GameAssembly | exports | `il2cpp_*` | required (39) | optional (36) |
|---|---|---|---|---|---|
| Schedule I | 65.9 MB | 386 | 241 | all | all |
| Sons Of The Forest | 100.4 MB | 383 | 238 | all | all |
| Roadside Research | 66.6 MB | 382 | 241 | all | all |
| Cave Crawlers | 51.0 MB | 389 | 237 | all | all |
| Forensics Demo | 106.2 MB | 371 | 241 | all | all |
| Plants vs Zombies Replanted | 87.2 MB | 382 | 241 | all | all |
| GORN 2 | 78.6 MB | 386 | 241 | all | all |
| IRON NEST | 47.5 MB | 371 | 241 | all | all |
| Road 96 Mile 0 | 84.5 MB | 391 | 235 | all | all |
| Road 96 | 73.9 MB | 243 | 232 | all | all |

Road 96 is a much older Unity with roughly a third of the total exports, and it is still
complete. That is the point of the approach.

The 36 optional entry points are present everywhere and are **still optional**: the moment one
of them is required, a stripped build the walk could have handled gets refused for the sake of
a nicety. Every one of them is guarded at the point of use.

---

## Two readings, and why both

There are two places a Unity game keeps its type system, and they hold different halves of it.

| | live (inject) | static (`global-metadata.dat`) |
|---|---|---|
| type set | grows while the game runs | **exact and repeatable** |
| concrete generics | **the only place they exist** | absent |
| field offsets | **truth at execution** | absent: they are in the binary |
| method addresses | derived from `MethodInfo` | absent: they are in the binary |
| field and method names | yes | yes |
| metadata tokens | yes | yes |
| needs the game to run | **yes** | no |
| APK, console, anti-cheat | impossible | **fine** |
| encrypted metadata | irrelevant, it is already decrypted | defeated |

`--mode dual` runs both and merges them. The metadata is the spine, because its type set does
not move between two reads; the runtime is the truth about execution, because offsets and
concrete generics only exist once something has run. Every record says which side it came
from, and **every disagreement goes in `header.conflicts` rather than being resolved quietly**
-- on a packed build the disagreement is the finding.

Measured on Cave Crawlers: 12,318 types both sides had, 36,244 the runtime had and the file
could not contain (instantiations), 841 the file declared and the runtime never built. Each
reading alone was missing a large piece of the other's answer.

Those three numbers are one run, not a property of the game. The split moves with how much of
the game had happened before the walk: the class cache grows as code runs, so a dump taken at
the main menu leaves more types in the metadata-only column than one taken with
`--wait-for-settle`, or one taken after loading a level. The metadata column is fixed -- the
file does not change -- and the runtime column is whatever had been built by the time you
looked.

So two dual runs of the same build can report different splits and both be right. A number
that dropped since last time usually means more of the game had run, not that something was
lost. What does not move is the total type set, and any disagreement between the two sides
lands in `header.conflicts` either way.

---

## Reading the metadata without a version table

Every other IL2CPP dumper parses `global-metadata.dat` with a table of struct layouts per
metadata version, and breaks on each Unity release until somebody hand-adds the new one. That
is the same dependency this project refuses everywhere else, so the layout is worked out
instead, from four things the file cannot satisfy by accident.

**1. The spans tile the file.** The header is a run of `(offset, size)` entries, and they
cover the file from the end of the header to EOF in order, with only alignment padding and
never an overlap. The first entry's offset is the header's own length, which gives the entry
count once the stride is known -- and the stride is whatever divides the header exactly *and*
tiles. Older metadata writes `offset, size`; newer writes `offset, size, count`. Which one
falls out of this rather than being known.

**2. The identifier blob** is the span made of NUL-terminated names: 80%+ identifier bytes,
one terminator every twenty or so, ending terminated.

**3. A record table** is a span whose leading int32s land on the start of one of those names,
for every record probed. Probes are consecutive, because a stride that divides the real record
size survives sampling whenever every probe happens to land on a real boundary -- an early
version sampled evenly, the step shared a factor with the record, and a 16-byte stride
"validated" a table whose records are 88 bytes. A column that never changes is rejected too:
zero is a valid string index, so a run of zeroes otherwise reads as a table of names that are
all the first name.

**4. A `(start, count)` pair inside a record partitions the table it indexes.** The ranges
cover `[0, N)` exactly once, no gap and no overlap, and which `N` they reach is what says which
table the pair points at. Note *partitions*, not *tiles*: fields and methods are **not** laid
out in type order, and the first version of this required they were and found nothing at all.

Two more facts come free from ECMA-335 rather than from Unity, so they do not move: a metadata
token carries its table in the top byte (`0x02` type, `0x04` field, `0x06` method), which makes
the token column unmistakable and makes the token a join key between the two readings; and the
first type of every assembly is `<Module>`, which is the cheapest end-to-end check there is.

### Measured

| game | metadata | header | type record | result |
|---|---|---|---|---|
| Job Simulator | v24 | 2 int32 | 100 B | solved |
| Road 96 | v27 | 2 int32 | 88 B | solved |
| Road 96 Mile 0 | v29 | 2 int32 | 88 B | solved |
| Sons Of The Forest | v29 | 2 int32 | 88 B | solved |
| Cave Crawlers, Schedule I, PvZ, GORN 2, Roadside Research | v31 | 2 int32 | 88 B | solved |
| IRON NEST | v39 | **3 int32** | **76 B** | solved |
| Forensics Demo | v39 | 3 int32 | — | **refused** |

Ten of eleven, Unity 2019 through Unity 6, with no version knowledge anywhere. The refusal is
the interesting one: its header reads cleanly and its identifier blob is there, but no span
declares ranges that partition anything, and its type span is not even four-byte aligned. That
file's tables have been scrambled. Guessing past it would produce a dump that is quietly wrong
rather than one that is honestly absent.

### What it cannot do

A field's type, a method's address and a field's offset are **not in the metadata**. They live
in arrays in `GameAssembly.dll` that the metadata only holds indices into. So a static dump
carries names, namespaces, tokens, assemblies and structure, and marks the rest unresolved.

Nor is *what kind of type* it is. Struct and enum come from a type's parent, and the parent is
another index into the binary, so a static dump files every type as a class and says so in its
header. A dump reporting no enums at all would otherwise read as a game that has none. A dual
run takes that from the runtime.

That is a real limit, and the reason `--mode dual` exists rather than static replacing live.

---

## Why the live half has to inject

The Unreal path is external and read-only and never touches the game. The IL2CPP live path
injects, and the difference is not a preference:

> `il2cpp_field_get_offset` is a **function**. Reading memory will not make it run.

A dumper that refuses to inject has two options — reconstruct `Il2CppClass` by hand, which is
the version-table trap again, or do without field offsets. So Mode 1 is in-process.

`src/il2cpp/Bridge.h` is written as an interface for this reason: the walk talks to
`IBridge`, not to the C API, so a minidump reader and an external no-inject reader can slot in
under the same walk later rather than forking it.

Injection inherits Zircon's refusal to load into a process with anti-cheat present.

---

## What is derived, and how

Everything above comes from the API. One thing does not.

### Where a method's compiled body lives

There is no `il2cpp_method_get_pointer`, and the body is the most valuable thing in the dump.
It has to come out of `MethodInfo`, whose layout moves between versions — 2021.2 inserted a
second code pointer, and fields have been appended repeatedly.

`src/il2cpp/MethodLayout.cpp` derives it, with the same rules `src/engine/` works under: a
positive distinguishing property, plus an independent anchor arrived at by a different route.

**Anchored exactly.** `name`, `klass` and `return_type` are found by scanning the head of
`MethodInfo` for the very pointer the API returned. A slot that holds it is not a guess. The
metadata token corroborates by exact 32-bit match, confirming the block being read is a
`MethodInfo` at all.

**Bounded structurally.** Every code pointer sits below `name`. That is not a claim about a
Unity release; the struct has only ever grown by appending, so whatever the head holds, the
run of code pointers begins at zero and ends where the name begins.

**Told apart by what an invoker is.** Two or three code pointers remain, and the wrong answer
is extremely attractive: the invoker thunk sits immediately beside the body and passes every
"does this point at code" test perfectly. It is separated by its defining property — one
invoker serves every method of a given signature shape, so across a sample it repeats far more
than a body does, and it barely varies inside one argument shape. Both are stated as
comparisons between two measured numbers rather than thresholds either has to clear, because
the absolute figures move with the sample and the ordering does not.

**Checked against something the runtime did not write.** A slot counts as holding code only if
its values land in an executable section, which a pointer into the metadata blob fails
outright. The exception directory — written by the linker, from a different source than
anything the runtime knows — then confirms those addresses are function entry points rather
than somewhere in the middle of `.text`.

**Refused when they disagree.** If the invoker-shaped slot is not the last pointer before the
name, or the two slots do not differ in repetition the way a body and an invoker must, the
answer is a refusal with the numbers in it. The dump then carries no RVAs rather than wrong
ones.

On the corpus this derives two different layouts with no version knowledge:

```
Cave Crawlers   body 0x0   virtual 0x8   invoker 0x10   name 0x18
Schedule I      body 0x0   virtual 0x8   invoker 0x10   name 0x18
Road 96         body 0x0                 invoker 0x8    name 0x10
```

The exception-directory figure is worth recording, because it is what a first attempt got
wrong: code slots score between **20% and 80%** there, not the 90%+ that seems obvious. A leaf
function is entitled to no unwind data, IL2CPP emits enormous numbers of one-line accessors,
and invoker thunks are almost all leaves. Gating on that number rejected every slot on the
first real game.

---

## What it refuses to answer

| situation | what the dump says |
|---|---|
| an open generic definition | `offset_unresolved` on every member. `List<T>` has no layout to have offsets in, and the runtime returns a number anyway |
| a const | `offset_unresolved`. A const has no storage |
| a thread-static | `offset_unresolved`. The runtime signals it with a negative offset, which is not an offset |
| a const the runtime will not read | the enum keeps its member names and carries `values_resolved: false` |
| a method sharing its address | `shared_body`, counted from the finished dump |
| an instantiation over another generic's parameter | one of each name is kept and the rest counted. They are separate classes to the runtime and indistinguishable to everything else |

**A generic parameter is not a type, and neither is a pointer.** Asked to describe the `T` in
`List<T>` as though it were a type, an older runtime does not return nonsense — it walks into
a null and takes the process with it. Asked for the class behind a pointer type, IL2CPP goes
and *builds* one, allocating and taking locks, which an injected thread has no business
requesting. The walk only asks for a class where one already exists.

---

## Identity, and why paths look the way they do

A type's path in the IR is its identity: emitters key on it, `diff` matches on it, and
`validate` rejects a dump in which two types claim the same one.

A C# name is unique only inside its assembly, and not by a narrow margin — every assembly
declares a `<Module>`, and a game with eighty assemblies declares eighty of them. So paths are
assembly-qualified in the spelling .NET itself uses:

```
UnityEngine.Vector3, UnityEngine.CoreModule
System.Collections.Generic.List<System.Int32>, mscorlib
```

This is the same thing the Unreal side does by starting every path with `/Script/Engine`. It is
package qualification, written the way this ecosystem writes it.

Instantiations are named from the **type**, not the class: `il2cpp_class_get_name` answers
`List`1` for `List<int>` and `List<string>` alike, and building paths from it collapsed 52,255
records in one game onto 14,522 of them.

---

## The value-type trap

The runtime measures every field offset from the start of a **boxed** object — header
included — for value types as much as for reference types. Unboxed, a struct's data starts at
zero. Half the published Unity SDKs are wrong by exactly that header.

Zircon asks the runtime for the header size (`il2cpp_object_header_size`) rather than assuming
0x10, records the field's place in the type as `offset`, and keeps the raw number as
`boxed_offset`. Both are in the dump and neither is inferred from the other by a reader.

Getting this half-right is instructive: the first pass recorded the type's size unboxed and
its offsets boxed, so nothing in a record could be compared with anything else in it. The
project's own linter found it, reporting that `Vector3.z` ran past the end of `Vector3`.

---

## A guard that was worse than the fault

The first way of reading a const wrapped `il2cpp_field_static_get_value` in
`__try`/`__except`, reasoning that a const has no storage and the call might walk off the end
of something.

**That is worse than the fault it catches.** The runtime takes a lock on the way in, and
unwinding out of the middle leaves it held. The process does not die at the fault; it dies a
little later somewhere unrelated, with nothing in the log connecting the two.

Guarding is safe where it guards a copy that holds no locks, which is where the memory
provider already does it. So the walk reads the static block directly when it can bounds-check
it, and asks the runtime only when it cannot — unguarded, because a guard there would be a
lie. A build that does not survive being asked can be told not to be, with a
`zircon-il2cpp-no-consts` marker beside the DLL.

---

## When the runtime faults

Some builds strip a type's metadata and leave its class record in the image. Asking that class
for its fields or its interfaces makes the runtime dereference a pointer into nothing. There is
no way to ask that works, and no way to know in advance without reading `Il2CppClass` by hand,
which is the version-table trap.

Three pieces, none of which catch the fault:

**The breadcrumb.** Before touching anything, the walk writes what it is about to touch into a
memory-mapped page: the type's assembly-qualified path on the first line, which member list on
the second. A `memcpy` and nothing else, so it stays on for every dump. Windows writes the
dirty page back even when the process is torn down, so the file survives the crash.

Before the first class is fetched it is the slot instead — `mscorlib.dll#412` — because naming
a class means asking the runtime for it, and a build that faults on *that* never gets far
enough to have a name.

**The fault watcher.** `AddVectoredExceptionHandler`, first in line, for access violations and
the other machine-level faults on the walking thread. Not for the C++ and CLR exceptions a
game raises all day, and not for other threads: a handler registered process-wide sees every
thread, and a game taking a first-chance fault of its own would otherwise get whatever type
the walk happened to be on written down as unreadable. It
appends the exception code, the faulting address, what was being read, and whether the address
falls inside `GameAssembly.dll` — then returns `EXCEPTION_CONTINUE_SEARCH`. It handles nothing.
See "A guard that was worse than the fault" above; that argument applies here and is the reason
this observes rather than intervenes. It allocates nothing and takes no lock, because it runs
on a thread that has just faulted and the heap lock may be that thread's.

**Auto-resume.** On the next injection the payload reads the breadcrumb. If a fault was
recorded *inside the runtime*, the type goes in `zircon-out\logs\<Game>.unreadable` and the
walk goes around it. If the fault was anywhere else it is Zircon's own bug and nothing is
skipped — it keeps crashing until somebody looks at it. That is what separates this from a
`--best-effort` flag that papers over a defect.

Every skipped type is named in `header.engine.evidence`, so a dump that lost something says so
in the dump, on the website, and in the log. `zircon-il2cpp-skip.txt` beside the DLL is the
hand-written list for anything else; it takes the breadcrumb's first line verbatim, matched
exactly, never as a prefix.

---

## Publishing

Unity dumps publish to Zdex. Zdex reads `header.runtime` and treats the dump as what it is:
the same browsing, search and diff as an Unreal build, with C# rendering (`ref Vector3 a`,
not `const FVector& a`), assembly-qualified type URLs, and no `.usmap` or C++ SDK offered
because neither exists for Unity.

It was going to be refused. The reason it is not: the walk fills `ir::Dump` rather than a
parallel model, so Zdex's SQLite sidecar (`packages / types / members / functions`) already
fit an assembly, a C# type, a field and a method without knowing it. The Zdex-side work was
a migration, a handful of columns, a renderer and some copy — not a second importer.

Verified against the live server on 2026-09-18: Cave Crawlers, 565 MB of JSON, 22 MB
gzipped, one command, imported and browsable. And HRDINA dumped three ways — CLI from
outside, payload from inside, standalone browser from outside — published from each route
and diffed **identical** by Zdex's own diff.

## Coverage

What "tested" means here matches `docs/UE-Test.md`: L0 is the export table read from the file,
L3 is a complete dump, L4 is a dump that passes `validate --strict`.

**The counts below are one run, not a property of the build.** The runtime's class cache grows
while a game is running, and the inflated-generic sweep reads that cache, so *when* you inject
changes how many types you get. Two dumps of the same Cave Crawlers build, minutes apart, gave
42,528 and 42,610 classes. Image enumeration is stable; the sweep is not. Two consequences
worth stating plainly: a build-to-build diff currently contains some noise about how long each
game sat at its menu, and a count that differs from the table is not evidence the build moved.

| game | Unity | result |
|---|---|---|
| Cave Crawlers | modern | **L4.** 42,601 classes, 6,967 structs, 1,641 enums, 91,698 fields, 491,926 methods, 83,564 properties. 364,976 method bodies. `validate --strict`: **0 errors**, 35,111 warnings |
| PvZ Replanted | modern | **L4.** 44,810 classes, 9,101 structs, 1,946 enums, 104,257 fields, 516,258 methods, 88,165 properties. 445,628 method bodies. **0 errors**, 19,067 warnings |
| IRON NEST | modern | **L4.** 36,895 classes, 8,796 structs, 2,654 enums, 107,603 fields, 518,194 methods, 76,495 properties. 456,444 method bodies. **0 errors**, 31,519 warnings |
| Schedule I | modern | **L3.** 44,741 classes, 6,759 structs, 2,448 enums, 104,865 fields, 521,224 methods. 448,166 method bodies |
| Road 96 | older | **L0.** Resolves 39/39 entry points and derives the two-pointer `MethodInfo` correctly, then its runtime faults on its own stripped types. See below |
| Road 96 Mile 0 | older | **L0.** Same, with a three-slot `MethodInfo` rather than two |
| the other five | — | **L0.** Export table and API resolution verified from the file; not yet walked live |

The first three were dumped back to back in 85 seconds, unattended, one command each:

```
zircon inject --launch "<exe>" --wait --headless --wait-for-settle 25 -o <game>.json
```

Road 96 as the fourth exited **4** without writing a dump, which is the `--wait` contract
working in the direction that matters.

### Cross-checked against facts, not against itself

Known layouts, from a Cave Crawlers dump:

```
UnityEngine.Vector3      12 bytes   x 0   y 4   z 8
UnityEngine.Quaternion   16 bytes   x 0   y 4   z 8   w 12
UnityEngine.Bounds       24 bytes   m_Center 0   m_Extents 12
System.String                       _stringLength 0x10   _firstChar 0x14
```

Enum values, which matter because a plausible-looking wrong answer is easy here — numbering
members by position would produce 0, 1, 2, 3 and look entirely reasonable:

```
UnityEngine.KeyCode   None = 0   Backspace = 8   Tab = 9   Delete = 127
```

Those are the real, non-sequential values.

### Cross-checked against the compiled code

The strongest check available, because it uses nothing the runtime said. An auto-property
getter compiles to a load from its backing field, so the field offset in the dump should be
the displacement in the machine code at the method RVA in the dump:

```
assembly                                checked    agree  DISAGREE
Assembly-CSharp.dll                          13       13         0
UnityEngine.CoreModule.dll                    3        3         0
UnityEngine.UIElementsModule.dll             34       34         0
System.dll                                    4        4         0
Unity.TextMeshPro.dll                         1        1         0
UnityEngine.UI.dll                           25       25         0
Unity.InputSystem.dll                        15       15         0
                                             95       95         0
```

Both the offset and the RVA are confirmed by each agreement, since a wrong RVA would land on
unrelated code and a wrong offset would not appear in the right code.

Worth recording about the method: three cases looked like disagreements and were not. A
getter that returns a struct by value takes a hidden return buffer in `rcx`, so `this` moves
to `rdx` — the dump was right and the disassembly was being read too narrowly. The first
version of the check also matched fixed byte patterns rather than decoding ModRM, so it saw
only loads into `xmm0` and missed the second half of a `Vector2` getter. A verification
harness gets the same scrutiny as the thing it verifies.

### About those 35,110 warnings

All of one kind, and expected rather than tolerated: a method signature names a generic
instantiation the game has never used. `IEnumerator<ISpawnable>` appears in a signature, but no
code has ever asked for one, so the runtime has not built it and a dump *of the runtime* cannot
contain it. The reference keeps the exact C# name in `raw`; only the link is dangling.

Resolving them would mean calling `il2cpp_class_from_type` on each, which makes the runtime
build the class — changing the game to describe it. That is the wrong trade.

### Road 96 — a runtime that faults on its own types

Road 96 and Road 96 Mile 0 (both Unity 2019-era) resolve everything, derive `MethodInfo`
correctly — and derive *different* layouts, two slots against three, which is what exonerated
the derivation — and then take the game down part way into `mscorlib`. Both have a heavily
managed-stripped `mscorlib`: 1,709 and 1,790 types against Cave Crawlers' 30,581.

What it actually is, from the fault watcher:

```
Mono.Globalization.Unicode.ContractionComparer, mscorlib
interfaces
faulted inside the runtime: code 0xc0000005 at 0x7ffbeb257d68 (+0x307d68)
                            reading 0x2aae30dd9a0
```

An access violation inside `GameAssembly.dll`, dereferencing a pointer into nothing. Stripping
removed the type's metadata and left its class record in the image's table. Ask that class for
its fields or its interfaces and the runtime walks into the hole. It is not one bad class
either — walk past `AttrListImpl` and the next one along fails the same way in a different
member list.

There is no way to ask differently. What there is, is a way to not ask: the walk records the
type and goes around it next time. See "When the runtime faults" above. Every type walked past
is named in the dump header.

Ruled out by direct experiment, so nobody retests them: the const read (it dies with
`zircon-il2cpp-no-consts` set), returning strings to `il2cpp_free`, the thread attach (which
succeeds and is now checked), describing generic parameters or pointer types as types, the
terminating `il2cpp_class_get_fields` call (bounded by `il2cpp_class_num_fields` now, and it
still dies), and MelonLoader — three games that dump cleanly have it installed and Mile 0,
which fails, does not.
