# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13...3.27)

set(PICO_BOARD pico_w CACHE STRING "Target board")

math(EXPR PICO_ARC_FLASH_TOTAL_BYTES_VALUE "16 * 1024 * 1024")
math(EXPR PICO_ARC_FLASH_STORAGE_BYTES_VALUE "1 * 1024 * 1024")
math(EXPR PICO_ARC_FLASH_FIRMWARE_BYTES_VALUE
    "${PICO_ARC_FLASH_TOTAL_BYTES_VALUE} - ${PICO_ARC_FLASH_STORAGE_BYTES_VALUE}")

set(PICO_FLASH_SIZE_BYTES "${PICO_ARC_FLASH_TOTAL_BYTES_VALUE}" CACHE STRING
    "External flash size on the target board")

include(pico_sdk_import.cmake)

project(pico_arc452a21 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PICO_ARC_IR_GPIO 0 CACHE STRING "GPIO used for the IR LED")
set(PICO_ARC_I2C_INSTANCE 0 CACHE STRING "I2C instance number: 0 or 1")
set(PICO_ARC_I2C_SDA_PIN 20 CACHE STRING "BMP280/BME280 I2C SDA GPIO")
set(PICO_ARC_I2C_SCL_PIN 21 CACHE STRING "BMP280/BME280 I2C SCL GPIO")
set(PICO_ARC_I2C_BAUD 100000 CACHE STRING "BMP280/BME280 I2C baud rate")
option(PICO_ARC_ENABLE_SENSOR "Enable BMP280/BME280 sensor startup and polling" ON)
option(PICO_ARC_ENABLE_IOT "Enable Pico W Wi-Fi/AP and HTTP server" ON)

set(PICO_ARC_SOURCES
    src/app_state.c
    src/bmp280.c
    src/daikin_frame.c
    src/daikin_ir.c
    src/main.c
)

if(PICO_ARC_ENABLE_IOT)
    list(APPEND PICO_ARC_SOURCES src/iot_server.c)
endif()

add_executable(pico_arc452a21
    ${PICO_ARC_SOURCES}
)

target_include_directories(pico_arc452a21 PRIVATE
    include
)

target_compile_definitions(pico_arc452a21 PRIVATE
    PICO_ARC_IR_GPIO=${PICO_ARC_IR_GPIO}
    PICO_ARC_I2C_INSTANCE=${PICO_ARC_I2C_INSTANCE}
    PICO_ARC_I2C_SDA_PIN=${PICO_ARC_I2C_SDA_PIN}
    PICO_ARC_I2C_SCL_PIN=${PICO_ARC_I2C_SCL_PIN}
    PICO_ARC_I2C_BAUD=${PICO_ARC_I2C_BAUD}
    PICO_ARC_ENABLE_SENSOR=$<BOOL:${PICO_ARC_ENABLE_SENSOR}>
    PICO_ARC_ENABLE_IOT=$<BOOL:${PICO_ARC_ENABLE_IOT}>
    PICO_ARC_FLASH_TOTAL_BYTES=${PICO_ARC_FLASH_TOTAL_BYTES_VALUE}
    PICO_ARC_FLASH_FIRMWARE_BYTES=${PICO_ARC_FLASH_FIRMWARE_BYTES_VALUE}
    PICO_ARC_FLASH_STORAGE_BYTES=${PICO_ARC_FLASH_STORAGE_BYTES_VALUE}
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
)

target_link_libraries(pico_arc452a21 PRIVATE
    pico_stdlib
    hardware_flash
    hardware_i2c
    hardware_pwm
    hardware_sync
)

if(PICO_ARC_ENABLE_IOT)
    target_link_libraries(pico_arc452a21 PRIVATE
        pico_cyw43_arch_lwip_poll
    )
endif()

pico_enable_stdio_usb(pico_arc452a21 1)
pico_enable_stdio_uart(pico_arc452a21 0)
pico_add_extra_outputs(pico_arc452a21)

add_custom_command(TARGET pico_arc452a21 POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D BIN_PATH=$<TARGET_FILE_DIR:pico_arc452a21>/pico_arc452a21.bin
        -D FIRMWARE_LIMIT=${PICO_ARC_FLASH_FIRMWARE_BYTES_VALUE}
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_flash_layout.cmake
    VERBATIM
)
