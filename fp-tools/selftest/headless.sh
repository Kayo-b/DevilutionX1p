#!/usr/bin/env bash
# Scripted headless first-person run: no window, no desktop input.
# usage: headless.sh '<FP_TEST_STEPS>'   (GAME=path/to/devilutionx to pick the build, ARGS="--hellfire" for extra game arguments)
HERE=$(cd "$(dirname "$0")" && pwd)
S=${FP_SELFTEST_OUT:-$HERE/out}
mkdir -p "$S"
# Every run saves the game into out/save (and its settings into out/config); with pristine copies in out/save.orig and
# out/config.orig, each run starts from them, so the same steps give the same levels.
if [ -d "$S/save.orig" ]; then rm -rf "$S/save" && cp -r "$S/save.orig" "$S/save"; fi
if [ -d "$S/config.orig" ]; then rm -rf "$S/config" && cp -r "$S/config.orig" "$S/config"; fi
GAME=${GAME:-$HERE/../../build/devilutionx}
DATA=${FP_DATA_DIR:-$HOME/Downloads/devilutionX}
FP_DAGGERFALL_DIR="${FP_DAGGERFALL_DIR-$HOME/.wine/drive_c/GOG Games/Daggerfall/arena2}" \
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy FP_TEST_STEPS="$1" FP_TEST_MENU_KEYS=${KEYS:-3} \
	timeout ${TIMEOUT:-120} "$GAME" --data-dir "$DATA" --save-dir "$S/save" --config-dir "$S/config" -n ${ARGS:-} >"$S/headless.log" 2>&1
echo "exit $?"
