#include "stream/rtsp_server.hpp"
#include "stream/rtp_sender.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <map>

namespace camctl {

// ---- вспомогательное -------------------------------------------------------

static std::string header_value(const std::string& req, const char* name) {
    // регистронезависимый поиск "Name:" по строкам
    std::istringstream ss(req);
    std::string line;
    const std::string key = name;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < key.size() + 1) continue;
        bool match = true;
        for (size_t i = 0; i < key.size(); ++i)
            if (std::tolower(line[i]) != std::tolower(key[i])) { match = false; break; }
        if (match && line[key.size()] == ':') {
            size_t p = key.size() + 1;
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
            return line.substr(p);
        }
    }
    return {};
}

static int cseq_of(const std::string& req) {
    const std::string v = header_value(req, "CSeq");
    return v.empty() ? 0 : std::atoi(v.c_str());
}

static void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

static std::string local_ip(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        return inet_ntoa(a.sin_addr);
    return "127.0.0.1";
}

static std::string base64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t b0 = d[i];
        const uint32_t b1 = (i + 1 < n) ? d[i + 1] : 0;
        const uint32_t b2 = (i + 2 < n) ? d[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out += t[(v >> 18) & 0x3F];
        out += t[(v >> 12) & 0x3F];
        out += (i + 1 < n) ? t[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < n) ? t[v & 0x3F] : '=';
    }
    return out;
}

// Перебор NAL в Annex-B буфере: fn(nal_ptr, nal_len) — без старт-кода, с заголовочным байтом.
template <class F>
static void for_each_nal(const uint8_t* buf, size_t size, F&& fn) {
    auto start_len = [&](size_t k) -> size_t {
        if (k + 3 <= size && buf[k] == 0 && buf[k + 1] == 0 && buf[k + 2] == 1) return 3;
        if (k + 4 <= size && buf[k] == 0 && buf[k + 1] == 0 && buf[k + 2] == 0 && buf[k + 3] == 1) return 4;
        return 0;
    };
    size_t i = 0, sc;
    while (i < size && (sc = start_len(i)) == 0) ++i;
    while (i < size) {
        i += sc;
        const size_t nal = i;
        size_t sc2 = 0;
        while (i < size && (sc2 = start_len(i)) == 0) ++i;
        if (i > nal) fn(buf + nal, i - nal);
        sc = sc2;
    }
}

// ---- RtspServer ------------------------------------------------------------

RtspServer::RtspServer(int port, int fps) : port_(port), fps_(fps > 0 ? fps : 30) {}

RtspServer::~RtspServer() { stop(); }

bool RtspServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { std::perror("socket"); return false; }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    if (::listen(listen_fd_, 4) < 0) {
        std::perror("listen"); ::close(listen_fd_); listen_fd_ = -1; return false;
    }
    running_ = true;
    accept_thread_ = std::thread(&RtspServer::accept_loop, this);
    std::printf("RTSP слушает 0.0.0.0:%d\n", port_);
    return true;
}

void RtspServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); ::close(listen_fd_); listen_fd_ = -1; }
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& t : client_threads_) if (t.joinable()) t.join();
    client_threads_.clear();
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.clear();
}

void RtspServer::accept_loop() {
    while (running_) {
        sockaddr_in cli{};
        socklen_t len = sizeof(cli);
        int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&cli), &len);
        if (cfd < 0) { if (running_) continue; else break; }
        int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        client_threads_.emplace_back(&RtspServer::client_loop, this, cfd);
    }
}

