# Changelog

Notable changes per release. Dates are when the work landed, not when it was tagged.

## 0.5.0 — 2026-09-16

### A game that would not dump, and a worse one that did

Atomic Heart (UE 4.27, Steam) attached cleanly, found 95,130 objects, derived the name pool
and the whole UObject layout, and then stopped:

```
UStruct layout: super +0x48, children +0x50, childprops +0x58, size +-0x1, align +-0x1
[error] reflection is incomplete; cannot produce a dump
```

The first theory was that the dump had been taken while shaders were still compiling. Worth
checking, and the screenshot said 86%, so the first run proved nothing. Waiting it out
changed nothing: same failure, same offsets, main menu, 111 FPS.

**PropertiesSize is anchored on one exact number.** A candidate offset is kept only if
`/Script/CoreUObject.Object` reports a size of `outer_offset + 8` there — i.e. if
OuterPrivate is the last member of UObject. That is true of stock UE and it is the whole
reason the field can be found at all without a version table. Atomic Heart appends to
UObject, so the real figure is 48 where the anchor wanted 40, nothing matched, and the
derivation failed closed.

Failing closed was right. Refusing to emit beats picking a plausible-but-wrong field, and
this is the fourth time that policy has paid for itself. The gap was that a correct answer
was reachable.

Reading the object out of the target settles what the shape actually is:

```
0x30  sizeof(UObject) = 48                       8 more than its members account for
0x30  UField::Next
0x38  FStructBaseChain::StructBaseChainArray     a UE5 structure, in a 4.27 build
0x40  FStructBaseChain::NumStructBasesInChainMinusOne
0x48  SuperStruct   0x50 Children   0x58 ChildProperties
0x60  PropertiesSize = 48   0x64 MinAlignment = 8
```

The chain depth at `+0x40` reads 0 for `Object`, 1 for `Actor`, 2 for `Struct`, 3 for
`Class`, which is `NumStructBasesInChainMinusOne` and nothing else.

Neither neighbour rescues the anchor, incidentally. Working back from SuperStruct assumes
`Next` is right before it, and the base chain sits in between, so that overshoots by 16.

The exact figure is tried first, so every target that already worked takes the identical
path. Only when it finds nothing does the fallback run, anchored on things that do not care
where UObject ends: the size is at least what the members account for, it is 8-aligned, no
class is smaller than the root, and the next dword reads as a real alignment. A build that
turns out to extend UObject says so in the log and in the dump header rather than passing
quietly.

### The same assumption, one file over, quietly truncating every dump

With PropertiesSize fixed Atomic Heart dumped: 4,763 classes and **1,640 functions**. For a
game that size that is far too few, and it lints clean, because nothing in the linter knows
how many functions a game ought to have.

`UField::Next` is found by walking `UStruct::Children` and keeping the offset whose chains
"stay in the object array and terminate". A field that reads null everywhere terminates
every chain immediately and satisfies that perfectly — and the scan took the first offset
that passed, starting from the same stock `sizeof(UObject)` that had just been proved wrong
for this build. It settled on a field that links nothing, every Children list came out one
entry long, and no warning was printed.

Chains have to actually link now, and the offset that links the most wins. There is a proper
anchor available too: UField is a UObject plus one pointer, so Next sits exactly at
`sizeof(UObject)` — which by then has been read out of the engine rather than assumed. Same
game, same process: **1,640 -> 13,368 functions.**

This is the more serious of the two. The first bug refused to produce anything; this one
produced something wrong and called it clean.

### Nothing else moved

Both changes are in the derivation every target depends on, so "it works on the new game" is
not evidence. Six games were dumped twice on the same running process, once with 0.4.0 and
once with this build, and diffed:

