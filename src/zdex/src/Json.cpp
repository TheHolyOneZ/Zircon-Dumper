#include "zdex/Json.h"

#include <cmath>
#include <cstdlib>

namespace zircon::zdex {
namespace {

const JsonValue& NullValue() {
    static const JsonValue null;
    return null;
}

class Reader {
public:
    Reader(std::string_view text, std::string& error) : text_(text), error_(error) {}

    bool Parse(JsonValue& out) {
        Skip();
        if (!Value(out, 0)) return false;
        Skip();
        return true;    // trailing bytes are tolerated; a proxy may append whitespace
    }

private:
    void Skip() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool Fail(const char* what) {
        if (error_.empty())
            error_ = std::string(what) + " at offset " + std::to_string(pos_);
        return false;
    }

    bool Literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) return false;
        pos_ += word.size();
        return true;
    }

    bool String(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected a string");
        ++pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) break;

            const char esc = text_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return Fail("bad \\u escape");
                    }
                    // Surrogate pairs, because a server message may contain an emoji and
                    // half a pair written out as UTF-8 is invalid.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                        text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                        unsigned low = 0;
                        bool ok = true;
                        for (int i = 0; i < 4; ++i) {
                            const char h = text_[pos_ + 2 + i];
                            low <<= 4;
                            if (h >= '0' && h <= '9')      low |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') low |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') low |= static_cast<unsigned>(h - 'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos_ += 6;
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool Value(JsonValue& out, int depth) {
        if (depth > 32) return Fail("nested too deep");
        if (pos_ >= text_.size()) return Fail("unexpected end");

        switch (text_[pos_]) {
            case 'n': out.kind = JsonValue::Kind::Null;
                      return Literal("null") || Fail("expected null");
            case 't': out.kind = JsonValue::Kind::Bool; out.boolean = true;
                      return Literal("true") || Fail("expected true");
            case 'f': out.kind = JsonValue::Kind::Bool; out.boolean = false;
                      return Literal("false") || Fail("expected false");
            case '"': out.kind = JsonValue::Kind::String;
                      return String(out.text);
            case '[': return Array(out, depth);
            case '{': return Object(out, depth);
            default:  return Number(out);
        }
    }

    bool Number(JsonValue& out) {
        const std::size_t start = pos_;

        // A number has to *start* like one. Without this the scanner happily accepts the
        // exponent characters on their own, so Cloudflare's plain-text "error code: 1010"
        // parses as the number 0 and the caller is told the server sent valid JSON with
        // no fields in it, instead of being told it never reached the server.
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9')
            return Fail("expected a value");

        bool any = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') { ++pos_; any = true; }
            else break;
        }
        if (!any) return Fail("expected a value");

        out.kind = JsonValue::Kind::Number;
        out.number = std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr);
        return true;
    }

    bool Array(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Array;
        ++pos_;                       // [
        Skip();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            JsonValue item;
            Skip();
            if (!Value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            Skip();
            if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return true; }
            return Fail("expected , or ] in array");
        }
    }

    bool Object(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Object;
        ++pos_;                       // {
        Skip();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            Skip();
            std::string key;
            if (!String(key)) return false;
            Skip();
            if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected :");
            ++pos_;
            Skip();
            JsonValue value;
            if (!Value(value, depth + 1)) return false;
            out.fields[key] = std::move(value);
            Skip();
            if (pos_ < text_.size() && text_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return true; }
            return Fail("expected , or } in object");
        }
    }

    std::string_view text_;
    std::string& error_;
    std::size_t pos_{0};
};

} // namespace

const JsonValue& JsonValue::operator[](std::string_view key) const {
    if (kind != Kind::Object) return NullValue();
    const auto it = fields.find(std::string(key));
    return it == fields.end() ? NullValue() : it->second;
}

std::string JsonValue::Str(std::string_view key, std::string_view fallback) const {
    const JsonValue& v = (*this)[key];
    return v.kind == Kind::String ? v.text : std::string(fallback);
}

std::int64_t JsonValue::Int(std::string_view key, std::int64_t fallback) const {
    const JsonValue& v = (*this)[key];
    if (v.kind == Kind::Number) return static_cast<std::int64_t>(v.number);
    // Some proxies stringify numbers; accept that rather than reporting nothing.
    if (v.kind == Kind::String && !v.text.empty()) return std::strtoll(v.text.c_str(), nullptr, 10);
    return fallback;
}

bool JsonValue::Bool(std::string_view key, bool fallback) const {
    const JsonValue& v = (*this)[key];
    if (v.kind == Kind::Bool) return v.boolean;
    if (v.kind == Kind::Number) return v.number != 0.0;
    return fallback;
}

std::vector<std::int64_t> JsonValue::IntArray(std::string_view key) const {
    std::vector<std::int64_t> out;
    const JsonValue& v = (*this)[key];
    if (v.kind != Kind::Array) return out;
    out.reserve(v.items.size());
    for (const auto& item : v.items)
        if (item.kind == Kind::Number) out.push_back(static_cast<std::int64_t>(item.number));
    return out;
}

bool ParseJson(std::string_view text, JsonValue& out, std::string& error) {
    out = JsonValue{};
    error.clear();
    if (text.empty()) { error = "empty response"; return false; }
    Reader reader(text, error);
    return reader.Parse(out);
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

} // namespace zircon::zdex
