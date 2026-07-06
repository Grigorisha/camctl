#pragma once
// Общий контракт камеры: за ним прячем конкретный SDK (Daheng GxIAPI, тепловизор).
#include "core/frame.hpp"
#include <string>

namespace camctl {

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual bool open() = 0;                            // перечислить + открыть + настроить
    virtual bool start() = 0;                           // включить поток
    virtual bool grab(Frame& out, int timeout_ms) = 0;  // один кадр (сырой Bayer); false = таймаут/ошибка
    virtual void stop() = 0;                            // выключить поток
    virtual void close() = 0;                           // закрыть устройство
    virtual std::string info() const = 0;              // модель / серийник
};

}  // namespace camctl
