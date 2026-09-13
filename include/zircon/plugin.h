/* Zircon plugin ABI.
 *
 * Pure C, no C++ types across the boundary, everything reached through a vtable the host
 * hands over. A plugin built with a different compiler, a different STL or a different
 * version of Zircon keeps working.
 *
 * ---------------------------------------------------------------------------------
 * The one design decision worth explaining
 * ---------------------------------------------------------------------------------
 *
 * The IR is this project's contract and it is going to grow. An ABI with one typed getter
 * per field (zn_struct_name, zn_struct_size, zn_property_offset, ...) freezes the IR: every
 * new field is a new export, and a plugin built against an older header cannot be linked
 * against a newer host without care.
 *
 * So there are no per-field getters. Every IR node is a ZnNode, and fields are addressed by
 * *name*:
 *
 *     zn->str(record, "name", &text, &len);
 *     zn->len(record, "properties", &count);
 *     zn->at (record, "properties", i, &property);
 *     zn->i64(property, "offset", &offset);
 *
 * Adding a field to the IR is then not an ABI change at all. Old plugins never ask for it;
 * new plugins asking a host that does not have it get ZN_ERR_NO_FIELD, which is a defined
 * answer instead of undefined behaviour. Nodes are also introspectable (field_count /
 * field_name / field_type), which is what lets a scripting binding expose them as ordinary
 * tables and iterate them.
 *
 * The cost is a string compare per access. That is the right trade for a boundary crossed
 * by scripts.
 *
 * ---------------------------------------------------------------------------------
 * Lifetime and threading
 * ---------------------------------------------------------------------------------
 *
 * Every ZnNode and every string a host returns is owned by the host and valid only until
 * the emit call that produced it returns. Copy anything you keep. Calls are made from one
 * thread; the host makes no calls into a plugin concurrently.
 */

#ifndef ZIRCON_PLUGIN_H
#define ZIRCON_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define ZN_EXPORT __declspec(dllexport)
#else
#  define ZN_EXPORT __attribute__((visibility("default")))
#endif

/* Bumped major when an existing call changes meaning or disappears; bumped minor when
 * something is added to the end of a struct.
 *
 * The check belongs to the plugin, because only the plugin knows both numbers: the host
 * publishes what it is in ZnApi, and the plugin compares that against the header it was
 * built against. The first line of zircon_plugin_main should be:
 *
 *     if (host->api->abi_major != ZN_ABI_MAJOR) return ZN_ERR_REFUSED;
 *     if (host->api->abi_minor <  ZN_ABI_MINOR) return ZN_ERR_REFUSED;
 *
 * A plugin built against 1.0 therefore runs on a 1.7 host, and one built against 1.7
 * declines a 1.0 host instead of calling through a vtable slot that is not there. */
#define ZN_ABI_MAJOR 1
#define ZN_ABI_MINOR 1

/* The IR schema the host is serving. Independent of the ABI: the ABI is how you ask, the
 * schema is what there is to ask about. */
#define ZN_SCHEMA_VERSION 1

/* ------------------------------------------------------------------ status codes */

typedef enum ZnStatus {
    ZN_OK = 0,
    ZN_ERR_NO_FIELD    = 1,  /* this node kind has no field by that name */
    ZN_ERR_WRONG_TYPE  = 2,  /* the field exists but is not of the requested type */
    ZN_ERR_RANGE       = 3,  /* index past the end of a list */
    ZN_ERR_NULL        = 4,  /* a null node or a null out-parameter */
    ZN_ERR_IO          = 5,  /* write_file failed */
    ZN_ERR_BAD_PATH    = 6,  /* write_file was given a path that escapes the output dir */
    ZN_ERR_REFUSED     = 7   /* the host declined, e.g. a duplicate emitter name */
} ZnStatus;

/* ------------------------------------------------------------------ nodes */

typedef enum ZnKind {
    ZN_KIND_NONE = 0,
    ZN_KIND_DUMP,
    ZN_KIND_HEADER,
    ZN_KIND_SOURCE,
    ZN_KIND_ENGINE,
    ZN_KIND_OFFSET,
    ZN_KIND_PACKAGE,
    ZN_KIND_STRUCT,
    ZN_KIND_ENUM,
    ZN_KIND_ENUM_VALUE,
    ZN_KIND_PROPERTY,
    ZN_KIND_FUNCTION,
    ZN_KIND_PARAM,
    ZN_KIND_STATEMENT,
    ZN_KIND_TYPE,
    ZN_KIND_OPTIONS
} ZnKind;

/* What a field holds, for introspection. */
typedef enum ZnFieldType {
    ZN_FIELD_STR = 1,
    ZN_FIELD_I64,
    ZN_FIELD_F64,
    ZN_FIELD_BOOL,
    ZN_FIELD_NODE,
    ZN_FIELD_LIST_NODE,
    ZN_FIELD_LIST_STR
} ZnFieldType;

/* An opaque handle onto one IR node. Copyable by value; never freed by the plugin. */
typedef struct ZnNode {
    const void* obj;
    uint32_t    kind;   /* ZnKind */
    uint32_t    _pad;
} ZnNode;

/* ------------------------------------------------------------------ target access */

/* A handle onto the process, dump or image being read. Only the two hooks below get one,
 * and only for as long as the call they were passed to. Read-only: Zircon does not write
 * to a target, and neither does a plugin through this. */
typedef struct ZnTarget ZnTarget;

typedef struct ZnModuleInfo {
    const char* name;
    uint64_t    base;
    uint64_t    size;
} ZnModuleInfo;

/* ------------------------------------------------------------------ emitter contract */

typedef struct ZnEmitContext ZnEmitContext;

/* Options the user gave on the command line, reachable through the same accessor API as
 * everything else: "out_dir", "package_filter", "single_file", "allow_partial". */

