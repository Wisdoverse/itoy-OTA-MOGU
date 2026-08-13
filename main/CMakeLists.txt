# itoy-OTA minimal build - only itoy board + OTA functionality

# Define source files
set(SOURCES "system_info.cc"
            "ota.cc"
            "settings.cc"
            "device_state_event.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "boards/itoy-mogu")

# Add board common files
# 注: ESP-IDF 不允许 file(GLOB ... CONFIGURE_DEPENDS), 所以新增 .cc 后需触发一次
# reconfigure (改本文件 / idf.py reconfigure) 才会被 glob 收录。
file(GLOB BOARD_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/boards/common/*.cc)
list(APPEND SOURCES ${BOARD_COMMON_SOURCES})
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/boards/common)

# Board type - only itoy-mogu
set(BOARD_TYPE "itoy-mogu")

file(GLOB BOARD_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_TYPE}/*.c
)
list(APPEND SOURCES ${BOARD_SOURCES})

idf_component_register(SRCS ${SOURCES}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    REQUIRES driver app_update spi_flash nvs_flash efuse esp_app_format esp_adc led_strip wifi_provisioning esp_http_server esp_wifi esp_netif mdns
                    WHOLE_ARCHIVE
                    )

# Define BOARD_TYPE and BOARD_NAME
if(NOT BOARD_NAME)
    set(BOARD_NAME ${BOARD_TYPE})
endif()
target_compile_definitions(${COMPONENT_LIB}
                    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                    )