void RtspServer::client_loop(int cfd) {
    // адрес клиента (для отправки RTP на его client_port)
    sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    ::getpeername(cfd, reinterpret_cast<sockaddr*>(&cli), &clen);
    const std::string srv_ip = local_ip(cfd);

    std::string session_id;                 // выдаётся на SETUP
    std::shared_ptr<RtpSession> my_session;  // регистрируется на PLAY
    uint16_t client_rtp_port = 0;
    uint16_t server_rtp_port = 0;

    std::string buf;
    char tmp[2048];
    while (running_) {
        // читаем до конца одного запроса (\r\n\r\n)
        size_t hdr_end;
        while ((hdr_end = buf.find("\r\n\r\n")) == std::string::npos) {
            ssize_t n = ::recv(cfd, tmp, sizeof(tmp), 0);
            if (n <= 0) goto done;
            buf.append(tmp, static_cast<size_t>(n));
        }
        {
            std::string req = buf.substr(0, hdr_end + 4);
            buf.erase(0, hdr_end + 4);

            // первая строка: METHOD URL RTSP/1.0
            std::string method, url;
            {
                std::istringstream ls(req);
                ls >> method >> url;
            }
            const int cseq = cseq_of(req);
            const std::string base = "rtsp://" + srv_ip + ":" + std::to_string(port_);

            std::ostringstream resp;
            if (method == "OPTIONS") {
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n"
                     << "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n";
                send_all(cfd, resp.str());

            } else if (method == "DESCRIBE") {
                const std::string body = build_sdp(srv_ip);
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n"
                     << "Content-Base: " << url << "/\r\n"
                     << "Content-Type: application/sdp\r\n"
                     << "Content-Length: " << body.size() << "\r\n\r\n"
                     << body;
                send_all(cfd, resp.str());

            } else if (method == "SETUP") {
                const std::string transport = header_value(req, "Transport");
                // вытащить client_port=A-B
                size_t cp = transport.find("client_port=");
                if (cp != std::string::npos) {
                    client_rtp_port = static_cast<uint16_t>(std::atoi(transport.c_str() + cp + 12));
                }
                // UDP-сокет-источник; server_port = его порт (источник RTP совпадёт с анонсом)
                int sfd = ::socket(AF_INET, SOCK_DGRAM, 0);
                sockaddr_in sa{};
                sa.sin_family = AF_INET;
                sa.sin_addr.s_addr = INADDR_ANY;
                sa.sin_port = 0;   // эфемерный
                ::bind(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
                sockaddr_in bound{}; socklen_t bl = sizeof(bound);
                ::getsockname(sfd, reinterpret_cast<sockaddr*>(&bound), &bl);
                server_rtp_port = ntohs(bound.sin_port);

                sockaddr_in dest = cli;
                dest.sin_port = htons(client_rtp_port);
                const uint32_t ssrc = static_cast<uint32_t>(::rand()) ^ 0xC0FFEE;
                my_session = std::make_shared<RtpSession>(sfd, dest, ssrc);

                session_id = std::to_string(0x1000 + (::rand() & 0x7FFF));
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n"
                     << "Transport: RTP/AVP;unicast;client_port=" << client_rtp_port
                     << "-" << (client_rtp_port + 1)
                     << ";server_port=" << server_rtp_port << "-" << (server_rtp_port + 1)
                     << ";ssrc=" << std::hex << ssrc << std::dec << "\r\n"
                     << "Session: " << session_id << ";timeout=60\r\n\r\n";
                send_all(cfd, resp.str());

            } else if (method == "PLAY") {
                if (my_session) {
                    std::lock_guard<std::mutex> lk(mu_);
                    sessions_.push_back(my_session);
                }
                if (on_keyframe_) on_keyframe_();   // мгновенный IDR для входящего клиента
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n"
                     << "Session: " << session_id << "\r\n"
                     << "Range: npt=0.000-\r\n\r\n";
                send_all(cfd, resp.str());
                std::printf("RTSP PLAY: %s -> RTP на %s:%u (src :%u)\n", url.c_str(),
                            inet_ntoa(cli.sin_addr), client_rtp_port, server_rtp_port);

            } else if (method == "TEARDOWN") {
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n"
                     << "Session: " << session_id << "\r\n\r\n";
                send_all(cfd, resp.str());
                goto done;

            } else {  // GET_PARAMETER (keepalive) и прочее — просто 200 OK
                resp << "RTSP/1.0 200 OK\r\nCSeq: " << cseq << "\r\n"
                     << "Session: " << session_id << "\r\n\r\n";
                send_all(cfd, resp.str());
            }
        }
    }
done:
    // убрать нашу сессию из играющих
    if (my_session) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            if (it->get() == my_session.get()) { sessions_.erase(it); break; }
        }
    }
    ::close(cfd);
}

std::string RtspServer::build_sdp(const std::string& srv_ip) {
    std::string fmtp = "a=fmtp:96 packetization-mode=1";
    {
        std::lock_guard<std::mutex> lk(ps_mu_);
        if (sps_.size() >= 4 && !pps_.empty()) {
            char plid[8];
            std::snprintf(plid, sizeof(plid), "%02x%02x%02x", sps_[1], sps_[2], sps_[3]);
            fmtp += ";profile-level-id=";
            fmtp += plid;
            fmtp += ";sprop-parameter-sets=" + base64(sps_.data(), sps_.size())
                  + "," + base64(pps_.data(), pps_.size());
        }
    }
    std::ostringstream sdp;
    sdp << "v=0\r\n"
        << "o=- 0 0 IN IP4 " << srv_ip << "\r\n"
        << "s=camctl\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "t=0 0\r\n"
        << "m=video 0 RTP/AVP 96\r\n"
        << "a=rtpmap:96 H264/90000\r\n"
        << fmtp << "\r\n"
        << "a=control:trackID=0\r\n";
    return sdp.str();
}

void RtspServer::push_au(const uint8_t* data, size_t size, bool /*keyframe*/) {
    // Запомнить SPS(7)/PPS(8) для SDP — энкодер шлёт их перед каждым IDR.
    for_each_nal(data, size, [&](const uint8_t* nal, size_t len) {
        const uint8_t type = nal[0] & 0x1F;
        if (type == 7 || type == 8) {
            std::lock_guard<std::mutex> lk(ps_mu_);
            (type == 7 ? sps_ : pps_).assign(nal, nal + len);
        }
    });

    if (!have_t0_) { t0_ = std::chrono::steady_clock::now(); have_t0_ = true; }
    const double sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0_).count();
    const uint32_t ts = static_cast<uint32_t>(sec * 90000.0);

    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : sessions_) s->send_au(data, size, ts);
}

}  // namespace camctl
