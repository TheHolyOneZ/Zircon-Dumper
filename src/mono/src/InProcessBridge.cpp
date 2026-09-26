#include "mono/Bridge.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace zircon::mono {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

constexpr std::size_t kMaxMembers = 65536;
constexpr std::uint32_t kMaxImageTypes = 1u * 1000 * 1000;

using FnVoidPtr     = void* (*)();
using FnPtrToPtr    = void* (*)(void*);
using FnPtrToStr    = const char* (*)(void*);
using FnPtrToCharP  = char* (*)(void*);
using FnPtrToI32    = std::int32_t (*)(void*);
using FnPtrToU32    = std::uint32_t (*)(void*);
using FnIterate     = void* (*)(void*, void**);
using FnImageRows   = std::int32_t (*)(void*, std::int32_t);
using FnClassGet    = void* (*)(void*, std::uint32_t);
using FnValueSize   = std::int32_t (*)(void*, std::uint32_t*);
using FnMethodFlags = std::uint32_t (*)(void*, std::uint32_t*);
using FnForeach     = void (*)(void*, void (*)(void*, void*), void*);
using FnFree        = void (*)(void*);
using FnVersion     = const char* (*)(void*, std::uint16_t*, std::uint16_t*,
                                      std::uint16_t*, std::uint16_t*);

template <typename Fn>
Fn At(Address address) {
    return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(Raw(address)));
}

void* AsPtr(Address address) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(Raw(address)));
}

Address FromPtr(void* p) {
    return core::Address{reinterpret_cast<std::uint64_t>(p)};
}

void CollectAssembly(void* assembly, void* user) {
    if (assembly) static_cast<std::vector<Address>*>(user)->push_back(FromPtr(assembly));
}

class InProcessBridge final : public IBridge {
public:
    InProcessBridge(const RuntimeInfo& runtime, core::IMemorySource& memory)
        : runtime_(runtime), memory_(memory) {}

    bool Attach() override {
        domain_ = FromPtr(At<FnVoidPtr>(runtime_.api.get_root_domain)());
        if (IsNull(domain_)) {
            core::LogError("mono_get_root_domain returned null, so there is no domain to walk");
            return false;
        }

        if (!IsNull(runtime_.api.thread_attach)) {
            const auto thread = At<FnPtrToPtr>(runtime_.api.thread_attach)(AsPtr(domain_));
            if (thread == nullptr) {
                core::LogError("mono_thread_attach refused this thread");
                return false;
            }
            evidence_.emplace_back("attached to the Mono root domain as a managed thread");
        }

        core::LogInfo("mono domain at {:#x}", Raw(domain_));
        return true;
    }

    Address Domain() override { return domain_; }

    std::vector<Address> Assemblies() override {
        std::vector<Address> out;
        if (IsNull(runtime_.api.domain_assembly_foreach)) return out;
        At<FnForeach>(runtime_.api.domain_assembly_foreach)(AsPtr(domain_), &CollectAssembly,
                                                           &out);
        return out;
    }

    Address AssemblyImage(Address assembly) override {
        if (IsNull(assembly)) return {};
        return FromPtr(At<FnPtrToPtr>(runtime_.api.assembly_get_image)(AsPtr(assembly)));
    }

    AssemblyFacts Assembly(Address assembly) override {
        AssemblyFacts facts;
        if (IsNull(assembly)) return facts;

        if (!IsNull(runtime_.api.assembly_get_name)) {
            const auto name = FromPtr(
                At<FnPtrToPtr>(runtime_.api.assembly_get_name)(AsPtr(assembly)));
            if (!IsNull(name)) {
                if (!IsNull(runtime_.api.assembly_name_get_name))
                    facts.name = Text(runtime_.api.assembly_name_get_name, name);
                if (!IsNull(runtime_.api.assembly_name_get_version)) {
                    std::uint16_t major = 0, minor = 0, build = 0, revision = 0;
                    At<FnVersion>(runtime_.api.assembly_name_get_version)(
                        AsPtr(name), &major, &minor, &build, &revision);
                    facts.version = std::format("{}.{}.{}.{}", major, minor, build, revision);
                }
            }
        }

        const auto image = AssemblyImage(assembly);
        if (!IsNull(image) && !IsNull(runtime_.api.image_get_filename))
            facts.file = Text(runtime_.api.image_get_filename, image);
        if (facts.name.empty() && !IsNull(image)) facts.name = ImageName(image);
        return facts;
    }

    std::string ImageName(Address image) override {
        if (IsNull(image)) return {};
        return Text(runtime_.api.image_get_name, image);
    }

    std::uint32_t ImageTypeCount(Address image) override {
        if (IsNull(image)) return 0;
        const auto rows = At<FnImageRows>(runtime_.api.image_get_table_rows)(
            AsPtr(image), static_cast<std::int32_t>(kTypeDefTable));
        if (rows <= 0) return 0;
        const auto count = static_cast<std::uint32_t>(rows);
        return count > kMaxImageTypes ? kMaxImageTypes : count;
    }

