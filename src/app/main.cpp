#include "core/Log.h"
#include "core/Term.h"
#include "core/Injector.h"
#include "plugin/Loader.h"
#include "core/ProcessList.h"
#include "core/MemorySource.h"
#include "core/PatternScanner.h"
#include "engine/EngineProfile.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/PropertyLayout.h"
#include "engine/StructLayout.h"
#include "engine/ClassLayout.h"
#include "engine/DumpBuilder.h"
#include "engine/FunctionLayout.h"
#include "engine/Kismet.h"
#include "engine/ValueReader.h"
#include "engine/ValueWriter.h"
#include "engine/TypeResolver.h"
#include "engine/UnrealDetect.h"
#include "il2cpp/Metadata.h"
#include "il2cpp/Static.h"
#include "zdex/Gzip.h"
#include "il2cpp/Runtime.h"
#include "diff/Diff.h"
#include "emit/Emitter.h"
#include "ir/Json.h"
#include "Publish.h"
#include "ir/Lint.h"

#include <algorithm>
#include <charconv>
#include <io.h>
#include <format>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// <windows.h> turns GetClassName into GetClassNameW, quietly rewriting calls to
// engine::GetClassName into window-manager calls. The engine's spelling wins here.
#undef GetClassName
#undef GetObject

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace zircon::core;
using namespace zircon::core::term;

namespace {

constexpr const char* kVersion = ZIRCON_VERSION;

struct TargetSpec {
    enum class Kind { None, Internal, Pid, ProcessName, DumpFile, StaticFile } kind{Kind::None};
    std::string   value;
    std::uint32_t pid{0};
};

// Output helpers. Every command that reports a result reports it the same way: a dim label
// in a fixed column, then the value. Reads as a table without being one, and stays aligned
// whether the values are hex, counts or prose.
//
// These go to stdout, never the log. They are the answer, not a note about how it was
// reached, and the distinction matters the moment anyone pipes the output.

// The label column. Format strings below add an explicit space after it, or a label exactly
// this long runs straight into its value.
constexpr int kFieldWidth = 17;

void Field(std::string_view label, std::string_view value) {
    std::printf("%.*s%-*.*s%.*s %.*s\n",
                static_cast<int>(Dim().size()), Dim().data(),
                kFieldWidth, static_cast<int>(label.size()), label.data(),
                static_cast<int>(Reset().size()), Reset().data(),
                static_cast<int>(value.size()), value.data());
}

template <typename... Args>
void Field(std::string_view label, std::format_string<Args...> fmt, Args&&... args) {
    Field(label, std::string(std::format(fmt, std::forward<Args>(args)...)));
}

// The point of the command: the version identified, the count produced. Bright, so it
// survives being scrolled past.
void FieldStrong(std::string_view label, std::string_view value) {
    std::printf("%.*s%-*.*s%.*s %.*s%.*s%.*s\n",
                static_cast<int>(Dim().size()), Dim().data(),
                kFieldWidth, static_cast<int>(label.size()), label.data(),
                static_cast<int>(Reset().size()), Reset().data(),
                static_cast<int>(Bold().size()), Bold().data(),
                static_cast<int>(value.size()), value.data(),
                static_cast<int>(Reset().size()), Reset().data());
}

// How the result was reached. Every derivation can produce one, and printing it is what
// makes a number checkable instead of merely asserted.
void Evidence(const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    std::printf("%.*sevidence%.*s\n",
                static_cast<int>(Dim().size()), Dim().data(),
                static_cast<int>(Reset().size()), Reset().data());
    for (const auto& line : lines)
        std::printf("  %.*s-%.*s %s\n",
                    static_cast<int>(Dim().size()), Dim().data(),
                    static_cast<int>(Reset().size()), Reset().data(), line.c_str());
}

// Column headings for the listing commands.
void Heading(std::string_view text) {
    std::printf("%.*s%.*s%.*s\n",
                static_cast<int>(Dim().size()), Dim().data(),
                static_cast<int>(text.size()), text.data(),
                static_cast<int>(Reset().size()), Reset().data());
}

// First thing anyone sees, so it groups by what you are trying to do, not alphabetically,
// and ends with commands that can be pasted as-is.
void PrintUsage() {
    const auto b = term::Bold();
    const auto d = term::Dim();
    const auto c = term::Cyan();
    const auto r = term::Reset();

    std::printf("%sZircon%s %s%s%s\n", b.data(), r.data(), d.data(), kVersion, r.data());
    std::printf("Game engine reflection extraction and analysis toolkit.\n");
    std::printf("%sUnreal Engine, and Unity IL2CPP from inside the process.%s\n\n",
                d.data(), r.data());

    std::printf("%sUsage%s  zircon <command> [target] [options]\n\n", b.data(), r.data());

    std::printf("%sInspect a target%s\n", b.data(), r.data());
    std::printf("  %sdetect%s        List running Unreal Engine and Unity IL2CPP processes\n", c.data(), r.data());
    std::printf("  %sfingerprint%s   Identify the runtime: UE version and layout, or Unity IL2CPP\n", c.data(), r.data());
    std::printf("  %smodules%s       List modules visible in the target\n", c.data(), r.data());
    std::printf("  %snames%s         Dump the FName pool\n", c.data(), r.data());
    std::printf("  %sobjects%s       List every UObject full name\n", c.data(), r.data());
    std::printf("  %sclasses%s       List classes and structs with sizes\n", c.data(), r.data());
    std::printf("  %sprops%s         List properties of classes and structs\n", c.data(), r.data());
    std::printf("  %sfunctions%s     List UFunctions with signatures\n", c.data(), r.data());
    std::printf("  %sscript%s        Decompile Kismet bytecode to pseudo-code\n", c.data(), r.data());
    std::printf("  %sread%s          Read live property values of one object\n", c.data(), r.data());
    std::printf("  %swrite%s         Write one property of one object (--set Name=Value)\n", c.data(), r.data());
    std::printf("  %sfind%s          Find objects by what they hold (--where Health<50)\n", c.data(), r.data());
    std::printf("  %sinspect%s       Annotated hexdump of one object (path, #slot or @address)\n", c.data(), r.data());
    std::printf("  %sscan%s          Pattern-scan the target\n", c.data(), r.data());
    std::printf("  %sscan-games%s    Find Unreal and Unity games on disk, and write a manifest\n", c.data(), r.data());
    std::printf("  %scheck%s         Whether a game can be dumped, without starting it\n", c.data(), r.data());
    std::printf("  %smetadata%s      What Zircon works out about a global-metadata.dat\n\n", c.data(), r.data());

    std::printf("%sProduce output%s\n", b.data(), r.data());
    std::printf("  %sdump%s          Full reflection dump to IR JSON\n", c.data(), r.data());
    std::printf("  %semit%s          Render a dump to one or more formats, comma separated\n",
                c.data(), r.data());
    std::printf("  %svalidate%s      Parse a dump, round-trip it, and with --strict lint it\n", c.data(), r.data());
    std::printf("  %sxref%s          What references a type, or with --uses what it references\n", c.data(), r.data());
    std::printf("  %sdiff%s          Compare two dumps and report what broke\n", c.data(), r.data());
    std::printf("  %sbatch%s         Dump every game in a manifest, unattended\n\n", c.data(), r.data());

    std::printf("%sWork live%s\n", b.data(), r.data());
    std::printf("  %sbrowse%s        Interactive object browser (zircon-gui.exe)\n", c.data(), r.data());
    std::printf("  %sinject%s        Load the payload DLL into a running game\n", c.data(), r.data());
    std::printf("                %sthe only way to dump Unity: its type data is behind calls%s\n\n",
                d.data(), r.data());

    std::printf("%sShare it%s (Zdex, at zlogic.eu/zdex)\n", b.data(), r.data());
    std::printf("  %spublish%s       Upload a dump and print where it landed\n", c.data(), r.data());
    std::printf("  %sfetch%s         Download a published dump, its mappings or its SDK\n", c.data(), r.data());
    std::printf("  %slogin%s         Store an API key so publishing works\n", c.data(), r.data());
    std::printf("  %slogout%s        Forget the stored key\n\n", c.data(), r.data());

    std::printf("%sSetup%s\n", b.data(), r.data());
    std::printf("  %sinstall%s       Add this folder to your PATH (per-user, no elevation)\n", c.data(), r.data());
    std::printf("  %suninstall%s     Take it off again\n\n", c.data(), r.data());

    std::printf("%sTarget%s (exactly one)\n", b.data(), r.data());
    std::printf("  --pid <n>          Attach externally to a running process\n"
                "  --process <name>   Attach externally by executable name\n"
                "  --dump <path>      Read a full-memory minidump\n"
                "  --file <path>      Read a PE image on disk (partial dumps only)\n"
                "  --internal         Run in-process (injected DLL builds)\n\n");

    std::printf("%sOptions%s\n", b.data(), r.data());
    std::printf("  -f, --filter <s>   Only show names containing this substring\n"
                "  -n, --limit <n>    Stop after n results\n"
                "  -o, --out <path>   Output path (dump: default dump.json)\n"
                "  -p, --pattern <s>  scan: the byte pattern to search for\n"
                "  -m, --module <s>   scan: restrict the scan to one module\n"
                "      --all-regions  scan: the whole address space, not only modules\n"
                "      --names        Embed the whole FName pool in the dump\n"
                "      --script       Decompile Kismet bytecode into the dump\n"
                "      --defaults     Read each property's value from its class default object\n"
                "      --plugins <d>  Load emitter plugins from a directory (repeatable)\n"
                "      --allow-partial  Let emitters run on a partial dump\n"
                "      --emit <fmts>  dump: also render the dump, e.g. cpp_sdk,usmap or all\n"
                "      --strict       validate: also check the dump against itself\n"
                "      --uses         xref: list what the type references, not what references it\n"
                "      --style <s>    diff output: text (default), json, markdown\n"
                "      --set <N=V>    write: the property and value, e.g. MaxWalkSpeed=1337\n"
                "      --where <cond> find: Name<op>Value, ops = != < > <= >=\n"
                "      --breaking     diff: only changes that break existing code\n"
                "      --publish      dump: publish it to Zdex once it is written\n"
                "      --game <name>  publish: which game this is (guessed from the process)\n"
                "      --label <s>    publish: which build, e.g. \"1.4.2 (Steam)\"\n"
                "      --notes <s>    publish: a line of context for whoever reads it\n"
                "      --dry-run      publish: check everything and send nothing\n"
                "      --no-wait      publish: return once uploaded, without waiting on indexing\n"
                "      --usmap        fetch: mappings instead of the dump (--sdk for the SDK zip)\n"
                "      --json         publish, fingerprint: machine-readable result on stdout\n"
                "      --open         publish: open the result in a browser when it is ready\n"
                "      --mode <m>     Unity: live (default), static or dual\n"
                "      --metadata <p> Unity: read global-metadata.dat, no process needed\n"
                "      --launch <exe> inject: start the game and wait for its runtime\n"
                "      --wait         inject: block until the walk is done\n"
                "      --wait-for-settle [s]  inject: let the class cache stop growing first\n"
                "      --timeout <s>  inject: ceiling for --launch and --wait (default 900)\n"
                "      --headless     inject: no console inside the game\n"
                "      --dll <path>   inject: a payload other than the one next door\n"
                "  -y, --yes          publish: skip the confirmation\n"
                "  -v, --verbose      Debug logging (repeat for trace)\n"
                "      --color/--no-color  Force colour on or off (also honours NO_COLOR)\n"
                "  -h, --help         Show this help\n"
                "      --version      Show version\n\n");

    std::printf("%sExit codes%s\n", b.data(), r.data());
    std::printf("  0  fine\n"
                "  1  bad arguments\n"
                "  2  the name given could not be pinned down: no match, or several\n"
                "  3  what you named was not there: no object, no matches, no scan hits,\n"
                "     no engine version, or a Unity build with the exports stripped\n"
                "  4  attached, but the reflection layout would not derive; inject failed\n"
                "  5  could not open the target, or could not write the output\n"
                "  6  a dump file would not parse, or the round trip was lossy\n"
                "  7  an emitter refused\n"
                "  8  diff found a breaking change\n"
                "  9  validate --strict found the dump contradicting itself\n\n");
    std::printf("%sOne wart: an ambiguous --process exits 5 from most commands but 2 from\n"
                "inject and browse, which resolve the name themselves.%s\n\n",
                d.data(), r.data());

    std::printf("%sExamples%s\n", b.data(), r.data());
    std::printf("  %szircon detect%s\n", d.data(), r.data());
    std::printf("  %szircon fingerprint --process MyGame-Win64-Shipping.exe%s\n", d.data(), r.data());
    std::printf("  %szircon dump --pid 1234 --script --defaults -o game.json%s\n", d.data(), r.data());
    std::printf("  %szircon emit cpp_sdk game.json -o sdk/%s\n", d.data(), r.data());
    std::printf("  %szircon diff old.json new.json --breaking%s\n", d.data(), r.data());
    std::printf("  %szircon dump --pid 1234 --emit cpp_sdk,usmap -o game.json%s\n", d.data(), r.data());
    std::printf("  %szircon validate game.json --strict%s\n", d.data(), r.data());
    std::printf("  %szircon xref game.json -f CharacterMovementComponent%s\n", d.data(), r.data());
    std::printf("  %szircon browse --pid 1234%s\n", d.data(), r.data());
    std::printf("  %szircon publish game.json --label \"1.4.2 (Steam)\"%s\n", d.data(), r.data());
}

// Per-command help.
//
// `zircon login --help` used to answer "unknown option: --help", which is a bad reply to a
// reasonable question. One entry per command: how it is called, the flags that apply to it,
// and what it exits with. Commands not listed here fall back to the full help.
struct CommandHelp {
    const char* name;
    const char* usage;
    const char* body;
};

constexpr const char* kTargetHelp =
    "  --pid <n>          Attach externally to a running process\n"
    "  --process <name>   Attach externally by executable name\n"
    "  --dump <path>      Read a full-memory minidump\n"
    "  --file <path>      Read a PE image on disk (partial)\n"
    "  --internal         Run in-process (injected builds)\n";

constexpr CommandHelp kCommandHelp[] = {
{"scan-games", "zircon scan-games [<folder>...] [-o games.toml]",
 "Finds Unreal and Unity games by what is in the folder rather than by a list of known\n"
 "titles. With no folders given it sweeps every fixed drive.\n\n"
 "  -o, --out <path>   Write a manifest for `zircon batch`\n"
 "      --json         Machine-readable list on stdout\n\n"
 "Mono-backend Unity games are listed and commented out of the manifest: they have no\n"
 "GameAssembly.dll, so there is nothing for the IL2CPP path to talk to.\n\n"
 "Exits 3 when nothing was found.\n"},

{"batch", "zircon batch <manifest.toml> [-o <dir>]",
 "Dumps every game in a manifest, unattended. One failing does not stop the rest, and a\n"
 "Unity game whose runtime faults partway through falls back to its metadata, so it\n"
 "still yields its type system.\n\n"
 "  -o, --out <dir>    Where the dumps go (default zircon-batch). Each one is .json.gz\n"
 "      --mode <m>     live (default) or dual, for the Unity ones\n"
 "      --wait-for-settle [s]  let each runtime's class cache stop growing first\n"
 "      --timeout <s>  per game (default 900)\n\n"
 "Write the manifest with `zircon scan-games -o games.toml`.\n\n"
 "Exits 4 when any game failed, and 3 when every game produced a dump but some fell\n"
 "back to metadata -- those carry the type system without offsets, RVAs or concrete\n"
 "generics.\n"},

{"check", "zircon check <game.exe>",
 "Says whether a game can be dumped, without starting it. A live dump means launching\n"
 "the game and waiting for its runtime, and when that cannot work the way you find out\n"
 "is a full launch and a timeout.\n\n"
 "Reads the install folder and the process list: which runtime it is, which store it\n"
 "came from, whether that store is running, whether the game already is.\n\n"
 "A Unity game gets the better answer -- it can be dumped from global-metadata.dat\n"
 "whether or not it will ever launch.\n\n"
 "Exits 3 when a live dump looks unlikely, 5 when there is no such file.\n"},

{"metadata", "zircon metadata <global-metadata.dat>",
 "Says what Zircon worked out about a Unity metadata file, and the evidence for each\n"
 "step: the header layout, which span is which table, the record sizes, and where the\n"
 "names and tokens sit. No version table is involved; it is all derived from the file.\n\n"
 "Worth running before dumping a target you cannot run, to find out whether it can be\n"
 "read at all.\n\n"
 "Exits 3 when the file cannot be read, naming the constraint that failed.\n"},

{"detect", "zircon detect",
 "Lists running games worth attaching to. Unreal processes come with a confidence score;\n"
 "Unity IL2CPP is a yes or no, since either a module exports the embedding API or it does\n"
 "not.\n\n"
 "  -v                 Also print the evidence behind each score\n\n"
 "Exits 0 when something was found, 3 when nothing is running.\n"},

{"fingerprint", "zircon fingerprint <target>",
 "Says what runtime a target is and how much of it can be read. Unity is checked first\n"
 "because it is a yes or no; the Unreal fingerprint is a score and will guess low about a\n"
 "game that is not Unreal at all.\n\n"
 "  --json             Machine-readable, for scripts that would otherwise scrape this\n\n"
 "Exits 0 when the runtime is identified, 3 when it is not, or when a Unity build has\n"
 "stripped exports the walk needs.\n"},

{"modules", "zircon modules <target>",
 "Every module the target has loaded, with base and size.\n"},

{"names", "zircon names <target>",
 "The FName pool. Unreal only.\n\n"
 "  -n, --limit <n>    Stop after n names\n"},

{"objects", "zircon objects <target>",
 "Every UObject in GObjects by full name. Unreal only.\n\n"
 "  -f, --filter <s>   Only names containing this substring\n"
 "  -n, --limit <n>    Stop after n results\n"},

{"classes", "zircon classes <target>",
 "Classes and structs with their sizes. Unreal only.\n\n"
 "  -f, --filter <s>   Only names containing this substring\n"
 "  -n, --limit <n>    Stop after n results\n"},

{"props", "zircon props <target> -f <class>",
 "Properties of a class or struct, with offsets and sizes. Unreal only.\n\n"
 "  -f, --filter <s>   Which class to list\n"
 "  -n, --limit <n>    Stop after n results\n"},

{"functions", "zircon functions <target>",
 "UFunctions with their signatures and native addresses. Unreal only.\n\n"
 "  -f, --filter <s>   Only names containing this substring\n"
 "  -n, --limit <n>    Stop after n results\n"},

{"script", "zircon script <target> -f <class>",
 "Decompiles Kismet bytecode into pseudo-code. Unreal only.\n\n"
 "  -f, --filter <s>   Which class or function to decompile\n"
 "  -n, --limit <n>    Stop after n functions\n"},

{"read", "zircon read <target> -f <object path>",
 "Live property values of one object. Unreal only, and external is enough.\n\n"
 "  -f, --filter <s>   The object: full path, #slot or @address\n"},

{"write", "zircon write <target> -f <object> --set Name=Value",
 "Writes one property of one object. The only command that changes the game.\n\n"
 "  -f, --filter <s>   The object: full path, #slot or @address\n"
 "      --set <N=V>    Property and value, e.g. MaxWalkSpeed=1337\n"},

{"find", "zircon find <target> -f <class> --where <cond>",
 "Finds objects by what they currently hold.\n\n"
 "  -f, --filter <s>   Which class to search\n"
 "      --where <c>    Name<op>Value, ops = != < > <= >=\n"
 "  -n, --limit <n>    Stop after n matches\n"},

{"inspect", "zircon inspect <target> -f <object>",
 "Annotated hexdump of one object: every byte, labelled with the property that owns it.\n\n"
 "  -f, --filter <s>   The object: full path, #slot or @address\n"
 "  -n, --limit <n>    Stop after n bytes\n"},

{"scan", "zircon scan <target> -p <pattern>",
 "Pattern-scans the target. Modules only unless told otherwise.\n\n"
 "  -p, --pattern <s>  Byte pattern, ?? for wildcards\n"
 "  -m, --module <s>   Restrict to one module\n"
 "      --all-regions  The whole address space, not only modules\n"
 "  -n, --limit <n>    Stop after n hits\n"},

{"dump", "zircon dump <target> [-o <path>]",
 "Full reflection dump to IR JSON. Unreal from outside the process; for Unity use inject,\n"
 "because IL2CPP keeps its field offsets behind a function call.\n\n"
 "  -o, --out <path>   Where to write it (default dump.json); .json.gz compresses it\n"
 "      --metadata <p> Unity: read global-metadata.dat instead, with no process at all\n"
 "      --mode static  the same thing, said the other way round\n"
 "      --names        Embed the whole FName pool\n"
 "      --script       Decompile Kismet bytecode into the dump\n"
 "      --defaults     Read each property's value from its class default object\n"
 "      --emit <fmts>  Also render it, e.g. cpp_sdk,usmap or all\n"
 "      --publish      Publish to Zdex once written (see publish --help)\n"
 "      --launch <exe> Start the game first and dump what comes up. GObjects is built\n"
 "                     during engine init, so a game dumped the moment its window\n"
 "                     appears has no object array yet\n"
 "      --timeout <s>  How long to wait for that (default 900)\n\n"
 "Exits 5 when the file could not be written, 4 when --launch gave up waiting.\n"},

{"emit", "zircon emit <format[,format]> <dump.json> [-o <dir>]",
 "Renders a dump into one or more output formats. `zircon emit list` prints them.\n\n"
 "  -o, --out <path>   Output directory\n"
 "      --allow-partial  Let emitters run on a dump with no object data\n"
 "      --plugins <d>  Load emitter plugins from a directory (repeatable)\n\n"
 "Exits 6 when the dump will not parse, 7 when an emitter failed.\n"},

{"validate", "zircon validate <dump.json> [--strict]",
 "Parses a dump, writes it back out and checks the two match. With --strict it also lints\n"
 "the contents: offsets past the end of a type, sizes that contradict a base, and so on.\n\n"
 "      --strict       Also lint the dump against itself\n"
 "  -n, --limit <n>    Stop printing after n findings\n\n"
 "Exits 6 when the dump will not parse or the round trip is lossy, 9 when the lint found\n"
 "something.\n"},

{"xref", "zircon xref <dump.json> -f <type>",
 "What references a type, or with --uses what that type references.\n\n"
 "  -f, --filter <s>   The type to look up\n"
 "      --uses         Invert it: what this type references\n"
 "  -n, --limit <n>    Stop after n results\n"},

{"diff", "zircon diff <before.json> <after.json>",
 "Compares two dumps and reports what changed, and what of that breaks existing code.\n\n"
 "      --breaking     Only changes that break code built against the old dump\n"
 "      --style <s>    text (default), json, markdown\n"
 "  -o, --out <path>   Write the report instead of printing it\n\n"
 "Exits 6 when either dump will not parse, 8 when --breaking found something.\n"},

{"browse", "zircon browse <target>",
 "Opens the interactive object browser. Hands off to zircon-gui.exe next door.\n"},

{"inject", "zircon inject --pid <n>",
 "Loads the payload DLL into a running game. The payload dumps on its own and writes into\n"
 "zircon-out beside the DLL. This is the only way to dump Unity.\n\n"
 "  --pid <n>          Which process\n"
 "  --process <name>   By name, when exactly one matches\n"
 "      --launch <exe> Start the game first and wait for GameAssembly.dll to be mapped,\n"
 "                     rather than guessing how long that takes\n"
 "  -o, --out <path>   Write the dump here instead of naming it after the process\n"
 "      --wait         Block until the walk is done, and exit non-zero if it wasn't.\n"
 "                     With --launch, the game is closed afterwards\n"
 "      --wait-for-settle [s]  Let the runtime's class cache stop growing before walking.\n"
 "                     Two dumps of one build differ otherwise, by how long it sat\n"
 "      --timeout <s>  Ceiling for --launch and --wait (default 900)\n"
 "      --headless     No console in the game; it steals focus from a fullscreen one\n"
 "      --dll <path>   A payload other than the zircon.dll next to this executable\n\n"
 "So one game, unattended, is:\n\n"
 "  zircon inject --launch \"D:\\Games\\Thing\\Thing.exe\" --wait --headless -o thing.json\n\n"
 "Each run gets its own log under zircon-out\\logs. If the runtime faults on one of its\n"
 "own types the payload records which, and the next run walks past it.\n\n"
 "Exits 2 when a name matches more than one process, 4 when the load failed, the game\n"
 "died mid-walk, or the timeout ran out.\n"},

{"publish", "zircon publish <dump.json|dir> --game <name> --label <build>",
 "Uploads a dump to Zdex. The game name and the build label are what tell two dumps\n"
 "apart later, so both are required.\n\n"
 "      --game <name>  Which game it is\n"
 "      --label <s>    Which build, e.g. \"1.4.2 (Steam)\"\n"
 "      --notes <s>    Anything worth saying about how it was taken\n"
 "      --all          Publish every dump in a directory, each named from its own\n"
 "                     header. --game overrides that for all of them\n"
 "      --force        Send a dump this machine has published before\n"
 "      --dry-run      Check everything and send nothing, before spending minutes\n"
 "                     compressing several hundred megabytes\n"
 "      --no-wait      Do not poll until the import settles\n"
 "      --open         Open the dump page when it is done\n"
 "  -y, --yes          Accept the publishing terms without being asked\n\n"
 "A dump already published from this machine is skipped, because Zdex refuses an\n"
 "identical one and finding that out otherwise costs the whole upload.\n\n"
 "Exits 1 on anything rejected, 2 on a key problem, 3 on the network.\n"},

{"fetch", "zircon fetch <id> [-o <path>]",
 "Downloads a published dump, or its mappings or SDK instead.\n\n"
 "      --usmap        Mappings rather than the dump\n"
 "      --sdk          The SDK zip rather than the dump\n"
 "  -o, --out <path>   Where to write it\n"},

{"login", "zircon login [<key>]",
 "Stores a Zdex API key so publishing works. The key goes in one file under %APPDATA% and\n"
 "nowhere else -- not in a dump, not in a log.\n\n"
 "      --key <s>      The key, if you would rather not have it in shell history\n"},

{"logout", "zircon logout",
 "Forgets the stored key.\n"},

{"install", "zircon install",
 "Adds this folder to the per-user PATH. No elevation, nothing outside your own profile.\n"},

{"uninstall", "zircon uninstall",
 "Takes it off the PATH again.\n"},
};

// True when the command had its own page. Anything else falls through to the full help.
bool PrintCommandHelp(std::string_view command) {
    for (const auto& entry : kCommandHelp) {
        if (command != entry.name) continue;

        const auto b = term::Bold();
        const auto d = term::Dim();
        const auto r = term::Reset();

        std::printf("%sUsage%s  %s\n\n", b.data(), r.data(), entry.usage);
        std::fputs(entry.body, stdout);

        // Only the commands that take one, and it is the same list every time.
        const bool needs_target =
            std::string_view(entry.body).find("<target>") != std::string_view::npos ||
            std::string_view(entry.usage).find("<target>") != std::string_view::npos;
        if (needs_target) {
            std::printf("\n%sTarget%s (exactly one)\n", b.data(), r.data());
            std::fputs(kTargetHelp, stdout);
        }

        std::printf("\n%szircon --help lists every command.%s\n", d.data(), r.data());
        return true;
    }
    return false;
}

// What `dump --publish` carries through to the publish step. A struct because
// CommandDump already takes eight parameters and none of these are its business
// beyond handing them on.
struct PublishAfterDump {
    bool requested{false};
    std::string game;
    std::string label;
    std::string notes;
    bool wait{true};
    bool assume_yes{false};
    bool open_browser{false};
};

// Bare argument, not a flag. Own function because arg[0] on an empty string_view is UB,
// and you get an empty argv entry any time a shell expands a variable to nothing.
bool IsPositional(std::string_view arg) {
    return !arg.empty() && arg.front() != '-';
}

std::optional<std::uint32_t> ParseU32(std::string_view text) {
    std::uint32_t value{};
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

Result<std::unique_ptr<IMemorySource>> OpenTarget(const TargetSpec& spec) {
    switch (spec.kind) {
        case TargetSpec::Kind::Internal:    return OpenInternal();
        case TargetSpec::Kind::Pid:         return OpenExternalByPid(spec.pid);
        case TargetSpec::Kind::ProcessName: return OpenExternalByName(spec.value);
        case TargetSpec::Kind::DumpFile:    return OpenDumpFile(spec.value);
        case TargetSpec::Kind::StaticFile:  return OpenStaticImage(spec.value);
        case TargetSpec::Kind::None:        break;
    }
    return Error{"no target specified; pass one of --pid/--process/--dump/--file/--internal", 1};
}

// Writes a dump, compressed when the path says so.
//
// A path ending .gz that holds plain JSON is worse than no compression at all, so this is the
// one place either kind gets written. The plain file is streamed through the compressor rather
// than held twice in memory, and it goes away afterwards.
bool WriteDumpFile(const zircon::ir::Dump& dump, const std::string& path, std::string& error) {
    const std::filesystem::path out{path};
    const bool compress = out.extension() == ".gz";
    const auto plain = compress ? std::filesystem::path(out).replace_extension() : out;

    if (!zircon::ir::WriteJsonFile(dump, plain.string(), error)) return false;
    if (!compress) return true;

    zircon::zdex::GzipStats stats;
    if (!zircon::zdex::GzipFile(plain.string(), out.string(), error, &stats)) return false;

    std::error_code ec;
    std::filesystem::remove(plain, ec);
    LogInfo("compressed to {} ({:.0f}x smaller than the {} MiB of JSON)", out.string(),
            stats.ratio(), static_cast<std::uint64_t>(stats.raw >> 20));
    return true;
}

// Loads a dump, compressed or not.
//
// One place, because 0.7.0 taught the lesson the hard way: the writer learned .json.gz and the
// readers did not, so validate, emit, xref and diff all met gzip's 1f 8b with "expected a
// number" on files Zircon had just written itself. Sniff here and every command that reads a
// dump gets it.
zircon::ir::JsonExpected<zircon::ir::Dump> LoadDump(std::string_view path) {
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) return zircon::ir::JsonError{std::format("cannot open {}", path), 0};

    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (!zircon::zdex::LooksGzipped(bytes)) return zircon::ir::ParseJson(bytes);

    std::string error;
    const std::string plain = zircon::zdex::GzipDecompress(bytes, error);
    if (!error.empty())
        return zircon::ir::JsonError{std::format("{}: {}", path, error), 0};
    return zircon::ir::ParseJson(plain);
}

// Reads a file into memory, or says why not. Metadata files are tens of megabytes, which is
// nothing next to the dump they produce.
std::optional<std::vector<std::uint8_t>> ReadWholeFile(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;

    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);

