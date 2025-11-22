nixbco = executable(
  'nixbco',
  ['nixbco.cpp', 'nix-log-adapter.cpp'],
  dependencies: [
    fmt_dep, 
   # simdjson_dep, 
    cli11_dep, 
    nix_store_dep, 
    nix_expr_dep, 
    nix_flake_dep, 
    nix_cmd_dep, 
    boost_dep,
    libcoro_dep
  ],
  install: true
)

# UI system with coroutine widgets
ui_example = executable(
  'ui-example',
  [
    'ui-app.cpp',
    'glyph-table.cpp',
    'tty-raster.cpp',
    'tty-raster-diff.cpp',
    'ansi.cpp',
    'ui-dom.cpp',
    'ui-layout.cpp',
    'ui-paint.cpp',
    'ui-widgets.cpp',
    'ui-example.cpp'
  ],
  dependencies: [
    fmt_dep,
    libcoro_dep,
    mdspan_dep
  ],
  cpp_args: ['-DMDSPAN_USE_BRACKET_OPERATOR=1', '-DMDSPAN_USE_PAREN_OPERATOR=0'],
  install: false
)

mini_raster_demo = executable(
  'mini-raster-demo',
  [
    'test/mini-raster-demo.cpp',
    'glyph-table.cpp',
    'tty-raster.cpp',
    'tty-raster-diff.cpp',
  ],
  dependencies: [
    fmt_dep,
    libcoro_dep,
    mdspan_dep
  ],
  cpp_args: ['-DMDSPAN_USE_BRACKET_OPERATOR=1', '-DMDSPAN_USE_PAREN_OPERATOR=0'],
  install: false
)

# Modern doctest-based test runner using vterm
nxb_vterm_tests = executable(
  'nxb-vterm-tests',
  [
    'test/vterm-doctest-main.cpp',
    'ansi.cpp',
    'glyph-table.cpp',
    'tty-raster.cpp',
    'tty-raster-diff.cpp',
    'ui-app.cpp',
    'ui-dom.cpp',
    'ui-layout.cpp',
    'ui-paint.cpp'
  ],
  include_directories: include_directories('test'),
  dependencies: [
    fmt_dep,
    libcoro_dep,
    libvterm_dep,
    mdspan_dep
  ],
  cpp_args: ['-DMDSPAN_USE_BRACKET_OPERATOR=1', '-DMDSPAN_USE_PAREN_OPERATOR=0'],
  install: false
)
