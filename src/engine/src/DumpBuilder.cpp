#include "engine/DumpBuilder.h"
#include "engine/ValueReader.h"
#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <map>

namespace zircon::engine {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// Anything unrecognised stays Unknown with `raw` preserved, so a dump never quietly loses
// a type this build hasn't been taught about.
ir::TypeKind KindFor(const std::string& raw, bool has_enum_reference) {
    if (raw == "BoolProperty")      return ir::TypeKind::Bool;
    if (raw == "ByteProperty")      return has_enum_reference ? ir::TypeKind::Enum
                                                              : ir::TypeKind::UInt8;
    if (raw == "Int8Property")      return ir::TypeKind::Int8;
    if (raw == "Int16Property")     return ir::TypeKind::Int16;
    if (raw == "IntProperty")       return ir::TypeKind::Int32;
    if (raw == "Int64Property")     return ir::TypeKind::Int64;
    if (raw == "UInt16Property")    return ir::TypeKind::UInt16;
    if (raw == "UInt32Property")    return ir::TypeKind::UInt32;
    if (raw == "UInt64Property")    return ir::TypeKind::UInt64;
    if (raw == "FloatProperty")     return ir::TypeKind::Float;
    if (raw == "DoubleProperty")    return ir::TypeKind::Double;
    if (raw == "NameProperty")      return ir::TypeKind::Name;
    if (raw == "StrProperty")       return ir::TypeKind::String;
    if (raw == "TextProperty")      return ir::TypeKind::Text;
    if (raw == "EnumProperty")      return ir::TypeKind::Enum;
    if (raw == "StructProperty")    return ir::TypeKind::Struct;
    if (raw == "ObjectProperty" || raw == "ObjectPtrProperty") return ir::TypeKind::ObjectPtr;
    if (raw == "WeakObjectProperty")   return ir::TypeKind::WeakPtr;
    if (raw == "LazyObjectProperty")   return ir::TypeKind::LazyPtr;
    if (raw == "SoftObjectProperty")   return ir::TypeKind::SoftPtr;
    if (raw == "SoftClassProperty")    return ir::TypeKind::SoftClassPtr;
    if (raw == "ClassProperty")        return ir::TypeKind::ClassPtr;
    if (raw == "InterfaceProperty")    return ir::TypeKind::Interface;
    if (raw == "ArrayProperty")        return ir::TypeKind::Array;
    if (raw == "SetProperty")          return ir::TypeKind::Set;
    if (raw == "MapProperty")          return ir::TypeKind::Map;
    if (raw == "OptionalProperty")     return ir::TypeKind::Optional;
    if (raw == "DelegateProperty")     return ir::TypeKind::Delegate;
    if (raw == "MulticastDelegateProperty" || raw == "MulticastInlineDelegateProperty" ||
        raw == "MulticastSparseDelegateProperty")
        return ir::TypeKind::MulticastDelegate;
    if (raw == "FieldPathProperty")    return ir::TypeKind::FieldPath;
    return ir::TypeKind::Unknown;
}

ir::TypeRef ToIr(const ResolvedType& resolved) {
    ir::TypeRef type;
    type.raw  = resolved.raw;
    type.name = resolved.referenced;
    type.size = resolved.size;
    type.kind = KindFor(resolved.raw, !resolved.referenced.empty());

    for (const auto& param : resolved.params) {
        // Drop an unresolved inner type instead of emitting a nameless Unknown. "TArray<>"
        // with a phantom element misleads more than an empty params list, which emitters
        // already read as "opaque container".
        if (param.raw.empty()) continue;
        type.params.push_back(ToIr(param));
    }
    return type;
}

std::string Utc() {
    const auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(now));
}

// Everything before the last dot is the outer chain; the package is the first segment.
std::string PackageOf(const std::string& path) {
    const auto dot = path.find('.');
    return dot == std::string::npos ? path : path.substr(0, dot);
}

