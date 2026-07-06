# Установка драйверов и настройка прав

> Статус: в работе
> Обновлён: 2026-07-02
> Автор:

Как подготовить машину к работе с камерами: драйверы, права на устройства, проверка.
Развёртывание на **Jetson (aarch64)** описано отдельно —
[../09-operations/deployment.md](../09-operations/deployment.md) (там же — сборка `.so`
тепловизора под ARM и `setup_jetson.sh`).

## RGB-камера (Daheng / gxipy)

1. Установить **Galaxy SDK** нужной архитектуры (x86_64 или aarch64) — он ставит
   системную `libgxiapi.so` + udev-правила. `gxipy` — чистый Python, ставится через pip.
2. Зависимости приложения: `gxipy`, `opencv-python-headless` (`cv2`), `numpy`, `PySide6`
   (см. `requirements.txt`). Полный `opencv-python` НЕ использовать — его Qt конфликтует
   с PySide6.
3. Быстрая проверка: `.venv/bin/python -c "import gxipy; print(gxipy.DeviceManager().update_all_device_list())"`.

## Тепловая камера (USB3.0 SC-SDK)

1. SDK и библиотеки — из `driwers/.../sdk/` (см.
   [thermal-usb3-scsdk.md](thermal-usb3-scsdk.md)).
2. Сборка пробника: `driwers/.../scripts/build.sh`.
3. Запуск: `driwers/.../scripts/run_probe.sh`.

## Права на устройства

USB-камеры обычно появляются как `/dev/video*` и/или USB-устройства. Без настройки
прав доступ требует root.

- Список видеоустройств: `driwers/.../scripts/list_video_devices.sh` (или `v4l2-ctl --list-devices`).
- Выдача прав: `driwers/.../scripts/setup_video_permissions.sh`.
- Для постоянного доступа — добавить пользователя в группу `video` и/или завести
  **udev-правило** под конкретный VID:PID камеры.

Реальные VID:PID: Daheng — `2ba2:4d55`, тепловизор PLUG617R — `04b4:f9f9`. Для Daheng
udev-правило ставит сам Galaxy SDK (`99-galaxy-u3v.rules`), вручную обычно не нужно.

```text
# пример udev-правила (если ставим вручную)
# /etc/udev/rules.d/99-camctl.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="2ba2", ATTR{idProduct}=="4d55", MODE="0660", GROUP="video"
```

После добавления: `sudo udevadm control --reload-rules && sudo udevadm trigger`.
**Важно:** уже подключённые камеры получают новые права только после `udevadm trigger`
(или переподключения USB) — иначе `gxipy` вернёт 0 устройств.

## Проверка

- `lsusb` — видно ли устройство на шине.
- `ls -l /dev/video*` — есть ли узлы и корректны ли права.
- Запуск пробника/`Test.py` — идёт ли поток кадров.

## Связанные документы

- Камеры: [cameras.md](cameras.md)
- Эксплуатация и типовые проблемы: [../09-operations/runbook.md](../09-operations/runbook.md)
