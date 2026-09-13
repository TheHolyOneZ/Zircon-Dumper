/* The Lua emitter host, written as an ordinary Zircon plugin.
 *
 * It is deliberately not built into the tool. Everything it does it does through the
 * public ZnApi, which means the ABI is exercised end to end by something real rather than
 * by a test that knows where the bodies are buried: if a field is missing from the node
 * tables, or write_file's path check is wrong, this stops working.
 *
 * At load it looks for *.lua next to itself and registers one emitter per script. A script
 * returns a table:
 *
 *     return {
 *       name        = "csv",
 *       description = "every property as a CSV row",
 *       emit = function(dump)
 *         for _, pkg in ipairs(dump.packages) do ... end
 *         zircon.write("properties.csv", table.concat(rows, "\n"))
 *       end
 *     }
 *
 * `dump` is a live view over the host's IR, not a copy: indexing it calls back through the
 * ABI. A 48000-property dump is therefore free to open and costs only what the script
 * actually reads.
 */

#include "zircon/plugin.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ZN_NODE_MT "zircon.node"
#define ZN_LIST_MT "zircon.list"

/* One registered script. The host keeps these for the life of the process, because the
 * emitters registered from them are callbacks into this Lua state. */
typedef struct Script {
    lua_State* L;
    int        emit_ref;      /* registry ref to the script's emit function */
    char*      name;
    char*      description;
} Script;

static const ZnApi*    g_api = NULL;
static ZnEmitContext*  g_ctx = NULL;   /* valid only inside an emit call */

/* ------------------------------------------------------------------ node userdata */

typedef struct NodeBox { ZnNode node; } NodeBox;

/* A lazy view onto one list field: holds the parent and the field name, reads on demand.
 * Converting `properties` into a Lua table eagerly would allocate 48000 tables for a
 * script that only wanted to count them. */
typedef struct ListBox { ZnNode parent; char field[64]; int is_str; } ListBox;

static void PushNode(lua_State* L, ZnNode node) {
    if (!node.obj) { lua_pushnil(L); return; }
    NodeBox* box = (NodeBox*)lua_newuserdatauv(L, sizeof(NodeBox), 0);
    box->node = node;
    luaL_setmetatable(L, ZN_NODE_MT);
}

static void PushList(lua_State* L, ZnNode parent, const char* field, int is_str) {
    ListBox* box = (ListBox*)lua_newuserdatauv(L, sizeof(ListBox), 0);
    box->parent = parent;
    box->is_str = is_str;
    snprintf(box->field, sizeof(box->field), "%s", field);
    luaL_setmetatable(L, ZN_LIST_MT);
}

/* Reading a field the node does not have is a script bug, so it raises rather than
 * returning nil. A silent nil turns a typo into empty output that looks like real output,
 * which is the one thing this project refuses to produce. */
static int NodeIndex(lua_State* L) {
    NodeBox* box = (NodeBox*)luaL_checkudata(L, 1, ZN_NODE_MT);
    const char* key = luaL_checkstring(L, 2);

    if (strcmp(key, "kind") == 0 && g_api->field_type(box->node, "kind") == 0) {
        lua_pushstring(L, g_api->kind_name(box->node));
        return 1;
    }

    int type = g_api->field_type(box->node, key);
    if (type == 0)
        return luaL_error(L, "'%s' has no field '%s'", g_api->kind_name(box->node), key);

    switch (type) {
        case ZN_FIELD_STR: {
            const char* text = NULL; size_t len = 0;
            if (g_api->str(box->node, key, &text, &len) != ZN_OK)
                return luaL_error(L, "could not read '%s'", key);
            lua_pushlstring(L, text, len);
            return 1;
        }
        case ZN_FIELD_I64: {
            int64_t value = 0;
            if (g_api->i64(box->node, key, &value) != ZN_OK)
                return luaL_error(L, "could not read '%s'", key);
            lua_pushinteger(L, (lua_Integer)value);
            return 1;
        }
        case ZN_FIELD_F64: {
            double value = 0;
            if (g_api->f64(box->node, key, &value) != ZN_OK)
                return luaL_error(L, "could not read '%s'", key);
            lua_pushnumber(L, (lua_Number)value);
            return 1;
        }
        case ZN_FIELD_BOOL: {
            int value = 0;
            if (g_api->bln(box->node, key, &value) != ZN_OK)
                return luaL_error(L, "could not read '%s'", key);
            lua_pushboolean(L, value);
            return 1;
        }
        case ZN_FIELD_NODE: {
            ZnNode sub;
            if (g_api->sub(box->node, key, &sub) != ZN_OK)
                return luaL_error(L, "could not read '%s'", key);
            PushNode(L, sub);
            return 1;
        }
        case ZN_FIELD_LIST_NODE: PushList(L, box->node, key, 0); return 1;
        case ZN_FIELD_LIST_STR:  PushList(L, box->node, key, 1); return 1;
        default:
            return luaL_error(L, "field '%s' has an unknown type", key);
    }
}

