# Contributing

Zircon is written and maintained by TheHolyOneZ. Issues and pull requests are welcome; this
file is what you need to know before opening one, and what I check before merging.

If you only want to report something, skip to [Reporting a target that fails](#reporting-a-target-that-fails).
A good report against a game I do not own is worth more than most patches.

---

## The rule the whole project is built on

**Nothing is keyed off an engine version number.** There is no table of offsets per UE
release anywhere in this repository, and there will not be one. The same rule binds the Unity
backend, where it is the entire reason that backend exists: every other IL2CPP dumper carries
a table of struct layouts per metadata version, and `src/il2cpp/` carries none.

Every offset is derived at runtime from a property that is structural to the engine rather
than incidental to a build. That is why the tool holds up on licensee forks, renamed
executables, builds carrying no version string, and engine versions that did not exist when it
was written.

A patch that adds `if (version == "5.4") offset = 0x48;` will not be merged, no matter how
many games it fixes. If a derivation fails on some build, the derivation is wrong.

### Every derivation needs two things

1. **A positive distinguishing property** — something true of the right field and of nothing
   else.
2. **An independent anchor** — a value you know the answer to before you look, arrived at by
   a different route.

Internal consistency alone has been the only test eight times in this project's history and
has been wrong all eight. A field that scores perfectly on a weak constraint is not the right
field; it is a field that the constraint cannot see.

`docs/ENGINEERING-LOG.md` has every one of those eight written up. Read it before touching
`src/engine/`. It is long, and it is the most useful thing in the repo.

### The trap that keeps recurring

**A constraint phrased as an absence is best satisfied by a field containing nothing.**

- "the chain terminates and stays in the object array" — a field that is null everywhere
  terminates *soonest* and never leaves the array, so it outscores the real field. This one
  shipped, and truncated every function list on affected builds to one entry.
- "does not loop", "does not disagree", "does not leave the range" — same shape.

Pair every such check with one phrased as a presence. *Terminates* needs *and links*.
*Reports a plausible size* needs *and nothing is smaller than the root*.

---

## Building

Visual Studio 2022 and CMake. Dear ImGui and Lua are vendored, so there is nothing to
install.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

A local build reports its version as `<version>-dev`. `-DZIRCON_RELEASE=ON` drops the
suffix; that is the only difference between a local build and a released one.

> If Strawberry Perl or MinGW is on PATH, CMake may pick up its GCC. Pass the Visual Studio
> generator explicitly as above.

---

## Tests

Nine suites, and **none of them may require a game installed.** They run against synthetic
memory, hand-built bytecode and checked-in fixtures. Anything that needs a live process is a
verification run, not a test, and belongs in `docs/UE-Test.md` instead.

### A test that cannot fail is not a test

Before you submit one, **check that it bites**: revert your fix, build, and watch the new
test fail. If it still passes, it is testing something other than what you think.

This is not a formality. Two tests in this repo once guarded the same behaviour so
redundantly that neither could fail alone, and a synthetic world was once built so tidily
that the bug it was written for could not occur in it.

When a derivation is involved, build the synthetic world so the *wrong* answer is present
and attractive — a decoy that satisfies the weak constraint perfectly. `tests/test_core.cpp`
has two worked examples: the CDO decoy, and a `UStruct` world parameterised by how far a
fork has extended `UObject`.

---

## Changing a derivation

Anything under `src/engine/` that decides an offset is load-bearing for every target, so
"it works on the game I was looking at" is not evidence.

For `src/il2cpp/`, the equivalent is `docs/IL2CPP.md`, and the same rule applies: a
derivation there needs a positive distinguishing property and an independent anchor. The
worked example is where a method body lives, told apart from an invoker thunk that passes
every test a pointer-to-code test can pose.

**Required: an A/B against real games.** Dump the same *live process* twice — once with the
previous release binary, once with your build — and diff them:

```
zircon dump --pid <pid> -o before.json      # previous release
zircon dump --pid <pid> -o after.json       # your build
zircon diff before.json after.json
```

`no differences` on a spread of targets, or an explanation of every difference. Spread means
both property models (`UProperty` pre-4.25, `FProperty` after) and both name pools
(`TNameEntryArray` pre-4.23, `FNamePool` after) — a change can be invisible on one and fatal
on the other. Say in the PR which games and which engine versions.

`docs/UE-Test.md` defines what "tested" means here, L0 through L6, and records what has
actually been run. Update it if you add a target.

---

## Reporting a target that fails

The most useful thing you can send. Include:

- the game, store, build number and date
- `zircon fingerprint --pid <pid>` in full
- the failing command with `-v`, and its exit code
- what the log said it derived before it stopped

If it attached but would not derive, `zircon inspect --pid <pid> -f /Script/CoreUObject.Object`
is usually the single most informative thing in the report — it is the root of every class
chain, and its layout says what kind of build you are on.

**Do not infer offsets from deltas and report those as facts.** A recent report worked out
`sizeof(UObject)` from the gaps between known fields, got a number 16 bytes too large
because two separate causes were assumed to be one, and proposed a fix that would have
failed in exactly the same way. Read the value out of the process and say that you did.

---

## Style

Match the file you are editing. Beyond that:

**Comments explain decisions, not code.** Why this approach and not the obvious one; what
broke last time; which assumption the line rests on. A comment restating what the next line
plainly does is noise. Keep the density of the surrounding file — some of these explain a
derivation over fifteen lines, and that is fine when all fifteen carry reasoning.

**Keep the war stories in the changelog.** "This field is anchored on X because Y is free to
change" belongs in the source. "It found 92k objects and then produced nothing" belongs in
`CHANGELOG.md`. Counts from one game go stale the next time that game patches.

**Update `CHANGELOG.md` and `README.md` in the same change.** Every feature, every fix. A
change that is not in the changelog did not happen, and a README claim that nothing
re-measures stops being true quietly — that has happened here, to a "zero warnings" claim
that had drifted to 27,334.

**Say what was measured, not what should be true.** Numbers in the docs are things that were
run. If you did not run it, do not write it.

---

## Scope

Some things are excluded on purpose and a PR adding them will be declined:

- **Anti-cheat evasion.** Zircon refuses to inject into a process with anti-cheat loaded,
  and that stays.
- **Patching or modifying game code.** Reading memory, and writing property *values* when
  explicitly enabled, is the line.
- **Engine-version offset tables.** See above.
- **Dependencies.** Everything vendored is in `NOTICE`. A new third-party library needs to
  earn its place against writing the part actually needed — the gzip encoder in
  `src/zdex/` is roughly 400 lines and replaced zlib for one job.

`docs/SCOPE.md` has the full list of goals and non-goals.

---

## Layout

| Path | What lives there |
|---|---|
| `src/core/` | memory providers, PE parsing, pattern scanning — no UE knowledge at all |
| `src/engine/` | every derivation, and the walk that turns a process into the IR |
| `src/il2cpp/` | the Unity backend. Beside `src/engine/`, not under it: neither needs the other |
| `src/ir/` | the dump schema, JSON round-trip, and the structural linter |
| `src/emit/` | one file per output format; they see a `Dump` and nothing else |
| `src/diff/` | build-to-build comparison |
| `src/zdex/` | publishing: gzip, HTTP, the upload protocol |
| `src/app/`, `src/gui/`, `src/dll/` | the CLI, the browser window, the injected payload |
| `docs/` | architecture, the engineering log, coverage, the plugin ABI |

`docs/ARCHITECTURE.md` explains why the boundaries fall where they do. The short version:
`src/ir/` links nothing, and emitters cannot reach the target.

If you want to add an output format, you may not need to touch this repo at all —
`docs/PLUGINS.md` covers the C ABI and the vendored Lua host.

---

## Licence

Apache 2.0, and contributions are taken under the same. By opening a pull request you
confirm you wrote the code, or that you are entitled to submit it under that licence.
