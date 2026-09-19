#include "ir/Json.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace zircon::ir {
namespace {

// ===================================================================================
// Writing
// ===================================================================================

class Writer {
public:
    explicit Writer(bool pretty) : pretty_(pretty) {}

    std::string Take() { return std::move(out_); }

    void BeginObject() { PrepareValue(); out_ += '{'; Push(Context::Object); }
    void EndObject()   { Pop('}'); }
    void BeginArray()  { PrepareValue(); out_ += '['; Push(Context::Array); }
    void EndArray()    { Pop(']'); }

    void Key(std::string_view key) {
        if (has_items_.back()) out_ += ',';
        Newline();
        WriteEscaped(key);
        out_ += ':';
        if (pretty_) out_ += ' ';
        has_items_.back() = true;
        expecting_value_  = true;
    }

    void String(std::string_view text) { PrepareValue(); WriteEscaped(text); }
    void Bool(bool value) { PrepareValue(); out_ += value ? "true" : "false"; }

    void Int(std::int64_t value)   { PrepareValue(); AppendNumber(value); }
    void UInt(std::uint64_t value) { PrepareValue(); AppendNumber(value); }

    // Shortest round-trip form. Fixed precision, printf included, can land on a different
    // float when parsed back.
    void Float(float value) {
        PrepareValue();
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out_.append(buffer, result.ptr);
    }

    // Hex strings, not numbers. That's how people read them, and a 64-bit address written
    // as a JSON number invites consumers in other languages to parse it as a double and
    // quietly drop the low bits.
    void Hex(std::uint64_t value) {
        PrepareValue();
        char buffer[32];
        const auto result = std::to_chars(buffer + 2, buffer + sizeof(buffer), value, 16);
        buffer[0] = '0';
        buffer[1] = 'x';
        out_ += '"';
        out_.append(buffer, result.ptr);
        out_ += '"';
    }

private:
    enum class Context { Object, Array };

    void Push(Context context) {
        stack_.push_back(context);
        has_items_.push_back(false);
        ++indent_;
    }

    void Pop(char closer) {
        --indent_;
        if (has_items_.back()) Newline();
        out_ += closer;
        stack_.pop_back();
        has_items_.pop_back();
    }

    void PrepareValue() {
        if (expecting_value_) { expecting_value_ = false; return; }
        if (stack_.empty() || stack_.back() != Context::Array) return;

        if (has_items_.back()) out_ += ',';
        Newline();
        has_items_.back() = true;
    }

    void Newline() {
        if (!pretty_) return;
        out_ += '\n';
        out_.append(static_cast<std::size_t>(indent_) * 2, ' ');
    }

    template <typename T>
    void AppendNumber(T value) {
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        out_.append(buffer, result.ptr);
    }

    void WriteEscaped(std::string_view text) {
        out_ += '"';
        for (const char raw : text) {
            const auto c = static_cast<unsigned char>(raw);
            switch (c) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\b': out_ += "\\b";  break;
                case '\f': out_ += "\\f";  break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                default:
                    // '/' deliberately unescaped. Legal either way, and UE paths are mostly
                    // slashes; escaping them bloats every dump and ruins readability.
                    if (c < 0x20) {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                        out_ += buffer;
                    } else {
                        out_ += raw;   // UTF-8 continuation bytes pass through unchanged
                    }
                    break;
            }
        }
        out_ += '"';
    }

    std::string        out_;
    bool               pretty_;
    int                indent_{0};
    bool               expecting_value_{false};
    std::vector<Context> stack_;
    std::vector<bool>    has_items_;
};

// Writers below omit any field equal to its default. Round-trip stays lossless because
// parsing starts from a default-constructed object and only overwrites keys that are
// present, which also handles a non-zero default like Property::array_dim = 1 with no
// special case. Most fields sit at their defaults on a real dump, so this cuts a large
// fraction of the output.
void WriteTypeRef(Writer& writer, const TypeRef& type) {
    const TypeRef defaults;
    writer.BeginObject();

    // Always emitted. It's the discriminator, and dumps read far better when every type
    // node says what it is.
    writer.Key("kind");
    writer.String(ToString(type.kind));

    if (!type.name.empty()) { writer.Key("name"); writer.String(type.name); }
    if (!type.raw.empty())  { writer.Key("raw");  writer.String(type.raw); }
    if (type.size != defaults.size) { writer.Key("size"); writer.Int(type.size); }

    if (!type.params.empty()) {
        writer.Key("params");
        writer.BeginArray();
        for (const auto& param : type.params) WriteTypeRef(writer, param);
        writer.EndArray();
    }
    writer.EndObject();
}

void WriteStringArray(Writer& writer, std::string_view key,
                      const std::vector<std::string>& values) {
    if (values.empty()) return;
    writer.Key(key);
    writer.BeginArray();
    for (const auto& value : values) writer.String(value);
    writer.EndArray();
}

