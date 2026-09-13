#include "Emitters.h"

#include "ir/Json.h"

#include <filesystem>
#include <string>

namespace zircon::emit {
namespace {

std::string Join(std::string_view dir, std::string_view leaf) {
    std::filesystem::path path(dir);
    path /= leaf;
    return path.lexically_normal().string();
}

// Copied because the dump arrives by const reference and belongs to the caller.
ir::Dump FilterPackages(const ir::Dump& dump, const std::string& filter) {
    if (filter.empty()) return dump;

    ir::Dump out;
    out.schema_version = dump.schema_version;
    out.header         = dump.header;
    out.names          = dump.names;

    for (const auto& package : dump.packages) {
        if (package.name.find(filter) == std::string::npos) continue;
        out.packages.push_back(package);
    }
    return out;
}

// Named after the source process so several dumps can share an output directory without
// quietly overwriting each other.
std::string FileStem(const ir::Dump& dump) {
    const std::string& process = dump.header.source.process;
    if (process.empty()) return "dump";

    const auto dot = process.find_last_of('.');
    return util::SanitizeIdentifier(dot == std::string::npos ? process
                                                             : process.substr(0, dot));
}

} // namespace

EmitResult EmitJson(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    const ir::Dump filtered = FilterPackages(dump, options.package_filter);
    if (!options.package_filter.empty() && filtered.packages.empty()) {
        result.warnings.push_back("package filter '" + options.package_filter +
                                  "' matched no packages; the dump is header-only");
    }

    const std::string path = Join(options.out_dir, FileStem(dump) + ".json");
    if (!util::WriteFile(path, ir::WriteJsonString(filtered, true), result.error))
        return result;

    result.files.push_back(path);
    return result;
}

} // namespace zircon::emit
