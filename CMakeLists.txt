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

# --- Кодер NVENC: только на Jetson, где есть jetson_multimedia_api ---
set(MMAPI /usr/src/jetson_multimedia_api)
if(EXISTS ${MMAPI})
  file(GLOB MMAPI_CLASSES ${MMAPI}/samples/common/classes/*.cpp)
  add_executable(enc_test
    src/app/enc_test.cpp
    src/encode/h264_encoder.cpp
    ${MMAPI_CLASSES}
  )
  target_include_directories(enc_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${MMAPI}/include
    ${MMAPI}/include/libjpeg-8b
    /usr/local/cuda/include
    /usr/include/aarch64-linux-gnu
    /usr/include/libdrm
  )
  target_link_directories(enc_test PRIVATE
    /usr/lib/aarch64-linux-gnu/tegra
    /usr/lib/aarch64-linux-gnu/nvidia
    /usr/local/cuda/lib64
  )
  target_link_libraries(enc_test
    pthread nvv4l2 nvbufsurface nvbufsurftransform
    EGL GLESv2 X11 nvjpeg nvosd drm cuda cudart
  )
  message(STATUS "Jetson MMAPI найден — собираю enc_test (NVENC)")
else()
  message(STATUS "jetson_multimedia_api не найден — enc_test пропущен")
endif()
