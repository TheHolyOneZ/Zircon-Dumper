#pragma once

#include "core/Types.h"

#include <memory>
#include <span>
#include <string>
#include <type_traits>

namespace zircon::core {

// The single abstraction the whole tool is built on. Everything above core/ reads
// target memory exclusively through this interface, which is why the same reflection
// walker serves a live process, a crash dump and a file on disk.
class IMemorySource {
public:
    virtual ~IMemorySource() = default;

    // Reads up to `size` bytes and returns how many were actually read. Short reads
    // are legal and expected: page boundaries in live processes, unmapped tails in
    // dumps, uninitialised .bss in static images. Callers must handle them.
    virtual std::size_t Read(Address addr, void* out, std::size_t size) = 0;

    // Only Internal/External implement this, and only when writes are enabled.
    virtual bool Write(Address, const void*, std::size_t) { return false; }

    virtual std::span<const ModuleInfo> Modules() const = 0;
    virtual std::span<const RegionInfo> Regions() const = 0;
    virtual Capabilities                Caps()    const = 0;

    // Human-readable description for dump headers and log output.
    virtual std::string Describe() const = 0;

    const ModuleInfo* FindModule(std::string_view name) const;
    const ModuleInfo* MainModule() const;
    const RegionInfo* RegionContaining(Address addr) const;
};

// Typed helpers are free functions, never virtuals: one virtual call per read is
// already the bottleneck in External mode, and templated virtuals are not a thing.

template <typename T>
bool ReadInto(IMemorySource& mem, Address addr, T& out) {
    static_assert(std::is_trivially_copyable_v<T>, "ReadInto requires a POD type");
    return mem.Read(addr, &out, sizeof(T)) == sizeof(T);
}

template <typename T>
T ReadOr(IMemorySource& mem, Address addr, T fallback = T{}) {
    T value{};
    return ReadInto(mem, addr, value) ? value : fallback;
}

template <typename T>
bool ReadArrayInto(IMemorySource& mem, Address addr, T* out, std::size_t count) {
    static_assert(std::is_trivially_copyable_v<T>, "ReadArrayInto requires a POD type");
    const std::size_t bytes = sizeof(T) * count;
    return mem.Read(addr, out, bytes) == bytes;
}

// Reads a NUL-terminated narrow string, chunked so we never over-read into an
// unmapped page just because the string sat near a boundary.
std::string ReadCString(IMemorySource& mem, Address addr, std::size_t max_len = 2048);

// Wraps any source in a page-granular cache. Without it External mode is unusable for a
// full walk; see CachedMemorySource.cpp for the numbers and the choice of policy.
std::unique_ptr<IMemorySource> MakeCached(std::unique_ptr<IMemorySource> inner,
                                          std::size_t cache_bytes = 64ull * 1024 * 1024);

// ---- Provider factories -----------------------------------------------------------
// Each returns an Error instead of throwing; the CLI turns that into a clean message.

Result<std::unique_ptr<IMemorySource>> OpenInternal();
Result<std::unique_ptr<IMemorySource>> OpenExternalByPid(std::uint32_t pid);
Result<std::unique_ptr<IMemorySource>> OpenExternalByName(std::string_view process_name);
Result<std::unique_ptr<IMemorySource>> OpenDumpFile(std::string_view path);
Result<std::unique_ptr<IMemorySource>> OpenStaticImage(std::string_view path);

} // namespace zircon::core