    Address ImageType(Address image, std::uint32_t index) override {
        if (IsNull(image)) return {};
        const std::uint32_t token = kTypeDefToken | (index + 1);
        return FromPtr(At<FnClassGet>(runtime_.api.class_get)(AsPtr(image), token));
    }

    ClassFacts Class(Address klass) override {
        ClassFacts facts;
        if (IsNull(klass)) return facts;

        At<FnPtrToPtr>(runtime_.api.class_init)(AsPtr(klass));

        facts.name       = Text(runtime_.api.class_get_name, klass);
        facts.name_space = Text(runtime_.api.class_get_namespace, klass);
        facts.parent     = Call(runtime_.api.class_get_parent, klass);
        facts.flags      = Number(runtime_.api.class_get_flags, klass);

        facts.is_valuetype = Flag(runtime_.api.class_is_valuetype, klass);
        facts.is_enum      = Flag(runtime_.api.class_is_enum, klass);
        facts.is_delegate  = Flag(runtime_.api.class_is_delegate, klass);
        facts.is_blittable = Flag(runtime_.api.class_is_blittable, klass);

        facts.instance_size = At<FnPtrToI32>(runtime_.api.class_instance_size)(AsPtr(klass));

        if (!IsNull(runtime_.api.class_get_nesting_type))
            facts.declaring = Call(runtime_.api.class_get_nesting_type, klass);
        if (!IsNull(runtime_.api.class_get_image))
            facts.image = Call(runtime_.api.class_get_image, klass);
        if (!IsNull(runtime_.api.class_get_type_token))
            facts.token = Number(runtime_.api.class_get_type_token, klass);
        if (!IsNull(runtime_.api.class_min_align))
            facts.alignment = At<FnPtrToI32>(runtime_.api.class_min_align)(AsPtr(klass));

        if (facts.is_valuetype && !IsNull(runtime_.api.class_value_size)) {
            std::uint32_t align = 0;
            facts.value_size =
                At<FnValueSize>(runtime_.api.class_value_size)(AsPtr(klass), &align);
        }
        return facts;
    }

    std::vector<Address> Fields(Address klass) override {
        return Iterate(runtime_.api.class_get_fields, klass, ReportedFieldCount(klass));
    }

    std::vector<Address> Methods(Address klass) override {
        return Iterate(runtime_.api.class_get_methods, klass, kFieldCountUnknown);
    }

    std::vector<Address> Properties(Address klass) override {
        return Iterate(runtime_.api.class_get_properties, klass, kFieldCountUnknown);
    }

    std::vector<Address> Interfaces(Address klass) override {
        return Iterate(runtime_.api.class_get_interfaces, klass, kFieldCountUnknown);
    }

    std::vector<Address> NestedTypes(Address klass) override {
        return Iterate(runtime_.api.class_get_nested_types, klass, kFieldCountUnknown);
    }

    FieldFacts Field(Address field) override {
        FieldFacts facts;
        if (IsNull(field)) return facts;
        facts.name  = Text(runtime_.api.field_get_name, field);
        facts.type  = Call(runtime_.api.field_get_type, field);
        facts.flags = Number(runtime_.api.field_get_flags, field);
        facts.offset = At<FnPtrToI32>(runtime_.api.field_get_offset)(AsPtr(field));
        return facts;
    }

    MethodFacts Method(Address method) override {
        MethodFacts facts;
        if (IsNull(method)) return facts;

        facts.name = Text(runtime_.api.method_get_name, method);

        std::uint32_t iflags = 0;
        facts.flags = At<FnMethodFlags>(runtime_.api.method_get_flags)(AsPtr(method), &iflags);

        if (!IsNull(runtime_.api.method_get_token))
            facts.token = Number(runtime_.api.method_get_token, method);

        return facts;
    }

    std::vector<Address> MethodParams(Address method) override {
        std::vector<Address> out;
        if (IsNull(method)) return out;
        const auto signature = Call(runtime_.api.method_signature, method);
        if (IsNull(signature)) return out;

        void* iter = nullptr;
        for (std::size_t i = 0; i < kMaxMembers; ++i) {
            void* next = At<FnIterate>(runtime_.api.signature_get_params)(AsPtr(signature),
                                                                         &iter);
            if (next == nullptr) break;
            out.push_back(FromPtr(next));
        }
        return out;
    }

    PropertyFacts Property(Address property) override {
        PropertyFacts facts;
        if (IsNull(property) || IsNull(runtime_.api.property_get_name)) return facts;
        facts.name = Text(runtime_.api.property_get_name, property);
        if (!IsNull(runtime_.api.property_get_get_method))
            facts.getter = Call(runtime_.api.property_get_get_method, property);
        if (!IsNull(runtime_.api.property_get_set_method))
            facts.setter = Call(runtime_.api.property_get_set_method, property);
        if (!IsNull(runtime_.api.property_get_flags))
            facts.flags = Number(runtime_.api.property_get_flags, property);
        return facts;
    }

