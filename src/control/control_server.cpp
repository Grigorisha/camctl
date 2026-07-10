#include "control/control_server.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <ctime>

namespace camctl {

// CLOCK_MONOTONIC — тот же источник, что и SEI-метка захвата в camera_service.
// Реальные часы Jetson шагает NTP, поэтому для замера задержки они непригодны.
static int64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

ControlServer::ControlServer(int port, ParamRegistry* reg, StatsFn stats, PresetFn preset)
    : port_(port), reg_(reg), stats_(std::move(stats)), preset_(std::move(preset)) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { std::perror("control socket"); return false; }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("control bind"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    if (::listen(listen_fd_, 4) < 0) {
        std::perror("control listen"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    running_ = true;
    accept_thread_ = std::thread(&ControlServer::accept_loop, this);
    std::printf("Control (TCP/JSON) слушает 0.0.0.0:%d\n", port_);
    return true;
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& t : client_threads_) if (t.joinable()) t.join();
    client_threads_.clear();
}

void ControlServer::accept_loop() {
    while (running_) {
        int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) { if (running_) continue; else break; }
        int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // recv-таймаут, чтобы поток периодически проверял running_ и не висел в stop().
        timeval tv{0, 500000};
        ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        client_threads_.emplace_back(&ControlServer::client_loop, this, cfd);
    }
}

void ControlServer::client_loop(int cfd) {
    std::string buf;
    char tmp[1024];
    while (running_) {
        size_t nl;
        while ((nl = buf.find('\n')) == std::string::npos) {
            ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!running_) { ::close(cfd); return; }
                continue;  // таймаут — просто перепроверили running_
            }
            if (n <= 0) { ::close(cfd); return; }
            buf.append(tmp, static_cast<size_t>(n));
        }
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::string resp = handle_request(line) + "\n";
        size_t off = 0;
        while (off < resp.size()) {
            ssize_t n = ::send(cfd, resp.data() + off, resp.size() - off, 0);
            if (n <= 0) { ::close(cfd); return; }
            off += static_cast<size_t>(n);
        }
    }
    ::close(cfd);
}

std::string ControlServer::handle_request(const std::string& line) {
    const int64_t t_recv = mono_ns();  // для команды time

    json::Value req;
    json::Value out = json::Value::O();
    if (!json::parse(line, req) || req.type != json::Value::Obj) {
        out.set("ok", json::Value::B(false));
        out.set("error", json::Value::S("bad json"));
        return json::dump(out);
    }

    const json::Value* pid = req.find("id");
    if (pid) out.set("id", *pid);
    const std::string cmd = [&] {
        const json::Value* c = req.find("cmd");
        return c ? c->as_str() : std::string();
    }();

    auto fail = [&](const std::string& msg) {
        out.set("ok", json::Value::B(false));
        out.set("error", json::Value::S(msg));
        return json::dump(out);
    };

    if (cmd == "get_params") {
        out.set("ok", json::Value::B(true));
        out.set("params", reg_->list());
        return json::dump(out);
    }
    if (cmd == "get") {
        const json::Value* n = req.find("name");
        if (!n) return fail("missing name");
        json::Value v; std::string err;
        if (!reg_->get(n->as_str(), v, err)) return fail(err);
        out.set("ok", json::Value::B(true));
        out.set("value", std::move(v));
        return json::dump(out);
    }
    if (cmd == "set") {
        const json::Value* n = req.find("name");
        const json::Value* v = req.find("value");
        if (!n || !v) return fail("missing name/value");
        std::string err;
        if (!reg_->set(n->as_str(), *v, err)) return fail(err);
        out.set("ok", json::Value::B(true));
        return json::dump(out);
    }
    if (cmd == "stats") {
        out.set("ok", json::Value::B(true));
        out.set("stats", stats_ ? stats_() : json::Value::O());
        return json::dump(out);
    }
    if (cmd == "load_preset") {
        const json::Value* n = req.find("name");
        if (!n) return fail("missing name");
        std::string err;
        if (!preset_ || !preset_(n->as_str(), err)) return fail(err.empty() ? "no preset" : err);
        out.set("ok", json::Value::B(true));
        return json::dump(out);
    }
    if (cmd == "save_preset") {
        const json::Value* n = req.find("name");
        if (!n) return fail("missing name");
        std::string err;
        if (!save_ || !save_(n->as_str(), err)) return fail(err.empty() ? "save failed" : err);
        out.set("ok", json::Value::B(true));
        return json::dump(out);
    }
    if (cmd == "list_presets") {
        out.set("ok", json::Value::B(true));
        out.set("presets", list_presets_ ? list_presets_() : json::Value::A());
        return json::dump(out);
    }
    if (cmd == "time") {
        // Обмен часами (SNTP-подобно): клиент шлёт t1, мы возвращаем t2(recv) и t3(send).
        const json::Value* t1 = req.find("t1");
        out.set("ok", json::Value::B(true));
        if (t1) out.set("t1", *t1);
        out.set("t2", json::Value::N(static_cast<double>(t_recv)));
        out.set("t3", json::Value::N(static_cast<double>(mono_ns())));
        return json::dump(out);
    }
    return fail("unknown cmd: " + cmd);
}

}  // namespace camctl
