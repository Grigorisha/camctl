// Автономный тест кодера (без камеры): синтетический I420-паттерн -> NVENC -> test.h264.
// Проверка: файл должен декодироваться (ffprobe/ffplay).
//   ./enc_test [width] [height] [fps] [frames]
#include "encode/h264_encoder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    const int W = (argc > 1) ? std::atoi(argv[1]) : 1280;
    const int H = (argc > 2) ? std::atoi(argv[2]) : 720;
    const int fps = (argc > 3) ? std::atoi(argv[3]) : 30;
    const int frames = (argc > 4) ? std::atoi(argv[4]) : 90;

    camctl::H264Encoder enc;
    std::ofstream out("test.h264", std::ios::binary);
    size_t total = 0;
    int nal_chunks = 0, keyframes = 0;
    enc.on_nal([&](const uint8_t* d, size_t n, bool key) {
        out.write(reinterpret_cast<const char*>(d), static_cast<std::streamsize>(n));
        total += n;
        ++nal_chunks;
        if (key) ++keyframes;
    });

    if (!enc.init(W, H, fps, 4000000)) {
        std::fprintf(stderr, "encoder init failed\n");
        return 1;
    }
    std::printf("Кодер: %dx%d @ %d fps, %d кадров\n", W, H, fps, frames);

    std::vector<uint8_t> i420(static_cast<size_t>(W) * H * 3 / 2);
    uint8_t* Y = i420.data();
    uint8_t* U = Y + static_cast<size_t>(W) * H;
    uint8_t* V = U + static_cast<size_t>(W) * H / 4;

    for (int f = 0; f < frames; ++f) {
        // Y — движущийся градиент; U/V — плавно меняющийся цвет (проверка кодека).
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                Y[y * W + x] = static_cast<uint8_t>((x + y + f * 4) & 0xFF);
        std::memset(U, static_cast<uint8_t>(96 + (f & 63)), static_cast<size_t>(W) * H / 4);
        std::memset(V, static_cast<uint8_t>(160 - (f & 63)), static_cast<size_t>(W) * H / 4);
        if (!enc.encodeFrame(i420.data(), i420.size())) {
            std::fprintf(stderr, "encodeFrame %d failed\n", f);
            break;
        }
    }
    enc.finish();
    out.close();

    std::printf("Готово: test.h264, %zu байт, порций NAL: %d, ключевых: %d\n",
                total, nal_chunks, keyframes);
    return total > 0 ? 0 : 2;
}
