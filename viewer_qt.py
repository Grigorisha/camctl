"""Тестовый просмотрщик всех камер проекта + минимальный Qt-интерфейс настройки.

  - 2× Daheng NS-301UCL через gxipy (выдержка/усиление/баланс белого);
  - тепловизор PLUG617R через C-SDK GuideUSBCamera (картинка + температуры °C).

Устойчивость: каждая камера снимается в своём потоке и САМ переподключается при
обрыве кабеля — программа не падает, панель показывает статус, при возврате камеры
захват возобновляется с восстановлением заданных параметров.

Рендер: воркер хранит только последний кадр, GUI рисует по таймеру — кадры не
копятся в очереди (это убирало фриз). Команды смены параметров схлопываются —
применяется только последнее значение (чтобы драг слайдера не топил очередь).

ВАЖНО: тепловизор использует тот же C-SDK, что и probe, поэтому одновременно НЕ
запускай run_probe.sh — оба захватывают /dev/video2.

Запуск:        .venv/bin/python viewer_qt.py
Только теплов.: .venv/bin/python viewer_qt.py --thermal-only
Только Daheng:  .venv/bin/python viewer_qt.py --daheng-only
"""

import os
import sys
import glob
import time
import argparse
import threading

import numpy as np
import cv2  # импортируем cv2 ПЕРЕД PySide6...

# ...и убираем Qt-плагины, которые тащит opencv, чтобы не конфликтовали с PySide6
os.environ.pop("QT_QPA_PLATFORM_PLUGIN_PATH", None)

import gxipy as gx
from guide_thermal import GuideThermal
from PySide6 import QtGui, QtWidgets
from PySide6.QtCore import Qt, QThread, QTimer

THERMAL_NAME_HINT = "Infrared"
GET_IMAGE_TIMEOUT_MS = 500
RENDER_INTERVAL_MS = 33          # ~30 кадров/с отрисовки
DAHENG_FPS_CAP = 20.0            # ограничение частоты Daheng (меньше нагрузка на USB)
AUTO_EXPOSURE_MAX_US = 33000.0   # потолок авто-выдержки (~30 fps), чтобы не было лагов
DAHENG_THROUGHPUT_LIMIT = 160_000_000  # байт/с на камеру: 2×160 + тепловизор влезают в USB3

# доступ к gxipy DeviceManager сериализуем — он один на процесс
_sdk_lock = threading.Lock()
_dm = gx.DeviceManager()


# ----------------------------- доступ к Daheng -----------------------------

def list_daheng():
    """[(sn, index_info), ...] подключённых Daheng-камер."""
    with _sdk_lock:
        _, lst = _dm.update_all_device_list()
        return [(d.get("sn"), d.get("index")) for d in lst]


def open_daheng_by_sn(sn):
    with _sdk_lock:
        _, lst = _dm.update_all_device_list()
        for d in lst:
            if d.get("sn") == sn:
                return _dm.open_device_by_index(d.get("index"))
    return None


def setup_daheng(cam):
    cam.PixelFormat.set(gx.GxPixelFormatEntry.BAYER_RG8)

    def try_set(setter):
        try:
            setter()
        except Exception:
            pass

    try_set(lambda: cam.GainSelector.set(gx.GxGainSelectorEntry.ALL))
    try_set(lambda: cam.ColorTransformationMode.set(gx.GxColorTransformationModeEntry.RGB_TO_RGB))
    try_set(lambda: cam.ColorTransformationEnable.set(True))
    try_set(lambda: cam.BalanceWhiteAuto.set(gx.GxAutoEntry.CONTINUOUS))
    # потолок авто-выдержки, иначе в темноте FPS падает в пол
    try_set(lambda: cam.AutoExposureTimeMax.set(AUTO_EXPOSURE_MAX_US))
    try_set(lambda: cam.AutoExposureTimeMin.set(20.0))
    # ограничим частоту кадров — снижает нагрузку на общую USB-шину
    try_set(lambda: cam.AcquisitionFrameRateMode.set(gx.GxSwitchEntry.ON))
    try_set(lambda: cam.AcquisitionFrameRate.set(DAHENG_FPS_CAP))
    # ограничим пиковую полосу КАЖДОЙ камеры, чтобы две Daheng + тепловизор делили
    # шину без коллизий (иначе неполные кадры). При 20fps нужно ~63 МБ/с — лимит выше.
    try_set(lambda: cam.DeviceLinkThroughputLimitMode.set(gx.GxSwitchEntry.ON))
    try_set(lambda: cam.DeviceLinkThroughputLimit.set(DAHENG_THROUGHPUT_LIMIT))


