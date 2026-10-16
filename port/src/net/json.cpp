// Minimal JSON (json.h).
#include "net/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dcp::net {
namespace {

const Json& null_json() {
    static const Json n;
    return n;
}

struct Parser {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool lit(const char* s) {
        size_t n = std::strlen(s);
        if (size_t(e - p) < n || std::memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += char(c);
        else if (c < 0x800) o += char(0xc0 | (c >> 6)), o += char(0x80 | (c & 0x3f));
        else if (c < 0x10000) o += char(0xe0 | (c >> 12)), o += char(0x80 | ((c >> 6) & 0x3f)), o += char(0x80 | (c & 0x3f));
        else
            o += char(0xf0 | (c >> 18)), o += char(0x80 | ((c >> 12) & 0x3f)), o += char(0x80 | ((c >> 6) & 0x3f)),
                o += char(0x80 | (c & 0x3f));
    }
    bool hex4(uint32_t* v) {
        if (e - p < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string* o) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                *o += c;
                continue;
            }
            if (p >= e) return false;
            char x = *p++;
            switch (x) {
            case '"': *o += '"'; break;
            case '\\': *o += '\\'; break;
            case '/': *o += '/'; break;
            case 'b': *o += '\b'; break;
            case 'f': *o += '\f'; break;
            case 'n': *o += '\n'; break;
            case 'r': *o += '\r'; break;
            case 't': *o += '\t'; break;
            case 'u': {
                uint32_t c1;
                if (!hex4(&c1)) return false;
                if (c1 >= 0xd800 && c1 < 0xdc00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t c2;
                    if (!hex4(&c2)) return false;
                    c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                }
                utf8(*o, c1);
                break;
            }
            default: return false;
            }
        }
        if (p >= e) return false;
        ++p;
        return true;
    }
    bool value(Json* out) {
        if (++depth > 64) return false;
        ws();
        if (p >= e) return false;
        bool ok = true;
        if (*p == '{') {
            ++p;
            *out = Json::object();
            ws();
            if (p < e && *p == '}') ++p;
            else
                for (;;) {
                    ws();
                    std::string k;
                    if (!str(&k)) return false;
                    ws();
                    if (p >= e || *p++ != ':') return false;
                    Json v;
                    if (!value(&v)) return false;
                    (*out)[k] = std::move(v);
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == '}') { ++p; break; }
                    return false;
                }
        } else if (*p == '[') {
            ++p;
            *out = Json::array();
            ws();
            if (p < e && *p == ']') ++p;
            else
                for (;;) {
                    Json v;
                    if (!value(&v)) return false;
                    out->push(std::move(v));
                    ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == ']') { ++p; break; }
                    return false;
                }
        } else if (*p == '"') {
            std::string s;
            ok = str(&s);
            *out = Json(std::move(s));
        } else if (lit("true")) *out = Json(true);
        else if (lit("false")) *out = Json(false);
        else if (lit("null")) *out = Json();
        else {
            const char* s = p;
            if (p < e && (*p == '-' || *p == '+')) ++p;
            bool integral = true;
            while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) {
                if (*p == '.' || *p == 'e' || *p == 'E') integral = false;
                ++p;
            }
            if (p == s) return false;
            std::string num(s, p);
            if (integral) *out = Json(int64_t(std::strtoll(num.c_str(), nullptr, 10)));
            else *out = Json(std::strtod(num.c_str(), nullptr));
        }
        --depth;
        return ok;
    }
};

}  // namespace

const std::string& Json::as_string() const {
    static const std::string empty;
    return t_ == Type::String ? s_ : empty;
}

uint64_t Json::as_u64(uint64_t def) const {
    if (t_ == Type::Number) return uint64_t(i_);
    if (t_ != Type::String || s_.empty()) return def;
    return std::strtoull(s_.c_str(), nullptr, 0);
}

Json& Json::operator[](const std::string& k) {
    if (t_ != Type::Object) *this = object();
    return o_[k];
}
const Json& Json::get(const std::string& k) const {
    if (t_ != Type::Object) return null_json();
    auto it = o_.find(k);
    return it == o_.end() ? null_json() : it->second;
}
bool Json::has(const std::string& k) const { return t_ == Type::Object && o_.count(k); }

void Json::push(Json v) {
    if (t_ != Type::Array) *this = array();
    a_.push_back(std::move(v));
}
const Json& Json::at(size_t i) const { return t_ == Type::Array && i < a_.size() ? a_[i] : null_json(); }

static void dump_str(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else o += char(c);
        }
    }
    o += '"';
}

void Json::dump_to(std::string& o) const {
    switch (t_) {
    case Type::Null: o += "null"; break;
    case Type::Bool: o += b_ ? "true" : "false"; break;
    case Type::Number: {
        char b[32];
        if (double(i_) == d_ || !std::isfinite(d_)) std::snprintf(b, sizeof b, "%lld", (long long)i_);
        else std::snprintf(b, sizeof b, "%.17g", d_);
        o += b;
        break;
    }
    case Type::String: dump_str(o, s_); break;
    case Type::Array:
        o += '[';
        for (size_t i = 0; i < a_.size(); ++i) {
            if (i) o += ',';
            a_[i].dump_to(o);
        }
        o += ']';
        break;
    case Type::Object: {
        o += '{';
        bool first = true;
        for (const auto& [k, v] : o_) {
            if (!first) o += ',';
            first = false;
            dump_str(o, k);
            o += ':';
            v.dump_to(o);
        }
        o += '}';
        break;
    }
    }
}

std::string Json::dump() const {
    std::string o;
    dump_to(o);
    return o;
}

bool Json::parse(const std::string& text, Json* out) {
    Parser ps{text.data(), text.data() + text.size()};
    Json v;
    if (!ps.value(&v)) return *out = Json(), false;
    ps.ws();
    if (ps.p != ps.e) return *out = Json(), false;
    *out = std::move(v);
    return true;
}

std::string u64_str(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    return b;
}

std::string hex_encode(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) s[2 * i] = d[p[i] >> 4], s[2 * i + 1] = d[p[i] & 15];
    return s;
}

bool hex_decode(const std::string& s, std::vector<uint8_t>* out) {
    out->clear();
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out->push_back(uint8_t(a << 4 | b));
    }
    return true;
}

}  // namespace dcp::net
