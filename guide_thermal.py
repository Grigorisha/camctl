"""ctypes-обёртка над GuideUSBCamera SDK (тепловизор PLUG617R).

SDK поставляется статической либой; собранную из неё libGuideUSBCamera.so грузим
через ctypes. Кадр приходит в C-коллбэке: берём YUV-люму как изображение и
param-строку как температуры (hot/cold/cursor/mean, значение/10 = °C).

Рабочая конфигурация для этой камеры: width=640, height=512, mode=5
(Y16_PARAM_YUV — картинка + температурная матрица + параметры), version=1.
"""

import os
import time
import ctypes as C
import threading

import numpy as np

_LIB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "libGuideUSBCamera.so")

# guide_usb_video_mode_e
Y16_PARAM_YUV = 5

# индексы температур в param-строке (как в выводе probe), значение/10 = °C
_P_HOT, _P_COLD, _P_CURSOR, _P_MEAN = 46, 49, 52, 53


class _DeviceInfo(C.Structure):
    _fields_ = [
        ("width", C.c_int),
        ("height", C.c_int),
        ("video_mode", C.c_int),
        ("device_version", C.c_int),
    ]


class _FrameData(C.Structure):
    _fields_ = [
        ("frame_width", C.c_int),
        ("frame_height", C.c_int),
        ("frame_src_data", C.POINTER(C.c_short)),
        ("frame_src_data_length", C.c_int),
        ("frame_yuv_data", C.POINTER(C.c_short)),
        ("frame_yuv_data_length", C.c_int),
        ("paramLine", C.POINTER(C.c_short)),
        ("paramLine_length", C.c_int),
    ]


_FRAME_CB = C.CFUNCTYPE(C.c_int, C.POINTER(_FrameData))
_STATUS_CB = C.CFUNCTYPE(C.c_int, C.c_int)


class GuideThermal:
    """Захват тепловизора через C-SDK. SDK сам пушит кадры в коллбэк из своего
    потока; мы складываем последний кадр под локом, GUI забирает get_latest()."""

    def __init__(self, device="/dev/video2", width=640, height=512,
                 mode=Y16_PARAM_YUV, version=1):
        self.device = device
        self.width = width
        self.height = height
        self.mode = mode
        self.version = version

        self._lib = C.CDLL(_LIB_PATH)
        self._lib.guide_usb_initialize.argtypes = [C.c_char_p]
        self._lib.guide_usb_initialize.restype = C.c_int
        self._lib.guide_usb_openStream.argtypes = [C.POINTER(_DeviceInfo), _FRAME_CB, _STATUS_CB]
        self._lib.guide_usb_openStream.restype = C.c_int
        self._lib.guide_usb_closeStream.restype = C.c_int
        self._lib.guide_usb_exit.restype = C.c_int
        self._lib.guide_usb_setLogLevel.argtypes = [C.c_int]

        self._lock = threading.Lock()
        self._latest = None          # luma uint8 (h, w)
        self._temps = {}
        self._connected = False
        self._got_frame = False
        self._last_frame_t = 0.0
        # держим ссылки на коллбэки, иначе их соберёт GC -> краш в C
        self._frame_cb = _FRAME_CB(self._on_frame)
        self._status_cb = _STATUS_CB(self._on_status)
        self._info = _DeviceInfo(width, height, mode, version)

    # --- C-коллбэки (вызываются из потока SDK) ---
    def _on_status(self, status):
        with self._lock:
            self._connected = (status == 1)
        return 0

    def _on_frame(self, pdata):
        try:
            fd = pdata.contents
            w, h = fd.frame_width, fd.frame_height
            luma = None
            if fd.frame_yuv_data and fd.frame_yuv_data_length > 0 and w > 0 and h > 0:
                nbytes = fd.frame_yuv_data_length * 2     # длина в short -> в байты
                raw = C.string_at(fd.frame_yuv_data, nbytes)
                arr = np.frombuffer(raw, dtype=np.uint8)
                y = arr[0::2]                              # YUYV: люма в чётных байтах
                if y.size >= w * h:
                    luma = y[:w * h].reshape(h, w).copy()
            temps = {}
            if fd.paramLine and fd.paramLine_length > _P_MEAN:
                praw = C.string_at(fd.paramLine, fd.paramLine_length * 2)
                p = np.frombuffer(praw, dtype=np.int16)
                temps = {
                    "hot": p[_P_HOT] / 10.0,
                    "cold": p[_P_COLD] / 10.0,
                    "cursor": p[_P_CURSOR] / 10.0,
                    "mean": p[_P_MEAN] / 10.0,
                }
            with self._lock:
                if luma is not None:
                    self._latest = luma
                    self._got_frame = True
                    self._last_frame_t = time.monotonic()
                if temps:
                    self._temps = temps
        except Exception as e:  # noqa: BLE001 — коллбэк не должен бросать в C
            print("GuideThermal callback error:", e)
        return 0

    # --- управление ---
    def start(self):
        self._lib.guide_usb_setLogLevel(0)  # тихо
        if self._lib.guide_usb_initialize(self.device.encode()) < 0:
            return False
        if self._lib.guide_usb_openStream(C.byref(self._info), self._frame_cb, self._status_cb) < 0:
            try:
                self._lib.guide_usb_exit()
            except Exception:
                pass
            return False
        return True

    def stop(self):
        try:
            self._lib.guide_usb_closeStream()
        except Exception:
            pass
        try:
            self._lib.guide_usb_exit()
        except Exception:
            pass

    def get_latest(self):
        """(luma uint8 HxW | None, temps dict, connected bool)."""
        with self._lock:
            luma, self._latest = self._latest, None
            return luma, dict(self._temps), self._connected

    def got_frame(self):
        with self._lock:
            return self._got_frame

    def since_last_frame(self):
        with self._lock:
            if self._last_frame_t == 0.0:
                return 1e9
            return time.monotonic() - self._last_frame_t


if __name__ == "__main__":
    # автономный тест: запустить, подождать кадры, показать температуры
    import sys
    import time

    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/video2"
    cam = GuideThermal(dev)
    print("start:", cam.start())
    deadline = time.time() + 8
    n = 0
    while time.time() < deadline:
        luma, temps, conn = cam.get_latest()
        if luma is not None:
            n += 1
            if n % 10 == 1:
                print(f"кадр {n}: shape={luma.shape} dtype={luma.dtype} "
                      f"min/max={int(luma.min())}/{int(luma.max())} | temps={temps}")
        time.sleep(0.03)
    print(f"итого кадров: {n}, connected={conn}")
    cam.stop()
