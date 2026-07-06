cmake_minimum_required(VERSION 3.16)
project(camctl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -O2)

add_executable(camctl
  src/app/main.cpp
  src/camera/daheng_camera.cpp
  src/core/debayer.cpp
)

target_include_directories(camctl PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party/galaxy/include
)

# libgxiapi.so ставится в /usr/lib установщиком Galaxy SDK (нужной архитектуры).
target_link_libraries(camctl gxiapi pthread)

# --- Кодер NVENC + стриминг: только на Jetson, где есть jetson_multimedia_api ---
set(MMAPI /usr/src/jetson_multimedia_api)
if(EXISTS ${MMAPI})
  file(GLOB MMAPI_CLASSES ${MMAPI}/samples/common/classes/*.cpp)

  # Общие настройки для целей, использующих NVENC.
  function(camctl_nvenc_target tgt)
    target_include_directories(${tgt} PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/third_party/galaxy/include
      ${MMAPI}/include
      ${MMAPI}/include/libjpeg-8b
      /usr/local/cuda/include
      /usr/include/aarch64-linux-gnu
      /usr/include/libdrm
    )
    target_link_directories(${tgt} PRIVATE
      /usr/lib/aarch64-linux-gnu/tegra
      /usr/lib/aarch64-linux-gnu/nvidia
      /usr/local/cuda/lib64
    )
    target_link_libraries(${tgt}
      pthread nvv4l2 nvbufsurface nvbufsurftransform
      EGL GLESv2 X11 nvjpeg nvosd drm cuda cudart
    )
  endfunction()

  # Автономный тест кодера (синтетика -> H.264-файл).
  add_executable(enc_test
    src/app/enc_test.cpp
    src/encode/h264_encoder.cpp
    ${MMAPI_CLASSES}
  )
  camctl_nvenc_target(enc_test)

  # Сквозной поток: камера -> дебайер -> I420 -> NVENC -> RTP/RTSP.
  add_executable(stream_test
    src/app/stream_test.cpp
    src/camera/daheng_camera.cpp
    src/core/debayer.cpp
    src/core/rgb_to_i420.cpp
    src/encode/h264_encoder.cpp
    src/stream/rtp_sender.cpp
    src/stream/rtsp_server.cpp
    ${MMAPI_CLASSES}
  )
  camctl_nvenc_target(stream_test)
  target_link_libraries(stream_test gxiapi)

  # Полный сервис камеры: поток + управляющий канал TCP/JSON (живой тюнинг).
  add_executable(camera_service
    src/app/camera_service.cpp
    src/camera/daheng_camera.cpp
    src/core/debayer.cpp
    src/core/rgb_to_i420.cpp
    src/encode/h264_encoder.cpp
    src/stream/rtp_sender.cpp
    src/stream/rtsp_server.cpp
    src/control/json.cpp
    src/control/param_registry.cpp
    src/control/control_server.cpp
    ${MMAPI_CLASSES}
  )
  camctl_nvenc_target(camera_service)
  target_link_libraries(camera_service gxiapi)

  message(STATUS "Jetson MMAPI найден — собираю enc_test + stream_test + camera_service (NVENC)")
else()
  message(STATUS "jetson_multimedia_api не найден — enc_test/stream_test пропущены")
endif()
