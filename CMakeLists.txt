# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MATRX-fw)

# Create a LittleFS image from the contents of the 'fs_image' directory
# that fits the partition named 'littlefs'. The generated image should be flashed
# when the entire project is flashed to the target with 'idf.py -p PORT flash'.
littlefs_create_partition_image(fs fs_image FLASH_IN_PROJECT)