void WriteProperty(Writer& writer, const Property& property) {
    const Property defaults;
    writer.BeginObject();
    writer.Key("name"); writer.String(property.name);
    writer.Key("type"); WriteTypeRef(writer, property.type);

    if (property.offset    != defaults.offset)    { writer.Key("offset");    writer.Int(property.offset); }
    if (property.size      != defaults.size)      { writer.Key("size");      writer.Int(property.size); }
    if (property.array_dim != defaults.array_dim) { writer.Key("array_dim"); writer.Int(property.array_dim); }
    if (property.flags     != defaults.flags)     { writer.Key("flags");     writer.UInt(property.flags); }
    WriteStringArray(writer, "flag_names", property.flag_names);

    if (property.boxed_offset != defaults.boxed_offset) {
        writer.Key("boxed_offset"); writer.Int(property.boxed_offset);
    }
    if (property.is_static)         { writer.Key("is_static");         writer.Bool(true); }
    if (property.offset_unresolved) { writer.Key("offset_unresolved"); writer.Bool(true); }

    if (property.is_bitfield) {
        writer.Key("is_bitfield"); writer.Bool(true);
        writer.Key("byte_mask");   writer.Int(property.byte_mask);
        writer.Key("field_mask");  writer.Int(property.field_mask);
        writer.Key("bit_index");   writer.Int(property.bit_index);
    }

    // Last: it's the longest, and someone scanning for layout wants the numbers first.
    if (!property.default_value.empty()) {
        writer.Key("default"); writer.String(property.default_value);
    }
    writer.EndObject();
}

void WriteFunction(Writer& writer, const Function& function) {
    const Function defaults;
    writer.BeginObject();
    writer.Key("name"); writer.String(function.name);
    if (function.flags != defaults.flags) { writer.Key("flags"); writer.UInt(function.flags); }
    WriteStringArray(writer, "flag_names", function.flag_names);

    if (!function.params.empty()) {
        writer.Key("params");
        writer.BeginArray();
        for (const auto& param : function.params) {
            const FunctionParam param_defaults;
            writer.BeginObject();
            writer.Key("name"); writer.String(param.name);
            writer.Key("type"); WriteTypeRef(writer, param.type);
            if (param.offset != param_defaults.offset) { writer.Key("offset"); writer.Int(param.offset); }
            if (param.size   != param_defaults.size)   { writer.Key("size");   writer.Int(param.size); }
            if (param.is_return) { writer.Key("is_return"); writer.Bool(true); }
            if (param.is_out)    { writer.Key("is_out");    writer.Bool(true); }
            if (param.is_const)  { writer.Key("is_const");  writer.Bool(true); }
            writer.EndObject();
        }
        writer.EndArray();
    }

    if (function.native_rva != defaults.native_rva) {
        writer.Key("native_rva"); writer.Hex(function.native_rva);
    }
    if (function.token != defaults.token) { writer.Key("token"); writer.UInt(function.token); }
    if (function.shared_body) { writer.Key("shared_body"); writer.Bool(true); }
    if (!function.script.empty()) {
        writer.Key("script");
        writer.BeginArray();
        for (const auto& statement : function.script) {
            writer.BeginObject();
            writer.Key("offset"); writer.Int(static_cast<std::int64_t>(statement.offset));
            if (statement.depth != 0) { writer.Key("depth"); writer.Int(statement.depth); }
            writer.Key("text"); writer.String(statement.text);
            writer.EndObject();
        }
        writer.EndArray();
    }
    if (function.script_complete != defaults.script_complete) {
        writer.Key("script_complete"); writer.Bool(function.script_complete);
    }
    if (function.script_size != defaults.script_size) {
        writer.Key("script_size"); writer.Int(function.script_size);
    }
    writer.EndObject();
}

void WriteStruct(Writer& writer, const Struct& record) {
    const Struct defaults;
    writer.BeginObject();
    writer.Key("name"); writer.String(record.name);
    writer.Key("path"); writer.String(record.path);
    if (!record.super.empty()) { writer.Key("super"); writer.String(record.super); }
    if (!record.name_space.empty()) { writer.Key("namespace"); writer.String(record.name_space); }
    if (record.is_interface) { writer.Key("is_interface"); writer.Bool(true); }
    if (record.is_abstract)  { writer.Key("is_abstract");  writer.Bool(true); }
    if (record.is_valuetype) { writer.Key("is_valuetype"); writer.Bool(true); }
    if (record.is_generic)   { writer.Key("is_generic");   writer.Bool(true); }
    if (record.explicit_layout) { writer.Key("explicit_layout"); writer.Bool(true); }
    if (record.token != defaults.token) { writer.Key("token"); writer.UInt(record.token); }
    if (!record.source.empty()) { writer.Key("source"); writer.String(record.source); }

    if (record.size           != defaults.size)           { writer.Key("size");           writer.Int(record.size); }
    if (record.alignment      != defaults.alignment)      { writer.Key("alignment");      writer.Int(record.alignment); }
    if (record.inherited_size != defaults.inherited_size) { writer.Key("inherited_size"); writer.Int(record.inherited_size); }

    if (record.vtable_rva != defaults.vtable_rva) {
        writer.Key("vtable_rva"); writer.Hex(record.vtable_rva);
    }
    if (record.cpp_prefix != defaults.cpp_prefix) {
        writer.Key("cpp_prefix");
        writer.String(std::string(1, record.cpp_prefix));
    }
    WriteStringArray(writer, "interfaces", record.interfaces);


    if (!record.properties.empty()) {
        writer.Key("properties");
        writer.BeginArray();
        for (const auto& property : record.properties) WriteProperty(writer, property);
        writer.EndArray();
    }
    if (!record.functions.empty()) {
        writer.Key("functions");
        writer.BeginArray();
        for (const auto& function : record.functions) WriteFunction(writer, function);
        writer.EndArray();
    }
    if (!record.accessors.empty()) {
        writer.Key("accessors");
        writer.BeginArray();
        for (const auto& accessor : record.accessors) {
            writer.BeginObject();
            writer.Key("name"); writer.String(accessor.name);
            writer.Key("type"); WriteTypeRef(writer, accessor.type);
            if (!accessor.getter.empty()) { writer.Key("getter"); writer.String(accessor.getter); }
            if (!accessor.setter.empty()) { writer.Key("setter"); writer.String(accessor.setter); }
            if (accessor.flags != 0) { writer.Key("flags"); writer.UInt(accessor.flags); }
            writer.EndObject();
        }
        writer.EndArray();
    }
    writer.EndObject();
}

