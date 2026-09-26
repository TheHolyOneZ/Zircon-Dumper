#include "mono/Walker.h"

#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::mono {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

namespace field_attr {
constexpr std::uint32_t kAccessMask    = 0x0007;
constexpr std::uint32_t kStatic        = 0x0010;
constexpr std::uint32_t kInitOnly      = 0x0020;
constexpr std::uint32_t kLiteral       = 0x0040;
constexpr std::uint32_t kNotSerialized = 0x0080;
constexpr std::uint32_t kSpecialName   = 0x0200;
}

namespace type_attr {
constexpr std::uint32_t kLayoutMask     = 0x0018;
constexpr std::uint32_t kExplicitLayout = 0x0010;
constexpr std::uint32_t kInterface      = 0x0020;
constexpr std::uint32_t kAbstract       = 0x0080;
constexpr std::uint32_t kSealed         = 0x0100;
}

namespace method_attr {
constexpr std::uint32_t kAccessMask  = 0x0007;
constexpr std::uint32_t kStatic      = 0x0010;
constexpr std::uint32_t kFinal       = 0x0020;
constexpr std::uint32_t kVirtual     = 0x0040;
constexpr std::uint32_t kNewSlot     = 0x0100;
constexpr std::uint32_t kAbstract    = 0x0400;
constexpr std::uint32_t kSpecialName = 0x0800;
constexpr std::uint32_t kPInvoke     = 0x2000;
}

std::string_view AccessName(std::uint32_t masked) {
    switch (masked) {
        case 0: return "PrivateScope";
        case 1: return "Private";
        case 2: return "FamANDAssem";
        case 3: return "Assembly";
        case 4: return "Family";
        case 5: return "FamORAssem";
        case 6: return "Public";
        default: return "";
    }
}

std::vector<std::string> FieldFlagNames(std::uint32_t flags) {
    std::vector<std::string> names;
    if (const auto access = AccessName(flags & field_attr::kAccessMask); !access.empty())
        names.emplace_back(access);
    if (flags & field_attr::kStatic)        names.emplace_back("Static");
    if (flags & field_attr::kInitOnly)      names.emplace_back("InitOnly");
    if (flags & field_attr::kLiteral)       names.emplace_back("Literal");
    if (flags & field_attr::kNotSerialized) names.emplace_back("NotSerialized");
    if (flags & field_attr::kSpecialName)   names.emplace_back("SpecialName");
    return names;
}

std::vector<std::string> MethodFlagNames(std::uint32_t flags) {
    std::vector<std::string> names;
    if (const auto access = AccessName(flags & method_attr::kAccessMask); !access.empty())
        names.emplace_back(access);
    if (flags & method_attr::kStatic)      names.emplace_back("Static");
    if (flags & method_attr::kFinal)       names.emplace_back("Final");
    if (flags & method_attr::kVirtual)     names.emplace_back("Virtual");
    if (flags & method_attr::kNewSlot)     names.emplace_back("NewSlot");
    if (flags & method_attr::kAbstract)    names.emplace_back("Abstract");
    if (flags & method_attr::kSpecialName) names.emplace_back("SpecialName");
    if (flags & method_attr::kPInvoke)     names.emplace_back("PInvokeImpl");
    return names;
}

struct Mapped {
    ir::TypeKind kind{ir::TypeKind::Unknown};
    std::int32_t size{0};
};

Mapped MapElement(ElementType element) {
    using ir::TypeKind;
    switch (element) {
        case ElementType::Boolean: return {TypeKind::Bool,   1};
        case ElementType::I1:      return {TypeKind::Int8,   1};
        case ElementType::U1:      return {TypeKind::UInt8,  1};
        case ElementType::I2:      return {TypeKind::Int16,  2};
        case ElementType::U2:      return {TypeKind::UInt16, 2};
        case ElementType::Char:    return {TypeKind::UInt16, 2};
        case ElementType::I4:      return {TypeKind::Int32,  4};
        case ElementType::U4:      return {TypeKind::UInt32, 4};
        case ElementType::I8:      return {TypeKind::Int64,  8};
        case ElementType::U8:      return {TypeKind::UInt64, 8};
        case ElementType::R4:      return {TypeKind::Float,  4};
        case ElementType::R8:      return {TypeKind::Double, 8};
        case ElementType::I:       return {TypeKind::Int64,  8};
        case ElementType::U:       return {TypeKind::UInt64, 8};
        case ElementType::String:  return {TypeKind::String, 8};
        case ElementType::Object:
        case ElementType::Class:   return {TypeKind::ObjectPtr, 8};
        case ElementType::SzArray:
        case ElementType::Array:   return {TypeKind::Array, 8};
        default:                   return {TypeKind::Unknown, 0};
    }
}

