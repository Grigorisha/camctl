#pragma once
// Самописный дебайер (без сторонних библиотек): 8-битный Bayer -> RGB24.
// Простой блочный nearest-neighbor (2x2): корректные цвета, годится для проверки захвата.
// Качество/скорость улучшим позже (или перенесём на GPU для стрима).
#include "frame.hpp"

namespace camctl {

Frame debayer_to_rgb(const Frame& bayer);

}  // namespace camctl
