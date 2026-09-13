# Writing a Zircon plugin

A plugin is a DLL exporting one function. It can add output formats, and it can teach
Zircon how to read a target that is strange enough that deriving alone will not reach it.

Plugins are **opt-in**. Nothing is loaded unless you pass `--plugins <dir>` or set
`ZIRCON_PLUGINS`. Running whatever DLL happens to be sitting next to the executable is not
a decision a tool should make for you.

```
zircon emit list  --plugins build/bin/Release/plugins
zircon emit csv dump.json -o out/ --plugins build/bin/Release/plugins
```

---

## The short version: write it in Lua

The Lua host (`plugins/zircon_lua.dll`) loads every `*.lua` file next to itself and
registers one emitter per script. No compiler, no ABI, no rebuild of Zircon.

```lua
return {
  name        = "sizes",
  description = "every class and its size",
  emit = function(dump)
    local lines = {}
    for i = 1, #dump.packages do
      local package = dump.packages[i]
      for j = 1, #package.classes do
        local class = package.classes[j]
        lines[#lines + 1] = string.format("%-60s %d", class.path, class.size)
      end
    end
    zircon.write("sizes.txt", table.concat(lines, "\n"))
  end,
}
```

Drop that next to `zircon_lua.dll` and `zircon emit sizes dump.json -o out/` works.

`dump` is a **live view** over the host's IR, not a copy. Indexing it calls back into
Zircon, so opening a 48 000-property dump costs nothing and you pay only for what you read.

Available to a script:

| | |
|---|---|
| `zircon.write(path, text)` | writes one file under the emit's output directory |
| `zircon.warn(message)` | surfaced to the user; does not fail the emit |
| `zircon.log(message)` | goes to Zircon's log |
| `zircon.options()` | the emit's options as a node |

Nodes behave like tables. `#list` is the length, `list[i]` is 1-based, `pairs(node)`
iterates the fields, and `tostring(node)` gives something readable. Asking for a field the
node does not have **raises** rather than returning `nil` — a typo that silently produced
empty output would be the one failure mode this project refuses to have.

`plugins/csv.lua` and `plugins/padding.lua` are worked examples. `padding.lua` is the one
worth reading: it finds bytes in every class that no reflected property accounts for, which
no built-in emitter does, in about eighty lines.

---

## The C ABI

`include/zircon/plugin.h` is the whole interface. Pure C, no C++ across the boundary,
everything reached through a vtable the host hands over — so a plugin built with a
different compiler or a different STL keeps working.

```c
#include "zircon/plugin.h"

static int emit_summary(ZnEmitContext* ctx, ZnNode dump, void* user) {
    const ZnApi* zn = (const ZnApi*)user;

    size_t packages = 0;
    zn->len(dump, "packages", &packages);

    char text[128];
    int n = snprintf(text, sizeof(text), "%zu packages\n", packages);

    return zn->write_file(ctx, "summary.txt", text, (size_t)n);
}

ZN_EXPORT int zircon_plugin_main(ZnHost* host) {
    if (host->api->abi_major != ZN_ABI_MAJOR) return ZN_ERR_REFUSED;
    if (host->api->abi_minor <  ZN_ABI_MINOR) return ZN_ERR_REFUSED;

    ZnEmitterDesc desc = {0};
    desc.name          = "summary";
    desc.description   = "how many packages there are";
    desc.needs_objects = 1;
    desc.emit          = emit_summary;
    desc.user          = (void*)host->api;

    return host->register_emitter(host, &desc);
}
```

Build it against nothing but that header, put the DLL in your plugin directory, done.

### Why fields are addressed by name

There are no per-field getters — no `zn_struct_size`, no `zn_property_offset`. Every IR
node is a `ZnNode` and every field is reached by name:

```c
zn->str(record, "name", &text, &len);
zn->len(record, "properties", &count);
zn->at (record, "properties", i, &property);
zn->i64(property, "offset", &offset);
```

The IR is this project's contract and it will grow. With typed getters, every new field is
a new export and the ABI freezes around the schema. By name, adding a field is not an ABI
change at all: old plugins never ask for it, and a new plugin asking an old host gets
`ZN_ERR_NO_FIELD` — a defined answer rather than undefined behaviour.

It also means nodes are introspectable, which is what makes the Lua binding possible:

```c
size_t n = zn->field_count(node);
for (size_t i = 0; i < n; ++i) {
    const char* name = zn->field_name(node, i);
    int type = zn->field_type(node, name);   /* ZN_FIELD_STR, ZN_FIELD_LIST_NODE, ... */
}
```

The cost is a string compare per access. For a boundary crossed by scripts, that is the
right trade.

### Node kinds and their fields

