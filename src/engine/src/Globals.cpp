#include "engine/Globals.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <optional>
#include <set>
#include <string_view>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

struct Section {
    std::string   name;
    std::uint32_t rva{};
    std::uint32_t size{};
    std::uint32_t characteristics{};

    bool Executable() const { return (characteristics & 0x20000000u) != 0; }
    bool Writable() const { return (characteristics & 0x80000000u) != 0; }
};

constexpr std::uint32_t kInitializedData = 0x00000040u;

std::vector<Section> ReadSections(core::IMemorySource& memory, Address base) {
    std::vector<Section> sections;
    std::uint8_t header[0x1000]{};
    if (memory.Read(base, header, sizeof(header)) != sizeof(header)) return sections;
    if (header[0] != 'M' || header[1] != 'Z') return sections;

    std::uint32_t pe = 0;
    std::memcpy(&pe, header + 0x3C, 4);
    if (pe + 24 > sizeof(header) || std::memcmp(header + pe, "PE\0\0", 4) != 0) return sections;

    std::uint16_t count = 0, optional_size = 0;
    std::memcpy(&count, header + pe + 6, 2);
    std::memcpy(&optional_size, header + pe + 20, 2);

    const std::size_t first = pe + 24 + optional_size;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t at = first + static_cast<std::size_t>(i) * 40;
        if (at + 40 > sizeof(header)) break;

        Section section;
        char name[9]{};
        std::memcpy(name, header + at, 8);
        section.name = name;
        std::memcpy(&section.size, header + at + 8, 4);
        std::memcpy(&section.rva, header + at + 12, 4);
        std::memcpy(&section.characteristics, header + at + 36, 4);
        sections.push_back(std::move(section));
    }
    return sections;
}

std::vector<std::uint8_t> ReadSection(core::IMemorySource& memory, Address base,
                                      const Section& section) {
    std::vector<std::uint8_t> bytes(section.size, 0);
    constexpr std::size_t kChunk = 1u << 20;
    for (std::size_t offset = 0; offset < bytes.size(); offset += kChunk) {
        const std::size_t want = std::min(kChunk, bytes.size() - offset);
        memory.Read(base + section.rva + offset, bytes.data() + offset, want);
    }
    return bytes;
}

struct CodeView {
    std::uint32_t             rva{};
    std::vector<std::uint8_t> bytes;

    bool Contains(std::uint64_t target_rva) const {
        return target_rva >= rva && target_rva < rva + bytes.size();
    }

    const std::uint8_t* At(std::uint64_t target_rva) const {
        return bytes.data() + (target_rva - rva);
    }

    std::size_t Remaining(std::uint64_t target_rva) const {
        return bytes.size() - static_cast<std::size_t>(target_rva - rva);
    }
};

bool HasFlagTest(const std::uint8_t* code, std::size_t length, std::uint32_t immediate,
                 int required_displacement) {
    for (std::size_t i = 0; i + 10 <= length; ++i) {
        if (code[i] != 0xF7) continue;

        const std::uint8_t modrm = code[i + 1];
        const int mod = modrm >> 6;
        const int reg = (modrm >> 3) & 7;
        const int rm  = modrm & 7;
        if (reg != 0 || mod == 0 || mod == 3) continue;

        std::size_t at = i + 2;
        if (rm == 4) ++at;

        std::int32_t displacement = 0;
        if (mod == 1) {
            displacement = static_cast<std::int8_t>(code[at]);
            at += 1;
        } else {
            std::memcpy(&displacement, code + at, 4);
            at += 4;
        }
        if (at + 4 > length) continue;

        std::uint32_t value = 0;
        std::memcpy(&value, code + at, 4);
        if (value != immediate) continue;
        if (required_displacement >= 0 && displacement != required_displacement) continue;
        return true;
    }
    return false;
}

std::vector<std::uint64_t> FindAll(const std::vector<std::uint8_t>& haystack,
                                   std::string_view needle) {
    std::vector<std::uint64_t> hits;
    if (needle.empty() || haystack.size() < needle.size()) return hits;
    auto it = haystack.begin();
    while (true) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end());
        if (it == haystack.end()) break;
        hits.push_back(static_cast<std::uint64_t>(it - haystack.begin()));
        ++it;
    }
    return hits;
}