    std::vector<std::uint8_t> bytes(size);
    if (size && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        return std::nullopt;
    return bytes;
}

// JSON strings, the small subset that shows up here: quotes, backslashes and control
// characters. Paths come through this and Windows paths are full of backslashes.
std::string JsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                else
                    out += c;
        }
    }
    return out + "\"";
}

std::string JsonList(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += JsonQuote(values[i]);
    }
    return out + "]";
}

// Unity puts the metadata at a fixed place inside a game folder. A layout convention, not a
// version table: if it is not there the caller is told to point at it directly.
std::filesystem::path MetadataBeside(const std::filesystem::path& game_folder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(game_folder, ec)) return {};

    for (const auto& entry : std::filesystem::directory_iterator(game_folder, ec)) {
        if (!entry.is_directory()) continue;
        const auto candidate = entry.path() / "il2cpp_data" / "Metadata" / "global-metadata.dat";
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

// A dump with no process anywhere in it.
//
// This is the half of hybrid mode that reaches targets the injected walk cannot: an APK, a
// console build, a game with anti-cheat, a game that crashes the moment the walk touches it.
// It is a true partial answer and says so -- the type set is exact and repeatable, and
// nothing that lives in the binary rather than the metadata is invented to fill the gaps.
int CommandStaticDump(std::string_view metadata_path, std::string_view out_path) {
    auto bytes = ReadWholeFile(metadata_path);
    if (!bytes) {
        LogError("cannot read {}", metadata_path);
        return 5;
    }

    const auto solved = zircon::il2cpp::SolveMetadataLayout(*bytes);
    if (!solved) {
        LogError("{}", solved.error().message);
        return 3;
    }
    const auto& layout = solved.value();
    LogInfo("metadata version {}, {} spans, type records of {} bytes",
            layout.version, layout.spans.size(), layout.type_record);
    for (const auto& line : layout.evidence) LogInfo("  - {}", line);

    zircon::il2cpp::StaticStats stats;
    auto dump = zircon::il2cpp::ReadStaticDump(*bytes, layout, stats);
    dump.header.tool_version = kVersion;

    LogInfo("static dump: {} assemblies, {} types, {} fields, {} methods",
            stats.images, stats.types, stats.fields, stats.methods);
    if (stats.orphan_types)
        LogWarn("{} types are not claimed by any assembly, so they are absent from this dump",
                stats.orphan_types);
    LogWarn("field types, field offsets and method addresses are not in the metadata; they "
            "come from the binary, which this mode does not read");

    const std::string path = out_path.empty() ? "dump.json" : std::string(out_path);
    std::string error;
    if (!WriteDumpFile(dump, path, error)) {
        LogError("could not write '{}': {}", path, error);
        return 5;
    }

    Heading("Static dump");
    Field("output", "{}", path);
    Field("assemblies", "{}", stats.images);
    Field("types", "{}", stats.types);
    return 0;
}

// How the payload gets told anything. It has no command line, so the CLI leaves key=value
// beside the DLL and the payload reads it once on the way in and deletes it.
constexpr const char* kPayloadHandoff = "zircon-payload.cfg";

// What `inject --launch` and `--wait` take when nobody says otherwise. Fifteen minutes is
// past the slowest walk in the corpus by a wide margin.
constexpr int kDefaultTimeoutSeconds = 900;

struct InjectOptions {
    std::string out_path;
    std::string dll_path;
    std::string launch;        // an executable to start first
    std::string mode;          // live or dual; empty means live
    bool  headless{false};
    bool  wait{false};
    int   settle{0};
    int   timeout{kDefaultTimeoutSeconds};
};

// Defined further down, next to the rest of the injection plumbing.
int CommandInject(const TargetSpec& spec, const InjectOptions& options);

// --- finding games on disk -----------------------------------------------------------

struct FoundGame {
    std::string name;        // the folder, which is what a person calls the game
    std::string exe;
    std::string runtime;     // "il2cpp", "mono" or "unreal"
    std::string metadata;    // global-metadata.dat, when there is one
};

// The executable that belongs to a game folder.
//
// Unity writes <Game>_Data beside <Game>.exe, so the data folder names the exe. Unreal ships
// a launcher next to the real thing and the real thing is the one ending -Shipping.
// Helpers Unreal ships beside every game. Named, because nothing about them says "not the
// game" except what they are called, and picking one produces a manifest that launches a
// crash reporter forty-eight times.
bool UnrealHelper(std::string_view stem) {
    for (const char* name : {"CrashReportClient", "CrashReporter", "UnrealCEFSubProcess",
                             "EpicWebHelper", "UnrealEditor", "ShaderCompileWorker",
                             "UnrealPak", "BootstrapPackagedGame"})
        if (stem.find(name) != std::string_view::npos) return true;
    return false;
}

std::string ExecutableIn(const std::filesystem::path& folder, std::string_view runtime) {
    std::error_code ec;
    std::string fallback;

    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto path = entry.path();
        if (path.extension() != ".exe") continue;

        const auto stem = path.stem().string();
        if (stem.find("UnityCrashHandler") != std::string::npos) continue;
        if (runtime == "unreal") {
            if (UnrealHelper(stem)) continue;
            if (stem.find("-Shipping") != std::string::npos) return path.string();
        } else {
            // Unity: the exe whose name matches a _Data folder beside it.
            std::error_code inner;
            if (std::filesystem::is_directory(folder / (stem + "_Data"), inner))
                return path.string();
        }
        if (fallback.empty()) fallback = path.string();
    }
    return fallback;
}

// Walks a directory tree looking for game folders.
//
// Bounded by depth rather than by a list of launchers, because games end up anywhere: a
// Steam library, an Epic folder, a drive somebody unzipped things onto. A folder holding
// GameAssembly.dll is Unity IL2CPP; UnityPlayer.dll without it is the Mono backend, which is
// a correct no rather than a gap; a Binaries\\Win64 holding -Shipping.exe is Unreal.
void ScanFolder(const std::filesystem::path& root, int depth, std::vector<FoundGame>& into) {
    if (depth < 0) return;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    const bool il2cpp = std::filesystem::exists(root / "GameAssembly.dll", ec);
    const bool unity  = il2cpp || std::filesystem::exists(root / "UnityPlayer.dll", ec);
    if (unity) {
        FoundGame game;
        game.name    = root.filename().string();
        game.runtime = il2cpp ? "il2cpp" : "mono";
        game.exe     = ExecutableIn(root, game.runtime);
        if (il2cpp) {
            const auto found = MetadataBeside(root);
            game.metadata = found.string();
        }
        if (!game.exe.empty()) into.push_back(std::move(game));
        return;                       // a game folder does not contain another game
    }

    std::vector<std::filesystem::path> children;
    for (const auto& entry : std::filesystem::directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory(ec)) continue;
        const auto name = entry.path().filename().string();
        // Nothing lives in these and they are enormous.
        if (name == "Windows" || name == "$Recycle.Bin" || name == "System Volume Information")
            continue;
        children.push_back(entry.path());
    }

    // Unreal: a project folder holding Binaries\Win64. `Engine` is the shared engine and
    // never the game, so it is only looked at when nothing else turned anything up -- taking
    // whatever sorted first is how a manifest ends up full of crash reporters.
    std::string found_exe;
    for (const bool engine_too : {false, true}) {
        for (const auto& child : children) {
            const bool is_engine = child.filename() == "Engine";
            if (is_engine != engine_too) continue;

            const auto binaries = child / "Binaries" / "Win64";
            if (!std::filesystem::is_directory(binaries, ec)) continue;
            const auto exe = ExecutableIn(binaries, "unreal");
            if (!exe.empty()) {
                found_exe = exe;
                break;
            }
        }
        if (!found_exe.empty()) break;
    }
    if (!found_exe.empty()) {
        into.push_back(FoundGame{root.filename().string(), found_exe, "unreal", {}});
        return;
    }

    for (const auto& child : children) ScanFolder(child, depth - 1, into);
}