void WriteEnum(Writer& writer, const Enum& record) {
    const Enum defaults;
    writer.BeginObject();
    writer.Key("name"); writer.String(record.name);
    writer.Key("path"); writer.String(record.path);
    if (record.underlying != defaults.underlying) {
        writer.Key("underlying"); writer.String(record.underlying);
    }
    if (record.is_flags) { writer.Key("is_flags"); writer.Bool(true); }
    if (!record.values_resolved) { writer.Key("values_resolved"); writer.Bool(false); }

    if (!record.values.empty()) {
        writer.Key("values");
        writer.BeginArray();
        for (const auto& value : record.values) {
            writer.BeginObject();
            writer.Key("name");  writer.String(value.name);
            writer.Key("value"); writer.Int(value.value);
            writer.EndObject();
        }
        writer.EndArray();
    }
    writer.EndObject();
}

void WriteHeader(Writer& writer, const Header& header) {
    const SourceInfo  source_defaults;
    const EngineInfo  engine_defaults;

    writer.BeginObject();
    if (!header.tool_version.empty()) { writer.Key("tool_version"); writer.String(header.tool_version); }
    if (!header.created_utc.empty())  { writer.Key("created_utc");  writer.String(header.created_utc); }
    if (header.runtime != Header{}.runtime) {
        writer.Key("runtime"); writer.String(header.runtime);
    }
    if (header.partial) { writer.Key("partial"); writer.Bool(true); }

    writer.Key("source");
    writer.BeginObject();
    if (!header.source.kind.empty())        { writer.Key("kind");        writer.String(header.source.kind); }
    if (!header.source.process.empty())     { writer.Key("process");     writer.String(header.source.process); }
    if (!header.source.main_module.empty()) { writer.Key("main_module"); writer.String(header.source.main_module); }
    if (header.source.module_base != source_defaults.module_base) {
        writer.Key("module_base"); writer.Hex(header.source.module_base);
    }
    if (header.source.image_size != source_defaults.image_size) {
        writer.Key("image_size"); writer.UInt(header.source.image_size);
    }
    writer.EndObject();

    writer.Key("engine");
    writer.BeginObject();
    if (!header.engine.version.empty()) { writer.Key("version"); writer.String(header.engine.version); }
    writer.Key("confidence"); writer.Float(header.engine.confidence);
    if (header.engine.uses_fproperty       != engine_defaults.uses_fproperty)       { writer.Key("uses_fproperty");       writer.Bool(header.engine.uses_fproperty); }
    if (header.engine.chunked_gobjects     != engine_defaults.chunked_gobjects)     { writer.Key("chunked_gobjects");     writer.Bool(header.engine.chunked_gobjects); }
    if (header.engine.chunked_name_pool    != engine_defaults.chunked_name_pool)    { writer.Key("chunked_name_pool");    writer.Bool(header.engine.chunked_name_pool); }
    if (header.engine.case_preserving_name != engine_defaults.case_preserving_name) { writer.Key("case_preserving_name"); writer.Bool(header.engine.case_preserving_name); }
    WriteStringArray(writer, "evidence", header.engine.evidence);
    writer.EndObject();

    if (!header.offsets.empty()) {
        writer.Key("offsets");
        writer.BeginArray();
        for (const auto& offset : header.offsets) {
            writer.BeginObject();
            writer.Key("name");  writer.String(offset.name);
            writer.Key("value"); writer.Int(offset.value);
            writer.EndObject();
        }
        writer.EndArray();
    }
    WriteStringArray(writer, "globals", header.globals);
    WriteStringArray(writer, "sources", header.sources);

    // Written in full, never truncated. On a packed build the disagreements are the finding.
    if (!header.conflicts.empty()) {
        writer.Key("conflicts");
        writer.BeginArray();
        for (const auto& conflict : header.conflicts) {
            writer.BeginObject();
            writer.Key("path");  writer.String(conflict.path);
            if (!conflict.member.empty()) { writer.Key("member"); writer.String(conflict.member); }
            writer.Key("field"); writer.String(conflict.field);
            writer.Key("live");  writer.String(conflict.live);
            writer.Key("other"); writer.String(conflict.other);
            writer.Key("used");  writer.String(conflict.used);
            writer.EndObject();
        }
        writer.EndArray();
    }
    writer.EndObject();
}