std::string UnderlyingName(ElementType element) {
    switch (element) {
        case ElementType::I1: return "int8";
        case ElementType::U1: return "uint8";
        case ElementType::I2: return "int16";
        case ElementType::U2: return "uint16";
        case ElementType::I4: return "int32";
        case ElementType::U4: return "uint32";
        case ElementType::I8: return "int64";
        case ElementType::U8: return "uint64";
        default:              return "int32";
    }
}

std::string Utc() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(now));
}

class Walker {
public:
    Walker(IBridge& bridge, const WalkOptions& options, WalkStats& stats)
        : bridge_(bridge), options_(options), stats_(stats),
          tracking_(options.breadcrumb != nullptr || !options.skip.empty()) {}

    ir::Dump Run() {
        ir::Dump dump;
        dump.header.runtime            = "mono";
        dump.header.source.main_module = "mono";
        dump.header.source.module_base = Raw(bridge_.ModuleBase());
        dump.header.source.image_size  = bridge_.ModuleSize();
        dump.header.engine.evidence    = bridge_.Evidence();
        dump.header.created_utc        = Utc();
        dump.header.sources            = {"live"};

        const auto assemblies = bridge_.Assemblies();
        std::uint64_t expected = 0;
        for (const Address assembly : assemblies) {
            const Address image = bridge_.AssemblyImage(assembly);
            if (!IsNull(image)) expected += bridge_.ImageTypeCount(image);
        }
        core::LogInfo("{} assemblies, {} types declared", assemblies.size(), expected);

        for (const Address assembly : assemblies) {
            ++stats_.assemblies;
            const Address image = bridge_.AssemblyImage(assembly);
            if (IsNull(image)) continue;
            ++stats_.images;

            const auto facts = bridge_.Assembly(assembly);
            const std::string package = bridge_.ImageName(image);
            const std::uint32_t count = bridge_.ImageTypeCount(image);

            assembly_of_.emplace(Raw(image), AssemblyName(package));
            if (!facts.version.empty()) version_of_[AssemblyName(package)] = facts.version;

            core::LogDebug("assembly {}/{}: {}, {} types", stats_.assemblies,
                           assemblies.size(), package, count);

            for (std::uint32_t i = 0; i < count; ++i) {
                if (tracking_) {
                    const std::string slot = std::format("{}#{}", package, i);
                    Note(slot);
                    if (Skipped(slot)) continue;
                }
                const Address klass = bridge_.ImageType(image, i);
                if (IsNull(klass)) { ++stats_.unreadable_types; continue; }
                Visit(klass, package);
            }
        }

        Finish(dump);
        return dump;
    }

private:
    static std::string AssemblyName(std::string_view image) {
        std::string name(image);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
            name.resize(name.size() - 4);
        return name;
    }

    void Note(std::string_view key) {
        if (!options_.breadcrumb) return;
        here_.assign(key);
        options_.breadcrumb(here_);
    }

    void Phase(std::string_view what) {
        if (!options_.breadcrumb) return;
        options_.breadcrumb(std::format("{}\n{}", here_, what));
    }

    bool Skipped(const std::string& key) const {
        return std::find(options_.skip.begin(), options_.skip.end(), key) != options_.skip.end();
    }

    std::string NameOf(Address klass, int depth = 0) {
        if (IsNull(klass) || depth > 16) return {};
        if (const auto it = names_.find(Raw(klass)); it != names_.end()) return it->second;

        const auto facts = bridge_.Class(klass);
        std::string name;
        if (!IsNull(facts.declaring) && Raw(facts.declaring) != Raw(klass))
            name = NameOf(facts.declaring, depth + 1) + ".";
        else if (!facts.name_space.empty())
            name = facts.name_space + ".";
        name += facts.name;

        names_.emplace(Raw(klass), name);
        return name;
    }

    std::string PathOf(Address klass) {
        if (IsNull(klass)) return {};
        if (const auto it = paths_.find(Raw(klass)); it != paths_.end()) return it->second;

        std::string path = NameOf(klass);
        if (!path.empty()) {
            const auto facts = bridge_.Class(klass);
            std::string assembly;
            if (!IsNull(facts.image)) {
                const auto it = assembly_of_.find(Raw(facts.image));
                if (it != assembly_of_.end()) assembly = it->second;
                else assembly = AssemblyName(bridge_.ImageName(facts.image));
            }
            if (!assembly.empty()) path += ", " + assembly;
        }

        paths_.emplace(Raw(klass), path);
        return path;
    }

