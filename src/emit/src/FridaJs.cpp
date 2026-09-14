#include "Emitters.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace zircon::emit {
namespace {

// One .js for `frida -l`. Data blob + a small runtime that turns an address and a type
// path into an object with real property accessors.
//
// The blob is JSON.parse'd rather than written as a JS object literal. V8 has a dedicated
// JSON path and it's several times quicker at this size - a 30 MB object literal leaves
// Frida chewing on it before the script even starts.

std::string JsString(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // anything under 0x20 breaks the literal. high bytes pass through - UE
                // names are ASCII in practice and the file's UTF-8 anyway.
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else          out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

// Escape the JSON again so it survives inside a JS string literal. Backslash and quote end
// it early; a newline is a straight syntax error. So the blob's own line breaks become
// `' + '` and the literal restarts on the next source line. Escaping them to \n instead
// would work and give you one 30 MB line that no editor will open.
std::string EmbedJson(std::string_view json) {
    std::string out;
    out.reserve(json.size() + json.size() / 8);
    out.push_back('\'');
    for (const char c : json) {
        if (c == '\n') { out += "' +\n'"; continue; }
        if (c == '\\' || c == '\'') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// All the reader needs, and short - this gets repeated 50k times in the blob, so the IR's
// full type tree would be wasteful.
//
//   u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 bool
//   name        FName, two int32s
//   str         FString
//   ptr:<path>  UObject* and friends
//   enum:<path>
//   struct:<path>
//   array:<inner>
//   opaque      anything with allocator state that isn't safe to touch
std::string TypeTag(const ir::TypeRef& type) {
    switch (type.kind) {
        case ir::TypeKind::Bool:   return "bool";
        case ir::TypeKind::Int8:   return "i8";
        case ir::TypeKind::Int16:  return "i16";
        case ir::TypeKind::Int32:  return "i32";
        case ir::TypeKind::Int64:  return "i64";
        case ir::TypeKind::UInt8:  return "u8";
        case ir::TypeKind::UInt16: return "u16";
        case ir::TypeKind::UInt32: return "u32";
        case ir::TypeKind::UInt64: return "u64";
        case ir::TypeKind::Float:  return type.size == 8 ? "f64" : "f32";
        case ir::TypeKind::Double: return "f64";
        case ir::TypeKind::Name:   return "name";
        case ir::TypeKind::String: return "str";
        case ir::TypeKind::Enum:   return "enum:" + type.name;
        case ir::TypeKind::Struct: return "struct:" + type.name;

        case ir::TypeKind::ObjectPtr:
        case ir::TypeKind::ClassPtr:
            return "ptr:" + type.name;

        case ir::TypeKind::Array:
            if (!type.params.empty()) return "array:" + TypeTag(type.params.front());
            return "opaque";

        // everything below has allocator state / a hash table / a delegate list sitting
        // next to the value. you need the engine's own code to read those properly, so the
        // runtime just hands back the address and says what it is.
        case ir::TypeKind::Text:
        case ir::TypeKind::WeakPtr: case ir::TypeKind::LazyPtr:
        case ir::TypeKind::SoftPtr: case ir::TypeKind::SoftClassPtr:
        case ir::TypeKind::Interface:
        case ir::TypeKind::Set: case ir::TypeKind::Map:
        case ir::TypeKind::Delegate: case ir::TypeKind::MulticastDelegate:
        case ir::TypeKind::FieldPath: case ir::TypeKind::Optional:
        case ir::TypeKind::Unknown:
            break;
    }
    return "opaque";
}

void AppendType(std::string& json, const ir::Struct& record, bool is_class) {
    json += "\n";
    json += std::format("{}:{{", JsString(record.path));
    json += std::format("\"n\":{},", JsString(record.name));
    if (!record.super.empty()) json += std::format("\"super\":{},", JsString(record.super));
    json += std::format("\"size\":{},\"align\":{},\"inherited\":{},\"class\":{}",
                        record.size, record.alignment, record.inherited_size,
                        is_class ? "true" : "false");
    if (record.vtable_rva != 0)
        json += std::format(",\"vtable\":{}", record.vtable_rva);

    if (!record.properties.empty()) {
        json += ",\"props\":[";
        bool first = true;
        for (const auto& property : record.properties) {
            if (!first) json += ",";
            first = false;
            json += std::format("{{\"n\":{},\"o\":{},\"t\":{},\"s\":{}",
                                JsString(property.name), property.offset,
                                JsString(TypeTag(property.type)), property.size);
            if (property.array_dim > 1) json += std::format(",\"dim\":{}", property.array_dim);
            if (property.is_bitfield) {
                json += std::format(",\"bit\":{},\"mask\":{}", property.bit_index,
                                    static_cast<int>(property.field_mask));
            }
            if (!property.default_value.empty())
                json += std::format(",\"d\":{}", JsString(property.default_value));
            json += "}";
        }
        json += "]";
    }

    if (!record.functions.empty()) {
        json += ",\"funcs\":[";
        bool first = true;
        for (const auto& function : record.functions) {
            if (!first) json += ",";
            first = false;
            json += std::format("{{\"n\":{},\"flags\":{}",
                                JsString(function.name), function.flags);
            if (function.native_rva != 0)
                json += std::format(",\"rva\":{}", function.native_rva);
            if (!function.params.empty()) {
                json += ",\"params\":[";
                bool first_param = true;
                for (const auto& param : function.params) {
                    if (!first_param) json += ",";
                    first_param = false;
                    json += std::format("{{\"n\":{},\"o\":{},\"t\":{},\"s\":{}",
                                        JsString(param.name), param.offset,
                                        JsString(TypeTag(param.type)), param.size);
                    if (param.is_return) json += ",\"ret\":true";
                    if (param.is_out)    json += ",\"out\":true";
                    json += "}";
                }
                json += "]";
            }
            json += "}";
        }
        json += "]";
    }
    json += "}";
}

constexpr std::string_view kRuntime = R"JS(
function decodeTag(tag) {
  var colon = tag.indexOf(':');
  if (colon < 0) return { kind: tag, arg: '' };
  return { kind: tag.substring(0, colon), arg: tag.substring(colon + 1) };
}

function readRaw(address, size) {
  if (size === 2) return address.readU16();
  if (size === 4) return address.readU32();
  if (size === 8) return address.readU64().toNumber();
  return address.readU8();
}

function writeRaw(address, size, value) {
  if (size === 2)      address.writeU16(value);
  else if (size === 4) address.writeU32(value);
  else if (size === 8) address.writeU64(value);
  else                 address.writeU8(value);
}

var Zircon = {
  meta: DATA.meta,
  offsets: DATA.offsets,
  types: DATA.types,
  enums: DATA.enums,

  // Lazy on purpose. Everything above here is plain data, and querying offsets from node
  // or a build script is a normal thing to want - grabbing a Frida-only global at load
  // time would break that for nothing.
  _base: null,
  get base() {
    if (this._base === null) {
      this._base = (typeof Module !== 'undefined' && Module.findBaseAddress)
        ? (Module.findBaseAddress(DATA.meta.module) || ptr(0))
        : null;
      if (this._base === null)
        throw new Error('module base needs Frida; set Zircon._base yourself to override');
    }
    return this._base;
  },
  set base(value) { this._base = value; },

  type: function (path) {
    var record = this.types[path];
    if (record) return record;
    // leaf names are what people actually type. only take it when there's exactly one
    // match; quietly picking one of two Actors is the kind of thing you miss for hours.
    var hits = [];
    for (var key in this.types) {
      var dot = key.lastIndexOf('.');
      if (key.substring(dot + 1) === path) hits.push(key);
    }
    if (hits.length === 1) return this.types[hits[0]];
    if (hits.length > 1) throw new Error(path + ' is ambiguous: ' + hits.join(', '));
    return null;
  },

  // walks the super chain, so you don't have to know which class declares it
  property: function (path, name) {
    var record = this.type(path);
    while (record) {
      var props = record.props || [];
      for (var i = 0; i < props.length; i++) if (props[i].n === name) return props[i];
      record = record.super ? this.types[record.super] : null;
    }
    return null;
  },

  offsetOf: function (path, name) {
    var prop = this.property(path, name);
    return prop ? prop.o : -1;
  },

  enumName: function (path, value) {
    var table = this.enums[path];
    if (!table) return String(value);
    for (var i = 0; i < table.length; i++) if (table[i][1] === value) return table[i][0];
    return String(value);
  },

  enumValue: function (path, name) {
    var table = this.enums[path];
    if (!table) return null;
    for (var i = 0; i < table.length; i++) if (table[i][0] === name) return table[i][1];
    return null;
  },

  // `size` only matters for enums. most UE enums are a byte, but TEnumAsByte isn't a rule,
  // and reading a 4-byte one as a byte is wrong across three quarters of its range.
  readAt: function (address, tag, size) {
    var t = decodeTag(tag);
    switch (t.kind) {
      case 'bool': return address.readU8() !== 0;
      case 'i8':   return address.readS8();
      case 'i16':  return address.readS16();
      case 'i32':  return address.readS32();
      case 'i64':  return address.readS64();
      case 'u8':   return address.readU8();
      case 'u16':  return address.readU16();
      case 'u32':  return address.readU32();
      case 'u64':  return address.readU64();
      case 'f32':  return address.readFloat();
      case 'f64':  return address.readDouble();
      case 'ptr':  return address.readPointer();
      case 'enum': return this.enumName(t.arg, readRaw(address, size));
      case 'name': return { index: address.readS32(), number: address.add(4).readS32() };
      case 'str': {
        var data = address.readPointer();
        var num = address.add(8).readS32();
        if (data.isNull() || num <= 0 || num > 0x10000) return '';
        return data.readUtf16String(num - 1);
      }
      case 'struct': return this.wrap(address, t.arg);
      // elements live behind a pointer the engine owns. hand back the pieces instead of a JS
      // array, since only the caller knows whether the game is mid-resize.
      case 'array': {
        return { data: address.readPointer(), num: address.add(8).readS32(),
                 max: address.add(12).readS32(), element: t.arg };
      }
      default: return address;
    }
  },

  writeAt: function (address, tag, value, size) {
    var t = decodeTag(tag);
    switch (t.kind) {
      case 'bool': address.writeU8(value ? 1 : 0); return true;
      case 'i8':   address.writeS8(value);   return true;
      case 'i16':  address.writeS16(value);  return true;
      case 'i32':  address.writeS32(value);  return true;
      case 'i64':  address.writeS64(value);  return true;
      case 'u8':   address.writeU8(value);   return true;
      case 'u16':  address.writeU16(value);  return true;
      case 'u32':  address.writeU32(value);  return true;
      case 'u64':  address.writeU64(value);  return true;
      case 'f32':  address.writeFloat(value);  return true;
      case 'f64':  address.writeDouble(value); return true;
      case 'ptr':  address.writePointer(value); return true;
      case 'enum': {
        var resolved = typeof value === 'string' ? this.enumValue(t.arg, value) : value;
        if (resolved === null) return false;
        writeRaw(address, size, resolved);
        return true;
      }
      // same rule as the rest of the tool: only kinds whose bytes the property owns outright.
      // strings, arrays and maps carry allocator state a plain write corrupts.
      default: return false;
    }
  },

  // getter and setter per property, inherited ones included. nothing reads until you
  // touch a field.
  wrap: function (address, path) {
    var record = this.type(path);
    if (!record) throw new Error('unknown type: ' + path);

    var self = this;
    var view = { $address: address, $type: record, $path: path };

    var chain = [];
    for (var walk = record; walk; walk = walk.super ? self.types[walk.super] : null)
      chain.push(walk);

    // nearest declaration wins, same as C++ would do it
    for (var i = chain.length - 1; i >= 0; i--) {
      var props = chain[i].props || [];
      for (var j = 0; j < props.length; j++) {
        (function (prop) {
          Object.defineProperty(view, prop.n, {
            enumerable: true,
            configurable: true,
            get: function () {
              var at = address.add(prop.o);
              if (prop.mask !== undefined) return (at.readU8() & prop.mask) !== 0;
              return self.readAt(at, prop.t, prop.s);
            },
            set: function (value) {
              var at = address.add(prop.o);
              // one bit, not the byte. CharacterMovementComponent packs seven
              // bools into 0x02E8; writing it whole clears six of them.
              if (prop.mask !== undefined) {
                var byte = at.readU8();
                at.writeU8(value ? (byte | prop.mask) : (byte & ~prop.mask));
                return;
              }
              if (!self.writeAt(at, prop.t, value, prop.s))
                throw new Error(prop.n + ' (' + prop.t + ') cannot be written safely');
            }
          });
        })(props[j]);
      }
    }
    return view;
  },

  // module-relative -> absolute. every address in the dump is stored that way.
  rva: function (offset) {
    return this.base.add(offset);
  },

  // A UFunction's native implementation, if it has one. Script-only ones point at the
  // shared interpreter thunk, so those give null instead of a misleading address.
  nativeOf: function (path, name) {
    var record = this.type(path);
    while (record) {
      var funcs = record.funcs || [];
      for (var i = 0; i < funcs.length; i++) {
        if (funcs[i].n !== name) continue;
        if (!funcs[i].rva) return null;
        if (!(funcs[i].flags & 0x400)) return null;   // FUNC_Native
        return this.rva(funcs[i].rva);
      }
      record = record.super ? this.types[record.super] : null;
    }
    return null;
  },

  dump: function (address, path) {
    var view = this.wrap(address, path);
    var out = {};
    for (var key in view) if (key.charAt(0) !== '$') {
      try { out[key] = view[key]; } catch (e) { out[key] = '<' + e.message + '>'; }
    }
    return out;
  }
};

if (typeof module !== 'undefined') module.exports = Zircon;
if (typeof global !== 'undefined') global.Zircon = Zircon;
)JS";

} // namespace

EmitResult EmitFridaJs(const ir::Dump& dump, const EmitOptions& options) {
    EmitResult result;

    if (dump.header.partial && !options.allow_partial) {
        result.error = "the dump is partial (no object data), so there is nothing for a "
                       "script to bind to; pass allow_partial to emit anyway";
        return result;
    }

    std::string json;
    json.reserve(1u << 20);
    json += "{\"meta\":{";
    json += std::format("\"tool\":{},", JsString(dump.header.tool_version));
    json += std::format("\"created\":{},", JsString(dump.header.created_utc));
    json += std::format("\"process\":{},", JsString(dump.header.source.process));
    json += std::format("\"module\":{},", JsString(dump.header.source.main_module.empty()
                                                       ? dump.header.source.process
                                                       : dump.header.source.main_module));
    json += std::format("\"engine\":{},", JsString(dump.header.engine.version));
    json += std::format("\"confidence\":{:.2f}", dump.header.engine.confidence);
    json += "},";

    json += "\"offsets\":{";
    bool first_offset = true;
    for (const auto& offset : dump.header.offsets) {
        if (!first_offset) json += ",";
        first_offset = false;
        json += std::format("{}:{}", JsString(offset.name), offset.value);
    }
    json += "},";

    std::size_t type_count = 0;
    std::size_t enum_count = 0;

    json += "\"enums\":{";
    bool first_enum = true;
    for (const auto& package : dump.packages) {
        if (!options.package_filter.empty() &&
            package.name.find(options.package_filter) == std::string::npos)
            continue;
        for (const auto& record : package.enums) {
            if (!first_enum) json += ",";
            first_enum = false;
            ++enum_count;
            json += std::format("\n{}:[", JsString(record.path));
            bool first_value = true;
            for (const auto& value : record.values) {
                if (!first_value) json += ",";
                first_value = false;
                json += std::format("[{},{}]", JsString(value.name), value.value);
            }
            json += "]";
        }
    }
    json += "},";

    json += "\"types\":{";
    bool first_type = true;
    for (const auto& package : dump.packages) {
        if (!options.package_filter.empty() &&
            package.name.find(options.package_filter) == std::string::npos)
            continue;
        for (const auto& record : package.classes) {
            if (!first_type) json += ",";
            first_type = false;
            ++type_count;
            AppendType(json, record, true);
        }
        for (const auto& record : package.structs) {
            if (!first_type) json += ",";
            first_type = false;
            ++type_count;
            AppendType(json, record, false);
        }
    }
    json += "}}";

    if (type_count == 0) {
        result.warnings.push_back(options.package_filter.empty()
            ? "the dump contains no types"
            : "no package matched the filter '" + options.package_filter + "'");
    }

    std::string js;
    js += "// Generated by Zircon - Unreal Engine reflection toolkit.\n";
    js += std::format("// Source: {}\n", dump.header.source.process.empty()
                                             ? "unknown" : dump.header.source.process);
    js += std::format("// Engine: {}\n", dump.header.engine.version.empty()
                                             ? "unknown" : dump.header.engine.version);
    js += std::format("// {} types, {} enums.\n", type_count, enum_count);
    js += "//\n";
    js += "//   frida -l zircon.js -n " +
          (dump.header.source.process.empty() ? std::string("YourGame.exe")
                                              : dump.header.source.process) + "\n";
    js += "//\n";
    js += "//   var actor = Zircon.wrap(ptr('0x...'), '/Script/Engine.Actor');\n";
    js += "//   console.log(actor.MaxSpeed);\n";
    js += "//   actor.MaxSpeed = 1337;\n";
    js += "//\n";
    js += "// Addresses in here are module-relative. Zircon.base is resolved at load time,\n";
    js += "// so ASLR is not your problem.\n";
    js += "\n";
    js += "'use strict';\n\n";
    js += "var DATA = JSON.parse(" + EmbedJson(json) + ");\n";
    js += kRuntime;

    const std::string path = options.out_dir + "/zircon.js";
    std::string error;
    if (!util::WriteFile(path, js, error)) {
        result.error = std::move(error);
        return result;
    }

    result.files.push_back(path);
    return result;
}

} // namespace zircon::emit