    TypeFacts Type(Address type) override {
        TypeFacts facts;
        if (IsNull(type)) return facts;

        facts.element = static_cast<ElementType>(
            At<FnPtrToI32>(runtime_.api.type_get_type)(AsPtr(type)) & 0xFF);

        if (!IsNull(runtime_.api.type_is_byref))
            facts.by_ref = Flag(runtime_.api.type_is_byref, type);

        switch (facts.element) {
            case ElementType::Class:
            case ElementType::Object:
            case ElementType::ValueType:
                facts.klass = Call(runtime_.api.type_get_class, type);
                break;

            case ElementType::GenericInst:
                facts.klass = Call(runtime_.api.class_from_mono_type, type);
                break;

            case ElementType::SzArray:
            case ElementType::Array: {
                const auto array_class = Call(runtime_.api.class_from_mono_type, type);
                facts.klass = Call(runtime_.api.class_get_element_class, array_class);
                break;
            }

            default:
                break;
        }

        return facts;
    }

    core::Address ModuleBase() const override { return runtime_.module_base; }
    std::uint64_t ModuleSize() const override { return runtime_.module_size; }

    std::vector<std::string> Evidence() const override {
        std::vector<std::string> all = runtime_.evidence;
        all.insert(all.end(), evidence_.begin(), evidence_.end());
        return all;
    }

private:
    std::string Text(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return {};
        const char* text = At<FnPtrToStr>(entry)(AsPtr(argument));
        if (text == nullptr) return {};
        return std::string(text);
    }

    std::string Owned(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return {};
        char* text = At<FnPtrToCharP>(entry)(AsPtr(argument));
        if (text == nullptr) return {};
        std::string out(text);
        if (!IsNull(runtime_.api.free)) At<FnFree>(runtime_.api.free)(text);
        return out;
    }

    Address Call(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return {};
        return FromPtr(At<FnPtrToPtr>(entry)(AsPtr(argument)));
    }

    std::uint32_t Number(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return 0;
        return At<FnPtrToU32>(entry)(AsPtr(argument));
    }

    bool Flag(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return false;
        return At<FnPtrToI32>(entry)(AsPtr(argument)) != 0;
    }

    std::int64_t ReportedFieldCount(Address klass) {
        if (IsNull(runtime_.api.class_num_fields) || IsNull(klass)) return kFieldCountUnknown;
        return static_cast<std::int64_t>(
            At<FnPtrToI32>(runtime_.api.class_num_fields)(AsPtr(klass)));
    }

    std::vector<Address> Iterate(Address entry, Address klass, std::int64_t reported) {
        std::vector<Address> out;
        if (IsNull(entry) || IsNull(klass)) return out;

        const std::size_t limit = FieldIterationLimit(reported, kMaxMembers);
        void* iter = nullptr;
        for (std::size_t i = 0; i < limit; ++i) {
            void* next = At<FnIterate>(entry)(AsPtr(klass), &iter);
            if (next == nullptr) break;
            out.push_back(FromPtr(next));
        }
        return out;
    }

    const RuntimeInfo&   runtime_;
    core::IMemorySource& memory_;
    Address              domain_{};
    std::vector<std::string> evidence_;
};

} // namespace

std::string_view ToString(ElementType type) {
    switch (type) {
        case ElementType::End:         return "end";
        case ElementType::Void:        return "void";
        case ElementType::Boolean:     return "bool";
        case ElementType::Char:        return "char";
        case ElementType::I1:          return "sbyte";
        case ElementType::U1:          return "byte";
        case ElementType::I2:          return "short";
        case ElementType::U2:          return "ushort";
        case ElementType::I4:          return "int";
        case ElementType::U4:          return "uint";
        case ElementType::I8:          return "long";
        case ElementType::U8:          return "ulong";
        case ElementType::R4:          return "float";
        case ElementType::R8:          return "double";
        case ElementType::String:      return "string";
        case ElementType::Ptr:         return "pointer";
        case ElementType::ByRef:       return "byref";
        case ElementType::ValueType:   return "valuetype";
        case ElementType::Class:       return "class";
        case ElementType::Var:         return "generic-parameter";
        case ElementType::Array:       return "array";
        case ElementType::GenericInst: return "generic-instance";
        case ElementType::TypedByRef:  return "typedbyref";
        case ElementType::I:           return "intptr";
        case ElementType::U:           return "uintptr";
        case ElementType::FnPtr:       return "function-pointer";
        case ElementType::Object:      return "object";
        case ElementType::SzArray:     return "vector";
        case ElementType::MVar:        return "method-generic-parameter";
        case ElementType::CModReqd:    return "modreq";
        case ElementType::CModOpt:     return "modopt";
        case ElementType::Internal:    return "internal";
    }
    return "unknown";
}

core::Result<std::unique_ptr<IBridge>> MakeInProcessBridge(const RuntimeInfo& runtime,
                                                           core::IMemorySource& memory) {
    if (!runtime.Valid())
        return core::Error{"the Mono runtime is not usable: required entry points are missing "
                           "from this build", 3};

    auto bridge = std::make_unique<InProcessBridge>(runtime, memory);
    if (!bridge->Attach())
        return core::Error{"could not attach to the Mono root domain", 4};

    return std::unique_ptr<IBridge>(std::move(bridge));
}

} // namespace zircon::mono