| target | engine | types | |
|---|---|---|---|
| HRDINA | 4.22 | 3,172 | byte-identical |
| Nightmare Kart | 4.25 | 3,745 | byte-identical |
| Peepo Island | 5.0 | 6,059 | byte-identical |
| Mizeria | 5.2 | 6,370 | byte-identical |
| Ready or Not | 5.3 | 10,731 | byte-identical |
| Backrooms: Escape Together | 5.7 | 13,599 | byte-identical |

43,676 types across both property models and both name pools, `no differences` on every one.
Atomic Heart goes from exit 4 to 392 packages, 4,763 classes, 33,300 properties, 13,368
functions, and `validate --strict` clean.

### Smaller

- An unresolved offset used to print as `+-0x1`, which reads like a real offset and sent at
  least one person looking in the wrong place. It says `unresolved` now.
- The end-to-end check on `/Script/CoreUObject.Object` treated `outer_offset + 8` as the
  size rather than the floor, so it would have failed a fork even once the derivation
  handled one. It compares against the derived size now, and a fork earns slightly less
  confidence than a stock build rather than being called broken.

## 0.4.0 — 2026-09-15

### The GUI was showing you memory from whenever it first looked

This one took a bug report to find, because nothing on the fixture side can catch it and
the CLI is immune by accident.

Every provider gets wrapped in `MakeCached` — a 64 MiB direct-mapped page cache, without
which External mode is roughly two orders of magnitude too slow for a full walk. It has
exactly one invalidation path: `Write` drops the pages it just changed. Nothing else, ever.
No age, no generation, no way for a caller to ask for fresh bytes.

That is correct for the CLI, which opens the process, derives, walks and exits inside a few
seconds. The target barely moves in that window. The GUI builds its cache once at attach
and keeps it until detach, which makes the same code mean something else entirely:

- `RefreshRows` re-reads the selected object's properties on a timer, so the refresh slider
  decided how often to re-read *the cache*. A value whose page nothing happened to evict
  sat there looking live and never moved. The live object browser was not live.
- A dump started later walked the object graph through pages put there by browsing. Mixing
  bytes from two moments in a running target is not a small error: UE recycles objects
  across a GC, so an `Outer` pointer cached before a level change resolves afterwards
  against whatever took that memory.

That last one is what got reported — Blueprint classes claiming a package that belongs to
an unrelated asset. Reproduced it on Funnel Runners (UE 5.6) by doing what a person does:
attach, browse a few hundred `BP_SqWaterTower_Destr` objects, load into a match, dump.

```
  BP_SqWaterTower_Destr_C   package: <0x30a05030605020c>
  BP_MobileArea_M_C         package: /Game/AdvancedPhotoMode/Textures/Keyboard/T_Keyboard_R
```

14 `_C` classes with a package that isn't theirs, 8 of them not even a name. 31 properties
with no name at all. 3 array dimensions in the hundreds of millions. Same build, same game,
same clicks, with the fix: 0, 0, 0.

`IMemorySource::Invalidate()` now exists — a no-op everywhere except the cache, which drops
every tag. The GUI calls it before a dump, before a reindex, and on every value refresh.
Dropping 64 MiB per refresh tick sounds expensive and isn't: a few hundred properties is a
few hundred page reads, four times a second, which is what an uncached read would have cost
all along.

The ENGINEERING-LOG note about an enum default reading as `3` through the dump path and
`ECC_Visibility` through the live path was the same thing, a year of confusion earlier.

Still open, and separate: the GUI sizes its object table from `num_elements` as it was at
attach. Objects created since then aren't in the table, so Reindex can't find them. Nothing
reads wrong, there's just less of it.

### `zircon publish` — dumps go to Zdex without leaving the terminal

Zdex indexes Zircon dumps and makes them browsable, searchable and diffable. The whole path
is four commands:

```
zircon login                     paste an API key; stored in %APPDATA%\Zircon\config.json
zircon publish dump.json         gzip, chunk, upload, wait for indexing, print the URL
zircon fetch 42 --usmap          pull a published dump's mappings or SDK back down
zircon logout                    forget the key
```

