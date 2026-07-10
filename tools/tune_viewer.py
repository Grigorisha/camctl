#!/usr/bin/env python3
"""camctl — вьюер для подбора параметров (dev-инструмент, НЕ релиз).

Показывает поток rtsp:// с оверлеем FPS и сквозной задержки (glass-to-glass) и
позволяет горячими клавишами менять битрейт/разрешение/децимацию через control-канал.

Задержка меряется по SEI-метке захвата, вложенной сервисом в каждый кадр, с поправкой
на смещение часов Jetson↔ПК (обмен по control-каналу, SNTP-подобно).

  GUI:       python3 tools/tune_viewer.py [host] [rtsp_port] [ctrl_port]
  Проверка:  python3 tools/tune_viewer.py [host] [rtsp_port] [ctrl_port] --headless [sec]

Горячие клавиши (GUI): ↑/↓ битрейт ±1Мбит | 1/2/3/4 разрешение 480/640/960/1280 |
                       d децимация 1↔2 | e авто-экспозиция вкл/выкл | q выход
"""
import sys, socket, json, time, threading, collections
import av

SEI_UUID = b'CAMCTL-TS-0001!!'


def epb_decode(b: bytes) -> bytes:
    """Убрать emulation-prevention байты (0x03 после 00 00) из фрагмента RBSP."""
    out = bytearray()
    zeros = 0
    for x in b:
        if zeros >= 2 and x == 3:
            zeros = 0
            continue
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


def extract_capture_ts(pkt: bytes):
    """Найти UUID и прочитать 8 байт метки захвата (ns, big-endian)."""
    i = pkt.find(SEI_UUID)
    if i < 0:
        return None
    tail = pkt[i + len(SEI_UUID): i + len(SEI_UUID) + 16]
    dec = epb_decode(tail)
    if len(dec) < 8:
        return None
    return int.from_bytes(dec[:8], "big")


class Control:
    """Клиент control-канала (TCP+JSON) + синхронизация часов."""
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.f = self.sock.makefile("r")
        self._id = 0
        self._lock = threading.Lock()
        self.offset = 0.0  # server - client, ns

    def rpc(self, obj):
        with self._lock:
            self._id += 1
            obj = dict(obj); obj["id"] = self._id
            self.sock.sendall((json.dumps(obj) + "\n").encode())
            return json.loads(self.f.readline())

    def sync_clock(self, samples=7):
        best = None  # (rtt, offset)
        for _ in range(samples):
            t1 = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            r = self.rpc({"cmd": "time", "t1": t1})
            t4 = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            t2, t3 = int(r["t2"]), int(r["t3"])
            rtt = (t4 - t1) - (t3 - t2)
            off = ((t2 - t1) + (t3 - t4)) / 2
            if best is None or rtt < best[0]:
                best = (rtt, off)
        self.offset = best[1]
        return best[1], best[0]

    def set(self, name, value):
        try:
            return self.rpc({"cmd": "set", "name": name, "value": value})
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def stats(self):
        try:
            return self.rpc({"cmd": "stats"}).get("stats", {})
        except Exception:
            return {}


class Decoder(threading.Thread):
    """Фоновый поток: демукс+декод rtsp, замер FPS и задержки, колбэк с кадром."""
    def __init__(self, url, ctrl, on_frame):
        super().__init__(daemon=True)
        self.url, self.ctrl, self.on_frame = url, ctrl, on_frame
        self.running = True
        self.times = collections.deque(maxlen=90)
        self.lat_ema = None

    def run(self):
        opts = {"rtsp_transport": "udp", "fflags": "nobuffer",
                "flags": "low_delay", "max_delay": "100000"}
        while self.running:
            try:
                container = av.open(self.url, options=opts, timeout=5)
            except Exception:
                time.sleep(0.5)
                continue
            try:
                vs = container.streams.video[0]
                for packet in container.demux(vs):
                    if not self.running:
                        break
                    cur_ts = extract_capture_ts(bytes(packet))
                    for frame in packet.decode():
                        now = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                        self.times.append(now)
                        fps = 0.0
                        if len(self.times) >= 2:
                            dt = (self.times[-1] - self.times[0]) / 1e9
                            if dt > 0:
                                fps = (len(self.times) - 1) / dt
                        lat_ms = None
                        if cur_ts is not None:
                            lat_ms = (now - cur_ts + self.ctrl.offset) / 1e6
                            self.lat_ema = lat_ms if self.lat_ema is None else \
                                0.8 * self.lat_ema + 0.2 * lat_ms
                        self.on_frame(frame, fps, self.lat_ema)
            except Exception:
                pass
            finally:
                try:
                    container.close()
                except Exception:
                    pass

    def stop(self):
        self.running = False


