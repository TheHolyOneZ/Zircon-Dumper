#include "il2cpp/Walker.h"

#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace zircon::il2cpp {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// ECMA-335 II.23.1.5 / II.23.1.10. CLI standard, not Unity's, so they don't move.
namespace field_attr {
constexpr std::uint32_t kAccessMask    = 0x0007;
constexpr std::uint32_t kStatic        = 0x0010;
constexpr std::uint32_t kInitOnly      = 0x0020;
constexpr std::uint32_t kLiteral       = 0x0040;
constexpr std::uint32_t kNotSerialized = 0x0080;
constexpr std::uint32_t kSpecialName   = 0x0200;
}

// ECMA-335 II.23.1.15. A type whose layout bits say Explicit placed its own fields.
namespace type_attr {
constexpr std::uint32_t kLayoutMask     = 0x0018;
constexpr std::uint32_t kExplicitLayout = 0x0010;
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

// Element type -> IR kind. Anything the IR has no kind for keeps its exact C# name in
// TypeRef::raw and reports Unknown. That's what Unknown is for.
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
        case ElementType::Char:    return {TypeKind::UInt16, 2};   // System.Char is UTF-16
        case ElementType::I4:      return {TypeKind::Int32,  4};
        case ElementType::U4:      return {TypeKind::UInt32, 4};
        case ElementType::I8:      return {TypeKind::Int64,  8};
        case ElementType::U8:      return {TypeKind::UInt64, 8};
        case ElementType::R4:      return {TypeKind::Float,  4};
        case ElementType::R8:      return {TypeKind::Double, 8};
        case ElementType::I:       return {TypeKind::Int64,  8};   // nint, on the only ABI here
        case ElementType::U:       return {TypeKind::UInt64, 8};
        case ElementType::String:  return {TypeKind::String, 8};
        case ElementType::Object:
        case ElementType::Class:   return {TypeKind::ObjectPtr, 8};
        case ElementType::SzArray:
        case ElementType::Array:   return {TypeKind::Array, 8};
        default:                   return {TypeKind::Unknown, 0};
    }
}

// Consts come back as bytes. Whether the top bit is a big positive or a small negative is
// in the declared type and nowhere else. Get it wrong and every -1 reads as four billion.
std::int64_t Widen(std::uint64_t raw, ElementType element) {
    switch (element) {
        case ElementType::I1: return static_cast<std::int8_t>(raw);
        case ElementType::I2: return static_cast<std::int16_t>(raw);
        case ElementType::I4: return static_cast<std::int32_t>(raw);
        case ElementType::I8: return static_cast<std::int64_t>(raw);
        default:              return static_cast<std::int64_t>(raw);
    }
}

// The IR names an enum's underlying type as int32/uint8/etc, and the linter reads it that
// way. "System.Int32" in this field made the linter treat it as a byte and complain that
// 343 didn't fit.
std::string UnderlyingName(ElementType element) {
    switch (element) {
        case ElementType::I1: return "int8";
        case ElementType::U1: return "uint8";
        case ElementType::I2: return "int16";
        case ElementType::U2: return "uint16";
        case ElementType::Char: return "uint16";
        case ElementType::I4: return "int32";
        case ElementType::U4: return "uint32";
        case ElementType::I8: return "int64";
        case ElementType::U8: return "uint64";
        case ElementType::Boolean: return "uint8";
        default: return "int32";
    }
}

// Same timestamp format as the Unreal side, so dumps from both sort together.
std::string Utc() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(now));
}

class Walker {
public:
    Walker(IBridge& bridge, const WalkOptions& options, WalkStats& stats)
        : bridge_(bridge), options_(options), stats_(stats) {}

