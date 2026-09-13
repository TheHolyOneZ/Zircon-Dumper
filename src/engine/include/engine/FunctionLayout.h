#pragma once

#include "core/MemorySource.h"
#include "engine/NamePool.h"
#include "engine/ObjectArray.h"
#include "engine/ObjectLayout.h"
#include "engine/PropertyLayout.h"
#include "engine/StructLayout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace zircon::engine {

// UFunction extends UStruct. Its parameters are its own ChildProperties, flagged as
// parameters, so once the property layout is known the signature falls out of machinery
// that already exists — only the two UFunction-specific members need deriving.
struct UFunctionLayout {
    int field_next{-1};        // UField::Next, needed to walk UStruct::Children
    int function_flags{-1};    // EFunctionFlags (uint32)
    int native_func{-1};       // FNativeFuncPtr Func

    float confidence{0.0f};
    std::vector<std::string> evidence;

    bool Valid() const { return field_next >= 0 && function_flags >= 0; }
};

UFunctionLayout DeriveFunctionLayout(core::IMemorySource& memory,
                                     const ObjectArrayInfo& array,
                                     const NamePoolInfo& pool,
                                     const UObjectLayout& object_layout,
                                     const UStructLayout& struct_layout,
                                     const FPropertyLayout& property_layout);

// Selected EPropertyFlags bits. Only the ones that change a signature's meaning are
// named here; the full flag word is carried through to the IR verbatim.
namespace property_flags {
inline constexpr std::uint64_t kParm        = 0x0000000000000080ull;
inline constexpr std::uint64_t kOutParm     = 0x0000000000000100ull;
inline constexpr std::uint64_t kConstParm   = 0x0000000000000200ull;
inline constexpr std::uint64_t kReturnParm  = 0x0000000000000400ull;
} // namespace property_flags

// Readable names for EPropertyFlags.
//
// A hardcoded table, which anywhere else in this project would be the wrong answer. There
// is simply nothing to derive: a bit's *name* exists only in the engine source, and no
// amount of staring at a running game recovers the word "BlueprintReadOnly" from bit 4.
// The numeric flags go into the IR verbatim regardless, so a build whose meanings differ
// loses nothing. The names are a convenience over data that is already exact.
//
// Two bits did change meaning across versions (0x1000 was Localized before 5.0, and
// 0x8000 before that), so those carry the modern reading and the raw word remains the
// authority. Anything unrecognised comes back as "Unknown(0x...)" instead of being
// dropped, so no set bit ever just disappears.
std::vector<std::string> DescribePropertyFlags(std::uint64_t flags);

namespace function_flags {
inline constexpr std::uint32_t kFinal     = 0x00000001;
inline constexpr std::uint32_t kNative    = 0x00000400;
inline constexpr std::uint32_t kEvent     = 0x00000800;
inline constexpr std::uint32_t kStatic    = 0x00002000;
inline constexpr std::uint32_t kPublic    = 0x00020000;
inline constexpr std::uint32_t kPrivate   = 0x00040000;
inline constexpr std::uint32_t kProtected = 0x00080000;
} // namespace function_flags

std::vector<std::string> DescribeFunctionFlags(std::uint32_t flags);

// Everything reachable through UStruct::Children whose class is "Function".
std::vector<core::Address> GetClassFunctions(core::IMemorySource& memory,
                                             const ObjectArrayInfo& array,
                                             const NamePoolInfo& pool,
                                             const UObjectLayout& object_layout,
                                             const UStructLayout& struct_layout,
                                             const UFunctionLayout& function_layout,
                                             core::Address klass);

std::uint32_t GetFunctionFlags(core::IMemorySource& memory, const UFunctionLayout& layout,
                               core::Address function);

// Absolute address of the native implementation, or null for a script-only function.
core::Address GetNativeFunc(core::IMemorySource& memory, const UFunctionLayout& layout,
                            core::Address function);

} // namespace zircon::engine
