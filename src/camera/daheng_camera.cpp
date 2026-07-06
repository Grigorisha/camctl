#include "camera/daheng_camera.hpp"

#include "GXDef.h"
#include "GxIAPI.h"
#include "GxPixelFormat.h"

#include <chrono>
#include <cstdio>

namespace camctl {

static int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static PixelFormat map_fmt(int64_t gx) {
    switch (gx) {
        case GX_PIXEL_FORMAT_BAYER_RG8: return PixelFormat::BayerRG8;
        case GX_PIXEL_FORMAT_BAYER_GB8: return PixelFormat::BayerGB8;
        case GX_PIXEL_FORMAT_BAYER_GR8: return PixelFormat::BayerGR8;
        case GX_PIXEL_FORMAT_BAYER_BG8: return PixelFormat::BayerBG8;
        default:                        return PixelFormat::Unknown;
    }
}

DahengCamera::DahengCamera(std::string serial) : serial_(std::move(serial)) {}
DahengCamera::~DahengCamera() { close(); }

bool DahengCamera::open() {
    if (GXInitLib() != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "GXInitLib failed\n");
        return false;
    }
    lib_init_ = true;

    uint32_t num = 0;
    if (GXUpdateAllDeviceList(&num, 1000) != GX_STATUS_SUCCESS || num == 0) {
        std::fprintf(stderr, "Daheng: устройств не найдено (num=%u)\n", num);
        return false;
    }

    // TODO: выбор по серийнику (serial_) через GXGetAllDeviceBaseInfo, когда камер >1.
    GX_DEV_HANDLE h = nullptr;
    if (GXOpenDeviceByIndex(1, &h) != GX_STATUS_SUCCESS) {
        std::fprintf(stderr, "Daheng: не удалось открыть устройство\n");
        return false;
    }
    handle_ = h;

    GX_STRING_VALUE model{}, sn{};
    GXGetStringValue(h, "DeviceModelName", &model);
    GXGetStringValue(h, "DeviceSerialNumber", &sn);
    info_ = std::string(model.strCurValue) + " / " + sn.strCurValue;

    // Базовая настройка захвата.
    GXSetEnumValueByString(h, "AcquisitionMode", "Continuous");
    GXSetEnumValueByString(h, "TriggerMode", "Off");
    // Лимит полосы USB (best-effort — есть не на всех моделях).
    GXSetEnumValueByString(h, "DeviceLinkThroughputLimitMode", "On");
    GXSetIntValue(h, "DeviceLinkThroughputLimit", 160000000);
    // Авто-экспозиция/усиление/баланс белого (best-effort), иначе дефолтный кадр тёмный.
    GXSetEnumValueByString(h, "ExposureAuto", "Continuous");
    GXSetFloatValue(h, "AutoExposureTimeMax", 50000.0);  // потолок авто-выдержки (µs) — не роняем FPS
    GXSetEnumValueByString(h, "GainAuto", "Continuous");
    GXSetEnumValueByString(h, "BalanceWhiteAuto", "Continuous");
    return true;
}

bool DahengCamera::start() {
    if (!handle_) return false;
    return GXStreamOn(static_cast<GX_DEV_HANDLE>(handle_)) == GX_STATUS_SUCCESS;
}

bool DahengCamera::grab(Frame& out, int timeout_ms) {
    if (!handle_) return false;
    PGX_FRAME_BUFFER pfb = nullptr;
    GX_STATUS st = GXDQBuf(static_cast<GX_DEV_HANDLE>(handle_), &pfb, static_cast<uint32_t>(timeout_ms));
    if (st != GX_STATUS_SUCCESS) return false;   // таймаут/ошибка -> вызывающий повторит

    bool ok = false;
    if (pfb->nStatus == GX_FRAME_STATUS_SUCCESS) {
        out.width = pfb->nWidth;
        out.height = pfb->nHeight;
        out.format = map_fmt(pfb->nPixelFormat);
        out.frame_id = pfb->nFrameID;
        out.ts_ns = now_ns();
        const uint8_t* p = static_cast<const uint8_t*>(pfb->pImgBuf);
        out.data.assign(p, p + pfb->nImgSize);
        ok = out.valid();
    }
    GXQBuf(static_cast<GX_DEV_HANDLE>(handle_), pfb);
    return ok;
}

void DahengCamera::stop() {
    if (handle_) GXStreamOff(static_cast<GX_DEV_HANDLE>(handle_));
}

void DahengCamera::close() {
    if (handle_) {
        GXStreamOff(static_cast<GX_DEV_HANDLE>(handle_));
        GXCloseDevice(static_cast<GX_DEV_HANDLE>(handle_));
        handle_ = nullptr;
    }
    if (lib_init_) {
        GXCloseLib();
        lib_init_ = false;
    }
}

}  // namespace camctl
