# UE version coverage

What Zircon has actually been run against, what it has not, and what is missing entirely.

This file is the record of which engine versions have actually been run, and why each is
interesting. This file is the **status board**: what is proven, what is merely present,
and what to work through next.

---

## What "tested" means here

"It works on that game" is not a claim worth making without saying how far it got. Six
levels, each one a thing that either happened or did not:

| Level | Means | How you know |
|---|---|---|
| **L0** detected | recognised as Unreal from the binary on disk | `zircon fingerprint --file <exe>` |
| **L1** discovery | GObjects, FNamePool and the UObject layout all derived from a live process | `zircon objects --pid N` lists real names |
| **L2** reflection | full dump, and it round-trips losslessly | `zircon dump` then `zircon validate` |
| **L3** SDK | the generated C++ SDK compiles with **zero** errors | `cl /c` over `SDK.hpp` |
| **L4** script | Kismet bytecode decompiles, with the undecoded fraction measured | `zircon script --pid N` |
| **L5** agreement | external, minidump and injected providers produce byte-identical dumps | `zircon diff a.json b.json` |

L5 has a precondition that the others do not: the target's object graph has to hold still.
Capturing a full-memory minidump of a large game takes minutes, and a game that is still
streaming creates and destroys objects the whole time, so the two providers are reading two
different states and the comparison says nothing about either. That is a property of the
target, not a result — see Subnautica 2 below.

L3 is the one that matters most. Every member carries a `static_assert` on its offset, so
a compiling SDK is tens of thousands of independent checks that the layout is right — not
a claim that output was produced.

---

## Status

| Target | Engine | Level | Notes |
|---|---|---|---|
| **HRDINA** | **4.22** | **L5** | `TNameEntryArray`; the last unimplemented pool |
| **We Went Back** | **4.23** | **L5** | `UProperty` era; the second property system |
| **Nightmare Kart** | **4.25** | **L5** | first `FProperty` version |
| **Pseudoregalia** | **5.1** | **L5** | version corroborated by its own modding toolchain |
| **Peepo Island** | **5.0** | **L5** | needed the Offset_Internal threshold fix |
| **Mizeria** | **5.2** | **L5** | almost pure C++; no Blueprint script to decompile |
| **Dark Pals: The 1st Floor** | **5.5** | **L5** | brackets the FProperty change from below |
| **Funnel Runners** | **5.6** | **L5** | the original reference |
| **Subnautica 2** | **5.6-era** | **L4** | licensee-branded; largest target. L5 is not reachable on it — see below |
| **Backrooms Escape Together** | **5.7** | **L5** | newest engine; moved three things, see below |
| **Motorslice** | **5.7** | **L5** | the second 5.7 sample; it is what closed the enum gap |
| **RV There Yet** | **5.6-era** | **L5** | licensee-branded, no UE version string anywhere |
| **Little Nightmares Enhanced** | **4.27** | **L5** | first UE4 target ever run |
| **Deep Rock Galactic** | **4.27-era** | **L5** | no version string at all; era identified from layout |
| **FF7 Rebirth** | **4.26** | **L5** | Square Enix fork; needed three real fixes |
| Atomic Heart | 4.27 | L0 | heavily customised renderer |
| Little Nightmares III | 4.27 | L0 | launcher exe separate from shipping exe |
| Ready Or Not | 4.27 / 5.x | L0 | no version string |

**Fifteen targets verified, UE 4.22 through UE 5.7 — every engine era Zircon claims to
support, every one implemented, and no known gap left. Fourteen reach full provider
agreement; the fifteenth is the one target where L5 cannot mean anything, for reasons that
are the game's and not the tool's.**

### Measured