    ir::Dump Run() {
        ir::Dump dump;
        dump.header.runtime           = "il2cpp";
        dump.header.source.main_module = "GameAssembly";
        dump.header.source.module_base = Raw(bridge_.ModuleBase());
        dump.header.source.image_size  = bridge_.ModuleSize();
        dump.header.engine.evidence    = bridge_.Evidence();
        dump.header.created_utc        = Utc();

        // Same two things the Unreal side records: derived offsets, and where things live.
        for (const auto& [name, value] : bridge_.Derived())
            dump.header.offsets.push_back(ir::DerivedOffset{name, value});
        dump.header.globals = bridge_.EntryPoints();

        // Classes, structs and enums come out of one enumeration and get sorted into the
        // IR's three lists. Logged as we go, log flushed per line -- if the walk takes the
        // game down we at least know what it was reading.
        const auto assemblies = bridge_.Assemblies();
        for (const Address assembly : assemblies) {
            ++stats_.assemblies;
            const Address image = bridge_.AssemblyImage(assembly);
            if (IsNull(image)) continue;
            ++stats_.images;

            const std::string package = bridge_.ImageName(image);
            const std::size_t count   = bridge_.ImageClassCount(image);
            core::LogDebug("assembly {}/{}: {}, {} types", stats_.assemblies,
                           assemblies.size(), package, count);

            for (std::size_t i = 0; i < count; ++i)
                Visit(bridge_.ImageClass(image, i), package);
        }

        // A generic definition has no real field offsets; its instantiations do, and they
        // only exist in the class cache. Swept second so definitions land first.
        if (options_.include_inflated) {
            core::LogDebug("sweeping the runtime class cache for generic instantiations");
            const auto cached = bridge_.AllClasses();
            core::LogDebug("{} classes in the cache", cached.size());
            for (const Address klass : cached) {
                if (seen_.count(Raw(klass))) continue;
                const auto facts = bridge_.Class(klass);
                if (!facts.is_inflated) continue;
                ++stats_.inflated;
                Visit(klass, ImagePackage(facts.image));
            }
        }
        core::LogDebug("walk complete: {} types visited", stats_.classes + stats_.enums);

        Finish(dump);
        return dump;
    }

private:
    // C# name, built outer-first. A nested type reports no namespace of its own, so we walk
    // out to find one.
    std::string NameOf(Address klass) {
        if (IsNull(klass)) return {};
        if (const auto it = names_.find(Raw(klass)); it != names_.end()) return it->second;

        const auto facts = bridge_.Class(klass);
        std::string name;

        // Instantiations need naming as such. il2cpp_class_get_name says "List`1" for
        // List<int> and List<string> alike, and building names from it put every
        // instantiation on its definition's name. The type name carries the args.
        if (facts.is_inflated) {
            name = bridge_.Type(bridge_.ClassType(klass)).name;
            // Runtime writes nested types with a slash; the rest of the dump (and C#) uses
            // dots.
            std::replace(name.begin(), name.end(), '/', '.');
        }

        if (name.empty()) {
            if (!IsNull(facts.declaring) && Raw(facts.declaring) != Raw(klass))
                name = NameOf(facts.declaring) + ".";
            else if (!facts.name_space.empty())
                name = facts.name_space + ".";
            name += facts.name;
        }

        names_.emplace(Raw(klass), name);
        return name;
    }

    // The IR path. It's an identity: emitters key on it, diff matches on it, the linter
    // rejects two types with the same one.
    //
    // A C# name is only unique inside its assembly -- every assembly has a <Module>, so a
    // game with 80 assemblies has 80 of them. The assembly goes on the end, .NET-style.
    // Same thing the Unreal side does with "/Script/Engine" at the front.
    std::string PathOf(Address klass) {
        if (IsNull(klass)) return {};
        if (const auto it = paths_.find(Raw(klass)); it != paths_.end()) return it->second;

        std::string path = NameOf(klass);
        if (!path.empty()) {
            const auto assembly = AssemblyOf(bridge_.Class(klass).image);
            if (!assembly.empty()) path += ", " + assembly;
        }

        paths_.emplace(Raw(klass), path);
        return path;
    }

    // "Assembly-CSharp.dll" is the file, "Assembly-CSharp" is the assembly.
    std::string AssemblyOf(Address image) {
        if (IsNull(image)) return {};
        const auto key = Raw(image);
        if (const auto it = assemblies_.find(key); it != assemblies_.end()) return it->second;

        std::string name = bridge_.ImageName(image);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
            name.resize(name.size() - 4);
        assemblies_.emplace(key, name);
        return name;
    }

    std::string ImagePackage(Address image) {
        if (IsNull(image)) return "<unknown>";
        const auto key = Raw(image);
        if (const auto it = packages_.find(key); it != packages_.end()) return it->second;
        auto name = bridge_.ImageName(image);
        if (name.empty()) name = "<unknown>";
        packages_.emplace(key, name);
        return name;
    }

    // A class as a type reference. Memoised; the walk asks ~1.5M times.
    //
    // Both the type branch and the array-element branch go through here. They didn't, and
    // the array copy never checked is_enum -- so every SomeEnum[] claimed to be a struct and
    // the linter reported 29k types missing that were all there.
    const ir::TypeRef& FromClass(Address klass) {
        static const ir::TypeRef kNone;
        if (IsNull(klass)) return kNone;

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

        // A generic parameter has its own class in the runtime, named T. Pointing a ref at
        // it names something that isn't a type and is in no dump. Kind and size stay.
        if (!IsGenericParameter(bridge_.Type(bridge_.ClassType(klass)).element))
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
        ref.raw  = facts.name;   // the full C# name, generic arguments and all

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
                // klass is the element class here, which is what the IR wants.
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

        const auto facts = bridge_.Class(klass);
        const std::string path = PathOf(klass);
        if (path.empty()) return;
        if (!options_.filter.empty() && path.find(options_.filter) == std::string::npos) return;

        // Some instantiations are over another generic's parameter -- IEnumerable<T> where T
        // is still T. Distinct classes to the runtime, identical names to us. Keep one,
        // count the rest; two records on one path is a contradiction.
        if (!taken_.insert(path).second) {
            ++stats_.indistinguishable;
            return;
        }

        if (facts.is_enum) {
            BuildEnum(klass, facts, path, package);
            return;
        }
        BuildStruct(klass, facts, path, package);
    }

