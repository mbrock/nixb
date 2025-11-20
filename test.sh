#!/usr/bin/env bash
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$here"

if [[ ! -x build/nixb ]]; then
  echo "build/nixb is missing; run 'ninja -C build nixb' first." >&2
  exit 1
fi

echo "Running nixb smoke test against testdata/nom.json..."
build/nixb < testdata/nom.json > /dev/null
echo "OK"
