// json.h - маленький JSON (UTF-8). Порядок ключей объекта сохраняется:
// конфиг Yggdrasil после правки остаётся в привычном виде.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <cstdlib>
#include <cstdio>
#include <cmath>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool isNull() const { return type == Type::Null; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }
    bool isBool() const { return type == Type::Bool; }
    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    static const Value& Null() { static const Value v; return v; }
    const Value& operator[](const std::string& k) const {
        if (type != Type::Object) return Null();
        for (const auto& kv : obj) if (kv.first == k) return kv.second;
        return Null();
    }
    const Value& operator[](size_t i) const {
        if (type != Type::Array || i >= arr.size()) return Null();
        return arr[i];
    }
    bool has(const std::string& k) const {
        if (type != Type::Object) return false;
        for (const auto& kv : obj) if (kv.first == k) return true;
        return false;
    }
    // изменяемый доступ: ключ создаётся, если его нет
    Value& at(const std::string& k) {
        if (type != Type::Object) { type = Type::Object; obj.clear(); }
        for (auto& kv : obj) if (kv.first == k) return kv.second;
        obj.push_back({ k, Value() });
        return obj.back().second;
    }
    void erase(const std::string& k) {
        for (size_t i = 0; i < obj.size(); ++i) if (obj[i].first == k) { obj.erase(obj.begin() + (long)i); return; }
    }
    size_t size() const { return type == Type::Array ? arr.size() : type == Type::Object ? obj.size() : 0; }
    std::string str(const std::string& def = "") const { return type == Type::String ? s : def; }
    double num(double def = 0) const { return type == Type::Number ? n : def; }
    long long i64(long long def = 0) const { return type == Type::Number ? (long long)n : def; }
    bool boolean(bool def = false) const { return type == Type::Bool ? b : def; }
    // число или строка как текст (getPeers отдаёт порт и т.п. по-разному)
    std::string text() const {
        if (type == Type::String) return s;
        if (type == Type::Number) {
            char buf[64];
            if (std::floor(n) == n && std::fabs(n) < 1e15) snprintf(buf, sizeof(buf), "%lld", (long long)n);
            else snprintf(buf, sizeof(buf), "%.6g", n);
            return buf;
        }
        if (type == Type::Bool) return b ? "true" : "false";
        return "";
    }
};

inline Value MakeString(const std::string& s) { Value v; v.type = Type::String; v.s = s; return v; }
inline Value MakeNumber(double n) { Value v; v.type = Type::Number; v.n = n; return v; }
inline Value MakeBool(bool b) { Value v; v.type = Type::Bool; v.b = b; return v; }
inline Value MakeArray() { Value v; v.type = Type::Array; return v; }
inline Value MakeObject() { Value v; v.type = Type::Object; return v; }

namespace detail {
struct Parser {
    const std::string& t;
    size_t i = 0;
    int depth = 0;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i; }
    bool lit(const char* w) {
        size_t l = strlen_(w);
        if (t.compare(i, l, w) != 0) return false;
        i += l;
        return true;
    }
    static size_t strlen_(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
    static void put(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
                case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
                case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
                case 'r': o += '\r'; break; case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Value& v) {
        if (++depth > 200) return false;
        ws();
        if (i >= t.size()) return false;
        char c = t[i];
        bool ok = false;
        if (c == '{') {
            v = MakeObject();
            ++i; ws();
            if (i < t.size() && t[i] == '}') { ++i; ok = true; }
            else for (;;) {
                ws();
                std::string k;
                if (!str(k)) break;
                ws();
                if (i >= t.size() || t[i] != ':') break;
                ++i;
                Value x;
                if (!value(x)) break;
                v.obj.push_back({ k, std::move(x) });
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == '}') { ++i; ok = true; }
                break;
            }
        } else if (c == '[') {
            v = MakeArray();
            ++i; ws();
            if (i < t.size() && t[i] == ']') { ++i; ok = true; }
            else for (;;) {
                Value x;
                if (!value(x)) break;
                v.arr.push_back(std::move(x));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; continue; }
                if (i < t.size() && t[i] == ']') { ++i; ok = true; }
                break;
            }
        } else if (c == '"') {
            v = MakeString("");
            ok = str(v.s);
        } else if (lit("true")) { v = MakeBool(true); ok = true; }
        else if (lit("false")) { v = MakeBool(false); ok = true; }
        else if (lit("null")) { v = Value(); ok = true; }
        else {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end && end != start) { v = MakeNumber(d); i += (size_t)(end - start); ok = true; }
        }
        --depth;
        return ok;
    }
};
inline void esc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    o += '"';
}
inline void dump(std::string& o, const Value& v, bool pretty, int ind) {
    auto nl = [&](int d) { if (pretty) { o += '\n'; o.append((size_t)d * 2, ' '); } };
    switch (v.type) {
        case Type::Null: o += "null"; break;
        case Type::Bool: o += v.b ? "true" : "false"; break;
        case Type::Number: o += v.text(); break;
        case Type::String: esc(o, v.s); break;
        case Type::Array:
            o += '[';
            for (size_t k = 0; k < v.arr.size(); ++k) { if (k) o += ','; nl(ind + 1); dump(o, v.arr[k], pretty, ind + 1); }
            if (!v.arr.empty()) nl(ind);
            o += ']';
            break;
        case Type::Object:
            o += '{';
            for (size_t k = 0; k < v.obj.size(); ++k) {
                if (k) o += ',';
                nl(ind + 1);
                esc(o, v.obj[k].first);
                o += pretty ? ": " : ":";
                dump(o, v.obj[k].second, pretty, ind + 1);
            }
            if (!v.obj.empty()) nl(ind);
            o += '}';
            break;
    }
}
} // namespace detail

inline bool Parse(const std::string& text, Value& out) {
    detail::Parser p(text);
    Value v;
    if (!p.value(v)) return false;
    p.ws();
    if (p.i != text.size()) return false;
    out = std::move(v);
    return true;
}
// Разбор первого JSON-значения в тексте (yggdrasilctl иногда пишет перед ним строку)
inline bool ParseLoose(const std::string& text, Value& out) {
    if (Parse(text, out)) return true;
    size_t a = text.find_first_of("{[");
    if (a == std::string::npos) return false;
    size_t b = text.find_last_of("}]");
    if (b == std::string::npos || b < a) return false;
    return Parse(text.substr(a, b - a + 1), out);
}
inline std::string Dump(const Value& v, bool pretty = false) {
    std::string o;
    detail::dump(o, v, pretty, 0);
    return o;
}
inline std::string Quote(const std::string& s) { std::string o; detail::esc(o, s); return o; }

} // namespace json
