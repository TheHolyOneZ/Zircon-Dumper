// Finding, loading and binding plugins.
//
// A plugin is a DLL exporting zircon_plugin_main. It gets handed a ZnHost, registers
// whatever emitters it provides, returns. Those emitters then show up in
// `zircon emit --list` beside the built-ins and run the same way.
//
// Loading third-party code into the process is a real decision, so it's opt-in: nothing is
// scanned unless the user passes --plugins or sets ZIRCON_PLUGINS. A dumper that quietly
// ran whatever DLL happened to be sitting next to it would be a poor thing to hand someone.

#include "plugin/Loader.h"
#include "plugin/Api.h"
#include "plugin/Nodes.h"

#include "engine/Hooks.h"

#include "core/Log.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace zircon::plugin {
namespace {

namespace fs = std::filesystem;

// The module stays loaded for the life of the process. An emitter it registered is a
// callback into its code, and the registry has no unregister.
struct LoadedPlugin {
    HMODULE     module{};
    std::string path;
    ZnHost      host{};
};

std::vector<std::unique_ptr<LoadedPlugin>>& Loaded() {
    static std::vector<std::unique_ptr<LoadedPlugin>> loaded;
    return loaded;
}

const char* HostPluginPath(ZnHost* host) {
    auto* self = static_cast<LoadedPlugin*>(host->_reserved);
    return self ? self->path.c_str() : nullptr;
}

// The descriptor's strings belong to the plugin and can be freed the moment it returns,
// so copy everything.
int HostRegisterEmitter(ZnHost* host, const ZnEmitterDesc* desc) {
    if (!desc || !desc->name || !desc->emit) return ZN_ERR_NULL;

    auto* self = static_cast<LoadedPlugin*>(host->_reserved);
    const std::string origin = self ? fs::path(self->path).filename().string() : "plugin";

    const ZnEmitFn callback = desc->emit;
    void* const    user     = desc->user;

    emit::Emitter bridged;
    bridged.name          = desc->name;
    bridged.description   = desc->description ? desc->description : "";
    bridged.needs_objects = desc->needs_objects != 0;

    bridged.emit = [callback, user](const ir::Dump& dump,
                                    const emit::EmitOptions& options) -> emit::EmitResult {
        EmitContext context;
        context.options = options;

        const int status = callback(reinterpret_cast<ZnEmitContext*>(&context),
                                    MakeNode(&dump, ZN_KIND_DUMP), user);

        // Fail silently and the CLI reports success having written nothing.
        if (status != ZN_OK && context.result.error.empty())
            context.result.error = std::format("the plugin's emitter returned {}", status);

        return std::move(context.result);
    };

    if (!emit::RegisterEmitter(std::move(bridged))) {
        core::LogWarn("{}: refused to register emitter '{}' (the name is already taken)",
                      origin, desc->name);
        return ZN_ERR_REFUSED;
    }

    core::LogDebug("{}: registered emitter '{}'", origin, desc->name);
    return ZN_OK;
}

// --- the two engine hooks --------------------------------------------------------------
//
// Both are process-wide, since both describe the target and not one walk of it. A second
// registration is refused instead of quietly replacing the first — two plugins
// each convinced they own name decoding is a thing to report, not to arbitrate.

bool g_decoder_taken  = false;
bool g_resolver_taken = false;

int HostRegisterNameDecoder(ZnHost* host, ZnNameDecoderFn fn, void* user) {
    if (!fn) return ZN_ERR_NULL;

    auto* self = static_cast<LoadedPlugin*>(host->_reserved);
    const std::string origin = self ? fs::path(self->path).filename().string() : "plugin";

    if (g_decoder_taken) {
        core::LogWarn("{}: a name decoder is already installed; refusing a second", origin);
        return ZN_ERR_REFUSED;
    }

    engine::SetNameEntryDecoder(
        [fn, user](core::IMemorySource& memory, core::Address entry,
                   std::uint8_t* out, std::size_t capacity) -> std::size_t {
            return fn(AsTarget(memory), core::Raw(entry), out, capacity, user);
        });

    g_decoder_taken = true;
    core::LogInfo("{}: installed a name-entry decoder", origin);
    return ZN_OK;
}

int HostRegisterGlobalResolver(ZnHost* host, ZnGlobalResolverFn fn, void* user) {
    if (!fn) return ZN_ERR_NULL;

    auto* self = static_cast<LoadedPlugin*>(host->_reserved);
    const std::string origin = self ? fs::path(self->path).filename().string() : "plugin";

    if (g_resolver_taken) {
        core::LogWarn("{}: a global resolver is already installed; refusing a second", origin);
        return ZN_ERR_REFUSED;
    }

    engine::SetGlobalResolver(
        [fn, user](core::IMemorySource& memory, engine::GlobalCandidates& out) -> bool {
            std::uint64_t gobjects = 0, name_pool = 0;
            if (!fn(AsTarget(memory), &gobjects, &name_pool, user)) return false;

            out.gobjects  = static_cast<core::Address>(gobjects);
            out.name_pool = static_cast<core::Address>(name_pool);
            return true;
        });

    g_resolver_taken = true;
    core::LogInfo("{}: installed a global resolver", origin);
    return ZN_OK;
}

LoadResult LoadOne(const fs::path& path) {
    LoadResult result;
    result.path = path.string();

    HMODULE module = ::LoadLibraryW(path.c_str());
    if (!module) {
        result.error = std::format("could not load ({})", ::GetLastError());
        return result;
    }

    auto entry = reinterpret_cast<ZnPluginMainFn>(
        reinterpret_cast<void*>(::GetProcAddress(module, "zircon_plugin_main")));
    if (!entry) {
        ::FreeLibrary(module);
        result.error = "no zircon_plugin_main export; not a Zircon plugin";
        return result;
    }

    auto loaded  = std::make_unique<LoadedPlugin>();
    loaded->module = module;
    loaded->path   = path.string();

    loaded->host.size             = sizeof(ZnHost);
    loaded->host.api              = HostApi();
    loaded->host.register_emitter = &HostRegisterEmitter;
    loaded->host.plugin_path      = &HostPluginPath;
    loaded->host._reserved        = loaded.get();

    loaded->host.register_name_decoder    = &HostRegisterNameDecoder;
    loaded->host.register_global_resolver = &HostRegisterGlobalResolver;

    const std::size_t before = emit::Emitters().size();

    const int status = entry(&loaded->host);
    if (status != ZN_OK) {
        ::FreeLibrary(module);
        result.error = std::format("zircon_plugin_main returned {}", status);
        return result;
    }

    result.emitters = emit::Emitters().size() - before;
    result.ok       = true;
    Loaded().push_back(std::move(loaded));
    return result;
}

} // namespace

std::vector<LoadResult> LoadPluginsFrom(std::string_view directory) {
    std::vector<LoadResult> results;

    std::error_code ec;
    const fs::path root{directory};
    if (!fs::is_directory(root, ec)) {
        LoadResult missing;
        missing.path  = root.string();
        missing.error = "not a directory";
        results.push_back(std::move(missing));
        return results;
    }

    // Sorted, so several plugins load in the same order every time and a name clash is
    // decided the same way every run.
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".dll") candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& candidate : candidates) {
        auto result = LoadOne(candidate);
        if (!result.ok)
            core::LogWarn("plugin {}: {}", fs::path(result.path).filename().string(),
                          result.error);
        else
            core::LogInfo("plugin {}: {} emitter(s)",
                          fs::path(result.path).filename().string(), result.emitters);
        results.push_back(std::move(result));
    }
    return results;
}

} // namespace zircon::plugin