`login` takes an API key, not an account. There is no browser flow and no OAuth; the key is
the credential, it lives in one file, and `logout` deletes it. It never goes in a dump, a
log line, or anything in the project.

A dump is offered for publishing right after `zircon dump` writes it, as a line you can
copy. Nothing uploads on its own.

Underneath: a DEFLATE encoder (fixed Huffman, LZ77 with hash chains) written for this, since
pulling in zlib for one job is not worth the dependency. Verified byte-identical against
Python's zlib across 15 shaped cases plus a real 59 MB dump — 17.3x at 133 MB/s. Files
under 64 KB skip compression, because a 971-byte dump compresses to 92 bytes and the server
rejects anything that small before it can tell you *why* it's rejecting it.

Chunked and resumable, so a dropped connection resumes instead of restarting. HTTPS through
WinHTTP with a real User-Agent, because Cloudflare answers generic ones with a plain-text
`error code: 1010` that is not JSON and does not explain itself.

Rate limits are reported, not waited out. The upload quota returns `Retry-After` in the
region of most of an hour, and a CLI that blocks silently for 54 minutes looks like it has
hung. Anything over two minutes prints the server's own message and exits.

### The GUI publishes too

A **Publish...** button next to Dump..., because telling someone who is already looking at
the dump they just made to go and open a terminal is silly. It guesses the file from the
last dump folder and the game from the attached process, asks for a build label, shows the
same phases the CLI prints, and ends with the URL and an Open on Zdex button. If no key is
stored it takes one there rather than sending you to `zircon login` -- same file either way.

The flow underneath moved into the zdex library as `zdex::Upload()`: compress, init or
resume, send the chunks, finish, poll. Both the CLI and the GUI call it and supply their own
hooks for showing progress. Writing the orchestration out a second time in the GUI was the
obvious shortcut, and this project already has a scar from exactly that -- the
`ClassifyObject` bug in 0.3.0 was the same mistake made twice, one file apart, and only one
copy got fixed the first time.

### Resume never resumed

Uploads are resumable, and the resume had never once fired. The record that says "this file
has a half-finished upload" was keyed on a fingerprint of the file *being sent* — which, for
anything large enough to compress, is a temporary `.gz` written fresh on every run. New file,
new timestamp, new fingerprint, no match, start over.

Nothing failed, which is why it sat there: an interrupted publish re-ran and worked, just
from the beginning and against a second upload session.

Keyed on the dump the user actually named now. That's the right question anyway — the
encoder writes `mtime 0` into the gzip header specifically so the same dump always
compresses to the same bytes, so the source identifies the payload. Resuming also checks the
stored payload length before continuing, rather than splicing chunks from two different
compressions together if that ever stops being true.

### Smaller

- `ParseJson` accepted `error code: 1010` as the number 0, because the number scanner took
  a leading `e`. It wants a digit after the optional sign now. Cloudflare's plain-text 403
  was parsing as valid JSON and the error came out empty.
- `zircon --help` covers `login`, `logout`, `publish` and `fetch`.

## 0.3.0 — 2026-09-14

### Two engine bugs the new linter found on a real game

Built `validate --strict` against fixtures, then pointed it at a 59 MB dump of Funnel
Runners (UE 5.6). **176 errors.** Both causes real, and both wrong since before 0.2.0.

**489 object properties and 43 struct properties had no type at all.** `TypeResolver`
checks that a pointer really is the kind it expects before accepting it, which is fine, but
it was checking by comparing the target's class *name*:

```cpp
if (GetClassName(...) != expected_kind) return {};      // "Class", "ScriptStruct"
```

A Blueprint class's class is `BlueprintGeneratedClass`. A Blueprint struct's is
`UserDefinedStruct`. A widget's is `WidgetBlueprintGeneratedClass`. None of those match the
string, so anything pointing at a Blueprint type came back untyped.