```
                        objects   props    SDK headers  asserts   errors  bytecode
HRDINA          4.22      52238   13795        189       14934       0    100.00%
We Went Back    4.23      82254   18829        213       20236       0    100.00%
Nightmare Kart  4.25      n/a     21023        394       22606       0    100.00%
Peepo Island    5.0       n/a     23303        229       26953       0    (no script)
Pseudoregalia   5.1       n/a     23372        233       26933       0    100.00%
Subnautica 2    5.6e      n/a     96115       3451      110426       0    100.00%
Backrooms       5.7       55986   48525        516       58984       0    100.00%
Motorslice      5.7       64787   42383        631       49377       0    100.00%
Dark Pals       5.5       40619   32596        388       38924       0    100.00%
Mizeria         5.2       n/a     23268        143       27144       0    (no script)
Funnel Runners  5.6       72562   48692        619       56396       0    100.00%
RV There Yet    5.6e      51256   41609        661       49186       0    100.00%
LN Enhanced     4.27      40679   22755        246       25114       0    100.00%
Deep Rock Gal.  4.27e     81242   37616        958       43652       0    100.00%
FF7 Rebirth     4.26     207123   48893        948       56718       0    100.00%
```

L5 agreement (external dump vs a full-memory minidump of the same session):

```
Funnel Runners   10850 / 10850 byte-identical      (also external vs injected)
RV There Yet     10672 / 10672 byte-identical
LN Enhanced       4680 /  4680 byte-identical
FF7 Rebirth      10591 / 10591 byte-identical
We Went Back      3511 /  3511 byte-identical
HRDINA            3172 /  3172 byte-identical
Pseudoregalia     5988 /  5988 byte-identical
Peepo Island      6060 /  6060 byte-identical
Mizeria           6370 /  6370 byte-identical
Backrooms        13599 / 13599 byte-identical
Motorslice       10291 / 10291 byte-identical
Dark Pals         9118 /  9118 byte-identical
Nightmare Kart    3726 /  3726 byte-identical
Deep Rock Gal.    8365 /  8365 byte-identical
```

### Subnautica 2 is the one target L5 cannot judge

Its live and minidump dumps differ, and the difference is real — but it is the game, not the
providers. Two dumps taken from the **same** live process two minutes apart differ as well:
7204 structs against 7186, 71933 properties against 69290. Every differing type is in
`/Engine`, all of them runtime-generated `PropertyBag_*` transients that the game creates
and discards while it streams.

The minidump itself is complete: 201840 of 201840 objects resolve from it, with nothing
unreadable. There is simply no single state to compare, because writing 10.9 GB takes
longer than the game takes to change. The other targets are quiescent at their menus, which
is why they compare cleanly.

So this is a limit of what L5 can mean for a streaming game, not a defect — and the diff
reporting those transients as removed is it doing its job.

### What the offsets prove

Nothing here is a version table. The same derivation produced genuinely different
layouts, and the UE4 targets agree with each other while differing from the UE5 ones:

```
                    FProperty next   name    offset   CDO     FVector
UE5 (5.6, RV)            +0x18      +0x20    +0x44   +0x110   24 bytes
UE4 (4.27, 4.26)         +0x20      +0x28    +0x4c   +0x118   12 bytes
```

Deep Rock Galactic and RV There Yet publish no engine version at all, and both were
placed in the right era purely by what their memory looked like.

That split is not the UE4/UE5 boundary, either. Mizeria on 5.2 has the old shape
(`next +0x20`, `offset +0x4c`) and Dark Pals on 5.5 has the new one (`next +0x18`,
`offset +0x44`) — so `FProperty` was rearranged somewhere in 5.3 or 5.4, a change no
release note mentions and nothing in either binary announces. Both shapes are derived and
both are verified at L5, which is the point: the tool did not need to be told where the
boundary was, and would not have been any worse off if it sat somewhere else.

### Five bugs found by doing this

**Bytecode vector precision was inferred from the version string.** RV There Yet has no
version, so `profile.major` was 0 and the decompiler chose UE4 float widths on a UE5
engine. `EX_VectorConst` then read 12 bytes where 24 were written, which does not fail
loudly — the instruction stream desynchronises and every later operand is read at the
wrong offset. 316 functions came back with wrong arguments before giving up. Now measured
from `/Script/CoreUObject.Vector`, which is 12 bytes on UE4 and 24 on UE5 and says so in
the reflection data. 93.95% → 100%.

**The FName pool stride was assumed to be 2.** FF7 Rebirth is a case-preserving build, so
its `FNameEntry` carries a `DisplayIndex` and aligns to 4. With the stride assumed every id
still resolved to *something*, because byte offset `id*2` lands inside the block either
way — so the pool looked like it worked while returning the wrong names, and the only
symptom was the UObject layout failing its end-to-end check with no reason given. Now
derived by walking a block under each candidate: the wrong alignment lands mid-entry within
a few steps and reads a length that is not a length. Its entries also reserve a byte past
their characters, so the advance rule is derived alongside the stride.