/* Returns ZN_OK, or anything else to fail the emit. On failure call zn->fail() first with
 * a reason the user can act on. */
typedef int (*ZnEmitFn)(ZnEmitContext* ctx, ZnNode dump, void* user);

typedef struct ZnEmitterDesc {
    const char* name;          /* what the user types: "csv", "rust_ffi" */
    const char* description;   /* one line, shown by `zircon emit --list` */
    int         needs_objects; /* refuse a partial dump unless allow_partial */
    ZnEmitFn    emit;
    void*       user;          /* passed back to emit, untouched by the host */
} ZnEmitterDesc;

/* ------------------------------------------------------------------ hooks */

/* Decodes one FName pool entry. `entry` is the address of the entry's two-byte header;
 * fill `out` with the plain bytes of that entry -- header followed by characters, exactly
 * as an unencrypted build would hold them -- and return how many bytes you wrote.
 *
 * Return 0 to decline, which is normal: a decoder that only recognises some entries leaves
 * the rest to the ordinary path. Everything Zircon checks about a normal entry it also
 * checks about a decoded one, so a decoder cannot inject nonsense into the dump; it can
 * only fail to produce a name. */
typedef size_t (*ZnNameDecoderFn)(ZnTarget* target, uint64_t entry,
                                  uint8_t* out, size_t capacity, void* user);

/* Supplies addresses for the globals, for a target where scanning will not find them.
 * Write an address into either out-parameter, or leave it 0 to let the scan handle that
 * one. Return 1 if you supplied anything, 0 to decline entirely.
 *
 * What you return is a hint, not an answer: it goes through exactly the same validation as
 * a scanned candidate, and is dropped if it fails. */
typedef int (*ZnGlobalResolverFn)(ZnTarget* target, uint64_t* gobjects,
                                  uint64_t* name_pool, void* user);

/* ------------------------------------------------------------------ the host vtable */

typedef struct ZnApi {
    /* Size of this struct as the host built it. A plugin built against a newer header can
     * check before calling anything past the end. */
    size_t size;

    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t schema_version;

    /* --- reading nodes ---------------------------------------------------------- */

    /* Strings are NUL-terminated and *len excludes the terminator. `len` may be NULL. */
    int (*str) (ZnNode node, const char* field, const char** out, size_t* len);
    int (*i64) (ZnNode node, const char* field, int64_t* out);
    int (*f64) (ZnNode node, const char* field, double* out);
    int (*bln) (ZnNode node, const char* field, int* out);
    int (*sub) (ZnNode node, const char* field, ZnNode* out);

    /* Lists. `len` works on ZN_FIELD_LIST_NODE and ZN_FIELD_LIST_STR alike. */
    int (*len)    (ZnNode node, const char* field, size_t* out);
    int (*at)     (ZnNode node, const char* field, size_t index, ZnNode* out);
    int (*at_str) (ZnNode node, const char* field, size_t index,
                   const char** out, size_t* len);

    /* --- introspection ---------------------------------------------------------- */

    const char* (*kind_name)   (ZnNode node);
    size_t      (*field_count) (ZnNode node);
    const char* (*field_name)  (ZnNode node, size_t index);
    int         (*field_type)  (ZnNode node, const char* field);   /* ZnFieldType, or 0 */

    /* --- output ----------------------------------------------------------------- */

    /* Writes one file under the emit's output directory. `path` is relative and is
     * rejected if it escapes: a plugin cannot write outside where the user pointed it.
     * Intermediate directories are created. */
    int (*write_file) (ZnEmitContext* ctx, const char* path, const char* bytes, size_t len);

    /* The options the emit was invoked with, as a node. */
    int (*options) (ZnEmitContext* ctx, ZnNode* out);

    /* Surfaced to the user. `warn` does not fail the emit; `fail` records the reason the
     * emit is about to return non-zero. */
    void (*warn) (ZnEmitContext* ctx, const char* message);
    void (*fail) (ZnEmitContext* ctx, const char* message);

    /* Goes to Zircon's log at info level, for progress on a long emit. */
    void (*log)  (const char* message);

    /* --- added in ABI 1.1: reading a target, for the hooks ---------------------- */

    /* Returns the number of bytes actually read, which may be less than asked for. */
    size_t (*read)        (ZnTarget* target, uint64_t address, void* out, size_t size);
    int    (*main_module) (ZnTarget* target, ZnModuleInfo* out);
} ZnApi;

/* ------------------------------------------------------------------ registration */

typedef struct ZnHost {
    size_t         size;
    const ZnApi*   api;

    /* Registering a name that already exists is refused, so a plugin cannot quietly
     * shadow a built-in emitter. */
    int (*register_emitter) (struct ZnHost* host, const ZnEmitterDesc* desc);

    /* Where this plugin was loaded from, so a script host can find files next to itself. */
    const char* (*plugin_path) (struct ZnHost* host);

    void* _reserved;

    /* --- added in ABI 1.1 ------------------------------------------------------- */

    /* At most one of each may be installed, process-wide: a hook describes the target,
     * and there is one target. Registering a second is refused. */
    int (*register_name_decoder)   (struct ZnHost* host, ZnNameDecoderFn fn, void* user);
    int (*register_global_resolver)(struct ZnHost* host, ZnGlobalResolverFn fn, void* user);
} ZnHost;

/* The entry point. A plugin exports exactly this, by exactly this name.
 * Return ZN_OK to stay loaded; anything else and the host unloads it and says so. */
ZN_EXPORT int zircon_plugin_main(ZnHost* host);
typedef int (*ZnPluginMainFn)(ZnHost* host);

/* Optional. If exported, the host calls it before unloading. */
typedef void (*ZnPluginShutdownFn)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZIRCON_PLUGIN_H */
