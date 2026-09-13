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

#include "core/Log.h"
#include "core/Term.h"
#include "core/MemorySource.h"
#include "emit/Emitter.h"
#include "engine/DumpBuilder.h"
#include "engine/ProcessEvent.h"
#include "engine/StructLayout.h"
#include "engine/UnrealDetect.h"
#include "ir/Json.h"

#if ZIRCON_WITH_GUI
#include "Host.h"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <memory>
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

void RunDump(const engine::Reflection& reflection, const std::filesystem::path& out) {
    engine::BuildOptions build;
    build.include_script = true;

    core::LogInfo("building the dump (this walks every object once)");
    const ir::Dump dump = engine::BuildDump(reflection, build);
    core::LogInfo("dump: {} packages, {} classes, {} structs, {} enums, {} properties, "
                  "{} functions",
                  dump.packages.size(), dump.TotalClasses(), dump.TotalStructs(),
                  dump.TotalEnums(), dump.TotalProperties(), dump.TotalFunctions());

    std::string error;
    if (!emit::util::EnsureDirectory(out.string(), error)) {
        core::LogError("cannot create {}: {}", out.string(), error);
        return;
    }

    for (const char* name : kDefaultEmitters) {
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

// Held for the lifetime of the payload: a generated SDK may call in at any point while
// the browser is open.
std::unique_ptr<core::IMemorySource> g_memory;
engine::Reflection                   g_reflection;
engine::ProcessEventInfo             g_process_event;

DWORD WINAPI PayloadThread(LPVOID) {
    OpenConsole();
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
    std::string log_error;
    if (emit::util::EnsureDirectory(out.string(), log_error))
        core::SetLogFile((out / "zircon.log").string());

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
