cmake_minimum_required(VERSION 3.22)
project(minios_opus C CXX)
include(FetchContent)
FetchContent_Declare(opus_source
 URL https://codeload.github.com/sh123/esp32_opus_arduino/tar.gz/a3816682932b8792f90072ee05c33fe25c055628
 URL_HASH SHA256=9a1b40e2d37f4c48d1040d1afc5aa3d4cb5015a18ded1336fd98b2512184e215)
FetchContent_Populate(opus_source)
file(GLOB OPUS_SOURCES "${opus_source_SOURCE_DIR}/src/*.c")
list(FILTER OPUS_SOURCES EXCLUDE REGEX "opus_compare.c$")
add_library(minios_opus SHARED encoder.cpp ${OPUS_SOURCES})
target_include_directories(minios_opus PRIVATE "${opus_source_SOURCE_DIR}/src")
target_compile_options(minios_opus PRIVATE -O2)
target_link_libraries(minios_opus m)