def run_headless(host, rtsp_port, ctrl_port, seconds):
    ctrl = Control(host, ctrl_port)
    off, rtt = ctrl.sync_clock()
    print(f"Синхронизация часов: offset={off/1e6:.1f} мс, rtt={rtt/1e6:.3f} мс")
    print("stats:", ctrl.stats())

    state = {"fps": 0.0, "lat": None, "n": 0}

    def on_frame(frame, fps, lat_ms):
        state["fps"] = fps
        state["lat"] = lat_ms
        state["n"] += 1

    dec = Decoder(f"rtsp://{host}:{rtsp_port}/cam0", ctrl, on_frame)
    dec.start()
    t0 = time.time()
    while time.time() - t0 < seconds:
        time.sleep(1.0)
        # периодически освежаем смещение часов (дрейф)
        ctrl.sync_clock(samples=3)
        lat = state["lat"]
        print(f"кадров={state['n']:4d}  FPS={state['fps']:5.1f}  "
              f"задержка={('%.0f мс' % lat) if lat is not None else 'н/д'}")
    dec.stop()
    print("готово")


def run_gui(host, rtsp_port, ctrl_port):
    from PySide6 import QtCore, QtGui, QtWidgets

    ctrl = Control(host, ctrl_port)
    off, rtt = ctrl.sync_clock()
    print(f"offset={off/1e6:.1f} мс, rtt={rtt/1e6:.3f} мс")

    class Viewer(QtWidgets.QLabel):
        frame_sig = QtCore.Signal(object, float, object)

        def __init__(self):
            super().__init__()
            self.setWindowTitle("camctl tune viewer")
            self.setMinimumSize(640, 640)
            self.setAlignment(QtCore.Qt.AlignCenter)
            self.setStyleSheet("background:#111;")
            self.stats = {}
            self.res_presets = [480, 640, 960, 1280]
            self.frame_sig.connect(self._on_frame)
            self.timer = QtCore.QTimer(self)
            self.timer.timeout.connect(self._poll)
            self.timer.start(1000)

        def _poll(self):
            self.stats = ctrl.stats()
            ctrl.sync_clock(samples=3)

        def _on_frame(self, frame, fps, lat_ms):
            img = frame.to_ndarray(format="rgb24")
            h, w, _ = img.shape
            qimg = QtGui.QImage(img.data, w, h, 3 * w,
                                QtGui.QImage.Format_RGB888).copy()
            pix = QtGui.QPixmap.fromImage(qimg)
            p = QtGui.QPainter(pix)
            p.setPen(QtGui.QColor(0, 255, 120))
            f = QtGui.QFont("monospace", max(11, w // 60)); f.setBold(True)
            p.setFont(f)
            lat = f"{lat_ms:.0f} мс" if lat_ms is not None else "н/д"
            br = self.stats.get("bitrate", 0)
            lines = [
                f"FPS: {fps:.1f}",
                f"Задержка: {lat}",
                f"Разрешение: {self.stats.get('enc_width','?')}x{self.stats.get('enc_height','?')}",
                f"Битрейт: {br/1e6:.1f} Мбит",
                f"Децимация: {self.stats.get('decimation','?')}",
            ]
            y = int(h * 0.04) + 20
            for ln in lines:
                p.drawText(12, y, ln); y += int(f.pointSize() * 1.8)
            p.end()
            self.setPixmap(pix.scaled(self.size(), QtCore.Qt.KeepAspectRatio,
                                      QtCore.Qt.SmoothTransformation))

        def keyPressEvent(self, e):
            k = e.key()
            s = self.stats
            if k in (QtCore.Qt.Key_Q, QtCore.Qt.Key_Escape):
                self.close()
            elif k == QtCore.Qt.Key_Up:
                ctrl.set("bitrate", min(50_000_000, int(s.get("bitrate", 6e6)) + 1_000_000))
            elif k == QtCore.Qt.Key_Down:
                ctrl.set("bitrate", max(100_000, int(s.get("bitrate", 6e6)) - 1_000_000))
            elif k in (QtCore.Qt.Key_1, QtCore.Qt.Key_2, QtCore.Qt.Key_3, QtCore.Qt.Key_4):
                r = self.res_presets[k - QtCore.Qt.Key_1]
                ctrl.set("enc_width", r); ctrl.set("enc_height", r)
            elif k == QtCore.Qt.Key_D:
                ctrl.set("decimation", 1 if int(s.get("decimation", 1)) != 1 else 2)
            elif k == QtCore.Qt.Key_E:
                ctrl.set("auto_exposure", not s.get("auto_exposure", True))

        def closeEvent(self, e):
            self.dec.stop(); e.accept()

    app = QtWidgets.QApplication(sys.argv)
    v = Viewer()
    v.dec = Decoder(f"rtsp://{host}:{rtsp_port}/cam0", ctrl,
                    lambda fr, fps, lat: v.frame_sig.emit(fr, fps, lat))
    v.dec.start()
    v.resize(900, 900); v.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.100"
    rtsp_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8554
    ctrl_port = int(sys.argv[3]) if len(sys.argv) > 3 else 8555
    if "--headless" in sys.argv:
        i = sys.argv.index("--headless")
        secs = int(sys.argv[i + 1]) if len(sys.argv) > i + 1 else 6
        run_headless(host, rtsp_port, ctrl_port, secs)
    else:
        run_gui(host, rtsp_port, ctrl_port)