// Every game under the roots given, as a table or as a batch manifest.
//
// The point is the manifest: `zircon scan-games ... -o games.toml` then
// `zircon batch games.toml` dumps all of them without anybody sitting there.
// Steam's own build number for the game an exe belongs to.
//
// Labels are the primary key a diff works on, and left to a human they are whatever got
// typed that day. Steam already keeps a number that changes on exactly the event that
// matters -- the game being updated -- in steamapps/appmanifest_<appid>.acf beside the
// install.
//
// Empty when this is not a Steam install, which is not an error. Epic and GOG keep nothing
// equivalent in a documented place.
std::string SteamBuildId(const std::filesystem::path& exe) {
    std::error_code ec;

    // .../steamapps/common/<Game>/[bin/...]/game.exe -- walk up looking for the common
    // folder, remembering the directory directly under it, which is what the manifest names.
    std::filesystem::path steamapps;
    std::string install_dir;
    for (auto at = exe.parent_path(); !at.empty() && at != at.root_path(); at = at.parent_path()) {
        if (at.filename() == "common" && at.parent_path().filename() == "steamapps") {
            steamapps = at.parent_path();
            break;
        }
        install_dir = at.filename().string();
    }
    if (steamapps.empty() || install_dir.empty()) return {};

    for (const auto& entry : std::filesystem::directory_iterator(steamapps, ec)) {
        const auto name = entry.path().filename().string();
        if (!name.starts_with("appmanifest_") || entry.path().extension() != ".acf") continue;

        std::ifstream in(entry.path());
        if (!in) continue;

        // The format is quoted key/value pairs, one per line. Read what is needed rather
        // than parsing VDF properly: two keys, both at the top level.
        std::string line, build_id, dir;
        while (std::getline(in, line)) {
            const auto field = [&](std::string_view key) -> std::string {
                const auto at = line.find(key);
                if (at == std::string::npos) return {};
                const auto open = line.find('"', at + key.size());
                if (open == std::string::npos) return {};
                const auto close = line.find('"', open + 1);
                if (close == std::string::npos) return {};
                return line.substr(open + 1, close - open - 1);
            };
            if (build_id.empty()) build_id = field("\"buildid\"");
            if (dir.empty())      dir      = field("\"installdir\"");
        }

        if (dir == install_dir && !build_id.empty()) return build_id;
    }
    return {};
}

// A TOML string that says what it holds.
//
// 0.7.0 quoted everything with " and wrote Windows paths straight in, so a manifest was full
// of exe = "C:\Program Files\..." -- and \P is not a TOML escape. Zircon's own reader was
// loose enough not to notice; every other TOML parser rejects the file.
//
// Literal strings exist for exactly this and cannot express a quote of their own, so anything
// holding one falls back to a basic string with the escapes written out.
std::string TomlString(std::string_view value) {
    const bool awkward = value.find('\'') != std::string_view::npos ||
                         std::any_of(value.begin(), value.end(), [](char c) {
                             return static_cast<unsigned char>(c) < 0x20;
                         });
    if (!awkward) return std::format("'{}'", value);

    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04X}", static_cast<unsigned char>(c));
                else
                    out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

// The other half. Handles both kinds, because manifests written by 0.7.0 are still out there
// and refusing to read one over a quote character would be its own kind of rude.
std::string TomlValue(std::string_view text) {
    const auto open = text.find_first_of("'\"");
    if (open == std::string_view::npos) return {};

    const char quote = text[open];
    const auto close = text.rfind(quote);
    if (close <= open) return {};
    const auto body = text.substr(open + 1, close - open - 1);

    if (quote == '\'') return std::string(body);   // literal: what you see is what it is

    std::string out;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) {
            out.push_back(body[i]);
            continue;
        }
        switch (body[++i]) {
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            // Not an escape TOML knows. 0.7.0 wrote paths this way, so the backslash was
            // meant literally and both characters are kept.
            default:   out.push_back('\\'); out.push_back(body[i]); break;
        }
    }
    return out;
}

int CommandScanGames(const std::vector<std::string>& roots, std::string_view out_path,
                     bool as_json) {
    std::vector<std::string> search = roots;
    if (search.empty()) {
        // Every fixed drive. A sweep with no arguments is the common case and typing out
        // drive letters is not something anybody should have to do.
        for (char letter = 'C'; letter <= 'Z'; ++letter) {
            const std::string drive = std::string(1, letter) + ":\\";
            if (::GetDriveTypeA(drive.c_str()) == DRIVE_FIXED) search.push_back(drive);
        }
    }

    std::vector<FoundGame> found;
    for (const auto& root : search) {
        LogInfo("scanning {}", root);
        ScanFolder(root, 6, found);
    }

    std::sort(found.begin(), found.end(), [](const FoundGame& a, const FoundGame& b) {
        return std::tie(a.runtime, a.name) < std::tie(b.runtime, b.name);
    });

    if (as_json) {
        std::printf("[");
        for (std::size_t i = 0; i < found.size(); ++i) {
            std::printf("%s{\"name\": %s, \"runtime\": %s, \"exe\": %s, \"metadata\": %s}",
                        i ? ", " : "", JsonQuote(found[i].name).c_str(),
                        JsonQuote(found[i].runtime).c_str(), JsonQuote(found[i].exe).c_str(),
                        JsonQuote(found[i].metadata).c_str());
        }
        std::printf("]\n");
    } else {
        Heading(std::format("{:<10} {:<34} {}", "RUNTIME", "GAME", "EXECUTABLE"));
        for (const auto& game : found) {
            const auto colour = game.runtime == "il2cpp" ? Green()
                              : game.runtime == "unreal" ? Green()
                                                         : Grey();
            std::printf("%.*s%-10s%.*s %-34s %s\n",
                        static_cast<int>(colour.size()), colour.data(), game.runtime.c_str(),
                        static_cast<int>(Reset().size()), Reset().data(),
                        game.name.substr(0, 34).c_str(), game.exe.c_str());
        }
        std::printf("\n%zu game(s). Mono ones are listed and cannot be dumped: they have no "
                    "GameAssembly.dll for the IL2CPP path to talk to.\n", found.size());
    }

    if (!out_path.empty()) {
        std::ofstream manifest{std::string(out_path), std::ios::trunc};
        if (!manifest) {
            LogError("cannot write {}", out_path);
            return 5;
        }
        manifest << "# Written by zircon scan-games. Run it with: zircon batch "
                 << out_path << "\n";
        manifest << "# Delete the ones you do not want. Mono entries are commented out "
                    "because they cannot be dumped.\n\n";
        for (const auto& game : found) {
            const bool can = game.runtime == "il2cpp" || game.runtime == "unreal";
            const char* lead = can ? "" : "# ";
            manifest << lead << "[[game]]\n";
            manifest << lead << "name = " << TomlString(game.name) << "\n";
            manifest << lead << "runtime = " << TomlString(game.runtime) << "\n";
            manifest << lead << "exe = " << TomlString(game.exe) << "\n";
            if (!game.metadata.empty())
                manifest << lead << "metadata = " << TomlString(game.metadata) << "\n";

            // Written when Steam knows it. Nothing reads this yet except a person deciding
            // what to publish under, which is the job it was missing.
            if (const auto build = SteamBuildId(game.exe); !build.empty())
                manifest << lead << "label = " << TomlString("steam-" + build) << "\n";
            manifest << "\n";
        }
        std::printf("manifest written to %.*s\n", static_cast<int>(out_path.size()),
                    out_path.data());
    }
    return found.empty() ? 3 : 0;
}

// --- batch ----------------------------------------------------------------------------

struct BatchEntry {
    std::string name;
    std::string runtime;
    std::string exe;
    std::string metadata;
};

// A small TOML reader for exactly the shape scan-games writes: [[game]] tables of
// key = "value". Not a general TOML parser, and it says so rather than pretending -- a
// manifest is a list this tool wrote, or one somebody typed by copying that.
std::vector<BatchEntry> ReadManifest(std::string_view path, std::string& error) {
    std::vector<BatchEntry> entries;
    std::ifstream in{std::string(path)};
    if (!in) {
        error = "cannot open it";
        return entries;
    }

    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        // Trim, then drop blanks and comments.
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        if (line == "[[game]]") {
            entries.emplace_back();
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (entries.empty()) continue;         // a key before any [[game]]

        auto key = line.substr(0, equals);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();

        const auto value = TomlValue(line.substr(equals + 1));
        if (value.empty()) continue;

        auto& entry = entries.back();
        if (key == "name")          entry.name = value;
        else if (key == "runtime")  entry.runtime = value;
        else if (key == "exe")      entry.exe = value;
        else if (key == "metadata") entry.metadata = value;
    }
    return entries;
}

// Dumps every game in a manifest, unattended.
//
// Each one is the same work `inject --launch --wait` does, run in turn, and one failing does
// not stop the rest -- the point is to come back to a folder of dumps and a list of what did
// not work, rather than to find it stopped on the second game four hours ago.
int CommandBatch(std::string_view manifest_path, std::string_view out_dir,
                 const InjectOptions& base) {
    std::string error;
    const auto entries = ReadManifest(manifest_path, error);
    if (!error.empty()) {
        LogError("{}: {}", manifest_path, error);
        return 5;
    }
    if (entries.empty()) {
        LogError("{} has no [[game]] entries", manifest_path);
        return 1;
    }

    const std::filesystem::path folder = out_dir.empty() ? std::filesystem::path("zircon-batch")
                                                         : std::filesystem::path(out_dir);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    int failed = 0;
    int done = 0;
    std::vector<std::string> degraded;
    for (const auto& entry : entries) {
        if (entry.exe.empty() && entry.metadata.empty()) continue;

        const std::string stem = entry.name.empty() ? std::string("game") : entry.name;
        const auto out = (folder / (stem + ".json.gz")).string();
        Heading(std::format("{} ({})", stem, entry.runtime));

        int result = 0;
        if (entry.runtime == "il2cpp" && !entry.exe.empty()) {
            InjectOptions options = base;
            options.launch   = entry.exe;
            options.out_path = out;
            options.wait     = true;
            options.headless = true;
            TargetSpec spec;
            result = CommandInject(spec, options);

            // A game whose runtime faults partway through gives nothing. Its metadata still
            // gives the type system, which is most of what the dump was for, so take that
            // rather than leaving a hole in the batch.
            if (result != 0 && !entry.metadata.empty()) {
                LogWarn("{}: the live walk did not finish, falling back to its metadata",
                        stem);
                result = CommandStaticDump(entry.metadata, out);

                // Counted apart from a clean run. The fallback dump has the type system but
                // no offsets, RVAs or concrete generics, and 0.7.0 filed it under "dumped"
                // with exit 0 -- so an unattended batch reported success over dumps missing
                // the half someone wanted them for.
                if (result == 0) degraded.push_back(stem);
            }
        } else if (!entry.metadata.empty()) {
            // No executable, or a runtime the injected walk cannot reach. The file still can.
            result = CommandStaticDump(entry.metadata, out);
        } else {
            LogWarn("{}: nothing to do -- {} games are not dumpable by injection and this "
                    "entry has no metadata file", stem, entry.runtime);
            continue;
        }

        if (result == 0) {
            ++done;
        } else {
            ++failed;
            LogError("{} failed with {}; carrying on", stem, result);
        }
    }

    Heading("Batch");
    Field("dumped", "{}", done - static_cast<int>(degraded.size()));
    Field("degraded", "{}", degraded.size());
    Field("failed", "{}", failed);
    Field("output", "{}", folder.string());

    if (!degraded.empty()) {
        LogWarn("metadata only, so no offsets, RVAs or concrete generics:");
        for (const auto& name : degraded) LogWarn("  {}", name);
    }

    if (failed != 0) return 4;
    return degraded.empty() ? 0 : 3;
}

// What Zircon works out about a global-metadata.dat without being told its version.
//
// A diagnostic rather than a dump: it answers "can this file be read at all", which is the
// first question for any target that cannot be run -- an APK, a console build, a game with
// anti-cheat.
int CommandMetadata(std::string_view path) {
    if (path.empty()) {
        LogError("metadata needs a file: zircon metadata <global-metadata.dat>");
        return 1;
    }

    std::ifstream in(std::string(path), std::ios::binary | std::ios::ate);
    if (!in) {
        LogError("cannot open {}", path);
        return 5;
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);

    std::vector<std::uint8_t> bytes(size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        LogError("cannot read {}", path);
        return 5;
    }

    const auto solved = zircon::il2cpp::SolveMetadataLayout(bytes);
    if (!solved) {
        LogError("{}", solved.error().message);
        return 3;
    }
    const auto& layout = solved.value();

    FieldStrong("metadata version", std::to_string(layout.version));
    Field("file", "{} ({:.1f} MiB)", path, static_cast<double>(size) / (1024.0 * 1024.0));
    Field("header", "{} spans, {} int32 each", layout.spans.size(), layout.ints_per_entry);

    const auto rows = [&](std::string_view label, int span, int record) {
        if (span < 0) {
            Field(label, "not identified");
            return;
        }
        const auto& s = layout.Span(span);
        if (record > 0)
            Field(label, "span {}, {} records of {} bytes", span, s.size / record, record);
        else
            Field(label, "span {}, {} bytes", span, s.size);
    };

    rows("strings",    layout.tables.strings,    0);
    rows("types",      layout.tables.types,      layout.type_record);
    rows("images",     layout.tables.images,     layout.image_record);
    rows("fields",     layout.tables.fields,     layout.field_record);
    rows("methods",    layout.tables.methods,    layout.method_record);
    rows("parameters", layout.tables.parameters, layout.parameter_record);

    Evidence(layout.evidence);

    // The first type in any assembly is <Module>. Printing it is the cheapest end-to-end
    // proof that the string table and the type record agree.
    if (layout.tables.types >= 0) {
        const auto& span = layout.Span(layout.tables.types);
        const auto name = zircon::il2cpp::MetadataString(
            bytes, layout, *reinterpret_cast<const std::int32_t*>(bytes.data() + span.offset));
        Field("first type", "{}", name.empty() ? "<unreadable>" : name);
    }
    return 0;
}