// ===================================================================================
// Parsing
// ===================================================================================

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type        type{Type::Null};
    bool        boolean{false};
    std::string text;          // string payload, or the raw number text
    std::size_t offset{0};     // where this value started, so mapping errors stay locatable

    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value* Find(std::string_view key) const {
        const auto it = std::find_if(members.begin(), members.end(),
                                     [&](const auto& entry) { return entry.first == key; });
        return it == members.end() ? nullptr : &it->second;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool Parse(Value& out) {
        SkipWhitespace();
        if (!ParseValue(out, 0)) return false;
        SkipWhitespace();
        if (pos_ != text_.size()) return Fail("trailing content after the top-level value");
        return true;
    }

    const JsonError& error() const { return error_; }

private:
    bool Fail(std::string message) {
        // Keep the first failure. Later ones are its consequences, and the earliest offset
        // is the one that locates the problem.
        if (!failed_) {
            failed_ = true;
            error_.message = std::move(message);
            error_.offset  = pos_;
        }
        return false;
    }

    void SkipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool Literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0)
            return Fail("unrecognised literal");
        pos_ += word.size();
        return true;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxJsonDepth)
            return Fail("nesting deeper than " + std::to_string(kMaxJsonDepth) + " levels");
        if (pos_ >= text_.size()) return Fail("unexpected end of input");

        out.offset = pos_;
        switch (text_[pos_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': out.type = Value::Type::String; return ParseString(out.text);
            case 't': out.type = Value::Type::Bool; out.boolean = true;  return Literal("true");
            case 'f': out.type = Value::Type::Bool; out.boolean = false; return Literal("false");
            case 'n': out.type = Value::Type::Null; return Literal("null");
            default:  out.type = Value::Type::Number; return ParseNumber(out.text);
        }
    }

    bool ParseObject(Value& out, int depth) {
        out.type = Value::Type::Object;
        ++pos_;                        // '{'
        SkipWhitespace();

        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }

        while (true) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return Fail("expected a quoted object key");

            std::string key;
            if (!ParseString(key)) return false;

            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return Fail("expected ':' after the object key");
            ++pos_;

            SkipWhitespace();
            Value member;
            if (!ParseValue(member, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(member));

            SkipWhitespace();
            if (pos_ >= text_.size()) return Fail("unterminated object");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == '}') { ++pos_; return true; }
            return Fail("expected ',' or '}' in object");
        }
    }

    bool ParseArray(Value& out, int depth) {
        out.type = Value::Type::Array;
        ++pos_;                        // '['
        SkipWhitespace();

        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }

        while (true) {
            SkipWhitespace();
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            out.items.push_back(std::move(item));

            SkipWhitespace();
            if (pos_ >= text_.size()) return Fail("unterminated array");
            if (text_[pos_] == ',') { ++pos_; continue; }
            if (text_[pos_] == ']') { ++pos_; return true; }
            return Fail("expected ',' or ']' in array");
        }
    }

    void AppendUtf8(std::string& out, std::uint32_t code_point) {
        if (code_point < 0x80) {
            out += static_cast<char>(code_point);
        } else if (code_point < 0x800) {
            out += static_cast<char>(0xC0 | (code_point >> 6));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
        } else if (code_point < 0x10000) {
            out += static_cast<char>(0xE0 | (code_point >> 12));
            out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code_point >> 18));
            out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code_point & 0x3F));
        }
    }

    bool ParseHex4(std::uint32_t& out) {
        if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9')      value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return Fail("non-hex digit in \\u escape");
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;                        // opening quote
        out.clear();

        while (true) {
            if (pos_ >= text_.size()) return Fail("unterminated string");

            const auto c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') { ++pos_; return true; }

            if (c < 0x20) return Fail("raw control character in string");

            if (c != '\\') { out += text_[pos_++]; continue; }

            ++pos_;                    // backslash
            if (pos_ >= text_.size()) return Fail("unterminated escape");

            switch (text_[pos_]) {
                case '"':  out += '"';  ++pos_; break;
                case '\\': out += '\\'; ++pos_; break;
                case '/':  out += '/';  ++pos_; break;
                case 'b':  out += '\b'; ++pos_; break;
                case 'f':  out += '\f'; ++pos_; break;
                case 'n':  out += '\n'; ++pos_; break;
                case 'r':  out += '\r'; ++pos_; break;
                case 't':  out += '\t'; ++pos_; break;
                case 'u': {
                    ++pos_;
                    std::uint32_t code_point = 0;
                    if (!ParseHex4(code_point)) return false;

                    // A high surrogate means nothing without its low half. Recombine
                    // before encoding or the result isn't UTF-8.
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            std::uint32_t low = 0;
                            if (!ParseHex4(low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF)
                                return Fail("high surrogate not followed by a low surrogate");
                            code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                                         (low - 0xDC00);
                        } else {
                            return Fail("unpaired high surrogate");
                        }
                    } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                        return Fail("unpaired low surrogate");
                    }
                    AppendUtf8(out, code_point);
                    break;
                }
                default: return Fail("unrecognised string escape");
            }
        }
    }

    bool ParseNumber(std::string& out) {
        const std::size_t start = pos_;

        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;

        if (pos_ >= text_.size()) return Fail("expected a number");
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return Fail("expected a number");
        }

        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return Fail("expected digits after the decimal point");
        }

        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            const std::size_t digits = pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
            if (pos_ == digits) return Fail("expected digits in the exponent");
        }

        out.assign(text_.substr(start, pos_ - start));
        return true;
    }

    std::string_view text_;
    std::size_t      pos_{0};
    bool             failed_{false};
    JsonError        error_;
};

// ===================================================================================
// Mapping the parsed tree onto the model
// ===================================================================================

