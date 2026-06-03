cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(itoy-ota)
