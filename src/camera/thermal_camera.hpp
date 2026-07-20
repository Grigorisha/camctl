#pragma once
// Драйвер тепловизора PLUG617R через GuideUSBCamera SDK (USB3.0 SC-SDK).
// Порт с прототипа guide_thermal.py. Режим 5 (Y16_PARAM_YUV): видимое изображение (YUV-люма)
// + матрица температур + param-строка (hot/cold/cursor/mean, значение/10 = °C).
// SDK пушит кадры в C-коллбэк из своего потока; grab() отдаёт последний как RGB24 (grayscale).
//
// ВНИМАНИЕ: OpenCV (cv2.VideoCapture) на этой камере зависает — только SDK (см. docs).
#include "camera/icamera.hpp"
#include <string>
#include <vector>

namespace camctl {

struct ThermalCameraImpl;  // прячем типы SDK из заголовка

// Цветовая палитра превью (индекс = значение enum, см. paletteNames()).
enum class ThermalPalette { Gray = 0, Ironbow = 1, Rainbow = 2 };

class ThermalCamera : public ICamera {
public:
    explicit ThermalCamera(std::string device = "/dev/video0", int width = 640, int height = 512);
    ~ThermalCamera() override;

    bool open() override;                            // initialize
    bool start() override;                           // openStream (пойдут кадры в коллбэк)
    bool grab(Frame& out, int timeout_ms) override;  // последний кадр -> RGB24 (grayscale/палитра)
    void stop() override;                            // closeStream
    void close() override;                           // exit
    std::string info() const override;

    // Точечные температуры последнего кадра (°C). false, если ещё не приходили.
    bool temps(double& hot, double& cold, double& cursor, double& mean) const;

    // Цветовая палитра применяется в SDK-коллбэке к следующему приходящему кадру (без рестарта).
    void setPalette(ThermalPalette p);
    ThermalPalette getPalette() const;
    static const std::vector<std::string>& paletteNames();  // индекс совпадает с ThermalPalette

    ThermalCamera(const ThermalCamera&) = delete;
    ThermalCamera& operator=(const ThermalCamera&) = delete;

private:
    ThermalCameraImpl* p_;
};

}  // namespace camctl