// Whether a game can be dumped, before spending fifteen minutes finding out it cannot.
//
// A live dump means launching the game and waiting for its runtime. When that cannot work --
// the exe is a stub that hands off to a launcher, the store client is not running, the game
// refuses to start directly -- the way you find out is a full launch and a timeout, per
// title. Three of twelve in one test run went that way.
//
// Everything here is read from the install folder and the process list. Nothing is started.
int CommandCheck(std::string_view exe_path) {
    if (exe_path.empty()) {
        LogError("check needs an executable: zircon check <game.exe>");
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path exe{std::string(exe_path)};
    if (!std::filesystem::exists(exe, ec)) {
        LogError("no such executable: {}", exe_path);
        return 5;
    }

    const auto folder = exe.parent_path();
    Heading(exe.filename().string());
    Field("folder", "{}", folder.string());

    // --- which runtime, from the files on disk ------------------------------------------
    const auto metadata = MetadataBeside(folder);

    bool unreal = false;
    for (auto at = folder; !at.empty() && at != at.root_path(); at = at.parent_path()) {
        if (std::filesystem::exists(at / "Engine" / "Binaries", ec)) { unreal = true; break; }
    }

    std::string runtime = "unknown";
    if (!metadata.empty()) runtime = "Unity IL2CPP";
    else if (unreal)       runtime = "Unreal";
    else if (std::filesystem::exists(folder / "UnityPlayer.dll", ec))
        runtime = "Unity, but not IL2CPP";
    FieldStrong("runtime", runtime);

    // --- what starting it would involve --------------------------------------------------
    std::vector<std::string> blockers;
    std::vector<std::string> notes;

    const bool steam_marker = std::filesystem::exists(folder / "steam_appid.txt", ec);
    bool under_steamapps = false;
    for (auto at = folder; !at.empty() && at != at.root_path(); at = at.parent_path())
        if (at.filename() == "steamapps") { under_steamapps = true; break; }

    const bool epic = std::filesystem::exists(folder / ".egstore", ec);

    // Which store clients are up. A game that needs one and does not have it exits on start
    // and the payload never gets a chance.
    bool steam_running = false, epic_running = false;
    std::uint32_t already = 0;
    for (const auto& process : zircon::core::EnumerateProcesses()) {
        if (process.name == "steam.exe")             steam_running = true;
        if (process.name == "EpicGamesLauncher.exe") epic_running = true;
        if (process.name == exe.filename().string()) already = process.pid;
    }

    if (steam_marker || under_steamapps) {
        Field("store", "{}", steam_running ? "Steam, running" : "Steam, not running");
        if (!steam_running)
            blockers.emplace_back("Steam is not running, and a Steam game started directly "
                                  "usually exits straight back out");
    }
    if (epic) {
        Field("store", "{}", epic_running ? "Epic, running" : "Epic, not running");
        if (!epic_running)
            blockers.emplace_back("this is an Epic install and its launcher is not running");
    }

    if (const auto build = SteamBuildId(exe); !build.empty())
        Field("steam build", "{}", build);

    if (already != 0) {
        Field("already running", "pid {}", already);
        notes.emplace_back(
            unreal ? std::format("attach to it rather than launching it: zircon dump --pid {}",
                                 already)
                   : std::format("inject into it rather than launching it: "
                                 "zircon inject --pid {}", already));
    }

    // A tiny executable beside no runtime files starts something else and exits.
    const auto size = std::filesystem::file_size(exe, ec);
    if (!ec && size < 512 * 1024 && runtime == "unknown") {
        Field("size", "{} KiB", size / 1024);
        blockers.emplace_back("this executable is small and sits beside no runtime, so it is "
                              "probably a launcher stub rather than the game itself");
    }

    // --- the verdict ----------------------------------------------------------------------
    std::printf("\n");
    if (already != 0) {
        FieldStrong("live dump", "the game is already up");
    } else if (blockers.empty()) {
        FieldStrong("live dump", "should work");

        // Which command depends on the engine. Unity is walked from inside the process
        // because its offsets are behind a function call; Unreal is read from outside and
        // never touched. Suggesting inject for an Unreal game sends someone the long way
        // round to a payload that would refuse.
        if (unreal)
            Field("try", "zircon dump --launch \"{}\" -o dump.json.gz", exe.string());
        else
            Field("try", "zircon inject --launch \"{}\" --wait", exe.string());
    } else {
        FieldStrong("live dump", "unlikely to work as it stands");

        // The fields above go to stdout and the log goes to stderr, so without this the
        // warnings arrive before the heading they belong under.
        std::fflush(stdout);
        for (const auto& line : blockers) LogWarn("{}", line);
    }
    std::fflush(stdout);
    for (const auto& line : notes) LogInfo("{}", line);

    // The point of the whole command: a refusal is worth much more when it arrives with the
    // reading that does still work.
    if (!metadata.empty()) {
        std::printf("\n");
        FieldStrong("static dump", "works either way, with the game never started");
        Field("metadata", "{}", metadata.string());
        Field("try", "zircon dump --metadata \"{}\" -o dump.json.gz", metadata.string());
        return 0;
    }

    std::fflush(stdout);
    if (unreal)
        LogInfo("Unreal reflection only exists inside a running process, so there is no file "
                "to read instead");

    return blockers.empty() ? 0 : 3;
}

int CommandModules(const TargetSpec& spec) {
    auto source = OpenTarget(spec);
    if (!source) {
        LogError("{}", source.error().message);
        return source.error().code;
    }

    auto mem = MakeCached(std::move(source.value()));
    LogInfo("target: {}", mem->Describe());

    std::printf("%-40s %18s %12s\n", "MODULE", "BASE", "SIZE");
    for (const auto& module : mem->Modules()) {
        std::printf("%-40s %#18llx %12llu\n", module.name.c_str(),
                    static_cast<unsigned long long>(Raw(module.base)),
                    static_cast<unsigned long long>(module.size));
    }
    return 0;
}

int CommandScan(const TargetSpec& spec, std::string_view pattern_text,
                std::string_view module, bool all_regions) {
    if (pattern_text.empty()) {
        LogError("scan requires --pattern, e.g. --pattern \"48 8B 05 ? ? ? ?\"");
        return 1;
    }

    auto pattern = Pattern::Parse(pattern_text);
    if (!pattern) {
        LogError("{}", pattern.error().message);
        return pattern.error().code;
    }

    auto source = OpenTarget(spec);
    if (!source) {
        LogError("{}", source.error().message);
        return source.error().code;
    }

    auto mem = MakeCached(std::move(source.value()));
    LogInfo("target: {}", mem->Describe());
    LogInfo("pattern: {} ({} bytes)", pattern.value().ToString(), pattern.value().Size());

    ScanOptions options;
    options.module          = module;
    options.executable_only = !all_regions;

    // With no module named, "all regions" means the whole address space; scoped to a module
    // it keeps the narrower sense. Only reading of the flag that is useful in both cases.
    options.whole_process   = all_regions && module.empty();

    PatternScanner scanner(*mem);
    const auto hits = scanner.Scan(pattern.value(), options);

    if (hits.empty()) {
        LogWarn("no matches");
        return 3;
    }

    const ModuleInfo* main_module = module.empty() ? mem->MainModule()
                                                   : mem->FindModule(module);
    const std::uint64_t base = main_module ? Raw(main_module->base) : 0;

    LogInfo("{} match{}", hits.size(), hits.size() == 1 ? "" : "es");
    for (const auto& hit : hits) {
        std::printf("  %#18llx", static_cast<unsigned long long>(Raw(hit)));
        if (base) std::printf("   +%#llx", static_cast<unsigned long long>(Raw(hit) - base));
        std::printf("\n");
    }
    return 0;
}

// Unity rows for `detect`. Separate from the Unreal table because there is no score to
// print: a folder has GameAssembly.dll beside it or it doesn't.
void PrintUnityProcesses(const std::vector<zircon::il2cpp::UnityProcess>& unity) {
    if (unity.empty()) return;

    std::printf("\n");
    Heading(std::format("{:<6} {:<8} {:<40} {}", "READY", "PID", "PROCESS", "RUNTIME"));
    for (const auto& entry : unity) {
        const auto colour = entry.runtime_loaded ? Green() : Yellow();
        std::printf("%.*s%-6s%.*s %.*s%-8u%.*s %-40s %s\n",
                    static_cast<int>(colour.size()), colour.data(),
                    entry.runtime_loaded ? "yes" : "not yet",
                    static_cast<int>(Reset().size()), Reset().data(),
                    static_cast<int>(Dim().size()), Dim().data(),
                    entry.process.pid,
                    static_cast<int>(Reset().size()), Reset().data(),
                    entry.process.name.c_str(),
                    entry.runtime_loaded ? "Unity IL2CPP, GameAssembly.dll loaded"
                                         : "Unity IL2CPP, still starting");
    }
    std::printf("%.*sUnity is dumped from inside: zircon inject --pid <n>%.*s\n",
                static_cast<int>(Dim().size()), Dim().data(),
                static_cast<int>(Reset().size()), Reset().data());
}

int CommandDetect() {
    const auto candidates = zircon::engine::DetectUnrealProcesses(0.2f);
    const auto unity      = zircon::il2cpp::DetectUnityProcesses();

    if (candidates.empty() && unity.empty()) {
        LogWarn("no Unreal Engine or Unity IL2CPP processes detected");
        return 3;
    }

    if (candidates.empty()) {
        PrintUnityProcesses(unity);
        return 0;
    }

    Heading(std::format("{:<6} {:<8} {:<40} {}", "SCORE", "PID", "PROCESS", "PROJECT"));
    for (const auto& candidate : candidates) {
        // The one number worth reading at a glance. A launcher spawns several processes
        // and exactly one of them is the game.
        const auto colour = candidate.confidence >= 0.75f ? Green()
                          : candidate.confidence >= 0.40f ? Yellow()
                                                          : Grey();
        std::printf("%.*s%5.0f%%%.*s %.*s%-8u%.*s %-40s %s\n",
                    static_cast<int>(colour.size()), colour.data(),
                    candidate.confidence * 100.0,
                    static_cast<int>(Reset().size()), Reset().data(),
                    static_cast<int>(Dim().size()), Dim().data(),
                    candidate.process.pid,
                    static_cast<int>(Reset().size()), Reset().data(),
                    candidate.process.name.c_str(),
                    candidate.project.c_str());
        if (GetLogLevel() <= LogLevel::Debug) {
            for (const auto& reason : candidate.evidence)
                std::printf("         %.*s- %s%.*s\n",
                            static_cast<int>(Dim().size()), Dim().data(), reason.c_str(),
                            static_cast<int>(Reset().size()), Reset().data());
        }
    }

    PrintUnityProcesses(unity);
    return 0;
}

int CommandFingerprint(const TargetSpec& spec, bool as_json) {
    // Machine-readable means only the object on stdout. The runtime search logs a line of
    // its own on the way past and a parser has no way to know it isn't part of the answer.
    if (as_json) SetLogLevel(LogLevel::Error);

    auto source = OpenTarget(spec);
    if (!source) {
        // Even a failure is JSON when JSON was asked for. A script that has to tell an
        // error apart by whether the output parses is a script that will get it wrong.
        if (as_json)
            std::printf("{\"ok\": false, \"error\": %s}\n",
                        JsonQuote(source.error().message).c_str());
        else
            LogError("{}", source.error().message);
        return source.error().code;
    }

    auto mem = MakeCached(std::move(source.value()));
    if (!as_json) LogInfo("target: {}", mem->Describe());

    // Unity first. IL2CPP is a yes/no (a module exports the API or it doesn't), the Unreal
    // fingerprint is a score that will happily guess low about a non-Unreal game.
    if (const auto unity = zircon::il2cpp::FindRuntime(*mem)) {
        if (as_json) {
            std::printf("{\"ok\": true, \"runtime\": \"il2cpp\", \"module\": %s, "
                        "\"module_base\": %llu, \"entry_points_resolved\": %d, "
                        "\"entry_points_required\": %d, \"entry_points_optional\": %d, "
                        "\"optional_resolved\": %d, \"complete\": %s, "
                        "\"confidence\": %.2f, \"missing\": %s, \"evidence\": %s}\n",
                        JsonQuote(unity->module_name).c_str(),
                        static_cast<unsigned long long>(Raw(unity->module_base)),
                        unity->api.resolved,
                        zircon::il2cpp::RequiredEntryPointCount(),
                        zircon::il2cpp::OptionalEntryPointCount(),
                        unity->api.enrichment,
                        unity->api.Complete() ? "true" : "false",
                        unity->confidence,
                        JsonList(unity->api.missing).c_str(),
                        JsonList(unity->evidence).c_str());
            return unity->api.Complete() ? 0 : 3;
        }

        FieldStrong("runtime", "Unity IL2CPP");
        Field("module", "{} at {:#x}", unity->module_name, Raw(unity->module_base));
        Field("api", "{}/{} entry points resolved", unity->api.resolved,
              unity->api.resolved + static_cast<int>(unity->api.missing.size()));
        Field("confidence", "{:.0f}%", unity->confidence * 100.0);
        Evidence(unity->evidence);

        if (!unity->api.Complete()) {
            LogWarn("this build does not export everything the walk needs; "
                    "a dump would be incomplete");
            return 3;
        }
        return 0;
    }

    const auto profile = zircon::engine::FingerprintEngine(*mem);

    if (as_json) {
        std::printf("{\"ok\": true, \"runtime\": \"unreal\", \"version\": %s, "
                    "\"known\": %s, \"confidence\": %.2f, \"fproperty\": %s, "
                    "\"chunked_name_pool\": %s, \"chunked_gobjects\": %s, "
                    "\"evidence\": %s}\n",
                    JsonQuote(profile.VersionString()).c_str(),
                    profile.Known() ? "true" : "false",
                    profile.confidence,
                    profile.uses_fproperty ? "true" : "false",
                    profile.chunked_name_pool ? "true" : "false",
                    profile.chunked_gobjects ? "true" : "false",
                    JsonList(profile.evidence).c_str());
        return profile.Known() ? 0 : 3;
    }

    FieldStrong("engine version", profile.VersionString());
    Field("confidence", "{:.0f}%", profile.confidence * 100.0);
    Field("property model", profile.uses_fproperty ? "FProperty" : "UProperty");
    Field("name pool", profile.chunked_name_pool ? "FNamePool" : "TNameEntryArray");
    Field("object array", profile.chunked_gobjects ? "chunked" : "fixed");
    Evidence(profile.evidence);

    // Non-zero when no version could be determined, so scripts can branch on it.
    return profile.Known() ? 0 : 3;
}

// Everything P1 discovers about a target, resolved once and shared by whichever commands
// want it. Ordered by dependency: the pool needs nothing, the layout needs both the array
// and the pool.
struct Session {
    std::unique_ptr<IMemorySource>  memory;
    zircon::engine::EngineProfile   profile;
    zircon::engine::ObjectArrayInfo array;
    zircon::engine::NamePoolInfo    pool;
    zircon::engine::UObjectLayout   layout;
    zircon::engine::UStructLayout   structs;
    zircon::engine::FPropertyLayout props;
    zircon::engine::SubclassLayout  subclass;
    zircon::engine::UFunctionLayout functions;
    zircon::engine::UEnumLayout     enums;
    zircon::engine::UClassLayout    classes;

    // Built on demand, never stored. A ResolveContext holds pointers into this object's own
    // members, so a cached one dangles the moment the Session moves, which it does on the
    // way out of OpenSession into the caller's optional.
    zircon::engine::ResolveContext Context() {
        zircon::engine::ResolveContext context;
        context.memory          = memory.get();
        context.array           = &array;
        context.pool            = &pool;
        context.object_layout   = &layout;
        context.struct_layout   = &structs;
        context.property_layout = &props;
        context.subclass_layout = &subclass;
        context.profile         = &profile;
        context.enum_layout     = &enums;
        return context;
    }
};

std::optional<Session> OpenSession(const TargetSpec& spec, bool need_layout) {
    auto source = OpenTarget(spec);
    if (!source) {
        LogError("{}", source.error().message);
        return std::nullopt;
    }

    Session session;
    session.memory = MakeCached(std::move(source.value()));
    LogInfo("target: {}", session.memory->Describe());

    session.profile = zircon::engine::FingerprintEngine(*session.memory);

    auto array = zircon::engine::FindObjectArray(*session.memory, session.profile);
    if (array) {
        session.array = *array;
    } else if (need_layout) {
        LogError("could not locate the object array");
        return std::nullopt;
    } else {
        // The name pool does not depend on the object array, and nor does dumping it.
        // Refusing here anyway coupled the two for nothing, and the one time that matters is
        // exactly when the array is the thing that will not derive. `names` is then the
        // quickest way to find out how much of the target *is* readable.
        LogWarn("no object array; continuing, since this command does not need one");
    }

    auto pool = zircon::engine::FindNamePool(*session.memory, session.profile);
    if (!pool) {
        LogError("could not locate the FName pool");
        return std::nullopt;
    }
    session.pool = *pool;

    if (need_layout) {
        session.layout = zircon::engine::DeriveObjectLayout(*session.memory, session.array,
                                                            session.pool);
        if (!session.layout.Valid()) {
            LogError("could not derive the UObject layout");
            return std::nullopt;
        }

        session.structs = zircon::engine::DeriveStructLayout(
            *session.memory, session.array, session.pool, session.layout);

        if (session.structs.Valid()) {
            session.props = zircon::engine::DerivePropertyLayout(
                *session.memory, session.array, session.pool, session.layout,
                session.structs);
        }

        if (session.structs.Valid()) {
            session.classes = zircon::engine::DeriveClassLayout(
                *session.memory, session.array, session.pool, session.layout,
                session.structs);
        }

        if (session.props.Valid()) {
            session.subclass = zircon::engine::DeriveSubclassLayout(
                *session.memory, session.array, session.pool, session.layout,
                session.structs, session.props);

            session.enums = zircon::engine::DeriveEnumLayout(
                *session.memory, session.array, session.pool, session.layout);

            session.functions = zircon::engine::DeriveFunctionLayout(
                *session.memory, session.array, session.pool, session.layout,
                session.structs, session.props);
        }
    }

    return session;
}

// Puts this executable's folder on the user's PATH so `zircon` works from anywhere, not
// just the build output directory.
//
// There is no installer and there should not be one. The whole tool is three files in a
// folder, and an MSI that copies them elsewhere adds an uninstall problem without solving
// a real one. HKCU\Environment only: per-user, no elevation, nothing outside the current
// account, and `zircon uninstall` puts it back.
int CommandInstall(bool remove) {
    wchar_t self[MAX_PATH * 2] = {};
    if (::GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self))) == 0) {
        LogError("could not determine where this executable lives");
        return 1;
    }
    const std::wstring directory = std::filesystem::path(self).parent_path().wstring();

    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ | KEY_WRITE, &key) !=
        ERROR_SUCCESS) {
        LogError("could not open HKCU\\Environment");
        return 4;
    }

    // Preserve REG_EXPAND_SZ. The existing value very often contains %USERPROFILE%, and
    // rewriting it as a plain string freezes those references where they stand.
    DWORD type = REG_SZ;
    DWORD bytes = 0;
    std::wstring current;
    if (::RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        bytes > sizeof(wchar_t)) {
        current.resize(bytes / sizeof(wchar_t));
        ::RegQueryValueExW(key, L"Path", nullptr, &type,
                           reinterpret_cast<LPBYTE>(current.data()), &bytes);
        while (!current.empty() && current.back() == L'\0') current.pop_back();
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) type = REG_EXPAND_SZ;

    // Split on ';' to match an entry exactly. A substring test calls "C:\tools\zircon"
    // already present because "C:\tools\zircon-old" is.
    //
    // Entries are kept **verbatim**. Trailing separators and spaces are ignored for
    // comparison and never rewritten. This is the user's PATH, and every entry in it bar
    // ours belongs to some other program entitled to its own spelling.
    std::vector<std::wstring> entries;
    for (std::size_t start = 0; start <= current.size();) {
        const auto end = current.find(L';', start);
        auto piece = current.substr(start, end == std::wstring::npos ? end : end - start);
        if (!piece.empty()) entries.push_back(std::move(piece));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    const auto normalise = [](std::wstring text) {
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\\'))
            text.pop_back();
        return text;
    };

    const std::wstring wanted = normalise(directory);

    const auto same = [](const std::wstring& a, const std::wstring& b) {
        return a.size() == b.size() &&
               ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                      b.c_str(), static_cast<int>(b.size()),
                                      TRUE) == CSTR_EQUAL;
    };

    const auto it = std::find_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
        return same(normalise(e), wanted);
    });
    const bool present = it != entries.end();

    if (remove) {
        if (!present) {
            ::RegCloseKey(key);
            LogWarn("not on PATH; nothing to remove");
            return 0;
        }
        entries.erase(it);
    } else {
        if (present) {
            ::RegCloseKey(key);
            Field("already on PATH", std::filesystem::path(wanted).string());
            return 0;
        }
        entries.push_back(wanted);
    }

    std::wstring updated;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) updated += L';';
        updated += entries[i];
    }

    const auto written = ::RegSetValueExW(
        key, L"Path", 0, type, reinterpret_cast<const BYTE*>(updated.c_str()),
        static_cast<DWORD>((updated.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);

    if (written != ERROR_SUCCESS) {
        LogError("could not write PATH (error {})", written);
        return 4;
    }

    // Without this only processes started after the next sign-in see the change. Explorer
    // picks the broadcast up and hands it to new consoles.
    ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                          reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG,
                          5000, nullptr);

    if (remove) {
        FieldStrong("removed from PATH", std::filesystem::path(wanted).string());
    } else {
        FieldStrong("added to PATH", std::filesystem::path(wanted).string());
        Field("", "open a new terminal, then `zircon detect` works from anywhere");
    }
    return 0;
}

// The one operation that needs a pid specifically. A name is ambiguous, and a dump or a PE
// on disk has no process to load anything into.
bool ProcessAlive(std::uint32_t pid) {
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) return false;

    DWORD code = 0;
    const bool running = ::GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    ::CloseHandle(handle);
    return running;
}

// Starts a game and waits for its IL2CPP runtime to come up.
//
// The tester's point, and he is right: the runtime already says when it is ready. Polling
// for GameAssembly.dll plus the entry points beats guessing a sleep per game, which is
// "always wrong twice" -- too short on a cold disk, wasted on a warm one.
std::uint32_t LaunchAndWait(const std::string& exe, int timeout_seconds) {
    const std::filesystem::path path{exe};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        LogError("no such executable: {}", exe);
        return 0;
    }

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);

    // Its own directory, because a game started from somewhere else looks for its data
    // beside the working directory and usually just exits.
    const auto folder  = path.parent_path().wstring();
    auto       command = L"\"" + path.wstring() + L"\"";

    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          folder.empty() ? nullptr : folder.c_str(), &startup, &process)) {
        LogError("could not start {} (error {})", exe, ::GetLastError());
        return 0;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    const std::uint32_t pid = process.dwProcessId;
    LogInfo("started {} as pid {}", path.filename().string(), pid);

    for (int elapsed = 0; elapsed < timeout_seconds; ++elapsed) {
        if (!ProcessAlive(pid)) {
            // Launchers do this: the exe you start hands off to another process and exits.
            for (const auto& other : zircon::il2cpp::DetectUnityProcesses()) {
                if (!other.runtime_loaded) continue;
                LogInfo("{} handed off to {} (pid {})", path.filename().string(),
                        other.process.name, other.process.pid);
                return other.process.pid;
            }
            LogError("{} exited before its runtime came up", path.filename().string());
            return 0;
        }

        for (const auto& candidate : zircon::il2cpp::DetectUnityProcesses()) {
            if (candidate.process.pid != pid || !candidate.runtime_loaded) continue;
            LogInfo("GameAssembly.dll mapped after {}s", elapsed);
            return pid;
        }
        ::Sleep(1000);
    }

    LogError("{} did not map GameAssembly.dll within {}s", path.filename().string(),
             timeout_seconds);
    return 0;
}

// The same thing for an Unreal game, which needs a different signal.
//
// GObjects is built during engine init, not at process start, so a dump taken the moment the
// window appears finds "no object array found in N writable region(s)". Unity got launch
// automation in 0.7.0 and Unreal did not, so the readiness problem --launch exists to solve
// was left unsolved on that half.
//
// The readiness test is a dump attempt, because nothing cheaper is honest. Scoring the
// process as Unreal was the first thing tried and it is useless here: the score is about what
// is mapped into the process, which is true from the first instant, so it reported ready at 0s
// and the dump then failed with "no object array found in 39 writable region(s)" -- the very
// error this was meant to prevent.
//
// So it asks the real question, repeatedly: can the reflection layout be derived yet. That
// costs a scan per attempt, which is why it backs off rather than spinning.
std::uint32_t LaunchAndWaitUnreal(const std::string& exe, int timeout_seconds) {
    const std::filesystem::path path{exe};

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        LogError("no such executable: {}", exe);
        return 0;
    }

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);

    const auto folder  = path.parent_path().wstring();
    auto       command = L"\"" + path.wstring() + L"\"";

    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          folder.empty() ? nullptr : folder.c_str(), &startup, &process)) {
        LogError("could not start {} (error {})", exe, ::GetLastError());
        return 0;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    const std::uint32_t pid = process.dwProcessId;
    LogInfo("started {} as pid {}", path.filename().string(), pid);

    // Which process to ask. A launcher hands off: the exe that was started exits and the
    // game is something else, so the pid can move once.
    std::uint32_t target = pid;
    bool said = false;

    for (int elapsed = 0; elapsed < timeout_seconds; elapsed += 3) {
        if (!ProcessAlive(target)) {
            std::uint32_t moved = 0;
            for (const auto& candidate : zircon::engine::DetectUnrealProcesses(0.5f))
                if (candidate.process.pid != pid) { moved = candidate.process.pid; break; }

            if (moved == 0) {
                LogError("{} exited before its engine came up", path.filename().string());
                return 0;
            }
            LogInfo("{} handed off to pid {}", path.filename().string(), moved);
            target = moved;
        }

        TargetSpec spec;
        spec.kind = TargetSpec::Kind::Pid;
        spec.pid  = target;

        // Quiet while probing. A half-started engine makes the derivation complain about
        // exactly the things that are not built yet, and one of those every three seconds
        // for a minute reads like a fault rather than like waiting. The attempt that
        // succeeds is the one whose output matters, and CommandDump runs it again at full
        // volume straight after.
        const auto volume = zircon::core::GetLogLevel();
        zircon::core::SetLogLevel(zircon::core::LogLevel::Error);

        bool ready = false;
        if (auto source = OpenTarget(spec)) {
            auto memory = MakeCached(std::move(source.value()));
            ready = zircon::engine::Reflect(*memory).Valid();
        }
        zircon::core::SetLogLevel(volume);

        if (ready) {
            LogInfo("the object array is up after {}s", elapsed);
            return target;
        }

        if (!said) {
            LogInfo("waiting for the engine to build its object array; a game that is still "
                    "on a loading screen has not done it yet");
            said = true;
        }
        ::Sleep(3000);
    }

    LogError("{} did not build an object array within {}s -- it may still be on a splash "
             "screen, so try a longer --timeout", path.filename().string(), timeout_seconds);
    return 0;
}

