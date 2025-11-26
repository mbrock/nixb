cmake_minimum_required(VERSION 3.24)
project(nixb VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

set(CMAKE_LINKER_TYPE "GOLD")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

include(cmake/CPM.cmake)

cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED COMPONENTS stacktrace_basic stacktrace_addr2line stacktrace_from_exception)
find_package(PkgConfig REQUIRED)

pkg_check_modules(NIX REQUIRED IMPORTED_TARGET
  nix-store nix-expr nix-flake nix-cmd nix-util nlohmann_json)

# Nix's clang wrapper injects include paths via NIX_CFLAGS_COMPILE,
# which causes CMake to detect them as "implicit" includes
# and omit them from compile_commands.json.
#
# This breaks clangd because it doesn't use the Nix wrapper.
#
# Fix: DESTROY ALL IMPLICIT INCLUDES!!!!!!!!!
set(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "")
set(CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES "")

CPMAddPackage("gh:boost-ext/ut#v2.3.1")
CPMAddPackage("gh:simdjson/simdjson#v3.10.1")
CPMAddPackage("gh:CLIUtils/CLI11#v2.6.1")


CPMAddPackage(
    NAME mdspan
    GITHUB_REPOSITORY kokkos/mdspan
    GIT_TAG 537053f83150f1b6f7528c1d961866d8628abc2e
)

# CPMAddPackage("gh:c-ares/c-ares#v1.34.5")

CPMFindPackage(
    NAME fmt
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 3391f9e9929d6ea2562863902cb523af3e47f3f5
    SYSTEM YES
    EXCLUDE_FROM_ALL NO
    OPTIONS "FMT_INSTALL ON"
)

CPMFindPackage(
    NAME mp-units
    VERSION 2.4.0
    GIT_TAG 6e06eddf205deaf6c2f2f63500c8c70ec220a99f
    GITHUB_REPOSITORY mpusz/mp-units
    SOURCE_SUBDIR src
    SYSTEM YES
    OPTIONS
        "MP_UNITS_BUILD_INSTALL OFF"
        "MP_UNITS_API_CONTRACTS NONE"
        "MP_UNITS_API_STD_FORMAT OFF"
)

CPMAddPackage(
    NAME range-v3
    URL https://github.com/ericniebler/range-v3/archive/0.12.0.zip
    VERSION 0.12.0
    DOWNLOAD_ONLY True
)

if(range-v3_ADDED)
    add_library(range-v3 INTERFACE IMPORTED)
    target_include_directories(range-v3 INTERFACE "${range-v3_SOURCE_DIR}/include")
endif()

CPMAddPackage(
    NAME libcoro
    GITHUB_REPOSITORY jbaldwin/libcoro
    GIT_TAG v0.15.0
    OPTIONS
        "LIBCORO_FEATURE_NETWORKING ON"
        "LIBCORO_BUILD_TESTS OFF"
        "LIBCORO_EXTERNAL_DEPENDENCIES ON"
        "LIBCORO_BUILD_EXAMPLES OFF"
        "LIBCORO_BUILD_SHARED_LIBS ON"
)

add_subdirectory(subprojects/libvterm EXCLUDE_FROM_ALL)

foreach(target IN ITEMS simdjson CLI11 mp-units fmt libcoro)
    if(TARGET ${target})
        get_target_property(inc_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
        if(inc_dirs)
            set_target_properties(${target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${inc_dirs}"
            )
        endif()
    endif()
endforeach()

add_library(libnxt STATIC
    src/nxt/app.cpp
    src/nxt/ansi.cpp
    src/nxt/compositor.cpp
    src/nxt/glyph-table.cpp
    src/nxt/raster.cpp
    src/nxt/signal-pipe.cpp
)

target_include_directories(libnxt PUBLIC src)
target_link_libraries(libnxt
    PUBLIC
        libcoro
        fmt::fmt
        mdspan
        mp-units::mp-units
        Boost::headers
)
target_compile_definitions(libnxt PUBLIC
    MDSPAN_USE_BRACKET_OPERATOR=1
    MDSPAN_USE_PAREN_OPERATOR=0
)
add_executable(nxb
    src/nxb-main.cpp
    src/nix-log-adapter.cpp
    src/log-replay.cpp
)
target_link_libraries(nxb
    PRIVATE
        libnxt
        PkgConfig::NIX
        simdjson::simdjson
        CLI11::CLI11
        Boost::stacktrace_addr2line
        Boost::stacktrace_from_exception
        ${CMAKE_DL_LIBS}
)
target_compile_definitions(nxb PRIVATE
    BOOST_STACKTRACE_LINK
    BOOST_STACKTRACE_USE_ADDR2LINE
    BOOST_STACKTRACE_ADDR2LINE_LOCATION="/usr/bin/addr2line"
)
# target_compile_options(nxb PRIVATE -g -fno-omit-frame-pointer)
# target_link_options(nxb PRIVATE -rdynamic -no-pie)

# Test executables
add_executable(raster-test src/test/raster-test.cpp)
target_link_libraries(raster-test PRIVATE libnxt Boost::ut libcoro)

add_executable(terminal-test src/test/terminal-test.cpp)
target_link_libraries(terminal-test PRIVATE libnxt libvterm::libvterm Boost::ut)

enable_testing()
add_test(NAME raster-test COMMAND raster-test)
add_test(NAME terminal-test COMMAND terminal-test)


