# Minimum CMake version
cmake_minimum_required(VERSION 3.16)

# Include the ESP-IDF project boilerplate
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the project name
project(ov2640)

set(EXTRA_COMPONENT_DIRS
    managed_components/espressif__esp-tflite-micro
    components/esp32-camera-f
)