/* Lets a script do `for key in pairs(node)`, which is what makes a generic dumper
 * possible without the script knowing the schema. */
static int NodeNext(lua_State* L) {
    NodeBox* box = (NodeBox*)luaL_checkudata(L, 1, ZN_NODE_MT);
    size_t index = (size_t)lua_tointeger(L, lua_upvalueindex(1));

    if (index >= g_api->field_count(box->node)) { lua_pushnil(L); return 1; }

    const char* name = g_api->field_name(box->node, index);
    lua_pushinteger(L, (lua_Integer)(index + 1));
    lua_replace(L, lua_upvalueindex(1));

    lua_pushstring(L, name);
    lua_pushvalue(L, 1);
    lua_pushstring(L, name);
    if (NodeIndex(L) != 1) return luaL_error(L, "field iteration failed");
    return 2;
}

static int NodePairs(lua_State* L) {
    luaL_checkudata(L, 1, ZN_NODE_MT);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, NodeNext, 1);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

static int NodeToString(lua_State* L) {
    NodeBox* box = (NodeBox*)luaL_checkudata(L, 1, ZN_NODE_MT);

    /* Most nodes have a name or a path; showing it beats "userdata: 0x...". */
    const char* text = NULL;
    if (g_api->field_type(box->node, "path") == ZN_FIELD_STR)
        g_api->str(box->node, "path", &text, NULL);
    else if (g_api->field_type(box->node, "name") == ZN_FIELD_STR)
        g_api->str(box->node, "name", &text, NULL);

    if (text && *text) lua_pushfstring(L, "%s<%s>", g_api->kind_name(box->node), text);
    else               lua_pushfstring(L, "%s", g_api->kind_name(box->node));
    return 1;
}

static int ListLen(lua_State* L) {
    ListBox* box = (ListBox*)luaL_checkudata(L, 1, ZN_LIST_MT);
    size_t count = 0;
    g_api->len(box->parent, box->field, &count);
    lua_pushinteger(L, (lua_Integer)count);
    return 1;
}

/* Lua is 1-based; the ABI is 0-based. Out of range is nil, not an error, so ipairs
 * terminates naturally. */
static int ListIndex(lua_State* L) {
    ListBox* box = (ListBox*)luaL_checkudata(L, 1, ZN_LIST_MT);
    lua_Integer i = luaL_checkinteger(L, 2);
    if (i < 1) { lua_pushnil(L); return 1; }

    size_t index = (size_t)(i - 1);
    if (box->is_str) {
        const char* text = NULL; size_t len = 0;
        if (g_api->at_str(box->parent, box->field, index, &text, &len) != ZN_OK) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, text, len);
        return 1;
    }

    ZnNode element;
    if (g_api->at(box->parent, box->field, index, &element) != ZN_OK) {
        lua_pushnil(L);
        return 1;
    }
    PushNode(L, element);
    return 1;
}

/* ------------------------------------------------------------------ zircon.* */

static int ZnWrite(lua_State* L) {
    size_t path_len = 0, body_len = 0;
    const char* path = luaL_checklstring(L, 1, &path_len);
    const char* body = luaL_checklstring(L, 2, &body_len);

    if (!g_ctx) return luaL_error(L, "zircon.write is only callable during an emit");

    int status = g_api->write_file(g_ctx, path, body, body_len);
    if (status == ZN_ERR_BAD_PATH)
        return luaL_error(L, "refused to write '%s': it escapes the output directory", path);
    if (status != ZN_OK)
        return luaL_error(L, "could not write '%s'", path);

    lua_pushboolean(L, 1);
    return 1;
}

static int ZnWarn(lua_State* L) {
    const char* message = luaL_checkstring(L, 1);
    if (g_ctx) g_api->warn(g_ctx, message);
    return 0;
}

static int ZnLog(lua_State* L) {
    g_api->log(luaL_checkstring(L, 1));
    return 0;
}

static int ZnOptions(lua_State* L) {
    ZnNode options;
    if (!g_ctx || g_api->options(g_ctx, &options) != ZN_OK)
        return luaL_error(L, "options are only available during an emit");
    PushNode(L, options);
    return 1;
}

/* ------------------------------------------------------------------ the emitter */

static int RunEmit(ZnEmitContext* ctx, ZnNode dump, void* user) {
    Script* script = (Script*)user;
    lua_State* L = script->L;

    g_ctx = ctx;

    lua_rawgeti(L, LUA_REGISTRYINDEX, script->emit_ref);
    PushNode(L, dump);

    int status = lua_pcall(L, 1, 0, 0);
    g_ctx = NULL;

    if (status != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        g_api->fail(ctx, message ? message : "the script raised an error");
        lua_pop(L, 1);
        return ZN_ERR_IO;
    }
    return ZN_OK;
}

