# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Specify the full path to the ESP32 toolchain compilers
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#include($ENV{IDF_PATH}/tools/cmake/utilities.cmake)
project(rs232_band_decoder)

# Ensure C++ is enabled
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
