nxt_libcoro_dep = dependency('libcoro')
nxt_libcares_dep = dependency('libcares')
nxt_libssl_dep = dependency('libssl')
nxt_libcrypto_dep = dependency('libcrypto')

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
  nxt_libcares_dep,
  nxt_libssl_dep,
  nxt_libcrypto_dep,
  nlohmann_json_dep,
]

nxt_runtime_sources = files(
  'src/nxtio/app.cpp',
  'src/nxtio/http.cpp',
  'src/nxtio/input.cpp',
  'src/nxtio/net.cpp',
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

executable('input_hud_example',
  'src/nxt/input_hud_example.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)

executable('nxthttp',
  'src/nxtio/nxthttp.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep],
)

executable('nxtllm',
  'src/nxtio/nxtllm.cpp',
  include_directories : nxt_inc,
  dependencies : [nxt_dep, duckdb_dep],
  cpp_args : ['-Wno-unused-parameter'],
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

nxt_http_io_test = executable('http-io-test',
  'test/http-io-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep],
)

nxt_buffers_test = executable('buffers-test',
  'test/buffers-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep],
)

nxt_input_test = executable('input-test',
  'test/input-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep],
)

nxt_llm_test = executable('llm-test',
  'test/llm-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep],
)

nxt_net_include_test = executable('net-include-test',
  'test/net-include-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep],
)

nxt_terminal_test = executable('terminal-test',
  'test/terminal-test.cpp',
  include_directories : [nxt_inc, nxt_test_inc],
  dependencies : [nxt_dep, nxt_libvterm_dep],
)

test('raster-test', nxt_raster_test)
test('http-test', nxt_http_test)
test('http-io-test', nxt_http_io_test)
test('buffers-test', nxt_buffers_test)
test('input-test', nxt_input_test)
test('llm-test', nxt_llm_test)
test('net-include-test', nxt_net_include_test)
test('terminal-test', nxt_terminal_test)
