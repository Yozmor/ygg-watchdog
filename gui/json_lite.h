// json_lite.h - маленький разборщик и писатель JSON (общий с ygg-watchdog).
// Понимает всё, что выдают yggdrasilctl -json и ygg_watchdog status-json:
// объекты, массивы, строки (с \uXXXX), числа, true/false/null.
// Строки сразу переводятся в std::wstring (вход - UTF-8).
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdlib>
#include <cstdio>

namespace json {

struct Value;
using Object = std::map<std::wstring, Value>;
using Array = std::vector<Value>;

struct Value {
    enum Type { Null, Bool, Number, String, ArrayT, ObjectT } type = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::shared_ptr<Array> arr;
    std::shared_ptr<Object> obj;

    bool isNull() const { return type == Null; }
    bool isObject() const { return type == ObjectT; }
    bool isArray() const { return type == ArrayT; }
    bool isString() const { return type == String; }

    // Доступ без исключений: отсутствующее поле -> пустое значение
    const Value& operator[](const std::wstring& key) const {
        static const Value empty;
        if (type != ObjectT || !obj) return empty;
        auto it = obj->find(key);
        return it == obj->end() ? empty : it->second;
    }
    const Value& operator[](size_t i) const {
        static const Value empty;
        if (type != ArrayT || !arr || i >= arr->size()) return empty;
        return (*arr)[i];
    }
    size_t size() const {
        if (type == ArrayT && arr) return arr->size();
        if (type == ObjectT && obj) return obj->size();
        return 0;
    }
    std::wstring str(const std::wstring& def = L"") const { return type == String ? s : def; }
    double num(double def = 0) const { return type == Number ? n : def; }
    long long i64(long long def = 0) const { return type == Number ? (long long)n : def; }
    bool boolean(bool def = false) const { return type == Bool ? b : def; }
};

class Parser {
public:
    explicit Parser(const std::string& text) : t(text) {}

    bool parse(Value& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return p == t.size();   // мусор после значения = не JSON (например строка лога "2026/10/16 ...")
    }

private:
    const std::string& t;
    size_t p = 0;

    void skipWs() {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
    }

    bool parseValue(Value& v) {
        skipWs();
        if (p >= t.size()) return false;
        char c = t[p];
        if (c == '{') return parseObject(v);
        if (c == '[') return parseArray(v);
        if (c == '"') { v.type = Value::String; return parseString(v.s); }
        if (c == 't' && t.compare(p, 4, "true") == 0) { v.type = Value::Bool; v.b = true; p += 4; return true; }
        if (c == 'f' && t.compare(p, 5, "false") == 0) { v.type = Value::Bool; v.b = false; p += 5; return true; }
        if (c == 'n' && t.compare(p, 4, "null") == 0) { v.type = Value::Null; p += 4; return true; }
        return parseNumber(v);
    }

    bool parseNumber(Value& v) {
        size_t start = p;
        if (p < t.size() && (t[p] == '-' || t[p] == '+')) ++p;
        while (p < t.size() && ((t[p] >= '0' && t[p] <= '9') || t[p] == '.' || t[p] == 'e' || t[p] == 'E' || t[p] == '-' || t[p] == '+')) ++p;
        if (p == start) return false;
        v.type = Value::Number;
        v.n = std::strtod(t.substr(start, p - start).c_str(), nullptr);
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    static unsigned hex4(const std::string& s, size_t at) {
        unsigned v = 0;
        for (size_t i = 0; i < 4 && at + i < s.size(); ++i) {
            char c = s[at + i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        }
        return v;
    }

    bool parseString(std::wstring& out) {
        if (t[p] != '"') return false;
        ++p;
        std::string utf8;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c == '\\' && p < t.size()) {
                char e = t[p++];
                switch (e) {
                    case 'n': utf8 += '\n'; break;
                    case 'r': utf8 += '\r'; break;
                    case 't': utf8 += '\t'; break;
                    case 'b': utf8 += '\b'; break;
                    case 'f': utf8 += '\f'; break;
                    case 'u': {
                        unsigned cp = hex4(t, p); p += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                            unsigned lo = hex4(t, p + 2);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                        }
                        appendUtf8(utf8, cp);
                        break;
                    }
                    default: utf8 += e; break; // \" \\ \/
                }
            } else {
                utf8 += c;
            }
        }
        if (p >= t.size()) return false;
        ++p; // закрывающая кавычка
        out = FromUtf8(utf8);
        return true;
    }

