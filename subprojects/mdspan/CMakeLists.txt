cmake_minimum_required(VERSION 3.24)
project(mdspan VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS ON)

# mdspan is a header-only library
add_library(mdspan INTERFACE)
add_library(mdspan::mdspan ALIAS mdspan)

target_include_directories(mdspan INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
