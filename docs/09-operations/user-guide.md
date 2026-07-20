# Юзергайд: сервер (Jetson) и клиент (ПК)

> Статус: в работе
> Обновлён: 2026-07-20
> Автор:

Практическое руководство по текущему **C++-релизу** camctl: `camera_service` на Jetson +
клиентские инструменты на ПК. В отличие от этого документа, [deployment.md](deployment.md) и
[runbook.md](runbook.md) описывают старый **Python-прототип** (`viewer_qt.py`, `Test.py`) —
он больше не актуален для повседневной работы, см. [ADR-0003](../02-architecture/decisions/0003-service-language-cpp.md).

## Архитектура в двух словах

Процесс-на-камеру ([ADR-0005](../02-architecture/decisions/0005-process-per-camera.md)): на
Jetson на каждую камеру поднимается свой `camera_service` — захват → дебайер/цветокоррекция →
I420 → NVENC H.264 → RTP/RTSP ([ADR-0002](../02-architecture/decisions/0002-streaming-transport.md),
[ADR-0004](../02-architecture/decisions/0004-streaming-inhouse-nvenc.md)), плюс отдельный
управляющий канал TCP+JSON ([ADR-0006](../02-architecture/decisions/0006-control-protocol-tcp-json.md)).
Один сбой камеры не валит остальные — изоляция на уровне процесса.

Клиент (ПК) подключается по сети к обоим портам каждой камеры: RTSP (видео) и control (управление).

---

## Часть 1. Сервер (Jetson)

### 1.1 Единоразовая настройка нового Jetson

1. **USB3-хаб с внешним питанием** (не USB2!) — две Daheng одновременно требуют реальной
   USB3-полосы. См. [../04-hardware/drivers-setup.md](../04-hardware/drivers-setup.md).
2. **Galaxy SDK (Daheng, aarch64)**: `driwers/Galaxy_Linux-arm64_.../Galaxy_camera.run`.
   Установщик интерактивный (`press Enter`, дальше — sudo-подтверждение), автоматизируется так:
   ```bash
   yes '' | sudo ./Galaxy_camera.run
   ```
   Udev-правила применяются сразу — `sudo udevadm trigger --subsystem-match=usb --action=add`,
   без перезагрузки/переподключения камеры.
3. **Guide SDK (тепловизор)**: нужен `libGuideUSBCamera.so` под aarch64 **в корне репозитория
   на Jetson**. Проще всего — взять готовый бинарник из вендорского пакета
   (`driwers/Linux_USB3.0_..-aarch64.../sdk/lib/libGuideUSBCamera.so`, проверить архитектуру
   `file ...` → `ARM aarch64`) и скопировать `scp` прямо в `$(JDIR)/libGuideUSBCamera.so`.
   Альтернатива — собрать из `.a`:
   ```bash
   gcc -shared -fPIC -o libGuideUSBCamera.so \
     -Wl,--whole-archive .../libGuideUSBCamera.a -Wl,--no-whole-archive -lpthread
   ```
   `make sync`/`jbuild` **никогда не трогают** этот файл на Jetson (см. exclude в `Makefile`) —
   в репозитории на ПК лежит x86_64-версия только для локальной проверки синтаксиса.
4. **Инструменты сборки**: `cmake`, `nvidia-l4t-jetson-multimedia-api`, минимальный CUDA-рантайм.
   На «чистом» JetPack-образе с настроенными nvidia-репозиториями (`/etc/apt/sources.list.d/nvidia-l4t-apt-source.list`):
   ```bash
   sudo apt update
   sudo apt install cmake nvidia-l4t-jetson-multimedia-api cuda-cudart-12-6
   ```
   Версию `cuda-cudart-12-X` подбирайте под версию L4T (`apt-cache policy nvidia-l4t-core`) —
   полный `cuda-toolkit` не нужен, в коде нет прямых вызовов CUDA API, только линковка runtime.
5. Питание/частоты (не обязательно, но снимает лишний тротлинг):
   `sudo nvpmodel -m 0 && sudo jetson_clocks`.

### 1.2 SSH-алиас и путь на Jetson

