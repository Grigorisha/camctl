// Веха 2: сквозной поток. Камера -> дебайер -> (даунскейл) -> I420 -> NVENC H.264 -> RTP/RTSP.
// Смотреть с десктопа:  ffplay -fflags nobuffer -flags low_delay rtsp://<jetson>:8554/cam0
//
// Запуск:  ./stream_test [serial] [port] [fps] [maxside]
//   serial  — серийник камеры ("" = первая), port — RTSP-порт (8554),
//   fps     — целевой FPS энкодера, maxside — ограничение большей стороны кадра (даунскейл).
#include "camera/daheng_camera.hpp"
#include "core/debayer.hpp"
#include "core/rgb_to_i420.hpp"
#include "encode/h264_encoder.hpp"
#include "stream/rtsp_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace camctl;

static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run = false; }

// Простейший nearest-neighbor даунскейл RGB24 (src WxH -> dst dw x dh).
static void downscale_rgb24(const uint8_t* src, int sw, int sh,
                            uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const int sy = static_cast<int>((static_cast<int64_t>(y) * sh) / dh);
        const uint8_t* srow = src + static_cast<size_t>(sy) * sw * 3;
        uint8_t* drow = dst + static_cast<size_t>(y) * dw * 3;
        for (int x = 0; x < dw; ++x) {
            const int sx = static_cast<int>((static_cast<int64_t>(x) * sw) / dw);
            drow[3 * x]     = srow[3 * sx];
            drow[3 * x + 1] = srow[3 * sx + 1];
            drow[3 * x + 2] = srow[3 * sx + 2];
        }
    }
}

int main(int argc, char** argv) {
    const std::string serial = (argc > 1) ? argv[1] : "";
    const int port    = (argc > 2) ? std::atoi(argv[2]) : 8554;
    const int fps     = (argc > 3) ? std::atoi(argv[3]) : 30;
    const int maxside = (argc > 4) ? std::atoi(argv[4]) : 1280;

    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    DahengCamera cam(serial);
    if (!cam.open())  { std::fprintf(stderr, "open() не удался\n"); return 1; }
    std::printf("Камера: %s\n", cam.info().c_str());
    if (!cam.start()) { std::fprintf(stderr, "start() не удался\n"); return 1; }

    // Первый кадр — узнать размеры/формат.
    Frame raw;
    for (int i = 0; i < 50 && !raw.valid(); ++i) cam.grab(raw, 500);
    if (!raw.valid()) { std::fprintf(stderr, "нет кадров с камеры\n"); return 2; }

    // Целевые размеры энкодера: ограничиваем большую сторону, держим чётность.
    int ow = raw.width, oh = raw.height;
    if (maxside > 0 && (raw.width > maxside || raw.height > maxside)) {
        const double s = static_cast<double>(maxside) / std::max(raw.width, raw.height);
        ow = static_cast<int>(raw.width * s);
        oh = static_cast<int>(raw.height * s);
    }
    ow &= ~1; oh &= ~1;
    const bool need_scale = (ow != raw.width || oh != raw.height);
    std::printf("Кадр %dx%d -> энкодер %dx%d @ %d fps%s\n",
                raw.width, raw.height, ow, oh, fps, need_scale ? " (даунскейл)" : "");

    H264Encoder enc;
    if (!enc.init(ow, oh, fps, 6000000)) { std::fprintf(stderr, "encoder init failed\n"); return 3; }

    RtspServer server(port, fps);
    if (!server.start()) { std::fprintf(stderr, "RTSP start failed\n"); return 4; }
    enc.on_nal([&](const uint8_t* d, size_t n, bool key) { server.push_au(d, n, key); });
    server.set_keyframe_requester([&] { enc.request_keyframe(); });

    std::printf("Готово. Открой:  ffplay -fflags nobuffer -flags low_delay "
                "rtsp://<jetson>:%d/cam0\n", port);
    std::printf("Ctrl-C — остановить.\n");

    std::vector<uint8_t> scaled(static_cast<size_t>(ow) * oh * 3);
    std::vector<uint8_t> i420(static_cast<size_t>(ow) * oh * 3 / 2);

    auto t0 = std::chrono::steady_clock::now();
    int count = 0;
    while (g_run) {
        Frame f;
        if (!cam.grab(f, 500)) continue;
        Frame rgb = debayer_to_rgb(f);
        if (!rgb.valid()) continue;

        const uint8_t* rgb_ptr;
        if (need_scale) {
            downscale_rgb24(rgb.data.data(), rgb.width, rgb.height, scaled.data(), ow, oh);
            rgb_ptr = scaled.data();
        } else {
            rgb_ptr = rgb.data.data();
        }
        rgb24_to_i420(rgb_ptr, ow, oh, i420.data());
        enc.encodeFrame(i420.data(), i420.size());

        if (++count % 30 == 0) {
            const double dt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            std::printf("кадров: %d, %.1f fps\r", count, count / dt);
            std::fflush(stdout);
        }
    }

    std::printf("\nОстановка...\n");
    enc.finish();
    server.stop();
    cam.stop();
    cam.close();
    return 0;
}
