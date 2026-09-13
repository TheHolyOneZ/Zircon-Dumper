#include "core/MemorySource.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace zircon::core {

const ModuleInfo* IMemorySource::FindModule(std::string_view name) const {
    const auto modules = Modules();
    const auto it = std::find_if(modules.begin(), modules.end(), [&](const ModuleInfo& m) {
        if (m.name.size() != name.size()) return false;
        return std::equal(m.name.begin(), m.name.end(), name.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    });
    return it == modules.end() ? nullptr : &*it;
}

const ModuleInfo* IMemorySource::MainModule() const {
    const auto modules = Modules();
    return modules.empty() ? nullptr : &modules.front();
}

const RegionInfo* IMemorySource::RegionContaining(Address addr) const {
    const auto regions = Regions();
    const auto it = std::find_if(regions.begin(), regions.end(), [&](const RegionInfo& r) {
        return Raw(addr) >= Raw(r.base) && Raw(addr) < Raw(r.base) + r.size;
    });
    return it == regions.end() ? nullptr : &*it;
}

std::string ReadCString(IMemorySource& mem, Address addr, std::size_t max_len) {
    // Chunked, so a string near a page boundary doesn't turn into one big read that
    // straddles unmapped memory and fails wholesale.
    constexpr std::size_t kChunk = 64;
    std::string out;
    std::array<char, kChunk> buffer{};

    while (out.size() < max_len) {
        const std::size_t want = std::min(kChunk, max_len - out.size());
        const std::size_t got  = mem.Read(addr + out.size(), buffer.data(), want);
        if (got == 0) break;

        const auto* end = std::find(buffer.data(), buffer.data() + got, '\0');
        out.append(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
        if (end != buffer.data() + got) break;   // hit the terminator
        if (got < want) break;                    // short read, nothing more to get
    }
    return out;
}


} // namespace zircon::core
