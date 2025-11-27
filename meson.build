project('nixb', 'cpp', 'c',
  version : '0.3.0',
  subproject_dir : 'nix-src/src',
  default_options : [
    'cpp_std=c++23',
    'warning_level=2',
    'buildtype=debug',
    'localstatedir=/nix/var',
  ],
  meson_version : '>= 1.1',
)

# Build nix libraries from source (via subprojects)
subproject('libutil')
subproject('libstore')
subproject('libfetchers')
subproject('libexpr')
subproject('libflake')
subproject('libmain')
subproject('libcmd')

# Now get them as dependencies (they register via meson.override_dependency)
nix_util_dep = dependency('nix-util')
nix_store_dep = dependency('nix-store')
nix_expr_dep = dependency('nix-expr')
nix_flake_dep = dependency('nix-flake')
nix_cmd_dep = dependency('nix-cmd')

# System dependencies
boost_dep = dependency('boost')
fmt_dep = dependency('fmt')
nlohmann_json_dep = dependency('nlohmann_json')

# Custom dependencies (from our flake)
libcoro_dep = dependency('libcoro')

# Header-only deps - use cmake to find them
mpunits_dep = dependency('mp-units', method : 'cmake', modules : ['mp-units::mp-units'])
mdspan_dep = dependency('mdspan', method : 'cmake', modules : ['std::mdspan'])

# Vendored libvterm - built directly
perl = find_program('perl')
tbl2inc = files('subprojects/libvterm/tbl2inc_c.pl')

#decdrawing_inc = custom_target('DECdrawing.inc',
#                               input : 'subprojects/libvterm/src/encoding/DECdrawing.tbl',
#                               install_dir : 'subprojects/libvterm/include/encoding/',
#                               output : 'DECdrawing.inc',
#                               command : [perl, tbl2inc, '@INPUT@'],
#                               capture : true,
#                              )

#uk_inc = custom_target('uk.inc',
#                       input : 'subprojects/libvterm/src/encoding/uk.tbl',
#                       install_dir : 'subprojects/libvterm/include/encoding/',                       
#                       output : 'uk.inc',
#                       command : [perl, tbl2inc, '@INPUT@'],
#                       capture : true,
#                      )

libvterm_sources = files(
  'subprojects/libvterm/src/encoding.c',
  'subprojects/libvterm/src/keyboard.c',
  'subprojects/libvterm/src/mouse.c',
  'subprojects/libvterm/src/parser.c',
  'subprojects/libvterm/src/pen.c',
  'subprojects/libvterm/src/screen.c',
  'subprojects/libvterm/src/state.c',
  'subprojects/libvterm/src/unicode.c',
  'subprojects/libvterm/src/vterm.c',
)

libvterm = static_library('vterm',
  libvterm_sources,
  include_directories : include_directories('subprojects/libvterm/include', 'subprojects/libvterm/src'),
  c_args : ['-I' + meson.current_build_dir()],
  link_args : ['-ltbb']
)

libvterm_dep = declare_dependency(
  link_with : libvterm,
  include_directories : include_directories('subprojects/libvterm/include'),
)

# libnxt library
libnxt_sources = files(
  'src/nxt/app.cpp',
  'src/nxt/ansi.cpp',
  'src/nxt/compositor.cpp',
  'src/nxt/glyph-table.cpp',
  'src/nxt/raster.cpp',
  'src/nxt/signal-pipe.cpp',
)

libnxt_inc = include_directories('src')

libnxt = static_library('nxt',
  libnxt_sources,
  include_directories : libnxt_inc,
  link_args : ['-ltbb'],
  dependencies : [
    libcoro_dep,
    fmt_dep,
    boost_dep,
    mpunits_dep,
    mdspan_dep,
  ],
  cpp_args : [
    '-DMDSPAN_USE_BRACKET_OPERATOR=1',
    '-DMDSPAN_USE_PAREN_OPERATOR=0',
    '-DMDSPAN_HAS_CXX_17=1',
  ],
)

libnxt_dep = declare_dependency(
  link_with : libnxt,
  include_directories : libnxt_inc,
  dependencies : [libcoro_dep, fmt_dep, boost_dep, mpunits_dep, mdspan_dep],
)

# nxb executable
nxb_sources = files(
  'src/nxb-main.cpp',
  'src/nix-log-adapter.cpp',
  'src/log-replay.cpp',
)

executable('nxb',
  nxb_sources,
  include_directories : libnxt_inc,
  dependencies : [
    libnxt_dep,
    nix_store_dep,
    nix_expr_dep,
    nix_flake_dep,
    nix_cmd_dep,
    nix_util_dep,
    nlohmann_json_dep,
    boost_dep,
  ],
  cpp_args : [
    '-DMDSPAN_USE_BRACKET_OPERATOR=1',
    '-DMDSPAN_USE_PAREN_OPERATOR=0',
    '-DMDSPAN_HAS_CXX_17=1',
  ],
  link_args : ['-rdynamic', '-ltbb'],
  install : true,
)
