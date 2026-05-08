nxt_libcoro_dep = dependency('libcoro')

nxt_mdspan_args = [
  '-DMDSPAN_USE_BRACKET_OPERATOR=1',
  '-DMDSPAN_USE_PAREN_OPERATOR=0',
]

nxt_inc = include_directories(
  'src',
  'subprojects/mdspan/include',
)

nxt_core_deps = []

nxt_core_sources = files(
  'src/nxt/ansi.cpp',
  'src/nxt/compositor.cpp',
  'src/nxt/glyph-table.cpp',
  'src/nxt/raster.cpp',
)

nxt_core_lib = static_library('nxt-core',
  nxt_core_sources,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  cpp_args : nxt_mdspan_args,
)

nxt_core_dep = declare_dependency(
  link_with : nxt_core_lib,
  include_directories : nxt_inc,
  dependencies : nxt_core_deps,
  compile_args : nxt_mdspan_args,
)

nxt_runtime_deps = [
  nxt_core_dep,
  nxt_libcoro_dep,
]

nxt_runtime_sources = files(
  'src/nxtio/app.cpp',
  'src/nxtio/signal-pipe.cpp',
)

nxt_lib = static_library('nxt',
  nxt_runtime_sources,
  include_directories : nxt_inc,
  dependencies : nxt_runtime_deps,
)

nxt_dep = declare_dependency(
  link_with : nxt_lib,
  include_directories : nxt_inc,
  dependencies : nxt_runtime_deps,
)

executable('tui_example',
  'src/nxt/tui_example.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)

nxt_test_inc = include_directories('test')

nxt_libvterm_sources = files(
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

nxt_libvterm = static_library('vterm',
  nxt_libvterm_sources,
  include_directories : include_directories(
    'subprojects/libvterm/include',
    'subprojects/libvterm/src'
  ),
  c_args : ['-I' + meson.current_build_dir()],
  override_options : ['warning_level=0'],
)

nxt_libvterm_dep = declare_dependency(
  link_with : nxt_libvterm,
  include_directories : include_directories('subprojects/libvterm/include'),
)

nxt_raster_test = executable('raster-test',
  'test/raster-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_core_dep],
)

nxt_http_test = executable('http-test',
  'test/http-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_core_dep],
)

nxt_http_coro_test = executable('http-coro-test',
  'test/http-coro-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_core_dep],
)

nxt_terminal_test = executable('terminal-test',
  'test/terminal-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep, nxt_libvterm_dep],
)

test('raster-test', nxt_raster_test)
test('http-test', nxt_http_test)
test('http-coro-test', nxt_http_coro_test)
test('terminal-test', nxt_terminal_test)