    const ir::TypeRef& FromClass(Address klass) {
        static const ir::TypeRef empty;
        if (IsNull(klass)) return empty;

        const auto key = Raw(klass);
        if (const auto it = class_refs_.find(key); it != class_refs_.end()) return it->second;

        const auto facts = bridge_.Class(klass);
        ir::TypeRef ref;
        ref.raw = NameOf(klass);

        if (facts.is_enum) {
            ref.kind = ir::TypeKind::Enum;
            ref.size = facts.value_size > 0 ? facts.value_size : 4;
        } else if (facts.is_valuetype) {
            ref.kind = ir::TypeKind::Struct;
            ref.size = facts.value_size > 0 ? facts.value_size : 0;
        } else {
            ref.kind = ir::TypeKind::ObjectPtr;
            ref.size = 8;
        }
        ref.name = PathOf(klass);

        return class_refs_.emplace(key, std::move(ref)).first->second;
    }

    ir::TypeRef TypeOf(Address type) {
        ir::TypeRef ref;
        if (IsNull(type)) return ref;

        const auto facts = bridge_.Type(type);
        const auto mapped = MapElement(facts.element);
        ref.kind = mapped.kind;
        ref.size = mapped.size;
        ref.raw  = facts.name;

        switch (facts.element) {
            case ElementType::Class:
            case ElementType::Object:
            case ElementType::GenericInst:
            case ElementType::ValueType: {
                const ir::TypeRef& from = FromClass(facts.klass);
                if (from.kind == ir::TypeKind::Unknown) break;
                ref.kind = from.kind;
                ref.name = from.name;
                ref.size = from.size;
                break;
            }
            case ElementType::SzArray:
            case ElementType::Array: {
                if (IsNull(facts.klass)) break;
                ref.params.push_back(FromClass(facts.klass));
                ref.size = 8;
                break;
            }
            default:
                break;
        }
        return ref;
    }

    void Visit(Address klass, const std::string& package) {
        if (IsNull(klass) || !seen_.insert(Raw(klass)).second) return;

        const std::string path = PathOf(klass);
        if (path.empty()) return;
        if (!options_.filter.empty() && path.find(options_.filter) == std::string::npos) return;

        Note(path);
        if (Skipped(path)) { ++stats_.skipped; return; }
        if (!taken_.insert(path).second) return;

        const auto facts = bridge_.Class(klass);

        if (facts.is_enum) BuildEnum(klass, facts, path, package);
        else               BuildStruct(klass, facts, path, package);

        for (const Address nested : bridge_.NestedTypes(klass)) {
            if (IsNull(nested)) continue;
            ++stats_.nested;
            Visit(nested, package);
        }
    }

    void BuildEnum(Address klass, const ClassFacts& facts, const std::string& path,
                   const std::string& package) {
        ir::Enum record;
        record.name = facts.name;
        record.path = path;

        ElementType element = ElementType::I4;
        Phase("enum values");
        for (const Address field : bridge_.Fields(klass)) {
            const auto info = bridge_.Field(field);
            if (!(info.flags & field_attr::kLiteral)) {
                if (info.name == "value__" && !IsNull(info.type))
                    element = bridge_.Type(info.type).element;
                continue;
            }
            ir::EnumValue entry;
            entry.name = info.name;
            record.values.push_back(std::move(entry));
        }
        record.underlying = UnderlyingName(element);

        record.values_resolved = false;
        ++stats_.enums_without_values;
        ++stats_.enums;
        PackageFor(package).enums.push_back(std::move(record));
    }

