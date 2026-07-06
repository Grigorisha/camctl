#include "debayer.hpp"

#include <cstddef>

namespace camctl {

Frame debayer_to_rgb(const Frame& b) {
    Frame out;
    if (b.width <= 0 || b.height <= 0 || b.data.empty()) return out;

    const int W = b.width, H = b.height;
    out.width = W;
    out.height = H;
    out.format = PixelFormat::RGB24;
    out.frame_id = b.frame_id;
    out.ts_ns = b.ts_ns;
    out.data.resize(static_cast<size_t>(W) * H * 3);

    const uint8_t* s = b.data.data();
    uint8_t* d = out.data.data();

    // Позиции R и B внутри верхнего-левого блока 2x2 (col, row в {0,1}).
    int rx, ry, bx, by;
    switch (b.format) {
        case PixelFormat::BayerRG8: rx = 0; ry = 0; bx = 1; by = 1; break;  // RGGB
        case PixelFormat::BayerGB8: rx = 0; ry = 1; bx = 1; by = 0; break;  // GBRG
        case PixelFormat::BayerGR8: rx = 1; ry = 0; bx = 0; by = 1; break;  // GRBG
        case PixelFormat::BayerBG8: rx = 1; ry = 1; bx = 0; by = 0; break;  // BGGR
        default:                    rx = 0; ry = 0; bx = 1; by = 1; break;
    }

    auto at = [&](int x, int y) -> int {
        if (x < 0) x = 0; else if (x >= W) x = W - 1;
        if (y < 0) y = 0; else if (y >= H) y = H - 1;
        return s[static_cast<size_t>(y) * W + x];
    };

    const int px[4] = {0, 1, 0, 1};
    const int py[4] = {0, 0, 1, 1};

    for (int y0 = 0; y0 < H; y0 += 2) {
        for (int x0 = 0; x0 < W; x0 += 2) {
            const int R = at(x0 + rx, y0 + ry);
            const int B = at(x0 + bx, y0 + by);
            int gsum = 0, gcnt = 0;
            for (int k = 0; k < 4; ++k) {
                if ((px[k] == rx && py[k] == ry) || (px[k] == bx && py[k] == by)) continue;
                gsum += at(x0 + px[k], y0 + py[k]);
                ++gcnt;
            }
            const int G = gcnt ? gsum / gcnt : 0;
            for (int k = 0; k < 4; ++k) {
                const int ox = x0 + px[k], oy = y0 + py[k];
                if (ox >= W || oy >= H) continue;
                uint8_t* dp = d + (static_cast<size_t>(oy) * W + ox) * 3;
                dp[0] = static_cast<uint8_t>(R);
                dp[1] = static_cast<uint8_t>(G);
                dp[2] = static_cast<uint8_t>(B);
            }
        }
    }
    return out;
}

}  // namespace camctl