// Blocks until the payload says it is done, the game dies, or the clock runs out. The
// signal is a file the payload writes once at the end -- waiting on the dump file instead
// means racing a 600 MB write, which is non-empty long before it is finished.
int WaitForPayload(std::uint32_t pid, const std::filesystem::path& status, int timeout_seconds) {
    LogInfo("waiting for the walk to finish (up to {}s)", timeout_seconds);

    for (int elapsed = 0; elapsed < timeout_seconds; ++elapsed) {
        std::ifstream in(status);
        if (in) {
            std::string outcome, detail;
            std::getline(in, outcome);
            std::getline(in, detail);
            in.close();

            std::error_code ec;
            std::filesystem::remove(status, ec);

            if (outcome == "ok") {
                std::printf("done in %ds: %s\n", elapsed, detail.c_str());
                return 0;
            }
            LogError("the payload gave up: {}", detail);
            return 4;
        }

        if (!ProcessAlive(pid)) {
            LogError("pid {} died after {}s without finishing; the log under zircon-out "
                     "says what it was reading", pid, elapsed);
            return 4;
        }
        ::Sleep(1000);
    }

    LogError("still walking after {}s; raise --timeout if this game is just slow", timeout_seconds);
    return 4;
}

int CommandInject(const TargetSpec& spec, const InjectOptions& options) {
    const std::string_view out_path = options.out_path;
    const std::string_view dll_path = options.dll_path;
    const bool             headless = options.headless;

    std::uint32_t pid = spec.pid;

    if (!options.launch.empty()) {
        pid = LaunchAndWait(options.launch, options.timeout);
        if (pid == 0) return 4;
    } else if (spec.kind == TargetSpec::Kind::ProcessName) {
        std::vector<std::uint32_t> matches;
        for (const auto& process : zircon::core::EnumerateProcesses())
            if (process.name.find(spec.value) != std::string::npos)
                matches.push_back(process.pid);

        // Same refusal the external provider makes. Loading a payload into the wrong game
        // is not something to guess at.
        if (matches.size() != 1) {
            LogError("'{}' matches {} processes; use --pid", spec.value, matches.size());
            return 2;
        }
        pid = matches.front();
    } else if (spec.kind != TargetSpec::Kind::Pid) {
        LogError("inject needs a running target: --pid <n> or --process <name>");
        return 1;
    }

    // The payload built alongside this executable: where it lands, and what is almost
    // always meant.
    std::filesystem::path payload{dll_path};
    if (payload.empty()) {
        wchar_t self[MAX_PATH * 2] = {};
        ::GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        payload = std::filesystem::path(self).parent_path() / "zircon.dll";
    }

    // Where the payload will say how it went. Named after the pid so two injections at
    // once do not read each other's.
    std::error_code ec;
    const auto status = std::filesystem::temp_directory_path(ec) /
                        std::format("zircon-{}.status", pid);
    std::filesystem::remove(status, ec);

    // Written before the load. Paths go in absolute, because the payload resolves them
    // inside the game's working directory and that is rarely this one.
    const auto handoff = payload.parent_path() / kPayloadHandoff;
    const bool anything_to_say = !out_path.empty() || headless || options.wait ||
                                 options.settle > 0 || !options.mode.empty();
    if (!anything_to_say) {
        std::filesystem::remove(handoff, ec);
    } else {
        std::ofstream note(handoff, std::ios::trunc);
        if (!note) {
            LogError("cannot write {}, so the payload would not see these options",
                     handoff.string());
            return 5;
        }
        if (!out_path.empty()) {
            const auto absolute = std::filesystem::absolute(out_path, ec);
            note << "out=" << (ec ? std::filesystem::path(out_path) : absolute).string()
                 << "\n";
        }
        if (headless)              note << "headless=1\n";
        if (options.wait)          note << "status=" << status.string() << "\n";
        if (options.settle > 0)    note << "settle=" << options.settle << "\n";
        if (!options.mode.empty()) note << "mode=" << options.mode << "\n";
    }

    LogInfo("injecting {} into pid {}", payload.string(), pid);
    if (const auto result = zircon::core::Inject(pid, payload.string()); !result) {
        LogError("{}", result.error().message);
        std::filesystem::remove(handoff, ec);
        return 4;
    }

    std::printf("payload loaded into pid %u\n", pid);
    if (!out_path.empty())
        std::printf("the dump will be written to %s\n",
                    std::filesystem::absolute(out_path, ec).string().c_str());
    if (!headless)
        std::printf("it opens its own console and window inside the game; output goes to %s\n",
                    (payload.parent_path() / "zircon-out").string().c_str());
    else
        std::printf("no console, as asked; the log is in %s\n",
                    (payload.parent_path() / "zircon-out" / "logs").string().c_str());

    if (!options.wait) return 0;

    const int result = WaitForPayload(pid, status, options.timeout);

    // Started by us, so it is ours to close. A game left running after a batch of these is
    // a machine with twelve games on it.
    if (result == 0 && !options.launch.empty()) {
        if (HANDLE handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            ::TerminateProcess(handle, 0);
            ::CloseHandle(handle);
            LogInfo("closed pid {}", pid);
        }
    }
    return result;
}

