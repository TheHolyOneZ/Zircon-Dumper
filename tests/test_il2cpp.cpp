// IL2CPP backend tests. Like every suite here, nothing needs a game installed: the export
// table a real GameAssembly.dll would present is built byte by byte in memory.

#include "core/MemorySource.h"
#include "il2cpp/Bridge.h"
#include "il2cpp/MethodLayout.h"
#include "il2cpp/Runtime.h"
#include "il2cpp/Walker.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace zircon::core;

namespace {

int g_failures = 0;
int g_checks   = 0;

void Check(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d  %s\n", file, line, expression);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

class FakeModule final : public IMemorySource {
public:
    FakeModule(Address base, std::vector<std::uint8_t> bytes, std::string name)
        : base_(base), bytes_(std::move(bytes)) {
        modules_.push_back(ModuleInfo{std::move(name), "", base_, bytes_.size()});
        regions_.push_back(RegionInfo{base_, bytes_.size(),
                                      RegionProtect::Read | RegionProtect::Execute, true});
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        if (Raw(addr) < Raw(base_)) return 0;
        const std::uint64_t offset = Raw(addr) - Raw(base_);
        if (offset >= bytes_.size()) return 0;
        const std::size_t available = std::min<std::size_t>(size, bytes_.size() - offset);
        std::memcpy(out, bytes_.data() + offset, available);
        return available;
    }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }
    Capabilities Caps() const override { return Capabilities{true, false, false, true}; }
    std::string  Describe() const override { return "fake il2cpp module"; }

private:
    Address                   base_;
    std::vector<std::uint8_t> bytes_;
    std::vector<ModuleInfo>   modules_;
    std::vector<RegionInfo>   regions_;
};

// A PE just complete enough to have a readable export table: DOS stub, NT headers, one data
// directory entry, and the three parallel arrays an export directory is made of. Built rather
// than checked in, so a test can decide what a build exports and what it has stripped.
std::vector<std::uint8_t> BuildPe(const std::vector<std::string>& export_names, bool pe64 = true) {
    std::vector<std::uint8_t> image(0x4000, 0);

    auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(image.data() + at, &v, 2); };
    auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(image.data() + at, &v, 4); };

    constexpr std::uint32_t kNt = 0x100;
    put16(0, 0x5A4D);                     // MZ
    put32(0x3C, kNt);
    put32(kNt, 0x00004550);               // PE\0\0
    put16(kNt + 0x18, pe64 ? 0x20B : 0x10B);

    const std::size_t data_dir = kNt + 0x18 + (pe64 ? 0x70 : 0x60);
    constexpr std::uint32_t kExportDir = 0x400;
    put32(data_dir, kExportDir);
    put32(data_dir + 4, 0x200);

    // Three arrays plus a string blob, laid out after the directory itself.
    const std::uint32_t count      = static_cast<std::uint32_t>(export_names.size());
    constexpr std::uint32_t kNames  = 0x600;
    constexpr std::uint32_t kOrds   = 0x800;
    constexpr std::uint32_t kFuncs  = 0xA00;
    constexpr std::uint32_t kBlob   = 0xC00;

    put32(kExportDir + 0x10, 1);          // ordinal base
    put32(kExportDir + 0x14, count);      // number of functions
    put32(kExportDir + 0x18, count);      // number of names
    put32(kExportDir + 0x1C, kFuncs);
    put32(kExportDir + 0x20, kNames);
    put32(kExportDir + 0x24, kOrds);

    std::uint32_t blob = kBlob;
    for (std::uint32_t i = 0; i < count; ++i) {
        put32(kNames + i * 4, blob);
        std::memcpy(image.data() + blob, export_names[i].data(), export_names[i].size());
        blob += static_cast<std::uint32_t>(export_names[i].size()) + 1;

        put16(kOrds + i * 2, static_cast<std::uint16_t>(i));
        put32(kFuncs + i * 4, 0x1000 + i * 0x10);   // any plausible code RVA
    }
    return image;
}

// Every name the resolver insists on. Kept here rather than reached into from the header so
// that dropping one from the table is a visible test change, not a silent pass.
std::vector<std::string> AllApiNames() {
    return {
        "il2cpp_domain_get", "il2cpp_domain_get_assemblies", "il2cpp_assembly_get_image",
        "il2cpp_image_get_name", "il2cpp_image_get_class_count", "il2cpp_image_get_class",
        "il2cpp_class_get_name", "il2cpp_class_get_namespace", "il2cpp_class_get_parent",
        "il2cpp_class_get_fields", "il2cpp_class_get_methods", "il2cpp_class_get_properties",
        "il2cpp_class_get_events", "il2cpp_class_get_nested_types",
        "il2cpp_class_get_interfaces", "il2cpp_class_is_valuetype", "il2cpp_class_is_enum",
        "il2cpp_class_get_flags", "il2cpp_class_get_type", "il2cpp_class_get_declaring_type",
        "il2cpp_class_instance_size", "il2cpp_field_get_name", "il2cpp_field_get_type",
        "il2cpp_field_get_offset", "il2cpp_field_get_flags", "il2cpp_method_get_name",
        "il2cpp_method_get_return_type", "il2cpp_method_get_param_count",
        "il2cpp_method_get_param", "il2cpp_method_get_param_name", "il2cpp_method_get_flags",
        "il2cpp_property_get_name", "il2cpp_property_get_get_method",
        "il2cpp_property_get_set_method", "il2cpp_type_get_name", "il2cpp_type_get_type",
        "il2cpp_type_get_class_or_element_class", "il2cpp_thread_attach",
        "il2cpp_thread_current",
    };
}