std::string LeafOf(const std::string& path) {
    const auto dot = path.find_last_of('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}

} // namespace

ResolveContext Reflection::Context() const {
    ResolveContext context;
    context.memory          = memory;
    context.array           = &array;
    context.pool            = &pool;
    context.object_layout   = &object_layout;
    context.struct_layout   = &struct_layout;
    context.property_layout = &property_layout;
    context.subclass_layout = &subclass_layout;
    context.profile         = &profile;

    // Without this a default reads "3" instead of "ECC_Visibility". The CLI's session wired
    // it up and this one didn't, so the same property resolved differently depending on
    // which entry point asked. Precisely the disagreement the diff tool exists to catch.
    context.enum_layout     = &enum_layout;
    context.script_layout   = &script_layout;
    return context;
}

bool Reflection::Valid() const {
    return memory && array.Valid() && pool.Valid() && object_layout.Valid() &&
           struct_layout.Valid() && property_layout.Valid();
}

Reflection Reflect(core::IMemorySource& memory) {
    Reflection reflection;
    reflection.memory  = &memory;
    reflection.profile = FingerprintEngine(memory);

    const auto array = FindObjectArray(memory, reflection.profile);
    if (!array) return reflection;
    reflection.array = *array;

    const auto pool = FindNamePool(memory, reflection.profile);
    if (!pool) return reflection;
    reflection.pool = *pool;

    reflection.object_layout = DeriveObjectLayout(memory, reflection.array, reflection.pool);
    if (!reflection.object_layout.Valid()) return reflection;

    reflection.struct_layout = DeriveStructLayout(memory, reflection.array, reflection.pool,
                                                   reflection.object_layout);
    if (!reflection.struct_layout.Valid()) return reflection;

    reflection.property_layout = DerivePropertyLayout(
        memory, reflection.array, reflection.pool, reflection.object_layout,
        reflection.struct_layout);
    if (!reflection.property_layout.Valid()) return reflection;

    reflection.class_layout = DeriveClassLayout(memory, reflection.array, reflection.pool,
                                                reflection.object_layout,
                                                reflection.struct_layout);

    reflection.subclass_layout = DeriveSubclassLayout(
        memory, reflection.array, reflection.pool, reflection.object_layout,
        reflection.struct_layout, reflection.property_layout);

    reflection.function_layout = DeriveFunctionLayout(
        memory, reflection.array, reflection.pool, reflection.object_layout,
        reflection.struct_layout, reflection.property_layout);

    reflection.enum_layout = DeriveEnumLayout(memory, reflection.array, reflection.pool,
                                               reflection.object_layout);

    reflection.script_layout = DeriveScriptLayout(memory, reflection.array, reflection.pool,
                                                   reflection.object_layout,
                                                   reflection.struct_layout);
    return reflection;
}

ir::Dump BuildDump(const Reflection& reflection, const BuildOptions& options) {
    ir::Dump dump;
    auto& memory = *reflection.memory;
    const auto context = reflection.Context();

    // These land in a file rather than on a screen, so they can run longer than the
    // browser's.
    ValueFormat value_format;
    value_format.max_array_items = 8;
    value_format.max_depth       = 3;

    // --- header -----------------------------------------------------------------------
    dump.header.tool_version = "0.1.0-dev";
    dump.header.created_utc  = Utc();
    dump.header.partial      = !memory.Caps().live_objects;

    const auto* main_module = memory.MainModule();
    const std::uint64_t base = main_module ? Raw(main_module->base) : 0;

    dump.header.source.kind        = memory.Caps().can_call ? "internal"
                                   : memory.Caps().live_objects ? "external" : "static";
    dump.header.source.process     = main_module ? main_module->name : std::string{};
    dump.header.source.main_module = dump.header.source.process;
    dump.header.source.module_base = base;
    dump.header.source.image_size  = main_module ? main_module->size : 0;

    dump.header.engine.version              = reflection.profile.VersionString();
    dump.header.engine.confidence           = reflection.profile.confidence;
    dump.header.engine.uses_fproperty       = reflection.profile.uses_fproperty;
    dump.header.engine.chunked_gobjects     = reflection.profile.chunked_gobjects;
    dump.header.engine.chunked_name_pool    = reflection.profile.chunked_name_pool;
    dump.header.engine.case_preserving_name = reflection.pool.case_preserving;
    dump.header.engine.evidence             = reflection.profile.evidence;

    // Every derived offset gets recorded. What makes a dump auditable: a reader sees what
    // the tool concluded about layout, and not only what it produced from that.
    const auto& ol = reflection.object_layout;
    const auto& sl = reflection.struct_layout;
    const auto& pl = reflection.property_layout;
    const auto& fl = reflection.function_layout;
    dump.header.offsets = {
        {"UObject.InternalIndex",      ol.index_offset},
        {"UObject.ClassPrivate",       ol.class_offset},
        {"UObject.NamePrivate",        ol.name_offset},
        {"UObject.OuterPrivate",       ol.outer_offset},
        {"UStruct.SuperStruct",        sl.super_struct},
        {"UStruct.Children",           sl.children},
        {"UStruct.ChildProperties",    sl.child_properties},
        {"UStruct.PropertiesSize",     sl.properties_size},
        {"UStruct.MinAlignment",       sl.min_alignment},
        {"FField.ClassPrivate",        pl.class_private},
        {"FField.Next",                pl.next},
        {"FField.NamePrivate",         pl.name},
        {"FProperty.ArrayDim",         pl.array_dim},
        {"FProperty.ElementSize",      pl.element_size},
        {"FProperty.PropertyFlags",    pl.property_flags},
        {"FProperty.Offset_Internal",  pl.offset_internal},
        {"UField.Next",                fl.field_next},
        {"UFunction.FunctionFlags",    fl.function_flags},
        {"UFunction.Func",             fl.native_func},
    };

    dump.header.globals = {
        std::format("GObjects=0x{:x}", Raw(reflection.array.gobjects) - base),
        std::format("FNamePool=0x{:x}", Raw(reflection.pool.blocks) - base),
    };

    if (options.include_names) {
        for (std::uint32_t id = 0, misses = 0; misses < 4096; ++id) {
            std::string name = ResolveName(memory, reflection.pool, id);
            if (name.empty()) { ++misses; continue; }
            misses = 0;
            dump.names.push_back(std::move(name));
        }
    }

    // --- walk -------------------------------------------------------------------------
    std::map<std::string, ir::Package> packages;
    auto package_for = [&](const std::string& path) -> ir::Package& {
        const std::string name = PackageOf(path);
        auto it = packages.find(name);
        if (it == packages.end()) {
            ir::Package package;
            package.name = name;
            it = packages.emplace(name, std::move(package)).first;
        }
        return it->second;
    };

    int skipped_unnamed = 0;

    for (std::int32_t index = 0; index < reflection.array.num_elements; ++index) {
        const auto object = ObjectAt(memory, reflection.array, index);
        if (IsNull(object)) continue;

        // By meta-class ancestry, not by name. A Blueprint class reports
        // BlueprintGeneratedClass, so a name comparison skips it and takes every Blueprint
        // type in the game with it.
        const ObjectKind kind = ClassifyObject(memory, ol, sl, reflection.pool, object);
        const bool is_class  = kind == ObjectKind::Class;
        const bool is_struct = kind == ObjectKind::ScriptStruct;
        const bool is_enum   = kind == ObjectKind::Enum;
        if (!is_class && !is_struct && !is_enum) continue;

        const std::string path = GetObjectPathName(memory, ol, reflection.pool, object);
        if (path.empty()) { ++skipped_unnamed; continue; }
        if (!options.package_filter.empty() &&
            path.find(options.package_filter) == std::string::npos)
            continue;

        if (is_enum) {
            ir::Enum record;
            record.path = path;
            record.name = LeafOf(path);
            for (const auto& [name, value] :
                 GetEnumValues(memory, reflection.enum_layout, reflection.pool, object))
                record.values.push_back(ir::EnumValue{name, value});
            package_for(path).enums.push_back(std::move(record));
            continue;
        }

        ir::Struct record;
        record.path     = path;
        record.name     = LeafOf(path);
        record.is_class = is_class;
        record.size     = GetPropertiesSize(memory, sl, object);
        record.alignment = GetMinAlignment(memory, sl, object);

        // One real instance of the class that the engine keeps around, and the only place
        // a default value exists. Its address isn't recorded: heap memory means nothing
        // outside this run. Only the contents matter.
        core::Address cdo{};
        if (is_class && reflection.class_layout.Valid())
            cdo = GetClassDefaultObject(memory, reflection.class_layout, object);

        // First pointer of any object with virtual functions is its vtable, and the CDO is
        // an object of this class. Only accepted pointing into the image, which is what
        // separates a real vtable from an uninitialised or reused slot and what makes the
        // value a stable RVA instead of a heap address.
        if (!IsNull(cdo) && main_module) {
            const auto vtable = core::ReadOr<core::Address>(memory, cdo);
            const std::uint64_t raw = Raw(vtable);
            if (raw >= base && raw < base + main_module->size)
                record.vtable_rva = raw - base;
        }

        // Walked here on the live chain, the only place the whole chain is guaranteed
        // reachable. An emitter working from a filtered dump can't, and used to answer 'U'
        // for every actor without saying so.
        if (is_class) {
            std::size_t guard = 0;
            for (auto current = object; !IsNull(current) && guard < 128; ++guard) {
                const std::string chain = GetObjectPathName(memory, ol, reflection.pool, current);
                if (chain == "/Script/Engine.Actor")               { record.cpp_prefix = 'A'; break; }
                if (chain == "/Script/CoreUObject.Interface")      { record.cpp_prefix = 'I'; break; }
                current = GetSuperStruct(memory, sl, current);
            }
            if (record.cpp_prefix == 0) record.cpp_prefix = 'U';
        } else {
            record.cpp_prefix = 'F';
        }

        for (const auto iface :
             GetClassInterfaces(memory, reflection.array, reflection.class_layout, object))
            record.interfaces.push_back(
                GetObjectPathName(memory, ol, reflection.pool, iface));

        const auto super = GetSuperStruct(memory, sl, object);
        if (!IsNull(super)) {
            record.super = GetObjectPathName(memory, ol, reflection.pool, super);
            // Where this type's own members begin. Emitters need it to lay out padding
            // without re-walking the base.
            record.inherited_size = GetPropertiesSize(memory, sl, super);
        }

        for (const auto field : GetChildProperties(memory, sl, pl, object)) {
            ir::Property property;
            property.name      = GetFieldName(memory, pl, reflection.pool, field);
            property.offset    = GetPropertyOffset(memory, pl, field);
            property.array_dim = std::max(1, GetArrayDim(memory, pl, field));
            property.flags     = GetPropertyFlags(memory, pl, field);
            property.flag_names = DescribePropertyFlags(property.flags);
            property.type      = ToIr(ResolveType(context, field));
            property.size      = GetElementSize(memory, pl, field) * property.array_dim;

            const auto bits = ResolveBitfield(context, field);
            property.is_bitfield = bits.is_bitfield;
            property.byte_mask   = bits.byte_mask;
            property.field_mask  = bits.field_mask;
            property.bit_index   = bits.bit_index;

            // Opt-in: reading a value costs several memory reads per property, and most
            // consumers of a dump want the layout, not the contents.
            if (options.include_defaults && !IsNull(cdo))
                property.default_value = ReadPropertyValue(context, cdo, field, value_format);

            record.properties.push_back(std::move(property));
        }

        if (is_class && fl.Valid()) {
            for (const auto function :
                 GetClassFunctions(memory, reflection.array, reflection.pool, ol, sl, fl,
                                   object)) {
                ir::Function entry;
                entry.name  = GetObjectName(memory, ol, reflection.pool, function);

                // A name that does not resolve is not an identity. Several unnamed
                // functions in one type would collapse into each other, and which one
                // survived a lookup would depend on iteration order — which made two dumps
                // of the same process disagree. The FName id is stable and distinguishing.
                if (entry.name.empty())
                    entry.name = std::format("<unnamed#{}>",
                                             GetObjectNameId(memory, ol, function));
                entry.flags = GetFunctionFlags(memory, fl, function);
                entry.flag_names = DescribeFunctionFlags(entry.flags);

                const auto native = GetNativeFunc(memory, fl, function);
                if (!IsNull(native) && Raw(native) >= base)
                    entry.native_rva = Raw(native) - base;

                for (const auto param : GetChildProperties(memory, sl, pl, function)) {
                    const auto flags = GetPropertyFlags(memory, pl, param);
                    if (!(flags & property_flags::kParm)) continue;

                    ir::FunctionParam argument;
                    argument.name      = GetFieldName(memory, pl, reflection.pool, param);
                    argument.type      = ToIr(ResolveType(context, param));
                    argument.offset    = GetPropertyOffset(memory, pl, param);
                    argument.size      = GetElementSize(memory, pl, param);
                    argument.is_return = (flags & property_flags::kReturnParm) != 0;
                    argument.is_out    = (flags & property_flags::kOutParm) != 0;
                    argument.is_const  = (flags & property_flags::kConstParm) != 0;
                    entry.params.push_back(std::move(argument));
                }

                if (options.include_script && reflection.script_layout.Valid()) {
                    const auto decompiled =
                        DecompileFunction(context, reflection.script_layout, function);
                    entry.script_size     = static_cast<std::int32_t>(decompiled.bytes_total);
                    entry.script_complete = decompiled.complete;
                    for (const auto& line : decompiled.lines)
                        entry.script.push_back(ir::ScriptStatement{
                            line.offset, line.depth, line.text});
                }

                record.functions.push_back(std::move(entry));
            }
        }

        auto& package = package_for(path);
        if (is_class) package.classes.push_back(std::move(record));
        else          package.structs.push_back(std::move(record));
    }

    // --- enum widths ------------------------------------------------------------------
    // UEnum does not record how wide its values are stored; only the property using it
    // does, through FEnumProperty::UnderlyingProp. Leaving this at the uint8 default made
    // every 4-byte enum emit as 1 byte, shifting every following member of
    // every struct containing one. Collected across the whole dump, because an enum is
    // frequently declared in one package and used in another.
    {
        std::map<std::string, ir::TypeKind> widths;

        std::function<void(const ir::TypeRef&)> note = [&](const ir::TypeRef& type) {
            if (type.kind == ir::TypeKind::Enum && !type.name.empty() &&
                !type.params.empty()) {
                widths.emplace(type.name, type.params.front().kind);
            }
            for (const auto& param : type.params) note(param);
        };

        for (const auto& package : packages) {
            for (const auto* list : {&package.second.classes, &package.second.structs})
                for (const auto& record : *list) {
                    for (const auto& property : record.properties) note(property.type);
                    for (const auto& function : record.functions)
                        for (const auto& param : function.params) note(param.type);
                }
        }

        auto name_for = [](ir::TypeKind kind) -> const char* {
            switch (kind) {
                case ir::TypeKind::Int8:   return "int8";
                case ir::TypeKind::Int16:  return "int16";
                case ir::TypeKind::Int32:  return "int32";
                case ir::TypeKind::Int64:  return "int64";
                case ir::TypeKind::UInt8:  return "uint8";
                case ir::TypeKind::UInt16: return "uint16";
                case ir::TypeKind::UInt32: return "uint32";
                case ir::TypeKind::UInt64: return "uint64";
                default: return nullptr;
            }
        };

        int widened = 0;
        for (auto& [name, package] : packages)
            for (auto& record : package.enums) {
                const auto it = widths.find(record.path);
                if (it == widths.end()) continue;   // unused enum: uint8 is the UE default
                if (const char* text = name_for(it->second)) {
                    if (record.underlying != text) ++widened;
                    record.underlying = text;
                }
            }

        if (widened > 0)
            core::LogDebug("{} enums are wider than the uint8 default", widened);
    }

    dump.packages.reserve(packages.size());
    for (auto& [name, package] : packages) dump.packages.push_back(std::move(package));

    if (skipped_unnamed > 0) {
        core::LogWarn("{} objects had no resolvable path and were skipped", skipped_unnamed);
    }

    core::LogInfo("dump: {} packages, {} classes, {} structs, {} enums, {} properties, "
                  "{} functions", dump.packages.size(), dump.TotalClasses(),
                  dump.TotalStructs(), dump.TotalEnums(), dump.TotalProperties(),
                  dump.TotalFunctions());
    return dump;
}

} // namespace zircon::engine