    bool parseArray(Value& v) {
        v.type = Value::ArrayT;
        v.arr = std::make_shared<Array>();
        ++p;
        skipWs();
        if (p < t.size() && t[p] == ']') { ++p; return true; }
        while (p < t.size()) {
            Value item;
            if (!parseValue(item)) return false;
            v.arr->push_back(item);
            skipWs();
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            return false;
        }
        return false;
    }

    bool parseObject(Value& v) {
        v.type = Value::ObjectT;
        v.obj = std::make_shared<Object>();
        ++p;
        skipWs();
        if (p < t.size() && t[p] == '}') { ++p; return true; }
        while (p < t.size()) {
            skipWs();
            std::wstring key;
            if (p >= t.size() || t[p] != '"' || !parseString(key)) return false;
            skipWs();
            if (p >= t.size() || t[p] != ':') return false;
            ++p;
            Value item;
            if (!parseValue(item)) return false;
            (*v.obj)[key] = item;
            skipWs();
            if (p < t.size() && t[p] == ',') { ++p; continue; }
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            return false;
        }
        return false;
    }

public:
    static std::wstring FromUtf8(const std::string& s) {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
};

// Разобрать текст; при ошибке вернуть false (out остаётся пустым)
inline bool Parse(const std::string& text, Value& out) {
    Parser parser(text);
    Value tmp;
    if (!parser.parse(tmp)) return false;
    out = tmp;
    return true;
}

// ------------------------------------------------------------------
// Запись обратно в текст (UTF-8). pretty - с отступами, для файлов.
// Ключи объектов идут по алфавиту (std::map) - Yggdrasil это не важно.
// ------------------------------------------------------------------
inline std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::string Quote(const std::wstring& w) {
    std::string s = ToUtf8(w), o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o + "\"";
}

inline std::string NumberText(double n) {
    char b[64];
    if (n == (double)(long long)n && n < 1e15 && n > -1e15) snprintf(b, 64, "%lld", (long long)n);
    else snprintf(b, 64, "%.17g", n);
    return b;
}

inline void Write(const Value& v, std::string& out, bool pretty, int indent = 0) {
    std::string pad = pretty ? std::string((size_t)(indent + 1) * 2, ' ') : "";
    std::string padEnd = pretty ? std::string((size_t)indent * 2, ' ') : "";
    const char* nl = pretty ? "\n" : "";
    switch (v.type) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.b ? "true" : "false"; break;
        case Value::Number: out += NumberText(v.n); break;
        case Value::String: out += Quote(v.s); break;
        case Value::ArrayT: {
            if (!v.arr || v.arr->empty()) { out += "[]"; break; }
            out += "["; out += nl;
            for (size_t i = 0; i < v.arr->size(); ++i) {
                out += pad; Write((*v.arr)[i], out, pretty, indent + 1);
                if (i + 1 < v.arr->size()) out += ",";
                out += nl;
            }
            out += padEnd + "]";
            break;
        }
        case Value::ObjectT: {
            if (!v.obj || v.obj->empty()) { out += "{}"; break; }
            out += "{"; out += nl;
            size_t i = 0;
            for (const auto& kv : *v.obj) {
                out += pad + Quote(kv.first) + (pretty ? ": " : ":");
                Write(kv.second, out, pretty, indent + 1);
                if (++i < v.obj->size()) out += ",";
                out += nl;
            }
            out += padEnd + "}";
            break;
        }
    }
}

inline std::string Dump(const Value& v, bool pretty = false) { std::string s; Write(v, s, pretty); return s; }
inline std::wstring DumpW(const Value& v, bool pretty = false) { return Parser::FromUtf8(Dump(v, pretty)); }

// Глубокая копия (Value делит массивы/объекты через shared_ptr)
inline Value Clone(const Value& v) {
    Value c = v;
    if (v.type == Value::ArrayT && v.arr) {
        c.arr = std::make_shared<Array>();
        for (const auto& x : *v.arr) c.arr->push_back(Clone(x));
    } else if (v.type == Value::ObjectT && v.obj) {
        c.obj = std::make_shared<Object>();
        for (const auto& kv : *v.obj) (*c.obj)[kv.first] = Clone(kv.second);
    }
    return c;
}

inline Value MakeString(const std::wstring& s) { Value v; v.type = Value::String; v.s = s; return v; }
inline Value MakeArray() { Value v; v.type = Value::ArrayT; v.arr = std::make_shared<Array>(); return v; }
inline Value MakeObject() { Value v; v.type = Value::ObjectT; v.obj = std::make_shared<Object>(); return v; }

} // namespace json
