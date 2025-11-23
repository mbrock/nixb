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
include(cmake/CPM.cmake)

# Dependencies from system/nix
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED)
find_package(PkgConfig REQUIRED)

foreach(nixpkg IN ITEMS NIX_STORE NIX_EXPR NIX_FLAKE NIX_CMD)
    string(TOLOWER ${nixpkg} nixpkg_lower)
    string(REPLACE "_" "-" nixpkg_name ${nixpkg_lower})
    pkg_check_modules(${nixpkg} REQUIRED IMPORTED_TARGET ${nixpkg_name})
endforeach()

# CPM Dependencies
CPMAddPackage("gh:simdjson/simdjson#v3.10.1")
CPMAddPackage("gh:CLIUtils/CLI11#v2.6.1")
CPMAddPackage("gh:NVIDIA/stdexec#nvhpc-25.09")
#CPMAddPackage("gh:kokkos/mdspan#mdspan-0.6.0")
CPMAddPackage(
    NAME mdspan
    GITHUB_REPOSITORY kokkos/mdspan
    GIT_TAG 537053f83150f1b6f7528c1d961866d8628abc2e
    #DOWNLOAD_ONLY True
    )
    CPMAddPackage("gh:c-ares/c-ares#v1.34.5")

CPMFindPackage(
    NAME fmt
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 3391f9e9929d6ea2562863902cb523af3e47f3f5
    SYSTEM YES
    EXCLUDE_FROM_ALL NO
    OPTIONS
        "FMT_INSTALL ON"  # OMG THIS MADE MP-UNITS WORK AHHAHAA
)

CPMFindPackage(
    NAME mp-units
    VERSION 2.4.0
    GIT_TAG 6e06eddf205deaf6c2f2f63500c8c70ec220a99f
    GITHUB_REPOSITORY mpusz/mp-units
    SOURCE_SUBDIR src
    SYSTEM YES
#    EXCLUDE_FROM_ALL YES
    OPTIONS
        "MP_UNITS_BUILD_INSTALL OFF"
        "MP_UNITS_API_CONTRACTS NONE"
        "MP_UNITS_API_STD_FORMAT OFF"
)

CPMAddPackage(
  NAME range-v3
  URL https://github.com/ericniebler/range-v3/archive/0.12.0.zip
  VERSION 0.12.0
  # the range-v3 CMakeLists screws with configuration options
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
)

# Keep libvterm as subproject (has custom build process)
add_subdirectory(subprojects/libvterm EXCLUDE_FROM_ALL)

# Mark third-party libraries as system includes to suppress warnings
foreach(target IN ITEMS simdjson CLI11 mp-units stdexec)
    if(TARGET ${target})
        get_target_property(inc_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
        if(inc_dirs)
            set_target_properties(
                ${target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${inc_dirs}"
                INTERFACE_COMPILE_FEATURES "cxx_std_26"
            )
        endif()
    endif()
endforeach()

set(NIX_LIBS PkgConfig::NIX_STORE PkgConfig::NIX_EXPR PkgConfig::NIX_FLAKE PkgConfig::NIX_CMD)
set(UI_LIBS libcoro fmt::fmt mdspan)
set(UI_DEFS MDSPAN_USE_BRACKET_OPERATOR=1 MDSPAN_USE_PAREN_OPERATOR=0)

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
target_include_directories(nixb_ui PUBLIC src ${Boost_INCLUDE_DIRS})
target_link_libraries(nixb_ui PUBLIC ${UI_LIBS})
target_compile_definitions(nixb_ui PUBLIC ${UI_DEFS})

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
target_include_directories(nixb PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(nixb PRIVATE fmt::fmt simdjson::simdjson CLI11::CLI11 ${NIX_LIBS})

# add_executable(ui-example src/tty/example.cpp)
# target_link_libraries(ui-example PRIVATE libcoro nixb_ui)
# target_include_directories(ui-example PRIVATE ${UI_LIBS})

# add_executable(nixbco src/nixbco.cpp src/nix-log-adapter.cpp)
# target_include_directories(
#     nixbco PRIVATE libcoro ${Boost_INCLUDE_DIRS} ${UI_LIBS}
#     range-v3 STDEXEC::stdexec)
# target_link_libraries(
#     nixbco PRIVATE
#     fmt::fmt simdjson::simdjson CLI11::CLI11 ${UI_LIBS} ${NIX_LIBS}
#     range-v3 STDEXEC::stdexec)

add_executable(nxbcg src/cgroups.cpp)
target_include_directories(nxbcg PRIVATE)
target_link_libraries(nxbcg PUBLIC fmt::fmt mp-units::mp-units STDEXEC::stdexec)

add_executable(nxb-vterm-tests src/test/vterm-ut-main.cpp)
target_include_directories(nxb-vterm-tests PRIVATE src/test)
target_link_libraries(nxb-vterm-tests PRIVATE nixb_ui libvterm libcoro)

enable_testing()
add_test(NAME vterm-tests COMMAND nxb-vterm-tests)

add_custom_command(TARGET nxb-vterm-tests POST_BUILD
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R vterm-tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running vterm tests..."
)

install(TARGETS nixb  RUNTIME DESTINATION bin)