    void BuildEnum(Address klass, const ClassFacts& facts, const std::string& path,
                   const std::string& package) {
        ir::Enum record;
        record.name = facts.name;
        record.path = path;

        const Address base = bridge_.EnumBaseType(klass);
        std::int32_t width = 4;
        ElementType  element = ElementType::I4;
        if (!IsNull(base)) {
            const auto base_facts = bridge_.Type(base);
            const auto mapped     = MapElement(base_facts.element);
            record.underlying     = UnderlyingName(base_facts.element);
            element               = base_facts.element;
            if (mapped.size > 0) width = mapped.size;
        }

        for (const Address field : bridge_.Fields(klass)) {
            const auto info = bridge_.Field(field);
            // One instance field (value__) plus the consts, which are the members.
            if (!(info.flags & field_attr::kLiteral)) continue;

            ir::EnumValue entry;
            entry.name = info.name;

            std::uint64_t raw_value = 0;
            if (options_.resolve_enum_values &&
                bridge_.LiteralValue(field, &raw_value, static_cast<std::size_t>(width))) {
                entry.value = Widen(raw_value, element);
            } else {
                // No API call reads a const directly. When the build won't answer, members
                // keep their names and the values are just absent. Numbering by position
                // would look right and be wrong for any enum that sets its own values.
                record.values_resolved = false;
            }
            record.values.push_back(std::move(entry));
        }

        if (!record.values_resolved) ++stats_.enums_without_values;
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
        record.is_interface = facts.is_interface;
        record.is_abstract  = facts.is_abstract;
        record.is_generic   = facts.is_generic;
        record.token        = facts.token;
        record.explicit_layout =
            (facts.flags & type_attr::kLayoutMask) == type_attr::kExplicitLayout;

        // Value type: unboxed size, that's what an SDK declares. Reference type: whole
        // object, header included, that's what a pointer addresses.
        record.size = facts.is_valuetype && facts.value_size > 0 ? facts.value_size
                                                                 : facts.instance_size;
        // Only with a base that has a size. A value type's parent is System.ValueType whose
        // size is the boxed header, and a type never laid out reports 0, which isn't a size.
        if (!IsNull(facts.parent) && !facts.is_valuetype) {
            const auto parent_size = bridge_.Class(facts.parent).instance_size;
            if (parent_size > 0) record.inherited_size = parent_size;
        }

        for (const Address iface : bridge_.Interfaces(klass))
            record.interfaces.push_back(PathOf(iface));

        const std::int32_t header = bridge_.ObjectHeaderSize();
        for (const Address field : bridge_.Fields(klass)) {
            const auto info = bridge_.Field(field);

            ir::Property property;
            property.name      = info.name;
            property.type      = TypeOf(info.type);
            property.flags     = info.flags;
            property.flag_names= FieldFlagNames(info.flags);
            property.is_static = (info.flags & field_attr::kStatic) != 0;
            property.offset    = info.offset;
            property.size      = property.type.size;

            // Three ways an offset can be a number that means nothing, and the runtime returns
            // a number in all three: open definition (no layout), const (no storage),
            // thread-static (per-thread storage, signalled with a negative offset).
            const bool is_literal = (info.flags & field_attr::kLiteral) != 0;
            if (facts.is_generic || is_literal || info.offset < 0) {
                property.offset            = 0;
                property.offset_unresolved = true;
                ++stats_.unresolved_offsets;
            } else if (facts.is_valuetype && !property.is_static && header > 0 &&
                       info.offset >= header) {
                // The value-type trap. Runtime measures from the boxed start even for structs,
                // so the real layout sits a header lower. Size is recorded unboxed, so offsets
                // have to be too, or nothing in the record compares with anything else -- the
                // linter found this by reporting Vector3.z past the end of Vector3.
                //
                // Raw number kept beside it.
                property.offset       = info.offset - header;
                property.boxed_offset = info.offset;
            }

            if (is_literal && !property.type.raw.empty()) {
                std::uint64_t raw_value = 0;
                const auto width = static_cast<std::size_t>(
                    property.type.size > 0 && property.type.size <= 8 ? property.type.size : 0);
                if (options_.resolve_enum_values && width &&
                    bridge_.LiteralValue(field, &raw_value, width)) {
                    property.default_value =
                        std::to_string(Widen(raw_value, bridge_.Type(info.type).element));
                }
            }

            ++stats_.fields;
            record.properties.push_back(std::move(property));
        }

        std::unordered_set<std::string> accessor_methods;
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
                accessor_methods.insert(getter.name);
            }
            if (!IsNull(info.setter)) {
                const auto setter = bridge_.Method(info.setter);
                accessor.setter = setter.name;
                accessor_methods.insert(setter.name);
                if (accessor.type.raw.empty() && setter.param_count > 0)
                    accessor.type = TypeOf(bridge_.ParamType(info.setter, 0));
            }

