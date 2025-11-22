cmake_minimum_required(VERSION 3.24)
project(libcoro VERSION 0.15.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS ON)

# Collect all the source files
set(LIBCORO_SOURCES
    src/condition_variable.cpp
    src/default_executor.cpp
    src/event.cpp
    src/io_scheduler.cpp
    src/mutex.cpp
    src/poll.cpp
    src/semaphore.cpp
    src/sync_wait.cpp
    src/thread_pool.cpp
    src/detail/pipe.cpp
    src/detail/task_self_deleting.cpp
    src/detail/timer_handle.cpp
)

# Add platform-specific I/O notifier
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND LIBCORO_SOURCES src/detail/io_notifier_epoll.cpp)
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin|FreeBSD|NetBSD|OpenBSD")
    list(APPEND LIBCORO_SOURCES src/detail/io_notifier_kqueue.cpp)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}. libcoro requires either epoll (Linux) or kqueue (BSD/macOS).")
endif()

# Build as a static library
add_library(libcoro STATIC ${LIBCORO_SOURCES})
add_library(libcoro::libcoro ALIAS libcoro)

target_include_directories(libcoro PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(libcoro PRIVATE
    LIBCORO_FEATURE_NETWORKING=0  # Disable networking for now
)

# Set output name to 'coro' to match meson build
set_target_properties(libcoro PROPERTIES OUTPUT_NAME coro)
