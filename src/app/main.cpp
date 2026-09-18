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
    std::printf("  %sdetect%s        List running Unreal Engine processes\n", c.data(), r.data());
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
    std::printf("  %sscan%s          Pattern-scan the target\n\n", c.data(), r.data());

    std::printf("%sProduce output%s\n", b.data(), r.data());
    std::printf("  %sdump%s          Full reflection dump to IR JSON\n", c.data(), r.data());
    std::printf("  %semit%s          Render a dump to one or more formats, comma separated\n",
                c.data(), r.data());
    std::printf("  %svalidate%s      Parse a dump, round-trip it, and with --strict lint it\n", c.data(), r.data());
    std::printf("  %sxref%s          What references a type, or with --uses what it references\n", c.data(), r.data());
    std::printf("  %sdiff%s          Compare two dumps and report what broke\n\n", c.data(), r.data());

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
                "      --no-wait      publish: return once uploaded, without waiting on indexing\n"
                "      --usmap        fetch: mappings instead of the dump (--sdk for the SDK zip)\n"
                "      --json         publish: machine-readable result on stdout\n"
                "      --open         publish: open the result in a browser when it is ready\n"
                "  -y, --yes          publish: skip the confirmation\n"
                "  -v, --verbose      Debug logging (repeat for trace)\n"
                "      --color/--no-color  Force colour on or off (also honours NO_COLOR)\n"
                "  -h, --help         Show this help\n"
                "      --version      Show version\n\n");

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

int CommandDetect() {
    const auto candidates = zircon::engine::DetectUnrealProcesses(0.2f);
    if (candidates.empty()) {
        LogWarn("no Unreal Engine processes detected");
        return 3;
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
    return 0;
}

int CommandFingerprint(const TargetSpec& spec) {
    auto source = OpenTarget(spec);
    if (!source) {
        LogError("{}", source.error().message);
        return source.error().code;
    }

    auto mem = MakeCached(std::move(source.value()));
    LogInfo("target: {}", mem->Describe());

    // Unity first. IL2CPP is a yes/no (a module exports the API or it doesn't), the Unreal
    // fingerprint is a score that will happily guess low about a non-Unreal game.
    if (const auto unity = zircon::il2cpp::FindRuntime(*mem)) {
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
int CommandInject(const TargetSpec& spec, std::string_view dll_path) {
    std::uint32_t pid = spec.pid;

    if (spec.kind == TargetSpec::Kind::ProcessName) {
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

    LogInfo("injecting {} into pid {}", payload.string(), pid);
    if (const auto result = zircon::core::Inject(pid, payload.string()); !result) {
        LogError("{}", result.error().message);
        return 4;
    }

    std::printf("payload loaded into pid %u\n", pid);
    std::printf("it opens its own console and window inside the game; output goes to %s\n",
                (payload.parent_path() / "zircon-out").string().c_str());
    return 0;
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

    auto before = zircon::ir::ReadJsonFile(before_path);
    if (!before.ok()) { LogError("{}: {}", before_path, before.error().message); return 6; }

    auto after = zircon::ir::ReadJsonFile(after_path);
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

    auto loaded = zircon::ir::ReadJsonFile(dump_path);
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

    auto loaded = zircon::ir::ReadJsonFile(path);
    if (!loaded.ok()) {
        LogError("{}", loaded.error().message);
        return 6;
    }

    const auto& dump = loaded.value();
    std::printf("%-16s %d\n", "schema", dump.schema_version);
    std::printf("%-16s %s\n", "tool", dump.header.tool_version.c_str());
    std::printf("%-16s %s (%.0f%%)\n", "engine", dump.header.engine.version.c_str(),
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

    auto loaded = zircon::ir::ReadJsonFile(dump_path);
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
                bool allow_partial, const PublishAfterDump& publish) {
    // resolve the formats before the walk. seventy thousand objects take a few seconds and
    // finding out afterwards that a name was mistyped is a bad trade.
    std::vector<const zircon::emit::Emitter*> emitters;
    if (!emit_formats.empty() && !ResolveEmitters(emit_formats, emitters)) return 1;

    auto source = OpenTarget(spec);
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
    if (!zircon::ir::WriteJsonFile(dump, path, error)) {
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
    if (command == "--version") { std::printf("zircon %s\n", kVersion); return 0; }

    TargetSpec  spec;
    int         verbosity = 0;
    std::string pattern_text;
    std::string scan_module;
    std::string name_filter;
    std::string assignment;
    std::string predicate;
    std::string out_path;
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
        } else if ((command == "validate" || command == "xref") &&
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
    if (command == "inject")      return CommandInject(spec, out_path);
    if (command == "fingerprint") return CommandFingerprint(spec);
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
                           allow_partial, publish);
    }
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
    if (command == "scan")    return CommandScan(spec, pattern_text, scan_module, all_regions);

    LogError("unknown command: {}", command);
    PrintUsage();
    return 1;
}
