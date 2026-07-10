#include "camera/thermal_camera.hpp"

#include "guideusbcamera.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace camctl {

// Индексы температур в param-строке (как в probe разработчиков), значение/10 = °C.
static constexpr int kHot = 46, kCold = 49, kCursor = 52, kMean = 53;

struct ThermalCameraImpl {
    std::string device;
    int width = 640, height = 512, version = 1;
    guide_usb_device_info_t info{};
    bool streaming = false;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> latest_rgb;    // width*height*3 (grayscale, R=G=B=люма)
    int lw = 0, lh = 0;
    uint64_t seq = 0, last_delivered = 0;
    bool connected = false;
    double hot = 0, cold = 0, cursor = 0, mean = 0;
    bool has_temps = false;
};

// Коллбэки SDK — плоские C-указатели без user-data, поэтому один глобальный экземпляр
// (нам и нужен один тепловизор на процесс — процесс-на-камеру).
static ThermalCameraImpl* g_impl = nullptr;

static int on_status(guide_usb_device_status_e st) {
    if (g_impl) {
        std::lock_guard<std::mutex> lk(g_impl->mu);
        g_impl->connected = (st == DEVICE_CONNECT_OK);
    }
    return 0;
}

static int on_frame(guide_usb_frame_data_t* fd) {
    ThermalCameraImpl* p = g_impl;
    if (!p || !fd) return 0;
    const int w = fd->frame_width, h = fd->frame_height;

    // Видимое изображение: YUYV, люма в чётных байтах. frame_yuv_data — short*, длина в short.
    if (fd->frame_yuv_data && fd->frame_yuv_data_length > 0 && w > 0 && h > 0) {
        const uint8_t* yuv = reinterpret_cast<const uint8_t*>(fd->frame_yuv_data);
        const size_t nbytes = static_cast<size_t>(fd->frame_yuv_data_length) * 2;
        const size_t npix = static_cast<size_t>(w) * h;
        if (nbytes >= npix * 2) {
            std::lock_guard<std::mutex> lk(p->mu);
            p->latest_rgb.resize(npix * 3);
            for (size_t i = 0; i < npix; ++i) {
                const uint8_t luma = yuv[i * 2];
                p->latest_rgb[i * 3] = p->latest_rgb[i * 3 + 1] = p->latest_rgb[i * 3 + 2] = luma;
            }
            p->lw = w; p->lh = h; ++p->seq;
            p->cv.notify_one();
        }
    }

    // Точечные температуры из param-строки.
    if (fd->paramLine && fd->paramLine_length > kMean) {
        const short* pl = fd->paramLine;
        std::lock_guard<std::mutex> lk(p->mu);
        p->hot = pl[kHot] / 10.0; p->cold = pl[kCold] / 10.0;
        p->cursor = pl[kCursor] / 10.0; p->mean = pl[kMean] / 10.0;
        p->has_temps = true;
    }
    return 0;
}

ThermalCamera::ThermalCamera(std::string device, int width, int height)
    : p_(new ThermalCameraImpl()) {
    p_->device = std::move(device);
    p_->width = width;
    p_->height = height;
}

ThermalCamera::~ThermalCamera() { close(); delete p_; }

bool ThermalCamera::open() {
    guide_usb_setLogLevel(0);
    if (guide_usb_initialize(p_->device.c_str()) < 0) {
        std::fprintf(stderr, "Thermal: initialize('%s') не удался\n", p_->device.c_str());
        return false;
    }
    return true;
}

bool ThermalCamera::start() {
    p_->info.width = p_->width;
    p_->info.height = p_->height;
    p_->info.video_mode = Y16_PARAM_YUV;   // режим 5: картинка + температуры
    p_->info.device_version = p_->version;
    g_impl = p_;
    if (guide_usb_openStream(&p_->info, on_frame, on_status) < 0) {
        std::fprintf(stderr, "Thermal: openStream не удался\n");
        guide_usb_exit();
        g_impl = nullptr;
        return false;
    }
    p_->streaming = true;
    return true;
}

bool ThermalCamera::grab(Frame& out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(p_->mu);
    const bool ok = p_->cv.wait_for(
        lk, std::chrono::milliseconds(timeout_ms),
        [&] { return p_->seq != p_->last_delivered && p_->lw > 0; });
    if (!ok) return false;
    p_->last_delivered = p_->seq;
    out.width = p_->lw;
    out.height = p_->lh;
    out.format = PixelFormat::RGB24;
    out.frame_id = p_->seq;
    out.data = p_->latest_rgb;   // копия
    return out.valid();
}

void ThermalCamera::stop() {
    if (p_->streaming) { guide_usb_closeStream(); p_->streaming = false; }
}

void ThermalCamera::close() {
    stop();
    if (g_impl == p_) { guide_usb_exit(); g_impl = nullptr; }
}

std::string ThermalCamera::info() const {
    return "PLUG617R thermal " + std::to_string(p_->width) + "x" + std::to_string(p_->height) +
           " (" + p_->device + ")";
}

bool ThermalCamera::temps(double& hot, double& cold, double& cursor, double& mean) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->has_temps) return false;
    hot = p_->hot; cold = p_->cold; cursor = p_->cursor; mean = p_->mean;
    return true;
}

}  // namespace camctl
