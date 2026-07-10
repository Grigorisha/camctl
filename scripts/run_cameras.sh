#!/usr/bin/env bash
# Запуск/останов всех камер из конфига — процесс-на-камеру (ADR-0005).
# Каждая камера = отдельный процесс camera_service со своими портами (изоляция сбоев).
#
#   ./scripts/run_cameras.sh list  [config]   показать камеры из конфига
#   ./scripts/run_cameras.sh start [config]   поднять все камеры (логи в logs/<name>.log)
#   ./scripts/run_cameras.sh stop             остановить все camera_service
#
# Переменные: BIN (по умолч. ./build/camera_service), LOGDIR (logs).
set -euo pipefail

BIN=${BIN:-./build/camera_service}
CONFIG=${2:-config/cameras.json}
LOGDIR=${LOGDIR:-logs}

case "${1:-}" in
  list)
    "$BIN" --config "$CONFIG" --list
    ;;
  start)
    mkdir -p "$LOGDIR"
    # имена камер = первый токен строк, начинающихся с двух пробелов (см. --list)
    names=$("$BIN" --config "$CONFIG" --list | awk '/^  /{print $1}')
    [ -z "$names" ] && { echo "в конфиге нет камер"; exit 1; }
    for n in $names; do
      echo "запускаю $n ..."
      setsid "$BIN" --config "$CONFIG" --name "$n" >"$LOGDIR/$n.log" 2>&1 &
    done
    echo "запущено: $(echo "$names" | tr '\n' ' ')— логи: $LOGDIR/*.log"
    ;;
  stop)
    # -x по имени процесса (не -f, иначе pkill убьёт сам себя по совпадению строки)
    pkill -TERM -x camera_service && echo "остановлено" || echo "нет запущенных camera_service"
    ;;
  *)
    echo "usage: $0 {list|start|stop} [config]"
    exit 1
    ;;
esac
