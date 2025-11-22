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

# Dependencies
cmake_policy(SET CMP0167 NEW)
find_package(Boost REQUIRED)
find_package(PkgConfig REQUIRED)

foreach(nixpkg IN ITEMS NIX_STORE NIX_EXPR NIX_FLAKE NIX_CMD)
    string(TOLOWER ${nixpkg} nixpkg_lower)
    string(REPLACE "_" "-" nixpkg_name ${nixpkg_lower})
    pkg_check_modules(${nixpkg} REQUIRED IMPORTED_TARGET ${nixpkg_name})
endforeach()

# Subprojects (suppress warnings from third-party code)
foreach(subproj IN ITEMS fmt-12.0.0 simdjson-3.10.1 CLI11-2.4.2 libcoro libvterm mdspan)
    add_subdirectory(subprojects/${subproj} EXCLUDE_FROM_ALL)
endforeach()

# Mark third-party libraries as system includes to suppress warnings
foreach(target IN ITEMS fmt simdjson CLI11)
    if(TARGET ${target})
        get_target_property(inc_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
        if(inc_dirs)
            set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${inc_dirs}")
        endif()
    endif()
endforeach()

# Common dependencies for Nix targets
set(NIX_LIBS PkgConfig::NIX_STORE PkgConfig::NIX_EXPR PkgConfig::NIX_FLAKE PkgConfig::NIX_CMD)

# Common dependencies and compile definitions for UI targets
set(UI_LIBS fmt::fmt libcoro mdspan)
set(UI_DEFS MDSPAN_USE_BRACKET_OPERATOR=1 MDSPAN_USE_PAREN_OPERATOR=0)

# Shared UI library (used by ui-example and tests)
add_library(nixb_ui STATIC
    src/ui-app.cpp
    src/ui-dom.cpp
    src/ui-layout.cpp
    src/ui-paint.cpp
    src/ui-widgets.cpp
    src/ansi.cpp
    src/glyph-table.cpp
    src/tty-raster.cpp
    src/tty-raster-diff.cpp
)
target_include_directories(nixb_ui PUBLIC src ${Boost_INCLUDE_DIRS})
target_link_libraries(nixb_ui PUBLIC ${UI_LIBS})
target_compile_definitions(nixb_ui PUBLIC ${UI_DEFS})

# Main nixb executable (old implementation)
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

# UI example executable
add_executable(ui-example src/ui-example.cpp)
target_link_libraries(ui-example PRIVATE nixb_ui)

# Test runner using vterm
add_executable(nxb-vterm-tests src/test/vterm-ut-main.cpp)
target_include_directories(nxb-vterm-tests PRIVATE src/test)
target_link_libraries(nxb-vterm-tests PRIVATE nixb_ui libvterm)

# Enable testing and register tests
enable_testing()
add_test(NAME vterm-tests COMMAND nxb-vterm-tests)

# Auto-run tests after successful build
add_custom_command(TARGET nxb-vterm-tests POST_BUILD
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R vterm-tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running vterm tests..."
)

# Install
install(TARGETS nixb ui-example nxb-vterm-tests RUNTIME DESTINATION bin)
