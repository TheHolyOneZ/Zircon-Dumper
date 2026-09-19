// Injectable payload. Load this DLL into a UE game and it runs the same pipeline the CLI
// runs, only backed by InternalMemorySource, which is the one provider that can call game
// functions: CDO construction, StaticFindObject, invoking UFunctions.
//
// Everything else works fine externally, so injection is a capability upgrade rather than
// the normal path. Nothing below is specific to being injected beyond the provider. Reflect,
// BuildDump and every emitter are the same code the CLI calls, which is what IMemorySource
// is for.
//
// The browser gets a window of the payload's own rather than being drawn over the game's.
// An overlay means hooking the swap chain's Present, and that means writing to code the
// game owns, which the scope rules out (docs/SCOPE.md, "Game modification"). A separate
// window costs nothing, works whether the game renders with D3D11, D3D12 or Vulkan, and
// leaves the game's memory alone.

#include "core/Breadcrumb.h"
#include "core/Log.h"
#include "core/Term.h"
#include "core/MemorySource.h"
#include "emit/Emitter.h"
#include "engine/DumpBuilder.h"
#include "engine/ProcessEvent.h"
#include "engine/StructLayout.h"
#include "engine/UnrealDetect.h"
#include "il2cpp/Bridge.h"
#include "il2cpp/Runtime.h"
#include "il2cpp/Static.h"
#include "il2cpp/Walker.h"
#include "ir/Json.h"
#include "zdex/Gzip.h"

#if ZIRCON_WITH_GUI
#include "Host.h"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace zircon;

namespace {

HMODULE g_self = nullptr;
FILE*   g_console_out = nullptr;

// Next to the DLL, not the game executable. Game directories are often read-only (Steam,
// Program Files) and a silently failed write is worse than an obvious one.
std::filesystem::path OutputDirectory() {
    wchar_t buffer[MAX_PATH * 2] = {};
    if (::GetModuleFileNameW(g_self, buffer, static_cast<DWORD>(std::size(buffer))))
        return std::filesystem::path(buffer).parent_path() / "zircon-out";
    return std::filesystem::current_path() / "zircon-out";
}

void OpenConsole() {
    if (!::AllocConsole()) return;
    freopen_s(&g_console_out, "CONOUT$", "w", stdout);
    freopen_s(&g_console_out, "CONOUT$", "w", stderr);
    ::SetConsoleTitleW(L"Zircon - injected payload");

    // The console only exists now, so terminal setup has to happen here rather than at
    // load time. Without it the payload's log does not look like the CLI's.
    core::InitTerminal();
}

void CloseConsole() {
    if (g_console_out) std::fclose(g_console_out);
    ::FreeConsole();
}

// What is worth writing unattended. Someone who injected the payload wants the SDK and the
// mappings without asking. The rest are one CLI command away from the resulting dump.json.
constexpr const char* kDefaultEmitters[] = {"cpp_sdk", "usmap", "json"};

// What a Unity target gets. json only for now -- the other emitters are Unreal-shaped and
// printing a C# type as a UClass is worse than not offering it.
constexpr const char* kIl2CppEmitters[] = {"json"};

void WriteDump(const ir::Dump& dump, const std::filesystem::path& out,
               std::span<const char* const> emitters) {
    std::string error;
    if (!emit::util::EnsureDirectory(out.string(), error)) {
        core::LogError("cannot create {}: {}", out.string(), error);
        return;
    }

    for (const char* name : emitters) {
        const auto* emitter = emit::FindEmitter(name);
        if (!emitter) continue;

        emit::EmitOptions options;
        options.out_dir = (out / name).string();

        const auto result = emitter->emit(dump, options);
        if (!result.ok()) {
            core::LogError("{}: {}", name, result.error);
            continue;
        }
        for (const auto& warning : result.warnings) core::LogWarn("{}: {}", name, warning);
        core::LogInfo("{}: {} file(s) -> {}", name, result.files.size(), options.out_dir);
    }
}

// A file name that survives being a file name. Process names are tame, but a game is free
// to call itself anything and one colon in there loses the whole log.
std::string SafeStem(std::string_view name) {
    if (name.size() > 4 && name.substr(name.size() - 4) == ".exe")
        name.remove_suffix(4);

    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '_');
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    return out.empty() ? "game" : out;
}

