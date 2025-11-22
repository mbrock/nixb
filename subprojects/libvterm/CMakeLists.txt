cmake_minimum_required(VERSION 3.24)
project(libvterm VERSION 0.3.3 LANGUAGES C)

set(CMAKE_C_STANDARD 17)

# Generate encoding tables from .tbl files using a helper script
add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/encoding_DECdrawing.inc
        ${CMAKE_CURRENT_BINARY_DIR}/encoding_uk.inc
        ${CMAKE_CURRENT_BINARY_DIR}/fullwidth.inc
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/gen-all-includes.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tbl2inc_c.pl
        ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/DECdrawing.tbl
        ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/uk.tbl
        ${CMAKE_CURRENT_SOURCE_DIR}/find-wide-chars.pl
        ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/gen-all-includes.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/tbl2inc_c.pl
        ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/DECdrawing.tbl
        ${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/uk.tbl
        ${CMAKE_CURRENT_SOURCE_DIR}/find-wide-chars.pl
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating libvterm encoding tables"
)

# Source files for libvterm
set(LIBVTERM_SOURCES
    src/encoding.c
    src/keyboard.c
    src/mouse.c
    src/parser.c
    src/pen.c
    src/screen.c
    src/state.c
    src/unicode.c
    src/vterm.c
)

# Build as static library
add_library(libvterm STATIC
    ${LIBVTERM_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/encoding_DECdrawing.inc
    ${CMAKE_CURRENT_BINARY_DIR}/encoding_uk.inc
    ${CMAKE_CURRENT_BINARY_DIR}/fullwidth.inc
)
add_library(libvterm::libvterm ALIAS libvterm)

target_include_directories(libvterm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(libvterm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Set output name to 'vterm' to match meson build
set_target_properties(libvterm PROPERTIES OUTPUT_NAME vterm)

