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
