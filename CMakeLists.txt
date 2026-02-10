# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

#KD-Common OTA
IF(DEFINED ENV{BUILD_VERSION})
set(PROJECT_VER $ENV{BUILD_VERSION})
ELSE()
set(PROJECT_VER "0.0.1")
ENDIF()


IF(DEFINED ENV{BUILD_VARIANT})
    set(BUILD_VARIANT $ENV{BUILD_VARIANT})
ELSE()
    set(BUILD_VARIANT "devel")
ENDIF()

message("Building version ${PROJECT_VER} for variant ${BUILD_VARIANT}")
add_compile_definitions(FIRMWARE_VARIANT="${BUILD_VARIANT}")
add_compile_definitions(ENABLE_OTA) #comment this line to disable OTA updates
#End KD-Common OTA

set(EXTRA_COMPONENT_DIRS ${CMAKE_SOURCE_DIR}/esp-hub75/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(matrx-fw)