`ClassifyObject` already exists for exactly this — it walks the meta-class chain — and
`StructLayout.h` carries a comment saying so, including that the same mistake had been made
once before and only turned up when the decompiler found functions the dump didn't have.
Made again anyway, one file over. Uses `ClassifyObject` now.

532 references came back. Checked that it's a pure gain and not a shuffle: **0 offsets
moved, 0 sizes changed, 0 properties renamed to anything else** — every difference is an
empty type becoming a named one. `IndicatorWidget` on `BP_TornadoTracker_C` is a
`W_TornadoIndicator_C` now instead of a bare pointer.

**133 enumerators didn't fit their own enum's declared width.** `Enum::underlying` comes
from how the enum gets *used* — an `FEnumProperty` names its underlying property. An enum
nothing uses as a property has no usage to read, so those fell back to `uint8`.
`ETransformGizmoSubElements` goes up to 524287.

0.2.0 patched the visible half by widening at emit time so the SDK would compile, and left
the IR still saying `uint8`. Fine while the built-in emitters are the only readers, wrong
for a plugin, a script, or either of the two new emitters below. The values come straight
out of `UEnum::Names`, so when the width disagrees with them it's the width that's wrong.
`DumpBuilder` widens it now. 22 enums on that game, **0 enum values changed**.

Same dump afterwards: 0 errors, 0 warnings.

### The SDK was throwing 27,334 compiler warnings

Found the same way, by compiling it rather than trusting the last measurement. Every type
in the SDK is declared `struct`, and every elaborated specifier in a signature or parameter
block said `class`:

```cpp
inline void UVariant::SetThumbnailFromCamera(class UObject* WorldContextObject, ...)
                                             ^^^^^ but `struct UObject { ... }`
```

MSVC raises C4099 on every one. 27,334 on a UE 5.6 game, which buries anything the compiler
actually wants to tell you about the cheat including the header. No errors and no layout
problem, so "0 errors" stayed true the whole time and "0 warnings" quietly stopped being.
The compile test greps for `error`, which is how it slid.

Seven sites plus one hand-written `class UClass*` in `TSubclassOf`. `/W3` is back to
**0 errors, 0 warnings** over 622 headers and 56,396 `static_assert`s. `/W4` leaves 352,
all C4458 — a wrapper parameter shadowing a member of its own class, which is UE's naming,
not ours to rename around.

### `--process` wouldn't take a name two other commands already took

`zircon detect` prints `StormEscape` in the PROJECT column. Paste that into `--process` and
you got `no running process named 'StormEscape'`, because it matched the whole file name
and nothing else. `inject` had always taken a substring. So two commands accepted a name
the rest of the tool threw out.

Exact match still wins, so a process can't get shadowed by one whose name contains it.
Ambiguous substrings are still refused, and the refusal lists the candidates now instead of
just their pids.

### Ran the whole thing against a live game

Funnel Runners (UE 5.6), the target the README numbers come from. Every binary, every
provider, all eleven formats:

```
dump --emit all     618 packages, 5252 classes, 48692 properties, 17698 functions   13 s
validate --strict   10850 types, 48692 properties, 2063 enums   0 errors, 0 warnings
cpp_sdk             622 headers, 56396 static_asserts, /W3 0 errors 0 warnings
external vs internal (injected)          10850 / 10850 types byte-identical
external vs a 7 GB full-memory minidump  10850 / 10850 types byte-identical
```

All three dumps lint clean and diff to `no differences`.

Drove the GUI end to end too: attach, filter, select, live values, **Allow edits**, Dump
dialog. It was already listing `binja`, `frida_js` and `python_stubs` without a line of GUI
code changing, because it renders the emitter registry. Dumped all three from it and
they're all valid — the binja script byte-compiles, 619 stub modules parse, the Frida
module loads in node at 9.9 MB and 10850 types in 181 ms.

