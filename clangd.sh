#!/usr/bin/env bash
set -euo pipefail

echo >&2 "Running nix develop -c clangd $*"
exec nix develop -c clangd \
  --compile-commands-dir=build \
  '--query-driver=/nix/store/*/bin/g++,/nix/store/*/bin/c++,/nix/store/*/bin/gcc,/nix/store/*/bin/cc' \
  "$@"
