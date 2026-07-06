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
