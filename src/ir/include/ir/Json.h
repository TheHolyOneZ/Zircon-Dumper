#pragma once

// JSON serialisation for the IR. Hand-rolled on purpose: the project carries no external
// dependencies, and the format is small and fixed enough that a library would be more
// surface than the parser it replaces.

#include "ir/Model.h"

#include <optional>
#include <string>
#include <string_view>

namespace zircon::ir {

struct JsonError {
    std::string message;
    std::size_t offset{0};   // byte offset into the input, so failures are locatable
};

// `ir` must not depend on `core`, so it cannot use core::Result. This is the same idea
// scoped to this layer.
template <typename T>
class JsonExpected {
public:
    JsonExpected(T value) : value_(std::move(value)) {}
    JsonExpected(JsonError error) : error_(std::move(error)) {}

    bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return ok(); }

    const T& value() const { return *value_; }
    T&       value()       { return *value_; }
    const JsonError& error() const { return error_; }

private:
    std::optional<T> value_;
    JsonError        error_;
};

// Pretty output is the default for dumps a human will open; compact is for pipes and for
// diffing tools that do not care about whitespace.
std::string WriteJsonString(const Dump& dump, bool pretty = true);
bool WriteJsonFile(const Dump& dump, std::string_view path, std::string& error,
                   bool pretty = true);

JsonExpected<Dump> ParseJson(std::string_view text);
JsonExpected<Dump> ReadJsonFile(std::string_view path);

// Maximum container nesting the parser will accept. Hostile or corrupt input must fail
// with an error instead of recursing until the stack runs out.
inline constexpr int kMaxJsonDepth = 64;

} // namespace zircon::ir