**`block_offset_bits` was a hardcoded 16.** Also derived now, by encoding a block's entries
back into ids and requiring them to round-trip. It measures 16 everywhere so far — but it
is measured, and only blocks past the first can settle it, which is precisely why a wrong
value would survive casual testing and then corrupt the long tail of a dump.

Those three were the same mistake: a constant that is usually right, asserted rather than
observed. The two engine eras only disagree about them in builds that do not advertise
what they are.

The remaining two came out of chasing what looked like a fourth — see below. Neither was a
version problem at all: one was a counter that reported free array slots as failures, and
one was the diff being unable to recognise a dump as equal to itself.

### The FF7 "25% of names missing" was a measurement artifact

Chasing it found two more things, neither of which was a name problem.

**The missing objects were empty slots.** 52 421 of the 52 749 unresolved sat in one
contiguous run, and reading any of them gives "no object in slot": they are free entries in
GObjects, which is what `NumElements` being a high-water mark means, and what a level
unloading leaves behind. The objects either side of the run resolve perfectly.
207 337 − 52 749 = 154 588, and 154 588 resolved. **Every object that exists resolves.**

`zircon objects` was reporting "154588 of 207337", counting array capacity as if it were
objects, and that number is what made this look like a failure. Empty slots and unresolved
names now count separately, because one is normal and the other is a defect.

**The diff could not recognise a file as equal to itself.** With names fixed, L5 still
reported a critical change — and so did diffing two live dumps twenty seconds apart, and
so did diffing one dump against *itself*. FF7 ships a Blueprint class with two properties
both named `Light`, at different offsets and of different types. The diff keyed properties
by name, so the map kept only the last and the first compared against the wrong member.

Functions had been fixed for exactly this in P4 and properties had not; nothing had ever
shipped a duplicate property name before. A diff tool inventing a critical change out of
two identical files is the worst failure it has available, since its whole job is to be
believed when it says a build changed. Matching is by name *and* occurrence now, and the
test asserts a dump equals itself, that duplicates are not reported as added or removed,
and that a real change to the second duplicate is still caught.

---

## Coverage by engine band

| Band | Structurally different because | Code path | Verified |
|---|---|---|---|
| **≤ 4.22** | `TNameEntryArray` name pool, `UProperty` | implemented | **4.22 at L5** |
| **4.23 – 4.24** | `FNamePool` arrives; still `UProperty` | implemented | **4.23 at L5** |
| **4.25 – 4.27** | `FProperty` / `FField` split lands | implemented | **4.25, 4.26, 4.27 all pass** |
| **5.0 – 5.3** | `TObjectPtr`, `FNamePool` unchanged | implemented | **5.0, 5.1, 5.2 pass** |
| **5.4 – 5.5** | licensee branding common, IoStore | implemented | **5.5 at L5** |
| **5.6 – 5.7** | current | implemented | **5.6 and 5.7 both at L5** |

---

## Every era is implemented

Both property systems and both name pools are supported. Nothing in the 4.20 – 5.7 range
is now refused for want of a code path.

### `UProperty`, UE ≤ 4.24

Detected structurally, from the *absence* of `UStruct::ChildProperties`. A `UProperty` is a
`UObject`, so its class pointer, name and identity are already where every `UObject` keeps
them; what was new is that `Children` mixes properties with functions and enums, so entries
are picked out by ancestry — a property is anything whose class reaches
`/Script/CoreUObject.Property`, an object found by name. Those classes are collected once
into a sorted set so the per-entry test is a binary search.

`is_property_field` also had its test **backwards** for this era: an `FField` lives outside
the object array, a `UProperty` inside it, and requiring "not in the array" made every
container resolve its element type to nothing.

### `TNameEntryArray`, UE ≤ 4.22

A different shape rather than different offsets. `FNamePool` packs entries end to end
inside blocks and addresses one by (block, byte offset); `TNameEntryArray` is a two-level
table of *pointers*, so an id is (chunk, index) and the entry can be anywhere. Entries
carry no length header and are NUL-terminated, so nothing about reading one is shared —
which is why the two are told apart where the pool is found rather than anywhere
downstream.

