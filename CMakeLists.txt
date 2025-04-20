# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

IF(DEFINED ENV{BUILD_VERSION})
set(PROJECT_VER $ENV{BUILD_VERSION})
message("Building production project with version $ENV{BUILD_VERSION}")
add_compile_definitions(ENABLE_OTA)
add_compile_definitions(FIRMWARE_VARIANT="$ENV{BUILD_VARIANT}")
ELSE()
message("Building development project")
set(PROJECT_VER "0.0.1")
#add_compile_definitions(FIRMWARE_VARIANT=development)

add_compile_definitions(ENABLE_OTA)
add_compile_definitions(FIRMWARE_VARIANT="matrx_v9_64x64")
ENDIF()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS esptool_py main esp_lcd)
project(MATRX-fw)


# Create a LittleFS image from the contents of the 'fs_image' directory
# that fits the partition named 'littlefs'. The generated image should be flashed
# when the entire project is flashed to the target with 'idf.py -p PORT flash'.
littlefs_create_partition_image(fs fs_image FLASH_IN_PROJECT)