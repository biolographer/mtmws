cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK import script
include(pico_sdk_import.cmake)

project(Atlas C CXX ASM)

# Initialize the Pico SDK
pico_sdk_init()

# Add the executable
add_executable(Atlas
    main.cpp
)

# Link the required Pico SDK libraries for the Workshop Station/ComputerCard
target_link_libraries(Atlas
    pico_stdlib
    hardware_adc
    hardware_clocks
    hardware_dma
    hardware_flash
    hardware_i2c
    hardware_irq
    hardware_spi
    hardware_pwm
    hardware_gpio
)

# Generate map/bin/hex/uf2 files
pico_add_extra_outputs(Atlas)