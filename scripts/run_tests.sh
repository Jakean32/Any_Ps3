#!/usr/bin/env bash
# Any_Ps3 — build + run the core test suite (Linux dev host / CI).
set -euo pipefail
cd "$(dirname "$0")/.."
make -j"$(nproc)" test
