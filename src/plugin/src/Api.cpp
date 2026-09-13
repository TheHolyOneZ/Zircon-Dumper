// The host side of the plugin ABI: the ZnApi vtable, and the emit context a plugin writes
// through.
//
// Defensive on purpose. A plugin is third-party code calling in with C types, so a null
// node, a misspelled field name or a path with ".." in it is an ordinary Tuesday. Every
// one of those produces a status code — never a crash, never a file outside where the user
// pointed the emit.

#include "plugin/Api.h"
#include "plugin/Nodes.h"

#include "core/Log.h"

#include <filesystem>
#include <fstream>

namespace zircon::plugin {
namespace {

const Field* Lookup(ZnNode node, const char* field, ZnFieldType want, int& status) {
    if (!node.obj || !field) { status = ZN_ERR_NULL; return nullptr; }

    const Field* found = FindField(node.kind, field);
    if (!found)              { status = ZN_ERR_NO_FIELD;   return nullptr; }
    if (found->type != want) { status = ZN_ERR_WRONG_TYPE; return nullptr; }

    status = ZN_OK;
    return found;
}

int ApiStr(ZnNode node, const char* field, const char** out, size_t* len) {
    if (!out) return ZN_ERR_NULL;

    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_STR, status);
    if (!f) return status;

    const std::string_view text = f->as_str(node.obj);
    *out = text.data();
    if (len) *len = text.size();
    return ZN_OK;
}

int ApiI64(ZnNode node, const char* field, int64_t* out) {
    if (!out) return ZN_ERR_NULL;
    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_I64, status);
    if (!f) return status;
    *out = f->as_i64(node.obj);
    return ZN_OK;
}

int ApiF64(ZnNode node, const char* field, double* out) {
    if (!out) return ZN_ERR_NULL;
    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_F64, status);
    if (!f) return status;
    *out = f->as_f64(node.obj);
    return ZN_OK;
}

int ApiBool(ZnNode node, const char* field, int* out) {
    if (!out) return ZN_ERR_NULL;
    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_BOOL, status);
    if (!f) return status;
    *out = f->as_bool(node.obj) ? 1 : 0;
    return ZN_OK;
}

int ApiSub(ZnNode node, const char* field, ZnNode* out) {
    if (!out) return ZN_ERR_NULL;
    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_NODE, status);
    if (!f) return status;
    *out = f->as_node(node.obj);
    return ZN_OK;
}

// The one call that takes either list flavour. A script iterating a node's fields
// shouldn't have to know which of the two it found.
int ApiLen(ZnNode node, const char* field, size_t* out) {
    if (!out) return ZN_ERR_NULL;
    if (!node.obj || !field) return ZN_ERR_NULL;

    const Field* f = FindField(node.kind, field);
    if (!f) return ZN_ERR_NO_FIELD;
    if (f->type != ZN_FIELD_LIST_NODE && f->type != ZN_FIELD_LIST_STR)
        return ZN_ERR_WRONG_TYPE;

    *out = f->list_len(node.obj);
    return ZN_OK;
}

int ApiAt(ZnNode node, const char* field, size_t index, ZnNode* out) {
    if (!out) return ZN_ERR_NULL;
    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_LIST_NODE, status);
    if (!f) return status;
    if (index >= f->list_len(node.obj)) return ZN_ERR_RANGE;
    *out = f->list_at(node.obj, index);
    return ZN_OK;
}

int ApiAtStr(ZnNode node, const char* field, size_t index, const char** out, size_t* len) {
    if (!out) return ZN_ERR_NULL;
    int status = ZN_OK;
    const Field* f = Lookup(node, field, ZN_FIELD_LIST_STR, status);
    if (!f) return status;
    if (index >= f->list_len(node.obj)) return ZN_ERR_RANGE;

    const std::string_view text = f->list_str(node.obj, index);
    *out = text.data();
    if (len) *len = text.size();
    return ZN_OK;
}

const char* ApiKindName(ZnNode node)   { return KindName(node.kind); }
size_t      ApiFieldCount(ZnNode node) { return node.obj ? FieldCount(node.kind) : 0; }

const char* ApiFieldName(ZnNode node, size_t index) {
    return node.obj ? FieldNameAt(node.kind, index) : nullptr;
}

int ApiFieldType(ZnNode node, const char* field) {
    if (!node.obj || !field) return 0;
    const Field* f = FindField(node.kind, field);
    return f ? static_cast<int>(f->type) : 0;
}

