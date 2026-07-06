# Daheng RGB-камеры (gxipy): ME2P-2621-15U3C и NS-301UCL

> Статус: работает
> Обновлён: 2026-07-02
> Автор:

Заметки по работе с промышленными камерами Daheng через Python-SDK `gxipy`. Рабочий код —
`viewer_qt.py` в корне репозитория (`Test.py` — исходный однокамерный пример).

В проекте встречались **две модели** с одинаковым VID:PID `2ba2:4d55`, но разными
характеристиками:

| Модель | Разрешение | Bayer | Примечание |
|--------|-----------|-------|------------|
| ME2P-2621-15U3C | 5120×5120 (26 Мп) | `GB8` | тяжёлая по USB-полосе |
| NS-301UCL | ~0.3 Мп | `RG8` | лёгкая |

## Подключение

1. `gx.DeviceManager()` → `update_all_device_list()` — перечисление камер.
2. `open_device_by_index(index)` — открытие по индексу.
3. `cam.stream_on()` / `cam.stream_off()` — старт/стоп потока.
4. `cam.data_stream[0].get_image()` — получение кадра.
5. `cam.close_device()` — закрытие.

## Ключевые параметры (из `Test.py`)

| Параметр | Значение в примере | Назначение |
|----------|--------------------|------------|
| `PixelFormat` | `BAYER_GB8` (ME2P) / `BAYER_RG8` (NS-301) | формат Байера **зависит от модели**; `viewer_qt.py` выбирает поддерживаемый автоматически |
| `GainSelector` / усиление | `ALL` | выбор канала усиления |
| `GammaMode` + `GammaEnable` | `SRGB`, on | гамма-коррекция |
| `ColorTransformation*` | `RGB_TO_RGB`, on | цветовая коррекция |
| `LightSourcePreset` | `DAYLIGHT_5000K` | пресет источника света |
| `BalanceWhiteAuto` | `CONTINUOUS` | авто-баланс белого |

## Получение RGB

`raw_image.convert(mode="RGB", valid_bits=BIT0_7, convert_type=NEIGHBOUR,
channel_order=ORDER_BGR)` → `get_numpy_array()`.

## Подводные камни (проверено на практике)

- **Bayer-формат зависит от модели.** Жёстко зашитый `BAYER_RG8` падает на ME2P
  (`enum_value out of bounds`), а `BAYER_GB8` — на NS-301. Решение: читать
  `cam.PixelFormat.get_range()` и брать поддерживаемый 8-битный Bayer.
- **«The device has been open».** Камеру может держать один открытый процесс. Нельзя
  запускать два экземпляра `viewer_qt.py` разом; зависший убить (`kill -9 <PID>`) —
  после этого камера освобождается.
- **Права/udev.** Уже подключённые камеры после установки SDK не видны пользователю,
  пока не применятся udev-правила: `sudo udevadm control --reload-rules && sudo udevadm trigger`
  (или переткнуть USB). Без этого `update_all_device_list()` вернёт 0.
- **Полоса USB.** Несколько камер на одной шине (особенно 26 Мп ME2P) дают неполные
  кадры (`RawImage.get_status() != SUCCESS`). Меры в `viewer_qt.py`:
  `DeviceLinkThroughputLimit` ~160 МБ/с на камеру, `AcquisitionFrameRate` 20 fps,
  `usbfs_memory_mb=1000`. Неполные кадры пропускаем по статусу, не по исключению.
- **Авто-экспозиция.** В темноте задирает выдержку до секунд и роняет FPS — ограничиваем
  `AutoExposureTimeMax` (~33 мс).
- Кадр может прийти `None` (таймаут) — проверять перед обработкой.
- Освобождать устройство (`stream_off` + `close_device`), иначе останется занятым.

## Ссылки

- Установка SDK / прав: [drivers-setup.md](drivers-setup.md)
- Внешние ресурсы: [../10-reference/links.md](../10-reference/links.md)
