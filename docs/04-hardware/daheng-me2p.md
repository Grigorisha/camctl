# Daheng ME2P-2621-15U3C (gxipy)

> Статус: в работе
> Обновлён: 2026-06-26
> Автор:

Заметки по работе с промышленной камерой Daheng через Python-SDK `gxipy`. Опорный
пример — `Test.py` в корне репозитория.

## Подключение

1. `gx.DeviceManager()` → `update_all_device_list()` — перечисление камер.
2. `open_device_by_index(index)` — открытие по индексу.
3. `cam.stream_on()` / `cam.stream_off()` — старт/стоп потока.
4. `cam.data_stream[0].get_image()` — получение кадра.
5. `cam.close_device()` — закрытие.

## Ключевые параметры (из `Test.py`)

| Параметр | Значение в примере | Назначение |
|----------|--------------------|------------|
| `PixelFormat` | `BAYER_GB8` | формат пикселя матрицы (требует дебайеризации) |
| `GainSelector` / усиление | `ALL` | выбор канала усиления |
| `GammaMode` + `GammaEnable` | `SRGB`, on | гамма-коррекция |
| `ColorTransformation*` | `RGB_TO_RGB`, on | цветовая коррекция |
| `LightSourcePreset` | `DAYLIGHT_5000K` | пресет источника света |
| `BalanceWhiteAuto` | `CONTINUOUS` | авто-баланс белого |

## Получение RGB

`raw_image.convert(mode="RGB", valid_bits=BIT0_7, convert_type=NEIGHBOUR,
channel_order=ORDER_BGR)` → `get_numpy_array()`.

## Подводные камни / TODO

- Кадр может прийти `None` — нужна проверка перед обработкой (как в `Test.py`).
- Освобождать устройство в `finally` (`stream_off` + `close_device`), иначе камера
  останется занятой.
- _Уточнить: точная модель цвета, нужные пиксельформаты, диапазон экспозиции._

## Ссылки

- Установка SDK / прав: [drivers-setup.md](drivers-setup.md)
- Внешние ресурсы: [../10-reference/links.md](../10-reference/links.md)
