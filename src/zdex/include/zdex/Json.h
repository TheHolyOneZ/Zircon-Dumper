#pragma once

// A small read-only JSON value, for API responses.
//
// Not the IR's parser: that one produces a Dump and nothing else. This reads whatever the
// server sent so the client can pull `ok`, `error`, `upload_id`, `received[]` and friends
// out of it. Read-only and forgiving on purpose - an unexpected field is not an error, and
// a field that is missing comes back as the default rather than throwing.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace zircon::zdex {

class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind{Kind::Null};
    bool boolean{false};
    double number{0.0};
    std::string text;
    std::vector<JsonValue> items;
    std::map<std::string, JsonValue> fields;

    bool IsNull() const { return kind == Kind::Null; }

    // Object lookup. A missing key gives a Null value rather than an error, so a caller
    // can chain without checking at every step.
    const JsonValue& operator[](std::string_view key) const;

    std::string     Str(std::string_view key, std::string_view fallback = {}) const;
    std::int64_t    Int(std::string_view key, std::int64_t fallback = 0) const;
    bool            Bool(std::string_view key, bool fallback = false) const;
    std::vector<std::int64_t> IntArray(std::string_view key) const;
};

// Returns false and fills `error` when the text is not JSON at all - which is how a
// Cloudflare HTML block page or a plain-text `error code: 1010` arrives.
bool ParseJson(std::string_view text, JsonValue& out, std::string& error);

// Minimal writer, for the three request bodies the protocol needs.
std::string JsonEscape(std::string_view text);

} // namespace zircon::zdex
