# Захват и отображение со всех Daheng-камер проекта (NS-301UCL) через gxipy / SDK Galaxy.
# Каждой камере — своё окно с подписью по серийному номеру. Выход — клавиша 'q'.
#
# ВНИМАНИЕ: использует cv2.imshow и требует ПОЛНЫЙ opencv-python. В проекте сейчас стоит
# opencv-python-headless (ради viewer_qt.py — иначе Qt из opencv конфликтует с PySide6),
# поэтому imshow тут не сработает. Замена этому скрипту: `viewer_qt.py --daheng-only`.
#
# Тепловизор PLUG617R здесь НЕ обрабатывается: у него свой SDK (GuideUSBCamera).
# Запуск тепловизора — отдельной командой:
#   cd "driwers/Linux_USB3.0_V2_0_0-x86_64-linux-gnu-gcc-13_3_0_20250822"
#   ./scripts/run_probe.sh --device /dev/video2 --mode 5 --version 1 --width 640 --height 512 \
#       --frames 1000000 --timeout-sec 0 --preview-format yuyv-luma --preview-palette blue-red --preview-fps 25
# (mode 5 отдаёт картинку + матрицу температур + param-строку с точками; температура = значение/10 °C)

import gxipy as gx
import cv2


def fit(img, target):
    """Масштабирует изображение под экран, сохраняя пропорции (длинная сторона -> target px)."""
    h, w = img.shape[:2]
    scale = target / max(h, w)
    return cv2.resize(img, (int(w * scale), int(h * scale)), interpolation=cv2.INTER_AREA)


def setup_daheng(cam):
    """Настройка Daheng-камеры. PixelFormat обязателен; остальные фичи опциональны
    (на другой модели часть может отсутствовать — тогда просто пропускаем)."""
    cam.PixelFormat.set(gx.GxPixelFormatEntry.BAYER_RG8)

    def try_set(setter):
        try:
            setter()
        except Exception:
            pass

    try_set(lambda: cam.GainSelector.set(gx.GxGainSelectorEntry.ALL))
    try_set(lambda: cam.GammaMode.set(gx.GxGammaModeEntry.SRGB))
    try_set(lambda: cam.GammaEnable.set(True))
    try_set(lambda: cam.ColorTransformationMode.set(gx.GxColorTransformationModeEntry.RGB_TO_RGB))
    try_set(lambda: cam.ColorTransformationEnable.set(True))
    try_set(lambda: cam.LightSourcePreset.set(gx.GxLightSourcePresetEntry.DAYLIGHT_5000K))
    try_set(lambda: cam.BalanceWhiteAuto.set(gx.GxAutoEntry.CONTINUOUS))


def main():
    dm = gx.DeviceManager()
    dev_num, dev_info_list = dm.update_all_device_list()
    if dev_num == 0:
        raise Exception("No camera found")

    print(f"Найдено Daheng-камер: {dev_num}")

    cams = []
    for dev in dev_info_list:
        cam = dm.open_device_by_index(dev.get("index"))
        setup_daheng(cam)
        cam.stream_on()
        cams.append((f"Daheng {dev.get('sn')}", cam))
        print(f"  открыта: {dev.get('sn')}")

    convert_kwargs = dict(
        mode="RGB",
        valid_bits=gx.DxValidBit.BIT0_7,
        convert_type=gx.DxBayerConvertType.NEIGHBOUR,
        channel_order=gx.DxRGBChannelOrder.ORDER_BGR,  # BGR — для OpenCV
    )

    try:
        while True:
            for title, cam in cams:
                raw_image = cam.data_stream[0].get_image(timeout=1000)
                if raw_image is None:
                    continue
                rgb_image = raw_image.convert(**convert_kwargs)
                if rgb_image is None:
                    continue
                numpy_image = rgb_image.get_numpy_array()
                if numpy_image is None:
                    continue
                cv2.imshow(title, fit(numpy_image, 720))

            if cv2.waitKey(1) == ord("q"):
                break
    finally:
        for _, cam in cams:
            cam.stream_off()
            cam.close_device()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    try:
        main()
        print("Done.")
    except Exception as e:
        print(f"An error occurred: {e}")
