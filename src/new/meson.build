# Coroutine prototype (experimental)
# libcoro_dep is provided by root meson.build

nixb_coro_prototype = executable(
  'nixb-coro-prototype',
  'main_coro.cpp',
  dependencies: [
    nix_main_dep,
    nix_store_dep,
    nix_expr_dep,
    fmt_dep,
    libcoro_dep
  ],
  install: false  # Just a prototype
)

# Lifecycle demonstration (doesn't need Nix deps)
nixb_lifecycle_demo = executable(
  'nixb-lifecycle-demo',
  'test_lifecycle.cpp',
  dependencies: [
    fmt_dep,
    libcoro_dep
  ],
  install: false
)