void TestModuleNames() {
    using zircon::il2cpp::LooksLikeIl2CppModuleName;
    CHECK(LooksLikeIl2CppModuleName("GameAssembly.dll"));
    CHECK(LooksLikeIl2CppModuleName("gameassembly.dll"));   // case is not a signal
    CHECK(LooksLikeIl2CppModuleName("libil2cpp.so"));
    CHECK(LooksLikeIl2CppModuleName("UnityFramework"));
    CHECK(!LooksLikeIl2CppModuleName("UnityPlayer.dll"));   // Unity, but not the runtime
    CHECK(!LooksLikeIl2CppModuleName("mono-2.0-bdwgc.dll"));// Mono backend, not IL2CPP
}

void TestExportReading() {
    const auto base = static_cast<Address>(0x180000000ull);
    FakeModule memory(base, BuildPe({"il2cpp_domain_get", "zzz_other"}), "GameAssembly.dll");

    const auto exports = zircon::il2cpp::ReadExports(memory, base);
    CHECK(exports.size() == 2);
    if (exports.size() == 2) {
        CHECK(exports[0].name == "il2cpp_domain_get");
        CHECK(exports[0].rva == 0x1000);
        CHECK(exports[1].name == "zzz_other");
        CHECK(exports[1].rva == 0x1010);
    }

    // A module that is not a PE at all must come back empty rather than inventing entries.
    FakeModule junk(base, std::vector<std::uint8_t>(0x2000, 0xCC), "random.dll");
    CHECK(zircon::il2cpp::ReadExports(junk, base).empty());
}

void TestFindRuntime() {
    using zircon::il2cpp::FindRuntime;
    const auto base = static_cast<Address>(0x180000000ull);

    {
        FakeModule memory(base, BuildPe(AllApiNames()), "GameAssembly.dll");
        const auto found = FindRuntime(memory);
        CHECK(found.has_value());
        if (found) {
            CHECK(found->Valid());
            CHECK(found->api.Complete());
            CHECK(found->api.missing.empty());
            CHECK(found->module_name == "GameAssembly.dll");
            CHECK(found->il2cpp_exports == static_cast<int>(AllApiNames().size()));
            // Addresses are the module base plus the export RVA, not the RVA on its own.
            CHECK(Raw(found->api.domain_get) == Raw(base) + 0x1000);
            CHECK(found->confidence > 0.9f);
        }
    }

    // The name is a shortcut, not the test. A build that renamed its runtime still qualifies,
    // which is the case a name-based check would miss entirely.
    {
        FakeModule memory(base, BuildPe(AllApiNames()), "Anticheat_x64.dll");
        const auto found = FindRuntime(memory);
        CHECK(found.has_value());
        if (found) CHECK(found->Valid());
    }

    // A stripped build reports what is missing instead of pretending to be usable.
    {
        auto names = AllApiNames();
        names.erase(names.begin() + 23);         // il2cpp_field_get_offset
        FakeModule memory(base, BuildPe(names), "GameAssembly.dll");

        const auto found = FindRuntime(memory);
        CHECK(found.has_value());
        if (found) {
            CHECK(!found->Valid());
            CHECK(!found->api.Complete());
            CHECK(found->api.missing.size() == 1);
            if (found->api.missing.size() == 1)
                CHECK(found->api.missing[0] == "il2cpp_field_get_offset");
            CHECK(found->confidence < 0.6f);
        }
    }

    // A game with no IL2CPP runtime is not an error, it is a different kind of game.
    {
        FakeModule memory(base, BuildPe({"CreateFileW", "ReadFile"}), "kernel32.dll");
        CHECK(!FindRuntime(memory).has_value());
    }
}

// ---------------------------------------------------------------------------------
// Placing the method body inside MethodInfo
// ---------------------------------------------------------------------------------

using zircon::core::IsNull;
using zircon::il2cpp::MethodProbe;

// A writable span of target memory, so a MethodInfo array can be laid out byte by byte and
// then read back through exactly the interface the derivation uses.
class FakeProcess final : public IMemorySource {
public:
    FakeProcess(Address base, std::size_t size) : base_(base), bytes_(size, 0) {
        modules_.push_back(ModuleInfo{"GameAssembly.dll", "", base_, size});
        regions_.push_back(RegionInfo{base_, size,
                                      RegionProtect::Read | RegionProtect::Execute, true});
    }

    std::size_t Read(Address addr, void* out, std::size_t size) override {
        if (Raw(addr) < Raw(base_)) return 0;
        const std::uint64_t offset = Raw(addr) - Raw(base_);
        if (offset >= bytes_.size()) return 0;
        const std::size_t available = std::min<std::size_t>(size, bytes_.size() - offset);
        std::memcpy(out, bytes_.data() + offset, available);
        return available;
    }

    void Put(std::uint32_t rva, std::uint64_t value) {
        std::memcpy(bytes_.data() + rva, &value, sizeof(value));
    }
    void Put32(std::uint32_t rva, std::uint32_t value) {
        std::memcpy(bytes_.data() + rva, &value, sizeof(value));
    }
    Address At(std::uint32_t rva) const { return base_ + rva; }

    std::span<const ModuleInfo> Modules() const override { return modules_; }
    std::span<const RegionInfo> Regions() const override { return regions_; }
    Capabilities Caps() const override { return Capabilities{true, false, false, true}; }
    std::string  Describe() const override { return "fake il2cpp process"; }

private:
    Address                   base_;
    std::vector<std::uint8_t> bytes_;
    std::vector<ModuleInfo>   modules_;
    std::vector<RegionInfo>   regions_;
};

