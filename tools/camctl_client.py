#!/usr/bin/env python3
"""CLI-клиент camctl: управляющий канал (TCP+JSON) + проверка работоспособности камеры.

Управление (без тяжёлых зависимостей — только сокет+json):
  params | get <name> | set <name> <value> | stats | time
  presets | load <name> | save <name>

Проверка потока (health/latency — нужен PyAV: pip install av):
  health   — сквозная проверка: control + часы + декод потока (кадры/FPS/задержка)
  latency  — только замер задержки/FPS по потоку

  python3 camctl_client.py --host 192.168.1.100 --ctrl 8555 --rtsp 8554 <команда> [аргументы]
"""
import argparse
import json
import socket
import sys
import time

SEI_UUID = b"CAMCTL-TS-0001!!"


# ---- управляющий канал --------------------------------------------------

def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    return s, s.makefile("r")


def rpc(s, f, obj):
    s.sendall((json.dumps(obj) + "\n").encode())
    return json.loads(f.readline())


def coerce(v: str):
    """Строку из CLI привести к числу/булеву/строке."""
    if v.lower() in ("true", "false"):
        return v.lower() == "true"
    try:
        return int(v)
    except ValueError:
        pass
    try:
        return float(v)
    except ValueError:
        return v


def clock(s, f):
    """Смещение часов сервер-клиент (нс) и RTT — как в SEI-задержке (CLOCK_MONOTONIC)."""
    t1 = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    r = rpc(s, f, {"cmd": "time", "t1": t1})
    t4 = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    t2, t3 = int(r["t2"]), int(r["t3"])
    offset = ((t2 - t1) + (t3 - t4)) / 2
    rtt = (t4 - t1) - (t3 - t2)
    return offset, rtt


# ---- разбор SEI (для задержки) ------------------------------------------

def epb_decode(b: bytes) -> bytes:
    out = bytearray(); zeros = 0
    for x in b:
        if zeros >= 2 and x == 3:
            zeros = 0; continue
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


def capture_ts(pkt: bytes):
    i = pkt.find(SEI_UUID)
    if i < 0:
        return None
    dec = epb_decode(pkt[i + len(SEI_UUID): i + len(SEI_UUID) + 16])
    return int.from_bytes(dec[:8], "big") if len(dec) >= 8 else None


def probe_stream(host, rtsp, offset, seconds=3.0):
    """Декодировать ~seconds потока: (кадров, fps, задержка_мс|None). Требует PyAV."""
    import av
    url = f"rtsp://{host}:{rtsp}/cam0"
    opts = {"rtsp_transport": "udp", "fflags": "nobuffer", "flags": "low_delay"}
    container = av.open(url, options=opts, timeout=5)
    frames = 0
    lat = None
    t0 = time.monotonic()
    try:
        vs = container.streams.video[0]
        for packet in container.demux(vs):
            ts = capture_ts(bytes(packet))
            for frame in packet.decode():
                frames += 1
                if ts is not None:
                    now = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
                    lat = (now - ts + offset) / 1e6
            if time.monotonic() - t0 > seconds:
                break
    finally:
        container.close()
    dt = max(1e-6, time.monotonic() - t0)
    return frames, frames / dt, lat, url


# ---- команды ------------------------------------------------------------

def do_params(s, f, a):
    for p in rpc(s, f, {"cmd": "get_params"}).get("params", []):
        rng = f" [{p.get('min')}..{p.get('max')}]" if "min" in p else ""
        unit = "/" + p["unit"] if p.get("unit") else ""
        print(f"  {p['name']:16} = {str(p['value']):>12}  ({p['type']}{unit}){rng}")
    return 0


def do_get(s, f, a):
    print(json.dumps(rpc(s, f, {"cmd": "get", "name": a.args[0]}), ensure_ascii=False))
    return 0


def do_set(s, f, a):
    r = rpc(s, f, {"cmd": "set", "name": a.args[0], "value": coerce(a.args[1])})
    print(json.dumps(r, ensure_ascii=False))
    return 0 if r.get("ok") else 1