Checked the writes from outside the process: set `MaxWalkSpeed` to 1337 in the GUI and read
it back with `zircon read` from a separate process, then flipped
`bUseSeparateBrakingFriction` through `zircon write` with the other six bools in byte
`0x02E8` left alone.

### Three more emitters: `binja`, `frida_js`, `python_stubs`

`frida_js` and `python_stubs` have been marked deferred since P3, and Binary Ninja sat in
the scope table next to IDA and Ghidra without anyone writing it. Eleven formats now.

**`binja`** — a Binary Ninja Python script. It doesn't spell out a `StructureBuilder`
sequence per type the way you would by hand. On a real dump that's a 40 MB `.py` that takes
minutes to import, and it's the same six lines copied fifty thousand times. It emits a data
table and one loop that reads it:

```python
("AActor", 680, 8, ((0,"baseclass_0","s","UObject",1),(40,"RootLocation","s","FVector",1), ...
```

The script reserves every type name at its final width first and fills members in after, so
a named reference always resolves. That's also why it needs no topological sort and raises
none of the cycle warnings the C-emitting backends have to — UE dumps have cycles in them
and here they cost nothing.

**`frida_js`** — one `zircon.js` for `frida -l`. Half data, half runtime:

```js
var actor = Zircon.wrap(ptr('0x...'), '/Script/Engine.Actor');
actor.MaxSpeed;          // reads through the derived offset
actor.MaxSpeed = 1337;   // writes it
actor.bHidden = true;    // one bit, not the byte the other six bools share
```

`wrap` walks the super chain so you don't have to qualify an inherited property. Enums read
and write by name. Strings, maps, sets and delegates get refused rather than written — same
rule as everywhere else, their memory has allocator state sitting next to the value.

The data goes through `JSON.parse` instead of being a JS object literal. V8 has a faster
path for JSON and it's worth seconds on a big game. It's escaped into a string literal that
closes and reopens at every type, so you still get one type per line rather than a single
30 MB line no editor will open.

`Zircon.base` is lazy. Grabbing Frida's `Module` at load time would make the file
unloadable anywhere else, and asking it for an offset from a build script is a reasonable
thing to want.

**`python_stubs`** — `.pyi` stubs, one module per UE package, for driving a game from
Python. Completion on every class, `__offsets__` as a class-level dict, enums as real
`IntEnum`s, the super chain as Python inheritance so inherited attributes resolve the way
they already do.

Stubs, not importable modules. Nothing here ever executes, which is the only reason the
circular imports between packages are fine — UE's dependency graph has cycles and nothing
would untangle them.

`None` is an enumerator in `EGizmoElements` and a keyword in Python, so it comes out
`None_`. That guard was in two places and neither test could fail while the other stood.
Strips the `EFoo::Bar` leaf *before* sanitising now instead of after, so there's one guard
and removing it fails two assertions.

### `validate --strict`

Every derived offset already comes with evidence and a confidence value. Nothing was
reading the finished dump back afterwards and asking whether the pieces fit together, and
that's a different question — it's where a derivation going wrong in a new way turns up
first.

```
> zircon validate game.json --strict
round-trip       lossless
checked          10850 types, 48692 properties, 2063 enums

CHECK                        COUNT  SEVERITY
property-overlap                 2  error
dangling-type-ref              118  warning

warnings        118
errors            2
```

About twenty checks: members overlapping or running past the end of their class, two
non-bitfield properties at one offset, two bools claiming the same bit, an enum too narrow
for its own values, a super or property type missing from the file, a function with two
return values.

Error vs warning splits on whether one file can prove it. A member at 400 sitting inside
one that runs to 408 is a contradiction — error. A dangling super isn't; a `--filter`ed
dump has no ancestors to point at and is still perfectly good.

Exit code `9`, not `6`. `6` already means the file wouldn't parse, and a build script wants
to tell that apart from "parsed fine and contradicts itself".

The enum width check is the C4369 thing from 0.2.0, turned into something that fires now
rather than as compiler warnings three steps later that nobody reads.

