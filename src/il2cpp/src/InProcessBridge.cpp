// Mode 1: answer the walk by calling the embedding API from inside the process.
//
// The only provider that can be complete. il2cpp_field_get_offset is a function; reading
// memory won't run it. A dumper that won't inject either rebuilds Il2CppClass by hand (the
// version-table trap again) or goes without field offsets.
//
// Nothing here writes to the game. The calls do mutate runtime state -- asking a class for
// its fields makes the runtime lay it out if it hasn't yet -- but that's the runtime's own
// bookkeeping under its own locks, same as when the game first touches the class.

#include "il2cpp/Bridge.h"
#include "il2cpp/MethodLayout.h"

#include "core/Log.h"

#include <algorithm>
#include <format>
#include <cstring>
#include <unordered_set>

namespace zircon::il2cpp {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// More members than this is a runaway iterator, not a class. Widest type in the corpus has a
// few thousand methods.
constexpr std::size_t kMaxMembers = 65536;

// Upper bound on the whole class cache (the inflated-generic sweep). Largest game we've seen
// is well under 100k.
constexpr std::size_t kMaxClasses = 2u * 1000 * 1000;

// Sample size for the MethodInfo derivation. Enough that "varies within one arg shape" is a
// measurement and not a coincidence, small enough to be free.
constexpr std::size_t kLayoutSample  = 1024;
constexpr std::size_t kLayoutClasses = 64;

using FnVoidPtr        = void* (*)();
using FnDomainAssemblies = void** (*)(void*, std::size_t*);
using FnPtrToPtr       = void* (*)(void*);
using FnPtrToStr       = const char* (*)(void*);
using FnPtrToInt       = int (*)(void*);

// The il2cpp_class_is_* calls return bool, not int, and on x64 a bool return only commits
// the low byte of the register. The rest is whatever the callee left there. Called through
// an int signature they read as true roughly whenever that garbage is non-zero, which is why
// UnityEngine.Vector3 came out of the walk marked interface AND abstract AND valuetype at
// once. Nothing downstream could have caught that: all three are legal on their own.
using FnPtrToBool      = bool (*)(void*);
using FnPtrToU32       = std::uint32_t (*)(void*);
using FnPtrToSize      = std::size_t (*)(void*);
using FnPtrIndex       = void* (*)(void*, std::size_t);
using FnIterate        = void* (*)(void*, void**);
using FnMethodParam    = void* (*)(void*, std::uint32_t);
using FnMethodParamName= const char* (*)(void*, std::uint32_t);
using FnMethodFlags    = std::uint32_t (*)(void*, std::uint32_t*);
using FnHeaderSize     = std::size_t (*)();
using FnFieldStaticGet = void (*)(void*, void*);

template <typename Fn>
Fn At(Address address) {
    return reinterpret_cast<Fn>(static_cast<std::uintptr_t>(Raw(address)));
}

void* AsPtr(Address address) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(Raw(address)));
}