// --- output ---------------------------------------------------------------------------

EmitContext& Ctx(ZnEmitContext* ctx) { return *reinterpret_cast<EmitContext*>(ctx); }

int ApiWriteFile(ZnEmitContext* raw, const char* path, const char* bytes, size_t len) {
    if (!raw || !path || (!bytes && len)) return ZN_ERR_NULL;
    return Ctx(raw).Write(path, std::string_view{bytes, len});
}

int ApiOptions(ZnEmitContext* raw, ZnNode* out) {
    if (!raw || !out) return ZN_ERR_NULL;
    *out = MakeNode(&Ctx(raw).options, ZN_KIND_OPTIONS);
    return ZN_OK;
}

void ApiWarn(ZnEmitContext* raw, const char* message) {
    if (!raw || !message) return;
    Ctx(raw).result.warnings.emplace_back(message);
}

void ApiFail(ZnEmitContext* raw, const char* message) {
    if (!raw || !message) return;
    Ctx(raw).result.error = message;
}

void ApiLog(const char* message) {
    if (message) core::LogInfo("{}", message);
}

// A ZnTarget is an IMemorySource and nothing else; the cast is the whole implementation.
// A plugin holds an opaque pointer it can do nothing with but hand back.
core::IMemorySource* Target(ZnTarget* target) {
    return reinterpret_cast<core::IMemorySource*>(target);
}

size_t ApiRead(ZnTarget* target, uint64_t address, void* out, size_t size) {
    if (!target || !out || size == 0) return 0;
    return Target(target)->Read(static_cast<core::Address>(address), out, size);
}

int ApiMainModule(ZnTarget* target, ZnModuleInfo* out) {
    if (!target || !out) return ZN_ERR_NULL;

    const auto* module = Target(target)->MainModule();
    if (!module) return ZN_ERR_NO_FIELD;

    out->name = module->name.c_str();
    out->base = core::Raw(module->base);
    out->size = module->size;
    return ZN_OK;
}

const ZnApi kApi = {
    sizeof(ZnApi),
    ZN_ABI_MAJOR,
    ZN_ABI_MINOR,
    ZN_SCHEMA_VERSION,

    &ApiStr, &ApiI64, &ApiF64, &ApiBool, &ApiSub,
    &ApiLen, &ApiAt, &ApiAtStr,

    &ApiKindName, &ApiFieldCount, &ApiFieldName, &ApiFieldType,

    &ApiWriteFile, &ApiOptions, &ApiWarn, &ApiFail, &ApiLog,

    &ApiRead, &ApiMainModule,
};

} // namespace

const ZnApi* HostApi() { return &kApi; }

ZnTarget* AsTarget(core::IMemorySource& memory) {
    return reinterpret_cast<ZnTarget*>(&memory);
}

// A plugin names the file it wants and the host decides where that lands. Anything but a
// plain relative path inside the output directory is refused. A plugin has no business
// writing to C:\Windows, and "../../" is the obvious way it would try.
int EmitContext::Write(std::string_view relative, std::string_view bytes) {
    namespace fs = std::filesystem;

    if (relative.empty()) return ZN_ERR_BAD_PATH;

    const fs::path wanted{relative};
    if (wanted.is_absolute() || wanted.has_root_name()) return ZN_ERR_BAD_PATH;

    for (const auto& part : wanted)
        if (part == "..") return ZN_ERR_BAD_PATH;

    std::error_code ec;
    const fs::path root = fs::absolute(fs::path{options.out_dir}, ec);
    if (ec) return ZN_ERR_IO;

    const fs::path full = root / wanted;

    // Belt and braces. Even with ".." rejected a symlink or an odd component can land
    // outside, so check the resulting path against the root too.
    const fs::path normalized = full.lexically_normal();
    const auto [mismatch, _] = std::mismatch(root.begin(), root.end(),
                                             normalized.begin(), normalized.end());
    if (mismatch != root.end()) return ZN_ERR_BAD_PATH;

    fs::create_directories(normalized.parent_path(), ec);

    std::ofstream file(normalized, std::ios::binary | std::ios::trunc);
    if (!file) return ZN_ERR_IO;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) return ZN_ERR_IO;

    result.files.push_back(normalized.string());
    return ZN_OK;
}

} // namespace zircon::plugin
