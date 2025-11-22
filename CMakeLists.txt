cmake_minimum_required(VERSION 3.24)
project(nixb VERSION 0.1.0 LANGUAGES CXX)

set(CLICOLOR ON)
set(CLICOLOR_FORCE ON)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

if(NOT CMAKE_BUILD_TYPE)
set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(
  -Wno-missing-field-initializers
  -Wno-unused-parameter
  -Wno-deprecated-literal-operator
  )


cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NIX_STORE REQUIRED IMPORTED_TARGET nix-store)
pkg_check_modules(NIX_EXPR REQUIRED IMPORTED_TARGET nix-expr)
pkg_check_modules(NIX_FLAKE REQUIRED IMPORTED_TARGET nix-flake)
pkg_check_modules(NIX_CMD REQUIRED IMPORTED_TARGET nix-cmd)
add_subdirectory(subprojects/fmt-12.0.0)
add_subdirectory(subprojects/simdjson-3.10.1)
add_subdirectory(subprojects/CLI11-2.4.2)
add_subdirectory(subprojects/libcoro)
add_subdirectory(subprojects/libvterm)
add_subdirectory(subprojects/mdspan)

add_executable(nixb
    src/main.cpp
    src/NixBuildState.cpp
    src/NixLogParser.cpp
    src/NixLogPlayer.cpp
    src/NixLogRecorder.cpp
    src/NixLogForwardingLogger.cpp
    src/NixLogWatcher.cpp
    src/Ui.cpp
    src/Ansi.cpp
    src/UiRender.cpp
    src/UiStateBuilder.cpp
)

target_include_directories(nixb PRIVATE ${Boost_INCLUDE_DIRS})

target_link_libraries(nixb PRIVATE
    fmt::fmt
    simdjson::simdjson
    CLI11::CLI11
    PkgConfig::NIX_STORE
    PkgConfig::NIX_EXPR
    PkgConfig::NIX_FLAKE
    PkgConfig::NIX_CMD
)

# UI system with coroutine widgets
add_executable(ui-example
    src/new/ui-app.cpp
    src/new/glyph-table.cpp
    src/new/tty-raster.cpp
    src/new/tty-raster-diff.cpp
    src/new/ansi.cpp
    src/new/ui-dom.cpp
    src/new/ui-layout.cpp
    src/new/ui-paint.cpp
    src/new/ui-widgets.cpp
    src/new/ui-example.cpp
)

target_link_libraries(ui-example PRIVATE
    fmt::fmt
    libcoro
    mdspan
)

target_include_directories(ui-example PRIVATE ${Boost_INCLUDE_DIRS})

target_compile_definitions(ui-example PRIVATE
    MDSPAN_USE_BRACKET_OPERATOR=1
    MDSPAN_USE_PAREN_OPERATOR=0
)

add_executable(nxb-vterm-tests
    src/new/test/vterm-ut-main.cpp
    src/new/ansi.cpp
    src/new/glyph-table.cpp
    src/new/tty-raster.cpp
    src/new/tty-raster-diff.cpp
    src/new/ui-app.cpp
    src/new/ui-dom.cpp
    src/new/ui-layout.cpp
    src/new/ui-paint.cpp
)

target_include_directories(nxb-vterm-tests PRIVATE src/new/test)

target_link_libraries(nxb-vterm-tests PRIVATE
    fmt::fmt
    libcoro
    libvterm
    mdspan
)

target_include_directories(nxb-vterm-tests PRIVATE ${Boost_INCLUDE_DIRS})

target_compile_definitions(nxb-vterm-tests PRIVATE
    MDSPAN_USE_BRACKET_OPERATOR=1
    MDSPAN_USE_PAREN_OPERATOR=0
)

install(TARGETS nixb ui-example nxb-vterm-tests
    RUNTIME DESTINATION bin
)
