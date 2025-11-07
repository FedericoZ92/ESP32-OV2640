# Minimum CMake version
cmake_minimum_required(VERSION 3.16)

# Include the ESP-IDF project boilerplate
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the project name
project(ov2640)

# Include directories for global visibility (optional)
#include_directories(components/esp32-camera-f)
