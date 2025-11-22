project('mdspan', 'cpp',
  version : '0.6.0',
  license : 'BSD-3-Clause',
  default_options : ['cpp_std=gnu++23'])

# mdspan is a header-only library
mdspan_inc = include_directories('include')

# Declare dependency for use by parent projects
mdspan_dep = declare_dependency(
  include_directories : mdspan_inc,
  version : meson.project_version()
)

# Allow the dependency to be used via meson.override_dependency
meson.override_dependency('mdspan', mdspan_dep)