// Numbers stay raw text through parsing and convert here with from_chars. Route a uint64
// through a double and any flag word above 2^53 comes back corrupt, and EPropertyFlags
// genuinely uses the high bits.
template <typename T>
bool ToNumber(std::string_view text, T& out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool ToHex(std::string_view text, std::uint64_t& out) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text.remove_prefix(2);
    if (text.empty()) return false;

    const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

class Mapper {
public:
    bool Map(const Value& root, Dump& dump) { return ReadDump(root, dump); }
    const JsonError& error() const { return error_; }

private:
    bool Fail(const Value& at, std::string message) {
        if (!failed_) {
            failed_ = true;
            error_.message = std::move(message);
            error_.offset  = at.offset;
        }
        return false;
    }

    // Readers below overwrite only what's present, so omitted keys keep the model's own
    // defaults and the writer is free to skip them.
    bool ReadString(const Value& parent, std::string_view key, std::string& out) {
        const Value* value = parent.Find(key);
        if (!value) return true;
        if (value->type != Value::Type::String)
            return Fail(*value, std::string(key) + " must be a string");
        out = value->text;
        return true;
    }

    bool ReadBool(const Value& parent, std::string_view key, bool& out) {
        const Value* value = parent.Find(key);
        if (!value) return true;
        if (value->type != Value::Type::Bool)
            return Fail(*value, std::string(key) + " must be a boolean");
        out = value->boolean;
        return true;
    }

    template <typename T>
    bool ReadNumber(const Value& parent, std::string_view key, T& out) {
        const Value* value = parent.Find(key);
        if (!value) return true;
        if (value->type != Value::Type::Number)
            return Fail(*value, std::string(key) + " must be a number");
        if (!ToNumber(value->text, out))
            return Fail(*value, std::string(key) + " is out of range for its field");
        return true;
    }

    bool ReadHex(const Value& parent, std::string_view key, std::uint64_t& out) {
        const Value* value = parent.Find(key);
        if (!value) return true;
        if (value->type != Value::Type::String)
            return Fail(*value, std::string(key) + " must be a hex string such as \"0x1a2b\"");
        if (!ToHex(value->text, out))
            return Fail(*value, std::string(key) + " is not a valid hex string");
        return true;
    }

    bool ReadStringArray(const Value& parent, std::string_view key,
                         std::vector<std::string>& out) {
        const Value* value = parent.Find(key);
        if (!value) return true;
        if (value->type != Value::Type::Array)
            return Fail(*value, std::string(key) + " must be an array");

        out.clear();
        for (const auto& item : value->items) {
            if (item.type != Value::Type::String)
                return Fail(item, std::string(key) + " must contain only strings");
            out.push_back(item.text);
        }
        return true;
    }

    bool ReadTypeRef(const Value& value, TypeRef& out) {
        if (value.type != Value::Type::Object) return Fail(value, "type must be an object");

        if (const Value* kind = value.Find("kind")) {
            if (kind->type != Value::Type::String) return Fail(*kind, "kind must be a string");
            const auto parsed = TypeKindFromString(kind->text);
            if (!parsed) return Fail(*kind, "unrecognised type kind '" + kind->text + "'");
            out.kind = *parsed;
        }

        if (!ReadString(value, "name", out.name)) return false;
        if (!ReadString(value, "raw",  out.raw))  return false;
        if (!ReadNumber(value, "size", out.size)) return false;

        if (const Value* params = value.Find("params")) {
            if (params->type != Value::Type::Array)
                return Fail(*params, "params must be an array");
            out.params.clear();
            for (const auto& item : params->items) {
                TypeRef param;
                if (!ReadTypeRef(item, param)) return false;
                out.params.push_back(std::move(param));
            }
        }
        return true;
    }

    bool ReadProperty(const Value& value, Property& out) {
        if (value.type != Value::Type::Object)
            return Fail(value, "property must be an object");

        if (!ReadString(value, "name", out.name)) return false;
        if (const Value* type = value.Find("type")) {
            if (!ReadTypeRef(*type, out.type)) return false;
        }
        if (!ReadNumber(value, "offset",    out.offset))    return false;
        if (!ReadNumber(value, "size",      out.size))      return false;
        if (!ReadNumber(value, "array_dim", out.array_dim)) return false;
        if (!ReadNumber(value, "flags",     out.flags))     return false;
        if (!ReadStringArray(value, "flag_names", out.flag_names)) return false;
        if (!ReadBool(value, "is_bitfield", out.is_bitfield)) return false;

        std::uint32_t byte_mask = out.byte_mask;
        std::uint32_t field_mask = out.field_mask;
        if (!ReadNumber(value, "byte_mask",  byte_mask))  return false;
        if (!ReadNumber(value, "field_mask", field_mask)) return false;
        if (byte_mask > 0xFF || field_mask > 0xFF)
            return Fail(value, "byte_mask and field_mask must fit in a byte");
        out.byte_mask  = static_cast<std::uint8_t>(byte_mask);
        out.field_mask = static_cast<std::uint8_t>(field_mask);

        if (!ReadNumber(value, "bit_index", out.bit_index)) return false;
        if (!ReadNumber(value, "boxed_offset", out.boxed_offset)) return false;
        if (!ReadBool(value, "is_static", out.is_static)) return false;
        if (!ReadBool(value, "offset_unresolved", out.offset_unresolved)) return false;
        if (!ReadString(value, "default", out.default_value)) return false;
        return true;
    }

    bool ReadFunction(const Value& value, Function& out) {
        if (value.type != Value::Type::Object)
            return Fail(value, "function must be an object");

        if (!ReadString(value, "name", out.name)) return false;
        if (!ReadNumber(value, "flags", out.flags)) return false;
        if (!ReadStringArray(value, "flag_names", out.flag_names)) return false;

        if (const Value* params = value.Find("params")) {
            if (params->type != Value::Type::Array)
                return Fail(*params, "params must be an array");
            out.params.clear();
            for (const auto& item : params->items) {
                FunctionParam param;
                if (item.type != Value::Type::Object)
                    return Fail(item, "parameter must be an object");
                if (!ReadString(item, "name", param.name)) return false;
                if (const Value* type = item.Find("type")) {
                    if (!ReadTypeRef(*type, param.type)) return false;
                }
                if (!ReadNumber(item, "offset", param.offset)) return false;
                if (!ReadNumber(item, "size",   param.size))   return false;
                if (!ReadBool(item, "is_return", param.is_return)) return false;
                if (!ReadBool(item, "is_out",    param.is_out))    return false;
                if (!ReadBool(item, "is_const",  param.is_const))  return false;
                out.params.push_back(std::move(param));
            }
        }

        if (!ReadHex(value, "native_rva", out.native_rva)) return false;
        if (!ReadNumber(value, "token", out.token)) return false;
        if (!ReadBool(value, "shared_body", out.shared_body)) return false;
        if (!ReadNumber(value, "script_size",  out.script_size))  return false;
        if (!ReadBool(value, "script_complete", out.script_complete)) return false;

        if (const Value* script = value.Find("script")) {
            if (script->type != Value::Type::Array)
                return Fail(*script, "script must be an array");
            for (const auto& item : script->items) {
                if (item.type != Value::Type::Object)
                    return Fail(item, "script statement must be an object");
                ScriptStatement statement;
                std::int64_t offset = 0;
                if (!ReadNumber(item, "offset", offset)) return false;
                statement.offset = static_cast<std::uint32_t>(offset);
                if (!ReadNumber(item, "depth", statement.depth)) return false;
                if (!ReadString(item, "text", statement.text)) return false;
                out.script.push_back(std::move(statement));
            }
        }
        return true;
    }

    bool ReadStruct(const Value& value, Struct& out) {
        if (value.type != Value::Type::Object) return Fail(value, "struct must be an object");

        if (!ReadString(value, "name",  out.name))  return false;
        if (!ReadString(value, "path",  out.path))  return false;
        if (!ReadString(value, "super", out.super)) return false;
        if (!ReadNumber(value, "size",           out.size))           return false;
        if (!ReadNumber(value, "alignment",      out.alignment))      return false;
        if (!ReadNumber(value, "inherited_size", out.inherited_size)) return false;
        if (!ReadHex(value, "vtable_rva", out.vtable_rva)) return false;

        std::string prefix;
        if (!ReadString(value, "cpp_prefix", prefix)) return false;
        if (prefix.size() > 1) return Fail(value, "cpp_prefix must be a single character");
        if (!prefix.empty()) out.cpp_prefix = prefix[0];

        if (!ReadString(value, "namespace", out.name_space)) return false;
        if (!ReadString(value, "source", out.source)) return false;
        if (!ReadBool(value, "is_interface", out.is_interface)) return false;
        if (!ReadBool(value, "is_abstract",  out.is_abstract))  return false;
        if (!ReadBool(value, "is_valuetype", out.is_valuetype)) return false;
        if (!ReadBool(value, "is_generic",   out.is_generic))   return false;
        if (!ReadBool(value, "explicit_layout", out.explicit_layout)) return false;
        if (!ReadNumber(value, "token", out.token)) return false;
        if (!ReadStringArray(value, "interfaces", out.interfaces)) return false;

        if (const Value* properties = value.Find("properties")) {
            if (properties->type != Value::Type::Array)
                return Fail(*properties, "properties must be an array");
            for (const auto& item : properties->items) {
                Property property;
                if (!ReadProperty(item, property)) return false;
                out.properties.push_back(std::move(property));
            }
        }
        if (const Value* functions = value.Find("functions")) {
            if (functions->type != Value::Type::Array)
                return Fail(*functions, "functions must be an array");
            for (const auto& item : functions->items) {
                Function function;
                if (!ReadFunction(item, function)) return false;
                out.functions.push_back(std::move(function));
            }
        }
        if (const Value* accessors = value.Find("accessors")) {
            if (accessors->type != Value::Type::Array)
                return Fail(*accessors, "accessors must be an array");
            for (const auto& item : accessors->items) {
                if (item.type != Value::Type::Object)
                    return Fail(item, "accessor must be an object");
                Accessor accessor;
                if (!ReadString(item, "name", accessor.name)) return false;
                if (const Value* type = item.Find("type")) {
                    if (!ReadTypeRef(*type, accessor.type)) return false;
                }
                if (!ReadString(item, "getter", accessor.getter)) return false;
                if (!ReadString(item, "setter", accessor.setter)) return false;
                if (!ReadNumber(item, "flags", accessor.flags)) return false;
                out.accessors.push_back(std::move(accessor));
            }
        }
        return true;
    }

    bool ReadEnum(const Value& value, Enum& out) {
        if (value.type != Value::Type::Object) return Fail(value, "enum must be an object");

        if (!ReadString(value, "name",       out.name))       return false;
        if (!ReadString(value, "path",       out.path))       return false;
        if (!ReadString(value, "underlying", out.underlying)) return false;
        if (!ReadBool(value, "is_flags", out.is_flags)) return false;
        if (!ReadBool(value, "values_resolved", out.values_resolved)) return false;

        if (const Value* values = value.Find("values")) {
            if (values->type != Value::Type::Array)
                return Fail(*values, "enum values must be an array");
            for (const auto& item : values->items) {
                EnumValue entry;
                if (item.type != Value::Type::Object)
                    return Fail(item, "enum value must be an object");
                if (!ReadString(item, "name",  entry.name))  return false;
                if (!ReadNumber(item, "value", entry.value)) return false;
                out.values.push_back(std::move(entry));
            }
        }
        return true;
    }

    bool ReadHeader(const Value& value, Header& out) {
        if (value.type != Value::Type::Object) return Fail(value, "header must be an object");

        if (!ReadString(value, "tool_version", out.tool_version)) return false;
        if (!ReadString(value, "created_utc",  out.created_utc))  return false;
        if (!ReadString(value, "runtime", out.runtime)) return false;
        if (!ReadBool(value, "partial", out.partial)) return false;

        if (const Value* source = value.Find("source")) {
            if (source->type != Value::Type::Object)
                return Fail(*source, "source must be an object");
            if (!ReadString(*source, "kind",        out.source.kind))        return false;
            if (!ReadString(*source, "process",     out.source.process))     return false;
            if (!ReadString(*source, "main_module", out.source.main_module)) return false;
            if (!ReadHex(*source, "module_base", out.source.module_base))    return false;
            if (!ReadNumber(*source, "image_size", out.source.image_size))   return false;
        }

        if (const Value* engine = value.Find("engine")) {
            if (engine->type != Value::Type::Object)
                return Fail(*engine, "engine must be an object");
            if (!ReadString(*engine, "version", out.engine.version)) return false;
            if (!ReadNumber(*engine, "confidence", out.engine.confidence)) return false;
            if (!ReadBool(*engine, "uses_fproperty",       out.engine.uses_fproperty))       return false;
            if (!ReadBool(*engine, "chunked_gobjects",     out.engine.chunked_gobjects))     return false;
            if (!ReadBool(*engine, "chunked_name_pool",    out.engine.chunked_name_pool))    return false;
            if (!ReadBool(*engine, "case_preserving_name", out.engine.case_preserving_name)) return false;
            if (!ReadStringArray(*engine, "evidence", out.engine.evidence)) return false;
        }

        if (const Value* offsets = value.Find("offsets")) {
            if (offsets->type != Value::Type::Array)
                return Fail(*offsets, "offsets must be an array");
            for (const auto& item : offsets->items) {
                DerivedOffset offset;
                if (item.type != Value::Type::Object)
                    return Fail(item, "offset must be an object");
                if (!ReadString(item, "name",  offset.name))  return false;
                if (!ReadNumber(item, "value", offset.value)) return false;
                out.offsets.push_back(std::move(offset));
            }
        }
        if (!ReadStringArray(value, "globals", out.globals)) return false;
        if (!ReadStringArray(value, "sources", out.sources)) return false;

        if (const auto* conflicts = value.Find("conflicts")) {
            if (conflicts->type != Value::Type::Array)
                return Fail(*conflicts, "conflicts must be an array");
            for (const auto& item : conflicts->items) {
                if (item.type != Value::Type::Object)
                    return Fail(item, "a conflict must be an object");
                Conflict conflict;
                if (!ReadString(item, "path",   conflict.path))   return false;
                if (!ReadString(item, "member", conflict.member)) return false;
                if (!ReadString(item, "field",  conflict.field))  return false;
                if (!ReadString(item, "live",   conflict.live))   return false;
                if (!ReadString(item, "other",  conflict.other))  return false;
                if (!ReadString(item, "used",   conflict.used))   return false;
                out.conflicts.push_back(std::move(conflict));
            }
        }
        return true;
    }

    bool ReadDump(const Value& root, Dump& out) {
        if (root.type != Value::Type::Object)
            return Fail(root, "the top-level value must be an object");

        if (!ReadNumber(root, "schema_version", out.schema_version)) return false;

        if (const Value* header = root.Find("header")) {
            if (!ReadHeader(*header, out.header)) return false;
        }
        if (!ReadStringArray(root, "names", out.names)) return false;

        if (const Value* packages = root.Find("packages")) {
            if (packages->type != Value::Type::Array)
                return Fail(*packages, "packages must be an array");

            for (const auto& item : packages->items) {
                Package package;
                if (item.type != Value::Type::Object)
                    return Fail(item, "package must be an object");
                if (!ReadString(item, "name", package.name)) return false;

                // is_class isn't serialized. The array a record sits in already says it,
                // and writing it too would let a file claim is_class=true inside "structs"
                // with nothing to catch the contradiction. Set from the containing array,
                // so the two can't disagree.
                const auto read_structs = [&](std::string_view key,
                                              std::vector<Struct>& into, bool is_class) {
                    const Value* array = item.Find(key);
                    if (!array) return true;
                    if (array->type != Value::Type::Array)
                        return Fail(*array, std::string(key) + " must be an array");
                    for (const auto& entry : array->items) {
                        Struct record;
                        if (!ReadStruct(entry, record)) return false;
                        record.is_class = is_class;
                        into.push_back(std::move(record));
                    }
                    return true;
                };

                if (!read_structs("classes", package.classes, true))  return false;
                if (!read_structs("structs", package.structs, false)) return false;

                if (const Value* enums = item.Find("enums")) {
                    if (enums->type != Value::Type::Array)
                        return Fail(*enums, "enums must be an array");
                    for (const auto& entry : enums->items) {
                        Enum record;
                        if (!ReadEnum(entry, record)) return false;
                        package.enums.push_back(std::move(record));
                    }
                }
                out.packages.push_back(std::move(package));
            }
        }
        return true;
    }

    bool      failed_{false};
    JsonError error_;
};

} // namespace