`Makefile` параметризован переменными `JETSON` (SSH-алиас/хост) и `JDIR` (путь на Jetson,
относительно `$HOME` там же). Дефолты (`JETSON=spam`, `JDIR=gregory/camctl`) заточены под
исходный Jetson — для любого другого передавайте оба явно:

```bash
make jbuild JETSON=jetson2 JDIR=camctl
```

Держите `~/.ssh/config` с алиасом и отдельным ключом на устройство — так `ssh <алиас>`
работает без пароля и без лишних вопросов при каждом деплое.

### 1.3 Деплой и сборка

| Команда | Что делает |
|---|---|
| `make sync JETSON=... [JDIR=...]` | rsync исходников на Jetson (без `.git`, `.venv`, `build/`, `driwers/`, `libGuideUSBCamera.so`, **`logs/`, `presets/`** — эти два каталога рантайм-данные, sync их не трогает, чтобы не терять логи и сохранённые пресеты при каждом деплое) |
| `make jbuild JETSON=... [JDIR=...]` | `sync` + `cmake -S . -B build && cmake --build build -j` на Jetson |
| `make build` | локальная сборка на ПК — **только проверка синтаксиса**; `camera_service`/`stream_test`/`enc_test` (NVENC, тепловизор) собираются ТОЛЬКО там, где есть `jetson_multimedia_api`, то есть на Jetson |
| `make clean` | удалить локальный `build/` |

После `rsync` исходники получают свежий `mtime` (`touch`) — иначе часы Jetson, отстающие от
ПК, дают ложное «уже собрано» и правки не пересобираются.

### 1.4 Конфигурация камер — `config/cameras.json`

```json
{
  "cameras": [
    { "name": "cam0", "type": "daheng", "serial": "FCN25010025",
      "rtsp_port": 8554, "ctrl_port": 8555, "fps": 30, "maxside": 1280, "preset": "" },
    { "name": "cam1", "type": "daheng", "serial": "FCN25010026",
      "rtsp_port": 8556, "ctrl_port": 8557, "fps": 30, "maxside": 1280, "preset": "" },
    { "name": "thermal", "type": "thermal", "device": "/dev/video0",
      "rtsp_port": 8558, "ctrl_port": 8559, "fps": 25, "maxside": 640, "preset": "" }
  ]
}
```

Поля: `name` (произвольное, для логов/`--name`), `type` — `daheng` | `thermal`, `serial`
(Daheng, узнать через `--list-cameras`; для одиночной камеры можно `""` = первая найденная,
для стереопары нужны оба явных серийника), `device` (тепловизор, `/dev/videoN`), `rtsp_port`,
`ctrl_port`, `fps`, `maxside` (даунскейл по большей стороне), `preset` (имя стартового пресета,
опционально).

### 1.5 Запуск и останов

| Команда | Что делает |
|---|---|
| `make list-cameras JETSON=...` | серийники подключённых Daheng (без конфига) |
| `make cams JETSON=... [CONFIG=...]` | список камер из конфига |
| `make up JETSON=... [JDIR=...]` | поднять ВСЕ камеры из конфига в фоне; логи — `logs/<name>.log` |
| `make down JETSON=...` | остановить все `camera_service` (`pkill -TERM -x camera_service`, точное имя — не трогает другие процессы на машине) |
| `make logs JETSON=...` | хвосты всех логов |
| `make run-cam NAME=cam0 JETSON=...` | одна камера из конфига, foreground (Ctrl-C = стоп) |
| `make run SERIAL=.. RTSP=.. CTRL=.. FPS=.. MAXSIDE=.. PRESET=.. JETSON=...` | Daheng без конфига |

Если нужно перезапустить/остановить **только одну** камеру без риска зацепить остальные
(например, чинить подвисший тепловизор, не трогая уже работающие Daheng) — не используйте
`make down` (убьёт все), а найдите PID точечно: `pgrep -f 'name thermal'` → `kill -TERM <pid>`.

### 1.6 Управляющий канал (control, TCP + JSON)

Не HTTP — построчный JSON, один объект на запрос/ответ:

```
→ {"id":1,"cmd":"get_params"}
← {"id":1,"ok":true,"params":[{"name":"bitrate","type":"int","unit":"bps","value":6000000,"min":100000,"max":50000000}, ...]}
```

