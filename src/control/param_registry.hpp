#pragma once
// Реестр параметров: единая таблица «ручек» камеры и энкодера с get/set по имени.
// Семантику задаёт приложение (лямбды), реестр — только маршрутизация и метаданные.
#include "control/json.hpp"

#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace camctl {

struct ParamInfo {
    std::string name;
    std::string type;                 // "int" | "float" | "bool" | "enum"
    std::string unit;                 // напр. "us", "bps", "" — только для подсказки
    bool has_range = false;
    double min = 0, max = 0;
    std::vector<std::string> options; // для enum
};

class ParamRegistry {
public:
    using Getter = std::function<json::Value()>;
    // Возвращает false + заполняет err, если значение отвергнуто.
    using Setter = std::function<bool(const json::Value&, std::string& err)>;

    void add(ParamInfo info, Getter g, Setter s);

    bool get(const std::string& name, json::Value& out, std::string& err) const;
    bool set(const std::string& name, const json::Value& v, std::string& err);

    // Список параметров с текущими значениями (для команды get_params).
    json::Value list() const;

private:
    struct Entry { ParamInfo info; Getter get; Setter set; };
    const Entry* find(const std::string& name) const;

    mutable std::mutex mu_;
    std::vector<Entry> entries_;
};

}  // namespace camctl
