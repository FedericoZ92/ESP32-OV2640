# Minimum CMake version
cmake_minimum_required(VERSION 3.16)

# Include the ESP-IDF project boilerplate
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the project name
project(ov2640)

set(EXTRA_COMPONENT_DIRS
    managed_components/espressif__esp-tflite-micro
    managed_components/espressif__esp_jpeg
    managed_components/espressif__esp-nn
    managed_components/espressif__esp32-camera
)