def read_state(cam):
    def safe(getter, default):
        try:
            return getter()
        except Exception:
            return default

    return {
        "exposure": safe(lambda: cam.ExposureTime.get(), 5000.0),
        "gain": safe(lambda: cam.Gain.get(), 0.0),
        "exposure_auto": safe(lambda: cam.ExposureAuto.get()[0], 0) == gx.GxAutoEntry.CONTINUOUS,
        "gain_auto": safe(lambda: cam.GainAuto.get()[0], 0) == gx.GxAutoEntry.CONTINUOUS,
        "wb_auto": safe(lambda: cam.BalanceWhiteAuto.get()[0], 0) == gx.GxAutoEntry.CONTINUOUS,
    }


# ----------------------------- воркеры захвата -----------------------------

class DahengWorker(QThread):
    """Самовосстанавливающийся захват одной Daheng-камеры (по серийнику)."""

    def __init__(self, sn, title):
        super().__init__()
        self.sn = sn
        self.title = title
        self._running = True
        self._lock = threading.Lock()
        self._latest = None          # последний кадр (BGR ndarray)
        self._status = "ожидание…"
        self._cmds = {}              # name -> value (схлопнутые команды)
        self._settings = {}          # последние применённые настройки (для восстановления)
        self._init_state = None      # стартовые значения для инициализации UI
        self._init_taken = False
        self._convert = dict(
            mode="RGB",
            valid_bits=gx.DxValidBit.BIT0_7,
            convert_type=gx.DxBayerConvertType.NEIGHBOUR,
            channel_order=gx.DxRGBChannelOrder.ORDER_BGR,
        )

    # вызывается из GUI-потока
    def post(self, name, value):
        with self._lock:
            self._cmds[name] = value
            self._settings[name] = value

    def get_latest(self):
        with self._lock:
            frame, self._latest = self._latest, None
            return frame, self._status

    def take_init_state(self):
        with self._lock:
            if self._init_state is not None and not self._init_taken:
                self._init_taken = True
                return self._init_state
            return None

    def stop(self):
        self._running = False

    # --- внутреннее ---
    def _set_status(self, text):
        with self._lock:
            self._status = text

    def _open(self):
        cam = open_daheng_by_sn(self.sn)
        if cam is None:
            return None
        try:
            setup_daheng(cam)
            cam.stream_on()
        except Exception:
            try:
                cam.close_device()
            except Exception:
                pass
            return None
        # восстановить заданные пользователем параметры
        with self._lock:
            settings = dict(self._settings)
            if self._init_state is None:
                self._init_state = read_state(cam)
        for name, value in settings.items():
            try:
                getattr(cam, name).set(value)
            except Exception:
                pass
        return cam

    def _close(self, cam):
        if cam is None:
            return
        try:
            cam.stream_off()
        except Exception:
            pass
        try:
            cam.close_device()
        except Exception:
            pass

    def run(self):
        cam = None
        misses = 0
        while self._running:
            if cam is None:
                self._set_status("нет связи — переподключение…")
                cam = self._open()
                if cam is None:
                    self.msleep(1000)
                    continue
                self._set_status("подключена")
                misses = 0

            # применить схлопнутые команды
            with self._lock:
                cmds, self._cmds = self._cmds, {}
            failed = False
            for name, value in cmds.items():
                try:
                    getattr(cam, name).set(value)
                except Exception as e:  # noqa: BLE001
                    print(f"[{self.title}] {name}={value}: {e}")
                    failed = True
                    break
            if failed:
                self._close(cam)
                cam = None
                continue

            try:
                raw = cam.data_stream[0].get_image(timeout=GET_IMAGE_TIMEOUT_MS)
            except Exception:
                self._close(cam)
                cam = None
                continue

            if raw is None:
                misses += 1
                if misses >= 20:            # ~10 с тишины — считаем потерянной
                    self._close(cam)
                    cam = None
                continue
            misses = 0

            if raw.get_status() != gx.GxFrameStatusList.SUCCESS:
                continue                    # неполный кадр (полоса) — тихо пропускаем

            try:
                rgb = raw.convert(**self._convert)
                arr = rgb.get_numpy_array() if rgb is not None else None
            except Exception:
                continue
            if arr is None:
                continue
            with self._lock:
                self._latest = np.ascontiguousarray(arr)
        self._close(cam)


