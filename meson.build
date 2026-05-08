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

fmt_dep = dependency('fmt')
nlohmann_json_dep = dependency('nlohmann_json')

#add_project_arguments('-ftime-trace', language : 'cpp')

subdir('nxt')

nxb_sources = files(
  'src/nxb-main.cpp',
  'src/nix-log-adapter.cpp',
  'src/log-replay.cpp',
)

# Suppress warnings from nix headers
nix_warning_args = [
  '-Wno-unused-parameter',
  '-Wno-interference-size',
  '-Wno-missing-field-initializers',
  '-Wno-missing-designated-field-initializers',
]

executable('nxb',
  nxb_sources,
  dependencies : [
    nxt_dep,
    nix_store_dep,
    nix_expr_dep,
    nix_flake_dep,
    nix_cmd_dep,
    nix_util_dep,
    nlohmann_json_dep,
  ],
  cpp_args : nix_warning_args,
  link_args : ['-rdynamic'],
  install : true,
)

# Standalone log converter (minimal dependencies)
executable('nix-log-to-md',
  'src/nix-log-to-md.cpp',
  dependencies : [nlohmann_json_dep, fmt_dep],
  install : true,
)
