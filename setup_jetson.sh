#!/usr/bin/env bash
# Одноразовая настройка camctl на Jetson (aarch64).
# Запуск из корня репозитория:   bash setup_jetson.sh
#   с тепловизором:              GUIDE_AR=/путь/к/ARM/libGuideUSBCamera.a bash setup_jetson.sh
#
# Скрипт НЕ ставит вендорские SDK — их надо получить/установить отдельно (см. итог внизу).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

echo "== camctl: настройка на Jetson =="
ARCH="$(uname -m)"
echo "Архитектура: $ARCH"
[ "$ARCH" = "aarch64" ] || echo "ВНИМАНИЕ: ожидался aarch64 (Jetson), а здесь $ARCH."

# --- 1) Python venv + зависимости ---
[ -x .venv/bin/pip ] || { rm -rf .venv; python3 -m venv .venv 2>/dev/null; }
if [ ! -x .venv/bin/pip ]; then
  echo "ОШИБКА: не удалось создать venv с pip. Поставь системные пакеты и перезапусти:"
  echo "  sudo apt update && sudo apt install -y python3-venv python3-pip python3-dev build-essential"
  exit 1
fi
.venv/bin/pip install --upgrade pip
echo "-- numpy / opencv-headless / pillow"
.venv/bin/pip install numpy opencv-python-headless pillow
echo "-- PySide6 (GUI на мониторе Jetson)"
.venv/bin/pip install PySide6 \
  || echo "PySide6 через pip не встал под ARM. Альтернатива: sudo apt install python3-pyside6 (или python3-pyqt5 + правка импортов)."
echo "-- gxipy (Daheng)"
.venv/bin/pip install gxipy \
  || .venv/bin/pip install "driwers/Galaxy_Linux_Python_2.4.2503.9202/Galaxy_Linux_Python_2.4.2503.9202/api" \
  || echo "gxipy не встал ни с PyPI, ни из бандла — установи вручную."

# --- 2) Пересборка libGuideUSBCamera.so из ARM-статической либы ---
if [ -n "${GUIDE_AR:-}" ] && [ -f "$GUIDE_AR" ]; then
  echo "-- собираю libGuideUSBCamera.so из $GUIDE_AR"
  gcc -shared -fPIC -o libGuideUSBCamera.so \
      -Wl,--whole-archive "$GUIDE_AR" -Wl,--no-whole-archive -lpthread \
    && (file libGuideUSBCamera.so | grep -q aarch64 \
          && echo "   OK: .so под ARM" \
          || echo "   ВНИМАНИЕ: .so НЕ aarch64 — проверь, что .a именно ARM-сборки.")
else
  echo "-- ПРОПУСК тепловизора: не задан GUIDE_AR (путь к ARM libGuideUSBCamera.a)."
  echo "   Daheng-камеры заработают; тепловизор — после получения ARM-сборки Guide SDK."
fi

# --- 3) USB-буфер (иначе incomplete-кадры у камер высокого разрешения) ---
CUR="$(cat /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null)"
echo "-- usbfs_memory_mb сейчас: ${CUR:-?}"
echo 1000 | sudo tee /sys/module/usbcore/parameters/usbfs_memory_mb >/dev/null 2>&1 \
  && echo "   выставил 1000 (на текущую сессию)" \
  || echo "   не смог выставить (нужен sudo)"
echo "   Для постоянства: добавь 'usbcore.usbfs_memory_mb=1000' в строку APPEND в"
echo "   /boot/extlinux/extlinux.conf и перезагрузись."

cat <<'EOF'

== Готово по доступному. Ручные шаги (один раз, не через скрипт): ==
  1. Установить ARM-версию Galaxy SDK (Daheng) -> libgxiapi.so под aarch64 + udev-права.
  2. Получить ARM-сборку Guide SDK (.a) у поставщика и прогнать с GUIDE_AR=<путь>.
  3. Питание/частоты Jetson:  sudo nvpmodel -m 0 && sudo jetson_clocks

Запуск (на мониторе Jetson; по SSH GUI требует DISPLAY):
  DISPLAY=:0 .venv/bin/python viewer_qt.py
EOF
