#!/usr/bin/env bash
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$here"

for fixture in nom hello hello2; do
  path="testdata/${fixture}.json"
  echo "Running nixb smoke test against ${path}..."
  build/nixb < "${path}" > /dev/null
  echo "OK (${fixture})"
done
