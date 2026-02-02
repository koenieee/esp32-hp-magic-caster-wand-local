# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# Conditionally require esp_tinyusb for S2/S3/P4
if(CONFIG_IDF_TARGET_ESP32S2 OR CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32P4)
    set(USB_REQUIRES esp_tinyusb)
endif()

idf_component_register(SRCS ${app_sources}
    INCLUDE_DIRS "../include"
    REQUIRES nvs_flash bt esp_netif spiffs driver esp_driver_gpio esp_http_server mqtt ${USB_REQUIRES})
