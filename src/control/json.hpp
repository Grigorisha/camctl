#pragma once
// Минимальный самописный JSON (независимость от библиотек, ADR-0003).
// Поддержка: object / array / string / number(double) / bool / null. Достаточно для control-протокола.
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace camctl::json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;  // порядок сохраняем, поиск линейный (объекты мелкие)

    Value() = default;
    static Value B(bool v)               { Value x; x.type = Bool; x.b = v; return x; }
    static Value N(double v)             { Value x; x.type = Num;  x.num = v; return x; }
    static Value S(std::string v)        { Value x; x.type = Str;  x.str = std::move(v); return x; }
    static Value A()                     { Value x; x.type = Arr; return x; }
    static Value O()                     { Value x; x.type = Obj; return x; }

    // Хелперы объекта.
    void set(const std::string& k, Value v) { obj.emplace_back(k, std::move(v)); }
    const Value* find(const std::string& k) const {
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool is_num() const { return type == Num; }
    bool is_str() const { return type == Str; }
    // Удобные извлечения с дефолтом.
    double as_num(double d = 0) const { return type == Num ? num : (type == Bool ? (b ? 1 : 0) : d); }
    bool   as_bool(bool d = false) const { return type == Bool ? b : (type == Num ? num != 0 : d); }
    std::string as_str(const std::string& d = "") const { return type == Str ? str : d; }
};

// Разобрать текст. Возвращает false при синтаксической ошибке.
bool parse(const std::string& text, Value& out);

// Сериализовать (компактно, одна строка).
std::string dump(const Value& v);

}  // namespace camctl::json
