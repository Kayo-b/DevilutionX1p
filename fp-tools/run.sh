#!/usr/bin/env bash
# Build changed files and launch the first-person build. Extra arguments go to the game (-f shows FPS).
# Configure once first: see fp-tools/README.md, section 2.
set -euo pipefail
cd "$(dirname "$0")/.."
mise exec cmake@4.4.3 ninja@1.13.2 -- ninja -C build
exec ./build/devilutionx --data-dir "${FP_DATA_DIR:-$HOME/Downloads/devilutionX}" -n "$@"
