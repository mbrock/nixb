#!/usr/bin/env bash
# Convert libcoro from git submodule to git subtree

set -e

echo "=== Converting libcoro from submodule to subtree ==="
echo

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

echo
echo "=== Conversion complete! ==="
echo
echo "libcoro is now a git subtree. Benefits:"
echo "  - Code is part of your repo (no submodule init needed)"
echo "  - Simpler for contributors"
echo "  - Can still pull updates with: git subtree pull --prefix subprojects/libcoro https://github.com/jbaldwin/libcoro main --squash"
echo
