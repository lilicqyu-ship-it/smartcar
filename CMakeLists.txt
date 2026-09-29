# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# SemVer shown on the About / Diagnostics pages (esp_app_get_description)
set(PROJECT_VER "0.1.0")

# Managed component espressif/esp_lcd_gc9503 (480x480 RGB panel option of the
# EV board) triggers a -Wuse-after-free under the IDF 6.1 GCC in a
# debug-log-after-free path; until the component updates, keep that diagnostic
# non-fatal.  Must be set before project.cmake so every component subdir
# inherits it.
add_compile_options(-Wno-error=use-after-free)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartcar_remote)