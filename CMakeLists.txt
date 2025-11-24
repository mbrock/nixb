cmake_minimum_required(VERSION 3.24)
project(nixb VERSION 0.3.0 LANGUAGES CXX)

# Build configuration
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# CPM Dependency Manager
# See: https://github.com/cpm-cmake/CPM.cmake
# Uses CPM_SOURCE_CACHE env var if set for faster rebuilds
include(cmake/CPM.cmake)

#------------------------------------------------------------------------------
# System Dependencies (from nix environment)
#------------------------------------------------------------------------------

cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED)
find_package(PkgConfig REQUIRED)

# Import nix libraries as PkgConfig targets
foreach(nixpkg IN ITEMS NIX_STORE NIX_EXPR NIX_FLAKE NIX_CMD)
    string(TOLOWER ${nixpkg} nixpkg_lower)
    string(REPLACE "_" "-" nixpkg_name ${nixpkg_lower})
    pkg_check_modules(${nixpkg} REQUIRED IMPORTED_TARGET ${nixpkg_name})
endforeach()

#------------------------------------------------------------------------------
# CPM Dependencies
#------------------------------------------------------------------------------

# Standard dependencies with straightforward configuration
CPMAddPackage("gh:simdjson/simdjson#v3.10.1")
CPMAddPackage("gh:CLIUtils/CLI11#v2.6.1")
CPMAddPackage("gh:NVIDIA/stdexec#nvhpc-25.09")

# mdspan: using specific commit instead of tag for compatibility
CPMAddPackage(
    NAME mdspan
    GITHUB_REPOSITORY kokkos/mdspan
    GIT_TAG 537053f83150f1b6f7528c1d961866d8628abc2e
)

# c-ares: required by libcoro when LIBCORO_FEATURE_NETWORKING is ON
CPMAddPackage("gh:c-ares/c-ares#v1.34.5")

# fmt: CRITICAL - must have FMT_INSTALL ON for mp-units formatter integration
# Without this, mp-units types cannot be formatted with fmt, causing massive template errors
# EXCLUDE_FROM_ALL must be NO to ensure proper installation
CPMFindPackage(
    NAME fmt
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 3391f9e9929d6ea2562863902cb523af3e47f3f5
    SYSTEM YES
    EXCLUDE_FROM_ALL NO
    OPTIONS
        "FMT_INSTALL ON"  # Required for mp-units fmt::formatter integration
)

# mp-units: uses SOURCE_SUBDIR because CMakeLists is in src/ subdirectory
# MP_UNITS_API_STD_FORMAT OFF because we're using fmt formatters instead
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
        "MP_UNITS_API_STD_FORMAT OFF"  # Using fmt formatters instead
)

# range-v3: DOWNLOAD_ONLY because their CMakeLists.txt interferes with our build config
# We manually create an INTERFACE target to just use the headers
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

# libcoro: coroutine library with networking enabled (requires c-ares)
CPMAddPackage(
    NAME libcoro
    GITHUB_REPOSITORY jbaldwin/libcoro
    GIT_TAG v0.15.0
    OPTIONS
        "LIBCORO_FEATURE_NETWORKING ON"       # Enables async networking (requires c-ares)
        "LIBCORO_BUILD_TESTS OFF"
        "LIBCORO_EXTERNAL_DEPENDENCIES ON"
        "LIBCORO_BUILD_EXAMPLES OFF"
)

# Keep libvterm as subproject (has custom build process)
add_subdirectory(subprojects/libvterm EXCLUDE_FROM_ALL)

#------------------------------------------------------------------------------
# Third-party library configuration
#------------------------------------------------------------------------------

