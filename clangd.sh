#!/usr/bin/env bash
echo >&2 "Running nix develop -c $@"
nix develop -c clangd "$@"
echo >&2 "Finished running nix develop -c $@"
