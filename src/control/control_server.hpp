#pragma once
// Управляющий канал: TCP + построчный JSON (НЕ HTTP, по ADR). Запрос — один JSON-объект в строке:
//   {"id":N,"cmd":"...", ...}  ->  ответ {"id":N,"ok":true|false, ...}
// Команды: get_params | get | set | stats | load_preset | time (обмен часами для замера задержки).
#include "control/param_registry.hpp"
#include "control/json.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <functional>

namespace camctl {

class ControlServer {
public:
    // stats_cb — вернуть текущую телеметрию (fps, разрешение и т.п.).
    // preset_cb — применить пресет по имени (true/false + err).
    using StatsFn  = std::function<json::Value()>;
    using PresetFn = std::function<bool(const std::string& name, std::string& err)>;

    ControlServer(int port, ParamRegistry* reg, StatsFn stats, PresetFn preset);
    ~ControlServer();

    bool start();
    void stop();

private:
    void accept_loop();
    void client_loop(int cfd);
    std::string handle_request(const std::string& line);

    int port_;
    ParamRegistry* reg_;
    StatsFn stats_;
    PresetFn preset_;

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
};

}  // namespace camctl
