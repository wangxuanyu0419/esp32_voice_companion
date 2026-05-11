# CMakeLists.txt for lvgl__lvgl (v8.4.0)
# Minimal IDF component wrapper for LVGL v8

cmake_minimum_required(VERSION 3.16)

# Collect all lvgl source files recursively
file(GLOB_RECURSE LVGL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

# Exclude demo and example files to reduce code size
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/demos/.*")
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/examples/.*")
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/tests/.*")

idf_component_register(
    SRCS            ${LVGL_SRCS}
    INCLUDE_DIRS    "."
                    "src"
)

# Suppress warnings from third-party library
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-implicit-fallthrough
    -Wno-format-overflow
    -DLV_CONF_INCLUDE_SIMPLE
)