// Hands off to zircon-gui.exe next door. The browser is a separate binary since it drags
// in a renderer the CLI doesn't need - but `browse` has been sitting in the help and the
// readme the whole time, and printing "that's not a command" for something the help
// lists is the worst of both.
int CommandBrowse(const TargetSpec& spec) {
    wchar_t self[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    const auto gui = std::filesystem::path(self).parent_path() / "zircon-gui.exe";

    std::error_code ec;
    if (!std::filesystem::exists(gui, ec)) {
        LogError("the browser is a separate binary and is not next to this one: {}",
                 gui.string());
        LogError("build the zircon-gui target, or run zircon-gui.exe directly");
        return 4;
    }

    // only a pid is any use to it. the GUI attaches externally, so --dump and --file have
    // nothing to browse - say so instead of opening a picker that ignores the argument.
    std::wstring arguments = L"\"" + gui.wstring() + L"\"";
    if (spec.kind == TargetSpec::Kind::Pid) {
        arguments += L" --attach " + std::to_wstring(spec.pid);
    } else if (spec.kind == TargetSpec::Kind::ProcessName) {
        std::vector<std::uint32_t> matches;
        for (const auto& process : zircon::core::EnumerateProcesses())
            if (process.name.find(spec.value) != std::string::npos)
                matches.push_back(process.pid);
        if (matches.size() != 1) {
            LogError("'{}' matches {} processes; use --pid", spec.value, matches.size());
            return 2;
        }
        arguments += L" --attach " + std::to_wstring(matches.front());
    } else if (spec.kind != TargetSpec::Kind::None) {
        LogError("browse attaches to a running process; --pid or --process, or neither "
                 "to pick from a list");
        return 1;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};

    if (!::CreateProcessW(gui.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                          0, nullptr, nullptr, &startup, &info)) {
        LogError("could not start the browser: error {}", ::GetLastError());
        return 4;
    }
    ::CloseHandle(info.hThread);
    ::CloseHandle(info.hProcess);

    // don't wait on it. the browser owns its own window, and blocking here would make this
    // useless from a script.
    Field("browser", gui.string());
    return 0;
}

int CommandObjects(const TargetSpec& spec, std::string_view filter, int limit) {
    auto session = OpenSession(spec, true);
    if (!session) return 4;

    const auto& array  = session->array;
    const auto& layout = session->layout;

    std::printf("GObjects         %#llx   (%d objects, %d chunks of %u)\n",
                static_cast<unsigned long long>(Raw(array.gobjects)),
                array.num_elements, array.num_chunks, array.elements_per_chunk);
    std::printf("FNamePool        %#llx\n",
                static_cast<unsigned long long>(Raw(session->pool.blocks)));
    std::printf("UObject layout   index +%#x  class +%#x  name +%#x  outer +%#x\n",
                layout.index_offset, layout.class_offset, layout.name_offset,
                layout.outer_offset);
    std::printf("confidence       array %.0f%%  pool %.0f%%  layout %.0f%%\n\n",
                array.confidence * 100.0, session->pool.confidence * 100.0,
                layout.confidence * 100.0);

    int printed = 0;
    // Counted separately because they mean opposite things. An empty slot is normal:
    // NumElements is the array's high-water mark, and an unloaded level leaves a run of
    // nulls behind. A live object whose name will not resolve is a defect.
    //
    // Reported together, FF7 Rebirth looked like it was failing a quarter of its names when
    // it had 52421 free slots and was resolving every object it actually had.
    int resolved = 0;
    int empty    = 0;
    for (std::int32_t i = 0; i < array.num_elements; ++i) {
        const auto object = zircon::engine::ObjectAt(*session->memory, array, i);
        if (IsNull(object)) { ++empty; continue; }

        const std::string full = zircon::engine::GetObjectFullName(
            *session->memory, layout, session->pool, object);
        if (full.empty()) continue;
        ++resolved;

        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        if (limit > 0 && printed >= limit) continue;

        std::printf("[%6d] %#14llx  %s\n", i,
                    static_cast<unsigned long long>(Raw(object)), full.c_str());
        ++printed;
    }

    std::fprintf(stderr, "\n%d empty slots (free, not a failure)\n", empty);
    std::fprintf(stderr, "%d of %d live objects resolved to names\n", resolved,
                 array.num_elements - empty);
    return 0;
}

// Annotated hexdump of one object. Deriving a layout blind is guesswork, so when a
// derivation disagrees with expectations this shows what is actually in memory.
int CommandInspect(const TargetSpec& spec, std::string_view path, int limit) {
    if (path.empty()) {
        LogError("inspect requires --filter <path>, e.g. --filter /Script/Engine.Actor, "
                 "or --filter #<index> to reach an object by array slot");
        return 1;
    }

    // Slot addressing needs the object array and nothing else, so do not gate it on a
    // layout derivation that may be the very thing under investigation.
    const bool by_slot    = path.size() > 1 && path.front() == '#';
    const bool by_address = path.size() > 2 && path.front() == '@';

    // A raw address needs nothing derived at all: not the layout, not the name pool, not
    // even the object array. That is the point. When the pool is the thing that will not
    // derive, this is the only way to look at the memory and find out why.
    if (by_address) {
        auto source = OpenTarget(spec);
        if (!source) {
            LogError("{}", source.error().message);
            return source.error().code;
        }
        auto memory = MakeCached(std::move(source.value()));

        const auto at = static_cast<Address>(
            std::strtoull(std::string(path.substr(1)).c_str(), nullptr, 0));
        if (IsNull(at)) {
            LogError("could not parse an address from '{}'", path);
            return 3;
        }

        const int bytes = limit > 0 ? limit : 0xA0;
        std::printf("%#llx\n\n", static_cast<unsigned long long>(Raw(at)));
        for (int offset = 0; offset + 8 <= bytes; offset += 8) {
            std::uint64_t qword{};
            if (!ReadInto(*memory, at + offset, qword)) { std::printf("  +%-6X <unreadable>\n", offset); continue; }

            // When chasing a table, what the pointer leads to is the interesting part.
            char text[17]{};
            const bool followed = memory->Read(static_cast<Address>(qword), text, 16) == 16;

            std::string preview;
            if (followed)
                for (int i = 0; i < 16; ++i)
                    preview += (static_cast<unsigned char>(text[i]) >= 32 &&
                                static_cast<unsigned char>(text[i]) < 127) ? text[i] : '.';

            std::printf("  +%-6X %016llx  %s\n", offset,
                        static_cast<unsigned long long>(qword), preview.c_str());
        }
        return 0;
    }

    auto session = OpenSession(spec, !by_slot);
    if (!session) return 4;

    // "#<n>" addresses an object by its GObjects slot instead of by name.
    //
    // Needed exactly when names are the thing that is broken. Deriving the UObject layout
    // depends on resolving names, and resolving names depends on the layout; when a target
    // breaks that circle there is otherwise no way to look at an object at all. A slot
    // index needs neither.
    Address object{};
    if (by_address) {
        // "@0x..." reaches any address at all, for following a pointer out of a hexdump
        // when nothing symbolic works yet.
        object = static_cast<Address>(
            std::strtoull(std::string(path.substr(1)).c_str(), nullptr, 0));
        if (IsNull(object)) {
            LogError("could not parse an address from '{}'", path);
            return 3;
        }
    } else if (by_slot) {
        const std::int32_t index = std::atoi(std::string(path.substr(1)).c_str());
        object = zircon::engine::ObjectAt(*session->memory, session->array, index);
        if (IsNull(object)) {
            LogError("no object in slot {}", index);
            return 3;
        }
    } else {
        object = zircon::engine::FindObjectByPath(
            *session->memory, session->array, session->layout, session->pool, path);
        if (IsNull(object)) {
            LogError("no object with path '{}'", path);
            return 3;
        }
    }

    const int bytes = limit > 0 ? limit : 0xA0;
    std::printf("%s\n  at %#llx\n\n", std::string(path).c_str(),
                static_cast<unsigned long long>(Raw(object)));
    std::printf("%-8s %-20s %-12s %s\n", "OFFSET", "QWORD", "INT32", "INTERPRETATION");

    for (int offset = 0; offset + 8 <= bytes; offset += 8) {
        std::uint64_t qword{};
        std::int32_t  low{}, high{};
        ReadInto(*session->memory, object + offset, qword);
        ReadInto(*session->memory, object + offset, low);
        ReadInto(*session->memory, object + offset + 4, high);

        std::string note;

        // Is it a pointer to another object in the array? That is the single most useful
        // thing to know about any slot in a UObject.
        const auto as_address = static_cast<Address>(qword);
        std::int32_t index{};
        if (ReadInto(*session->memory, as_address + session->array.index_offset, index) &&
            index >= 0 && index < session->array.num_elements &&
            Raw(zircon::engine::ObjectAt(*session->memory, session->array, index)) == qword) {
            note = "-> " + zircon::engine::GetObjectPathName(
                *session->memory, session->layout, session->pool, as_address);
        } else if (qword != 0) {
            std::uint8_t probe[8];
            if (session->memory->Read(as_address, probe, sizeof(probe)) == sizeof(probe))
                note = "(readable, not an array object)";
        }

        // The same bytes read as an FName often identify a name field immediately.
        if (note.empty()) {
            const std::string name = zircon::engine::ResolveName(
                *session->memory, session->pool, static_cast<std::uint32_t>(low));
            if (!name.empty()) note = "FName? \"" + name + "\"";
        }

        std::printf("+%-#7x %-#20llx %-12d %s\n", offset,
                    static_cast<unsigned long long>(qword), low, note.c_str());
        if (high != 0 && note.empty())
            std::printf("%-8s %-20s %-12d (high half)\n", "", "", high);
    }
    return 0;
}

int CommandDiff(std::string_view before_path, std::string_view after_path,
                std::string_view out_path, std::string_view filter, std::string_view style,
                bool breaking_only) {
    if (before_path.empty() || after_path.empty()) {
        LogError("diff needs two dumps, e.g. zircon diff old.json new.json");
        return 1;
    }

    auto before = LoadDump(before_path);
    if (!before.ok()) { LogError("{}: {}", before_path, before.error().message); return 6; }

    auto after = LoadDump(after_path);
    if (!after.ok()) { LogError("{}: {}", after_path, after.error().message); return 6; }

    zircon::diff::DiffOptions options;
    options.package_filter = filter;
    if (breaking_only) {
        options.include_additions = false;
        options.minimum           = zircon::diff::Severity::High;
    }

    const auto result = zircon::diff::Diff(before.value(), after.value(), options);

    std::string rendered;
    if (style == "json") {
        rendered = zircon::diff::RenderJson(result);
    } else if (style == "markdown" || style == "md") {
        rendered = zircon::diff::RenderMarkdown(result, before.value(), after.value());
    } else {
        // Never colour a file: escape codes written into one corrupt it for everything
        // downstream. Colours() has already decided the rest -- terminal or not, NO_COLOR,
        // and an explicit --color/--no-color.
        const bool colour = out_path.empty() && Colours();
        rendered = zircon::diff::RenderText(result, before.value(), after.value(), colour);
    }

    if (out_path.empty()) {
        std::fwrite(rendered.data(), 1, rendered.size(), stdout);
    } else {
        std::string error;
        if (!zircon::emit::util::WriteFile(out_path, rendered, error)) {
            LogError("{}", error);
            return 5;
        }
        LogInfo("wrote {}", out_path);
    }

    // Exit code carries the verdict so a build script can gate on it: 0 clean, 8 broken.
    return result.HasBreakingChanges() ? 8 : 0;
}

// Emitters warn per occurrence, which is right -- the emitter does not know how many
// times it is about to say the same thing. Printing all of them is not: one unrepresentable
// property type produced seventy-one identical lines, which pushed the actual result off
// the screen and made a solved problem look like a broken run.
//
// So identical warnings are collapsed and counted. Nothing is hidden: the text is shown
// once with how often it happened, and the total is still reported.
void ReportWarnings(const std::vector<std::string>& warnings) {
    if (warnings.empty()) return;

    // Insertion-ordered, because the first warning is usually the most informative and
    // sorting them alphabetically would bury it.
    std::vector<std::pair<std::string, std::size_t>> unique;
    for (const auto& warning : warnings) {
        const auto it = std::find_if(unique.begin(), unique.end(),
                                     [&](const auto& entry) { return entry.first == warning; });
        if (it == unique.end()) unique.emplace_back(warning, 1);
        else                    ++it->second;
    }

    // Even deduplicated, an SDK emit can warn about forty different delegate properties,
    // and forty lines of the same shape are no more informative than eight. The rest are
    // a flag away, and the count is always printed, so nothing disappears unannounced.
    constexpr std::size_t kMaxShown = 8;
    const bool verbose = GetLogLevel() <= LogLevel::Debug;
    const std::size_t shown = verbose ? unique.size() : std::min(unique.size(), kMaxShown);

    for (std::size_t i = 0; i < shown; ++i) {
        const auto& [text, count] = unique[i];
        if (count == 1) LogWarn("{}", text);
        else            LogWarn("{} (x{})", text, count);
    }

    if (shown < unique.size())
        LogWarn("... and {} more distinct warnings; -v to see them all",
                unique.size() - shown);

    if (unique.size() != warnings.size())
        LogWarn("{} warnings, {} distinct", warnings.size(), unique.size());
}

// "cpp_sdk,usmap,json", or "all". Empty entries get dropped so a trailing comma isn't
// an error nobody would have expected.
std::vector<std::string> SplitFormats(std::string_view spec) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? spec.size() : comma;
        std::string_view piece = spec.substr(start, end - start);
        while (!piece.empty() && piece.front() == ' ') piece.remove_prefix(1);
        while (!piece.empty() && piece.back() == ' ')  piece.remove_suffix(1);
        if (!piece.empty()) out.emplace_back(piece);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

// Resolve a format list. Reports every bad name, not just the first - if you mistyped two
// of five you want both now, not on the next run.
bool ResolveEmitters(std::string_view spec,
                     std::vector<const zircon::emit::Emitter*>& out) {
    if (spec == "all") {
        for (const auto& emitter : zircon::emit::Emitters()) out.push_back(&emitter);
        return true;
    }

    bool ok = true;
    for (const auto& name : SplitFormats(spec)) {
        const auto* emitter = zircon::emit::FindEmitter(name);
        if (!emitter) {
            LogError("unknown format '{}'; run 'zircon emit list' to see them", name);
            ok = false;
            continue;
        }
        // same format twice would just run it twice into the same directory
        if (std::find(out.begin(), out.end(), emitter) == out.end()) out.push_back(emitter);
    }
    if (ok && out.empty()) {
        LogError("no formats given, e.g. zircon emit cpp_sdk dump.json -o out/");
        return false;
    }
    return ok;
}

// Run one emitter, print what it wrote. `labelled` is false for a single-format run so
// that output stays exactly as it was; with several it prefixes each block.
int RunEmitter(const zircon::emit::Emitter& emitter, const zircon::ir::Dump& dump,
               const zircon::emit::EmitOptions& options, bool labelled) {
    const auto emitted = emitter.emit(dump, options);
    ReportWarnings(emitted.warnings);

    if (!emitted.ok()) {
        LogError("{}: {}", emitter.name, emitted.error);
        return 7;
    }

    if (labelled) {
        FieldStrong(emitter.name, std::format("{} file(s) -> {}", emitted.files.size(),
                                              options.out_dir));
        return 0;
    }

    FieldStrong("files written", std::format("{}", emitted.files.size()));
    for (std::size_t i = 0; i < emitted.files.size() && i < 12; ++i)
        std::printf("  %.*s%s%.*s\n",
                    static_cast<int>(Dim().size()), Dim().data(),
                    emitted.files[i].c_str(),
                    static_cast<int>(Reset().size()), Reset().data());
    if (emitted.files.size() > 12)
        std::printf("  %.*s... and %zu more%.*s\n",
                    static_cast<int>(Dim().size()), Dim().data(),
                    emitted.files.size() - 12,
                    static_cast<int>(Reset().size()), Reset().data());
    return 0;
}

// Several formats into one directory means `docs` and `graphs` write over each other, so
// each gets a subdirectory. A single format still writes straight into -o, because that's
// where everyone's scripts already look.
std::string OutDirFor(std::string_view base, std::string_view format, bool split) {
    if (!split) return std::string(base);
    std::filesystem::path path(base);
    path /= format;
    return path.lexically_normal().string();
}

int CommandEmit(std::string_view format, std::string_view dump_path,
                std::string_view out_dir, std::string_view filter, bool allow_partial) {
    if (format.empty() || format == "list") {
        Heading(std::format("{:<13} {:<6} {}", "FORMAT", "NEEDS", "DESCRIPTION"));
        for (const auto& emitter : zircon::emit::Emitters()) {
            std::printf("%.*s%-13s%.*s %.*s%-6s%.*s %s\n",
                        static_cast<int>(Cyan().size()), Cyan().data(),
                        std::string(emitter.name).c_str(),
                        static_cast<int>(Reset().size()), Reset().data(),
                        static_cast<int>(Dim().size()), Dim().data(),
                        emitter.needs_objects ? "objs" : "-",
                        static_cast<int>(Reset().size()), Reset().data(),
                        std::string(emitter.description).c_str());
        }
        return format.empty() ? 1 : 0;
    }

    std::vector<const zircon::emit::Emitter*> emitters;
    if (!ResolveEmitters(format, emitters)) return 1;

    if (dump_path.empty()) {
        LogError("emit requires a dump, e.g. zircon emit cpp_sdk dump.json -o out/");
        return 1;
    }

    auto loaded = LoadDump(dump_path);
    if (!loaded.ok()) {
        LogError("{}", loaded.error().message);
        return 6;
    }

    const std::string base = out_dir.empty() ? "." : std::string(out_dir);
    const bool split = emitters.size() > 1;

    int worst = 0;
    for (const auto* emitter : emitters) {
        zircon::emit::EmitOptions options;
        options.out_dir        = OutDirFor(base, emitter->name, split);
        options.package_filter = filter;
        options.allow_partial  = allow_partial;

        // one failing doesn't stop the rest. ask for eleven and lose ten because usmap refused a
        // partial dump - not useful.
        const int code = RunEmitter(*emitter, loaded.value(), options, split);
        if (code != 0) worst = code;
    }
    return worst;
}

// Lint report: a table of check names with counts, then the findings up to a limit.
// Errors before warnings - an overlapping member matters more than a hundred dangling
// refs in a filtered dump.
//
// --strict is a different question from the round-trip. The round-trip says the file
// survived being written and read back; the lint says whether what's in it makes sense.
void ReportLint(const zircon::ir::LintReport& report, int limit) {
    if (report.findings.empty()) {
        std::printf("%-16s %s\n", "structure", "no problems found");
        return;
    }

    std::map<std::pair<int, std::string>, std::size_t> counts;
    for (const auto& finding : report.findings)
        ++counts[{static_cast<int>(finding.severity), finding.check}];

    Heading(std::format("{:<26} {:>7}  {}", "CHECK", "COUNT", "SEVERITY"));
    for (const auto& [key, count] : counts) {
        const auto severity = static_cast<zircon::ir::LintSeverity>(key.first);
        const bool is_error = severity == zircon::ir::LintSeverity::Error;
        std::printf("%.*s%-26s%.*s %7zu  %.*s%s%.*s\n",
                    static_cast<int>(Cyan().size()), Cyan().data(), key.second.c_str(),
                    static_cast<int>(Reset().size()), Reset().data(), count,
                    static_cast<int>(is_error ? Red().size() : Dim().size()),
                    is_error ? Red().data() : Dim().data(),
                    std::string(zircon::ir::ToString(severity)).c_str(),
                    static_cast<int>(Reset().size()), Reset().data());
    }
    std::printf("\n");

    const int shown_cap = limit > 0 ? limit : 20;
    int shown = 0;
    for (const int pass : {0, 1}) {
        for (const auto& finding : report.findings) {
            if (static_cast<int>(finding.severity) != pass) continue;
            if (shown >= shown_cap) break;
            ++shown;
            std::printf("  %-24s %s\n", finding.check.c_str(), finding.where.c_str());
            std::printf("  %.*s%-24s %s%.*s\n",
                        static_cast<int>(Dim().size()), Dim().data(), "",
                        finding.detail.c_str(),
                        static_cast<int>(Reset().size()), Reset().data());
        }
    }

    const std::size_t total = report.errors + report.warnings;
    if (static_cast<std::size_t>(shown) < total)
        std::printf("  %.*s... and %zu more; -n to raise the limit%.*s\n",
                    static_cast<int>(Dim().size()), Dim().data(),
                    total - static_cast<std::size_t>(shown),
                    static_cast<int>(Reset().size()), Reset().data());
    std::printf("\n");
}

// Parses a dump back and re-emits it, checking the result is identical. Synthetic
// fixtures cannot cover what a 30 MB dump of a real game contains, so this is what
// actually proves the serializer round-trips.
//
// --strict adds the structural checks in ir/Lint.h, which is a different question: the
// round-trip says the file survived being written and read, the lint says whether what is
// in it makes sense.
int CommandValidate(std::string_view path, bool strict, int limit) {
    if (path.empty()) {
        LogError("validate requires a dump path, e.g. zircon validate dump.json");
        return 1;
    }

    auto loaded = LoadDump(path);
    if (!loaded.ok()) {
        LogError("{}", loaded.error().message);
        return 6;
    }

    const auto& dump = loaded.value();
    std::printf("%-16s %d\n", "schema", dump.schema_version);
    std::printf("%-16s %s\n", "tool", dump.header.tool_version.c_str());
    // Unity builds do not carry a version the way Unreal ones do, so the runtime name
    // stands in rather than printing a percentage against an empty string.
    const std::string engine = dump.header.engine.version.empty()
                                   ? dump.header.runtime
                                   : dump.header.engine.version;
    std::printf("%-16s %s (%.0f%%)\n", "engine", engine.c_str(),
                dump.header.engine.confidence * 100.0);
    std::printf("%-16s %s\n", "source", dump.header.source.process.c_str());
    std::printf("%-16s %zu\n", "packages",   dump.packages.size());
    std::printf("%-16s %zu\n", "classes",    dump.TotalClasses());
    std::printf("%-16s %zu\n", "structs",    dump.TotalStructs());
    std::printf("%-16s %zu\n", "enums",      dump.TotalEnums());
    std::printf("%-16s %zu\n", "properties", dump.TotalProperties());
    std::printf("%-16s %zu\n", "functions",  dump.TotalFunctions());

    const std::string again = zircon::ir::WriteJsonString(dump, true);
    auto reparsed = zircon::ir::ParseJson(again);
    if (!reparsed.ok()) {
        LogError("re-emitted dump does not parse: {}", reparsed.error().message);
        return 6;
    }

    if (!(reparsed.value() == dump)) {
        LogError("round-trip is lossy: the re-parsed dump differs from the original");
        return 6;
    }

    std::printf("%-16s %s\n", "round-trip", "lossless");
    if (!strict) return 0;

    const auto report = zircon::ir::Lint(dump);
    std::printf("%-16s %zu types, %zu properties, %zu enums\n", "checked",
                report.types_checked, report.properties_checked, report.enums_checked);
    std::printf("\n");

    ReportLint(report, limit);

    Field("warnings", "{}", report.warnings);
    if (report.errors == 0) {
        FieldStrong("errors", "0");
        return 0;
    }

    // own exit code. 6 already means "wouldn't parse", and a build script wants to tell
    // that apart from "parsed fine and contradicts itself".
    FieldStrong("errors", std::format("{}", report.errors));
    return 9;
}

// Does this type tree mention `path` anywhere, at any depth? TMap<FName, TArray<AActor*>>
// references AActor, and only looking at the outer kind would miss it.
bool TypeMentions(const zircon::ir::TypeRef& type, std::string_view path) {
    if (type.name == path) return true;
    for (const auto& param : type.params) if (TypeMentions(param, path)) return true;
    return false;
}

// Full path wins. Otherwise a leaf name, and only when it's unambiguous - quietly picking
// one of two Actors is the sort of wrong answer this tool exists not to give.
std::string ResolveTypePath(const zircon::ir::Dump& dump, std::string_view query) {
    std::vector<std::string> hits;

    const auto consider = [&](const std::string& path) {
        if (path == query) { hits.assign(1, path); return true; }
        if (zircon::emit::util::LeafName(path) == query) hits.push_back(path);
        return false;
    };

    for (const auto& package : dump.packages) {
        for (const auto& record : package.classes) if (consider(record.path)) return record.path;
        for (const auto& record : package.structs) if (consider(record.path)) return record.path;
        for (const auto& record : package.enums)   if (consider(record.path)) return record.path;
    }

    if (hits.empty()) {
        LogError("no type named '{}' in this dump", query);
        return {};
    }
    if (hits.size() > 1) {
        LogError("'{}' is ambiguous, {} types have that leaf name:", query, hits.size());
        for (std::size_t i = 0; i < hits.size() && i < 8; ++i)
            std::printf("  %s\n", hits[i].c_str());
        if (hits.size() > 8) std::printf("  ... and %zu more\n", hits.size() - 8);
        return {};
    }
    return hits.front();
}

// Who points at this type, and how. The thing a dump can answer and a header can't: if I
// change AActor, what else is looking at it?
int CommandXref(std::string_view dump_path, std::string_view query, bool uses, int limit) {
    if (dump_path.empty()) {
        LogError("xref requires a dump, e.g. zircon xref game.json -f Actor");
        return 1;
    }
    if (query.empty()) {
        LogError("xref requires a type, e.g. zircon xref game.json -f CharacterMovementComponent");
        return 1;
    }

    auto loaded = LoadDump(dump_path);
    if (!loaded.ok()) {
        LogError("{}", loaded.error().message);
        return 6;
    }
    const auto& dump = loaded.value();

    const std::string target = ResolveTypePath(dump, query);
    if (target.empty()) return 2;

    FieldStrong("type", target);

    // grouped - "inherits from it" and "holds a pointer to it" are different questions, and
    // a flat list leaves you sorting them out yourself
    std::map<std::string, std::vector<std::string>> groups;
    const auto note = [&](const char* group, std::string entry) {
        groups[group].push_back(std::move(entry));
    };

    if (uses) {
        const zircon::ir::Struct* record = nullptr;
        for (const auto& package : dump.packages) {
            for (const auto* list : {&package.classes, &package.structs})
                for (const auto& candidate : *list)
                    if (candidate.path == target) record = &candidate;
        }
        if (!record) {
            LogError("'{}' is an enum; --uses only applies to classes and structs", target);
            return 2;
        }

        if (!record->super.empty()) note("extends", record->super);
        for (const auto& interface_path : record->interfaces) note("implements", interface_path);

        // dedup per group. a class with forty AActor* properties references AActor once as far
        // as this question goes.
        std::set<std::string> seen_types;
        std::function<void(const zircon::ir::TypeRef&)> walk =
            [&](const zircon::ir::TypeRef& type) {
                if (!type.name.empty() && seen_types.insert(type.name).second)
                    note("uses", type.name);
                for (const auto& param : type.params) walk(param);
            };
        for (const auto& property : record->properties) walk(property.type);
        for (const auto& function : record->functions)
            for (const auto& param : function.params) walk(param.type);
    } else {
        for (const auto& package : dump.packages) {
            for (const auto* list : {&package.classes, &package.structs}) {
                for (const auto& record : *list) {
                    if (record.super == target) note("extended by", record.path);

                    for (const auto& interface_path : record.interfaces)
                        if (interface_path == target) note("implemented by", record.path);

                    for (const auto& property : record.properties)
                        if (TypeMentions(property.type, target))
                            note("held by", record.path + "." + property.name);

                    for (const auto& function : record.functions)
                        for (const auto& param : function.params)
                            if (TypeMentions(param.type, target))
                                note("passed to", record.path + "." + function.name +
                                                  "(" + param.name + ")");
                }
            }
        }
    }

    std::size_t total = 0;
    for (const auto& [group, entries] : groups) total += entries.size();

    if (total == 0) {
        Field("references", "none");
        // own code so a script can tell "nothing points at this" from "the dump wouldn't load".
        // same shape as find returning 3.
        return 3;
    }

    const std::size_t cap = limit > 0 ? static_cast<std::size_t>(limit) : 40;
    for (auto& [group, entries] : groups) {
        std::sort(entries.begin(), entries.end());
        Heading(std::format("{} ({})", group, entries.size()));
        for (std::size_t i = 0; i < entries.size() && i < cap; ++i)
            std::printf("  %s\n", entries[i].c_str());
        if (entries.size() > cap)
            std::printf("  %.*s... and %zu more%.*s\n",
                        static_cast<int>(Dim().size()), Dim().data(),
                        entries.size() - cap,
                        static_cast<int>(Reset().size()), Reset().data());
    }

    Field("references", "{}", total);
    return 0;
}

int CommandDump(const TargetSpec& spec, std::string_view out_path,
                std::string_view filter, bool with_names, bool with_script,
                bool with_defaults, std::string_view emit_formats,
                bool allow_partial, const PublishAfterDump& publish,
                std::string_view launch_exe, int launch_timeout) {
    // resolve the formats before the walk. seventy thousand objects take a few seconds and
    // finding out afterwards that a name was mistyped is a bad trade.
    std::vector<const zircon::emit::Emitter*> emitters;
    if (!emit_formats.empty() && !ResolveEmitters(emit_formats, emitters)) return 1;

    // Start it first if asked, and attach to what came up rather than to what was asked
    // for -- a launcher hands off, and the pid that answers is not the pid that was started.
    TargetSpec target = spec;
    if (!launch_exe.empty()) {
        const std::uint32_t pid = LaunchAndWaitUnreal(std::string(launch_exe), launch_timeout);
        if (pid == 0) return 4;
        target.kind = TargetSpec::Kind::Pid;
        target.pid  = pid;
    }

    auto source = OpenTarget(target);
    if (!source) {
        LogError("{}", source.error().message);
        return source.error().code;
    }

    auto memory = MakeCached(std::move(source.value()));
    LogInfo("target: {}", memory->Describe());

    // Unity first, and only to refuse. IL2CPP answers come from calling into the runtime,
    // which we can't do from out here. Say so now rather than fail later with an Unreal
    // reflection message that sends someone looking in the wrong place.
    if (const auto unity = zircon::il2cpp::FindRuntime(*memory)) {
        LogError("this is a Unity IL2CPP game ({}), and its type information only exists "
                 "as answers the runtime gives to calls", unity->module_name);
        const std::string how = spec.kind == TargetSpec::Kind::Pid
                                    ? std::format("--pid {}", spec.pid)
                                    : std::format("--process {}", spec.value);
        LogError("run 'zircon inject {}' instead; the payload walks it from inside and "
                 "writes the dump next to the DLL", how);
        Field("api", "{}/{} entry points resolved", unity->api.resolved,
              zircon::il2cpp::RequiredEntryPointCount());
        return 2;
    }

    const auto reflection = zircon::engine::Reflect(*memory);
    if (!reflection.Valid()) {
        LogError("reflection is incomplete; cannot produce a dump");
        return 4;
    }

    zircon::engine::BuildOptions options;
    options.include_names  = with_names;
    options.include_script   = with_script;
    options.include_defaults = with_defaults;
    options.package_filter = filter;

    const auto dump = zircon::engine::BuildDump(reflection, options);

    const std::string path{out_path.empty() ? "dump.json" : out_path};
    std::string error;
    if (!WriteDumpFile(dump, path, error)) {
        LogError("could not write '{}': {}", path, error);
        return 5;
    }

    FieldStrong("output", path);
    Field("packages",   "{}", dump.packages.size());
    Field("classes",    "{}", dump.TotalClasses());
    Field("structs",    "{}", dump.TotalStructs());
    Field("enums",      "{}", dump.TotalEnums());
    Field("properties", "{}", dump.TotalProperties());
    Field("functions",  "{}", dump.TotalFunctions());
    if (!dump.names.empty()) Field("names", "{}", dump.names.size());

    // Whether or not they publish now, the next step belongs in front of them while
    // they are still looking at the file they just made.
    const auto suggest_publish = [&]() -> int {
        // Asked of the dump in hand, not the file just written.
        if (const auto refusal = zircon::app::PublishRefusal(dump.header.runtime);
            !refusal.empty()) {
            if (publish.requested) {
                LogError("{}", refusal);
                return 2;
            }
            // No hint either -- pointing at a command that will refuse is worse than nothing,
            // and nothing looks like an oversight.
            std::printf("\n");
            LogInfo("not publishable: {}", refusal);
            return 0;
        }

        const std::string game = publish.game.empty()
            ? zircon::app::GameNameFromProcess(dump.header.source.process)
            : publish.game;
        const std::string label = publish.label.empty()
            ? zircon::app::LabelFromTimestamp(dump.header.created_utc)
            : publish.label;

        if (!publish.requested) {
            zircon::app::PrintPublishHint(path, game, label);
            return 0;
        }

        zircon::app::PublishOptions options;
        options.path         = path;
        options.game         = game;
        options.label        = label;
        options.notes        = publish.notes;
        options.wait         = publish.wait;
        options.assume_yes   = publish.assume_yes;
        options.open_browser = publish.open_browser;

        std::printf("\n");
        Heading("publish");
        return zircon::app::CommandPublish(options);
    };

    if (emitters.empty()) return suggest_publish();

    // next to the dump, not in the cwd. `-o out/game.json --emit cpp_sdk` meaning "json over
    // there, headers over here" would surprise everyone.
    const std::filesystem::path base = std::filesystem::path(path).parent_path();
    const bool split = emitters.size() > 1;

    Heading("emit");
    int worst = 0;
    for (const auto* emitter : emitters) {
        zircon::emit::EmitOptions emit_options;
        emit_options.out_dir = OutDirFor(base.empty() ? "." : base.string(), emitter->name,
                                         split);
        emit_options.package_filter = filter;
        emit_options.allow_partial  = allow_partial;

        const int code = RunEmitter(*emitter, dump, emit_options, split);
        if (code != 0) worst = code;
    }

    const int published = suggest_publish();
    return worst != 0 ? worst : published;
}

// Reads the live values of one object's properties. The dump says where a member is;
// this says what is in it right now, which is the whole point of attaching to a running
// game rather than analysing a file.
int CommandRead(const TargetSpec& spec, std::string_view path, int limit) {
    if (path.empty()) {
        LogError("read requires --filter <object path>, e.g. --filter /Script/Engine.Actor");
        return 1;
    }

    auto session = OpenSession(spec, true);
    if (!session) return 4;

    const auto object = zircon::engine::FindObjectByPath(
        *session->memory, session->array, session->layout, session->pool, path);
    if (IsNull(object)) {
        LogError("no object with path '{}'", path);
        return 3;
    }

    auto context = session->Context();

    // A class is not an instance: reading it as one shows the handful of members UClass
    // itself declares, which is nothing. What the caller means by "read PlayerController"
    // is its defaults, and the engine keeps those in the class default object.
    auto instance = object;
    std::string note;
    if (zircon::engine::ClassifyObject(*session->memory, session->layout, session->structs,
                                       session->pool, object) ==
        zircon::engine::ObjectKind::Class) {
        const auto cdo = zircon::engine::GetClassDefaultObject(*session->memory,
                                                               session->classes, object);
        if (IsNull(cdo)) {
            LogWarn("{} has no class default object; nothing to read", path);
            return 3;
        }
        instance = cdo;
        note = "  (class defaults)";
    }

    // Properties come from the object's class, walking up the chain so inherited members
    // show too: an instance is far more interesting than the few members it declares.
    const auto klass = zircon::engine::GetObjectClass(*session->memory, session->layout, instance);
    std::printf("%s%s\n  at %#llx\n  class %s\n\n", std::string(path).c_str(), note.c_str(),
                static_cast<unsigned long long>(Raw(instance)),
                zircon::engine::GetObjectPathName(*session->memory, session->layout,
                                                   session->pool, klass).c_str());

    int shown = 0;
    for (auto current = klass; !IsNull(current) && (limit <= 0 || shown < limit);
         current = zircon::engine::GetSuperStruct(*session->memory, session->structs, current)) {

        const auto fields = zircon::engine::GetChildProperties(
            *session->memory, session->structs, session->props, current);
        if (fields.empty()) continue;

        std::printf("  // %s\n", zircon::engine::GetObjectPathName(
            *session->memory, session->layout, session->pool, current).c_str());

        for (const auto field : fields) {
            if (limit > 0 && shown >= limit) break;
            const std::string name = zircon::engine::GetFieldName(
                *session->memory, session->props, session->pool, field);
            const std::string value =
                zircon::engine::ReadPropertyValue(context, instance, field);
            std::printf("  0x%04X  %-44s = %s\n",
                        zircon::engine::GetPropertyOffset(*session->memory, session->props, field),
                        name.c_str(), value.c_str());
            ++shown;
        }
    }
    return 0;
}

// Writes one property of one object. The counterpart of `read`, and deliberately narrow:
// it takes a single Name=Value, resolves the same way `read` does, and reports what the
// value was before so a mistake can be undone by hand.
int CommandWrite(const TargetSpec& spec, std::string_view path, std::string_view assignment) {
    if (path.empty() || assignment.empty()) {
        LogError("write needs an object and an assignment, e.g. "
                 "zircon write --pid 1234 -f /Script/Engine.CharacterMovementComponent "
                 "--set MaxWalkSpeed=1337");
        return 1;
    }

    const auto equals = assignment.find('=');
    if (equals == std::string_view::npos) {
        LogError("--set takes Name=Value, e.g. --set MaxWalkSpeed=1337");
        return 1;
    }

    const auto wanted_name = assignment.substr(0, equals);
    const auto wanted_value = assignment.substr(equals + 1);
    if (wanted_name.empty()) {
        LogError("--set is missing a property name");
        return 1;
    }

    auto session = OpenSession(spec, true);
    if (!session) return 4;

    // Writing is opt-in at the provider, the same as it is in the browser. Reaching this
    // command is the opt-in; a session that only reads never asks for the handle.
    if (!session->memory->EnableWrites(true)) {
        LogError("this target cannot be opened for writing");
        return 4;
    }

    const auto object = zircon::engine::FindObjectByPath(
        *session->memory, session->array, session->layout, session->pool, path);
    if (IsNull(object)) {
        LogError("no object with path '{}'", path);
        return 3;
    }

    // Same rule as `read`: naming a class means its defaults, since a class read as an
    // instance shows the few members UClass itself declares.
    auto instance = object;
    if (zircon::engine::ClassifyObject(*session->memory, session->layout, session->structs,
                                       session->pool, object) ==
        zircon::engine::ObjectKind::Class) {
        const auto cdo = zircon::engine::GetClassDefaultObject(*session->memory,
                                                               session->classes, object);
        if (IsNull(cdo)) {
            LogError("{} has no class default object; nothing to write", path);
            return 3;
        }
        instance = cdo;
    }

    auto context = session->Context();

    // Walk the class chain so an inherited property can be named without qualifying it.
    const auto klass = zircon::engine::GetObjectClass(*session->memory, session->layout, instance);
    for (auto current = klass; !IsNull(current);
         current = zircon::engine::GetSuperStruct(*session->memory, session->structs, current)) {

        for (const auto field : zircon::engine::GetChildProperties(
                 *session->memory, session->structs, session->props, current)) {

            const std::string name = zircon::engine::GetFieldName(
                *session->memory, session->props, session->pool, field);
            if (name != wanted_name) continue;

            const std::string before =
                zircon::engine::ReadPropertyValue(context, instance, field);

            const auto result =
                zircon::engine::WritePropertyValue(context, instance, field, wanted_value);
            if (!result.ok) {
                LogError("{}: {}", name, result.error);
                return 5;
            }

            const std::string after =
                zircon::engine::ReadPropertyValue(context, instance, field);

            Field("object", path);
            Field("property", name);
            Field("was", before);
            FieldStrong("now", after);

            // The engine may own this field and put it back on the next tick. Saying so
            // beats leaving someone to wonder why the change did not take.
            if (after != result.written)
                LogWarn("reads back as {}, not {}; the game may own this field",
                        after, result.written);
            return 0;
        }
    }

    LogError("{} has no property named '{}'", path, wanted_name);
    return 3;
}

// Finds objects by what they hold, not by what they are called.
//
// "every Actor whose Health is under 50" is the question a dumper usually cannot answer:
// the dump says where Health lives, and finding the ones that matter means reading it
// across every instance. That is one pass over the object array, which the walker already
// does for everything else.
int CommandFind(const TargetSpec& spec, std::string_view class_filter,
                std::string_view predicate, int limit) {
    if (predicate.empty()) {
        LogError("find needs a condition, e.g. "
                 "zircon find --pid 1234 --where MaxWalkSpeed=600 [-f PartialClassName]");
        return 1;
    }

    // Longest operator first, or "<=" is read as "<".
    static constexpr std::string_view kOperators[] = {">=", "<=", "!=", "=", ">", "<"};

    std::string_view op;
    std::size_t at = std::string_view::npos;
    for (const auto candidate : kOperators) {
        const auto found = predicate.find(candidate);
        if (found == std::string_view::npos) continue;
        if (at == std::string_view::npos || found < at) { at = found; op = candidate; }
    }
    if (at == std::string_view::npos) {
        LogError("--where takes Name<op>Value, e.g. Health<50 or MovementMode=MOVE_Falling");
        return 1;
    }

    const std::string wanted_property{predicate.substr(0, at)};
    const std::string wanted_value{predicate.substr(at + op.size())};
    if (wanted_property.empty()) {
        LogError("--where is missing a property name");
        return 1;
    }

    auto session = OpenSession(spec, true);
    if (!session) return 4;

    auto context = session->Context();

    // Numeric comparisons need a number. Anything else falls back to comparing the text
    // the reader produced, which is what makes MovementMode=MOVE_Falling work.
    double wanted_number = 0.0;
    const bool numeric = [&] {
        const auto* end = wanted_value.data() + wanted_value.size();
        const auto result = std::from_chars(wanted_value.data(), end, wanted_number);
        return result.ec == std::errc{} && result.ptr == end;
    }();

    if (!numeric && op != "=" && op != "!=") {
        LogError("'{}' is not a number, so it can only be compared with = or !=",
                 wanted_value);
        return 1;
    }

    Heading(std::format("{:<58} {:<22} {}", "OBJECT", "PROPERTY", "VALUE"));

    int matched = 0;
    int scanned = 0;
    for (std::int32_t i = 0; i < session->array.num_elements; ++i) {
        if (limit > 0 && matched >= limit) break;

        const auto object = zircon::engine::ObjectAt(*session->memory, session->array, i);
        if (IsNull(object)) continue;

        const auto klass = zircon::engine::GetObjectClass(*session->memory,
                                                          session->layout, object);
        if (IsNull(klass)) continue;

        const std::string class_path = zircon::engine::GetObjectPathName(
            *session->memory, session->layout, session->pool, klass);
        if (!class_filter.empty() && class_path.find(class_filter) == std::string::npos)
            continue;

        // Walk the chain so an inherited property counts, the same as `read` and `write`.
        for (auto current = klass; !IsNull(current);
             current = zircon::engine::GetSuperStruct(*session->memory, session->structs,
                                                      current)) {

            bool done = false;
            for (const auto field : zircon::engine::GetChildProperties(
                     *session->memory, session->structs, session->props, current)) {

                if (zircon::engine::GetFieldName(*session->memory, session->props,
                                                 session->pool, field) != wanted_property)
                    continue;

                ++scanned;
                done = true;

                const std::string value =
                    zircon::engine::ReadPropertyValue(context, object, field);

                bool hit = false;
                if (numeric) {
                    double actual = 0.0;
                    const auto* end = value.data() + value.size();
                    const auto parsed = std::from_chars(value.data(), end, actual);
                    if (parsed.ec == std::errc{} && parsed.ptr == end) {
                        if      (op == "=")  hit = actual == wanted_number;
                        else if (op == "!=") hit = actual != wanted_number;
                        else if (op == ">")  hit = actual >  wanted_number;
                        else if (op == "<")  hit = actual <  wanted_number;
                        else if (op == ">=") hit = actual >= wanted_number;
                        else if (op == "<=") hit = actual <= wanted_number;
                    }
                } else {
                    hit = (op == "=") ? value == wanted_value : value != wanted_value;
                }

                if (hit) {
                    const std::string path = zircon::engine::GetObjectPathName(
                        *session->memory, session->layout, session->pool, object);
                    std::printf("%-58s %-22s %s\n", path.c_str(), wanted_property.c_str(),
                                value.c_str());
                    ++matched;
                }
                break;
            }
            if (done) break;
        }
    }

    std::fprintf(stderr, "\n%d match%s from %d object%s carrying '%s'\n",
                 matched, matched == 1 ? "" : "es", scanned, scanned == 1 ? "" : "s",
                 wanted_property.c_str());
    return matched > 0 ? 0 : 3;
}

int CommandScript(const TargetSpec& spec, std::string_view filter, int limit) {
    auto session = OpenSession(spec, true);
    if (!session) return 4;

    const auto script_layout = zircon::engine::DeriveScriptLayout(
        *session->memory, session->array, session->pool, session->layout, session->structs);
    if (!script_layout.Valid()) {
        LogError("could not locate UStruct::Script");
        return 4;
    }
    for (const auto& line : script_layout.evidence) std::printf("  - %s\n", line.c_str());
    std::printf("\n");

    auto context = session->Context();

    // Without this the decompiler falls back to the engine version for constant widths,
    // and a licensee build has no version — which silently desyncs the instruction stream
    // at the first vector literal.
    context.script_layout = &script_layout;

    int shown = 0, with_code = 0, complete = 0;
    std::size_t total_bytes = 0;
    std::map<std::uint8_t, int> unknown;

    for (std::int32_t i = 0; i < session->array.num_elements; ++i) {
        const auto object = zircon::engine::ObjectAt(*session->memory, session->array, i);
        if (IsNull(object)) continue;
        if (zircon::engine::GetClassName(*session->memory, session->layout, session->pool,
                                         object) != "Function")
            continue;

        const std::string path = zircon::engine::GetObjectPathName(
            *session->memory, session->layout, session->pool, object);
        if (path.empty()) continue;
        if (!filter.empty() && path.find(filter) == std::string::npos) continue;

        const auto decompiled = zircon::engine::DecompileFunction(context, script_layout,
                                                                   object);
        if (decompiled.bytes_total == 0) continue;

        ++with_code;
        total_bytes += decompiled.bytes_total;
        if (decompiled.complete) ++complete;
        for (const auto opcode : decompiled.unknown_opcodes) ++unknown[opcode];

        if (limit > 0 && shown >= limit) continue;
        ++shown;

        std::printf("%s   // %u bytes of bytecode%s\n", path.c_str(), decompiled.bytes_total,
                    decompiled.complete ? "" : "  [INCOMPLETE]");
        for (const auto& line : decompiled.lines)
            std::printf("  %04X  %s%s\n", line.offset,
                        std::string(static_cast<std::size_t>(line.depth) * 2, ' ').c_str(),
                        line.text.c_str());
        if (!decompiled.complete)
            std::printf("  ....  // stopped: %s\n", decompiled.stop_reason.c_str());
        std::printf("\n");
    }

    std::fprintf(stderr, "%d functions carry bytecode (%zu bytes); %d decoded fully (%.1f%%)\n",
                 with_code, total_bytes, complete,
                 with_code ? 100.0 * complete / with_code : 0.0);
    if (!unknown.empty()) {
        std::fprintf(stderr, "opcodes not decoded:\n");
        for (const auto& [opcode, count] : unknown)
            std::fprintf(stderr, "  0x%02X %-24s %d function(s)\n", opcode,
                         std::string(zircon::engine::OpcodeName(opcode)).c_str(), count);
    }
    return 0;
}

int CommandFunctions(const TargetSpec& spec, std::string_view filter, int limit) {
    auto session = OpenSession(spec, true);
    if (!session) return 4;

    const auto& fn = session->functions;
    if (!fn.Valid()) {
        LogError("could not derive the UFunction layout");
        return 4;
    }

    std::printf("UFunction layout next +%#x  flags +%#x  func +%#x   confidence %.0f%%\n",
                fn.field_next, fn.function_flags, fn.native_func, fn.confidence * 100.0);
    for (const auto& line : fn.evidence) std::printf("  - %s\n", line.c_str());
    std::printf("\n");

    const auto* main_module = session->memory->MainModule();
    const std::uint64_t base = main_module ? Raw(main_module->base) : 0;

    auto context = session->Context();
    int shown = 0;

    for (std::int32_t i = 0; i < session->array.num_elements && (limit <= 0 || shown < limit);
         ++i) {
        const auto object = zircon::engine::ObjectAt(*session->memory, session->array, i);
        if (IsNull(object)) continue;
        if (zircon::engine::GetClassName(*session->memory, session->layout, session->pool,
                                         object) != "Class")
            continue;

        const std::string path = zircon::engine::GetObjectPathName(
            *session->memory, session->layout, session->pool, object);
        if (path.empty()) continue;
        if (!filter.empty() && path.find(filter) == std::string::npos) continue;

        const auto found = zircon::engine::GetClassFunctions(
            *session->memory, session->array, session->pool, session->layout,
            session->structs, fn, object);
        if (found.empty()) continue;

        std::printf("%s  // %zu function%s\n", path.c_str(), found.size(),
                    found.size() == 1 ? "" : "s");

        for (const auto function : found) {
            const auto flags = zircon::engine::GetFunctionFlags(*session->memory, fn, function);
            const auto native = zircon::engine::GetNativeFunc(*session->memory, fn, function);

            // Parameters are the function's own ChildProperties, flagged as parameters.
            std::string params, returns = "void";
            for (const auto param :
                 zircon::engine::GetChildProperties(*session->memory, session->structs,
                                                     session->props, function)) {
                const auto pflags = zircon::engine::GetPropertyFlags(*session->memory,
                                                                     session->props, param);
                if (!(pflags & zircon::engine::property_flags::kParm)) continue;

                const std::string rendered = zircon::engine::DescribeType(
                    zircon::engine::ResolveType(context, param));

                if (pflags & zircon::engine::property_flags::kReturnParm) {
                    returns = rendered;
                    continue;
                }
                if (!params.empty()) params += ", ";
                if (pflags & zircon::engine::property_flags::kOutParm) params += "out ";
                params += rendered + " " +
                          zircon::engine::GetFieldName(*session->memory, session->props,
                                                        session->pool, param);
            }

            std::string flag_text;
            for (const auto& name : zircon::engine::DescribeFunctionFlags(flags)) {
                if (!flag_text.empty()) flag_text += "|";
                flag_text += name;
            }

            std::printf("    %-10s %s %s(%s)",
                        IsNull(native) ? "" :
                            std::format("+{:#x}", Raw(native) - base).c_str(),
                        returns.c_str(),
                        zircon::engine::GetObjectName(*session->memory, session->layout,
                                                       session->pool, function).c_str(),
                        params.c_str());
            if (!flag_text.empty()) std::printf("   // %s", flag_text.c_str());
            std::printf("\n");
        }
        std::printf("\n");
        ++shown;
    }

    std::fprintf(stderr, "%d classes with functions listed\n", shown);
    return 0;
}

int CommandProps(const TargetSpec& spec, std::string_view filter, int limit) {
    auto session = OpenSession(spec, true);
    if (!session) return 4;

    const auto& props = session->props;
    if (!props.Valid()) {
        LogError("could not derive the FProperty layout");
        return 4;
    }

    std::printf("FProperty layout class +%#x  next +%#x  name +%#x  dim +%#x  "
                "size +%#x  flags +%#x  offset +%#x\n",
                props.class_private, props.next, props.name, props.array_dim,
                props.element_size, props.property_flags, props.offset_internal);
    std::printf("confidence       %.0f%%\n", props.confidence * 100.0);
    for (const auto& line : props.evidence)
        std::printf("  - %s\n", line.c_str());
    std::printf("\n");

    int shown = 0;
    for (std::int32_t i = 0; i < session->array.num_elements && (limit <= 0 || shown < limit);
         ++i) {
        const auto object = zircon::engine::ObjectAt(*session->memory, session->array, i);
        if (IsNull(object)) continue;

        const std::string kind = zircon::engine::GetClassName(
            *session->memory, session->layout, session->pool, object);
        if (kind != "Class" && kind != "ScriptStruct") continue;

        const std::string path = zircon::engine::GetObjectPathName(
            *session->memory, session->layout, session->pool, object);
        if (path.empty()) continue;
        if (!filter.empty() && path.find(filter) == std::string::npos) continue;

        const auto fields = zircon::engine::GetChildProperties(
            *session->memory, session->structs, props, object);
        if (fields.empty()) continue;

        std::printf("%s  // 0x%X bytes, %zu own propert%s\n", path.c_str(),
                    zircon::engine::GetPropertiesSize(*session->memory, session->structs,
                                                       object),
                    fields.size(), fields.size() == 1 ? "y" : "ies");

        auto context = session->Context();

        for (const auto field : fields) {
            const std::int32_t at   = zircon::engine::GetPropertyOffset(*session->memory,
                                                                        props, field);
            const std::int32_t size = zircon::engine::GetElementSize(*session->memory,
                                                                      props, field);
            const std::int32_t dim  = zircon::engine::GetArrayDim(*session->memory,
                                                                   props, field);

            std::string name = zircon::engine::GetFieldName(*session->memory, props,
                                                             session->pool, field);
            if (dim > 1) name += std::string("[") + std::to_string(dim) + "]";

            const auto type = zircon::engine::ResolveType(context, field);
            const auto bits = zircon::engine::ResolveBitfield(context, field);
            if (bits.is_bitfield)
                name += std::format(" : 1  // bit {}, mask 0x{:02X}", bits.bit_index,
                                    bits.field_mask);

            std::printf("    0x%04X  0x%04X  %-40s %s\n", at, size * dim,
                        zircon::engine::DescribeType(type).c_str(), name.c_str());
        }
        std::printf("\n");
        ++shown;
    }

    std::fprintf(stderr, "%d types with properties listed\n", shown);
    return 0;
}

int CommandClasses(const TargetSpec& spec, std::string_view filter, int limit) {
    auto session = OpenSession(spec, true);
    if (!session) return 4;

    const auto& structs = session->structs;
    if (!structs.Valid()) {
        LogError("could not derive the UStruct layout");
        return 4;
    }

    Field("UStruct layout",
          "super +{:#x}  children +{:#x}  childprops +{:#x}  size +{:#x}  align +{:#x}",
          structs.super_struct, structs.children, structs.child_properties,
          structs.properties_size, structs.min_alignment);
    Field("confidence", "{:.0f}%", structs.confidence * 100.0);
    Evidence(structs.evidence);

    std::printf("\n");
    Heading(std::format("{:<62} {:>8} {:>6}  {}", "CLASS", "SIZE", "ALIGN", "SUPER"));

    int printed = 0;
    for (std::int32_t i = 0; i < session->array.num_elements; ++i) {
        const auto object = zircon::engine::ObjectAt(*session->memory, session->array, i);
        if (IsNull(object)) continue;

        const std::string kind = zircon::engine::GetClassName(
            *session->memory, session->layout, session->pool, object);
        if (kind != "Class" && kind != "ScriptStruct") continue;

        const std::string path = zircon::engine::GetObjectPathName(
            *session->memory, session->layout, session->pool, object);
        if (path.empty()) continue;
        if (!filter.empty() && path.find(filter) == std::string::npos) continue;
        if (limit > 0 && printed >= limit) break;

        const auto super = zircon::engine::GetSuperStruct(*session->memory, structs, object);
        const std::string super_path =
            IsNull(super) ? std::string{}
                          : zircon::engine::GetObjectPathName(*session->memory,
                                                              session->layout,
                                                              session->pool, super);

        const std::int32_t size = zircon::engine::GetPropertiesSize(*session->memory,
                                                                    structs, object);
        const std::int32_t alignment =
            zircon::engine::GetMinAlignment(*session->memory, structs, object);

        std::printf("%-62s %8d %6d  %s\n", path.c_str(), size, alignment,
                    super_path.empty() ? "-" : super_path.c_str());
        ++printed;
    }

    std::fprintf(stderr, "\n%d types listed\n", printed);
    return 0;
}

int CommandNames(const TargetSpec& spec, int limit) {
    auto session = OpenSession(spec, false);
    if (!session) return 4;

    FieldStrong("FNamePool", std::format("{:#x}", Raw(session->pool.blocks)));
    Field("entry format", "header >> {}, chars at +{}{}",
          session->pool.len_shift, session->pool.case_preserving ? 6u : 2u,
          session->pool.case_preserving ? ", case-preserving" : "");
    Field("confidence", "{:.0f}%", session->pool.confidence * 100.0);
    Evidence(session->pool.evidence);
    std::printf("\n");

    // Walk ids until the pool stops resolving. Names are dense from zero, so a run of
    // consecutive failures means the end rather than a gap.
    int printed = 0;
    int misses  = 0;
    for (std::uint32_t id = 0; misses < 4096 && (limit <= 0 || printed < limit); ++id) {
        const std::string name = zircon::engine::ResolveName(*session->memory,
                                                             session->pool, id);
        if (name.empty()) { ++misses; continue; }
        misses = 0;
        std::printf("[%8u] %s\n", id, name.c_str());
        ++printed;
    }

    std::fprintf(stderr, "\n%d names resolved\n", printed);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);

    // Decided before anything can print. --color and --no-color are honoured wherever
    // they appear, since a user who reaches for one has usually already seen the output
    // they want changed.
    InitTerminal();
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--no-color" || arg == "--no-colour") SetColourEnabled(false);
        else if (arg == "--color" || arg == "--colour")  SetColourEnabled(true);
    }

    if (args.empty()) {
        PrintUsage();
        return 1;
    }

    std::string_view command = args.front();
    if (command == "-h" || command == "--help") { PrintUsage(); return 0; }

    // Asked before the options are parsed, because half the point is that a command with a
    // flag you got wrong still explains itself instead of complaining about the flag.
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] != "-h" && args[i] != "--help") continue;
        if (!PrintCommandHelp(command)) PrintUsage();
        return 0;
    }
    if (command == "--version") { std::printf("zircon %s\n", kVersion); return 0; }

    TargetSpec  spec;
    int         verbosity = 0;
    std::string pattern_text;
    std::string scan_module;
    std::string name_filter;
    std::string assignment;
    std::string predicate;
    std::string out_path;
    std::string dll_path;
    bool        headless = false;
    std::string metadata_path;
    std::string walk_mode;
    std::vector<std::string> scan_roots;
    std::string launch_exe;
    bool        wait_for_payload = false;
    int         settle_seconds = 0;
    int         timeout_seconds = kDefaultTimeoutSeconds;
    std::string validate_path;
    std::string emit_format;
    std::string diff_before, diff_after;
    std::string report_style;
    bool        breaking_only = false;
    bool        allow_partial = false;
    bool        strict = false;
    bool        uses = false;
    bool        publish_after = false;
    bool        no_wait = false;
    bool        dry_run = false;
    bool        publish_all = false;
    bool        publish_force = false;
    bool        json_output = false;
    bool        assume_yes = false;
    bool        open_browser = false;
    std::string zdex_key;
    std::string zdex_game;
    std::string zdex_label;
    std::string zdex_notes;
    std::string fetch_kind;
    std::string publish_path;
    bool        with_names = false;
    bool        with_script = false;
    bool        with_defaults = false;
    int         limit = 0;
    bool        all_regions = false;
    std::vector<std::string> plugin_dirs;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto next = [&](std::string_view name) -> std::optional<std::string_view> {
            if (i + 1 >= args.size()) {
                LogError("{} requires a value", name);
                return std::nullopt;
            }
            return args[++i];
        };

        if (arg == "--internal") {
            spec.kind = TargetSpec::Kind::Internal;
        } else if (arg == "--pid") {
            const auto value = next(arg);
            if (!value) return 1;
            const auto pid = ParseU32(*value);
            if (!pid) { LogError("invalid pid: {}", *value); return 1; }
            spec.kind = TargetSpec::Kind::Pid;
            spec.pid  = *pid;
        } else if (arg == "--process") {
            const auto value = next(arg);
            if (!value) return 1;
            spec.kind  = TargetSpec::Kind::ProcessName;
            spec.value = *value;
        } else if (arg == "--dump") {
            const auto value = next(arg);
            if (!value) return 1;
            spec.kind  = TargetSpec::Kind::DumpFile;
            spec.value = *value;
        } else if (arg == "--file") {
            const auto value = next(arg);
            if (!value) return 1;
            spec.kind  = TargetSpec::Kind::StaticFile;
            spec.value = *value;
        } else if (arg == "-p" || arg == "--pattern") {
            const auto value = next(arg);
            if (!value) return 1;
            pattern_text = *value;
        } else if (arg == "-m" || arg == "--module") {
            const auto value = next(arg);
            if (!value) return 1;
            scan_module = *value;
        } else if (arg == "-f" || arg == "--filter") {
            const auto value = next(arg);
            if (!value) return 1;
            name_filter = *value;
        } else if (arg == "-n" || arg == "--limit") {
            const auto value = next(arg);
            if (!value) return 1;
            const auto parsed = ParseU32(*value);
            if (!parsed) { LogError("invalid limit: {}", *value); return 1; }
            limit = static_cast<int>(*parsed);
        } else if (arg == "--color" || arg == "--colour" ||
                   arg == "--no-color" || arg == "--no-colour") {
            // Already applied before parsing; accepted here so it is not "unknown".
        } else if (arg == "--where") {
            const auto value = next(arg);
            if (!value) return 1;
            predicate = *value;
        } else if (arg == "--set") {
            const auto value = next(arg);
            if (!value) return 1;
            assignment = *value;
        } else if (arg == "--all-regions") {
            all_regions = true;
        } else if (arg == "-v" || arg == "--verbose") {
            ++verbosity;
        } else if (arg == "-o" || arg == "--out") {
            const auto value = next(arg);
            if (!value) return 1;
            out_path = *value;
        } else if (arg == "--metadata") {
            const auto value = next(arg);
            if (!value) return 1;
            metadata_path = *value;
        } else if (arg == "--mode") {
            const auto value = next(arg);
            if (!value) return 1;
            if (*value != "live" && *value != "static" && *value != "dual") {
                LogError("unknown mode '{}'; it is live, static or dual", *value);
                return 1;
            }
            walk_mode = *value;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--wait") {
            wait_for_payload = true;
        } else if (arg == "--launch") {
            const auto value = next(arg);
            if (!value) return 1;
            launch_exe = *value;
        } else if (arg == "--timeout") {
            const auto value = next(arg);
            if (!value) return 1;
            const auto parsed = ParseU32(*value);
            if (!parsed || *parsed == 0) { LogError("invalid timeout: {}", *value); return 1; }
            timeout_seconds = static_cast<int>(*parsed);
        } else if (arg == "--wait-for-settle") {
            // Optional value: bare means "use a sensible ceiling".
            settle_seconds = 60;
            if (i + 1 < args.size() && IsPositional(args[i + 1])) {
                const auto parsed = ParseU32(args[i + 1]);
                if (parsed && *parsed > 0) { settle_seconds = static_cast<int>(*parsed); ++i; }
            }
        } else if (arg == "--dll") {
            const auto value = next(arg);
            if (!value) return 1;
            dll_path = *value;
        } else if (arg == "--plugins") {
            const auto value = next(arg);
            if (!value) return 1;
            plugin_dirs.emplace_back(*value);
        } else if (arg == "--names") {
            with_names = true;
        } else if (arg == "--script") {
            with_script = true;
        } else if (arg == "--defaults") {
            with_defaults = true;
        } else if (arg == "--emit") {
            // dump only: render as we write, so the common case is one command instead of two
            // and one less chance to emit from a stale file
            const auto value = next(arg);
            if (!value) return 1;
            emit_format = *value;
        } else if (arg == "--allow-partial") {
            allow_partial = true;
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--uses") {
            uses = true;
        } else if (arg == "--publish") {
            publish_after = true;
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (command == "publish" && arg == "--all") {
            publish_all = true;
        } else if (command == "publish" && arg == "--force") {
            publish_force = true;
        } else if (arg == "--no-wait") {
            no_wait = true;
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "-y" || arg == "--yes") {
            assume_yes = true;
        } else if (arg == "--open") {
            open_browser = true;
        } else if (arg == "--usmap" || arg == "--sdk") {
            fetch_kind = arg.substr(2);
        } else if (arg == "--key") {
            const auto value = next(arg);
            if (!value) return 1;
            zdex_key = *value;
        } else if (arg == "--game") {
            const auto value = next(arg);
            if (!value) return 1;
            zdex_game = *value;
        } else if (arg == "--label") {
            const auto value = next(arg);
            if (!value) return 1;
            zdex_label = *value;
        } else if (arg == "--notes") {
            const auto value = next(arg);
            if (!value) return 1;
            zdex_notes = *value;
        } else if (command == "scan-games" && IsPositional(arg)) {
            scan_roots.emplace_back(arg);
        } else if ((command == "validate" || command == "xref" || command == "metadata" ||
                    command == "batch" || command == "check") &&
                   validate_path.empty() && IsPositional(arg)) {
            validate_path = arg;
        } else if ((command == "publish" || command == "fetch" || command == "login") &&
                   publish_path.empty() && IsPositional(arg)) {
            publish_path = arg;
        } else if (arg == "--style") {
            const auto value = next(arg);
            if (!value) return 1;
            report_style = *value;
        } else if (arg == "--breaking") {
            breaking_only = true;
        } else if (command == "diff" && IsPositional(arg)) {
            if (diff_before.empty())     diff_before = arg;
            else if (diff_after.empty()) diff_after = arg;
            else { LogError("diff takes exactly two dumps"); return 1; }
        } else if (command == "emit" && IsPositional(arg)) {
            // `emit <format> <dump>`: two bare arguments, in that order.
            if (emit_format.empty())        emit_format = arg;
            else if (validate_path.empty()) validate_path = arg;
            else { LogError("unexpected argument: {}", arg); return 1; }
        } else {
            LogError("unknown option: {}", arg);
            return 1;
        }
    }

    SetLogLevel(verbosity >= 2 ? LogLevel::Trace
              : verbosity == 1 ? LogLevel::Debug
                               : LogLevel::Info);

    // Plugins are loaded before dispatch so their emitters are in the registry by the time
    // `emit list` prints it, and only when the user asked: running third-party code found
    // lying next to the executable is not something a tool should decide on its own.
    // ZIRCON_PLUGINS is the same opt-in for a shell that sets it once.
    if (plugin_dirs.empty()) {
        char from_env[MAX_PATH * 4] = {};
        if (::GetEnvironmentVariableA("ZIRCON_PLUGINS", from_env, sizeof(from_env)) > 0)
            plugin_dirs.emplace_back(from_env);
    }
    for (const auto& directory : plugin_dirs)
        zircon::plugin::LoadPluginsFrom(directory);

    if (command == "detect")      return CommandDetect();
    if (command == "install")     return CommandInstall(false);
    if (command == "uninstall")   return CommandInstall(true);
    if (command == "inject") {
        InjectOptions inject;
        inject.out_path = out_path;
        inject.dll_path = dll_path;
        inject.launch   = launch_exe;
        inject.headless = headless;
        inject.wait     = wait_for_payload;
        inject.settle   = settle_seconds;
        inject.timeout  = timeout_seconds;
        if (walk_mode == "static") {
            LogError("static mode needs no process: zircon dump --metadata <file>");
            return 1;
        }
        inject.mode = walk_mode;
        return CommandInject(spec, inject);
    }
    if (command == "fingerprint") return CommandFingerprint(spec, json_output);
    if (command == "objects")     return CommandObjects(spec, name_filter, limit);
    if (command == "names")       return CommandNames(spec, limit);
    if (command == "classes")     return CommandClasses(spec, name_filter, limit);
    if (command == "props")       return CommandProps(spec, name_filter, limit);
    if (command == "functions")   return CommandFunctions(spec, name_filter, limit);
    if (command == "script")      return CommandScript(spec, name_filter, limit);
    if (command == "read")        return CommandRead(spec, name_filter, limit);
    if (command == "write")       return CommandWrite(spec, name_filter, assignment);
    if (command == "find")        return CommandFind(spec, name_filter, predicate, limit);
    if (command == "dump") {
        // No process needed, and none asked for: read the file and stop.
        if (!metadata_path.empty() || walk_mode == "static") {
            if (metadata_path.empty()) {
                LogError("--mode static needs the metadata file: "
                         "--metadata <global-metadata.dat>");
                return 1;
            }
            return CommandStaticDump(metadata_path, out_path);
        }

        PublishAfterDump publish;
        publish.requested    = publish_after;
        publish.game         = zdex_game;
        publish.label        = zdex_label;
        publish.notes        = zdex_notes;
        publish.wait         = !no_wait;
        publish.assume_yes   = assume_yes;
        publish.open_browser = open_browser;
        return CommandDump(spec, out_path, name_filter, with_names,
                           with_script, with_defaults, emit_format,
                           allow_partial, publish, launch_exe, timeout_seconds);
    }
    if (command == "check")       return CommandCheck(validate_path);
    if (command == "validate")    return CommandValidate(validate_path, strict, limit);
    if (command == "xref")        return CommandXref(validate_path, name_filter, uses, limit);
    if (command == "login")       return zircon::app::CommandLogin(
                                             zdex_key.empty() ? publish_path : zdex_key);
    if (command == "logout")      return zircon::app::CommandLogout();
    if (command == "fetch")       return zircon::app::CommandFetch(
                                             std::strtoll(publish_path.c_str(), nullptr, 10),
                                             fetch_kind.empty() ? "json" : fetch_kind,
                                             out_path);
    if (command == "publish") {
        zircon::app::PublishOptions publish;
        publish.path         = publish_path;
        publish.game         = zdex_game;
        publish.label        = zdex_label;
        publish.notes        = zdex_notes;
        publish.wait         = !no_wait;
        publish.json_output  = json_output;
        publish.assume_yes   = assume_yes;
        publish.open_browser = open_browser;
        publish.dry_run      = dry_run;
        publish.all          = publish_all;
        publish.force        = publish_force;
        if (publish.all) return zircon::app::CommandPublishAll(publish);
        return zircon::app::CommandPublish(publish);
    }
    if (command == "emit")        return CommandEmit(emit_format, validate_path, out_path,
                                                     name_filter, allow_partial);
    if (command == "diff")        return CommandDiff(diff_before, diff_after, out_path,
                                                     name_filter, report_style,
                                                     breaking_only);
    if (command == "inspect")     return CommandInspect(spec, name_filter, limit);
    if (command == "browse")  return CommandBrowse(spec);
    if (command == "modules") return CommandModules(spec);
    if (command == "metadata") return CommandMetadata(validate_path);
    if (command == "scan-games") return CommandScanGames(scan_roots, out_path, json_output);
    if (command == "batch") {
        InjectOptions base;
        base.dll_path = dll_path;
        base.mode     = walk_mode == "static" ? std::string{} : walk_mode;
        base.settle   = settle_seconds;
        base.timeout  = timeout_seconds;
        return CommandBatch(validate_path, out_path, base);
    }
    if (command == "scan")    return CommandScan(spec, pattern_text, scan_module, all_regions);

    LogError("unknown command: {}", command);
    PrintUsage();
    return 1;
}
