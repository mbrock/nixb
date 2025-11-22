#!/usr/bin/env bash
# Quick setup script for the coroutine prototype

set -e

echo "=== Setting up nixb coroutine prototype ==="
echo

# Check if libcoro is available (it's now a git subtree, so it should just be there)
if [ -d "../../subprojects/libcoro/include" ]; then
  echo "✓ libcoro found"
else
  echo "✗ libcoro not found"
  echo
  echo "libcoro should be a git subtree in subprojects/libcoro."
  echo "If you just cloned this repo, libcoro might be missing."
  echo
  echo "To add it, run from repo root:"
  echo "  git subtree add --prefix subprojects/libcoro \\"
  echo "    https://github.com/jbaldwin/libcoro main --squash"
  exit 1
fi

# Check if meson.build exists for libcoro
if [ ! -f "../../subprojects/libcoro/meson.build" ]; then
  echo "! libcoro/meson.build not found - this shouldn't happen"
  echo "  The meson.build file should be in the repo"
  exit 1
fi

echo "✓ export.hpp generated"
echo

echo "=== Ready to build! ==="
echo
echo "Run from project root:"
echo "  meson setup build"
echo "  meson compile -C build"
echo
echo "Then run the demos:"
echo "  ./build/src/new/nixb-lifecycle-demo"
echo "  ./build/src/new/nixb-coro-prototype"
echo
