#include "core/rgb_to_i420.hpp"

namespace camctl {

static inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Целочисленные коэффициенты BT.601 (limited range: Y∈[16..235], C∈[16..240]).
void rgb24_to_i420(const uint8_t* rgb, int width, int height, uint8_t* out) {
    const int W = width & ~1;   // на всякий случай — чётные размеры
    const int H = height & ~1;

    uint8_t* Yp = out;
    uint8_t* Up = out + static_cast<size_t>(W) * H;
    uint8_t* Vp = Up + static_cast<size_t>(W / 2) * (H / 2);

    // Y — по каждому пикселю.
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = rgb + static_cast<size_t>(y) * width * 3;
        uint8_t* yr = Yp + static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const int R = row[3 * x], G = row[3 * x + 1], B = row[3 * x + 2];
            yr[x] = clamp8(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
        }
    }

    // U/V — усредняем RGB по блоку 2x2, затем одна конвертация на блок.
    for (int y = 0; y < H; y += 2) {
        const uint8_t* r0 = rgb + static_cast<size_t>(y) * width * 3;
        const uint8_t* r1 = rgb + static_cast<size_t>(y + 1) * width * 3;
        uint8_t* ur = Up + static_cast<size_t>(y / 2) * (W / 2);
        uint8_t* vr = Vp + static_cast<size_t>(y / 2) * (W / 2);
        for (int x = 0; x < W; x += 2) {
            const int a = 3 * x, b = 3 * x + 3;
            int R = r0[a]     + r0[b]     + r1[a]     + r1[b];
            int G = r0[a + 1] + r0[b + 1] + r1[a + 1] + r1[b + 1];
            int B = r0[a + 2] + r0[b + 2] + r1[a + 2] + r1[b + 2];
            R >>= 2; G >>= 2; B >>= 2;
            ur[x / 2] = clamp8(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
            vr[x / 2] = clamp8(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
        }
    }
}

}  // namespace camctl
