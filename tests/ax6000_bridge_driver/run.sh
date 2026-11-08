#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
if command -v uv >/dev/null 2>&1; then
  UV_CACHE_DIR=${UV_CACHE_DIR:-/tmp/ax6000-bridge-uv} \
    uv run --no-project python "$repo/tests/ax6000_bridge_driver/run.py"
else
  # The runner uses only the standard library; CI needs no extra installation.
  python3 "$repo/tests/ax6000_bridge_driver/run.py"
fi
