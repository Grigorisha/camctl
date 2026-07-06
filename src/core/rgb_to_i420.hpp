#pragma once
// Самописная конвертация RGB24 -> I420 (планарный YUV 4:2:0), BT.601 limited range.
// Нужна для подачи кадров в H.264-энкодер (тот принимает I420).
#include <cstdint>
#include <cstddef>

namespace camctl {

// rgb: интерливнутый R,G,B (3 байта/пиксель), width*height*3 байт.
// out: буфер width*height*3/2 байт (Y-плоскость, затем U, затем V).
// width и height должны быть чётными (требование 4:2:0).
void rgb24_to_i420(const uint8_t* rgb, int width, int height, uint8_t* out);

}  // namespace camctl
