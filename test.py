# Пример скрипта для захвата изображения с камеры Daheng ME2P-2621-15U3C в Python

import gxipy as gx
import cv2
import numpy as np
#from PIL import Image


def capture_image():
    # Инициализация класса менеджера 
    device_manager = gx.DeviceManager()
    # перечисление всех камер 
    dev_num, dev_info_list =  device_manager.update_all_device_list()
    if dev_num == 0:
        raise Exception("No camera found")
    # открываем камеру 
    str_index = dev_info_list[0].get("index")
    cam = device_manager.open_device_by_index(str_index)
    
    # Register callback
    #cam.data_stream[0].register_capture_callback(capture_callback)
    # Запуск потока захвата изображений 
    
    # Установка параметров камеры
    
    cam.PixelFormat.set(gx.GxPixelFormatEntry.BAYER_RG8)
    cam.GainSelector.set(gx.GxGainSelectorEntry.ALL)
    cam.GammaMode.set(gx.GxGammaModeEntry.SRGB)
    cam.GammaEnable.set(True)
    cam.ColorTransformationMode.set(gx.GxColorTransformationModeEntry.RGB_TO_RGB)
    cam.ColorTransformationEnable.set(True)
    cam.LightSourcePreset.set(gx.GxLightSourcePresetEntry.DAYLIGHT_5000K)
    cam.BalanceWhiteAuto.set(gx.GxAutoEntry.CONTINUOUS)
    
    
    cam.stream_on()
    
    mode="RGB"
    valid_bits = gx.DxValidBit.BIT0_7
    convert_type=gx.DxBayerConvertType.NEIGHBOUR 
    channel_order=gx.DxRGBChannelOrder.ORDER_BGR

    try:
        while True:
            # Open the data stream of channel 0
            raw_image = cam.data_stream[0].get_image()
            
            # получаем RGB изображение 
            rgb_image = raw_image.convert(mode=mode, valid_bits = valid_bits, convert_type=convert_type, channel_order=channel_order)
            if rgb_image is None:
                continue
            
            #rgb_image.image_improvement(up_param[0], up_param[1], up_param[2])
            # переводим в массив numpy
            numpy_image = rgb_image.get_numpy_array()

            # Масштабирование до 1000x1000
            resized = cv2.resize(numpy_image, (750, 750), interpolation=cv2.INTER_AREA)

            # Вырезание центральной части 1000x1000
            center_x, center_y = numpy_image.shape[1] // 2, numpy_image.shape[0] // 2
            half_width, half_height = 375, 375
            cropped = numpy_image[center_y - half_height:center_y + half_height, center_x - half_width:center_x + half_width]

            # Отображение изображений
            cv2.imshow('Resized Image', resized)
            cv2.imshow('Cropped Center Image', cropped)

            key = cv2.waitKey(1)
            if key == ord('q'):
                break
    
    finally:
        # Завершение работы с камерой
        cam.stream_off()
        cam.close_device()

if __name__ == "__main__":
    try:
        capture_image()
        print("Image captured successfully.")
    except Exception as e:
        print(f"An error occurred: {e}")