// A synthetic run of MethodInfo blocks with the wrong answer present and attractive: the
// invoker is a code pointer right next to the body, and it's what a weaker test picks. Where
// the body sits is a parameter -- a derivation that assumes offset 0 passes every real build
// today and breaks the first time Unity puts something in front of it.
constexpr std::uint32_t kBodiesRva   = 0x001000;
constexpr std::uint32_t kInvokersRva = 0x010000;
constexpr std::uint32_t kMethodsRva  = 0x020000;
constexpr std::uint32_t kNamesRva    = 0x040000;
constexpr std::uint32_t kClassesRva  = 0x060000;
constexpr std::uint32_t kReturnsRva  = 0x070000;
constexpr std::uint32_t kMethodStride = 0x80;

constexpr Address       kModuleBase  = static_cast<Address>(0x180000000ull);
constexpr std::uint64_t kModuleSize  = 0x200000;

constexpr std::size_t kMethodCount = 128;
constexpr std::size_t kClassCount  = 8;
constexpr std::size_t kShapeCount  = 6;

struct MethodWorld {
    FakeProcess                memory{kModuleBase, kModuleSize};
    std::vector<MethodProbe>   probes;
    std::vector<std::uint32_t> starts;
    std::int32_t               expected_name{0};
    std::int32_t               expected_body{0};
    std::int32_t               expected_invoker{0};
};

// leading_data_slots puts non-code pointers ahead of the body, which is what a future Unity
// that prepends a field would look like. code_slots is 2 on builds before 2021.2 and 3 after,
// when a second body pointer appeared for value-type adjustor thunks. constant_body makes
// every method share one body, which is what a slot that is not a body at all looks like.
std::unique_ptr<MethodWorld> BuildMethodWorld(int leading_data_slots = 0, int code_slots = 2,
                                              bool constant_body = false) {
    auto world = std::make_unique<MethodWorld>();
    auto& memory = world->memory;

    const int body_slot    = leading_data_slots;
    const int invoker_slot = leading_data_slots + code_slots - 1;
    const int name_slot    = leading_data_slots + code_slots;

    world->expected_body    = body_slot * 8;
    world->expected_invoker = invoker_slot * 8;
    world->expected_name    = name_slot * 8;

    for (std::size_t i = 0; i < kMethodCount; ++i) {
        const auto method = static_cast<std::uint32_t>(kMethodsRva + i * kMethodStride);
        const auto shape  = i % kShapeCount;
        const auto klass  = i % kClassCount;

        const auto body_rva = constant_body
                                  ? kBodiesRva
                                  : static_cast<std::uint32_t>(kBodiesRva + i * 0x20);
        const auto invoker_rva = static_cast<std::uint32_t>(kInvokersRva + shape * 0x20);
        const auto name_rva    = static_cast<std::uint32_t>(kNamesRva + i * 0x20);
        const auto klass_rva   = static_cast<std::uint32_t>(kClassesRva + klass * 0x40);
        const auto return_rva  = static_cast<std::uint32_t>(kReturnsRva + shape * 0x20);

        // Ahead of the body: a pointer that is real, unique per method, and not code. A test
        // that only asks whether a slot holds a pointer into the module accepts it.
        for (int lead = 0; lead < leading_data_slots; ++lead)
            memory.Put(method + lead * 8, Raw(memory.At(name_rva + 8)));

        for (int code = 0; code < code_slots - 1; ++code)
            memory.Put(method + (body_slot + code) * 8, Raw(memory.At(body_rva)));
        memory.Put(method + invoker_slot * 8, Raw(memory.At(invoker_rva)));

        memory.Put(method + name_slot * 8,       Raw(memory.At(name_rva)));
        memory.Put(method + (name_slot + 1) * 8, Raw(memory.At(klass_rva)));
        memory.Put(method + (name_slot + 2) * 8, Raw(memory.At(return_rva)));
        memory.Put32(method + (name_slot + 3) * 8,
                     0x06000001u + static_cast<std::uint32_t>(i));

        MethodProbe probe;
        probe.method      = memory.At(method);
        probe.name        = memory.At(name_rva);
        probe.klass       = memory.At(klass_rva);
        probe.return_type = memory.At(return_rva);
        probe.param_count = static_cast<std::uint32_t>(shape / 2);
        probe.is_instance = (shape % 2) == 0;
        probe.token       = 0x06000001u + static_cast<std::uint32_t>(i);
        world->probes.push_back(probe);

        world->starts.push_back(body_rva);
        world->starts.push_back(invoker_rva);
    }

    std::sort(world->starts.begin(), world->starts.end());
    world->starts.erase(std::unique(world->starts.begin(), world->starts.end()),
                        world->starts.end());
    return world;
}

// The module's code section, which is what separates a pointer to code from a pointer to
// anything else in the same module. Covers the bodies and the invokers and stops well short
// of the names, so the data pointers a build might put ahead of the body fall outside it.
const std::vector<RegionInfo>& ExecutableSections() {
    static const std::vector<RegionInfo> sections{
        RegionInfo{kModuleBase + kBodiesRva, 0x030000,
                   RegionProtect::Read | RegionProtect::Execute, true}};
    return sections;
}

zircon::il2cpp::MethodInfoLayout DeriveFrom(MethodWorld& world) {
    return zircon::il2cpp::DeriveMethodInfoLayout(world.memory, world.probes, kModuleBase,
                                                  kModuleSize, world.starts,
                                                  ExecutableSections());
}