Команды: `get_params`, `get {"name":...}`, `set {"name":...,"value":...}`, `stats`,
`time {"t1":...}` (обмен часами, `CLOCK_MONOTONIC` — реальные часы Jetson шагает NTP и ломает
замер задержки), `load_preset {"name":...}`, `save_preset {"name":...}`, `list_presets`.

### 1.7 Параметры («ручки») по типу камеры

Общие (любая камера):

| Параметр | Тип | Диапазон | Эффект |
|---|---|---|---|
| `bitrate` | int, bps | 100000..50000000 | рантайм, без пересоздания энкодера |
| `enc_width` / `enc_height` | int | 16..4096 | пересоздание энкодера (width+height коалесцируются в одно пересоздание) |
| `fps` | int | 1..60 | пересоздание энкодера |

Только Daheng:

| Параметр | Тип | Эффект |
|---|---|---|
| `exposure_us` | float, µs (диапазон — из реального GenICam-range камеры) | рантайм |
| `auto_exposure` | bool | рантайм |
| `gain` | float, dB | рантайм |
| `decimation` | int, 1..8 | рестарт стрима камеры (сенсор меняет разрешение) |

Только тепловизор (PLUG617R):

| Параметр | Тип | Значения | Эффект |
|---|---|---|---|
| `palette` | enum | `gray` (по умолчанию) / `ironbow` / `rainbow` | рантайм — применяется к следующему кадру из SDK-коллбэка, без рестарта потока |

`ironbow` — тёмно-синий/фиолетовый (холодно) → красный/оранжевый → жёлто-белый (горячо),
универсальная палитра для поиска тепловых аномалий. `rainbow` — тот же диапазон, но шире
гамма (добавлен зелёный) — лучше видны минимальные перепады температур в однородной сцене.

### 1.8 Пресеты

`presets/<name>.json` — плоский объект `{"имя_ручки": значение}`, применяется тем же путём,
что и обычный `set` (значит reconfig/пересоздание энкодера и т.п. отрабатывают штатно).
`load_preset`/`save_preset`/`list_presets` — через control-канал (см. 1.6) или клиентские
`make -C client preset|save-preset|presets` (см. 2.4). Стартовый пресет — поле `preset` в
конфиге камеры (1.4).

### 1.9 Известные проблемы

- **USB2-хаб вместо USB3** → неполные кадры у Daheng при одновременной работе двух камер.
- **Тепловизор «подвисает»** при частых открытиях/закрытиях устройства (рестарты сервиса
  внахлёст, force-kill). Симптом: `Thermal: openStream не удался` при исправном железе.
  Лечится физическим переподключением USB-кабеля; программная альтернатива без физического
  доступа — «мягкий replug» через unbind/bind именно этого устройства (не всего хаба):
  ```bash
  # bus-id узнать: readlink -f /sys/class/video4linux/video0/device
  echo -n "<bus-id>" | sudo tee /sys/bus/usb/drivers/usb/unbind
  sleep 2
  echo -n "<bus-id>" | sudo tee /sys/bus/usb/drivers/usb/bind
  ```
  Помогает не всегда — если не сработало, нужен физический replug.
- **Одно устройство — один процесс.** Не запускайте два `camera_service`/пробника на один
  и тот же `/dev/videoN` одновременно — конфликт за устройство может его подвесить.
- **`ssh host "cmd & disown"` иногда «висит»** — это локальная особенность наследования
  stdin у `ssh`, не проблема Jetson; лечится флагом `ssh -n`.

---

## Часть 2. Клиент (ПК)

`client/Makefile` — обёртка над `tools/camctl_client.py` (CLI, только TCP+JSON, без тяжёлых
зависимостей) и `tools/tune_viewer.py` (GUI: видео + оверлей + панель параметров + хоткеи).
Переопределяемые переменные: `HOST RTSP CTRL IFACE MYIP SEC NAME VALUE`.

### 2.1 Сеть до Jetson

Если ПК и Jetson в одной обычной сети (Wi-Fi/LAN/роутер) — просто передавайте `HOST=<ip>`,
никакой доп. настройки не требуется.

