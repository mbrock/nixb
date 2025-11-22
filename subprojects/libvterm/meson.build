project('libvterm', 'c',
  version: '0.3.3',
  default_options: ['c_std=c11'])

# Generate encoding tables from .tbl files using a helper script
gen_includes = custom_target(
  'gen_includes',
  output: ['encoding_DECdrawing.inc', 'encoding_uk.inc', 'fullwidth.inc'],
  command: [
    find_program('sh'),
    files('gen-all-includes.sh'),
    files('tbl2inc_c.pl'),
    files('src/encoding/DECdrawing.tbl'),
    files('src/encoding/uk.tbl'),
    files('find-wide-chars.pl'),
    '@OUTDIR@',
  ],
)

# Source files for libvterm
libvterm_sources = files(
  'src/encoding.c',
  'src/keyboard.c',
  'src/mouse.c',
  'src/parser.c',
  'src/pen.c',
  'src/screen.c',
  'src/state.c',
  'src/unicode.c',
  'src/vterm.c',
)

# Include directories
# - include: public headers
# - src: internal headers
# - build/subprojects/libvterm: generated include files
libvterm_inc = include_directories('include', 'src')
libvterm_gen_inc = include_directories('.', is_system: false)

# Build as static library
libvterm_lib = static_library(
  'vterm',
  [libvterm_sources, gen_includes],
  include_directories: [libvterm_inc, libvterm_gen_inc],
  install: false,
)

# Declare dependency for other projects to use
libvterm_dep = declare_dependency(
  link_with: libvterm_lib,
  include_directories: libvterm_inc,
)

meson.override_dependency('libvterm', libvterm_dep)
