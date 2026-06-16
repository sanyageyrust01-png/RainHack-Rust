#include "Json.h"

#include <cstdio>
#include <cmath>
#include <cstring>

namespace json {

static void writeStr(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    _snprintf_s(b, _TRUNCATE, "\\u%04x", c);
                    out += b;
                } else out.push_back(c);
        }
    }
    out.push_back('"');
}

static void writeVal(std::string& out, const Value& v) {
    switch (v.t) {
        case Value::TNull:   out += "null"; break;
        case Value::TBool:   out += v.b ? "true" : "false"; break;
        case Value::TNumber: {
            char buf[64];
            double d = v.n;
            if (std::isfinite(d) && d == (double)(long long)d) {
                _snprintf_s(buf, _TRUNCATE, "%lld", (long long)d);
            } else {
                _snprintf_s(buf, _TRUNCATE, "%.17g", d);
            }
            out += buf;
            break;
        }
        case Value::TString: writeStr(out, v.s); break;
        case Value::TArray: {
            out.push_back('[');
            const auto& arr = v.asArray();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i) out.push_back(',');
                writeVal(out, arr[i]);
            }
            out.push_back(']');
            break;
        }
        case Value::TObject: {
            out.push_back('{');
            const auto& obj = v.asObject();
            bool first = true;
            for (auto& kv : obj) {
                if (!first) out.push_back(',');
                first = false;
                writeStr(out, kv.first);
                out.push_back(':');
                writeVal(out, kv.second);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string serialize(const Value& v) { std::string s; s.reserve(64); writeVal(s, v); return s; }

struct Parser {
    const char* p; const char* end;
    Parser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool match(const char* tok) {
        size_t n = strlen(tok);
        if ((size_t)(end - p) < n) return false;
        if (memcmp(p, tok, n) != 0) return false;
        p += n; return true;
    }
    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (p + 4 > end) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = p[i]; unsigned v;
                            if (h >= '0' && h <= '9') v = h - '0';
                            else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
                            else return false;
                            cp = (cp << 4) | v;
                        }
                        p += 4;
                        if (cp < 0x80) out.push_back((char)cp);
                        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
                        else { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
                        break;
                    }
                    default: return false;
                }
            } else out.push_back(c);
        }
        return false;
    }

    bool parseValue(Value& v) {
        skipWs();
        if (p >= end) return false;
        char c = *p;
        if (c == '"') { v.t = Value::TString; return parseString(v.s); }
        if (c == '{') {
            ++p; v.t = Value::TObject; v.o = std::make_shared<Object>();
            skipWs();
            if (p < end && *p == '}') { ++p; return true; }
            while (p < end) {
                skipWs();
                std::string k;
                if (!parseString(k)) return false;
                skipWs();
                if (p >= end || *p != ':') return false;
                ++p;
                Value child;
                if (!parseValue(child)) return false;
                (*v.o)[k] = std::move(child);
                skipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            ++p; v.t = Value::TArray; v.a = std::make_shared<Array>();
            skipWs();
            if (p < end && *p == ']') { ++p; return true; }
            while (p < end) {
                Value child;
                if (!parseValue(child)) return false;
                v.a->push_back(std::move(child));
                skipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (c == 't') { if (!match("true")) return false; v.t = Value::TBool; v.b = true; return true; }
        if (c == 'f') { if (!match("false")) return false; v.t = Value::TBool; v.b = false; return true; }
        if (c == 'n') { if (!match("null")) return false; v.t = Value::TNull; return true; }
        const char* s = p;
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (*p == '-') ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
            std::string num(s, p);
            v.t = Value::TNumber;
            v.n = atof(num.c_str());
            return true;
        }
        return false;
    }
};

bool parse(const std::string& src, Value& out) {
    Parser p(src);
    if (!p.parseValue(out)) return false;
    p.skipWs();
    return p.p == p.end;
}

}
