cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.3")

add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(itoy-ota-0.1.3)

# 单文件整片镜像: ninja itoy-all → build/itoy-all.bin
# (bootloader+分区表+otadata+应用, 从 0x0 一次烧录; 亦作 QEMU/Wokwi 仿真镜像。不含 assets 分区)
add_custom_target(itoy-all
    COMMAND ${CMAKE_COMMAND} -E rename
        "${CMAKE_BINARY_DIR}/merged-binary.bin" "${CMAKE_BINARY_DIR}/itoy-all.bin"
    DEPENDS merge-bin
    VERBATIM)
