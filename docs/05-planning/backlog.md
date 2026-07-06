# Бэклог задач

> Статус: в работе
> Обновлён: 2026-06-29
> Автор:

Рабочий список задач. Статусы: `todo` → `в работе` → `done` (или `отложено`).
Приоритет: `выс` / `сред` / `низ`. Привязывайте задачи к требованиям (`FR-NN`) и фазам.

| ID | Задача | Приоритет | Статус | Фаза | Связи / заметки |
|----|--------|-----------|--------|------|-----------------|
| T-001 | Запустить RGB-поток Daheng (по `Test.py`) | выс | done | 0 | [../04-hardware/daheng-me2p.md](../04-hardware/daheng-me2p.md) |
| T-002 | Запустить тепловой поток (plug617) | выс | done | 0 | [../04-hardware/thermal-usb3-scsdk.md](../04-hardware/thermal-usb3-scsdk.md) |
| T-003 | Настроить права на устройства (udev) | выс | done | 0 | [../04-hardware/drivers-setup.md](../04-hardware/drivers-setup.md) |
| T-004 | Выбрать язык/рантайм сервиса | выс | done | 1 | **C++** — [ADR-0003](../02-architecture/decisions/0003-service-language-cpp.md) |
| T-005 | Спроектировать общий интерфейс устройства | сред | done | 1 | воркеры в `viewer_qt.py`; [../02-architecture/components.md](../02-architecture/components.md) |
| T-006 | Выбрать транспорт API | сред | todo | 3 | видео-транспорт — [ADR-0002](../02-architecture/decisions/0002-streaming-transport.md); control plane — TBD; [../03-api/overview.md](../03-api/overview.md) |
| T-007 | Вещание видео: H.264 + RTSP/RTP (`ffmpeg`) | выс | todo | 3 | FR-41, FR-42, [ADR-0002](../02-architecture/decisions/0002-streaming-transport.md) |
| T-008 | JSON-пресеты камер (в здании / на улице / кастом) | сред | todo | 2 | FR-13 |
| T-009 | Авто-выбор Bayer-формата (RG8/GB8 по модели) | выс | done | 1 | `viewer_qt.py`; FR-11 |
| T-010 | Единый просмотрщик всех камер (Qt) | выс | done | 2 | `viewer_qt.py`, `guide_thermal.py` |
| T-011 | Развёртывание на Jetson (aarch64) | выс | done | — | [../09-operations/deployment.md](../09-operations/deployment.md) |
| T-012 | Лимит полосы USB для многокамерности | сред | done | 2 | `DeviceLinkThroughputLimit`, FPS-cap |

## Идеи / на потом

_Сюда складывайте мысли, которые ещё не оформились в задачу._

- ...