// ===================================================================================
// Public entry points
// ===================================================================================

std::string WriteJsonString(const Dump& dump, bool pretty) {
    Writer writer(pretty);

    writer.BeginObject();
    writer.Key("schema_version");
    writer.Int(dump.schema_version);

    writer.Key("header");
    WriteHeader(writer, dump.header);

    if (!dump.names.empty()) {
        writer.Key("names");
        writer.BeginArray();
        for (const auto& name : dump.names) writer.String(name);
        writer.EndArray();
    }

    writer.Key("packages");
    writer.BeginArray();
    for (const auto& package : dump.packages) {
        writer.BeginObject();
        writer.Key("name"); writer.String(package.name);

        if (!package.classes.empty()) {
            writer.Key("classes");
            writer.BeginArray();
            for (const auto& record : package.classes) WriteStruct(writer, record);
            writer.EndArray();
        }
        if (!package.structs.empty()) {
            writer.Key("structs");
            writer.BeginArray();
            for (const auto& record : package.structs) WriteStruct(writer, record);
            writer.EndArray();
        }
        if (!package.enums.empty()) {
            writer.Key("enums");
            writer.BeginArray();
            for (const auto& record : package.enums) WriteEnum(writer, record);
            writer.EndArray();
        }
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();

    std::string text = writer.Take();
    text += '\n';
    return text;
}

bool WriteJsonFile(const Dump& dump, std::string_view path, std::string& error,
                   bool pretty) {
    std::ofstream file{std::string(path), std::ios::binary};
    if (!file) {
        error = "cannot open '" + std::string(path) + "' for writing";
        return false;
    }

    const std::string text = WriteJsonString(dump, pretty);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        error = "failed while writing '" + std::string(path) + "'";
        return false;
    }
    return true;
}