Three things had to be measured rather than assumed, and all three were wrong on the first
real target:

- **Where the characters start.** HRDINA puts them at `+0xc`, not the `+0x10` the obvious
  reading of the struct gives.
- **Which encoding.** The entries seen while searching were UTF-16, so an ANSI-only anchor
  found nothing at all and gave no hint why.
- **How the table is reached.** Classically `FNameEntryArray` holds its chunk pointers
  inline, so the global *is* `Chunks[0]`. HRDINA allocates the chunk array on the heap and
  keeps only a pointer to it in the image — one more hop, and looking for only the first
  shape finds nothing. Both are tried now.

The anchor alone was not enough here either: the first candidate accepted resolved id 0 to
`"None"` and then produced `"r2DFloat"` and `"otify"`, having matched a fragment of
unrelated memory. It is rejected now by requiring the *table* to work — walking the first
96 ids must yield whole, distinct, printable names.

## UE 5.7 moved two things

Neither failed gracefully, and the first one stopped the tool dead.

**`FUObjectItem` puts the flags first and the `UObject*` at `+8`.** It had been the first
member of the item through every engine up to 5.6, so reading an item *as* a pointer
worked and nothing needed to know better. On 5.7 every slot reads as a flags word, no slot
stores its own index, and the array is simply never found — the search reports nothing and
gives no hint that one offset is the whole problem. The offset is derived now: 0, 8 and 16
are each tried, and the slot test decides.

While finding that, the element and chunk counters turned out to be worth deriving too.
They sit together as MaxElements, NumElements, MaxChunks, NumChunks, but their *address*
had been assumed at `+0x10`. Their order and relationship identifies them — a count inside
its own maximum, and the chunks needed for NumElements not exceeding NumChunks — so the
window is searched rather than fixed.

**`UEnum::Names` was split into two arrays.** Through 5.6 a UEnum holds one
`TArray<TPair<FName, int64>>`, names and values interleaved. 5.7 keeps the names in one
array and the values in another, each reached through a pointer whose **low bit is set as a
tag**, with the entry count after them.

This is the one that needed a second target. With only Backrooms there was no way to tell
an engine change from one studio's fork — and Backrooms is a `CL-0` build with no patch
label, which made that worse. The first attempt, widening the existing search to more
strides and a larger probe window, found nothing and was reverted: it gained nothing and
made a false positive likelier on the twelve engines that did work. Until it could be
derived rather than guessed at, the tool refused, and enums came out with names, sizes and
underlying types and **no invented values**.

Motorslice settled it. A different studio, a different game, no engine version string at
all — and byte-for-byte the same UEnum shape, failing in exactly the same way. Two
independent samples is what made deriving it defensible rather than fitting a curve to one
point.

The derivation keys on the tag, which is the part nothing else in a UEnum looks like: both
pointers have bit 0 set, and both are 8-byte aligned once it is cleared. It is tried only
after the interleaved shape fails, so no older build can match it by accident.

2486 of 2489 enums on Backrooms and 1970 of 1973 on Motorslice, with `EMovementMode`
identical on both: `MOVE_None=0, MOVE_Walking=1, … MOVE_MAX=7`.

Following those pointers *without* clearing the tag is the failure worth guarding: it lands
one byte into the array and every id after it is read misaligned, which does not fail — it
returns different names. The test compares the text, and reverting the mask fails ten
assertions.

Everything else on 5.7 works, including the largest SDK of any target here — 516 headers,
58984 `static_assert`s, zero errors — and all 225 KB of its bytecode.

---

## Working through the list

In the order that buys the most per hour:

Every engine era is covered. What is left is breadth within eras, not missing support:

