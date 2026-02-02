cmake_minimum_required(VERSION 3.16.0)

# Tell ESP-IDF to use 'src' instead of 'main'
set(EXTRA_COMPONENT_DIRS src)

# Use custom partition table with SPIFFS
set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-wand-gateway)
