#include "minijson.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace anyps3 {

const JVal& JVal::at(const std::string& key) const {
    static const JVal nullv;
    if (type != Obj || !obj) return nullv;
    auto it = obj->find(key);
    return it == obj->end() ? nullv : it->second;
}
std::string JVal::get_str(const std::string& key, const std::string& def) const {
    const JVal& v = at(key);
    return v.is_str() ? v.str : def;
}
double JVal::get_num(const std::string& key, double def) const {
    const JVal& v = at(key);
    return v.is_num() ? v.num : def;
}
int64_t JVal::get_int(const std::string& key, int64_t def) const {
    const JVal& v = at(key);
    return v.is_num() ? (int64_t)llround(v.num) : def;
}
bool JVal::get_bool(const std::string& key, bool def) const {
    const JVal& v = at(key);
    return v.is_bool() ? v.b : def;
}
void JVal::set(const std::string& key, const JVal& v) {
    if (type == Obj && obj) (*obj)[key] = v;
}
void JVal::push(const JVal& v) {
    if (type == Arr && arr) arr->push_back(v);
}

// ---------------- writer ----------------
static void dump_rec(const JVal& v, std::string* out) {
    switch (v.type) {
        case JVal::Null: *out += "null"; break;
        case JVal::Bool: *out += v.b ? "true" : "false"; break;
        case JVal::Num: {
            double i;
            if (std::modf(v.num, &i) == 0 && std::fabs(v.num) < 9.0e15) {
                char buf[32];
                snprintf(buf, sizeof buf, "%lld", (long long)v.num);
                *out += buf;
            } else {
                char buf[40];
                snprintf(buf, sizeof buf, "%.17g", v.num);
                *out += buf;
            }
            break;
        }
        case JVal::Str: *out += json_quote(v.str); break;
        case JVal::Arr: {
            *out += '[';
            if (v.arr) {
                for (size_t i = 0; i < v.arr->size(); i++) {
                    if (i) *out += ',';
                    dump_rec((*v.arr)[i], out);
                }
            }
            *out += ']';
            break;
        }
        case JVal::Obj: {
            *out += '{';
            if (v.obj) {
                bool first = true;
                for (const auto& kv : *v.obj) {
                    if (!first) *out += ',';
                    first = false;
                    *out += json_quote(kv.first);
                    *out += ':';
                    dump_rec(kv.second, out);
                }
            }
            *out += '}';
            break;
        }
    }
}

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
    return out;
}

std::string json_dump(const JVal& v) {
    std::string out;
    dump_rec(v, &out);
    return out;
}

// ---------------- parser ----------------
namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;

    bool fail(const std::string& m) {
        if (err.empty()) err = m;
        return false;
    }
    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool parse(JVal* out) {
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        char c = *p;
        if (c == '{') return parse_obj(out);
        if (c == '[') return parse_arr(out);
        if (c == '"') return parse_str(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == 'n') return parse_null(out);
        return parse_num(out);
    }
    bool parse_obj(JVal* out) {
        *out = JVal::make_obj();
        p++;  // {
        skip_ws();
        if (p < end && *p == '}') { p++; return true; }
        while (true) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected object key");
            JVal key;
            if (!parse_str(&key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            p++;
            JVal val;
            if (!parse(&val)) return false;
            out->set(key.str, val);
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; return true; }
            return fail("expected ',' or '}'");
        }
    }
    bool parse_arr(JVal* out) {
        *out = JVal::make_arr();
        p++;  // [
        skip_ws();
        if (p < end && *p == ']') { p++; return true; }
        while (true) {
            JVal val;
            if (!parse(&val)) return false;
            out->push(val);
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == ']') { p++; return true; }
            return fail("expected ',' or ']'");
        }
    }
    bool parse_str(JVal* out) {
        out->type = JVal::Str;
        p++;  // "
        std::string s;
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { p++; out->str = s; return true; }
            if (c == '\\') {
                p++;
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        if (end - p < 4) return fail("bad \\u escape");
                        unsigned v = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p++;
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                            else return fail("bad \\u hex");
                        }
                        // encode as UTF-8 (BMP only; good enough for our files)
                        if (v < 0x80) s += char(v);
                        else if (v < 0x800) {
                            s += char(0xC0 | (v >> 6));
                            s += char(0x80 | (v & 0x3F));
                        } else {
                            s += char(0xE0 | (v >> 12));
                            s += char(0x80 | ((v >> 6) & 0x3F));
                            s += char(0x80 | (v & 0x3F));
                        }
                        break;
                    }
                    default: return fail("bad escape char");
                }
            } else {
                s += (char)c;
                p++;
            }
        }
        return fail("unterminated string");
    }
    bool parse_bool(JVal* out) {
        if (end - p >= 4 && strncmp(p, "true", 4) == 0) { *out = JVal::bl(true); p += 4; return true; }
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) { *out = JVal::bl(false); p += 5; return true; }
        return fail("bad literal");
    }
    bool parse_null(JVal* out) {
        if (end - p >= 4 && strncmp(p, "null", 4) == 0) { *out = JVal(); p += 4; return true; }
        return fail("bad literal");
    }
    bool parse_num(JVal* out) {
        std::string tok;
        if (p < end && (*p == '-' || *p == '+')) tok += *p++;
        bool dot = false;
        while (p < end && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '-' || *p == '+')) {
            if (*p == '.') dot = true;
            tok += *p++;
            // prevent 'e'/'+'/'-' runs outside exponent — acceptable for our files
            if (tok.size() > 64) return fail("number too long");
        }
        if (tok.empty()) return fail("expected number");
        try {
            *out = JVal::n(dot ? std::stod(tok) : (double)std::stoll(tok));
            return true;
        } catch (...) {
            return fail("invalid number");
        }
    }
};
}  // namespace

bool json_parse(const std::string& text, JVal* out, std::string* err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    if (!ps.parse(out)) {
        if (err) *err = ps.err;
        return false;
    }
    ps.skip_ws();
    if (ps.p != ps.end) {
        if (err) *err = "trailing data after JSON value";
        return false;
    }
    return true;
}

}  // namespace anyps3