Если это прямой Ethernet-кабель без DHCP (сценарий по умолчанию, `HOST=192.168.1.100`,
`IFACE=enp4s0`, `MYIP=192.168.1.10`):
```bash
make -C client net              # sudo ip link up + статический адрес на IFACE
make -C client ping             # проверка rtt
```
Подвох прямого кабеля: если `enp4s0` теряет carrier (кабель выдернут/NetworkManager сбросил
адрес), маршрут в подсеть Jetson может перехватить VPN/другой интерфейс — `ping` при этом
может «отвечать» с испорченным маршрутом, а TCP-соединение сразу рвётся. Проверка:
`cat /sys/class/net/enp4s0/carrier` и `ip route get <IP-Jetson>` (должно быть `dev enp4s0`).

### 2.2 Просмотр

| Команда | Что делает |
|---|---|
| `make -C client ffplay HOST=... [RTSP=...]` | минимальный просмотр, без доп. Python-зависимостей |
| `make -C client view HOST=... [RTSP=...] [CTRL=...]` | полный вьюер: видео + оверлей FPS/задержки + панель параметров справа |
| `make -C client record SEC=5 HOST=... [RTSP=...]` | запись потока в `out.h264` |
| `make -C client snapshot HOST=... [RTSP=...]` | один кадр в `snapshot.png` |

Три камеры одновременно — просто три отдельных вызова `ffplay`/`view` с разными `RTSP`/`CTRL`
(своё окно на каждую; общего мультикамерного окна для сетевого клиента сейчас нет).

**Панель параметров** (`view`) строится автоматически по `get_params` — под каждый тип
виджета: числовой параметр с диапазоном → слайдер, без диапазона → спинбокс, `bool` →
чекбокс, `enum` (например, `palette`) → выпадающий список. Показывает ровно то, что реально
поддерживает конкретная камера, без хардкода — новую ручку на сервере подхватит сама.

**Хоткеи** (фокус на видео): `↑`/`↓` битрейт ±1 Мбит, `1`/`2`/`3`/`4` разрешение
480/640/960/1280, `d` децимация 1↔2, `e` авто-экспозиция вкл/выкл, `q`/`Esc` выход. Работают
параллельно с панелью (форвардятся на видео-виджет, даже если фокус на слайдере/списке).

### 2.3 Управление из командной строки

```bash
make -C client params HOST=... CTRL=...                    # все ручки с диапазонами/опциями
make -C client stats  HOST=... CTRL=...                    # телеметрия (fps/разрешение/битрейт/темп.)
make -C client get    NAME=bitrate      HOST=... CTRL=...
make -C client set    NAME=bitrate VALUE=8000000  HOST=... CTRL=...
make -C client set    NAME=palette VALUE=ironbow  HOST=... CTRL=...   # только для thermal
make -C client presets     HOST=... CTRL=...                # список пресетов
make -C client preset      NAME=indoor  HOST=... CTRL=...   # применить
make -C client save-preset NAME=mine    HOST=... CTRL=...   # сохранить текущие значения
make -C client time        HOST=... CTRL=...                # смещение часов + RTT
```

### 2.4 Диагностика

| Команда | Что делает |
|---|---|
| `make -C client check` / `health` | сквозная проверка: control-канал + синхронизация часов + декод потока (кадры/FPS/задержка) |
| `make -C client latency` | только задержка/FPS по потоку |

### 2.5 Зависимости клиента

- `ffmpeg`/`ffplay` — системный пакет, нужен только для `ffplay`/`record`/`snapshot`.
- `PySide6` + `av` (PyAV) — только для `view`/`health`/`latency` (`pip install PySide6 av`).
  Без них по-прежнему работают `params`/`get`/`set`/`stats`/`presets`/`ffplay` — чистый
  TCP+JSON и системный ffplay, без Python-зависимостей вообще.

## Связанные документы

- Архитектура: [../02-architecture/overview.md](../02-architecture/overview.md),
  [components.md](../02-architecture/components.md)
- Железо и драйверы: [../04-hardware/](../04-hardware/)
- ADR: [../02-architecture/decisions/](../02-architecture/decisions/)
- Эксплуатационные проблемы прототипа (устарело): [runbook.md](runbook.md)