class GuideThermalWorker(QThread):
    """Самовосстанавливающийся захват тепловизора через C-SDK GuideUSBCamera.
    Люма раскрашивается палитрой, поверх выводятся температуры (°C)."""

    def __init__(self, title="Thermal"):
        super().__init__()
        self.title = title
        self._running = True
        self._lock = threading.Lock()
        self._thermal = None         # GuideThermal
        self._status = "ожидание…"

    def take_init_state(self):
        return None

    def stop(self):
        self._running = False

    def _set_status(self, text):
        with self._lock:
            self._status = text

    def _get_status(self):
        with self._lock:
            return self._status

    def get_latest(self):
        with self._lock:
            thermal = self._thermal
        if thermal is None:
            return None, self._get_status()
        luma, temps, _ = thermal.get_latest()
        if luma is None:
            return None, self._get_status()
        color = cv2.applyColorMap(luma, cv2.COLORMAP_INFERNO)
        if temps:
            txt = (f"hot {temps['hot']:.1f}  cold {temps['cold']:.1f}  "
                   f"cursor {temps['cursor']:.1f}  mean {temps['mean']:.1f} C")
            cv2.putText(color, txt, (8, color.shape[0] - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 2, cv2.LINE_AA)
        return color, self._get_status()

    def is_streaming(self):
        with self._lock:
            return self._thermal is not None

    def run(self):
        # держим ссылки на остановленные экземпляры, чтобы GC не дёрнул их объекты,
        # пока поток SDK ещё может вызвать колбэк
        dead = []
        while self._running:
            self._set_status("подключение…")
            dev = find_thermal_device() or "/dev/video2"
            thermal = GuideThermal(dev)
            if not thermal.start():
                dead.append(thermal)
                self._set_status("нет связи — переподключение…")
                self.msleep(2000)
                continue
            # ждём первый кадр (на свободной шине приходит за ~1 с)
            t0 = time.monotonic()
            while self._running and not thermal.got_frame() and time.monotonic() - t0 < 10:
                self.msleep(100)
            if not thermal.got_frame():
                thermal.stop()
                dead.append(thermal)
                self._set_status("нет кадров — переподключение…")
                self.msleep(2000)
                continue
            with self._lock:
                self._thermal = thermal
            self._set_status("подключён")
            # держим, пока идут кадры; длительное пропадание — переподключение
            while self._running and thermal.since_last_frame() < 8.0:
                self.msleep(300)
            with self._lock:
                self._thermal = None
            thermal.stop()
            dead.append(thermal)


def find_thermal_device():
    for sys_path in sorted(glob.glob("/sys/class/video4linux/video*")):
        try:
            with open(os.path.join(sys_path, "name")) as f:
                card = f.read().strip()
        except OSError:
            continue
        if THERMAL_NAME_HINT.lower() in card.lower():
            return "/dev/" + os.path.basename(sys_path)
    return None


# ----------------------------- GUI -----------------------------

EXPOSURE_MIN, EXPOSURE_MAX = 20, 100000
GAIN_MIN, GAIN_MAX = 0, 240          # 0..24 dB, значение слайдера /10


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, daheng_workers, thermal_worker):
        super().__init__()
        self.setWindowTitle("camctl — просмотр камер")
        self.daheng_workers = daheng_workers     # list[DahengWorker]
        self.thermal_worker = thermal_worker
        self.all_workers = list(daheng_workers) + ([thermal_worker] if thermal_worker else [])
        self.views = {}        # title -> QLabel (видео)
        self.status_lbls = {}  # title -> QLabel (статус)
        self.states = {}       # title -> dict (для Daheng)

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QHBoxLayout(central)

        grid = QtWidgets.QGridLayout()
        for i, w in enumerate(self.all_workers):
            box = QtWidgets.QVBoxLayout()
            cap = QtWidgets.QLabel(w.title)
            cap.setAlignment(Qt.AlignCenter)
            status = QtWidgets.QLabel("ожидание…")
            status.setAlignment(Qt.AlignCenter)
            status.setStyleSheet("color:#aaa;")
            view = QtWidgets.QLabel()
            view.setMinimumSize(440, 330)
            view.setAlignment(Qt.AlignCenter)
            view.setStyleSheet("background:#202020;")
            self.views[w.title] = view
            self.status_lbls[w.title] = status
            box.addWidget(cap)
            box.addWidget(status)
            box.addWidget(view, 1)
            holder = QtWidgets.QWidget()
            holder.setLayout(box)
            grid.addWidget(holder, i // 2, i % 2)
        root.addLayout(grid, 1)

        root.addWidget(self._build_controls())

        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)
        self.timer.start(RENDER_INTERVAL_MS)

    def _build_controls(self):
        panel = QtWidgets.QGroupBox("Параметры Daheng")
        panel.setFixedWidth(300)
        form = QtWidgets.QFormLayout(panel)

        self.cam_select = QtWidgets.QComboBox()
        for w in self.daheng_workers:
            self.cam_select.addItem(w.title)
        self.cam_select.currentIndexChanged.connect(self._load_state_into_ui)
        form.addRow("Камера", self.cam_select)

        self.exp_auto = QtWidgets.QCheckBox("Авто выдержка")
        self.exp_auto.toggled.connect(self._on_exposure_auto)
        form.addRow(self.exp_auto)

        self.exp_slider = QtWidgets.QSlider(Qt.Horizontal)
        self.exp_slider.setRange(EXPOSURE_MIN, EXPOSURE_MAX)
        self.exp_value = QtWidgets.QLabel()
        self.exp_slider.valueChanged.connect(self._on_exposure)
        form.addRow("Выдержка, µs", self.exp_slider)
        form.addRow("", self.exp_value)

        self.gain_auto = QtWidgets.QCheckBox("Авто усиление")
        self.gain_auto.toggled.connect(self._on_gain_auto)
        form.addRow(self.gain_auto)

        self.gain_slider = QtWidgets.QSlider(Qt.Horizontal)
        self.gain_slider.setRange(GAIN_MIN, GAIN_MAX)
        self.gain_value = QtWidgets.QLabel()
        self.gain_slider.valueChanged.connect(self._on_gain)
        form.addRow("Усиление, dB", self.gain_slider)
        form.addRow("", self.gain_value)

        self.wb_auto = QtWidgets.QCheckBox("Авто баланс белого")
        self.wb_auto.toggled.connect(self._on_wb_auto)
        form.addRow(self.wb_auto)

        if not self.daheng_workers:
            panel.setEnabled(False)
        else:
            self._load_state_into_ui()
        return panel

    def _default_state(self):
        return dict(exposure=5000.0, gain=0.0, exposure_auto=False,
                    gain_auto=False, wb_auto=True)

    def _cur(self):
        if not self.daheng_workers:
            return None, None
        i = self.cam_select.currentIndex()
        return self.daheng_workers[i], self.states.get(self.daheng_workers[i].title)

    def _load_state_into_ui(self):
        worker, st = self._cur()
        if worker is None:
            return
        if st is None:
            st = self._default_state()
        for w in (self.exp_auto, self.gain_auto, self.wb_auto,
                  self.exp_slider, self.gain_slider):
            w.blockSignals(True)
        self.exp_auto.setChecked(st["exposure_auto"])
        self.gain_auto.setChecked(st["gain_auto"])
        self.wb_auto.setChecked(st["wb_auto"])
        self.exp_slider.setValue(int(np.clip(st["exposure"], EXPOSURE_MIN, EXPOSURE_MAX)))
        self.gain_slider.setValue(int(np.clip(st["gain"] * 10, GAIN_MIN, GAIN_MAX)))
        for w in (self.exp_auto, self.gain_auto, self.wb_auto,
                  self.exp_slider, self.gain_slider):
            w.blockSignals(False)
        self.exp_slider.setEnabled(not st["exposure_auto"])
        self.gain_slider.setEnabled(not st["gain_auto"])
        self.exp_value.setText(str(self.exp_slider.value()))
        self.gain_value.setText(f"{self.gain_slider.value() / 10:.1f}")

    # --- обработчики параметров ---
    def _on_exposure_auto(self, on):
        worker, st = self._cur()
        if worker is None:
            return
        st["exposure_auto"] = on
        worker.post("ExposureAuto", gx.GxAutoEntry.CONTINUOUS if on else gx.GxAutoEntry.OFF)
        self.exp_slider.setEnabled(not on)

    def _on_gain_auto(self, on):
        worker, st = self._cur()
        if worker is None:
            return
        st["gain_auto"] = on
        worker.post("GainAuto", gx.GxAutoEntry.CONTINUOUS if on else gx.GxAutoEntry.OFF)
        self.gain_slider.setEnabled(not on)

    def _on_wb_auto(self, on):
        worker, st = self._cur()
        if worker is None:
            return
        st["wb_auto"] = on
        worker.post("BalanceWhiteAuto", gx.GxAutoEntry.CONTINUOUS if on else gx.GxAutoEntry.OFF)

    def _on_exposure(self, value):
        worker, st = self._cur()
        if worker is None:
            return
        st["exposure"] = float(value)
        worker.post("ExposureTime", float(value))
        self.exp_value.setText(str(value))

    def _on_gain(self, value):
        worker, st = self._cur()
        if worker is None:
            return
        st["gain"] = value / 10.0
        worker.post("Gain", value / 10.0)
        self.gain_value.setText(f"{value / 10:.1f}")

    # --- периодическая отрисовка ---
    def _tick(self):
        for w in self.all_workers:
            init = w.take_init_state()
            if init is not None:
                self.states[w.title] = init
                if self._cur()[0] is w:
                    self._load_state_into_ui()
            frame, status = w.get_latest()
            self.status_lbls[w.title].setText(status)
            if frame is None:
                continue
            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            h, wd = rgb.shape[:2]
            qimg = QtGui.QImage(rgb.data, wd, h, 3 * wd, QtGui.QImage.Format_RGB888).copy()
            label = self.views[w.title]
            label.setPixmap(QtGui.QPixmap.fromImage(qimg).scaled(
                label.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation))

    def closeEvent(self, event):
        self.timer.stop()
        for w in self.all_workers:
            w.stop()
        for w in self.all_workers:
            w.wait(2000)
        super().closeEvent(event)


