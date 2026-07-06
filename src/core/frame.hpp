#pragma once
// Кадр и формат пикселей — базовые типы, общие для всех слоёв.
#include <cstdint>
#include <vector>

namespace camctl {

enum class PixelFormat {
    Unknown,
    BayerRG8,
    BayerGB8,
    BayerGR8,
    BayerBG8,
    RGB24,
};

struct Frame {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Unknown;
    uint64_t frame_id = 0;
    int64_t ts_ns = 0;              // host-метка времени (steady_clock)

    bool valid() const { return width > 0 && height > 0 && !data.empty(); }
};

}  // namespace camctl
