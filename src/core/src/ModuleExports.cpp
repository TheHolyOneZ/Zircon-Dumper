#include "core/ModuleExports.h"

namespace zircon::core {
namespace {

constexpr std::uint16_t kDosMagic = 0x5A4D;
constexpr std::uint32_t kPeMagic  = 0x00004550;
constexpr std::uint32_t kMaxExportNames = 65536;
constexpr std::size_t   kMaxNameLength  = 512;

} // namespace

std::vector<PeExport> ReadModuleExports(IMemorySource& memory, Address module_base) {
    std::vector<PeExport> out;
    if (IsNull(module_base)) return out;

    if (ReadOr<std::uint16_t>(memory, module_base) != kDosMagic) return out;

    const auto e_lfanew = ReadOr<std::int32_t>(memory, module_base + 0x3C);
    if (e_lfanew <= 0 || e_lfanew > 0x10000) return out;

    const Address nt = module_base + static_cast<std::uint64_t>(e_lfanew);
    if (ReadOr<std::uint32_t>(memory, nt) != kPeMagic) return out;

    const auto magic = ReadOr<std::uint16_t>(memory, nt + 0x18);
    const bool pe64  = magic == 0x20B;
    const Address data_dir = nt + 0x18 + (pe64 ? 0x70 : 0x60);

    const auto export_rva = ReadOr<std::uint32_t>(memory, data_dir);
    if (export_rva == 0) return out;

    const Address dir = module_base + export_rva;
    const auto name_count = ReadOr<std::uint32_t>(memory, dir + 0x18);
    const auto names_rva  = ReadOr<std::uint32_t>(memory, dir + 0x20);
    const auto ords_rva   = ReadOr<std::uint32_t>(memory, dir + 0x24);
    const auto funcs_rva  = ReadOr<std::uint32_t>(memory, dir + 0x1C);
    const auto ord_base   = ReadOr<std::uint32_t>(memory, dir + 0x10);

    if (name_count == 0 || name_count > kMaxExportNames) return out;
    if (names_rva == 0 || ords_rva == 0 || funcs_rva == 0) return out;

    out.reserve(name_count);
    for (std::uint32_t i = 0; i < name_count; ++i) {
        const auto name_rva = ReadOr<std::uint32_t>(memory, module_base + names_rva + i * 4);
        if (name_rva == 0) continue;

        std::string name = ReadCString(memory, module_base + name_rva, kMaxNameLength);
        if (name.empty()) continue;

        const auto ordinal  = ReadOr<std::uint16_t>(memory, module_base + ords_rva + i * 2);
        const auto func_rva = ReadOr<std::uint32_t>(memory,
                                                    module_base + funcs_rva + ordinal * 4);
        if (func_rva == 0) continue;

        out.push_back(PeExport{std::move(name), func_rva,
                               static_cast<std::uint16_t>(ordinal + ord_base)});
    }
    return out;
}

} // namespace zircon::core
