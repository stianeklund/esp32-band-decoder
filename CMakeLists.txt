# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)

# Ensure C++ is enabled
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The project builds with -Wall -Werror -Wextra. ESP-IDF v6.0 added new fields to
# several public structs (i2c_config_t.clk_flags, httpd_uri_t.is_websocket/
# handle_ws_control_frames/supported_subprotocol, uart_config_t.flags/
# rx_flow_ctrl_thresh), so our partial designated-initializers now trip
# -Wmissing-field-initializers (part of -Wextra) and fail the build. Demote it
# back to a non-fatal warning project-wide. Must be set before project().
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

project(rs232_band_decoder)