std::string Stamp() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(now));
}

// One log per injection rather than one file everybody appends to. Four games dumped in an
// evening used to land in one zircon.log with nothing to tell the runs apart.
void StartLog(const std::filesystem::path& out, std::string_view process) {
    std::string error;
    if (!emit::util::EnsureDirectory((out / "logs").string(), error)) {
        // Better one shared log than none.
        if (emit::util::EnsureDirectory(out.string(), error))
            core::SetLogFile((out / "zircon.log").string());
        return;
    }

    const auto path = out / "logs" / std::format("{}-{}.log", SafeStem(process), Stamp());
    core::SetLogFile(path.string());
    core::LogInfo("log: {}", path.string());
}

// The payload has no command line, so its switches are files beside the DLL. Same trick as
// the const marker.
constexpr const char* kSkipFile = "zircon-il2cpp-skip.txt";

// What `zircon inject` passes in for this one load: key=value, read once and deleted. The
// marker files beside it are settings that stick; this is an argument list for a payload
// that has no command line.
constexpr const char* kHandoffFile = "zircon-payload.cfg";

struct Handoff {
    std::filesystem::path out;     // -o: where the dump goes
    std::filesystem::path status;  // --wait: where to say how it went, when it is over
    bool headless{false};          // --headless: no console, it steals focus from a game
    int  settle{0};                // --wait-for-settle: seconds to let the class cache stop
    std::string mode;              // --mode: "live" or "dual"; empty means live
};

// Filled from the handoff file the moment the payload starts, because whether to open a
// console has to be decided before anything prints.
Handoff g_handoff;

Handoff TakeHandoff(const std::filesystem::path& path) {
    Handoff handoff;

    std::ifstream in(path);
    if (!in) return handoff;

    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto split = line.find('=');
        if (split == std::string::npos) continue;

        const auto key   = std::string_view(line).substr(0, split);
        const auto value = std::string_view(line).substr(split + 1);
        if (key == "out" && !value.empty())         handoff.out = std::filesystem::path(value);
        else if (key == "status" && !value.empty()) handoff.status = std::filesystem::path(value);
        else if (key == "headless")                 handoff.headless = value == "1";
        else if (key == "settle")                   handoff.settle = std::atoi(std::string(value).c_str());
        else if (key == "mode" && !value.empty())   handoff.mode = std::string(value);
    }
    in.close();

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return handoff;
}

// How the caller finds out it is over. Written last, once, and the CLI waits on it existing
// rather than on a file growing -- a 600 MB write is non-empty long before it is finished.
void ReportStatus(std::string_view outcome, std::string_view detail) {
    if (g_handoff.status.empty()) return;

    std::error_code ignored;
    std::filesystem::create_directories(g_handoff.status.parent_path(), ignored);

    std::ofstream out(g_handoff.status, std::ios::trunc);
    if (out) out << outcome << "\n" << detail << "\n";
}

// One breadcrumb line per line, blanks and # comments ignored. The whole point is that you
// paste the line the last crash left, so it is matched exactly and never trimmed into
// something broader.
std::vector<std::string> ReadSkipList(const std::filesystem::path& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in) return out;

    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        out.push_back(line);
    }
    return out;
}

// Types this game's runtime has already faulted on, one per line. Written by the payload,
// not by hand -- the hand-written list is kSkipFile.
std::filesystem::path ResumeFile(const std::filesystem::path& out, std::string_view game) {
    return out / "logs" / (std::string(SafeStem(game)) + ".unreadable");
}

void RememberUnreadable(const std::filesystem::path& path, std::string_view type) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);

    for (const auto& known : ReadSkipList(path))
        if (known == type) return;              // already on the list, don't grow the file

    std::ofstream out(path, std::ios::app);
    if (out) out << type << "\n";
}

