#include "zdex/Config.h"
#include "zdex/Json.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>

namespace zircon::zdex {
namespace {

std::filesystem::path ConfigDir() {
    wchar_t* roaming = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) &&
        roaming) {
        base = roaming;
        ::CoTaskMemFree(roaming);
    } else {
        // No roaming profile is unusual but not fatal; the temp directory still keeps the
        // key out of the project folder, which is the property that matters.
        base = std::filesystem::temp_directory_path();
    }
    return base / "Zircon";
}

std::string ReadWhole(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteWhole(const std::filesystem::path& path, std::string_view data, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        error = "write failed for " + path.string();
        return false;
    }
    return true;
}

std::string Environment(const char* name) {
    char buffer[4096] = {};
    const DWORD size = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (size == 0 || size >= sizeof(buffer)) return {};
    return std::string(buffer, size);
}

std::filesystem::path UploadsPath() { return ConfigDir() / "uploads.json"; }

} // namespace

std::string ConfigPath() { return (ConfigDir() / "config.json").string(); }

std::string Config::KeyHint() const {
    if (api_key.size() < 10) return "(none)";
    return api_key.substr(0, 8) + "…" + api_key.substr(api_key.size() - 2);
}

Config LoadConfig() {
    Config config;

    const std::string text = ReadWhole(ConfigDir() / "config.json");
    if (!text.empty()) {
        JsonValue root;
        std::string error;
        if (ParseJson(text, root, error)) {
            config.api_key = root.Str("zdex_api_key");
            config.base_url = root.Str("zdex_url", kDefaultBaseUrl);
            config.terms_accepted_for = root.Str("zdex_terms_accepted_for");
        }
    }

    if (const std::string key = Environment("ZDEX_API_KEY"); !key.empty()) {
        config.api_key = key;
        config.from_environment = true;
    }
    if (const std::string url = Environment("ZDEX_URL"); !url.empty()) {
        config.base_url = url;
    }

    while (!config.base_url.empty() && config.base_url.back() == '/')
        config.base_url.pop_back();
    if (config.base_url.empty()) config.base_url = kDefaultBaseUrl;
    return config;
}

bool SaveConfig(const Config& config, std::string& error) {
    std::string out = "{\n";
    out += "  \"zdex_api_key\": \"" + JsonEscape(config.api_key) + "\",\n";
    out += "  \"zdex_url\": \"" + JsonEscape(config.base_url) + "\"";
    if (!config.terms_accepted_for.empty()) {
        out += ",\n  \"zdex_terms_accepted_for\": \"" +
               JsonEscape(config.terms_accepted_for) + "\"";
    }
    out += "\n}\n";

    const auto path = ConfigDir() / "config.json";
    if (!WriteWhole(path, out, error)) return false;

    // The key is a credential: take it off the "everyone can read" list. Best effort -
    // a failure here is worth neither aborting nor a warning the user cannot act on.
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return true;
}

bool ClearConfig(std::string& error) {
    std::error_code ec;
    const auto path = ConfigDir() / "config.json";
    if (!std::filesystem::exists(path, ec)) return true;
    if (!std::filesystem::remove(path, ec)) {
        error = "cannot remove " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string FingerprintFile(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    const auto when = std::filesystem::last_write_time(path, ec);
    if (ec) return {};

    return std::filesystem::absolute(path, ec).string() + "|" + std::to_string(size) + "|" +
           std::to_string(when.time_since_epoch().count());
}

std::vector<UploadRecord> LoadUploads() {
    std::vector<UploadRecord> out;
    const std::string text = ReadWhole(UploadsPath());
    if (text.empty()) return out;

    JsonValue root;
    std::string error;
    if (!ParseJson(text, root, error)) return out;

    const JsonValue& list = root["uploads"];
    for (const auto& item : list.items) {
        UploadRecord record;
        record.fingerprint = item.Str("fingerprint");
        record.upload_id = item.Str("upload_id");
        record.size = static_cast<std::uint64_t>(item.Int("size"));
        record.game = item.Str("game");
        record.label = item.Str("label");
        if (!record.fingerprint.empty() && !record.upload_id.empty())
            out.push_back(std::move(record));
    }
    return out;
}

namespace {

void WriteUploads(const std::vector<UploadRecord>& records) {
    std::string out = "{\n  \"uploads\": [\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        out += "    {\"fingerprint\": \"" + JsonEscape(r.fingerprint) +
               "\", \"upload_id\": \"" + JsonEscape(r.upload_id) +
               "\", \"size\": " + std::to_string(r.size) +
               ", \"game\": \"" + JsonEscape(r.game) +
               "\", \"label\": \"" + JsonEscape(r.label) + "\"}";
        if (i + 1 < records.size()) out += ",";
        out += "\n";
    }
    out += "  ]\n}\n";

    std::string error;
    WriteWhole(UploadsPath(), out, error);
}

} // namespace

void RememberUpload(const UploadRecord& record) {
    auto records = LoadUploads();
    for (auto& existing : records) {
        if (existing.fingerprint == record.fingerprint) {
            existing = record;
            WriteUploads(records);
            return;
        }
    }
    // Sessions expire server-side after 24 h idle, so this list is capped rather than
    // pruned by age: a stale id costs one 410 and is then forgotten.
    records.push_back(record);
    if (records.size() > 32) records.erase(records.begin(), records.end() - 32);
    WriteUploads(records);
}

void ForgetUpload(const std::string& fingerprint) {
    auto records = LoadUploads();
    const auto before = records.size();
    std::erase_if(records, [&](const UploadRecord& r) { return r.fingerprint == fingerprint; });
    if (records.size() != before) WriteUploads(records);
}

} // namespace zircon::zdex