std::string Utf16(std::string_view text) {
    std::string out;
    for (char c : text) {
        out.push_back(c);
        out.push_back('\0');
    }
    return out;
}

void FindProcessEvent(core::IMemorySource& memory, Address base, Address object_cdo,
                      const CodeView& text, int function_flags, GlobalsInfo& info) {
    if (IsNull(object_cdo) || function_flags < 0) return;

    const auto vtable = core::ReadOr<Address>(memory, object_cdo);
    if (IsNull(vtable)) return;

    constexpr int kSlots = 0xA0;
    std::vector<int> matches;
    Address match_address{};

    for (int slot = 0; slot < kSlots; ++slot) {
        const auto entry = core::ReadOr<Address>(memory, vtable + static_cast<std::uint64_t>(slot) * 8);
        if (IsNull(entry) || Raw(entry) < Raw(base)) continue;

        const std::uint64_t rva = Raw(entry) - Raw(base);
        if (!text.Contains(rva)) continue;

        const std::uint8_t* code = text.At(rva);
        const std::size_t   left = text.Remaining(rva);

        if (!HasFlagTest(code, std::min<std::size_t>(left, 0x400), 0x00000400u, function_flags))
            continue;
        if (!HasFlagTest(code, std::min<std::size_t>(left, 0xF00), 0x00400000u, -1))
            continue;

        matches.push_back(slot);
        match_address = entry;
    }

    if (matches.size() == 1) {
        info.process_event_slot = matches.front();
        info.process_event      = match_address;
        info.evidence.push_back(std::format(
            "ProcessEvent: vtable slot {:#x} is the only UObject virtual testing "
            "FunctionFlags@{:#x} for FUNC_Native and FUNC_HasOutParms",
            matches.front(), function_flags));
    } else if (matches.size() > 1) {
        info.evidence.push_back(std::format(
            "ProcessEvent: {} vtable slots match the code signature; not choosing one",
            matches.size()));
    } else {
        info.evidence.push_back("ProcessEvent: no vtable slot matches the code signature");
    }
}

void FindGWorld(core::IMemorySource& memory, Address base, const std::vector<Section>& sections,
                const std::vector<Address>& worlds, GlobalsInfo& info) {
    if (worlds.empty()) {
        info.evidence.push_back("GWorld: no UWorld instance is loaded");
        return;
    }

    std::set<std::uint64_t> wanted;
    for (const auto world : worlds) wanted.insert(Raw(world));

    std::set<std::uint64_t> hits;
    for (const auto& section : sections) {
        if (!section.Writable() || section.Executable()) continue;
        if ((section.characteristics & kInitializedData) == 0) continue;

        const auto bytes = ReadSection(memory, base, section);
        for (std::size_t at = 0; at + 8 <= bytes.size(); at += 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data() + at, 8);
            if (wanted.contains(value)) hits.insert(section.rva + at);
        }
    }

    if (hits.size() == 1) {
        info.gworld = base + *hits.begin();
        info.evidence.push_back(std::format(
            "GWorld: the only module global pointing at a live UWorld, at RVA {:#x}",
            *hits.begin()));
    } else if (hits.size() > 1) {
        info.evidence.push_back(std::format(
            "GWorld: {} module globals point at a live UWorld; not choosing one", hits.size()));
    } else {
        info.evidence.push_back("GWorld: no module global points at a live UWorld");
    }
}