// Waits for the runtime's class cache to stop growing.
//
// The cache fills as a game touches types, so injecting at the menu and injecting after a
// level loads give different dumps of the same build. Polling the count until it holds
// still costs one call per second and makes the answer repeatable. `seconds` is a ceiling,
// not a target: a cache that settles in ten seconds is not waited on for the rest.
void WaitForSettle(il2cpp::IBridge& bridge, int seconds) {
    std::size_t last  = 0;
    int         still = 0;

    core::LogInfo("waiting for the class cache to stop growing (up to {}s)", seconds);
    for (int elapsed = 0; elapsed < seconds; ++elapsed) {
        const std::size_t now = bridge.AllClasses().size();
        if (now == last && now > 0) {
            // Three in a row, because one identical reading is a pause and not a plateau.
            if (++still >= 3) {
                core::LogInfo("class cache settled at {} after {}s", now, elapsed);
                return;
            }
        } else {
            still = 0;
        }
        last = now;
        ::Sleep(1000);
    }
    core::LogWarn("the class cache was still growing after {}s ({} classes); dumping anyway, "
                  "and this dump may not match another of the same build", seconds, last);
}

// Waits until the runtime is actually ready to be asked questions.
//
// A module being mapped is not the same as a runtime being up. `zircon inject --launch`
// injects as soon as GameAssembly.dll appears, which is about a second into a cold start,
// and at that point il2cpp_domain_get already returns something -- a domain with no
// assemblies in it yet. Walking that takes the game down with no useful breadcrumb, because
// the walk never got far enough to write one.
//
// So ask the runtime rather than sleeping: poll for a domain with assemblies in it, and for
// that count to stop moving. Two identical readings a second apart is the runtime saying it
// has finished loading what it loads at startup.
bool WaitForRuntime(const il2cpp::RuntimeInfo& runtime, int seconds) {
    using FnDomainGet = void* (*)();
    using FnAssemblies = void** (*)(void*, std::size_t*);

    const auto domain_get = reinterpret_cast<FnDomainGet>(
        static_cast<std::uintptr_t>(core::Raw(runtime.api.domain_get)));
    const auto assemblies = reinterpret_cast<FnAssemblies>(
        static_cast<std::uintptr_t>(core::Raw(runtime.api.domain_get_assemblies)));
    if (!domain_get || !assemblies) return false;

    std::size_t last = 0;
    int steady = 0;
    for (int elapsed = 0; elapsed < seconds; ++elapsed) {
        if (void* domain = domain_get()) {
            std::size_t count = 0;
            if (assemblies(domain, &count) && count > 0) {
                if (count == last && ++steady >= 2) {
                    core::LogInfo("runtime ready after {}s: {} assemblies loaded", elapsed,
                                  count);
                    return true;
                }
                if (count != last) steady = 0;
                last = count;
            }
        }
        ::Sleep(1000);
    }

    core::LogWarn("the runtime still had no settled assembly list after {}s; walking anyway",
                  seconds);
    return last > 0;
}

// Writes the dump, compressed when the path says so.
//
// A Unity dump is 600 MB of JSON and about 20 of gzip, and four games in an evening is the
// difference between 2.5 GB and a hundred. The plain file is written first and then streamed
// through the compressor rather than held twice in memory, and it goes away afterwards.
void WriteJsonMaybeGzipped(const ir::Dump& dump, const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);

    const bool compress = path.extension() == ".gz";
    const auto plain = compress ? std::filesystem::path(path).replace_extension() : path;

    std::string error;
    if (!ir::WriteJsonFile(dump, plain.string(), error)) {
        core::LogError("cannot write {}: {}", plain.string(), error);
        return;
    }
    if (!compress) {
        core::LogInfo("json: {}", plain.string());
        return;
    }

    zdex::GzipStats stats;
    if (!zdex::GzipFile(plain.string(), path.string(), error, &stats)) {
        core::LogError("wrote {} but could not compress it: {}", plain.string(), error);
        return;
    }
    std::filesystem::remove(plain, ignored);
    core::LogInfo("json: {} ({:.0f}x smaller than the {} MiB it was)", path.string(),
                  stats.ratio(), static_cast<std::uint64_t>(stats.raw >> 20));
}

// Unity puts the metadata at a fixed place inside a game folder. A layout convention rather
// than a version table: when it is not there, dual mode says so and the live half still runs.
std::filesystem::path MetadataBeside(const std::filesystem::path& exe) {
    std::error_code ec;
    const auto folder = exe.parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_directory()) continue;
        const auto candidate = entry.path() / "il2cpp_data" / "Metadata" / "global-metadata.dat";
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}

