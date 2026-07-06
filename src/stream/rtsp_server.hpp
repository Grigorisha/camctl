#pragma once
// Минимальный самописный RTSP-сервер (RFC 2326, подмножество): OPTIONS / DESCRIBE /
// SETUP / PLAY / TEARDOWN. Транспорт видео — RTP/AVP over UDP unicast (см. rtp_sender).
// Один эндпоинт = один поток H.264. Клиент: ffplay/VLC rtsp://<host>:<port>/<path>.
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <chrono>

namespace camctl {

class RtpSession;

class RtspServer {
public:
    RtspServer(int port, int fps);
    ~RtspServer();

    bool start();   // поднять слушающий TCP-сокет + accept-поток
    void stop();

    // Вызывается из потока энкодера: очередной access unit (Annex-B) во все играющие сессии.
    void push_au(const uint8_t* data, size_t size, bool keyframe);

    // Колбэк «нужен keyframe» (дёргается на PLAY — чтобы клиент получил IDR сразу).
    void set_keyframe_requester(std::function<void()> cb) { on_keyframe_ = std::move(cb); }

private:
    void accept_loop();
    void client_loop(int cfd);
    std::string build_sdp(const std::string& srv_ip);   // с sprop-parameter-sets, если известны

    int port_;
    int fps_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;

    std::mutex mu_;
    std::vector<std::shared_ptr<RtpSession>> sessions_;   // играющие назначения
    std::chrono::steady_clock::time_point t0_;
    bool have_t0_ = false;

    std::function<void()> on_keyframe_;
    std::mutex ps_mu_;                    // защищает sps_/pps_ (пишутся из потока энкодера)
    std::vector<uint8_t> sps_, pps_;      // последние виденные SPS/PPS (без старт-кода)
};

}  // namespace camctl
