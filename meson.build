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

executable('nixb', 'src/main.cpp',
  dependencies : [fmt_dep, simdjson_dep, cli11_dep],
  install : true)