    void BuildStruct(Address klass, const ClassFacts& facts, const std::string& path,
                     const std::string& package) {
        ir::Struct record;
        record.name         = facts.name;
        record.path         = path;
        record.name_space   = facts.name_space;
        record.super        = PathOf(facts.parent);
        record.is_class     = !facts.is_valuetype;
        record.is_valuetype = facts.is_valuetype;
        record.is_interface = (facts.flags & type_attr::kInterface) != 0;
        record.is_abstract  = (facts.flags & type_attr::kAbstract) != 0;
        record.is_generic   = facts.name.find('`') != std::string::npos;
        record.token        = facts.token;
        record.alignment    = facts.alignment;
        record.explicit_layout =
            (facts.flags & type_attr::kLayoutMask) == type_attr::kExplicitLayout;

        record.size = facts.is_valuetype && facts.value_size > 0 ? facts.value_size
                                                                 : facts.instance_size;

        if (!IsNull(facts.parent) && !facts.is_valuetype) {
            const auto parent_size = bridge_.Class(facts.parent).instance_size;
            if (parent_size > 0 && record.size > 0 && parent_size > record.size)
                ++stats_.contradictory_bases;
            else if (parent_size > 0)
                record.inherited_size = parent_size;
        }

        Phase("interfaces");
        for (const Address iface : bridge_.Interfaces(klass))
            record.interfaces.push_back(PathOf(iface));

        Phase("fields");
        for (const Address field : bridge_.Fields(klass)) {
            const auto info = bridge_.Field(field);

            ir::Property property;
            property.name       = info.name;
            property.type       = TypeOf(info.type);
            property.flags      = info.flags;
            property.flag_names = FieldFlagNames(info.flags);
            property.is_static  = (info.flags & field_attr::kStatic) != 0;
            property.offset     = info.offset;
            property.size       = property.type.size;

            const bool is_literal = (info.flags & field_attr::kLiteral) != 0;
            if (record.is_generic || is_literal || info.offset < 0) {
                property.offset            = 0;
                property.offset_unresolved = true;
                ++stats_.unresolved_offsets;
            } else if (facts.is_valuetype && !property.is_static &&
                       info.offset >= kObjectHeader) {
                property.offset       = info.offset - kObjectHeader;
                property.boxed_offset = info.offset;
            }

            ++stats_.fields;
            record.properties.push_back(std::move(property));
        }

        Phase("properties");
        for (const Address property : bridge_.Properties(klass)) {
            const auto info = bridge_.Property(property);
            if (info.name.empty()) continue;

            ir::Accessor accessor;
            accessor.name  = info.name;
            accessor.flags = info.flags;

            if (!IsNull(info.getter)) {
                const auto getter = bridge_.Method(info.getter);
                accessor.getter = getter.name;
                accessor.type   = TypeOf(getter.return_type);
            }
            if (!IsNull(info.setter)) {
                const auto setter = bridge_.Method(info.setter);
                accessor.setter = setter.name;
                if (accessor.type.raw.empty()) {
                    const auto params = bridge_.MethodParams(info.setter);
                    if (!params.empty()) accessor.type = TypeOf(params.front());
                }
            }

            ++stats_.accessors;
            record.accessors.push_back(std::move(accessor));
        }

        const auto methods = bridge_.Methods(klass);
        std::size_t method_index = 0;
        for (const Address method : methods) {
            Phase(std::format("method {} of {}", method_index++, methods.size()));
            const auto info = bridge_.Method(method);

            ir::Function function;
            function.name       = info.name;
            function.flags      = info.flags;
            function.flag_names = MethodFlagNames(info.flags);
            function.token      = info.token;

            ++stats_.methods;
            record.functions.push_back(std::move(function));
        }

        if (record.is_generic) ++stats_.open_generics;
        ++stats_.classes;

        if (facts.is_valuetype) PackageFor(package).structs.push_back(std::move(record));
        else                    PackageFor(package).classes.push_back(std::move(record));
    }

    ir::Package& PackageFor(const std::string& name) {
        const auto it = package_index_.find(name);
        if (it != package_index_.end()) return packages_[it->second];

        package_index_.emplace(name, packages_.size());
        ir::Package package;
        package.name = name;
        packages_.push_back(std::move(package));
        return packages_.back();
    }

    void Finish(ir::Dump& dump) {
        dump.packages = std::move(packages_);
        std::sort(dump.packages.begin(), dump.packages.end(),
                  [](const ir::Package& a, const ir::Package& b) { return a.name < b.name; });

        for (auto& package : dump.packages) {
            const auto it = version_of_.find(AssemblyName(package.name));
            if (it != version_of_.end()) package.assembly_version = it->second;

            const auto by_path = [](const auto& a, const auto& b) { return a.path < b.path; };
            std::sort(package.classes.begin(), package.classes.end(), by_path);
            std::sort(package.structs.begin(), package.structs.end(), by_path);
            std::sort(package.enums.begin(), package.enums.end(), by_path);
        }

        dump.header.engine.evidence.emplace_back(
            "enum values are absent from a live Mono walk: a const has no storage and the "
            "embedding API has no call that reads one, so they come from the assembly "
            "metadata instead -- run --mode dual for them");
    }

    static constexpr std::int32_t kObjectHeader = 16;

    std::string        here_;
    IBridge&           bridge_;
    const WalkOptions& options_;
    WalkStats&         stats_;
    bool               tracking_{false};

    std::vector<ir::Package> packages_;
    std::unordered_map<std::string, std::size_t> package_index_;
    std::unordered_map<std::uint64_t, std::string> names_;
    std::unordered_map<std::uint64_t, std::string> paths_;
    std::unordered_map<std::uint64_t, std::string> assembly_of_;
    std::unordered_map<std::string, std::string>   version_of_;
    std::unordered_map<std::uint64_t, ir::TypeRef> class_refs_;
    std::unordered_set<std::uint64_t> seen_;
    std::unordered_set<std::string>   taken_;
};

} // namespace

ir::Dump Walk(IBridge& bridge, const WalkOptions& options, WalkStats& stats) {
    Walker walker(bridge, options, stats);
    return walker.Run();
}

} // namespace zircon::mono