void FindAppendString(core::IMemorySource& memory, Address base,
                      const std::vector<Section>& sections, const CodeView& text,
                      GlobalsInfo& info) {
    constexpr std::string_view kAnchor = "ForwardShadingQuality_";

    std::vector<std::uint64_t> anchors;
    for (const auto& section : sections) {
        if (section.Executable() || section.Writable()) continue;
        if ((section.characteristics & kInitializedData) == 0) continue;

        const auto bytes = ReadSection(memory, base, section);
        for (const auto hit : FindAll(bytes, kAnchor)) anchors.push_back(section.rva + hit);
        for (const auto hit : FindAll(bytes, Utf16(kAnchor))) anchors.push_back(section.rva + hit);
    }
    if (anchors.empty()) {
        info.evidence.push_back("AppendString: anchor string not present");
        return;
    }

    std::set<std::uint64_t> results;
    const auto& code = text.bytes;
    for (std::size_t i = 0; i + 7 <= code.size(); ++i) {
        if ((code[i] != 0x48 && code[i] != 0x4C) || code[i + 1] != 0x8D) continue;
        if ((code[i + 2] & 0xC7) != 0x05) continue;

        std::int32_t displacement = 0;
        std::memcpy(&displacement, code.data() + i + 3, 4);
        const std::uint64_t target = text.rva + i + 7 + static_cast<std::int64_t>(displacement);
        if (std::find(anchors.begin(), anchors.end(), target) == anchors.end()) continue;

        std::vector<std::uint64_t> calls;
        const std::size_t end = std::min(code.size() - 5, i + 7 + 0x60);
        for (std::size_t j = i + 7; j < end && calls.size() < 2; ++j) {
            if (code[j] != 0xE8) continue;
            std::int32_t relative = 0;
            std::memcpy(&relative, code.data() + j + 1, 4);
            const std::uint64_t callee = text.rva + j + 5 + static_cast<std::int64_t>(relative);
            if (!text.Contains(callee)) continue;
            if (!calls.empty() && calls.back() == callee) continue;
            calls.push_back(callee);
        }
        if (calls.size() == 2) results.insert(calls[1]);
    }

    if (results.size() == 1) {
        info.append_string = base + *results.begin();
        info.evidence.push_back(std::format(
            "AppendString: the call after the FName built from \"{}\", at RVA {:#x}",
            kAnchor, *results.begin()));
    } else if (results.size() > 1) {
        info.evidence.push_back(std::format(
            "AppendString: {} candidates after the anchor string; not choosing one",
            results.size()));
    } else {
        info.evidence.push_back("AppendString: no call pattern after the anchor string");
    }
}

}

GlobalsInfo FindGlobals(core::IMemorySource& memory, const ObjectArrayInfo& array,
                        const UObjectLayout& object_layout, const NamePoolInfo& pool,
                        const UFunctionLayout& function_layout) {
    GlobalsInfo info;

    const auto* module = memory.MainModule();
    if (!module) return info;
    const Address base = module->base;

    const auto sections = ReadSections(memory, base);
    const auto text_section = std::find_if(sections.begin(), sections.end(),
                                           [](const Section& s) { return s.Executable(); });
    if (text_section == sections.end()) {
        info.evidence.push_back("globals: the main module has no executable section");
        return info;
    }

    CodeView text;
    text.rva   = text_section->rva;
    text.bytes = ReadSection(memory, base, *text_section);

    Address world_class{};
    Address object_cdo{};
    for (std::int32_t index = 0; index < array.num_elements; ++index) {
        const auto object = ObjectAt(memory, array, index);
        if (IsNull(object)) continue;

        const std::string name = GetObjectName(memory, object_layout, pool, object);
        if (name == "World" && IsNull(world_class)) {
            const auto klass = GetObjectClass(memory, object_layout, object);
            if (!IsNull(klass) && GetObjectName(memory, object_layout, pool, klass) == "Class")
                world_class = object;
        } else if (name == "Default__Object" && IsNull(object_cdo)) {
            object_cdo = object;
        }
        if (!IsNull(world_class) && !IsNull(object_cdo)) break;
    }

    std::vector<Address> worlds;
    if (!IsNull(world_class)) {
        for (std::int32_t index = 0; index < array.num_elements; ++index) {
            const auto object = ObjectAt(memory, array, index);
            if (IsNull(object)) continue;
            if (GetObjectClass(memory, object_layout, object) != world_class) continue;
            if (GetObjectName(memory, object_layout, pool, object).starts_with("Default__")) continue;
            worlds.push_back(object);
        }
    }

    FindGWorld(memory, base, sections, worlds, info);
    FindProcessEvent(memory, base, object_cdo, text, function_layout.function_flags, info);
    FindAppendString(memory, base, sections, text, info);

    for (const auto& line : info.evidence) core::LogInfo("{}", line);
    return info;
}

}