def do_stats(s, f, a):
    print(json.dumps(rpc(s, f, {"cmd": "stats"}).get("stats", {}), ensure_ascii=False, indent=2))
    return 0


def do_time(s, f, a):
    off, rtt = clock(s, f)
    print(f"offset={off/1e6:.3f} мс  rtt={rtt/1e6:.3f} мс")
    return 0


def do_presets(s, f, a):
    print(rpc(s, f, {"cmd": "list_presets"}).get("presets"))
    return 0


def do_load(s, f, a):
    r = rpc(s, f, {"cmd": "load_preset", "name": a.args[0]})
    print(json.dumps(r, ensure_ascii=False))
    return 0 if r.get("ok") else 1


def do_save(s, f, a):
    r = rpc(s, f, {"cmd": "save_preset", "name": a.args[0]})
    print(json.dumps(r, ensure_ascii=False))
    return 0 if r.get("ok") else 1


def do_latency(s, f, a):
    off, _ = clock(s, f)
    frames, fps, lat, url = probe_stream(a.host, a.rtsp, off, seconds=4.0)
    print(f"{url}: {frames} кадров, {fps:.1f} fps, задержка "
          f"{('%.0f мс' % lat) if lat is not None else 'н/д'}")
    return 0 if frames > 0 else 1


def do_health(s, f, a):
    ok = True
    st = rpc(s, f, {"cmd": "stats"}).get("stats", {})
    print(f"[control] камера={st.get('name')}/{st.get('type')} "
          f"enc={st.get('enc_width')}x{st.get('enc_height')} "
          f"fps(сервер)={st.get('fps', 0):.1f} битрейт={st.get('bitrate')}")
    if "temps_c" in st:
        print(f"[thermal] температуры(°C): {st['temps_c']}")

    off, rtt = clock(s, f)
    print(f"[clock]   rtt={rtt/1e6:.3f} мс  offset={off/1e6:.1f} мс")

    try:
        import av  # noqa: F401
    except ImportError:
        print("[stream]  PyAV не установлен — проверка потока пропущена (pip install av)")
        print("ИТОГ: control OK, поток не проверен")
        return 0

    print(f"[stream]  проверяю поток ...")
    try:
        frames, fps, lat, url = probe_stream(a.host, a.rtsp, off, seconds=3.0)
    except Exception as e:
        print(f"[stream]  ОШИБКА подключения к потоку: {e}")
        print("ИТОГ: ПРОБЛЕМА")
        return 1
    if frames == 0:
        print("[stream]  НЕТ КАДРОВ — камера/поток не работает")
        ok = False
    else:
        print(f"[stream]  OK: {frames} кадров, {fps:.1f} fps, задержка "
              f"{('%.0f мс' % lat) if lat is not None else 'н/д'}")
    print("ИТОГ:", "OK — камера работает" if ok else "ПРОБЛЕМА")
    return 0 if ok else 1


COMMANDS = {
    "params": (do_params, 0), "get": (do_get, 1), "set": (do_set, 2),
    "stats": (do_stats, 0), "time": (do_time, 0), "presets": (do_presets, 0),
    "load": (do_load, 1), "save": (do_save, 1),
    "latency": (do_latency, 0), "health": (do_health, 0),
}


def main():
    ap = argparse.ArgumentParser(description="CLI-клиент camctl")
    ap.add_argument("--host", default="192.168.1.100")
    ap.add_argument("--ctrl", type=int, default=8555)
    ap.add_argument("--rtsp", type=int, default=8554)
    ap.add_argument("cmd", choices=list(COMMANDS.keys()))
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()

    fn, need = COMMANDS[a.cmd]
    if len(a.args) < need:
        ap.error(f"команде '{a.cmd}' нужно {need} аргумент(ов)")
    try:
        s, f = connect(a.host, a.ctrl)
    except OSError as e:
        print(f"не подключиться к control {a.host}:{a.ctrl}: {e}", file=sys.stderr)
        return 2
    try:
        return fn(s, f, a)
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
