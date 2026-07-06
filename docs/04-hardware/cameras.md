# Камеры: сводка

> Статус: в работе
> Обновлён: 2026-07-02
> Автор:

Перечень камер, с которыми работает сервис, и их базовые характеристики. Подробности
по каждой — в отдельных файлах.

| Модель | Тип | Разрешение | Bayer | VID:PID | SDK | Статус | Подробности |
|--------|-----|-----------|-------|---------|-----|--------|-------------|
| Daheng ME2P-2621-15U3C | RGB / промышленная | 5120×5120 (26 Мп) | GB8 | 2ba2:4d55 | gxipy (Galaxy) | работает | [daheng-me2p.md](daheng-me2p.md) |
| Daheng NS-301UCL | RGB / промышленная | ~0.3 Мп | RG8 | 2ba2:4d55 | gxipy (Galaxy) | работает | [daheng-me2p.md](daheng-me2p.md) |
| PLUG617R | Тепловизор (радиометрия) | 640×512 | — | 04b4:f9f9 | GuideUSBCamera (USB3.0 SC-SDK) | работает | [thermal-usb3-scsdk.md](thermal-usb3-scsdk.md) |
| _(камера глубины — если есть)_ | Depth | ? | ? | ? | ? | планируется | — |

## Общие заметки

- Все известные устройства — по USB 3.0.
- **Разные модели Daheng требуют РАЗНЫЙ Bayer-формат** (ME2P → `GB8`, NS-301 → `RG8`).
  `viewer_qt.py` подбирает поддерживаемый 8-битный Bayer автоматически, не хардкодит.
- **Полоса USB.** ME2P на 26 Мп тяжёлая по шине; несколько камер на одной шине упираются
  в полосу → неполные кадры. Ограничиваем `DeviceLinkThroughputLimit` и FPS (см.
  [daheng-me2p.md](daheng-me2p.md)).
- Обе Daheng имеют одинаковый VID:PID `2ba2:4d55` — различаем по серийнику/модели.
- Развёртывание на **Jetson (aarch64)** — см. [../09-operations/deployment.md](../09-operations/deployment.md).
- Настройка прав доступа и драйверов — в [drivers-setup.md](drivers-setup.md).
- Форматы кадров — в [../03-api/frame-formats.md](../03-api/frame-formats.md).

## Связанные документы

- Настройка драйверов: [drivers-setup.md](drivers-setup.md)
- Глоссарий: [../00-overview/glossary.md](../00-overview/glossary.md)
