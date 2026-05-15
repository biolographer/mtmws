cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(mtmws_telemetry C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(telemetry_module
    telemetry_module.cpp
    usb_descriptors.c          # <-- NEW: required descriptors
)

target_include_directories(telemetry_module PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Critical: makes the board boot reliably after reset
target_compile_definitions(telemetry_module PRIVATE
    PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)

# Strongly recommended for ComputerCard: removes flash cache jitter
pico_set_binary_type(telemetry_module copy_to_ram)

target_link_libraries(telemetry_module
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_timer
    hardware_pwm
    hardware_adc
    hardware_dma
    hardware_i2c
    hardware_spi
    tinyusb_device          # <-- NEW: TinyUSB device stack
    tinyusb_board           # <-- NEW: board support
)

# *** CRITICAL CHANGE ***
# Turn OFF stdio_usb. It conflicts with manual TinyUSB CDC use,
# AND it interferes with the normalisation probe on GPIO4.
pico_enable_stdio_usb(telemetry_module 0)
pico_enable_stdio_uart(telemetry_module 0)

pico_add_extra_outputs(telemetry_module)