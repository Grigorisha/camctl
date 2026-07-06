#include "stream/rtp_sender.hpp"

#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <vector>

namespace camctl {

// Полезная нагрузка RTP на пакет (с запасом под заголовки, чтобы влезть в типовой MTU 1500).
static constexpr size_t kMaxPayload = 1400;

RtpSession::RtpSession(int send_fd, const sockaddr_in& dest, uint32_t ssrc)
    : fd_(send_fd), dest_(dest), ssrc_(ssrc) {}

RtpSession::~RtpSession() {
    if (fd_ >= 0) ::close(fd_);
}

void RtpSession::send_packet(const uint8_t* payload, size_t len, uint32_t ts, bool marker) {
    uint8_t pkt[12 + kMaxPayload + 2];
    pkt[0] = 0x80;                                   // V=2, P=0, X=0, CC=0
    pkt[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | 96);  // M + PT=96 (dynamic H264)
    pkt[2] = static_cast<uint8_t>(seq_ >> 8);
    pkt[3] = static_cast<uint8_t>(seq_ & 0xFF);
    pkt[4] = static_cast<uint8_t>(ts >> 24);
    pkt[5] = static_cast<uint8_t>(ts >> 16);
    pkt[6] = static_cast<uint8_t>(ts >> 8);
    pkt[7] = static_cast<uint8_t>(ts);
    pkt[8]  = static_cast<uint8_t>(ssrc_ >> 24);
    pkt[9]  = static_cast<uint8_t>(ssrc_ >> 16);
    pkt[10] = static_cast<uint8_t>(ssrc_ >> 8);
    pkt[11] = static_cast<uint8_t>(ssrc_);
    std::memcpy(pkt + 12, payload, len);
    ::sendto(fd_, pkt, 12 + len, 0,
             reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
    ++seq_;
}

void RtpSession::send_nal(const uint8_t* nal, size_t len, uint32_t ts, bool last_nal) {
    if (len == 0) return;
    if (len <= kMaxPayload) {
        // Single NAL unit packet — NAL целиком (включая свой заголовочный байт).
        send_packet(nal, len, ts, last_nal);
        return;
    }
    // FU-A: дробим NAL, заголовочный байт NAL не передаём как есть — воссоздаём во FU header.
    const uint8_t nal_hdr = nal[0];
    const uint8_t fu_ind = static_cast<uint8_t>((nal_hdr & 0xE0) | 28);  // F|NRI|Type=28
    const uint8_t nal_type = static_cast<uint8_t>(nal_hdr & 0x1F);

    const uint8_t* p = nal + 1;
    size_t rem = len - 1;
    bool first = true;
    while (rem > 0) {
        const size_t frag = rem < (kMaxPayload - 2) ? rem : (kMaxPayload - 2);
        const bool last_frag = (frag == rem);
        uint8_t buf[2 + kMaxPayload];
        buf[0] = fu_ind;
        uint8_t fu_hdr = nal_type;
        if (first)     fu_hdr |= 0x80;  // S — начало
        if (last_frag) fu_hdr |= 0x40;  // E — конец
        buf[1] = fu_hdr;
        std::memcpy(buf + 2, p, frag);
        send_packet(buf, frag + 2, ts, last_nal && last_frag);
        p += frag;
        rem -= frag;
        first = false;
    }
}

// Разбор Annex-B: собираем границы NAL (старт-коды 00 00 01 / 00 00 00 01).
void RtpSession::send_au(const uint8_t* au, size_t size, uint32_t rtp_ts) {
    std::vector<std::pair<const uint8_t*, size_t>> nals;
    size_t i = 0;
    // до первого старт-кода
    auto is_start = [&](size_t k, size_t& sc_len) -> bool {
        if (k + 3 <= size && au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) { sc_len = 3; return true; }
        if (k + 4 <= size && au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 0 && au[k + 3] == 1) { sc_len = 4; return true; }
        return false;
    };
    size_t sc;
    while (i < size && !is_start(i, sc)) ++i;
    while (i < size) {
        i += sc;                       // пропускаем старт-код
        const uint8_t* nal = au + i;
        // ищем следующий старт-код
        size_t j = i;
        size_t sc2 = 0;
        while (j < size && !is_start(j, sc2)) ++j;
        const size_t nal_len = j - i;
        if (nal_len > 0) nals.emplace_back(nal, nal_len);
        i = j;
        sc = sc2;
    }

    for (size_t k = 0; k < nals.size(); ++k) {
        const bool last = (k + 1 == nals.size());
        send_nal(nals[k].first, nals[k].second, rtp_ts, last);
    }
}

}  // namespace camctl
