#pragma once

// Where the API key and the resume state live.
//
// %APPDATA%\Zircon\config.json    the key, the base URL, whether the terms were shown
// %APPDATA%\Zircon\uploads.json   in-flight upload ids, so a killed run resumes
//
// The key never goes near a project directory, a log line or a dump. ZDEX_API_KEY and
// ZDEX_URL in the environment win over the file, which is what a CI job wants.

#include <cstdint>
#include <string>
#include <vector>

namespace zircon::zdex {

inline constexpr const char* kDefaultBaseUrl = "https://zlogic.eu/zdex";

struct Config {
    std::string api_key;
    std::string base_url{kDefaultBaseUrl};

    // Key prefix the terms were last confirmed for. Stored rather than a plain bool so
    // switching accounts asks again.
    std::string terms_accepted_for;

    bool from_environment{false};   // key came from ZDEX_API_KEY, so do not rewrite the file

    std::string ApiBase() const { return base_url + "/api/v1"; }
    bool HasKey() const { return !api_key.empty(); }

    // "zdx_1a2b…f9" — safe to print. Never print api_key itself.
    std::string KeyHint() const;
};

Config LoadConfig();
bool SaveConfig(const Config& config, std::string& error);
bool ClearConfig(std::string& error);

std::string ConfigPath();

// --- resume state --------------------------------------------------------------------

struct UploadRecord {
    std::string fingerprint;    // path + size + mtime, so a rebuilt dump is a new upload
    std::string upload_id;
    std::uint64_t size{0};
    std::string game;
    std::string label;
};

std::vector<UploadRecord> LoadUploads();
void RememberUpload(const UploadRecord& record);
void ForgetUpload(const std::string& fingerprint);

// path + size + mtime. A dump regenerated from the same game is a different file and must
// not resume into the previous session's chunks.
std::string FingerprintFile(const std::string& path);

} // namespace zircon::zdex