Address AsAddress(const void* pointer) {
    return static_cast<Address>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::string Copy(const char* text) {
    return text ? std::string(text) : std::string();
}

class InProcessBridge final : public IBridge {
public:
    InProcessBridge(const RuntimeInfo& runtime, core::IMemorySource& memory, bool consts)
        : runtime_(runtime), memory_(memory), consts_(consts) {}

    bool Attach() override {
        domain_ = At<FnVoidPtr>(runtime_.api.domain_get)();
        if (!domain_) {
            core::LogError("il2cpp_domain_get returned nothing; the runtime is not up yet");
            return false;
        }

        // Join the GC's thread list. Touching runtime structures from a thread the collector
        // doesn't know about takes the game down later, at some arbitrary point.
        //
        // Result checked, not discarded -- a thread that failed to attach reads exactly like
        // one that did, until it doesn't.
        void* thread = At<FnPtrToPtr>(runtime_.api.thread_attach)(domain_);
        if (!thread) {
            core::LogError("il2cpp_thread_attach refused this thread; walking the runtime "
                           "from it would be reading structures the collector may move");
            return false;
        }
        core::LogInfo("attached to the IL2CPP domain as thread {:#x}",
                      reinterpret_cast<std::uintptr_t>(thread));

        header_size_ = -1;
        if (!IsNull(runtime_.api.object_header_size))
            header_size_ = static_cast<std::int32_t>(At<FnHeaderSize>(runtime_.api.object_header_size)());
        if (header_size_ <= 0) {
            // The one number we could assume and mustn't. Without it, value-type offsets are
            // reported as the runtime gives them and the unboxed column stays unresolved
            // instead of getting a 0x10 guess.
            evidence_.emplace_back(
                "this build does not export il2cpp_object_header_size, so value-type field "
                "offsets are recorded as the runtime reports them and not adjusted");
            header_size_ = -1;
        } else {
            evidence_.push_back(std::format(
                "object header is {} bytes, from the runtime rather than assumed", header_size_));
        }

        if (!IsNull(runtime_.api.class_num_fields))
            evidence_.emplace_back("field iteration is bounded by il2cpp_class_num_fields, so "
                                   "the call that terminates it is never made");
        else
            evidence_.emplace_back("this build does not export il2cpp_class_num_fields, so the "
                                   "field walk iterates until the runtime returns null");

        for (const auto& [name, rva] : ResolvedEntryPoints(runtime_))
            entry_points_.push_back(std::format("{}=0x{:x}", name, rva));

        DeriveMethodBodies();
        return true;
    }

    std::int32_t ObjectHeaderSize() const override { return header_size_; }

    Address Domain() override { return AsAddress(domain_); }

    std::vector<Address> Assemblies() override {
        std::vector<Address> out;
        std::size_t count = 0;
        void** list = At<FnDomainAssemblies>(runtime_.api.domain_get_assemblies)(domain_, &count);
        if (!list) return out;
        out.reserve(count);
        for (std::size_t i = 0; i < count && i < kMaxMembers; ++i)
            if (list[i]) out.push_back(AsAddress(list[i]));
        return out;
    }

    Address AssemblyImage(Address assembly) override {
        return Call(runtime_.api.assembly_get_image, assembly);
    }

    std::vector<Address> AllClasses() override {
        if (IsNull(runtime_.api.class_for_each)) return {};

        // Plain C callback with a user-data arg; the vector rides in that, not a global.
        using FnReport  = void (*)(void*, void*);
        using FnForEach = void (*)(FnReport, void*);

        std::vector<Address> out;
        At<FnForEach>(runtime_.api.class_for_each)(
            [](void* klass, void* user) {
                auto* into = static_cast<std::vector<Address>*>(user);
                if (klass && into->size() < kMaxClasses) into->push_back(AsAddress(klass));
            },
            &out);
        return out;
    }

    std::string ImageName(Address image) override {
        return Text(runtime_.api.image_get_name, image);
    }

    std::size_t ImageClassCount(Address image) override {
        if (IsNull(image)) return 0;
        return At<FnPtrToSize>(runtime_.api.image_get_class_count)(AsPtr(image));
    }

    Address ImageClass(Address image, std::size_t index) override {
        if (IsNull(image)) return {};
        return AsAddress(At<FnPtrIndex>(runtime_.api.image_get_class)(AsPtr(image), index));
    }

    ClassFacts Class(Address klass) override {
        ClassFacts facts;
        if (IsNull(klass)) return facts;
        void* k = AsPtr(klass);

        // Fields first, on purpose: it's what makes the runtime lay the class out, and
        // instance_size is whatever it was (often 0) until then.
        void* iter = nullptr;
        At<FnIterate>(runtime_.api.class_get_fields)(k, &iter);

        facts.name          = Text(runtime_.api.class_get_name, klass);
        facts.name_space    = Text(runtime_.api.class_get_namespace, klass);
        facts.parent        = Call(runtime_.api.class_get_parent, klass);
        facts.declaring     = Call(runtime_.api.class_get_declaring_type, klass);
        facts.flags         = static_cast<std::uint32_t>(
            At<FnPtrToInt>(runtime_.api.class_get_flags)(k));
        facts.instance_size = static_cast<std::int32_t>(
            At<FnPtrToU32>(runtime_.api.class_instance_size)(k));
        facts.is_valuetype  = At<FnPtrToBool>(runtime_.api.class_is_valuetype)(k);
        facts.is_enum       = At<FnPtrToBool>(runtime_.api.class_is_enum)(k);

        facts.image       = Call(runtime_.api.class_get_image, klass);
        facts.token       = Number(runtime_.api.class_get_type_token, klass);
        facts.rank        = static_cast<std::int32_t>(Number(runtime_.api.class_get_rank, klass));
        facts.is_interface= Flag(runtime_.api.class_is_interface, klass);
        facts.is_abstract = Flag(runtime_.api.class_is_abstract, klass);
        facts.is_generic  = Flag(runtime_.api.class_is_generic, klass);
        facts.is_inflated = Flag(runtime_.api.class_is_inflated, klass);

        if (facts.is_valuetype && !IsNull(runtime_.api.class_value_size)) {
            // out-param is alignment, which we don't need; null is documented as fine.
            using FnValueSize = std::int32_t (*)(void*, std::uint32_t*);
            facts.value_size = At<FnValueSize>(runtime_.api.class_value_size)(k, nullptr);
        }
        return facts;
    }

    std::vector<Address> Fields(Address klass) override {
        return Iterate(runtime_.api.class_get_fields, klass,
                       FieldIterationLimit(ReportedFieldCount(klass), kMaxMembers));
    }
    std::vector<Address> Methods(Address klass) override {
        return Iterate(runtime_.api.class_get_methods, klass);
    }
    std::vector<Address> Properties(Address klass) override {
        return Iterate(runtime_.api.class_get_properties, klass);
    }
    std::vector<Address> NestedTypes(Address klass) override {
        return Iterate(runtime_.api.class_get_nested_types, klass);
    }
    std::vector<Address> Interfaces(Address klass) override {
        return Iterate(runtime_.api.class_get_interfaces, klass);
    }

    Address ClassType(Address klass) override {
        return Call(runtime_.api.class_get_type, klass);
    }
    Address EnumBaseType(Address klass) override {
        return Call(runtime_.api.class_enum_basetype, klass);
    }

    FieldFacts Field(Address field) override {
        FieldFacts facts;
        if (IsNull(field)) return facts;
        facts.name  = Text(runtime_.api.field_get_name, field);
        facts.type  = Call(runtime_.api.field_get_type, field);
        facts.flags = static_cast<std::uint32_t>(
            At<FnPtrToInt>(runtime_.api.field_get_flags)(AsPtr(field)));
        facts.offset = static_cast<std::int32_t>(
            At<FnPtrToSize>(runtime_.api.field_get_offset)(AsPtr(field)));
        facts.is_literal = Flag(runtime_.api.field_is_literal, field);
        return facts;
    }

    MethodFacts Method(Address method) override {
        MethodFacts facts;
        if (IsNull(method)) return facts;
        void* m = AsPtr(method);

        facts.name        = Text(runtime_.api.method_get_name, method);
        facts.return_type = Call(runtime_.api.method_get_return_type, method);
        facts.param_count = At<FnPtrToU32>(runtime_.api.method_get_param_count)(m);
        std::uint32_t iflags = 0;
        facts.flags       = At<FnMethodFlags>(runtime_.api.method_get_flags)(m, &iflags);
        facts.token       = Number(runtime_.api.method_get_token, method);
        facts.is_generic  = Flag(runtime_.api.method_is_generic, method);
        facts.is_inflated = Flag(runtime_.api.method_is_inflated, method);
        facts.is_instance = IsNull(runtime_.api.method_is_instance)
                                ? true
                                : Flag(runtime_.api.method_is_instance, method);

        if (layout_.Valid()) {
            const auto body = core::ReadOr<std::uint64_t>(
                memory_, method + static_cast<std::uint64_t>(layout_.body));
            facts.body = static_cast<Address>(body);
        }
        return facts;
    }

    PropertyFacts Property(Address property) override {
        PropertyFacts facts;
        if (IsNull(property)) return facts;
        facts.name   = Text(runtime_.api.property_get_name, property);
        facts.getter = Call(runtime_.api.property_get_get_method, property);
        facts.setter = Call(runtime_.api.property_get_set_method, property);
        facts.flags  = Number(runtime_.api.property_get_flags, property);
        return facts;
    }

    TypeFacts Type(Address type) override {
        TypeFacts facts;
        if (IsNull(type)) return facts;
        void* t = AsPtr(type);

        // Element kind first, because for one kind it's the only safe question. A generic
        // parameter (the T in List<T>) isn't a type: no class behind it, no name outside its
        // declaration. Older runtimes don't return nonsense when asked anyway -- they walk
        // into a null and take the process with them. That's how this was found.
        facts.element = static_cast<ElementType>(
            At<FnPtrToInt>(runtime_.api.type_get_type)(t) & 0xFF);
        if (IsGenericParameter(facts.element)) return facts;

        facts.name    = TextOwned(runtime_.api.type_get_name, type);
        if (NamesAClass(facts.element))
            facts.klass = Call(runtime_.api.type_get_class_or_element_class, type);
        facts.by_ref  = Flag(runtime_.api.type_is_byref, type);
        facts.is_pointer = Flag(runtime_.api.type_is_pointer_type, type);
        facts.attrs   = Number(runtime_.api.type_get_attrs, type);
        return facts;
    }

    std::string ParamName(Address method, std::uint32_t index) override {
        if (IsNull(method)) return {};
        return Copy(At<FnMethodParamName>(runtime_.api.method_get_param_name)(AsPtr(method),
                                                                             index));
    }

    Address ParamType(Address method, std::uint32_t index) override {
        if (IsNull(method)) return {};
        return AsAddress(At<FnMethodParam>(runtime_.api.method_get_param)(AsPtr(method), index));
    }

    // Read the static block ourselves, bounds-checked, instead of calling
    // il2cpp_field_static_get_value.
    //
    // The call is the obvious way and it's how this started. A const has no storage, so on
    // some builds the call walks off the end of the block and faults. Wrapping it in
    // __try/__except is worse than the fault: the runtime takes a lock on the way in,
    // unwinding leaves it held, and the process dies later somewhere unrelated. Which is
    // what happened. Don't SEH-guard calls into someone else's runtime.
    //
    // So: ask where the block is and how big, check the field fits, read the bytes. The
    // read is guarded in the memory provider, around a memcpy that holds no locks.
    bool LiteralValue(Address field, void* out, std::size_t size) override {
        if (IsNull(field) || size == 0 || size > sizeof(std::uint64_t)) return false;
        if (!consts_) return false;
        if (IsNull(runtime_.api.field_get_parent) ||
            IsNull(runtime_.api.class_get_static_field_data) ||
            IsNull(runtime_.api.class_get_data_size))
            return AskRuntimeForLiteral(field, out, size);

        const Address klass = Call(runtime_.api.field_get_parent, field);
        if (IsNull(klass)) return false;

        const Address block = Call(runtime_.api.class_get_static_field_data, klass);
        if (IsNull(block)) return AskRuntimeForLiteral(field, out, size);

        const auto capacity = Number(runtime_.api.class_get_data_size, klass);
        const auto offset   = static_cast<std::int64_t>(
            At<FnPtrToSize>(runtime_.api.field_get_offset)(AsPtr(field)));
        if (offset < 0 || static_cast<std::uint64_t>(offset) + size > capacity) return false;

        return memory_.Read(block + static_cast<std::uint64_t>(offset), out, size) == size;
    }

    // Fallback when the read above can't serve. On most builds a const isn't in the static
    // block at all, it's in the metadata, and only the runtime can get at that.
    //
    // Unguarded on purpose -- see above. A lock held across an unwind kills the process
    // later with nothing in the log to connect the two.
    bool AskRuntimeForLiteral(Address field, void* out, std::size_t size) {
        if (IsNull(runtime_.api.field_static_get_value)) return false;

        std::uint64_t scratch = 0;
        At<FnFieldStaticGet>(runtime_.api.field_static_get_value)(AsPtr(field), &scratch);
        std::memcpy(out, &scratch, size);
        return true;
    }

    Address       ModuleBase() const override { return runtime_.module_base; }
    std::uint64_t ModuleSize() const override { return runtime_.module_size; }
    std::vector<std::string> Evidence() const override { return evidence_; }

    std::vector<std::pair<std::string, std::int32_t>> Derived() const override {
        std::vector<std::pair<std::string, std::int32_t>> out;
        if (header_size_ > 0) out.emplace_back("Il2CppObject.header", header_size_);
        if (!layout_.Valid()) return out;
        out.emplace_back("MethodInfo.body", layout_.body);
        if (layout_.virtual_body >= 0) out.emplace_back("MethodInfo.virtualBody", layout_.virtual_body);
        out.emplace_back("MethodInfo.invoker", layout_.invoker);
        out.emplace_back("MethodInfo.name", layout_.name);
        if (layout_.klass >= 0)       out.emplace_back("MethodInfo.klass", layout_.klass);
        if (layout_.return_type >= 0) out.emplace_back("MethodInfo.returnType", layout_.return_type);
        if (layout_.token >= 0)       out.emplace_back("MethodInfo.token", layout_.token);
        return out;
    }

    std::vector<std::string> EntryPoints() const override { return entry_points_; }

private:
    Address Call(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return {};
        return AsAddress(At<FnPtrToPtr>(entry)(AsPtr(argument)));
    }

    // For the two getters that return memory we own. Everything else returns a pointer into
    // the metadata and must not be freed, so the call site decides.
    std::string TextOwned(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return {};
        const char* text = At<FnPtrToStr>(entry)(AsPtr(argument));
        if (!text) return {};
        std::string copy(text);
        if (!IsNull(runtime_.api.free))
            At<void (*)(void*)>(runtime_.api.free)(const_cast<char*>(text));
        return copy;
    }

    std::string Text(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return {};
        return Copy(At<FnPtrToStr>(entry)(AsPtr(argument)));
    }

    // Optional entry points, null-checked here so call sites don't have to. Absent means
    // the neutral value, never a stand-in that reads as real.
    bool Flag(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return false;
        return At<FnPtrToBool>(entry)(AsPtr(argument));
    }

    std::uint32_t Number(Address entry, Address argument) {
        if (IsNull(entry) || IsNull(argument)) return 0;
        return At<FnPtrToU32>(entry)(AsPtr(argument));
    }

    // What the runtime says a class's field count is. Optional entry point, so plenty of
    // builds won't answer; FieldIterationLimit handles that.
    std::int64_t ReportedFieldCount(Address klass) {
        if (IsNull(runtime_.api.class_num_fields) || IsNull(klass)) return kFieldCountUnknown;
        return static_cast<std::int64_t>(
            At<FnPtrToSize>(runtime_.api.class_num_fields)(AsPtr(klass)));
    }

    // limit is a ceiling, not a promise. A null still ends the loop, so a wrong count can
    // only make us stop early, never read past the end.
    std::vector<Address> Iterate(Address entry, Address klass, std::size_t limit = kMaxMembers) {
        std::vector<Address> out;
        if (IsNull(entry) || IsNull(klass) || limit == 0) return out;
        if (limit > kMaxMembers) limit = kMaxMembers;

        auto fn = At<FnIterate>(entry);
        void* iter = nullptr;
        while (out.size() < limit) {
            void* item = fn(AsPtr(klass), &iter);
            if (!item) break;
            out.push_back(AsAddress(item));
        }
        return out;
    }

    // Collects a spread of methods for the derivation. Spread matters -- from one class every
    // klass pointer is identical and any constant slot would anchor as well as the real one.
    void DeriveMethodBodies() {
        std::vector<MethodProbe> sample;
        std::size_t classes_used = 0;

        for (const Address assembly : Assemblies()) {
            const Address image = AssemblyImage(assembly);
            const std::size_t count = ImageClassCount(image);

            for (std::size_t i = 0; i < count && classes_used < kLayoutClasses; ++i) {
                const Address klass = ImageClass(image, i);
                if (IsNull(klass)) continue;

                const auto methods = Methods(klass);
                if (methods.empty()) continue;
                ++classes_used;

                for (const Address method : methods) {
                    void* m = AsPtr(method);
                    MethodProbe probe;
                    probe.method      = method;
                    probe.name        = AsAddress(
                        At<FnPtrToStr>(runtime_.api.method_get_name)(m));
                    probe.klass       = IsNull(runtime_.api.method_get_class)
                                            ? klass : Call(runtime_.api.method_get_class, method);
                    probe.return_type = Call(runtime_.api.method_get_return_type, method);
                    probe.param_count = At<FnPtrToU32>(runtime_.api.method_get_param_count)(m);
                    probe.token       = Number(runtime_.api.method_get_token, method);
                    probe.is_instance = IsNull(runtime_.api.method_is_instance)
                                            ? true : Flag(runtime_.api.method_is_instance, method);
                    sample.push_back(probe);
                    if (sample.size() >= kLayoutSample) break;
                }
                if (sample.size() >= kLayoutSample) break;
            }
            if (sample.size() >= kLayoutSample) break;
        }

        const auto starts = ReadFunctionStarts(memory_, runtime_.module_base);

        // Only this module. ExecutableRanges reports every loaded DLL.
        std::vector<core::RegionInfo> executable;
        for (const auto& region : core::ExecutableRanges(memory_)) {
            if (Raw(region.base) < Raw(runtime_.module_base)) continue;
            if (Raw(region.base) >= Raw(runtime_.module_base) + runtime_.module_size) continue;
            executable.push_back(region);
        }

        core::LogInfo("{} at {:#x}, {} bytes; {} executable section(s), {} function entry "
                      "points in .pdata", runtime_.module_name, Raw(runtime_.module_base),
                      runtime_.module_size, executable.size(), starts.size());
        layout_ = DeriveMethodInfoLayout(memory_, sample, runtime_.module_base,
                                         runtime_.module_size, starts, executable);

        evidence_.push_back(std::format(
            "{} function entry points in the exception directory, used to check that a slot "
            "holds code and not merely an address inside the module", starts.size()));

        if (layout_.Valid()) {
            for (const auto& line : layout_.evidence) evidence_.push_back(line);
            core::LogInfo("MethodInfo: body at {:#x}, invoker at {:#x}, name at {:#x}",
                          layout_.body, layout_.invoker, layout_.name);
        } else {
            evidence_.push_back("method bodies are unresolved: " + layout_.refusal);
            for (const auto& line : layout_.evidence) evidence_.push_back(line);
            core::LogWarn("could not place the method body in MethodInfo: {}", layout_.refusal);
            for (const auto& line : layout_.evidence) core::LogWarn("  {}", line);
            core::LogWarn("the dump will carry no RVAs rather than wrong ones");
        }
    }

    RuntimeInfo           runtime_;
    core::IMemorySource&  memory_;
    void*                 domain_{nullptr};
    std::int32_t          header_size_{-1};
    MethodInfoLayout      layout_;
    bool                  consts_{true};
    std::vector<std::string> evidence_;
    std::vector<std::string> entry_points_;
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
        case ElementType::Var:         return "typeparam";
        case ElementType::Array:       return "array";
        case ElementType::GenericInst: return "genericinst";
        case ElementType::TypedByRef:  return "typedbyref";
        case ElementType::I:           return "nint";
        case ElementType::U:           return "nuint";
        case ElementType::FnPtr:       return "fnptr";
        case ElementType::Object:      return "object";
        case ElementType::SzArray:     return "szarray";
        case ElementType::MVar:        return "methodparam";
        case ElementType::CModReqd:    return "modreq";
        case ElementType::CModOpt:     return "modopt";
        case ElementType::Internal:    return "internal";
    }
    return "unknown";
}

core::Result<std::unique_ptr<IBridge>> MakeInProcessBridge(const RuntimeInfo& runtime,
                                                           core::IMemorySource& memory,
                                                           bool read_consts) {
    if (!runtime.Valid())
        return core::Error{"the runtime's required entry points are not all resolved", 3};

    // Every answer is a call into the target. A source that can't call isn't one step short
    // of working; it's a different mode that isn't written yet.
    if (!memory.Caps().can_call) {
        return core::Error{"walking an IL2CPP runtime means calling into it, which only "
                           "works from inside the process; inject the payload instead", 2};
    }

    auto bridge = std::make_unique<InProcessBridge>(runtime, memory, read_consts);
    if (!bridge->Attach())
        return core::Error{"could not attach to the IL2CPP domain", 4};

    return std::unique_ptr<IBridge>(std::move(bridge));
}

} // namespace zircon::il2cpp
