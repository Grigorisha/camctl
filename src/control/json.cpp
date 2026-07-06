#include "control/json.hpp"

#include <cstdio>
#include <cmath>

namespace camctl::json {

// ---- парсер (рекурсивный спуск) --------------------------------------------

namespace {

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parse_value(Value& out) {
        skip_ws();
        if (p >= end) return false;
        switch (*p) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': { out.type = Value::Str; return parse_string(out.str); }
            case 't': case 'f': return parse_bool(out);
            case 'n': return parse_null(out);
            default:  return parse_number(out);
        }
    }

    bool parse_string(std::string& s) {
        if (*p != '"') return false;
        ++p;
        s.clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        if (end - p < 4) return false;
                        int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= h - '0';
                            else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
                            else return false;
                        }
                        // минимальный UTF-8 (BMP) — для управляющих строк достаточно
                        if (code < 0x80) s += static_cast<char>(code);
                        else if (code < 0x800) {
                            s += static_cast<char>(0xC0 | (code >> 6));
                            s += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (code >> 12));
                            s += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                s += c;
            }
        }
        if (p >= end || *p != '"') return false;
        ++p;
        return true;
    }

    bool parse_number(Value& out) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool any = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '+' || *p == '-')) { ++p; any = true; }
        if (!any) return false;
        std::string tok(start, p);
        out.type = Value::Num;
        out.num = std::strtod(tok.c_str(), nullptr);
        return true;
    }

    bool parse_bool(Value& out) {
        if (end - p >= 4 && std::string(p, p + 4) == "true")  { p += 4; out = Value::B(true);  return true; }
        if (end - p >= 5 && std::string(p, p + 5) == "false") { p += 5; out = Value::B(false); return true; }
        return false;
    }

    bool parse_null(Value& out) {
        if (end - p >= 4 && std::string(p, p + 4) == "null") { p += 4; out = Value(); return true; }
        return false;
    }

    bool parse_array(Value& out) {
        ++p;  // '['
        out.type = Value::Arr;
        skip_ws();
        if (p < end && *p == ']') { ++p; return true; }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (p >= end) return false;
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return false;
        }
    }

    bool parse_object(Value& out) {
        ++p;  // '{'
        out.type = Value::Obj;
        skip_ws();
        if (p < end && *p == '}') { ++p; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (p >= end || *p != '"' || !parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return false;
            ++p;
            Value v;
            if (!parse_value(v)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (p >= end) return false;
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return false;
        }
    }
};

void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    out += '"';
}

void dump_value(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.b ? "true" : "false"; break;
        case Value::Num: {
            char buf[32];
            if (v.num == std::floor(v.num) && std::fabs(v.num) < 1e15)
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.num));
            else
                std::snprintf(buf, sizeof(buf), "%.6g", v.num);
            out += buf;
            break;
        }
        case Value::Str: dump_string(v.str, out); break;
        case Value::Arr: {
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                dump_value(v.arr[i], out);
            }
            out += ']';
            break;
        }
        case Value::Obj: {
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                dump_string(v.obj[i].first, out);
                out += ':';
                dump_value(v.obj[i].second, out);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

bool parse(const std::string& text, Value& out) {
    Parser ps{text.data(), text.data() + text.size()};
    if (!ps.parse_value(out)) return false;
    ps.skip_ws();
    return true;  // хвостовой мусор игнорируем — терпимо для построчного протокола
}

std::string dump(const Value& v) {
    std::string out;
    dump_value(v, out);
    return out;
}

}  // namespace camctl::json