JsonExpected<Dump> ParseJson(std::string_view text) {
    Parser parser(text);
    Value  root;
    if (!parser.Parse(root)) return parser.error();

    Dump   dump;
    Mapper mapper;
    if (!mapper.Map(root, dump)) return mapper.error();
    return dump;
}

JsonExpected<Dump> ReadJsonFile(std::string_view path) {
    std::ifstream file{std::string(path), std::ios::binary};
    if (!file) return JsonError{"cannot open '" + std::string(path) + "'", 0};

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return ParseJson(buffer.str());
}

JsonExpected<Header> ReadJsonHeaderFile(std::string_view path) {
    // A megabyte is far past any header this tool writes and still nothing next to the
    // dumps it writes them into.
    constexpr std::size_t kPrefix = 1024 * 1024;

    std::ifstream file{std::string(path), std::ios::binary};
    if (!file) return JsonError{"cannot open '" + std::string(path) + "'", 0};

    std::string prefix(kPrefix, '\0');
    file.read(prefix.data(), static_cast<std::streamsize>(kPrefix));
    prefix.resize(static_cast<std::size_t>(file.gcount()));

    // Find the top-level "header" key, tracking strings so the word inside an evidence
    // line can't be mistaken for it.
    std::size_t depth = 0, start = std::string::npos;
    bool in_string = false, escaped = false;

    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char c = prefix[i];
        if (in_string) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  in_string = false;
            continue;
        }
        if (c == '"') {
            if (depth == 1 && prefix.compare(i, 9, "\"header\":") == 0) {
                const std::size_t brace = prefix.find('{', i + 9);
                if (brace == std::string::npos) break;
                start = brace;
                break;
            }
            in_string = true;
            continue;
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (depth == 0) break;
            --depth;
        }
    }

    if (start == std::string::npos)
        return JsonError{"no header in the first " + std::to_string(prefix.size()) +
                         " bytes of '" + std::string(path) + "'", 0};

    // Brace-match the header object itself, again respecting strings.
    std::size_t nesting = 0, end = std::string::npos;
    in_string = false;
    escaped   = false;
    for (std::size_t i = start; i < prefix.size(); ++i) {
        const char c = prefix[i];
        if (in_string) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') ++nesting;
        else if (c == '}' && --nesting == 0) { end = i + 1; break; }
    }

    if (end == std::string::npos)
        return JsonError{"the header in '" + std::string(path) + "' is cut off", start};

    // Wrap it in a minimal document and hand it to the real parser. One reader.
    std::string document = "{\"schema_version\":";
    document += std::to_string(kSchemaVersion);
    document += ",\"header\":";
    document.append(prefix, start, end - start);
    document += ",\"packages\":[]}";

    auto parsed = ParseJson(document);
    if (!parsed) return parsed.error();
    return parsed.value().header;
}

} // namespace zircon::ir
