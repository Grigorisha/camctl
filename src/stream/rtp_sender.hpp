#pragma once
// Самописный RTP-пакетизатор H.264 (RFC 6184): один NAL в пакете либо фрагментация FU-A.
// Одна сессия = одно назначение (IP:port клиента) + свой UDP-сокет-источник.
#include <cstdint>
#include <cstddef>
#include <netinet/in.h>

namespace camctl {

class RtpSession {
public:
    // send_fd — UDP-сокет, привязанный к server_port (источник совпадает с анонсом в SETUP).
    // dest    — куда слать RTP (client_ip:client_port).
    RtpSession(int send_fd, const sockaddr_in& dest, uint32_t ssrc);
    ~RtpSession();

    RtpSession(const RtpSession&) = delete;
    RtpSession& operator=(const RtpSession&) = delete;

    // Отправить один access unit (Annex-B, возможно несколько NAL со старт-кодами).
    // rtp_ts — метка времени в тактах 90 кГц.
    void send_au(const uint8_t* au, size_t size, uint32_t rtp_ts);

    int fd() const { return fd_; }

private:
    void send_nal(const uint8_t* nal, size_t len, uint32_t ts, bool last_nal);
    void send_packet(const uint8_t* payload, size_t len, uint32_t ts, bool marker);

    int fd_;
    sockaddr_in dest_;
    uint32_t ssrc_;
    uint16_t seq_ = 0;
};

}  // namespace camctl
