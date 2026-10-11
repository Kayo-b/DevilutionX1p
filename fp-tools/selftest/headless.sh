#!/usr/bin/env bash
# Scripted headless first-person run: no window, no desktop input.
# usage: headless.sh '<FP_TEST_STEPS>'   (GAME=path/to/devilutionx to pick the build)
HERE=$(cd "$(dirname "$0")" && pwd)
S=${FP_SELFTEST_OUT:-$HERE/out}
mkdir -p "$S"
GAME=${GAME:-$HERE/../../build/devilutionx}
DATA=${FP_DATA_DIR:-$HOME/Downloads/devilutionX}
FP_DAGGERFALL_DIR="${FP_DAGGERFALL_DIR-$HOME/.wine/drive_c/GOG Games/Daggerfall/arena2}" \
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy FP_TEST_STEPS="$1" FP_TEST_MENU_KEYS=${KEYS:-3} \
	timeout ${TIMEOUT:-120} "$GAME" --data-dir "$DATA" --save-dir "$S/save" --config-dir "$S/config" -n >"$S/headless.log" 2>&1
echo "exit $?"
