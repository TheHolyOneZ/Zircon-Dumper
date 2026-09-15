#include "core/Log.h"
#include "core/MemorySource.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace zircon::core {
namespace {

constexpr std::size_t kPageShift = 12;
constexpr std::size_t kPageSize  = 1u << kPageShift;   // 4 KiB

// Direct-mapped page cache. A reflection walk issues millions of small reads and in
// External mode every one is a syscall, which puts uncached External about two orders of
// magnitude behind Internal.
//
// Direct-mapped rather than LRU on purpose. The access pattern is dominated by repeated
// reads of a few structures — UObject headers, the FName pool — so true LRU bookkeeping
// buys almost nothing and charges for it on every hit.
//
// Nothing ages out either. A page sits in its slot until something else wants it, which is
// fine for a one-shot walk and wrong for anything longer: the target keeps running, GC
// recycles objects, and a slot nothing conflicts with keeps serving bytes from whenever it
// was first read. Anything long-lived calls Invalidate().
class CachedMemorySource final : public IMemorySource {
public:
    CachedMemorySource(std::unique_ptr<IMemorySource> inner, std::size_t cache_bytes)
        : inner_(std::move(inner)) {
        std::size_t slots = std::max<std::size_t>(cache_bytes / kPageSize, 64);

        // Power of two so slot selection is a mask.
        std::size_t power = 1;
        while (power * 2 <= slots) power *= 2;
        slots = power;

        slot_mask_ = slots - 1;
        tags_.assign(slots, kInvalidTag);
        valid_.assign(slots, 0);
        data_.assign(slots * kPageSize, 0);

        LogDebug("read cache: {} pages ({} MiB)", slots, (slots * kPageSize) >> 20);
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        auto* dest = static_cast<std::uint8_t*>(out);
        std::size_t done = 0;

        while (done < size) {
            const std::uint64_t cur  = Raw(addr) + done;
            const std::uint64_t page = cur >> kPageShift;
            const std::size_t   in_page = static_cast<std::size_t>(cur & (kPageSize - 1));
            const std::size_t   want = std::min(size - done, kPageSize - in_page);

            const std::uint8_t* page_data = FetchPage(page);
            if (!page_data) break;

            const std::size_t valid = valid_[page & slot_mask_];
            if (in_page >= valid) break;   // page was only partially readable

            const std::size_t available = std::min(want, valid - in_page);
            std::memcpy(dest + done, page_data + in_page, available);
            done += available;

            if (available < want) break;   // ran into the unreadable tail of this page
        }

        return done;
    }

    bool EnableWrites(bool enable) override { return inner_->EnableWrites(enable); }

    void Invalidate() override {
        std::fill(tags_.begin(), tags_.end(), kInvalidTag);
        inner_->Invalidate();
    }

    bool Write(Address addr, const void* in, std::size_t size) override {
        const bool ok = inner_->Write(addr, in, size);
        // Otherwise later reads hand back the pre-write bytes.
        if (ok) InvalidateRange(addr, size);
        return ok;
    }

    std::span<const ModuleInfo> Modules() const override { return inner_->Modules(); }
    std::span<const RegionInfo> Regions() const override { return inner_->Regions(); }
    Capabilities                Caps()    const override { return inner_->Caps(); }

    std::string Describe() const override { return inner_->Describe() + " [cached]"; }

private:
    static constexpr std::uint64_t kInvalidTag = ~0ull;

    const std::uint8_t* FetchPage(std::uint64_t page) {
        const std::size_t slot = static_cast<std::size_t>(page & slot_mask_);
        std::uint8_t* slot_data = data_.data() + slot * kPageSize;

        if (tags_[slot] == page) {
            return valid_[slot] ? slot_data : nullptr;
        }

        const std::size_t got = inner_->Read(static_cast<Address>(page << kPageShift),
                                             slot_data, kPageSize);
        tags_[slot]  = page;
        valid_[slot] = got;

        // Cache the miss too. Re-issuing syscalls for known-bad addresses is a real cost
        // during a wide scan.
        return got ? slot_data : nullptr;
    }

    void InvalidateRange(Address addr, std::size_t size) {
        const std::uint64_t first = Raw(addr) >> kPageShift;
        const std::uint64_t last  = (Raw(addr) + size - 1) >> kPageShift;
        for (std::uint64_t page = first; page <= last; ++page) {
            const std::size_t slot = static_cast<std::size_t>(page & slot_mask_);
            if (tags_[slot] == page) tags_[slot] = kInvalidTag;
        }
    }

    std::unique_ptr<IMemorySource> inner_;
    std::vector<std::uint64_t>     tags_;
    std::vector<std::size_t>       valid_;
    std::vector<std::uint8_t>      data_;
    std::size_t                    slot_mask_{};
};

} // namespace

std::unique_ptr<IMemorySource> MakeCached(std::unique_ptr<IMemorySource> inner,
                                          std::size_t cache_bytes) {
    if (!inner) return inner;

    // Static images read from a buffer already; a cache would just add a copy.
    if (!inner->Caps().live_objects) return inner;

    return std::make_unique<CachedMemorySource>(std::move(inner), cache_bytes);
}

} // namespace zircon::core
