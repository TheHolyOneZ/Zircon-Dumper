#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

namespace zircon::core {

// A target-process virtual address. Deliberately a distinct type: mixing a target
// address with a host pointer is the single easiest way to crash an external dumper,
// and in Dump/Static modes a target address is never dereferenceable at all.
enum class Address : std::uint64_t {};

constexpr Address operator+(Address a, std::uint64_t d) {
    return static_cast<Address>(static_cast<std::uint64_t>(a) + d);
}
constexpr Address operator-(Address a, std::uint64_t d) {
    return static_cast<Address>(static_cast<std::uint64_t>(a) - d);
}
constexpr std::uint64_t Raw(Address a) { return static_cast<std::uint64_t>(a); }
constexpr bool IsNull(Address a) { return Raw(a) == 0; }

struct ModuleInfo {
    std::string name;       // "Game-Win64-Shipping.exe"
    std::string path;       // full path when known, empty otherwise
    Address     base{};
    std::uint64_t size{};
};

enum class RegionProtect : std::uint8_t {
    None    = 0,
    Read    = 1 << 0,
    Write   = 1 << 1,
    Execute = 1 << 2,
};

constexpr RegionProtect operator|(RegionProtect a, RegionProtect b) {
    return static_cast<RegionProtect>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool HasFlag(RegionProtect v, RegionProtect f) {
    return (static_cast<std::uint8_t>(v) & static_cast<std::uint8_t>(f)) != 0;
}

struct RegionInfo {
    Address       base{};
    std::uint64_t size{};
    RegionProtect protect{RegionProtect::None};
    bool          is_image{false};   // backed by a mapped module
};

// What a memory source can actually do. The reflection layer branches on these rather
// than on the provider type, so adding a fifth provider never touches engine/ code.
struct Capabilities {
    bool live_objects{false};        // GObjects is populated (false for Static)
    bool writable{false};            // Internal / External with --allow-write
    bool can_call{false};            // Internal only: we can invoke game functions
    bool full_address_space{false};  // every mapped region is readable
};

// Minimal Result<T>. std::expected is C++23 and we are targeting C++20 for MSVC
// compatibility across the toolchains people actually have installed.
struct Error {
    std::string message;
    int         code{0};
};

template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}

    bool ok() const { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const { return ok(); }

    const T& value() const { return std::get<T>(storage_); }
    T&       value()       { return std::get<T>(storage_); }
    const Error& error() const { return std::get<Error>(storage_); }

private:
    std::variant<T, Error> storage_;
};

// Operations that either work or explain why. Spelling it Result<void> keeps one idiom
// across the codebase instead of a second convention for the value-less case.
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    bool ok() const { return !error_.has_value(); }
    explicit operator bool() const { return ok(); }

    const Error& error() const { return *error_; }

private:
    std::optional<Error> error_;
};

} // namespace zircon::core