/* ------------------------------------------------------------------ loading */

static void OpenBindings(lua_State* L) {
    luaL_newmetatable(L, ZN_NODE_MT);
    lua_pushcfunction(L, NodeIndex);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, NodePairs);    lua_setfield(L, -2, "__pairs");
    lua_pushcfunction(L, NodeToString); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);

    luaL_newmetatable(L, ZN_LIST_MT);
    lua_pushcfunction(L, ListIndex); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ListLen);   lua_setfield(L, -2, "__len");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ZnWrite);   lua_setfield(L, -2, "write");
    lua_pushcfunction(L, ZnWarn);    lua_setfield(L, -2, "warn");
    lua_pushcfunction(L, ZnLog);     lua_setfield(L, -2, "log");
    lua_pushcfunction(L, ZnOptions); lua_setfield(L, -2, "options");
    lua_setglobal(L, "zircon");
}

static char* Duplicate(const char* text) {
    size_t len = strlen(text) + 1;
    char* copy = (char*)malloc(len);
    if (copy) memcpy(copy, text, len);
    return copy;
}

/* Loads one script and registers what it describes. Any problem is reported against the
 * file by name and the rest keep loading: one bad script must not cost the user the
 * others. */
static void LoadScript(ZnHost* host, const char* path, const char* filename) {
    lua_State* L = luaL_newstate();
    if (!L) return;

    luaL_openlibs(L);
    OpenBindings(L);

    if (luaL_dofile(L, path) != LUA_OK) {
        g_api->log(lua_tostring(L, -1));
        lua_close(L);
        return;
    }

    if (!lua_istable(L, -1)) {
        char message[512];
        snprintf(message, sizeof(message),
                 "%s: expected the script to return a table with name/description/emit",
                 filename);
        g_api->log(message);
        lua_close(L);
        return;
    }

    lua_getfield(L, -1, "name");
    lua_getfield(L, -2, "description");
    lua_getfield(L, -3, "emit");

    if (!lua_isstring(L, -3) || !lua_isfunction(L, -1)) {
        char message[512];
        snprintf(message, sizeof(message),
                 "%s: needs a string 'name' and a function 'emit'", filename);
        g_api->log(message);
        lua_close(L);
        return;
    }

    Script* script = (Script*)calloc(1, sizeof(Script));
    if (!script) { lua_close(L); return; }

    script->L           = L;
    script->name        = Duplicate(lua_tostring(L, -3));
    script->description = Duplicate(lua_isstring(L, -2) ? lua_tostring(L, -2)
                                                        : "a Lua emitter");
    script->emit_ref    = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops emit */

    ZnEmitterDesc desc;
    desc.name          = script->name;
    desc.description   = script->description;
    desc.needs_objects = 1;
    desc.emit          = RunEmit;
    desc.user          = script;

    if (host->register_emitter(host, &desc) != ZN_OK) {
        /* The host has already said why. The state stays open: unwinding a partially
         * registered script is more risk than the memory is worth for a process that is
         * about to do one emit and exit. */
    }

    lua_settop(L, 0);
}

ZN_EXPORT int zircon_plugin_main(ZnHost* host) {
    if (!host || !host->api) return ZN_ERR_NULL;

    /* The plugin is the side that can check this, because it knows both numbers. A
       different major means the calls it is about to make may mean something else; an
       older minor means some of them are not in the vtable at all. */
    if (host->api->abi_major != ZN_ABI_MAJOR) return ZN_ERR_REFUSED;
    if (host->api->abi_minor <  ZN_ABI_MINOR) return ZN_ERR_REFUSED;
    if (host->api->size < sizeof(ZnApi))      return ZN_ERR_REFUSED;

    g_api = host->api;

    const char* self = host->plugin_path ? host->plugin_path(host) : NULL;
    if (!self) return ZN_ERR_NULL;

    /* Scripts live next to this DLL. */
    char folder[MAX_PATH * 2];
    snprintf(folder, sizeof(folder), "%s", self);
    char* slash = strrchr(folder, '\\');
    if (!slash) slash = strrchr(folder, '/');
    if (slash) *slash = '\0';

    char pattern[MAX_PATH * 2];
    snprintf(pattern, sizeof(pattern), "%s\\*.lua", folder);

    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        g_api->log("lua: no *.lua scripts found next to the plugin");
        return ZN_OK;
    }

    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char full[MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s\\%s", folder, found.cFileName);
        LoadScript(host, full, found.cFileName);
    } while (FindNextFileA(search, &found));

    FindClose(search);
    return ZN_OK;
}
