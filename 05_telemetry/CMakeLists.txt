cmake_minimum_required(VERSION 3.13)

# Pull in the unmodified Pico SDK import file you provided
include(pico_sdk_import.cmake)

project(mtmws_telemetry C CXX ASM)

# The ComputerCard environment generally prefers C++17
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# Define your main executable
add_executable(telemetry_module
    telemetry_module.cpp
    # Note: If you decided to write custom USB descriptors from scratch, 
    # you would add usb_descriptors.c to this list.
)

# Tell CMake where to look for your header files (.h)
# This assumes ComputerCard.h and tusb_config.h are in the same folder as this file.
target_include_directories(telemetry_module PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link the required Pico SDK libraries
target_link_libraries(telemetry_module
    pico_stdlib
    pico_multicore      
    hardware_timer      
    hardware_pwm        
    hardware_adc        
    hardware_dma        
    hardware_i2c
    hardware_spi
)

# --- USB Configuration ---
# This tells the SDK to generate the default CDC descriptors for you.
# Even with this enabled, because we linked `tinyusb_device` above, 
# you can still manually use `tud_cdc_write()` in your Core 1 thread.
pico_enable_stdio_usb(telemetry_module 1)

# Disable standard UART (Serial over GPIO pins) to save overhead, 
# since we are exclusively using USB for telemetry.
pico_enable_stdio_uart(telemetry_module 0)

# Generate the .uf2 file that you will drag-and-drop onto the RP2040
pico_add_extra_outputs(telemetry_module)