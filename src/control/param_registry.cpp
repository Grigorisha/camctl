#include "control/param_registry.hpp"

namespace camctl {

void ParamRegistry::add(ParamInfo info, Getter g, Setter s) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.push_back(Entry{std::move(info), std::move(g), std::move(s)});
}

const ParamRegistry::Entry* ParamRegistry::find(const std::string& name) const {
    for (const auto& e : entries_) if (e.info.name == name) return &e;
    return nullptr;
}

bool ParamRegistry::get(const std::string& name, json::Value& out, std::string& err) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Entry* e = find(name);
    if (!e) { err = "unknown param: " + name; return false; }
    out = e->get();
    return true;
}

bool ParamRegistry::set(const std::string& name, const json::Value& v, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    const Entry* e = find(name);
    if (!e) { err = "unknown param: " + name; return false; }
    // Проверка диапазона для числовых.
    if (e->info.has_range && (v.type == json::Value::Num)) {
        if (v.num < e->info.min || v.num > e->info.max) {
            err = "out of range [" + std::to_string(e->info.min) + ".." +
                  std::to_string(e->info.max) + "]";
            return false;
        }
    }
    return e->set(v, err);
}

json::Value ParamRegistry::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    json::Value arr = json::Value::A();
    for (const auto& e : entries_) {
        json::Value o = json::Value::O();
        o.set("name", json::Value::S(e.info.name));
        o.set("type", json::Value::S(e.info.type));
        if (!e.info.unit.empty()) o.set("unit", json::Value::S(e.info.unit));
        o.set("value", e.get());
        if (e.info.has_range) {
            o.set("min", json::Value::N(e.info.min));
            o.set("max", json::Value::N(e.info.max));
        }
        if (!e.info.options.empty()) {
            json::Value opts = json::Value::A();
            for (const auto& s : e.info.options) opts.arr.push_back(json::Value::S(s));
            o.set("options", std::move(opts));
        }
        arr.arr.push_back(std::move(o));
    }
    return arr;
}

}  // namespace camctl
