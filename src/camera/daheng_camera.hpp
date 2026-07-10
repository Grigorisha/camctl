#pragma once
// Драйвер Daheng через GxIAPI (C-API Galaxy). Работает с NS-301UCL (RG8) и ME2P (GB8):
// формат Байера определяется из кадра, не хардкодится.
#include "camera/icamera.hpp"
#include <string>
#include <vector>

namespace camctl {

class DahengCamera : public ICamera {
public:
    explicit DahengCamera(std::string serial = "");  // пустой = первая найденная
    ~DahengCamera() override;

    // Перечисление подключённых Daheng (FR-01). Две камеры с одним VID:PID различаем по серийнику.
    struct DeviceInfo { std::string model; std::string serial; };
    static std::vector<DeviceInfo> enumerate();

    bool open() override;
    bool start() override;
    bool grab(Frame& out, int timeout_ms) override;
    void stop() override;
    void close() override;
    std::string info() const override { return info_; }

    // Прямой доступ к фичам GenICam по имени (best-effort; false = не поддержано/ошибка).
    // Геометрия (Width/Height/Decimation*) требует остановленного потока — см. camera_service.
    bool set_float(const char* name, double v);
    bool set_int(const char* name, int64_t v);
    bool set_enum(const char* name, const char* v);
    bool get_float(const char* name, double& v) const;
    bool get_int(const char* name, int64_t& v) const;
    bool get_enum(const char* name, std::string& v) const;
    bool get_float_range(const char* name, double& lo, double& hi) const;
    bool get_int_range(const char* name, int64_t& lo, int64_t& hi) const;

private:
    std::string serial_;
    std::string info_;
    void* handle_ = nullptr;   // GX_DEV_HANDLE (прячем тип SDK из заголовка)
    bool lib_init_ = false;
};

}  // namespace camctl