// The metadata half of a dual run. Empty when there is nothing to read, which is reported
// and is not fatal -- a dual run that finds no metadata is a live run with a warning.
std::optional<ir::Dump> ReadMetadataHalf(const std::filesystem::path& exe,
                                         il2cpp::StaticStats& stats) {
    const auto path = MetadataBeside(exe);
    if (path.empty()) {
        core::LogWarn("dual mode asked for, but no global-metadata.dat was found beside {}",
                      exe.string());
        return std::nullopt;
    }
    core::LogInfo("metadata: {}", path.string());

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        core::LogWarn("cannot open {}", path.string());
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);

    std::vector<std::uint8_t> bytes(size);
    if (size && !in.read(reinterpret_cast<char*>(bytes.data()),
                         static_cast<std::streamsize>(size))) {
        core::LogWarn("cannot read {}", path.string());
        return std::nullopt;
    }

    const auto solved = il2cpp::SolveMetadataLayout(bytes);
    if (!solved) {
        core::LogWarn("{}", solved.error().message);
        return std::nullopt;
    }
    for (const auto& line : solved.value().evidence) core::LogInfo("  - {}", line);
    return il2cpp::ReadStaticDump(bytes, solved.value(), stats);
}

// --- fault watch ---------------------------------------------------------------------
//
// Watches for the walk faulting and writes what it was into the breadcrumb, then gets out
// of the way. It does not handle anything: catching the fault would leave the runtime's
// locks held and take the game down later somewhere unrelated, which is worse than the
// fault and much harder to read.
//
// Everything here runs on a thread that has just faulted, so no allocation, no logging and
// no loader calls. Hex into a stack buffer and a memcpy into the mapped page.

// The two ways a fault line can start. A prefix rather than something to parse out of the
// text, because the resume logic below turns on which one it is.
constexpr const char* kFaultInRuntime = "faulted inside the runtime:";
constexpr const char* kFaultElsewhere = "faulted outside the runtime:";

core::CrashBreadcrumb* g_fault_crumb  = nullptr;
std::uint64_t          g_fault_base   = 0;
std::uint64_t          g_fault_size   = 0;
LONG volatile          g_fault_seen   = 0;
void*                  g_fault_token  = nullptr;
DWORD                  g_walk_thread  = 0;

char* HexInto(char* at, char* end, std::uint64_t value) {
    char digits[17];
    int  n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while (value && n < 16);
    while (n-- > 0 && at < end) *at++ = digits[n];
    return at;
}

char* TextInto(char* at, char* end, const char* text) {
    while (*text && at < end) *at++ = *text++;
    return at;
}