Lives in `ir/Lint.h` next to the JSON reader, for the same reason: pure function of the IR,
so no game needed and the tests need nothing beyond a struct literal.

### `zircon xref`

The thing a header can't tell you: if I change this, what's looking at it?

```
> zircon xref game.json -f CharacterMovementComponent
extended by (14)
held by (31)
passed to (3)
references        48
```

Inheritance, interfaces, properties and function parameters, including through containers —
a `TMap<FName, TArray<AActor*>>` counts as a reference to `AActor`, not to nothing.
`--uses` runs it the other way.

A leaf name resolves when there's one match; when there isn't you get the candidates rather
than a silent pick. Exit `3` when nothing points at it, same as `find`.

### Several formats at once, and `dump --emit`

`emit` takes a comma-separated list, and `all`:

```
zircon emit cpp_sdk,usmap,frida_js game.json -o out/
zircon emit all game.json -o out/
```

A single format still writes straight into `-o`. Several get a subdirectory each, or `docs`
and `graphs` write over one another — same layout the GUI and the payload already used. One
format refusing doesn't stop the rest; asking for eleven and losing ten because one wanted
objects is no use to anyone.

Bad format names all get reported, not just the first. Mistype two of five and you want
both now.

`dump` can render as it writes:

```
zircon dump --pid 12345 --script --defaults -o game.json --emit cpp_sdk,usmap
```

Output lands next to the dump, not in the cwd, and the format list is resolved *before* the
walk — finding out after seventy thousand objects that you mistyped a name is a bad trade.

### `zircon browse` was in the help and wasn't a command

It's listed in `--help`, in the README's command reference, and in the README's own
examples. Running it printed `'browse' is not a command`.

Starts `zircon-gui.exe` from next to the executable now, forwarding `--pid` or `--process`
as the browser's `--attach`. `--dump` and `--file` get refused with a reason, since the GUI
attaches externally and has nothing to browse in either. Doesn't wait on the window, which
would make it useless from a script.

### Fixes

- `arg[0]` on a `std::string_view` that can be empty, which is UB. You get an empty argv
  entry from any shell that expands a variable to nothing. Three call sites, one
  `IsPositional` helper.
- The injected payload prints every warning where the CLI collapses identical ones with a
  count. 21 identical `usmap` lines in `zircon.log` tell you nothing one line with a count
  wouldn't. Left alone for now, written down so it doesn't get rediscovered.
- `emit list` had its format column ten wide, so `python_stubs` knocked the rest of its row
  out of line.
- `-p/--pattern`, `-m/--module` and `--all-regions` are all accepted by the parser and were
  missing from `--help`.

### Tests

1600 checks across seven suites, up from 1537.

`tests/test_emit_usmap.cpp` is `tests/test_emit.cpp` — it's covered reclass for a while and
now covers three more formats, so the name had stopped being true.

The binja test parses the emitted table back and checks the members *tile* the struct with
no gap and no overlap, instead of matching golden text. A gap puts every member after it at
the wrong address and nothing in Binary Ninja would tell you. Verified all three bite:
padding emitted one byte wide instead of its real width fails two assertions, dropping the
bitfield mask from the Frida blob fails one, removing the keyword guard from the stubs
fails two.

Also ran the Frida runtime outside Frida, over a `Buffer` with a NativePointer stand-in:
fifteen checks covering the bit-preserving write, enum round-trip by name, nested struct
offsets, an inherited property read through a derived class, and `FString` refusing.

The linter has ten tests, each starting from a dump it passes and breaking exactly one
thing. First version of the overlap test asserted a count of one and got two — a member
wide enough to overlap `Health` also reaches the packed bools behind it. The cascade is
right, so the test uses a width that hits exactly one member and the count means something.

And then it found two real bugs the first time it saw a shipped game, which is the only
result in this release nobody designed in advance.

---

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
