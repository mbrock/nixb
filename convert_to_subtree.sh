#!/usr/bin/env bash
# Convert libcoro from git submodule to git subtree

set -e

echo "=== Converting libcoro from submodule to subtree ==="
echo

# Save our custom files that aren't in the upstream repo
echo "Saving custom meson integration files..."
mkdir -p /tmp/libcoro-meson-files
cp subprojects/libcoro/meson.build /tmp/libcoro-meson-files/ 2>/dev/null || true
cp subprojects/libcoro/include/coro/export.hpp /tmp/libcoro-meson-files/ 2>/dev/null || true

# 1. Remove the submodule
echo "1. Removing submodule..."
git submodule deinit -f subprojects/libcoro
git rm -f subprojects/libcoro
rm -rf .git/modules/subprojects/libcoro

# 2. Commit the removal
echo "2. Committing submodule removal..."
git commit -m "Remove libcoro submodule (converting to subtree)"

# 3. Add as subtree
echo "3. Adding libcoro as subtree..."
git subtree add --prefix subprojects/libcoro \
  https://github.com/jbaldwin/libcoro \
  main --squash

# 4. Restore our custom meson files
echo "4. Restoring meson integration files..."
cp /tmp/libcoro-meson-files/meson.build subprojects/libcoro/ 2>/dev/null || \
  cat > subprojects/libcoro/meson.build <<'EOF'
project('libcoro', 'cpp',
  version : '0.15.0',
  default_options : ['cpp_std=c++20']
)

# libcoro is header-only
libcoro_inc = include_directories('include')

libcoro_dep = declare_dependency(
  include_directories : libcoro_inc
)

# Make available as a subproject dependency
meson.override_dependency('libcoro', libcoro_dep)
EOF

cp /tmp/libcoro-meson-files/export.hpp subprojects/libcoro/include/coro/ 2>/dev/null || \
  cat > subprojects/libcoro/include/coro/export.hpp <<'EOF'
#pragma once

// Generated export header for header-only build
// libcoro is being used as header-only, no shared library exports needed

#define CORO_EXPORT
#define CORO_NO_EXPORT
#define CORO_DEPRECATED
#define CORO_DEPRECATED_EXPORT
#define CORO_DEPRECATED_NO_EXPORT
EOF

# 5. Commit the meson integration
echo "5. Adding meson integration files..."
git add subprojects/libcoro/meson.build
git add subprojects/libcoro/include/coro/export.hpp
git commit -m "Add meson build integration for libcoro subtree"

# Cleanup
rm -rf /tmp/libcoro-meson-files

echo
echo "=== Conversion complete! ==="
echo
echo "libcoro is now a git subtree with meson integration. Benefits:"
echo "  - Code is part of your repo (no submodule init needed)"
echo "  - Simpler for contributors"
echo "  - Can still pull updates with: git subtree pull --prefix subprojects/libcoro https://github.com/jbaldwin/libcoro main --squash"
echo
echo "Try building:"
echo "  meson setup --reconfigure build"
echo "  meson compile -C build"
echo
