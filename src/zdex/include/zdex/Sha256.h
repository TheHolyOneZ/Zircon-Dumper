#pragma once

// SHA-256, because Zdex keys a dump on the hash of its uncompressed JSON and the client has
// to be able to work out the same number. Knowing it before the upload starts is what makes
// re-running a batch cheap: a dump that is already up does not need sending again.
//
// Written rather than pulled in, the same call as the gzip code next door. It is a published
// algorithm with published test vectors, and the tests check against those.

#include <cstdint>
#include <string>
#include <string_view>

namespace zircon::zdex {

// Lowercase hex, 64 characters. Same spelling the server stores.
std::string Sha256Hex(std::string_view data);

// For a file too large to want in memory twice. Returns empty when the file will not open.
std::string Sha256File(const std::string& path);

} // namespace zircon::zdex
