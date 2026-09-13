<img src="docs/images/icon.png" alt="Zircon" width="112" align="right">

# Zircon

**Unreal Engine reflection extraction and analysis toolkit.**

Point it at a UE game — running, crashed, or just sitting on disk — and get back the
engine's entire type system: every class, struct, enum, property offset, function
signature and Blueprint script. Then turn that into a C++ SDK, a `.usmap`, types for IDA
or Ghidra, readable Blueprint logic, or a report on what a patch just broke.

<sub>Fifteen games verified · UE 4.22 → 5.7 · both property systems · both name pools ·
no engine-version table anywhere in the codebase</sub>

---

## Pick your door

<table>
<tr>
<td width="50%" valign="top">

### 🎮 &nbsp;[I want to **use** it](#users)

Dump a game, generate an SDK, browse live objects, and find out where the files went.
Written for people who just want the output.

**[Get started →](#get-started-in-four-commands)**

</td>
<td width="50%" valign="top">

### 🔧 &nbsp;[I want to know **how it works**](#devs)

Derivation strategy, architecture, the IR contract, the plugin ABI, and an honest
comparison with Dumper-7 and UEDumper.

**[Read the design →](#why-it-works-the-way-it-does)**

</td>
</tr>
</table>

---

<a id="users"></a>

# Part I · For users

*Everything in this half assumes no reverse-engineering background. If a term is needed,
it gets explained where it appears.*

---

## What Zircon gives you

### It finds the game for you

You don't tell Zircon which engine version to assume, and you don't pick your game from a
list of supported titles. It scores every running process on what is actually inside it,
then reads the engine version out of memory.

![Terminal showing zircon detect and zircon fingerprint output](docs/images/cli-detect.png)

```
> zircon detect
SCORE  PID      PROCESS                          PROJECT
 100%  25244    StormEscape-Win64-Shipping.exe   StormEscape
  20%  7248     crashpad_handler.exe             crashpad_handler
  20%  8368     crashpad_handler.exe             crashpad_handler

> zircon fingerprint --pid 25244
engine version    5.6
confidence        60%
property model    FProperty
name pool         FNamePool
object array      chunked
evidence
  - version string: ++UE5+Release-5.6
  - layout defaults from 5.6: FProperty, FNamePool, chunked GObjects
```

`100%` is your game. The 20% rows are the helper processes every launcher spawns. Hover
or use `-v` to see the evidence behind each score.

### It dumps everything in one pass

![Terminal showing a full dump, with one log line per derived offset](docs/images/cli-dump.png)

```
> zircon dump --pid 25244 --script --defaults -o game.json
[info ] target: external process 'StormEscape-Win64-Shipping.exe' (pid 25244), 161 modules
[info ] object array at 0x7ff7625a70d0 (GObjects 0x7ff7625a70c0): 72561 objects, index offset +0xc
[info ] name pool blocks at 0x7ff7624c3590 (confidence 0.95, block-offset bits 16)
[info ] UObject layout: index +0xc, class +0x10, name +0x18, outer +0x20
[info ] UStruct layout: super +0x40, children +0x48, childprops +0x50, size +0x58, align +0x5c
[info ] FProperty layout: class +0x8, next +0x18, name +0x20, dim +0x30, size +0x34, flags +0x38, offset +0x44
[info ] UClass layout: CDO +0x110 (300/300 named Default__*)
[info ] UClass layout: Interfaces +0x1d8, stride 16 (13 classes, 18 entries)
[info ] UFunction layout: next +0x28, flags +0xe0, func +0xd8
[info ] UEnum layout: names +0x40, stride 16
[info ] bytecode constants: double-precision vectors (FVector 24 bytes)
[info ] UStruct::Script at +0x60
output            game.json
packages          618
classes           5252
structs           5598
enums             2063
properties        48692
functions         17698
```

Every one of those `[info]` lines is Zircon **working out** where a field lives in *this
specific build* — not looking it up in a table. `CDO +0x110 (300/300 named Default__*)` is
the shape of all of them: a candidate offset, and the independent check that confirmed it.

That is why it works on games patched yesterday, on studio-modified engines, and on engine
versions that did not exist when the tool was written.

The result is one JSON file containing the game's whole type system.

### It turns that into eight different things

| You want… | Use | What you get |
|---|---|---|
| To write a cheat/mod in C++ | `cpp_sdk` | Headers with every class, correct offsets, and compile-time checks |
| To use UE4SS, FModel, or an asset tool | `usmap` | A `.usmap` mappings file |
| To reverse the binary in IDA Pro | `ida` | A Python script that imports every struct into your database |
| Same, but Ghidra | `ghidra` | The same, for Ghidra |
| To poke at memory by hand | `reclass` | A ReClass.NET project |
| To read the API like documentation | `docs` | Browsable Markdown with an index |
| To see the class hierarchy | `graphs` | Inheritance graphs (DOT + Mermaid) |
| To script your own output | `json` | The raw IR, plus [plugins](#plugins) |

![Terminal showing the emitter list and a diff reporting no differences](docs/images/cli-emit-diff.png)

```
> zircon emit list
FORMAT     NEEDS  DESCRIPTION
cpp_sdk    objs   C++ SDK headers with static_assert offset checks
usmap      objs   UE4SS / FModel .usmap mappings
ida        objs   IDA Pro Python script importing types
ghidra     objs   Ghidra Python script importing types
reclass    objs   ReClass.NET node file
docs       objs   Browsable Markdown API reference
graphs     objs   Inheritance graphs in DOT and Mermaid
json       -      Re-emit the IR as JSON

> zircon emit cpp_sdk game.json -o out/
files written     620
  out/SDK/Basic.hpp
  out/SDK/Engine.hpp
  ... and 608 more
```

### It shows you the game while it runs

A live object browser — standalone, or opened from inside the game if you inject.

![The browser showing CharacterMovementComponent's properties with live values](docs/images/gui-properties.png)

The object list is on the left with a filter box; the right pane is a table of
`Offset / Type / Name / Value`, re-read on a timer. Selecting
`/Script/Engine.CharacterMovementComponent` on a UE 5.6 game gives its real defaults:

```
Offset   Type                  Name                    Value
0x0198   Character*            CharacterOwner          nullptr
0x01A0   float                 GravityScale            1
0x01A4   float                 MaxStepHeight           45
0x01A8   float                 JumpZVelocity           420
0x01D0   float                 WalkableFloorZ          0.71
0x01D8   FVector               GravityDirection        {X=0, Y=0, Z=-1}
0x0231   EMovementMode         MovementMode            MOVE_None
0x0233   ENetworkSmoothingMode NetworkSmoothingMode    Exponential
0x0278   float                 MaxWalkSpeed            600
0x028C   float                 MaxAcceleration         2048
```

Enums resolve to their names rather than integers, object properties to their paths,
structs to their members, and unreadable memory says so instead of showing zeroes.

### It reads Blueprint scripts back into something readable

![The Script tab showing decompiled Kismet bytecode for a Blueprint graph](docs/images/gui-script.png)

Roughly ninety Kismet opcodes, with names, properties and called functions resolved
through the same reflection data. Each line is prefixed with its bytecode offset:

```
0000  PushFlow(Label_0416);
0005  goto [EntryPoint];
001F  Label_001F:
001F  CallFunc_IsValid_ReturnValue = IsValid(IndicatorWidget);
003C  if (!(CallFunc_IsValid_ReturnValue)) PopFlow();
0046  CallFunc_GetTornadoCategory_ReturnValue = K2Node_Event_Tornado_1.GetTornadoCategory();
0078  IndicatorWidget.HandleSpawn(CallFunc_GetTornadoCategory_ReturnValue);
0207  K2Node_DynamicCast_AsW_Tornado_Indicator = Cast<W_TornadoIndicator_C>(CallFunc_GetWidget_ReturnValue);
0248  if (!(K2Node_DynamicCast_bSuccess)) PopFlow();
```

Available in the GUI's **Script** tab, and from the CLI as
`zircon script --pid 12345 -f <name>`.

### It tells you what an update broke

Dump before the patch, dump after, and `zircon diff` reports every class that changed
size, every property that moved, and every function that vanished — graded by how badly
it breaks code you already wrote.

```
> zircon diff before.json after.json
NMKART-Win64-Shipping.exe (4.25)  ->  NMKART-Win64-Shipping.exe (4.25)

types               3726  ->      3726
properties         21023  ->     21023
functions          11227  ->     11227

3726 of 3726 types present in both are byte-identical (100.0%)

no differences
```

---

## Three binaries — which one do you want?

Zircon builds three things. They are not alternatives to each other; they are three ways
into the same engine.

| | What it is | Use it when |
|---|---|---|
| **`zircon.exe`** | the command-line tool | almost always — it does everything, from outside the game |
| **`zircon-gui.exe`** | the live object browser | you want to look around a running game, or dump by clicking |
| **`zircon.dll`** | the injectable payload | you specifically want to be *inside* the process |

### "Is `zircon.dll` like Dumper-7's DLL — inject it and it dumps?"

**Yes, that exact model works.** Inject it and it dumps the SDK by itself, no interaction:

```
zircon inject --pid 12345
```

It attaches, derives the layout, writes a **C++ SDK, a `.usmap` and the full JSON dump**
to `zircon-out\` next to the DLL — and then, unlike a fire-and-forget dumper, it opens the
live browser in its own window so you can keep poking at the game. Close that window and
it unloads itself.

**But the important difference: you almost certainly don't need to inject at all.**
`zircon.exe` produces the same SDK, the same usmap, the same everything, by reading the
process from outside. Injection buys exactly one capability — the ability to *call* the
game's own functions — and if you don't need that, the external path is strictly safer,
because a bug in Zircon cannot crash a game it never wrote to.

So: Dumper-7's inject-and-done workflow is supported, it just isn't the default.

### "Can the GUI dump, or does it only browse?"

**It dumps.** There's a **Dump…** button in the top bar once you're attached.

![The GUI's dump dialog, with include options, format checkboxes, an output path and a log](docs/images/gui-dump.png)

The dialog holds the same choices the CLI has:

```
Dump this target
  Include    [x] Blueprint bytecode   [x] Default values   [ ] Name pool
  Formats    [x] cpp_sdk   [x] usmap   [ ] ida      [ ] ghidra
             [ ] reclass   [ ] docs    [ ] graphs   [x] json
  Output     ...\build\bin\Release\zircon-out
             [ Dump ]  [ Close ]

  618 packages, 5252 classes, 5598 structs, 2063 enums, 48692 properties, 17698 functions
  cpp_sdk: 620 file(s) -> ...\zircon-out\cpp_sdk
  usmap: 1 file(s) -> ...\zircon-out\usmap
  json: 1 file(s) -> ...\zircon-out\json
```

`cpp_sdk`, `usmap` and `json` are ticked by default. It runs on a background thread, so
the window stays alive and tells you what it is doing; the log at the bottom is what was
actually written.

While a dump is running the browser stops reading the game, and the Properties and Script
tabs say so. That's deliberate: one reader at a time is easier to be certain of than a
lock around every memory read.

### "Is there an installer?"

No, and there shouldn't be. The whole tool is three files in a folder — an installer that
copied them somewhere else would create an uninstall problem without solving a real one.

What you probably actually wanted is `zircon` working from any directory, so:

```
zircon install
```

That adds the folder it's sitting in to **your** PATH. Per-user, no administrator rights,
nothing touched outside your own account. Open a new terminal afterwards and `zircon
detect` works from anywhere.

```
zircon uninstall
```

takes it back off. Existing PATH entries are never rewritten — it appends one entry and
removes that same one, and leaves everything else spelled exactly as it found it.

---

## Get started in four commands

### 0. Build it

You need **Visual Studio 2022** and **CMake**. Nothing else — ImGui and Lua ship inside
the repo.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Everything lands in `build\bin\Release\` — see
[Three binaries](#three-binaries--which-one-do-you-want) for what each one is.

Optional, but worth doing once:

```
zircon install
```

so `zircon` works from any folder instead of only that one.

### 1. Start your game, then find it

```
zircon detect
```

Note the PID of the row scoring highest.

### 2. Check Zircon understands it

```
zircon fingerprint --pid 12345
```

If this prints an engine version and some evidence, everything else will work. If it
can't, see [When something goes wrong](#when-something-goes-wrong).

### 3. Dump it

```
zircon dump --pid 12345 -o game.json
```

Takes a couple of seconds. Add options if you want more in the file:

| Flag | Adds | Costs |
|---|---|---|
| `--script` | decompiled Blueprint bytecode | a second or two |
| `--defaults` | every property's default value | a second or two |
| `--names` | the game's entire string pool | a bigger file |

For the full thing: `zircon dump --pid 12345 --script --defaults -o game.json`

### 4. Turn it into what you actually wanted

```
zircon emit cpp_sdk game.json -o out/
```

Swap `cpp_sdk` for any format from the table above. `zircon emit list` shows them all.

> Prefer clicking? [`zircon-gui.exe`](#can-the-gui-dump-or-does-it-only-browse) does steps
> 1–4 in one dialog. Prefer injecting? [`zircon inject`](#is-zircondll-like-dumper-7s-dll--inject-it-and-it-dumps)
> does them with no interaction at all.

---

## Where the files go

This trips people up, so here it is explicitly. **Nothing is ever written next to the
game.** Game folders are often read-only, and a silently failed write is worse than an
obvious one.

### When you run the CLI

| Command | Writes to |
|---|---|
| `zircon dump` | `dump.json` in the current folder, or wherever `-o` says |
| `zircon emit cpp_sdk … -o out/` | `out\SDK\*.hpp`, one header per package, plus `out\SDK.hpp` that includes them all |
| `zircon emit usmap … -o out/` | `out\<GameName>-Win64-Shipping.usmap` |
| `zircon emit ida … -o out/` | `out\zircon_ida.py` |
| `zircon emit ghidra … -o out/` | `out\zircon_ghidra.py` |
| `zircon emit reclass … -o out/` | `out\<GameName>.reclass.xml` and `.rcnet` |
| `zircon emit docs … -o out/` | `out\index.md` plus one file per package |
| `zircon emit graphs … -o out/` | `out\inheritance.dot`, `.mmd`, and `out\packages\*` |
| `zircon emit json … -o out/` | `out\<GameName>.json` |

Everything else (`detect`, `classes`, `props`, `script`, …) prints to the console and
writes nothing.

### When you dump from the GUI

Into `zircon-out\` **next to `zircon-gui.exe`** by default, one folder per format
(`zircon-out\cpp_sdk\`, `zircon-out\usmap\`, …). The box in the dialog changes it.

### When you inject the DLL

`zircon.dll` writes to a **`zircon-out\` folder next to the DLL itself**:

```
zircon-out\
├── cpp_sdk\SDK\*.hpp      the C++ SDK
├── usmap\*.usmap          the mappings file
└── json\*.json            the full dump
```

So if you put `zircon.dll` in `C:\tools\`, look in `C:\tools\zircon-out\`.

---

## Browsing a live game

```
zircon-gui.exe
```

Pick your game from the list and hit **Attach**. Or skip the picker with
`zircon-gui.exe --attach 12345`. This is the *external* browser — it reads the game from
outside and never loads anything into it. (The injected payload opens this same browser;
see [Three binaries](#three-binaries--which-one-do-you-want).)

![The process picker listing scored processes with Attach buttons](docs/images/gui-attach.png)

```
Attach to a running Unreal Engine process
Scored by what is in the process, not by a list of known games.

SCORE   PID     PROCESS                          PROJECT
 100%   25244   StormEscape-Win64-Shipping.exe   StormEscape        [ Attach ]
  20%   7248    crashpad_handler.exe             crashpad_handler   [ Attach ]
  20%   8368    crashpad_handler.exe             crashpad_handler   [ Attach ]
```

Type in the filter box to find something, click it, and the right pane fills in.

**What you're looking at.** If you select a *class* (like `CharacterMovementComponent`),
you get that class's **default values** — Unreal keeps one master copy of every class, and
that's where the designer-set values live. If you select an actual *instance* of something
in the running level, you get that object's current values, refreshed on a timer.

Handy bits:

- **inherited** — show properties the class inherited from its parents, grouped by which
  class declares them
- **hex** — show integers in hex
- **refresh** — how often to re-read (0.05s to 2s)
- **Pause** — stop reading, so values hold still while you look at them
- **(?)** — hover it to see every offset Zircon derived for this game
- The **Script** tab decompiles the selected function, if it's a Blueprint function
- **Dump…** writes an SDK and anything else you tick, without leaving the window

---

## Comparing two versions of a game

This is the one people don't expect and then use constantly.

```
zircon dump --pid 12345 -o before.json      … game updates …
zircon dump --pid 67890 -o after.json

zircon diff before.json after.json
```

You get a report of everything that changed, graded by severity. Add `--breaking` to see
only the changes that would break code you already wrote:

```
zircon diff before.json after.json --breaking
```

**It exits with code 8 if anything breaking changed**, so you can wire it into a build
script and have it fail automatically. `--style markdown` or `--style json` if you want
the report in a file instead.

---

## Injecting (and when you don't need to)

**Most of the time you don't need to inject anything.** Dumping, SDK generation, the live
browser, reading values, decompiling script — all of it works from outside the process, by
just reading its memory. That's safer: a bug in Zircon can't crash your game.

Injection buys exactly one thing: the ability to *call* the game's own functions. If you
don't need that, skip it.

```
zircon inject --pid 12345
```

This loads `zircon.dll`, which dumps the game from the inside, writes an SDK + usmap +
JSON to `zircon-out\`, and opens the browser in its own window. Close that window to
unload.

> **It refuses to inject into anything with anti-cheat loaded**, on purpose. See
> [What Zircon will not do](#what-zircon-will-not-do).

---

## When something goes wrong

**`no Unreal Engine processes detected`**
The game isn't running yet, or it's still on its splash screen. Wait until you reach the
main menu and try again. Some launchers also start the game as a child process with a
different name — `zircon detect` lists everything it scored, so check the lower rows.

**`fingerprint` says `engine version unknown`**
This is fine and not an error. Plenty of games (studio-modified engines especially) ship
no version string at all. Zircon works out the layout from memory regardless — just
continue to `zircon dump`. Deep Rock Galactic reports `unknown` and dumps perfectly.

**`could not derive the reflection layout`**
This one *is* a real failure. Most often the game hasn't finished loading. Get to the main
menu first. If it persists, run with `-v` and the log will say which stage gave up.

**The SDK doesn't compile**
Report it — that shouldn't happen. Every SDK Zircon emits is checked against a real
compiler, and `static_assert`s are baked into the headers specifically so a wrong offset
is a compile error rather than a silent crash later.

**Enums have no values / a property is an opaque byte array**
That's deliberate. When Zircon can't work something out with confidence, it emits an
honestly-sized blank rather than a plausible guess. A missing name you can see; a member
at the wrong offset you can't.

**The output has no colour**
Check whether `NO_COLOR` is set in your environment. Force it with `--color`.

---

## What Zircon will not do

Three things are out of scope, permanently, and they shaped the design rather than just
the readme:

- **Anti-cheat evasion.** `zircon inject` refuses outright when the target has anti-cheat
  loaded.
- **Modifying games.** Zircon reads. The only writes that exist are the ones the injected
  payload needs for itself, and they're off by default.
- **Non-Unreal engines.**

A concrete consequence: the injected browser opens its **own window** instead of drawing
over the game. Overlaying would mean hooking the game's rendering — writing over code the
game owns — and that's modification. A separate window costs nothing, works whether the
game uses D3D11, D3D12 or Vulkan, and leaves the game's memory untouched.

---

<a id="devs"></a>

# Part II · For developers and reverse engineers

*How it works, why it's built this way, and what you'd need to know to extend it.*

---

## Why it works the way it does

**Nothing is keyed off an engine version number.** Every offset Zircon needs is *derived
at runtime* from a property that is structural to the engine rather than incidental to a
build. That is the whole design, and it is why the tool survives licensee forks, renamed
executables, and engine versions that did not exist when it was written.

Examples of the actual reasoning used:

| Fact | How it is established |
|---|---|
| the object array | the object in slot *i* stores *i* in its own `InternalIndex` |
| `ClassPrivate` vs `OuterPrivate` | follow each repeatedly: class converges to a fixed point (the class of `UClass` is `UClass`), outer terminates at null |
| the name pool | block 0 begins with `"None"`, always the first name interned |
| `PropertiesSize` | `/Script/CoreUObject.Object` must report exactly the `UObject` size that the separately derived object layout implies |
| `Offset_Internal` | a struct's own properties must *tile* it, at distinct offsets, ending inside it |
| `UFunction::Func` | it is the code pointer that points somewhere **different** per function |
| the class default object | it points at an object whose class is *this* class, named `Default__*` |
| what an object *is* | walk its meta-class chain, not its class name |

Every derivation reports a confidence value and the evidence behind it, and every
conclusion lands in the dump header.

### The rule this project learned the hard way

> **A field that scores perfectly on a weak constraint is not the right field.**
> Every derivation needs a *positive distinguishing property* plus an *independent anchor
> with a known-correct value.* Internal consistency alone was wrong every single time it
> was the only test.

That was learned six times, each time from real game data:

| What was selected instead of the real field | Why it scored perfectly |
|---|---|
| the upper half of a vtable pointer, as `NamePrivate` | near-constant per module, so it "resolved" to one valid name every time |
| the high half of a heap pointer, as `PropertiesSize` | constant across objects, so it looked beautifully stable |
| the inheritance depth counter, as `MinAlignment` | small integers look like alignments |
| `PropertyLink`, as `ChildProperties` | non-null for *more* classes, since it includes inherited properties |
| a field that is always zero, as `Offset_Internal` | trivially satisfies `0 <= v < size` for every property |
| a shared thunk pointer, as `UFunction::Func` | 100% "points into executable memory", beating the real field where some entries are null |

The corollary, applied throughout: **never emit plausible-but-wrong output.** An
unresolvable type becomes an opaque byte array of the correct size; an unknown bytecode
opcode stops the walk and says so; an ambiguous signature match is refused rather than
guessed. Losing a name is visible. A member at the wrong offset is not.

UE 5.7 is where that policy paid for itself. It moved three things, one of them the layout
of `UEnum::Names`. Rather than ship a guess, Zircon refused to emit enum values at all —
enums came out with their names, sizes and underlying types and nothing invented — until a
*second* independent 5.7 game confirmed the same shape and it could be derived instead.

---

## What the dump contains

The dump is one JSON file, and it is the contract everything else is built on. Rendering
an SDK, a usmap and an IDA script are all the same operation applied to it, which is why
adding a format never means touching the engine code.

```
Dump
├── header
│   ├── tool version, UTC timestamp
│   ├── engine        version, confidence, property model, name pool shape, evidence
│   ├── source        internal | external | dump | static, process, module, base, size
│   ├── offsets       every offset that was derived, by name and value
│   └── globals       GObjects, FNamePool — module-relative where known
├── names[]           the whole FName pool, when asked for with --names
└── packages[]        "/Script/Engine", "/Game/MyGame/..."
    ├── classes[]  ─┐
    ├── structs[]  ─┤ size, alignment, inherited size, super, interfaces, vtable RVA
    └── enums[]      │
                     ├── properties[]   offset, size, array dim, flags (raw + named),
                     │                  element type, bitfield byte/field mask and bit
                     │                  index, and the CDO default value
                     └── functions[]    flags, params with offsets and sizes, return
                                        value, native RVA, bytecode size, and the
                                        decompiled statements with their offsets
```

Two things in there are worth calling out, because they are what make the file auditable
rather than merely plausible:

- **`header.offsets` records every conclusion the tool reached.** If a member looks wrong,
  you can check the offset it was derived from and the evidence for it, rather than
  trusting the output because it parsed.
- **`header.engine.evidence`** says *why* it decided on a version, in words.

The IR is versioned, round-trips losslessly (`zircon validate` proves it on a real 30 MB
dump rather than a fixture), and is the only thing `emit/` and `diff/` are allowed to see.

---

## How this compares to Dumper-7 and UEDumper

Both are mature, widely used, and good at what they do. This is a comparison of design
goals rather than a benchmark, and the honest summary is that if all you want is a C++ SDK
from an injected DLL, Dumper-7 is a well-trodden path and it works.

Zircon is aiming at a wider surface:

| | Dumper-7 | UEDumper | **Zircon** |
|---|---|---|---|
| How it attaches | injected DLL | external | **injected, external, minidump, or a PE on disk** |
| Primary output | C++ SDK | C++ SDK + live viewer | **8 formats + plugins** |
| Live object browser | — | yes | yes, standalone **and** in-process |
| Bytecode decompiler | — | — | ~90 Kismet opcodes |
| Build-to-build diffing | — | — | 27 change kinds, CI exit code |
| Reads CDO default values | — | — | yes, into SDK, docs and diff |
| Scriptable / extensible | — | — | C ABI + vendored Lua |
| Behaviour when unsure | — | — | **refuses rather than guessing** |

The four things that actually drive the design:

1. **One walker, four providers.** `IMemorySource` is the keystone: the reflection walker
   is written once, and reading a live process, an injected process, a minidump or a file
   on disk are the same code. This is what makes the next point possible.

2. **Cross-provider agreement as a correctness test.** Dumping a game live and dumping a
   full-memory minidump of that same process must produce byte-identical output. Fourteen
   of fifteen targets pass that check. Two real bugs were found by it disagreeing with
   itself — bugs that no amount of "the SDK compiles" would have caught.

3. **Auditability over convenience.** Every offset is reported with evidence and a
   confidence value, and lands in the dump. A wrong offset that nobody can check is worse
   than a missing one that is obvious.

4. **Not only an SDK.** The SDK is a rendering of the IR, and so is everything else. Once
   the reflection data is a real data structure rather than a stream of emitted text, a
   usmap, a diff, an IDA script and a Lua plugin all become the same kind of small program.

---

## Architecture

```
        zircon.exe          zircon.dll          zircon-gui.exe
        (CLI shell)         (injected)          (GUI shell)
              \                  |                   /
               +---- emit/ ---- diff/ ---- engine/ -+
                          \       |       /
                           +---- ir/ ----+
                                  |
                               core/
```

| Layer | Knows about | Never knows about |
|---|---|---|
| `core/` | memory, PE files, patterns | Unreal |
| `engine/` | Unreal reflection | output formats |
| `ir/` | nothing — pure data | everything |
| `emit/`, `diff/` | the IR | memory, targets, the engine |

The CMake graph enforces it: `zircon_ir` links nothing, `zircon_emit` and `zircon_diff`
link only `zircon_ir`.

**`IMemorySource` is the keystone.** Every read goes through one interface, so the
reflection walker is written once and four providers come free:

| Provider | Live objects? | Notes |
|---|---|---|
| `Internal` | yes | injected; the only one that can *call* game functions |
| `External` | yes | `OpenProcess` + RPM, no injection, a bug here cannot crash the game |
| `Dump` | yes, with `MiniDumpWithFullMemory` | offline, reproducible, shareable |
| `Static` | **no** | a PE on disk has empty `.data`; pattern work only |

Injection is a capability upgrade, not the normal path — it is only needed for
`can_call`. Walking objects, reading names and emitting a full SDK all work externally.

### Source layout

```
src/core/      IMemorySource + 4 providers, PeImage, PatternScanner, page cache,
               Injector (payload loading), Term (colour)
src/engine/    EngineProfile, ObjectArray, NamePool, ObjectLayout, StructLayout,
               PropertyLayout, ClassLayout, TypeResolver, FunctionLayout, EnumLayout,
               Kismet, ValueReader, DumpBuilder, UnrealDetect
src/ir/        Model.h (the contract), hand-rolled JSON
src/emit/      one file per output format, plus shared Util
src/plugin/    the host side of the C ABI: node tables, vtable, loader
src/plugins/   plugins that ship with the tool (the Lua host)
include/       zircon/plugin.h — the public ABI, the only header a plugin needs
plugins/       example Lua emitters, copied next to the Lua host at build time
src/diff/      Diff (classification) + Report (text / json / markdown)
src/app/       CLI shell
src/dll/       injected payload — dumps, emits, then opens the browser in-process
src/gui/       Browser (host-agnostic UI) + Host (window, device, frame loop)
docs/          SCOPE.md, ARCHITECTURE.md, PLUGINS.md,
               UE-Test.md (version coverage), ENGINEERING-LOG.md
```

---

## Full command reference

```
detect        list running UE processes, scored with evidence
modules       list modules in any target
fingerprint   engine version and layout, with confidence
names         the FName pool
objects       every UObject full name
classes       classes and structs with sizes
props         properties with offsets, types, bitfield masks
functions     UFunctions with rendered signatures and native RVAs
script        decompile Kismet bytecode to pseudo-code
read          live property values of one object (a class reads its defaults)
inspect       annotated hexdump of one object (the layout-debugging tool)
scan          pattern-scan a module, or the whole process
dump          full reflection dump to IR JSON   [--script] [--names] [--defaults]
validate      parse a dump and verify it round-trips
emit          render a dump to a format         (emit list)
diff          compare two dumps                 [--breaking] [--style json|markdown]
browse        the live object browser
inject        load the payload into a running game

install       add this folder to the user PATH (HKCU only, no elevation)
uninstall     take it off again
```

Targets are interchangeable everywhere: `--pid`, `--process`, `--dump`, `--file`,
`--internal`. `--plugins <dir>` loads emitter plugins; nothing is loaded without it.
Colour follows the terminal, `NO_COLOR`, and `--color` / `--no-color`.

---

## Plugins

A plugin is a DLL exporting one function. It can add output formats, and it can teach
Zircon how to read a target that deriving alone will not reach — an encrypted `FName` pool,
a `GObjects` that cannot be scanned for.

Lua 5.4 is vendored, and the Lua host is itself an ordinary plugin using nothing but the
public ABI. So the shortest useful emitter is a file you drop in a folder:

```lua
return {
  name = "sizes",
  description = "every class and its size",
  emit = function(dump)
    local lines = {}
    for i = 1, #dump.packages do
      for j = 1, #dump.packages[i].classes do
        local class = dump.packages[i].classes[j]
        lines[#lines + 1] = string.format("%-60s %d", class.path, class.size)
      end
    end
    zircon.write("sizes.txt", table.concat(lines, "\n"))
  end,
}
```

`dump` is a live view over the IR rather than a copy, so a script pays only for what it
reads. `plugins/padding.lua` is the example worth reading: it reports bytes in every class
that no reflected property accounts for — something no built-in emitter does — in eighty
lines.

**Fields are addressed by name, not by typed getters.** That is the one design decision in
the ABI worth knowing: adding a field to the IR is then not an ABI change at all, and nodes
become introspectable, which is what makes a scripting binding possible.

**A hook is never trusted.** A global resolver says *where* `GObjects` is; the address
still has to pass "the object in slot *i* stores *i* in its InternalIndex" or it is
dropped and the scan runs. A name decoder's output still has to pass the same length and
printable checks as an ordinary entry. A hook supplies an input — it is never believed
about what is there.

`docs/PLUGINS.md` is the authoring guide.

---

## Default values

`zircon dump --defaults` reads every class property's value out of its class default
object — the one real instance of each class the engine keeps — and records it:

```
float InputYawScale;             // 0x0540(0x0004) = 2.5
float InputPitchScale;           // 0x0544(0x0004) = -2.5
uint8 bEnableTouchEvents : 1;    // 0x054C(0x0001) bit 2, mask 0x04 = true
```

Enum defaults resolve to names (`ECC_Visibility`, not `3`), object defaults to paths
(`/Script/Engine.CheatManager`), structs to their members. They land in the SDK's comments,
in a Default column in the docs, and in the diff — so a patch that moves nothing and
changes how the game behaves no longer produces an empty report.

Structs get no defaults, because a struct has no default object. That is a real absence,
not a missing feature.

---

## Status

All phases P0–P7 complete, verified against fifteen live games from UE 4.22 to UE 5.7.

| | |
|---|---|
| **P0** foundation | four memory providers, PE parser, pattern scanner, page cache |
| **P1** discovery | GObjects, FNamePool, UObject layout — all derived, zero manual input |
| **P2** reflection → IR | UStruct/FProperty/UFunction/UEnum layouts, nested types, bitfields |
| **P3** emitters | cpp_sdk, usmap, ida, ghidra, reclass, docs, graphs, json |
| **P4** diffing | 27 change kinds graded by what they break, migration report |
| **P5** bytecode | ~90 Kismet opcodes, 100% of 1.16 MB decoded |
| **P6** live browser | Dear ImGui, standalone *and* in-process; CDO defaults, live values |
| **P7** plugin API | C ABI + vendored Lua; emitters, FName decoders, global resolvers |
| gap list | CDO defaults, interfaces, property flag names, class vtables — all closed |

Measured on Funnel Runners (UE 5.6):

```
dump          618 packages, 5252 classes, 48692 properties, 17698 functions   1.8 s
defaults      27065 of 27065 class properties read from their CDOs            2.0 s
SDK           619 headers, 56396 static_asserts, compiles clean in 3.8 s
bytecode      2871 functions, 1156611 bytes, 100% decoded
determinism   live process vs a 6.5 GB minidump of itself: 10850/10850 identical
              external vs injected (Internal provider): 10850/10850 identical
```

Those last two lines are the ones that matter: **the same target produces the same answer
from a live process, from a dump file, and from inside the game itself.**

`docs/UE-Test.md` is the version-coverage board — which engine versions have been run,
how far each was taken, and what is left. Fifteen targets verified from UE 4.22 to UE 5.7,
fourteen to full provider agreement. `docs/ENGINEERING-LOG.md` is the record of what broke
along the way and what each failure turned out to mean.

---

## Build and test

Requires Visual Studio 2022 (C++20) and CMake 3.25+. No external dependencies to install:
Dear ImGui and Lua are vendored under `third_party/`, with their licences.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Six test suites, 1469 checks, none of which need a game installed. They run against
synthetic memory, hand-built bytecode and checked-in fixtures.

> If Strawberry Perl or MinGW is on PATH, CMake may pick up its GCC. Pass the Visual
> Studio generator explicitly as above.

**Tests are verified to bite.** Each suite has had deliberate regressions injected to
confirm the right assertions fail — a green suite that cannot fail is worse than no suite.

---

## Scope

**In:** reading and analysing UE reflection data for modding and reverse engineering,
across any engine version from roughly 4.20 to 5.7, including licensee forks.

**Out:** anti-cheat evasion, game modification (Zircon reads; writes exist only for what
the injected payload needs and are off by default), non-UE engines.

That is not a disclaimer, it decided a design. The injected payload opens its own window
instead of overlaying the game, because an overlay means hooking `Present` — writing a
trampoline over code the game owns. `zircon inject` is the plainest possible loader for
the same reason, and it refuses outright when the target has anti-cheat loaded: every
feature except *calling* game functions works externally anyway.

---

## Licence

Apache License 2.0 — see [`LICENSE`](LICENSE). Copyright (c) 2026 TheHolyOneZ.

Dear ImGui and Lua are vendored under `third_party/`, each under its own MIT licence and
each keeping its own copyright. [`NOTICE`](NOTICE) lists them.

---

<sub>[↑ Back to the top](#zircon) · [User guide](#users) ·
[Developer guide](#devs)</sub>