            ++stats_.accessors;
            record.accessors.push_back(std::move(accessor));
        }

        for (const Address method : bridge_.Methods(klass)) {
            const auto info = bridge_.Method(method);

            ir::Function function;
            function.name       = info.name;
            function.flags      = info.flags;
            function.flag_names = MethodFlagNames(info.flags);
            function.token      = info.token;

            for (std::uint32_t i = 0; i < info.param_count; ++i) {
                const Address param_type = bridge_.ParamType(method, i);
                ir::FunctionParam param;
                param.name   = bridge_.ParamName(method, i);
                param.type   = TypeOf(param_type);
                param.size   = param.type.size;
                // ref/out. byref is a flag on the type, not its own element kind.
                param.is_out = !IsNull(param_type) && bridge_.Type(param_type).by_ref;
                function.params.push_back(std::move(param));
            }
            if (!IsNull(info.return_type)) {
                ir::FunctionParam result;
                result.name      = "ReturnValue";
                result.type      = TypeOf(info.return_type);
                result.size      = result.type.size;
                result.is_return = true;
                function.params.push_back(std::move(result));
            }

            // Module-relative, always. A VA stops meaning anything on the next launch.
            if (!IsNull(info.body)) {
                const auto base = Raw(bridge_.ModuleBase());
                const auto body = Raw(info.body);
                if (body >= base && body < base + bridge_.ModuleSize()) {
                    function.native_rva = body - base;
                    ++body_counts_[function.native_rva];
                    ++stats_.bodies;
                }
            }

            ++stats_.methods;
            record.functions.push_back(std::move(function));
        }

        if (facts.is_generic) ++stats_.open_generics;
        ++stats_.classes;

        // Every so often. Enough to place a crash, not enough to make the log the slow part.
        if ((stats_.classes % 1000) == 0)
            core::LogDebug("{} types in, currently at {}", stats_.classes, path);

        auto& into = PackageFor(package);
        (facts.is_valuetype ? into.structs : into.classes).push_back(std::move(record));
    }

    ir::Package& PackageFor(const std::string& name) {
        const auto it = package_index_.find(name);
        if (it != package_index_.end()) return packages_out_[it->second];
        package_index_.emplace(name, packages_out_.size());
        packages_out_.push_back(ir::Package{name, {}, {}, {}});
        return packages_out_.back();
    }

    // Which bodies more than one method landed on. Only knowable after the fact. Unreferenced
    // methods get a shared stub and the linker folds identical bodies, so this is a count,
    // not a suspicion.
    void Finish(ir::Dump& dump) {
        const auto mark = [&](std::vector<ir::Struct>& records) {
            for (auto& record : records)
                for (auto& function : record.functions) {
                    if (function.native_rva == 0) continue;
                    if (body_counts_[function.native_rva] > 1) {
                        function.shared_body = true;
                        ++stats_.shared_bodies;
                    }
                }
        };
        for (auto& package : packages_out_) {
            mark(package.classes);
            mark(package.structs);
        }

        std::sort(packages_out_.begin(), packages_out_.end(),
                  [](const ir::Package& a, const ir::Package& b) { return a.name < b.name; });
        dump.packages = std::move(packages_out_);
    }

    IBridge&           bridge_;
    const WalkOptions& options_;
    WalkStats&         stats_;

    std::unordered_set<std::uint64_t>              seen_;
    std::unordered_set<std::string>                taken_;
    std::unordered_map<std::uint64_t, std::string> names_;
    std::unordered_map<std::uint64_t, std::string> paths_;
    std::unordered_map<std::uint64_t, std::string> assemblies_;
    std::unordered_map<std::uint64_t, ir::TypeRef> class_refs_;
    std::unordered_map<std::uint64_t, std::string> packages_;
    std::unordered_map<std::uint64_t, int>         body_counts_;
    std::unordered_map<std::string, std::size_t>   package_index_;
    std::vector<ir::Package>                       packages_out_;
};

} // namespace

ir::Dump Walk(IBridge& bridge, const WalkOptions& options, WalkStats& stats) {
    Walker walker(bridge, options, stats);
    return walker.Run();
}

} // namespace zircon::il2cpp
