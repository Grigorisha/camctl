#pragma once
// Драйвер Daheng через GxIAPI (C-API Galaxy). Работает с NS-301UCL (RG8) и ME2P (GB8):
// формат Байера определяется из кадра, не хардкодится.
#include "camera/icamera.hpp"
#include <string>

namespace camctl {

class DahengCamera : public ICamera {
public:
    explicit DahengCamera(std::string serial = "");  // пустой = первая найденная
    ~DahengCamera() override;

    bool open() override;
    bool start() override;
    bool grab(Frame& out, int timeout_ms) override;
    void stop() override;
    void close() override;
    std::string info() const override { return info_; }

private:
    std::string serial_;
    std::string info_;
    void* handle_ = nullptr;   // GX_DEV_HANDLE (прячем тип SDK из заголовка)
    bool lib_init_ = false;
};

}  // namespace camctl
