# matrx_resources component CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# Set component name
set(COMPONENT_NAME "matrx_resources")

# Define the GitHub repository details
set(GITHUB_REPO "koiosdigital/MATRX-resources")
set(STATIC_FILES_HEADER "static_files.h")
set(DOWNLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/downloads")
set(INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
set(STATIC_FILES_PATH "${INCLUDE_DIR}/${STATIC_FILES_HEADER}")

# Create directories early - this is safe in early expansion
file(MAKE_DIRECTORY ${DOWNLOAD_DIR})
file(MAKE_DIRECTORY ${INCLUDE_DIR})

# Function to get the latest release tag
function(get_latest_release_tag REPO_NAME OUTPUT_VAR)
    if(DEFINED ENV{GITHUB_TOKEN})
        execute_process(
            COMMAND curl -s -H "Authorization: token $ENV{GITHUB_TOKEN}" "https://api.github.com/repos/${REPO_NAME}/releases/latest"
            COMMAND grep "tag_name"
            COMMAND head -n 1
            COMMAND cut -d ":" -f 2
            COMMAND sed "s/[\", ]//g"
            OUTPUT_VARIABLE LATEST_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE CURL_RESULT
        )
    else()
        execute_process(
            COMMAND curl -s "https://api.github.com/repos/${REPO_NAME}/releases/latest"
            COMMAND grep "tag_name"
            COMMAND head -n 1
            COMMAND cut -d ":" -f 2
            COMMAND sed "s/[\", ]//g"
            OUTPUT_VARIABLE LATEST_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE CURL_RESULT
        )
    endif()
    
    if(CURL_RESULT EQUAL 0 AND NOT "${LATEST_TAG}" STREQUAL "")
        set(${OUTPUT_VAR} ${LATEST_TAG} PARENT_SCOPE)
        message(STATUS "Latest release tag for ${REPO_NAME}: ${LATEST_TAG}")
    else()
        set(${OUTPUT_VAR} "main" PARENT_SCOPE)
        message(WARNING "Failed to get latest release tag for ${REPO_NAME}, using 'main' branch")
    endif()
endfunction()

# Fallback content for static_files.h
set(FALLBACK_CONTENT "#pragma once
// Fallback static_files.h when download fails
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Structure to hold image information
typedef struct {
    const char* name;
    const uint8_t* data;
    size_t size;
} image_info_t;

// Empty images array as fallback
static const image_info_t available_images[] = {};
static const size_t num_available_images = 0;

// Fallback functions
static inline int get_image_data(const char* name, const uint8_t** data, size_t* size) {
    if (data) *data = NULL;
    if (size) *size = 0;
    return 0;
}

static inline void get_image_dimensions(int* width, int* height) {
    if (width) *width = 64;
    if (height) *height = 32;
}

static inline size_t list_available_images(const char** names, size_t max_names) {
    return 0;
}
")

# Always create a basic fallback file first - this is safe in early expansion
if(NOT EXISTS ${STATIC_FILES_PATH})
    file(WRITE ${STATIC_FILES_PATH} "${FALLBACK_CONTENT}")
    message(STATUS "Created initial fallback static_files.h")
endif()

# Register the component early with basic configuration
# ESP-IDF needs this to happen before the expansion boundary
idf_component_register(
    INCLUDE_DIRS ${INCLUDE_DIR}
    REQUIRES esp_http_server
)

# ESP-IDF early expansion boundary - everything after this only runs during actual build
if(NOT CMAKE_BUILD_EARLY_EXPANSION)

    message(STATUS "Processing matrx_resources component download logic...")

    # Get matrix dimensions from kconfig
    # These variables are available during build configuration
    if(DEFINED CONFIG_MATRIX_WIDTH AND DEFINED CONFIG_MATRIX_HEIGHT)
        set(MATRIX_WIDTH ${CONFIG_MATRIX_WIDTH})
        set(MATRIX_HEIGHT ${CONFIG_MATRIX_HEIGHT})
        message(STATUS "Matrix dimensions from config: ${MATRIX_WIDTH}x${MATRIX_HEIGHT}")
    else()
        # Fallback to default values if kconfig not available
        set(MATRIX_WIDTH 64)
        set(MATRIX_HEIGHT 32)
        message(WARNING "Matrix dimensions not found in config, using defaults: ${MATRIX_WIDTH}x${MATRIX_HEIGHT}")
    endif()

    # Determine the source filename based on matrix dimensions
    set(SOURCE_FILENAME "${MATRIX_WIDTH}x${MATRIX_HEIGHT}_static.h")

    # Get the latest release tag
    get_latest_release_tag(${GITHUB_REPO} LATEST_TAG)

    # Download URL for the dimension-specific static file
    if("${LATEST_TAG}" STREQUAL "main")
        # Use raw GitHub URL for main branch
        set(DOWNLOAD_URL "https://raw.githubusercontent.com/${GITHUB_REPO}/main/${SOURCE_FILENAME}")
    else()
        # Use release download URL
        set(DOWNLOAD_URL "https://github.com/${GITHUB_REPO}/releases/download/${LATEST_TAG}/${SOURCE_FILENAME}")
    endif()

    # Try to download the file during actual build configuration
    execute_process(
        COMMAND curl -L -f -o ${STATIC_FILES_PATH} ${DOWNLOAD_URL}
        RESULT_VARIABLE DOWNLOAD_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )

    # Check if download was successful and file has content
    if(DOWNLOAD_RESULT EQUAL 0 AND EXISTS ${STATIC_FILES_PATH})
        file(SIZE ${STATIC_FILES_PATH} FILE_SIZE)
        if(FILE_SIZE GREATER 100)  # Reasonable minimum size check
            message(STATUS "Successfully downloaded ${SOURCE_FILENAME} from ${GITHUB_REPO} (${LATEST_TAG})")
            file(READ ${STATIC_FILES_PATH} FILE_CONTENT LIMIT 500)
        else()
            message(WARNING "Downloaded file is too small, using fallback")
            file(WRITE ${STATIC_FILES_PATH} "${FALLBACK_CONTENT}")
        endif()
    else()
        message(WARNING "Failed to download ${SOURCE_FILENAME} from ${DOWNLOAD_URL}, keeping fallback")
    endif()

    # Print detailed status information
    message(STATUS "matrx_resources component configured:")
    message(STATUS "  Repository: ${GITHUB_REPO}")
    message(STATUS "  Matrix dimensions: ${MATRIX_WIDTH}x${MATRIX_HEIGHT}")
    message(STATUS "  Source file: ${SOURCE_FILENAME}")
    message(STATUS "  Target file: ${STATIC_FILES_HEADER}")
    message(STATUS "  Download URL: ${DOWNLOAD_URL}")
    message(STATUS "  Local path: ${STATIC_FILES_PATH}")
    if(EXISTS ${STATIC_FILES_PATH})
        file(SIZE ${STATIC_FILES_PATH} FINAL_FILE_SIZE)
        message(STATUS "  File exists: YES (${FINAL_FILE_SIZE} bytes)")
    else()
        message(STATUS "  File exists: NO")
    endif()

endif() # CMAKE_BUILD_EARLY_EXPANSION