LONG CALLBACK FaultWatch(EXCEPTION_POINTERS* info) {
    // Our thread only. A handler registered process-wide sees every thread, and a game
    // taking a first-chance fault of its own would otherwise get whatever type the walk
    // happened to be on written down as unreadable.
    if (::GetCurrentThreadId() != g_walk_thread) return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = info->ExceptionRecord->ExceptionCode;

    // Only the ones that mean something went wrong at the machine level. Games raise C++
    // and CLR exceptions all the time and those are none of our business.
    const bool interesting = code == EXCEPTION_ACCESS_VIOLATION ||
                             code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                             code == EXCEPTION_PRIV_INSTRUCTION ||
                             code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                             code == EXCEPTION_STACK_OVERFLOW;
    if (!interesting || !g_fault_crumb) return EXCEPTION_CONTINUE_SEARCH;

    // First one only. A fault often repeats on the way down and the first is the one that
    // tells you anything.
    if (::InterlockedCompareExchange(&g_fault_seen, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char  buffer[256];
    char* at  = buffer;
    char* end = buffer + sizeof(buffer) - 1;

    // Which side faulted goes first, so a reader doesn't have to parse an address to find
    // out. Inside the runtime means the type is broken; outside means we are.
    const auto pc = reinterpret_cast<std::uint64_t>(info->ExceptionRecord->ExceptionAddress);
    const bool in_runtime = g_fault_size && pc >= g_fault_base &&
                            pc < g_fault_base + g_fault_size;

    at = TextInto(at, end, in_runtime ? kFaultInRuntime : kFaultElsewhere);
    at = TextInto(at, end, " code 0x");
    at = HexInto(at, end, code);
    at = TextInto(at, end, " at 0x");
    at = HexInto(at, end, pc);

    if (in_runtime) {
        at = TextInto(at, end, " (+0x");
        at = HexInto(at, end, pc - g_fault_base);
        at = TextInto(at, end, ")");
    }

    // On an access violation the record carries what was being read or written, which is
    // usually the more useful half.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        const auto kind = info->ExceptionRecord->ExceptionInformation[0];
        at = TextInto(at, end, kind == 1 ? " writing 0x" : " reading 0x");
        at = HexInto(at, end, info->ExceptionRecord->ExceptionInformation[1]);
    }

    *at = '\0';

    g_fault_crumb->Append(buffer);
    return EXCEPTION_CONTINUE_SEARCH;
}

// False when this isn't a Unity game: carry on into the Unreal path.
bool RunIl2CppDump(core::IMemorySource& memory, const std::filesystem::path& out) {
    const auto runtime = il2cpp::FindRuntime(memory);
    if (!runtime) return false;

    core::LogInfo("Unity IL2CPP: {} at {:#x}", runtime->module_name,
                  core::Raw(runtime->module_base));
    for (const auto& line : runtime->evidence) core::LogInfo("  - {}", line);

    if (!runtime->api.Complete()) {
        core::LogError("this build does not export everything the walk needs, so the dump "
                       "would be missing whole categories rather than a few entries");
        for (const auto& name : runtime->api.missing) core::LogError("  missing {}", name);
        ReportStatus("failed", "this build has stripped exports the walk needs");
        return true;   // handled: it is Unity, and the answer is no
    }

    // Reading consts is the one call that makes the runtime do work rather than answer, and
    // the one a build might not survive. On by default (an enum without values is half an
    // enum), off with a marker file beside the DLL like the ProcessEvent probe.
    const bool read_consts = !std::filesystem::exists(out.parent_path() / "zircon-il2cpp-no-consts");
    if (!read_consts)
        core::LogInfo("const reading is off, so enums will carry names without values");

    const auto& requested_out = g_handoff.out;
    if (!requested_out.empty())
        core::LogInfo("writing the dump to {} as asked", requested_out.string());

    // Before anything calls into it. Cheap when the game has been up for a while, and the
    // difference between a dump and a dead game when it has not.
    WaitForRuntime(*runtime, 120);

    auto bridge = il2cpp::MakeInProcessBridge(*runtime, memory, read_consts);
    if (!bridge) {
        core::LogError("{}", bridge.error().message);
        ReportStatus("failed", bridge.error().message);
        return true;
    }

    // A breadcrumb from last time means the last walk didn't come back. Say what it was on
    // before starting the next one, because that name is what --skip wants.
    // Per game, so two games dumped at once don't overwrite each other's, and so the
    // "last run died here" lookup finds the right one next time.
    const auto game = memory.MainModule() ? memory.MainModule()->name : std::string{};
    const auto crumb_path  = out / "logs" / (SafeStem(game) + ".breadcrumb");
    const auto resume_path = ResumeFile(out, game);

    if (const auto last = core::ReadBreadcrumb(crumb_path)) {
        const auto key = core::BreadcrumbKey(*last);
        core::LogWarn("a previous walk stopped at {}", key);

        // Whatever else it managed to record: which member list it was on, and the fault
        // itself if the watcher below caught it.
        bool runtime_faulted = false;
        auto rest = core::BreadcrumbPhase(*last);
        while (!rest.empty()) {
            const auto cut  = rest.find('\n');
            const auto line = rest.substr(0, cut);
            if (!line.empty()) core::LogWarn("  {}", line);
            if (line.starts_with(kFaultInRuntime)) runtime_faulted = true;
            if (cut == std::string_view::npos) break;
            rest = rest.substr(cut + 1);
        }

        // Only when the runtime faulted inside its own code. That means the type's metadata
        // is broken and no amount of asking differently will fix it, so walking past it is
        // the right answer. A fault anywhere else is our bug and gets to keep crashing until
        // someone looks at it.
        if (runtime_faulted && !key.empty()) {
            RememberUnreadable(resume_path, key);
            core::LogWarn("the runtime faulted in its own code on that type, so it goes in "
                          "{} and this walk will go around it", resume_path.string());
        } else if (!key.empty()) {
            core::LogWarn("no runtime fault was recorded, so this is not being skipped on "
                          "its own; put that first line in {} if you want it walked past",
                          (out.parent_path() / kSkipFile).string());
        }
    }

    core::CrashBreadcrumb crumb;
    crumb.Open(crumb_path);

    g_fault_crumb = &crumb;
    g_fault_base  = core::Raw(runtime->module_base);
    g_fault_size  = runtime->module_size;
    g_walk_thread = ::GetCurrentThreadId();
    g_fault_seen  = 0;
    g_fault_token = ::AddVectoredExceptionHandler(1, FaultWatch);

    // The class cache grows while a game runs, and the inflated-generic sweep reads it, so
    // when you inject changes what you get. Waiting for it to stop moving is what makes two
    // dumps of one build comparable. It is not a sleep: the count is the thing being asked.
    if (g_handoff.settle > 0) WaitForSettle(*bridge.value(), g_handoff.settle);

    core::LogInfo("walking the runtime (domain, assemblies, images, classes)");
    il2cpp::WalkOptions options;
    options.skip = ReadSkipList(out.parent_path() / kSkipFile);
    for (auto& entry : ReadSkipList(resume_path)) options.skip.push_back(std::move(entry));
    if (crumb.IsOpen())
        options.breadcrumb = [&crumb](std::string_view what) { crumb.Note(what); };
    for (const auto& entry : options.skip)
        core::LogInfo("skipping {} on request", entry);

    il2cpp::WalkStats  stats;
    ir::Dump dump = il2cpp::Walk(*bridge.value(), options, stats);

    if (g_fault_token) ::RemoveVectoredExceptionHandler(g_fault_token);
    g_fault_crumb = nullptr;
    g_walk_thread = 0;
    crumb.Finish();

    // A dump that quietly lost types is the thing this tool is supposed not to produce, so
    // what was skipped travels with it into the header and onto the website.
    for (const auto& entry : options.skip)
        dump.header.engine.evidence.push_back(
            std::format("walked past {} on request; it is not in this dump", entry));

    dump.header.tool_version  = ZIRCON_VERSION;
    dump.header.source.kind    = "internal";
    dump.header.source.process = memory.MainModule() ? memory.MainModule()->name : "";

    core::LogInfo("dump: {} assemblies, {} classes, {} structs, {} enums, {} fields, "
                  "{} methods, {} properties",
                  stats.images, dump.TotalClasses(), dump.TotalStructs(), dump.TotalEnums(),
                  stats.fields, stats.methods, stats.accessors);
    core::LogInfo("{} method bodies resolved, {} of them shared with another method",
                  stats.bodies, stats.shared_bodies);
    if (stats.inflated)
        core::LogInfo("{} generic instantiations swept out of the class cache", stats.inflated);
    if (stats.indistinguishable)
        core::LogInfo("{} instantiations were over another generic's parameter rather than a "
                      "type, so only the first of each name is in the dump",
                      stats.indistinguishable);
    if (stats.open_generics)
        core::LogInfo("{} open generic definitions, whose field offsets are left unresolved",
                      stats.open_generics);
    if (stats.enums_without_values)
        core::LogWarn("{} enums kept their member names but not their values; this build's "
                      "runtime will not read a const", stats.enums_without_values);
    if (stats.skipped)
        core::LogWarn("{} types were skipped on request and are absent from this dump",
                      stats.skipped);
    if (stats.contradictory_bases)
        core::LogInfo("{} types have a base the runtime reports as bigger than they are, so "
                      "their inherited size is left out rather than written down as a "
                      "contradiction", stats.contradictory_bases);

    // Dual: ask the file the same questions and keep both answers.
    if (g_handoff.mode == "dual") {
        il2cpp::StaticStats from_file;
        const auto exe = memory.MainModule() ? std::filesystem::path(memory.MainModule()->path)
                                             : std::filesystem::path{};
        if (auto other = ReadMetadataHalf(exe, from_file)) {
            core::LogInfo("metadata half: {} assemblies, {} types, {} fields, {} methods",
                          from_file.images, from_file.types, from_file.fields,
                          from_file.methods);

            il2cpp::MergeStats merged;
            dump = il2cpp::MergeDumps(dump, *other, merged);
            core::LogInfo("merged: {} types in both, {} only the runtime had, {} only the "
                          "metadata declared", merged.in_both, merged.live_only,
                          merged.static_only);
            if (merged.conflicts)
                core::LogWarn("{} disagreements between the two, all recorded in the dump "
                              "header rather than resolved quietly", merged.conflicts);
        } else {
            core::LogWarn("carrying on with the live reading alone");
            dump.header.sources = {"live"};
        }
    } else {
        dump.header.sources = {"live"};
    }

    // With a path asked for, write exactly there and nowhere else. Scripting this otherwise
    // means guessing a filename the payload derived from the process name.
    if (requested_out.empty()) {
        WriteDump(dump, out, kIl2CppEmitters);
    } else {
        WriteJsonMaybeGzipped(dump, requested_out);
    }
    core::LogInfo("publishable: 'zircon publish <the json above> --game ... --label ...'");

    ReportStatus("ok", requested_out.empty() ? out.string() : requested_out.string());
    return true;
}

void RunDump(const engine::Reflection& reflection, const std::filesystem::path& out) {
    engine::BuildOptions build;
    build.include_script = true;

    core::LogInfo("building the dump (this walks every object once)");
    const ir::Dump dump = engine::BuildDump(reflection, build);
    core::LogInfo("dump: {} packages, {} classes, {} structs, {} enums, {} properties, "
                  "{} functions",
                  dump.packages.size(), dump.TotalClasses(), dump.TotalStructs(),
                  dump.TotalEnums(), dump.TotalProperties(), dump.TotalFunctions());

    WriteDump(dump, out, kDefaultEmitters);
}

// Held for the lifetime of the payload: a generated SDK may call in at any point while
// the browser is open.
std::unique_ptr<core::IMemorySource> g_memory;
engine::Reflection                   g_reflection;
engine::ProcessEventInfo             g_process_event;

DWORD WINAPI PayloadThread(LPVOID) {
    g_handoff = TakeHandoff(OutputDirectory().parent_path() / kHandoffFile);
    if (!g_handoff.headless) OpenConsole();
    core::SetLogLevel(core::LogLevel::Debug);

    core::LogInfo("Zircon payload attached to pid {}", ::GetCurrentProcessId());

    auto source = core::OpenInternal();
    if (!source) {
        core::LogError("{}", source.error().message);
        return 1;
    }

    auto memory = core::MakeCached(std::move(source.value()));
    core::LogInfo("target: {}", memory->Describe());

    const auto* main_module = memory->MainModule();
    if (main_module) {
        core::LogInfo("main module: {} at {:#x} ({} MiB)", main_module->name,
                      core::Raw(main_module->base), main_module->size >> 20);
    }

    // Unity before Unreal. IL2CPP is a yes/no; the Unreal check is a score that'll return
    // a low number about a non-Unreal game and leave someone reading the wrong log.
    {
        const auto out = OutputDirectory();
        StartLog(out, main_module ? main_module->name : std::string{});

        if (RunIl2CppDump(*memory, out)) {
            core::LogInfo("output directory: {}", out.string());
            core::LogInfo("press END to unload");
            while ((::GetAsyncKeyState(VK_END) & 1) == 0) ::Sleep(50);
            core::CloseLogFile();
            CloseConsole();
            ::FreeLibraryAndExitThread(g_self, 0);
        }
    }

    // Confirm we are inside something Unreal-shaped, so a mis-injection is obvious now
    // rather than as a confusing dump later.
    core::ProcessInfo self;
    self.pid  = ::GetCurrentProcessId();
    self.name = main_module ? main_module->name : std::string{};
    self.path = main_module ? main_module->path : std::string{};

    const auto scored = engine::ScoreProcess(self);
    core::LogInfo("Unreal confidence: {:.2f} ({})", scored.confidence,
                  scored.project.empty() ? "unknown project" : scored.project);
    for (const auto& reason : scored.evidence)
        core::LogInfo("  - {}", reason);

    auto reflection = engine::Reflect(*memory);
    if (!reflection.Valid()) {
        core::LogError("could not derive the reflection layout; nothing to do");
        core::LogInfo("press END to unload");
        while ((::GetAsyncKeyState(VK_END) & 1) == 0) ::Sleep(50);
        CloseConsole();
        ::FreeLibraryAndExitThread(g_self, 0);
    }

    const auto out = OutputDirectory();
    core::LogInfo("output directory: {}", out.string());

    // From here the log is also written to a file, because the console is not somewhere a
    // game leaves readable.
    StartLog(out, self.name);

    // Before the dump, so the slot lands in the header the SDK is generated from. Finding
    // it calls through vtable slots that are not it, and those calls usually cost the game
    // a few seconds later, so it is opt-in: a marker file beside the DLL is a deliberate
    // act in a way a default is not.
    if (std::filesystem::exists(out.parent_path() / "zircon-find-processevent")) {
        core::LogWarn("probing for ProcessEvent; this calls unknown virtuals and the game "
                      "will probably not survive it");
        g_process_event = engine::DeriveProcessEvent(reflection.Context(),
                                                     reflection.class_layout);
        if (g_process_event.Valid()) {
            reflection.process_event_index = g_process_event.vtable_index;
            core::LogInfo("baking ProcessEvent slot {} into the dump",
                          g_process_event.vtable_index);
        }
    } else {
        core::LogInfo("ProcessEvent probing is off; create 'zircon-find-processevent' "
                      "beside the DLL to enable it");
    }

    RunDump(reflection, out);

    // Published before the browser opens, so an SDK compiled from this dump can bind as
    // soon as the payload is in.
    g_reflection        = reflection;
    g_memory            = std::move(memory);
    g_reflection.memory = g_memory.get();
    core::LogInfo("zircon_find_object exported for a generated SDK");



#if ZIRCON_WITH_GUI
    // Browser takes over from here, in its own window.
    core::LogInfo("opening the live browser; close its window to unload");
    gui::RunBrowserWindow(std::move(g_memory), g_reflection);
#else
    core::LogInfo("press END in the game window to unload");
    while ((::GetAsyncKeyState(VK_END) & 1) == 0) ::Sleep(50);
#endif

    core::LogInfo("unloading");
    core::CloseLogFile();
    CloseConsole();
    ::FreeLibraryAndExitThread(g_self, 0);
}

} // namespace

