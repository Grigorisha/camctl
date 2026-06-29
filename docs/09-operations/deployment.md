# Развёртывание

> Статус: черновик
> Обновлён: 2026-06-29
> Автор:

Как собрать и запустить сервис в целевом окружении.

## Окружение

- ОС: Linux x86_64 (десктоп, драйверы под gcc 13); **Jetson aarch64** — см. раздел ниже.
- Подключение камер: USB 3.0.
- Права на устройства настроены — см.
  [../04-hardware/drivers-setup.md](../04-hardware/drivers-setup.md).

## Зависимости

| Компонент | Назначение | Примечание |
|-----------|------------|------------|
| gxipy + Galaxy SDK | RGB-камера Daheng | Python |
| USB3.0 SC-SDK | тепловая камера | C, `driwers/.../sdk/` |
| OpenCV (`cv2`) | обработка/отображение | из `Test.py` |
| numpy | работа с массивами кадров | из `Test.py` |
| CMake / vcpkg | сборка C/C++ части | по `.gitignore` |

## Сборка

- C/C++ часть: CMake (детали — по мере появления `CMakeLists.txt`).
- Пробник тепловой камеры: `driwers/.../scripts/build.sh`.

## Запуск

- Просмотрщик всех камер (GUI): `.venv/bin/python viewer_qt.py`
  (флаги `--thermal-only` / `--daheng-only`).
- Быстрая проверка RGB: `python Test.py` из корня репозитория.

## Развёртывание на Jetson (aarch64)

Рабочий цикл: код правится на десктопе → коммит/пуш → на Jetson `git pull` → запуск.
**Через git едет только Python** (`viewer_qt.py`, `guide_thermal.py`, `cameras.py`).
Нативное (ARM-бинарники) ставится на Jetson один раз и в git не хранится — `.gitignore`
исключает `*.so`, `*.a`, `.venv/`.

### Один раз на Jetson

1. Клонировать репозиторий (по SSH-ключу Jetson или HTTPS-токену).
2. Установить **ARM-версию Galaxy SDK** (Daheng) — даёт `libgxiapi.so` под aarch64 и udev.
3. Получить **ARM-сборку Guide SDK** (`libGuideUSBCamera.a`) у поставщика — в репозитории
   лежит x86_64, на ARM не годится.
4. Прогнать настройку (venv, зависимости, пересборка `.so`, usbfs):

   ```bash
   GUIDE_AR=/путь/к/ARM/libGuideUSBCamera.a bash setup_jetson.sh
   ```

5. Питание/частоты: `sudo nvpmodel -m 0 && sudo jetson_clocks`.

### Запуск на Jetson

На Jetson есть монитор → GUI идёт на его экран. По SSH нужен `DISPLAY`:

```bash
DISPLAY=:0 .venv/bin/python viewer_qt.py
```

### Обновление версии

```bash
git pull && DISPLAY=:0 .venv/bin/python viewer_qt.py
```

Пересборка `.so` / переустановка зависимостей нужны только если менялись именно они,
а не Python-код.

## Конфигурация

- _(Параметры запуска, переменные окружения, файлы конфигурации — по мере появления.)_

## Связанные документы

- Эксплуатация и проблемы: [runbook.md](runbook.md)
- Драйверы: [../04-hardware/drivers-setup.md](../04-hardware/drivers-setup.md)
