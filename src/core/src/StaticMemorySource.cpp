#include "core/Log.h"
#include "core/MemorySource.h"
#include "core/PeImage.h"

#include <format>

namespace zircon::core {
namespace {

// Reads a PE on disk as if mapped at its preferred base. GObjects and the FName pool live
// in .data/.bss and only get populated at runtime, so this reports live_objects = false and
// the reflection layer refuses to walk objects through it.
//
// It's for locating globals by pattern scan, validating signatures across builds, and
// feeding the disassembler emitters. None of that needs a running game.
class StaticMemorySource final : public IMemorySource {
public:
    explicit StaticMemorySource(PeImage image) : image_(std::move(image)) {
        modules_.push_back(ModuleInfo{
            image_.Name(), std::string{},
            static_cast<Address>(image_.PreferredBase()),
            image_.SizeOfImage(),
        });

        regions_.reserve(image_.Sections().size());
        for (const auto& section : image_.Sections()) {
            regions_.push_back(RegionInfo{
                static_cast<Address>(image_.PreferredBase() + section.rva),
                std::max(section.virtual_size, section.raw_size),
                section.protect,
                true,
            });
        }
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        const std::uint64_t base = image_.PreferredBase();
        if (Raw(addr) < base) return 0;

        const std::uint64_t rva = Raw(addr) - base;
        if (rva > 0xFFFFFFFFull) return 0;

        return image_.ReadRva(static_cast<std::uint32_t>(rva), out, size);
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }

    Capabilities Caps() const override {
        return Capabilities{
            /*live_objects*/       false,
            /*writable*/           false,
            /*can_call*/           false,
            /*full_address_space*/ false,
        };
    }

    std::string Describe() const override {
        return std::format("static PE '{}' at preferred base {:#x} ({})",
                           image_.Name(), image_.PreferredBase(),
                           image_.Is64Bit() ? "x64" : "x86");
    }

private:
    PeImage                 image_;
    std::vector<ModuleInfo> modules_;
    std::vector<RegionInfo> regions_;
};

} // namespace

Result<std::unique_ptr<IMemorySource>> OpenStaticImage(std::string_view path) {
    auto image = PeImage::FromFile(path);
    if (!image) return image.error();

    if (!image.value().Is64Bit())
        LogWarn("'{}' is a 32-bit image; UE shipping games are normally x64", path);

    LogInfo("static image: {} sections, {} exports",
            image.value().Sections().size(), image.value().Exports().size());

    return std::unique_ptr<IMemorySource>(
        std::make_unique<StaticMemorySource>(std::move(image.value())));
}

} // namespace zircon::core
