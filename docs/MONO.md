# The Mono backend

Unity has two scripting backends. IL2CPP compiles C# to C++ to native and keeps its type
information in `global-metadata.dat` plus registration structs; `docs/IL2CPP.md` covers that
one. Mono is the other, it is what Unity shipped for years, and plenty of games still use it.

A Mono game looks like this:

```
Game.exe
UnityPlayer.dll
MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll     the runtime
Game_Data/Managed/*.dll                              the code, as real .NET assemblies
```

Two things follow from that, and they are what make this backend different from IL2CPP.

## No version table, for two separate reasons

**The runtime is asked, not parsed.** `mono-2.0-bdwgc.dll` exports the Mono embedding API by
name — 1,202 `mono_*` entry points on the builds measured here. Zircon resolves 34 required and
35 optional ones and calls them. Nothing reconstructs `MonoClass` by hand, so nothing breaks
when Unity changes it.

**The assemblies are a published standard.** Unlike `global-metadata.dat`, which needed a
constraint solver because its layout moves per metadata version, `Managed/*.dll` is ECMA-335.
There was nothing to derive. `src/mono/src/Assembly.cpp` reads the CLI header, the metadata
root, the `#~` table stream and the heaps directly.

The one place that needs care is the table row widths. A coded index is 2 bytes or 4 depending
on how many rows the tables it can point at hold, so every table's width has to be computed
before any table's *offset* is known — get one wrong and every later table is misplaced.
`MemberRef.Class` is `MemberRefParent`, a **3-bit** coded index over five tables, not the
2-bit `TypeDefOrRef` over three. Both are 2 bytes until `MethodDef` passes 8,192 rows, which is
why the mistake read fine on small assemblies and put the assembly name at the wrong offset in
`Unity.Mathematics` — it reported its own name as `1` and its version as `86.93.100.107`.

## Neither half is complete

| | live runtime | the assemblies |
|---|---|---|
| type system, names, namespaces, nesting | yes | yes |
| base types, interfaces | yes | yes |
| **field offsets** | **yes** | no |
| **enum values** | no | **yes** |
| **IL RVAs** | no | **yes** |
| assembly version and MVID | partly | **yes** |
| needs the game to run | **yes** | no |

**Only the runtime knows field offsets.** The CLI does not store them. A type's layout is
computed the first time it is used, which is why `mono_field_get_offset` is a function call and
not a table lookup.

**Only the assemblies know enum values.** A `const` has no storage, and there is no call in the
embedding API that reads one — the value lives in the `Constant` table and nowhere else at
runtime. Any dumper that reports enum values from a live Mono process is reporting something it
did not read. Zircon's live walk marks them `values_resolved: false` and the merge fills them
in.

**Only the assemblies know where a method body is.** Mono compiles a method the first time it
is called, so there is no stable native address to record the way there is on IL2CPP. The IL
RVA is the part that does not move, which is what `functions[].il_rva` carries.

So a live Mono dump reads the assemblies too, by default, and merges. `--mode live` opts out
and says in the header what is missing as a result.

Measured on Haste, one run: 16,816 types both sides had, 118,218 the assemblies declared that
the runtime had never built, 2,135 enums filled in, 142,113 IL bodies placed, 43,769 field
offsets, 0 lint errors. As with the IL2CPP dual split, those numbers move with how much of the
game had run before the walk — the assembly column is fixed, the runtime column is whatever had
been built by the time you looked.

## Verified against another implementation

The static reader is checked against `System.Reflection.Metadata`, which is Microsoft's own
ECMA-335 implementation, over all 217 assemblies of one game:

| | Zircon | .NET |
|---|---|---|
| assemblies read | 217 | 217 |
| types | 137,783 | 137,783 |
| methods | 829,152 | 829,152 |
| methods with an IL body | 427,639 | 427,639 |
| enums | 3,709 | 3,709 |
| enum values | 40,362 | 40,362 |
| MVID of `Assembly-CSharp` | `2da85630-…` | same |

Field counts differ by exactly 44,071, which is 40,362 enum literals plus 3,709 `value__`
fields. This dump files those as enum values rather than as fields, so the two reconcile to the
byte. That is the independent anchor `CONTRIBUTING.md` asks for, and it is worth keeping: a
reader that agrees with Microsoft's on seven metrics across 217 real assemblies is not agreeing
by luck.

## What the live walk deliberately does not ask

Three things killed real games during development, and all three were the same shape — a C API
called through a prototype that did not match it.

`mono_assembly_name_get_version` takes four `uint16_t*` out-params and writes through all of
them. Called as a one-argument getter it wrote into whatever the argument registers held.

`mono_type_get_class` is a raw union read of `type->data.klass`. For a multi-dimensional array
that union holds a `MonoArrayType*`, so `System.Globalization.ChineseLunisolarCalendar` — which
has `static readonly int[,]` fields — had `mono_class_init` called on something that had never
been a class. Array types go through `mono_class_from_mono_type` now, which handles every case,
and the element class comes from `mono_class_get_element_class`.

The third was not a prototype but a policy. Asking the runtime for **method signatures** meant
`mono_method_signature` and per-parameter `mono_type_get_name` on every method of every type,
including unsafe P/Invoke declarations with pointer parameters, and it faulted inside the
runtime. The assemblies describe signatures better anyway, so the live walk no longer asks. It
asks for field offsets, which are the one thing only the runtime can answer.

Note for anyone carrying over 0.8.0's findings: Mono's `mono_bool` really is a 32-bit int, so
the IL2CPP `bool`-return trap does **not** apply here. `int` is correct for `mono_class_is_*`.

## Coverage

What has actually been run, in the sense `docs/UE-Test.md` means it.

| Target | Store | How | Result |
|---|---|---|---|
| Haste | Steam | launched by Zircon, injected, assemblies merged | 217 packages, 133,468 types, 43,769 field offsets, 43,930 enum values, 0 lint errors |
| Haste | — | `--managed`, game never started | 217/217 assemblies, 137,557 types, 40,362 enum values, 0 lint errors |
| How to Fish | Steam | launched by Zircon, wrapper handed off, injected | 178 packages, 26,519 types, 0 lint errors |
| 217 assemblies of Haste | — | A/B against `System.Reflection.Metadata` | every metric identical |

Both live dumps were published to Zdex and imported: the runtime shows as Unity Mono, the schema
as 4, and no `.usmap` or C++ SDK is offered, which is correct for a Unity dump.

Nineteen Mono games are installed on the development machine; two have been walked live. That is
the same shape of gap the IL2CPP backend had at 0.6.0, and it is the thing to widen next — a
`batch` run across all nineteen would say more than any amount of reasoning here.

## Commands

```
zircon check <Game.exe>                             which backend, and what will work
zircon assemblies <Managed>                         what the assemblies hold
zircon assemblies <Managed>/Assembly-CSharp.dll     one of them, in detail
zircon dump --managed <Managed> -o game.json.gz     no process at all
zircon inject --launch "<Game.exe>" --wait          live, assemblies merged in
zircon emit csharp game.json.gz -o src/             the source tree
```

`scan-games` records each Mono game's `Managed` folder in the `metadata` field of the manifest,
so `batch` runs them live and falls back to the assemblies when a walk does not finish — the
same degraded accounting the IL2CPP fallback gets.
