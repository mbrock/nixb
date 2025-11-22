project('nixb', 'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=gnu++26',
    'warning_level=2',
    'werror=false',
    'b_ndebug=if-release',
  ])

fs = import('fs')

fmt_dep = dependency('fmt', fallback : ['fmt', 'fmt_dep'])
simdjson_dep = dependency('simdjson', fallback : ['simdjson', 'simdjson_dep'])
cli11_dep = dependency('cli11', fallback : ['cli11', 'CLI11_dep'])
nix_store_dep = dependency('nix-store')
nix_expr_dep = dependency('nix-expr')
nix_flake_dep = dependency('nix-flake')
nix_cmd_dep = dependency('nix-cmd')
nix_main_dep = dependency('nix-main')
boost_dep = dependency('boost')
mdspan_dep = dependency('mdspan', fallback : ['mdspan', 'mdspan_dep'])

# libcoro for coroutine prototype (git subtree in subprojects/libcoro)
# It's header-only and always available as a subtree
if fs.is_dir('subprojects/libcoro')
  libcoro_subproject = subproject('libcoro')
  libcoro_dep = libcoro_subproject.get_variable('libcoro_dep')
else
  warning('libcoro subtree not found - coroutine prototypes will not be built')
  warning('Run: git subtree add --prefix subprojects/libcoro https://github.com/jbaldwin/libcoro main --squash')
  libcoro_dep = disabler()
endif

# libvterm for terminal emulation in tests
if fs.is_dir('subprojects/libvterm')
  libvterm_subproject = subproject('libvterm')
  libvterm_dep = libvterm_subproject.get_variable('libvterm_dep')
else
  warning('libvterm not found - terminal emulation tests will not be available')
  libvterm_dep = disabler()
endif

add_project_arguments(
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-deprecated-literal-operator',
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

# Experimental coroutine prototype
subdir('src/new')
