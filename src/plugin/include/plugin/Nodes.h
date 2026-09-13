#pragma once

// The read side of the plugin ABI: one table per ZnKind describing how to get each field
// off the corresponding IR type. See Nodes.cpp for the tables themselves.

#include "emit/Emitter.h"
#include "ir/Model.h"
#include "zircon/plugin.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zircon::plugin {

using Options = emit::EmitOptions;

// One field of one node kind. Exactly one of the readers is set, matching `type`.
//
// Every string_view a reader returns must point at NUL-terminated storage: the ABI hands
// plugins a `const char*`, and a C plugin will treat it as a C string. std::string and
// string literals both satisfy that; a view into the middle of a buffer would not.
struct Field {
    std::string_view name;
    ZnFieldType      type;

    std::string_view (*as_str)  (const void*)                = nullptr;
    std::int64_t     (*as_i64)  (const void*)                = nullptr;
    double           (*as_f64)  (const void*)                = nullptr;
    bool             (*as_bool) (const void*)                = nullptr;
    ZnNode           (*as_node) (const void*)                = nullptr;
    std::size_t      (*list_len)(const void*)                = nullptr;
    ZnNode           (*list_at) (const void*, std::size_t)   = nullptr;
    std::string_view (*list_str)(const void*, std::size_t)   = nullptr;
};

const Field* FindField(std::uint32_t kind, const char* name);

const char* KindName(std::uint32_t kind);
std::size_t FieldCount(std::uint32_t kind);
const char* FieldNameAt(std::uint32_t kind, std::size_t index);

ZnNode MakeNode(const void* object, ZnKind kind);

} // namespace zircon::plugin
