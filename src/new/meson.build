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
    libcoro_dep
  ],
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
    libcoro_dep
  ],
  install: false
)
