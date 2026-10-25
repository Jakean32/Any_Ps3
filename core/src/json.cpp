#include "any_ps3/json.h"
#include <cstdlib>

namespace any_ps3 {

JsonValue JsonValue::make_bool(bool v) { JsonValue j; j.kind = Kind::Bool; j.b = v; return j; }
JsonValue JsonValue::make_int(long long v) { JsonValue j; j.kind = Kind::Int; j.i = v; return j; }
JsonValue JsonValue::make_str(std::string v) { JsonValue j; j.kind = Kind::Str; j.s = std::move(v); return j; }
JsonValue JsonValue::make_arr() { JsonValue j; j.kind = Kind::Arr; return j; }
JsonValue JsonValue::make_obj() { JsonValue j; j.kind = Kind::Obj; return j; }

JsonValue* JsonValue::find(const std::string& key) {
    for (auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}
const JsonValue* JsonValue::find(const std::string& key) const {
    for (auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}
JsonValue& JsonValue::set(const std::string& key, JsonValue v) {
    for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return kv.second; }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
}
std::string JsonValue::get_str(const std::string& key, const std::string& def) const {
    const JsonValue* v = find(key);
    return (v && v->kind == Kind::Str) ? v->s : def;
}
long long JsonValue::get_int(const std::string& key, long long def) const {
    const JsonValue* v = find(key);
    return (v && v->kind == Kind::Int) ? v->i : def;
}
bool JsonValue::get_bool(const std::string& key, bool def) const {
    const JsonValue* v = find(key);
    return (v && v->kind == Kind::Bool) ? v->b : def;
}

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else out.push_back((char)c);
        }
    }
    return out;
}

static void dump_impl(const JsonValue& v, std::string& out) {
    switch (v.kind) {
        case JsonValue::Kind::Null: out += "null"; break;
        case JsonValue::Kind::Bool: out += v.b ? "true" : "false"; break;
        case JsonValue::Kind::Int: out += std::to_string(v.i); break;
        case JsonValue::Kind::Str: out += '"'; out += json_escape(v.s); out += '"'; break;
        case JsonValue::Kind::Arr: {
            out += '[';
            bool first = true;
            for (const auto& e : v.arr) {
                if (!first) out += ',';
                first = false;
                dump_impl(e, out);
            }
            out += ']';
            break;
        }
        case JsonValue::Kind::Obj: {
            out += '{';
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) out += ',';
                first = false;
                out += '"';
                out += json_escape(kv.first);
                out += "\":";
                dump_impl(kv.second, out);
            }
            out += '}';
            break;
        }
    }
}

std::string JsonValue::dump() const {
    std::string out;
    dump_impl(*this, out);
    return out;
}

// ---- parser ----
namespace {

struct JParser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    void skip_ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool fail(const std::string& m) { err = m; return false; }

    bool parse_value(JsonValue& out) {
        if (++depth > 64) return fail("nesting too deep");
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        char c = *p;
        if (c == '{') return parse_obj(out);
        if (c == '[') return parse_arr(out);
        if (c == '"') { out.kind = JsonValue::Kind::Str; return parse_string(out.s); }
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        return parse_number(out);
    }

    bool parse_null(JsonValue& out) {
        if (end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') {
            p += 4;
            out.kind = JsonValue::Kind::Null;
            return true;
        }
        return fail("bad literal");
    }

    bool parse_bool(JsonValue& out) {
        if (end - p >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') {
            p += 4; out.kind = JsonValue::Kind::Bool; out.b = true; return true;
        }
        if (end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') {
            p += 5; out.kind = JsonValue::Kind::Bool; out.b = false; return true;
        }
        return fail("bad literal");
    }

    bool parse_number(JsonValue& out) {
        const char* start = p;
        if (p < end && (*p == '-')) p++;
        if (p >= end || !(*p >= '0' && *p <= '9')) return fail("invalid number");
        while (p < end && *p >= '0' && *p <= '9') p++;
        // Integers only, by design (floats are not used by Any_Ps3 manifests).
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E'))
            return fail("floating point numbers are not supported");
        std::string num(start, p);
        out.kind = JsonValue::Kind::Int;
        out.i = strtoll(num.c_str(), nullptr, 10);
        return true;
    }

    static void utf8_push(std::string& s, uint32_t cp) {
        if (cp < 0x80) s.push_back((char)cp);
        else if (cp < 0x800) {
            s.push_back((char)(0xC0 | (cp >> 6)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back((char)(0xE0 | (cp >> 12)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            s.push_back((char)(0xF0 | (cp >> 18)));
            s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string& out) {
        out.clear();
        p++; // opening quote
        while (p < end) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) return fail("bad \\u escape");
                        uint32_t v = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = p[i];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= (uint32_t)(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= (uint32_t)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= (uint32_t)(h - 'A' + 10);
                            else return fail("bad \\u escape");
                        }
                        p += 4;
                        utf8_push(out, v);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                out.push_back((char)c);
            }
        }
        return fail("unterminated string");
    }

    bool parse_obj(JsonValue& out) {
        out = JsonValue::make_obj();
        p++; // '{'
        skip_ws();
        if (p < end && *p == '}') { p++; return true; }
        while (true) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected object key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            p++;
            JsonValue v;
            if (!parse_value(v)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_arr(JsonValue& out) {
        out = JsonValue::make_arr();
        p++; // '['
        skip_ws();
        if (p < end && *p == ']') { p++; return true; }
        while (true) {
            JsonValue v;
            if (!parse_value(v)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == ']') { p++; return true; }
            return fail("expected ',' or ']'");
        }
    }
};

} // namespace

bool JsonValue::parse(const std::string& text, JsonValue& out, std::string& err) {
    JParser jp{text.data(), text.data() + text.size(), {}};
    if (!jp.parse_value(out)) { err = jp.err; return false; }
    jp.skip_ws();
    if (jp.p != jp.end) { err = "trailing characters"; return false; }
    return true;
}

} // namespace any_ps3
