#!/usr/bin/env bash
# Build changed files and launch the first-person build. Extra arguments go to the game (-f shows FPS).
# Configure once first: see fp-tools/README.md, section 2.
set -euo pipefail
cd "$(dirname "$0")/.."
mise exec cmake@4.4.3 ninja@1.13.2 -- ninja -C build
# Daggerfall's first-person weapon art, read from your own install (never shipped); unset to draw no weapon.
export FP_DAGGERFALL_DIR="${FP_DAGGERFALL_DIR-$HOME/.wine/drive_c/GOG Games/Daggerfall/arena2}"
exec ./build/devilutionx --data-dir "${FP_DATA_DIR:-$HOME/Downloads/devilutionX}" -n "$@"
