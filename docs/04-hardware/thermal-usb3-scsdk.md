# Тепловая камера (USB3.0 SC-SDK / plug617)

> Статус: черновик
> Обновлён: 2026-06-26
> Автор:

Заметки по работе с тепловой камерой через USB3.0 SC-SDK. Материалы драйвера лежат в
`driwers/Linux_USB3.0_V2_0_0-...` (SDK, примеры, документация).

## Где что лежит

- **SDK:** `driwers/.../sdk/` — `include/`, `lib/`, пример `Demo.c`, `ReadMe.txt`.
- **Документация (PDF):**
  - `driwers/.../doc/General USB3.0 SC-SDK Development Documentation V2.0.0.pdf`
  - `driwers/.../doc/通用USB3.0 SC-SDK 开发文档V2.0.0.pdf` (китайская версия)
- **Пробник потока:** `driwers/.../src/plug617_stream_probe.c`,
  собранный бинарь — `driwers/.../bin/plug617_stream_probe`.
- **Скрипты:** `driwers/.../scripts/` — `build.sh`, `run_probe.sh`,
  `sweep_modes.sh`, `list_video_devices.sh`, `setup_video_permissions.sh`.
- **Примеры выходов:** `driwers/.../outputs/run_*`.

## Запуск потока (план)

_Заполнить по мере освоения SDK (по `Demo.c` / `plug617_stream_probe.c`):_

1. Инициализация SDK / открытие устройства.
2. Выбор режима (разрешение, формат, частота) — см. `sweep_modes.sh`.
3. Старт потока, получение кадров.
4. Преобразование отсчётов в температуру (радиометрия).
5. Остановка и освобождение.

## Открытые вопросы

- Пиксельформат и битность теплового кадра?
- Формула/таблица перевода отсчётов в °C?
- Совмещение с RGB-камерой (калибровка)?

## Ссылки

- Настройка драйверов/прав: [drivers-setup.md](drivers-setup.md)
- Форматы кадров: [../03-api/frame-formats.md](../03-api/frame-formats.md)
