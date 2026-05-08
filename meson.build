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

nix_opts = ['warning_level=0']
subproject('libutil', default_options : nix_opts)
subproject('libstore', default_options : nix_opts)
subproject('libfetchers', default_options : nix_opts)
subproject('libexpr', default_options : nix_opts)
subproject('libflake', default_options : nix_opts)
subproject('libmain', default_options : nix_opts)
subproject('libcmd', default_options : nix_opts)

nix_util_dep = dependency('nix-util')
nix_store_dep = dependency('nix-store')
nix_expr_dep = dependency('nix-expr')
nix_flake_dep = dependency('nix-flake')
nix_cmd_dep = dependency('nix-cmd')

boost_dep = dependency('boost')
fmt_dep = dependency('fmt')
nlohmann_json_dep = dependency('nlohmann_json')
libcoro_dep = dependency('libcoro')
tbb_dep = dependency('tbb')

mpunits_dep = dependency('mp-units',
                         method : 'cmake',
                         modules : ['mp-units::mp-units'])
mdspan_dep = dependency('mdspan',
                        method : 'cmake',
                        modules : ['std::mdspan'])

#add_project_arguments('-ftime-trace', language : 'cpp')

mdspan_args = [
  '-DMDSPAN_USE_BRACKET_OPERATOR=1',
  '-DMDSPAN_USE_PAREN_OPERATOR=0',
  '-DMDSPAN_HAS_CXX_17=1',
]

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
  include_directories : include_directories(
    'subprojects/libvterm/include',
    'subprojects/libvterm/src'
  ),
  c_args : ['-I' + meson.current_build_dir()],
)

libvterm_dep = declare_dependency(
  link_with : libvterm,
  include_directories : include_directories('subprojects/libvterm/include'),
)

libnxt_sources = files(
  'src/nxt/app.cpp',
  'src/nxt/ansi.cpp',
  'src/nxt/compositor.cpp',
  'src/nxt/glyph-table.cpp',
  'src/nxt/raster.cpp',
  'src/nxt/signal-pipe.cpp',
)

libnxt_inc = include_directories('src')

libnxt_deps = [
  libcoro_dep, fmt_dep, boost_dep, mpunits_dep, mdspan_dep, tbb_dep
]

libnxt = static_library('nxt',
  libnxt_sources,
  include_directories : libnxt_inc,
  dependencies : libnxt_deps,
  cpp_args : mdspan_args,
)

libnxt_dep = declare_dependency(
  link_with : libnxt,
  include_directories : libnxt_inc,
  dependencies : libnxt_deps,
)

nxb_sources = files(
  'src/nxb-main.cpp',
  'src/nix-log-adapter.cpp',
  'src/log-replay.cpp',
)

# Suppress warnings from nix headers
nix_warning_args = [
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-missing-designated-field-initializers',
]

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
  ],
  cpp_args : mdspan_args + nix_warning_args,
  link_args : ['-rdynamic'],
  install : true,
)

executable('tui_example',
  'src/nxt/tui_example.cpp',
  include_directories : libnxt_inc,
  dependencies : [libnxt_dep],
)

test_inc = include_directories('src/test')

raster_test = executable('raster-test',
  'src/test/raster-test.cpp',
  include_directories : [libnxt_inc, test_inc],
  dependencies : [libnxt_dep],
  cpp_args : mdspan_args,
)

terminal_test = executable('terminal-test',
  'src/test/terminal-test.cpp',
  include_directories : [libnxt_inc, test_inc],
  dependencies : [libnxt_dep, libvterm_dep],
  cpp_args : mdspan_args,
)

test('raster-test', raster_test)
test('terminal-test', terminal_test)

# Standalone log converter (minimal dependencies)
executable('nix-log-to-md',
  'src/nix-log-to-md.cpp',
  dependencies : [nlohmann_json_dep, fmt_dep],
  install : true,
)