void TestMethodLayout() {
    // The shape every build before 2021.2 has: body, invoker, name.
    {
        auto world = BuildMethodWorld();
        const auto layout = DeriveFrom(*world);
        CHECK(layout.Valid());
        CHECK(layout.body        == world->expected_body);
        CHECK(layout.invoker     == world->expected_invoker);
        CHECK(layout.name        == world->expected_name);
        CHECK(layout.klass       == world->expected_name + 8);
        CHECK(layout.return_type == world->expected_name + 16);
        CHECK(layout.token       == world->expected_name + 24);
        CHECK(layout.virtual_body == -1);
        CHECK(layout.refusal.empty());
        CHECK(!layout.evidence.empty());
    }

    // 2021.2+: a second body pointer for value-type adjustor thunks. The lower one is the
    // method's own code; the extra one is reported, not skipped.
    {
        auto world = BuildMethodWorld(0, 3);
        const auto layout = DeriveFrom(*world);
        CHECK(layout.Valid());
        CHECK(layout.body         == 0);
        CHECK(layout.virtual_body == 8);
        CHECK(layout.invoker      == 16);
        CHECK(layout.name         == 24);
    }

    // The one that matters. Put anything ahead of the body and it is no longer at offset
    // zero -- and every case above still passes for something that assumed it was.
    {
        auto world = BuildMethodWorld(1, 2);
        const auto layout = DeriveFrom(*world);
        CHECK(layout.Valid());
        CHECK(layout.body    == 8);
        CHECK(layout.invoker == 16);
        CHECK(layout.name    == 24);
    }

    {
        auto world = BuildMethodWorld(2, 3);
        const auto layout = DeriveFrom(*world);
        CHECK(layout.Valid());
        CHECK(layout.body         == 16);
        CHECK(layout.virtual_body == 24);
        CHECK(layout.invoker      == 32);
    }
}

