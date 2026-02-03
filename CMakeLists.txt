cmake_minimum_required(VERSION 3.16.0)

# Tell ESP-IDF to use 'src' instead of 'main'
set(EXTRA_COMPONENT_DIRS src)

# Use custom partition table with SPIFFS
# Use partitions-s3.csv for ESP32-S3 builds
if(IDF_TARGET STREQUAL "esp32s3")
    set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions-s3.csv")
else()
    set(PARTITION_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-wand-gateway)