// Half of what a generated SDK needs, and the half that can be answered honestly.
//
// A wrapper has to turn "/Script/Engine.PawnMovementComponent.GetPawnOwner" into a
// UFunction pointer. That is an object-path lookup, which is the walk this payload has
// already done, so the SDK needs neither StaticFindObject nor us to go find it.
//
// The other half is UObject::ProcessEvent, a virtual whose vtable index the reflection
// data does not record. It stays the caller's one line. Guessing an index would be an
// unverifiable answer, and a wrong one calls something arbitrary on a live object.
// The vtable slot UObject::ProcessEvent occupies, or -1 when it was never looked for.
// A generated SDK can turn that into its ProcessEvent hook without knowing the engine
// version.
extern "C" __declspec(dllexport) int zircon_process_event_index() {
    return g_process_event.vtable_index;
}

extern "C" __declspec(dllexport) void* zircon_find_object(const char* full_path) {
    if (!full_path || !g_memory || !g_reflection.Valid()) return nullptr;

    const auto object = engine::FindObjectByPath(*g_memory, g_reflection.array,
                                                 g_reflection.object_layout,
                                                 g_reflection.pool, full_path);
    return core::IsNull(object) ? nullptr : reinterpret_cast<void*>(core::Raw(object));
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    g_self = module;

    // No real work in DllMain. It runs under the loader lock, where almost anything
    // interesting - loading a library, creating a thread that waits on one - deadlocks.
    ::DisableThreadLibraryCalls(module);

    HANDLE thread = ::CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
    if (thread) ::CloseHandle(thread);

    return TRUE;
}
