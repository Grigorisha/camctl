# Установка драйверов и настройка прав

> Статус: черновик
> Обновлён: 2026-06-26
> Автор:

Как подготовить машину к работе с камерами: драйверы, права на устройства, проверка.

## RGB-камера (Daheng / gxipy)

1. Установить Galaxy SDK и Python-пакет `gxipy` (по инструкции производителя).
2. Проверить зависимости примера: `gxipy`, `opencv-python` (`cv2`), `numpy`.
3. Быстрая проверка: запуск `Test.py` из корня репозитория.

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

```
# пример udev-правила (заполнить idVendor/idProduct реальными значениями)
# /etc/udev/rules.d/99-camctl.rules
SUBSYSTEM=="usb", ATTR{idVendor}=="XXXX", ATTR{idProduct}=="YYYY", MODE="0660", GROUP="video"
```

После добавления: `sudo udevadm control --reload-rules && sudo udevadm trigger`.

## Проверка

- `lsusb` — видно ли устройство на шине.
- `ls -l /dev/video*` — есть ли узлы и корректны ли права.
- Запуск пробника/`Test.py` — идёт ли поток кадров.

## Связанные документы

- Камеры: [cameras.md](cameras.md)
- Эксплуатация и типовые проблемы: [../09-operations/runbook.md](../09-operations/runbook.md)