def main():
    parser = argparse.ArgumentParser(description="Просмотрщик камер camctl")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--thermal-only", action="store_true",
                       help="запустить ТОЛЬКО тепловизор (без двух Daheng)")
    group.add_argument("--daheng-only", action="store_true",
                       help="запустить только Daheng (без тепловизора)")
    args = parser.parse_args()

    app = QtWidgets.QApplication([])  # CLI-аргументы разбирает argparse, не Qt

    if args.thermal_only:
        daheng_workers = []
        print("Режим: только тепловизор")
    else:
        daheng_workers = [DahengWorker(sn, f"Daheng {sn}") for sn, _ in list_daheng()]
        print(f"Daheng-камер при старте: {len(daheng_workers)}")

    thermal_worker = None if args.daheng_only else GuideThermalWorker()

    if not daheng_workers and thermal_worker is None:
        print("Нечего запускать.")
        return 1

    win = MainWindow(daheng_workers, thermal_worker)
    mode = "тепловизор" if args.thermal_only else "Daheng" if args.daheng_only else "все камеры"
    win.setWindowTitle(f"camctl — {mode}")
    win.resize(1100, 720)
    win.show()

    # Тепловизор стартуем ПЕРВЫМ и даём ему установить поток на свободной шине.
    # Иначе Daheng при старте забивают USB, тепловизор не получает первый кадр и
    # рвёт SDK на нестабильном teardown -> segfault.
    if thermal_worker is not None:
        thermal_worker.start()
        t0 = time.monotonic()
        while time.monotonic() - t0 < 8 and not thermal_worker.is_streaming():
            app.processEvents()
            time.sleep(0.05)

    for w in daheng_workers:
        w.start()

    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
