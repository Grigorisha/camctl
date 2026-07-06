# Тепловизор PLUG617R (USB3.0 SC-SDK / GuideUSBCamera)

> Статус: работает
> Обновлён: 2026-07-02
> Автор:

Заметки по тепловизору **PLUG617R** через USB3.0 SC-SDK (GuideUSBCamera). Это UVC-устройство
`04b4:f9f9` ("InfraredCamera"); `gxipy` его НЕ видит. Материалы драйвера — в
`driwers/Linux_USB3.0_V2_0_0-...` (x86_64 и aarch64-сборки, SDK, примеры, документация).

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

## Запуск потока (рабочая конфигурация)

Узел: `/dev/video2` (десктоп) или `/dev/video0` (Jetson без встроенной веб-камеры).
Рабочие параметры: `width=640 height=512 mode=5 version=1`.

**Standalone-превью** (probe разработчиков, mode 5 = картинка + температуры):

```bash
cd driwers/Linux_USB3.0_...          # x86_64 на десктопе, aarch64 на Jetson
./scripts/run_probe.sh --device /dev/video2 --mode 5 --version 1 --width 640 --height 512 \
  --frames 1000000 --timeout-sec 0 --preview-format yuyv-luma --preview-palette blue-red --preview-fps 25
```

**В приложении** (`viewer_qt.py`) тепловизор идёт через ctypes-обёртку `guide_thermal.py`,
которая грузит `libGuideUSBCamera.so`. `.so` собирается из вендорской `.a`:

```bash
gcc -shared -fPIC -o libGuideUSBCamera.so \
  -Wl,--whole-archive .../sdk/lib/libGuideUSBCamera.a -Wl,--no-whole-archive -lpthread
```

⚠️ **OpenCV (`cv2.VideoCapture`) на этой камере ЗАВИСАЕТ** (блокируется в `read()` на
нестандартном формате). Использовать только SDK, не V4L2-захват OpenCV.

## Формат кадра и температуры (разобрано)

Режимы SDK (`guide_usb_video_mode_e`): **mode 5 = `Y16_PARAM_YUV`** отдаёт всё сразу:

- `frame_yuv_data` — YUV422, видимое изображение (люма = тепловая картинка);
- `frame_src_data` — Y16, матрица «сырой» температуры;
- `paramLine` — строка параметров (640×`short`) с точками.

**Температура = значение / 10 °C** (signed `int16`). Индексы в `paramLine`:
hot_spot = 46, cold_spot = 49, cursor = 52, regional_mean = 53 (x/y — рядом).
`device_version` допускает значения 1/2/3.

По «сырому» V4L2 камера отдаёт «удвоенный» кадр (640×1024 = картинка + данные) — это и
сбивает OpenCV; SDK разбирает его сам.

## Открытые вопросы

- Совмещение с RGB-камерой (калибровка)?

## Ссылки

- Настройка драйверов/прав: [drivers-setup.md](drivers-setup.md)
- Форматы кадров: [../03-api/frame-formats.md](../03-api/frame-formats.md)
