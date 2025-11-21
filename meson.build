project('nixb', 'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
    'werror=false',
    'b_ndebug=if-release',
  ])

fmt_dep = dependency('fmt', fallback : ['fmt', 'fmt_dep'])
simdjson_dep = dependency('simdjson', fallback : ['simdjson', 'simdjson_dep'])
cli11_dep = dependency('cli11', fallback : ['cli11', 'CLI11_dep'])
nix_store_dep = dependency('nix-store')
nix_expr_dep = dependency('nix-expr')
nix_flake_dep = dependency('nix-flake')
nix_cmd_dep = dependency('nix-cmd')
boost_dep = dependency('boost')

add_project_arguments(
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  language: 'cpp'
)

executable('nixb', [
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
    'src/UiStateBuilder.cpp'
  ],
  dependencies : [fmt_dep, simdjson_dep, cli11_dep, nix_store_dep, nix_expr_dep, nix_flake_dep, nix_cmd_dep, boost_dep],
  install : true)
