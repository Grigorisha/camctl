#pragma once
// Аппаратный H.264-энкодер на NVENC Jetson (NvVideoEncoder / jetson_multimedia_api, путь MMAP).
// Вход: I420 (YUV420 planar). Выход: H.264 NAL'ы через колбэк (SPS/PPS на каждом IDR).
#include <cstddef>
#include <cstdint>
#include <functional>

namespace camctl {

struct H264EncoderImpl;  // определён в .cpp (на уровне namespace, чтобы колбэк имел доступ)

class H264Encoder {
public:
    // (data, size, keyframe) — очередная порция закодированного потока (обычно один AU/NAL-набор).
    using NalCallback = std::function<void(const uint8_t* data, size_t size, bool keyframe)>;

    H264Encoder();
    ~H264Encoder();

    bool init(int width, int height, int fps, int bitrate_bps);
    void on_nal(NalCallback cb);
    bool encodeFrame(const uint8_t* i420, size_t size);  // size = width*height*3/2
    void request_keyframe();                              // форсировать IDR (для быстрого входа клиента)
    void set_bitrate(int bitrate_bps);                    // рантайм-смена битрейта (без пересоздания)
    void finish();                                        // EOS + дождаться слива капчур-потока

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

private:
    H264EncoderImpl* p_;
};

}  // namespace camctl
