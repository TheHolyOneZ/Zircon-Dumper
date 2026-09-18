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

| game | GameAssembly | exports | `il2cpp_*` | required (39) | optional (35) |
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

The 35 optional entry points are present everywhere and are **still optional**: the moment one
of them is required, a stripped build the walk could have handled gets refused for the sake of
a nicety. Every one of them is guarded at the point of use.

---

## Why this one has to inject

The Unreal path is external and read-only and never touches the game. The IL2CPP path injects,
and the difference is not a preference:

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

| game | Unity | result |
|---|---|---|
| Cave Crawlers | modern | **L4.** 42,605 classes, 6,967 structs, 1,641 enums, 91,698 fields, 491,938 methods, 83,564 properties. 365,202 method bodies. `validate --strict`: **0 errors**, 35,110 warnings |
| Schedule I | modern | **L3.** 44,741 classes, 6,759 structs, 2,448 enums, 104,865 fields, 521,224 methods. 448,166 method bodies |
| Road 96 | older | **L0.** Resolves 39/39 entry points and derives the two-pointer `MethodInfo` correctly, then the walk takes the game down. See below |
| the other seven | — | **L0.** Export table and API resolution verified from the file; not yet walked live |

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

### Road 96 — a known-failing target

Road 96 (Unity 2019-era, cracked, MelonLoader present) resolves everything and then dies part
way into `mscorlib`, reproducibly, on the third call to `il2cpp_class_get_fields` for
`Mono.Xml.SmallXmlParser.AttrListImpl`. Its `mscorlib` has 1,709 types against Cave Crawlers'
30,581, so it is heavily managed-stripped.

Ruled out by direct experiment: the const read (it dies with `zircon-il2cpp-no-consts` set),
returning strings to `il2cpp_free`, the thread attach (which succeeds and is now checked), and
describing generic parameters or pointer types as types — all of which were separate real bugs
found while narrowing this one, and all of which are fixed.

The remaining hypothesis is that stripping has left a class whose field array does not match
its declared count, or that the game's own threads are still initialising the type while the
walk reads it. Neither is actionable from outside the runtime. Recorded rather than papered
over.