1. **UE 5.3 and 5.4 — and why they are not worth buying.** They are the only versions in
   the supported range never run, and they are bracketed on both sides:

   ```
   4.25 – 5.2   FProperty next +0x20, offset +0x4c
   5.5  – 5.7   FProperty next +0x18, offset +0x44
   ```

   The change happened in 5.3 or 5.4, and both resulting shapes are proven — 5.2 below and
   5.5 above, each at L5. Everything else matches across the boundary too: the classic
   interleaved `UEnum`, `Interfaces +0x1d8`, `CDO +0x110`, the same name pool.

   The argument that closes it is that engine changes persist forward. Anything introduced
   in 5.3 or 5.4 is still present in 5.5, which derives cleanly. For those versions to hold
   a surprise, a structure would have to be introduced *and reverted* within two minor
   releases, which is not something Unreal does.

   The residual risk is not zero, but it is small enough that the two downloads would be
   buying documentation rather than coverage. If one is bought anyway, make it 5.4: it sits
   directly against the boundary and would pin down exactly which release moved it.
2. **Atomic Heart, Little Nightmares III, Ready Or Not** — more 4.27, so low value now.
   Atomic Heart's customised renderer is the interesting one.
3. **UE 4.20 / 4.21** — would add nothing structural. `TNameEntryArray` is the same across
   4.20 – 4.22 and 4.22 passes at L5, so these are breadth only.

Beyond that, nothing is known to be missing. There is no target on hand that Zircon refuses,
and no structure it has met that it cannot derive.

### Finding a target for a band

Release date is a rough guide and nothing more: 2017-19 tends to be 4.18-4.22, 2020
4.24-4.25, 2021-22 4.26-4.27, late 2022 5.0, 2023 5.1-5.2, 2024 5.3-5.4. Studios sit on
an engine version for years, so treat all of that as a shortlist, not an answer.

The answer costs a second:

```
zircon fingerprint --file "<any Shipping.exe>"
```

That reads the file on disk. Nothing has to be launched, so working through a pile of
candidates is faster than researching any one of them.

---

## Why version strings are not the signal

An early sweep tried to classify engine generation from feature strings in the binary:
`Nanite`, `Lumen`, `WorldPartition`, `MassEntity`, `VirtualShadowMap`.

It does not work. Atomic Heart, confirmed UE 4.27 by its own embedded version string,
contains `Nanite`, `Lumen` and `ChaosSolver`. Deep Rock Galactic, also 4.27, contains
`Lumen`. Those tokens survive in shader and config string tables whether or not the
feature exists in that engine version.

Vendor version strings fail for a different reason: licensee builds carry their own.
`++Project+SN2-Release-CL-123362` and `++RideGamejam+rel-1.2-CL-17120` say nothing about
which Unreal release they were forked from, and several shipped games carry no version
string at all.

So the engine generation is derived from observable memory layout - struct sizes, name
pool shape, object array shape, FProperty versus UProperty - and an embedded version
string is used only to raise confidence in a conclusion already reached. That was the
design from the start; it is now a measured result rather than an assumption.

---

## How to verify a target

```bash
# L0 — from disk, no launching
zircon fingerprint --file "<path to Shipping.exe>"

# when names or the layout will not derive, read objects without them
zircon inspect --pid <N> --filter "#1"              # by object-array slot
zircon inspect --pid <N> --filter "@0x7ff..." -n 128 # by raw address

# L1 — launch the game, reach the main menu, then
zircon detect
zircon objects --pid <N> --limit 40
zircon fingerprint --pid <N>

# L2
zircon dump --pid <N> --script --defaults --out game.json
zircon validate game.json

# L3 — the one that counts
zircon emit cpp_sdk game.json -o sdk/
cl /nologo /std:c++20 /EHsc /c /I sdk tu.cpp      # tu.cpp: #include "SDK.hpp"

# L4
zircon script --pid <N> --limit 20

# L5 — needs a full-memory minidump of the same session
zircon dump --dump game.dmp --script --out fromdump.json
zircon diff game.json fromdump.json
```

Record the result here, and write down anything that broke along with *why*. The failures
are the part worth keeping; a version that passed tells you much less than one that did
not and was then understood.

### What counts as a pass

- L1: names resolve to readable UE paths, not noise, and confidence is reported
- L2: `validate` says `round-trip lossless`
- L3: **zero** compile errors. Not "a few"
- L4: the undecoded percentage is stated, whatever it is
- L5: `no differences`

A partial result is a result. A target that reaches L2 and fails L3 is more useful than
one nobody ran, as long as the failure is written down.