# Mark third-party libraries as SYSTEM to suppress warnings from their headers
foreach(target IN ITEMS simdjson CLI11 mp-units stdexec fmt)
    if(TARGET ${target})
        get_target_property(inc_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
        if(inc_dirs)
            set_target_properties(${target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${inc_dirs}"
            )
        endif()
    endif()
endforeach()

#------------------------------------------------------------------------------
# Project Targets
#------------------------------------------------------------------------------

# nixb_ui: Static library for terminal UI functionality
add_library(nixb_ui STATIC
    src/tty/app.cpp
    src/tty/dom.cpp
    src/tty/layout.cpp
    src/tty/paint.cpp
    src/tty/widgets.cpp
    src/tty/ansi.cpp
    src/tty/glyph-table.cpp
    src/tty/raster.cpp
    src/tty/raster-diff.cpp
)

# PUBLIC because users of nixb_ui need these headers and dependencies
target_include_directories(nixb_ui PUBLIC src)
target_link_libraries(nixb_ui
    PUBLIC
        libcoro          # Coroutine functionality exposed in headers
        fmt::fmt         # Formatting used in public interface
        mdspan           # Multidimensional spans in public interface
        Boost::headers   # Boost headers used in public interface (header-only)
)
target_compile_definitions(nixb_ui PUBLIC
    MDSPAN_USE_BRACKET_OPERATOR=1
    MDSPAN_USE_PAREN_OPERATOR=0
)

# nixb: Main executable (old implementation)
add_executable(nixb
    src/old/main.cpp
    src/old/NixBuildState.cpp
    src/old/NixLogParser.cpp
    src/old/NixLogPlayer.cpp
    src/old/NixLogRecorder.cpp
    src/old/NixLogForwardingLogger.cpp
    src/old/NixLogWatcher.cpp
    src/old/Ui.cpp
    src/old/Ansi.cpp
    src/old/UiRender.cpp
    src/old/UiStateBuilder.cpp
)

# PRIVATE because this is an executable - nothing links to it
target_link_libraries(nixb
    PRIVATE
        fmt::fmt
        simdjson::simdjson
        CLI11::CLI11
        Boost::headers
        PkgConfig::NIX_STORE
        PkgConfig::NIX_EXPR
        PkgConfig::NIX_FLAKE
        PkgConfig::NIX_CMD
)

# nxbcg: cgroups utility executable
add_executable(nxbcg
    src/cgroups.cpp
    src/uring.cpp
)
target_link_libraries(nxbcg
    PRIVATE
        fmt::fmt
        mp-units::mp-units
        STDEXEC::stdexec
)

# nxb-vterm-tests: Terminal emulator tests
add_executable(nxb-vterm-tests src/test/vterm-ut-main.cpp)
target_include_directories(nxb-vterm-tests PRIVATE src/test)
target_link_libraries(nxb-vterm-tests
    PRIVATE
        nixb_ui          # Includes libcoro, fmt, mdspan transitively
        libvterm
)

#------------------------------------------------------------------------------
# Testing
#------------------------------------------------------------------------------

enable_testing()
add_test(NAME vterm-tests COMMAND nxb-vterm-tests)

# Auto-run tests after building the test executable
add_custom_command(TARGET nxb-vterm-tests POST_BUILD
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R vterm-tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running vterm tests..."
)

#------------------------------------------------------------------------------
# Installation
#------------------------------------------------------------------------------

install(TARGETS nixb nxbcg RUNTIME DESTINATION bin)

#------------------------------------------------------------------------------
# Commented out executables (for reference)
#------------------------------------------------------------------------------

add_executable(ui-example src/tty/example.cpp)
target_link_libraries(ui-example PRIVATE nixb_ui)

# add_executable(nixbco src/nixbco.cpp src/nix-log-adapter.cpp)
# target_link_libraries(nixbco
#     PRIVATE
#         nixb_ui                  # Includes libcoro, fmt, mdspan transitively
#         simdjson::simdjson
#         CLI11::CLI11
#         range-v3
#         STDEXEC::stdexec
#         PkgConfig::NIX_STORE
#         PkgConfig::NIX_EXPR
#         PkgConfig::NIX_FLAKE
#         PkgConfig::NIX_CMD
# )