void TestMethodLayoutRefusals() {
    // Every method sharing one body is what a non-body slot looks like. Nothing varies
    // inside a shape, nothing qualifies, and we want a refusal with a reason -- not the
    // invoker wearing the body's name.
    {
        auto world = BuildMethodWorld(0, 2, /*constant_body=*/true);
        const auto layout = DeriveFrom(*world);
        CHECK(!layout.Valid());
        CHECK(layout.body == -1);
        CHECK(!layout.refusal.empty());
    }

    // Too few methods for "varies within its shape" to be a measurement at all.
    {
        auto world = BuildMethodWorld();
        std::vector<MethodProbe> few(world->probes.begin(), world->probes.begin() + 8);
        const auto layout = zircon::il2cpp::DeriveMethodInfoLayout(
            world->memory, few, kModuleBase, kModuleSize, world->starts,
            ExecutableSections());
        CHECK(!layout.Valid());
        CHECK(layout.refusal.find("sample") != std::string::npos);
    }

    // A sample drawn from one class makes the class pointer constant for a reason that has
    // nothing to do with being the class pointer, so the sample is refused, not used.
    {
        auto world = BuildMethodWorld();
        std::vector<MethodProbe> one_class = world->probes;
        for (auto& probe : one_class) probe.klass = world->probes[0].klass;
        const auto layout = zircon::il2cpp::DeriveMethodInfoLayout(
            world->memory, one_class, kModuleBase, kModuleSize, world->starts,
            ExecutableSections());
        CHECK(!layout.Valid());
        CHECK(layout.refusal.find("classes") != std::string::npos);
    }

    // No exception directory: nothing confirms the code-slot addresses are function starts.
    {
        auto world = BuildMethodWorld(1, 2);
        const auto layout = zircon::il2cpp::DeriveMethodInfoLayout(
            world->memory, world->probes, kModuleBase, kModuleSize, {}, ExecutableSections());
        CHECK(!layout.Valid());
        CHECK(layout.refusal.find("exception directory") != std::string::npos);
    }

    // And with no executable sections there is nothing to tell a pointer to code from a
    // pointer to the metadata sitting beside it.
    {
        auto world = BuildMethodWorld();
        const auto layout = zircon::il2cpp::DeriveMethodInfoLayout(
            world->memory, world->probes, kModuleBase, kModuleSize, world->starts, {});
        CHECK(!layout.Valid());
        CHECK(layout.refusal.find("executable sections") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------------

// A runtime small enough to check by hand, shaped around the three things the walk has to
// get right and that nothing else in the suite covers: a value type whose field offsets are
// reported from the boxed start, an open generic whose offsets mean nothing, and a property
// that exists only as a pair of methods until something puts it back together.
class FakeBridge final : public zircon::il2cpp::IBridge {
public:
    using ClassFacts    = zircon::il2cpp::ClassFacts;
    using FieldFacts    = zircon::il2cpp::FieldFacts;
    using MethodFacts   = zircon::il2cpp::MethodFacts;
    using PropertyFacts = zircon::il2cpp::PropertyFacts;
    using TypeFacts     = zircon::il2cpp::TypeFacts;
    using ElementType   = zircon::il2cpp::ElementType;

    struct Entry {
        ClassFacts  facts;
        std::vector<Address> fields;
        std::vector<Address> methods;
        std::vector<Address> properties;
        std::vector<Address> interfaces;
        Address     enum_base{};
    };

    FakeBridge() { Build(); }

    bool Attach() override { return true; }
    std::int32_t ObjectHeaderSize() const override { return 0x10; }

    Address Domain() override { return static_cast<Address>(0x1); }
    std::vector<Address> Assemblies() override { return {static_cast<Address>(0xA1)}; }
    Address AssemblyImage(Address) override { return static_cast<Address>(0xB1); }
    std::vector<Address> AllClasses() override { return inflated_; }

    std::string ImageName(Address) override { return "Assembly-CSharp.dll"; }
    std::size_t ImageClassCount(Address) override { return order_.size(); }
    Address ImageClass(Address, std::size_t index) override {
        return index < order_.size() ? order_[index] : Address{};
    }

    ClassFacts Class(Address klass) override {
        const auto it = classes_.find(Raw(klass));
        return it == classes_.end() ? ClassFacts{} : it->second.facts;
    }
    std::vector<Address> Fields(Address klass) override { return Of(klass).fields; }
    std::vector<Address> Methods(Address klass) override { return Of(klass).methods; }
    std::vector<Address> Properties(Address klass) override { return Of(klass).properties; }
    std::vector<Address> NestedTypes(Address) override { return {}; }
    std::vector<Address> Interfaces(Address klass) override { return Of(klass).interfaces; }
    Address ClassType(Address klass) override { return types_for_class_[Raw(klass)]; }
    Address EnumBaseType(Address klass) override { return Of(klass).enum_base; }

    FieldFacts    Field(Address f) override    { return fields_[Raw(f)]; }
    MethodFacts   Method(Address m) override   { return methods_[Raw(m)]; }
    PropertyFacts Property(Address p) override { return properties_[Raw(p)]; }
    TypeFacts     Type(Address t) override     { return types_[Raw(t)]; }

    std::string ParamName(Address method, std::uint32_t index) override {
        const auto& names = param_names_[Raw(method)];
        return index < names.size() ? names[index] : std::string{};
    }
    Address ParamType(Address method, std::uint32_t index) override {
        const auto& types = param_types_[Raw(method)];
        return index < types.size() ? types[index] : Address{};
    }

    bool LiteralValue(Address field, void* out, std::size_t size) override {
        const auto it = literals_.find(Raw(field));
        if (it == literals_.end()) return false;   // this build will not answer for that one
        std::memcpy(out, &it->second, size);
        return true;
    }

    Address       ModuleBase() const override { return kModuleBase; }
    std::uint64_t ModuleSize() const override { return kModuleSize; }
    std::vector<std::string> Evidence() const override { return {"synthetic runtime"}; }
    std::vector<std::pair<std::string, std::int32_t>> Derived() const override {
        return {{"Il2CppObject.header", 0x10}, {"MethodInfo.body", 0}};
    }
    std::vector<std::string> EntryPoints() const override {
        return {"il2cpp_domain_get=0x1000"};
    }

private:
    Entry& Of(Address klass) {
        static Entry empty;
        const auto it = classes_.find(Raw(klass));
        return it == classes_.end() ? empty : it->second;
    }

    Address AddType(const std::string& name, ElementType element, Address klass = {}) {
        const auto handle = static_cast<Address>(0x7000 + types_.size() * 8);
        TypeFacts facts;
        facts.name    = name;
        facts.element = element;
        facts.klass   = klass;
        types_[Raw(handle)] = facts;
        return handle;
    }

    Address AddClass(const ClassFacts& facts) {
        const auto handle = static_cast<Address>(0x1000 + classes_.size() * 0x40);
        Entry entry;
        entry.facts = facts;
        if (IsNull(entry.facts.image)) entry.facts.image = static_cast<Address>(0xB1);
        classes_.emplace(Raw(handle), std::move(entry));
        order_.push_back(handle);
        return handle;
    }

    Address AddField(Address owner, const std::string& name, Address type,
                     std::int32_t offset, std::uint32_t flags,
                     std::optional<std::uint64_t> literal = std::nullopt) {
        const auto handle = static_cast<Address>(0x3000 + fields_.size() * 0x20);
        FieldFacts facts;
        facts.name   = name;
        facts.type   = type;
        facts.offset = offset;
        facts.flags  = flags;
        facts.is_literal = (flags & 0x0040) != 0;
        fields_[Raw(handle)] = facts;
        if (literal) literals_[Raw(handle)] = *literal;
        classes_.at(Raw(owner)).fields.push_back(handle);
        return handle;
    }

    Address AddMethod(Address owner, const std::string& name, Address return_type,
                      std::uint32_t flags, std::uint32_t body_rva,
                      const std::vector<std::pair<std::string, Address>>& params = {}) {
        const auto handle = static_cast<Address>(0x5000 + methods_.size() * 0x20);
        MethodFacts facts;
        facts.name        = name;
        facts.return_type = return_type;
        facts.param_count = static_cast<std::uint32_t>(params.size());
        facts.flags       = flags;
        facts.token       = 0x06000001u + static_cast<std::uint32_t>(methods_.size());
        facts.is_instance = (flags & 0x0010) == 0;
        facts.body        = kModuleBase + body_rva;
        methods_[Raw(handle)] = facts;

        for (const auto& [param_name, param_type] : params) {
            param_names_[Raw(handle)].push_back(param_name);
            param_types_[Raw(handle)].push_back(param_type);
        }
        classes_.at(Raw(owner)).methods.push_back(handle);
        return handle;
    }

    void Build() {
        const Address t_void   = AddType("System.Void",   ElementType::Void);
        const Address t_int    = AddType("System.Int32",  ElementType::I4);
        const Address t_string = AddType("System.String", ElementType::String);

        // A reference type. Field offsets are measured from the object start and stay there.
        ClassFacts player;
        player.name          = "Player";
        player.name_space    = "Game";
        player.instance_size = 0x30;
        const Address player_handle = AddClass(player);
        const Address t_player = AddType("Game.Player", ElementType::Class, player_handle);
        types_for_class_[Raw(player_handle)] = t_player;

        AddField(player_handle, "health", t_int,    0x18, 0x0006);
        AddField(player_handle, "name",   t_string, 0x20, 0x0001);
        AddField(player_handle, "Count",  t_int,    0x00, 0x0016);   // static

        const Address getter = AddMethod(player_handle, "get_Health", t_int, 0x0886, 0x1000);
        AddMethod(player_handle, "set_Health", t_void, 0x0886, 0x1020,
                  {{"value", t_int}});
        AddMethod(player_handle, "Heal", t_void, 0x0006, 0x1040, {{"amount", t_int}});
        // a ref parameter. byref is a flag on the type, not its own element kind
        const Address t_int_ref = AddType("System.Int32&", ElementType::I4);
        types_[Raw(t_int_ref)].by_ref = true;
        AddMethod(player_handle, "TryHeal", t_void, 0x0006, 0x10a0, {{"amount", t_int_ref}});
        // Two methods the linker folded onto one body, which the walk must notice and flag
        // rather than report as two distinct addresses that happen to match.
        AddMethod(player_handle, "Reset",  t_void, 0x0006, 0x1080);
        AddMethod(player_handle, "Revive", t_void, 0x0006, 0x1080);

        const auto property = static_cast<Address>(0x9000);
        PropertyFacts health;
        health.name   = "Health";
        health.getter = getter;
        health.setter = classes_.at(Raw(player_handle)).methods[1];
        properties_[Raw(property)] = health;
        classes_.at(Raw(player_handle)).properties.push_back(property);

        // A value type. The runtime reports its field offsets from the boxed start, header
        // included, so 0x10 and 0x14 are really 0 and 4 once it is unboxed.
        // Every value type derives from System.ValueType, whose own instance size is the boxed
        // header. Carrying that over as an inherited region is what said an eight-byte struct
        // starts sixteen bytes into itself.
        ClassFacts value_type;
        value_type.name          = "ValueType";
        value_type.name_space    = "System";
        value_type.instance_size = 0x10;
        const Address value_type_handle = AddClass(value_type);

        ClassFacts vec;
        vec.name          = "Vec2";
        vec.name_space    = "Game";
        vec.is_valuetype  = true;
        vec.instance_size = 0x18;
        vec.value_size    = 8;
        vec.parent        = value_type_handle;
        const Address vec_handle = AddClass(vec);
        types_for_class_[Raw(vec_handle)] = AddType("Game.Vec2", ElementType::ValueType,
                                                    vec_handle);
        AddField(vec_handle, "x", t_int, 0x10, 0x0006);
        AddField(vec_handle, "y", t_int, 0x14, 0x0006);
        // A static on a value type. Its offset is measured into the type's own block, so
        // subtracting an object header from it produces a number that looks like a field
        // offset and is not one.
        AddField(vec_handle, "zero", types_for_class_[Raw(vec_handle)], 0x20, 0x0016);

        // An enum, on a build that will not hand over const values.
        ClassFacts mode;
        mode.name         = "Mode";
        mode.name_space   = "Game";
        mode.is_valuetype = true;
        mode.is_enum      = true;
        mode.value_size   = 4;
        const Address mode_handle = AddClass(mode);
        classes_.at(Raw(mode_handle)).enum_base = t_int;
        types_for_class_[Raw(mode_handle)] =
            AddType("Game.Mode", ElementType::ValueType, mode_handle);

        // An array of that enum. An array type reports its *element* class, so this is the
        // one place a type reference is built from a class rather than from a type, and it is
        // where the two ways of doing it drifted apart.
        const Address t_modes = AddType("Game.Mode[]", ElementType::SzArray, mode_handle);
        AddField(player_handle, "modes", t_modes, 0x28, 0x0006);
        AddField(mode_handle, "value__", t_int, 0x10, 0x0006);
        AddField(mode_handle, "Idle",    t_int, 0x00, 0x8056);
        AddField(mode_handle, "Running", t_int, 0x00, 0x8056);

        // A union. C# writes one with [StructLayout(LayoutKind.Explicit)], which sets the
        // layout bits below, and its members are meant to sit on top of each other.
        ClassFacts overlapped;
        overlapped.name          = "Union";
        overlapped.name_space    = "Game";
        overlapped.is_valuetype  = true;
        overlapped.flags         = 0x0010;   // TypeAttributes.ExplicitLayout
        overlapped.instance_size = 0x18;
        overlapped.value_size    = 8;
        const Address union_handle = AddClass(overlapped);
        types_for_class_[Raw(union_handle)] =
            AddType("Game.Union", ElementType::ValueType, union_handle);
        AddField(union_handle, "asInt",   t_int, 0x10, 0x0006);
        AddField(union_handle, "alsoInt", t_int, 0x10, 0x0006);

        // A thread-static. The runtime has no single offset to give for one and says so with
        // a negative number, which is not an offset and must not be recorded as one.
        AddField(player_handle, "perThread", t_int, -1, 0x0016);

        // A second enum, on the half of the fixture whose consts can be read. Its negative
        // member arrives as four bytes that are all ones, and it is the declared type -- not
        // the bytes -- that says whether that is minus one or four billion.
        ClassFacts direction;
        direction.name         = "Direction";
        direction.name_space   = "Game";
        direction.is_valuetype = true;
        direction.is_enum      = true;
        direction.value_size   = 4;
        const Address direction_handle = AddClass(direction);
        classes_.at(Raw(direction_handle)).enum_base = t_int;
        AddField(direction_handle, "value__",  t_int, 0x10, 0x0006);
        AddField(direction_handle, "Forward",  t_int, 0x00, 0x8056, std::uint64_t{1});
        AddField(direction_handle, "Backward", t_int, 0x00, 0x8056, std::uint64_t{0xFFFFFFFFu});

        // An open generic definition. Asking the runtime for its field offsets returns a
        // number, and the number is meaningless until the type is given an argument.
        ClassFacts box;
        box.name          = "Box`1";
        box.name_space    = "Game";
        box.is_generic    = true;
        box.instance_size = 0x18;
        const Address box_handle = AddClass(box);
        AddField(box_handle, "item", t_int, 0x10, 0x0006);

        // And one instantiation of it, which is where a real offset for `item` exists. The
        // runtime answers il2cpp_class_get_name with "Box`1" for this as well as for the
        // definition, so the two only stay apart if the path comes from the type name.
        ClassFacts boxed;
        boxed.name          = "Box`1";
        boxed.name_space    = "Game";
        boxed.is_inflated   = true;
        boxed.instance_size = 0x18;
        const Address boxed_handle = AddClass(boxed);
        order_.pop_back();   // instantiations live in the class cache, not the image
        types_for_class_[Raw(boxed_handle)] =
            AddType("Game.Box`1<System.Int32>", ElementType::GenericInst, boxed_handle);
        AddField(boxed_handle, "item", t_int, 0x10, 0x0006);
        inflated_.push_back(boxed_handle);
    }

    std::map<std::uint64_t, Entry>         classes_;
    std::map<std::uint64_t, FieldFacts>    fields_;
    std::map<std::uint64_t, MethodFacts>   methods_;
    std::map<std::uint64_t, PropertyFacts> properties_;
    std::map<std::uint64_t, TypeFacts>     types_;
    std::map<std::uint64_t, Address>       types_for_class_;
    std::map<std::uint64_t, std::uint64_t>  literals_;
    std::map<std::uint64_t, std::vector<std::string>> param_names_;
    std::map<std::uint64_t, std::vector<Address>>     param_types_;
    std::vector<Address>                   order_;
    std::vector<Address>                   inflated_;
};

const zircon::ir::Struct* FindRecord(const std::vector<zircon::ir::Struct>& records,
                                     std::string_view path) {
    for (const auto& record : records)
        if (record.path == path) return &record;
    return nullptr;
}

const zircon::ir::Property* FindField(const zircon::ir::Struct& record, std::string_view name) {
    for (const auto& property : record.properties)
        if (property.name == name) return &property;
    return nullptr;
}

void TestWalk() {
    FakeBridge bridge;
    zircon::il2cpp::WalkOptions options;
    zircon::il2cpp::WalkStats  stats;
    const auto dump = zircon::il2cpp::Walk(bridge, options, stats);

    // The field publishing gates on. Without it an IL2CPP dump is just a dump.
    CHECK(dump.header.runtime == "il2cpp");

    // A dump with no timestamp sorts nowhere and displays as a dash wherever it lands.
    CHECK(dump.header.created_utc.size() == 20);
    CHECK(dump.header.created_utc.back() == 'Z');

    // What was derived travels with the dump, the same way the Unreal side records what it
    // worked out about UObject.
    CHECK(dump.header.offsets.size() == 2);
    CHECK(!dump.header.globals.empty());

    // A path is an identity in this format, and a C# name is only unique inside its assembly:
    // every one of eighty assemblies in a real game declares its own <Module>.
    CHECK(!FindRecord(dump.packages.front().classes, "Game.Player"));
    CHECK(dump.packages.size() == 1);
    if (dump.packages.empty()) return;

    const auto& package = dump.packages.front();
    CHECK(package.name == "Assembly-CSharp.dll");
    CHECK(package.classes.size() == 4);   // Player, System.ValueType, open Box`1, instantiation
    CHECK(package.structs.size() == 2);   // Vec2 and the union; enums go to enums
    CHECK(package.enums.size()   == 2);

    const auto* player = FindRecord(package.classes, "Game.Player, Assembly-CSharp");
    CHECK(player != nullptr);
    if (player) {
        CHECK(player->name_space == "Game");
        CHECK(player->is_class);
        CHECK(!player->is_valuetype);
        CHECK(player->size == 0x30);

        const auto* health = FindField(*player, "health");
        CHECK(health != nullptr);
        if (health) {
            CHECK(health->offset == 0x18);
            // A reference type's offset is already measured the same way its size is, so
            // there is no second number and the field says so rather than repeating itself.
            CHECK(health->boxed_offset == -1);
            CHECK(!health->is_static);
            CHECK(health->type.kind == zircon::ir::TypeKind::Int32);
            CHECK(health->type.raw  == "System.Int32");
        }

        // An array of enums names an enum. It used to name one and call it a struct, which
        // sent anything looking the name up into the wrong half of the dump.
        const auto* modes = FindField(*player, "modes");
        CHECK(modes != nullptr);
        if (modes) {
            CHECK(modes->type.kind == zircon::ir::TypeKind::Array);
            CHECK(modes->type.params.size() == 1);
            if (modes->type.params.size() == 1) {
                CHECK(modes->type.params[0].kind == zircon::ir::TypeKind::Enum);
                CHECK(modes->type.params[0].name == "Game.Mode, Assembly-CSharp");
            }
        }

        const auto* count = FindField(*player, "Count");
        CHECK(count != nullptr);
        if (count) CHECK(count->is_static);

        // A negative offset is the runtime saying there is no one offset, not an offset.
        const auto* per_thread = FindField(*player, "perThread");
        CHECK(per_thread != nullptr);
        if (per_thread) {
            CHECK(per_thread->offset_unresolved);
            CHECK(per_thread->offset == 0);
        }

        // The property came back as a property, not as two methods named get_ and set_.
        CHECK(player->accessors.size() == 1);
        if (player->accessors.size() == 1) {
            CHECK(player->accessors[0].name   == "Health");
            CHECK(player->accessors[0].getter == "get_Health");
            CHECK(player->accessors[0].setter == "set_Health");
            CHECK(player->accessors[0].type.kind == zircon::ir::TypeKind::Int32);
        }

        // Bodies are module-relative, and the two that share one are flagged as sharing it.
        int shared = 0, with_body = 0;
        for (const auto& function : player->functions) {
            if (function.native_rva != 0) ++with_body;
            if (function.shared_body) ++shared;
            if (function.name == "TryHeal") {
                CHECK(function.params.size() == 2);
                if (function.params.size() == 2) CHECK(function.params[0].is_out);
            }
            if (function.name == "Heal") {
                CHECK(function.native_rva == 0x1040);
                CHECK(!function.shared_body);
                CHECK(function.params.size() == 2);   // the argument and the return value
                if (function.params.size() == 2) {
                    CHECK(function.params[0].name == "amount");
                    CHECK(function.params[1].is_return);
                }
            }
        }
        CHECK(with_body == 6);
        CHECK(shared == 2);
    }

    // The value-type trap. Why the IR grew a second offset column.
    const auto* vec = FindRecord(package.structs, "Game.Vec2, Assembly-CSharp");
    CHECK(vec != nullptr);
    if (vec) {
        CHECK(vec->is_valuetype);
        CHECK(vec->size == 8);            // unboxed, which is what an SDK declares
        const auto* x = FindField(*vec, "x");
        const auto* y = FindField(*vec, "y");
        CHECK(x != nullptr && y != nullptr);
        if (x && y) {
            // Measured the same way the type's own size is, so x, y and size agree: two
            // four-byte fields in an eight-byte struct.
            CHECK(x->offset == 0);
            CHECK(y->offset == 4);
            CHECK(x->offset + x->size <= vec->size);
            CHECK(y->offset + y->size <= vec->size);
            // And what the runtime actually said, kept rather than discarded.
            CHECK(x->boxed_offset == 0x10);
            CHECK(y->boxed_offset == 0x14);
        }

        // A static is not measured from an object at all, so it gets no second number.
        const auto* zero = FindField(*vec, "zero");
        CHECK(zero != nullptr);
        if (zero) {
            CHECK(zero->is_static);
            CHECK(zero->offset == 0x20);
            CHECK(zero->boxed_offset == -1);
        }
    }

    // An open generic keeps its members and refuses to put a number on them.
    const auto* box = FindRecord(package.classes, "Game.Box`1, Assembly-CSharp");
    CHECK(box != nullptr);
    if (box) {
        CHECK(box->is_generic);
        const auto* item = FindField(*box, "item");
        CHECK(item != nullptr);
        if (item) {
            CHECK(item->offset_unresolved);
            CHECK(item->offset == 0);
        }
    }

    // Names without values, said out loud. Numbering them by position would have looked
    // right for this enum and been wrong for any that assigns its own values.
    // A value type inherits nothing. Its parent is System.ValueType, whose own size is the
    // boxed header, and carrying that over as an inherited region says a two-field struct
    // begins sixteen bytes into itself.
    if (vec) CHECK(vec->inherited_size == 0);

    const zircon::ir::Enum* mode = nullptr;
    const zircon::ir::Enum* direction = nullptr;
    for (const auto& e : package.enums) {
        if (e.path == "Game.Mode, Assembly-CSharp")      mode = &e;
        if (e.path == "Game.Direction, Assembly-CSharp") direction = &e;
    }
    CHECK(mode != nullptr);
    CHECK(direction != nullptr);

    if (direction) {
        // Said in the vocabulary the IR uses for this field, not in C#'s. The linter reads
        // it to decide whether a member fits, and "System.Int32" read as a byte.
        CHECK(direction->underlying == "int32");
        CHECK(direction->values_resolved);
        CHECK(direction->values.size() == 2);
        if (direction->values.size() == 2) {
            CHECK(direction->values[0].value == 1);
            // The one that matters: read as bytes this is 4294967295.
            CHECK(direction->values[1].value == -1);
        }
    }
    if (mode) CHECK(mode->path == "Game.Mode, Assembly-CSharp");
    if (mode) {
        CHECK(!mode->values_resolved);
        CHECK(mode->values.size() == 2);
    }
    CHECK(stats.enums_without_values == 1);

    // The instantiation is its own record under its own path. Naming it after the class
    // instead of the type put every generic in a real game on one path: 52,255 records on
    // 14,522 paths.
    const auto* boxed = FindRecord(package.classes, "Game.Box`1<System.Int32>, Assembly-CSharp");
    CHECK(boxed != nullptr);
    if (boxed) {
        const auto* item = FindField(*boxed, "item");
        CHECK(item != nullptr);
        if (item) {
            CHECK(!item->offset_unresolved);   // an instantiation has a real layout
            CHECK(item->offset == 0x10);
        }
    }
    CHECK(stats.inflated == 1);

    const auto* overlapped = FindRecord(package.structs, "Game.Union, Assembly-CSharp");
    CHECK(overlapped != nullptr);
    if (overlapped) {
        CHECK(overlapped->explicit_layout);
        CHECK(overlapped->properties.size() == 2);
        // Two members at one offset. That is the type, not a defect.
        if (overlapped->properties.size() == 2)
            CHECK(overlapped->properties[0].offset == overlapped->properties[1].offset);
    }

    CHECK(stats.classes == 6);
    CHECK(stats.enums   == 2);
    CHECK(stats.shared_bodies == 2);
    CHECK(stats.open_generics == 1);
}

} // namespace

int main() {
    TestModuleNames();
    TestExportReading();
    TestFindRuntime();
    TestMethodLayout();
    TestMethodLayoutRefusals();
    TestWalk();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