`dump` → `header`, `names`, `packages`, `schema_version`
`header` → `tool_version`, `created_utc`, `partial`, `source`, `engine`, `offsets`, `globals`
`package` → `name`, `classes`, `structs`, `enums`
`struct` → `name`, `path`, `super`, `is_class`, `size`, `alignment`, `inherited_size`, `cpp_prefix`, `vtable_rva`, `interfaces`, `properties`, `functions`
`property` → `name`, `type`, `offset`, `size`, `array_dim`, `flags`, `flag_names`, `is_bitfield`, `byte_mask`, `field_mask`, `bit_index`, `default_value`
`function` → `name`, `flags`, `flag_names`, `params`, `native_rva`, `script_size`, `script`, `script_complete`
`param` → `name`, `type`, `offset`, `size`, `is_return`, `is_out`, `is_const`
`type` → `kind`, `name`, `raw`, `size`, `params`
`enum` → `name`, `path`, `underlying`, `is_flags`, `values`
`statement` → `offset`, `depth`, `text`

`type.kind` is the *name* of the kind (`"Array"`, `"ObjectPtr"`, `"Int32"`), never a
number: a plugin that knows `Array == 24` is coupled to an enumerator order that is free
to change.

Rather than trust this list, ask: `field_count` / `field_name` always tell you what a
build actually has.

### Writing files

A plugin names a file; the host decides where it lands. `write_file` takes a **relative**
path and refuses anything that escapes the emit's output directory — absolute paths, `..`
components, and anything that normalises outside the root. A plugin has no business
writing to `C:\Windows`, and the check is in the host rather than left to the plugin's
good manners.

### Lifetimes

Every `ZnNode` and every string the host returns is valid **only until the emit call
returns**. Copy anything you keep. Calls come from one thread and the host never calls
into a plugin concurrently.

---

## Teaching Zircon to read a difficult target

Two hooks, both added in ABI 1.1. Both are narrow, and **neither is trusted**: a hook
supplies an input, and Zircon still proves it with the same checks it applies to anything
it found itself. That is the only way an extension point is safe in a tool whose whole
value is that its output is correct.

### A name decoder, for an encrypted FName pool

Some shipped games encrypt pool entries. The decoder is handed the address of one entry and
fills a buffer with the plain bytes — the two-byte header followed by the characters,
exactly as an unencrypted build would hold them.

```c
static size_t decode(ZnTarget* target, uint64_t entry,
                     uint8_t* out, size_t capacity, void* user) {
    const ZnApi* zn = (const ZnApi*)user;

    size_t got = zn->read(target, entry, out, capacity < 512 ? capacity : 512);
    for (size_t i = 0; i < got; ++i) out[i] ^= 0x5A;
    return got;      /* 0 to decline, and declining is fine */
}

host->register_name_decoder(host, decode, (void*)host->api);
```

Returning 0 declines, and the ordinary path runs — so a decoder that only recognises some
entries is perfectly normal. Everything Zircon checks about a normal entry it also checks
about a decoded one: the length bound, the printable test. A broken decoder can fail to
produce a name. It cannot put nonsense into the dump.

### A global resolver, for GObjects that cannot be scanned for

```c
static int resolve(ZnTarget* target, uint64_t* gobjects, uint64_t* name_pool, void* user) {
    ZnModuleInfo module;
    if (((const ZnApi*)user)->main_module(target, &module) != ZN_OK) return 0;

    *gobjects = module.base + 0x7C70C0;   /* however you know this */
    return 1;                             /* leave *name_pool at 0 to let the scan find it */
}

host->register_global_resolver(host, resolve, (void*)host->api);
```

What you return is a hint, not an answer. It runs through exactly the same validation as a
scanned candidate — for GObjects, "the object in slot *i* stores *i* in its InternalIndex";
for the pool, "block 0 begins with the `None` entry" — and is dropped if it fails, with the
scan then running as usual. A wrong hint costs you a little time and nothing else.

At most one of each may be installed process-wide, because each describes the target and
there is one target. A second registration is refused rather than silently replacing the
first.

---

## Versioning

`ZN_ABI_MAJOR` / `ZN_ABI_MINOR` — major changes when an existing call changes meaning or
disappears, minor when something is appended. `ZN_SCHEMA_VERSION` is separate: the ABI is
how you ask, the schema is what there is to ask about.

The check belongs to the plugin, because only the plugin knows both numbers:

```c
if (host->api->abi_major != ZN_ABI_MAJOR) return ZN_ERR_REFUSED;
if (host->api->abi_minor <  ZN_ABI_MINOR) return ZN_ERR_REFUSED;
```

A plugin built against 1.0 runs on a 1.7 host. One built against 1.7 declines a 1.0 host,
rather than calling through a vtable slot that is not there.

Registering a name that a built-in emitter already uses is refused. A user who asks for
`cpp_sdk` gets the `cpp_sdk` that shipped with the tool.

---

## What a plugin cannot do

It runs in Zircon's process, so this is a trust boundary in only one direction: the host
defends its own invariants, not against hostile code. What is enforced:

- it cannot write outside the emit's output directory
- it cannot shadow a built-in emitter's name
- it cannot make Zircon believe an unvalidated address or an unprintable name
- it cannot write to the target — `ZnTarget` is read-only, and Zircon does not write to
  targets either

Load plugins you trust, the same as any other DLL.
