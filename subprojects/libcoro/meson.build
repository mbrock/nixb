project('libcoro', 'cpp',
  version : '0.15.0',
  default_options : ['cpp_std=gnu++23']
)

# libcoro is NOT header-only, it has implementation files
libcoro_inc = include_directories('include')

# Collect all the source files
libcoro_sources = files(
  'src/condition_variable.cpp',
  'src/default_executor.cpp',
  'src/event.cpp',
  'src/io_scheduler.cpp',
  'src/mutex.cpp',
  'src/poll.cpp',
  'src/semaphore.cpp',
  'src/sync_wait.cpp',
  'src/thread_pool.cpp',
  'src/detail/io_notifier_kqueue.cpp',
  'src/detail/pipe.cpp',
  'src/detail/task_self_deleting.cpp',
  'src/detail/timer_handle.cpp',
)

# Build as a static library
libcoro_lib = static_library('coro',
  libcoro_sources,
  include_directories : libcoro_inc,
  cpp_args : ['-DLIBCORO_FEATURE_NETWORKING=0'], # Disable networking for now
)

libcoro_dep = declare_dependency(
  include_directories : libcoro_inc,
  link_with : libcoro_lib
)

# Make available as a subproject dependency
meson.override_dependency('libcoro', libcoro_dep)
