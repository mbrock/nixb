project(
    'nixb',
    'cpp',
    version: '0.1.0',
    default_options: [
        'cpp_std=gnu++26',
        'warning_level=1',
        'werror=false',
        'b_ndebug=if-release',
    ],
)

fs = import('fs')

fmt_dep = dependency('fmt', fallback: ['fmt', 'fmt_dep'])
simdjson_dep = dependency('simdjson', fallback: ['simdjson', 'simdjson_dep'])
cli11_dep = dependency('cli11', fallback: ['cli11', 'CLI11_dep'])
nix_store_dep = dependency('nix-store')
nix_expr_dep = dependency('nix-expr')
nix_flake_dep = dependency('nix-flake')
nix_cmd_dep = dependency('nix-cmd')
boost_dep = dependency('boost')
mdspan_dep = dependency('mdspan', fallback: ['mdspan', 'mdspan_dep'])

libcoro_subproject = subproject('libcoro')
libcoro_dep = libcoro_subproject.get_variable('libcoro_dep')

libvterm_subproject = subproject('libvterm')
libvterm_dep = libvterm_subproject.get_variable('libvterm_dep')

add_project_arguments(
    '-Wno-missing-field-initializers',
    '-Wno-unused-parameter',
    '-Wno-deprecated-literal-operator',
    language: 'cpp',
)

executable(
    'nixb',
    [
        'src/main.cpp',
        'src/NixBuildState.cpp',
        'src/NixLogParser.cpp',
        'src/NixLogPlayer.cpp',
        'src/NixLogRecorder.cpp',
        'src/NixLogForwardingLogger.cpp',
        'src/NixLogWatcher.cpp',
        'src/Ui.cpp',
        'src/Ansi.cpp',
        'src/UiRender.cpp',
        'src/UiStateBuilder.cpp',
    ],
    dependencies: [
        fmt_dep,
        simdjson_dep,
        cli11_dep,
        nix_store_dep,
        nix_expr_dep,
        nix_flake_dep,
        nix_cmd_dep,
        boost_dep,
    ],
    install: true,
)

# Experimental coroutine prototype executables
executable(
    'nixbco',
    ['src/new/nixbco.cpp', 'src/new/nix-log-adapter.cpp'],
    dependencies: [
        fmt_dep,
        cli11_dep,
        nix_store_dep,
        nix_expr_dep,
        nix_flake_dep,
        nix_cmd_dep,
        boost_dep,
        libcoro_dep,
    ],
    install: true,
)

# UI system with coroutine widgets
executable(
    'ui-example',
    [
        'src/new/ui-app.cpp',
        'src/new/glyph-table.cpp',
        'src/new/tty-raster.cpp',
        'src/new/tty-raster-diff.cpp',
        'src/new/ansi.cpp',
        'src/new/ui-dom.cpp',
        'src/new/ui-layout.cpp',
        'src/new/ui-paint.cpp',
        'src/new/ui-widgets.cpp',
        'src/new/ui-example.cpp',
    ],
    dependencies: [fmt_dep, libcoro_dep, mdspan_dep],
    cpp_args: [
        '-DMDSPAN_USE_BRACKET_OPERATOR=1',
        '-DMDSPAN_USE_PAREN_OPERATOR=0',
    ],
    install: true,
)

executable(
    'mini-raster-demo',
    [
        'src/new/test/mini-raster-demo.cpp',
        'src/new/glyph-table.cpp',
        'src/new/tty-raster.cpp',
        'src/new/tty-raster-diff.cpp',
    ],
    dependencies: [fmt_dep, libcoro_dep, mdspan_dep],
    cpp_args: [
        '-DMDSPAN_USE_BRACKET_OPERATOR=1',
        '-DMDSPAN_USE_PAREN_OPERATOR=0',
    ],
    install: false,
)

# Modern boost-ut based test runner using vterm
executable(
    'nxb-vterm-tests',
    [
        'src/new/test/vterm-ut-main.cpp',
        'src/new/ansi.cpp',
        'src/new/glyph-table.cpp',
        'src/new/tty-raster.cpp',
        'src/new/tty-raster-diff.cpp',
        'src/new/ui-app.cpp',
        'src/new/ui-dom.cpp',
        'src/new/ui-layout.cpp',
        'src/new/ui-paint.cpp',
    ],
    include_directories: include_directories('src/new/test'),
    dependencies: [fmt_dep, libcoro_dep, libvterm_dep, mdspan_dep],
    cpp_args: [
        '-DMDSPAN_USE_BRACKET_OPERATOR=1',
        '-DMDSPAN_USE_PAREN_OPERATOR=0',
    ],
    install: true,
)
