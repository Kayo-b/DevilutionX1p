#!/usr/bin/env bash
# Build changed files and launch the first-person build. Extra arguments go to the game (-f shows FPS).
# Configure once first: see fp-tools/README.md, section 2.
#
# Start straight on a level, in first person and god mode (the menu is clicked through for you, then the game is
# yours; fp_autotest.cpp runs the steps):
#   LEVEL=9 fp-tools/run.sh             dungeon level 9 (0: town; 17-24: Hellfire's, from its data)
#   LEVEL=9 SEED=900 fp-tools/run.sh    always the same layout (every quest active, as in the selftests)
#   GOD=0, FP=0                         without god mode; in the original view
#   STEPS="lua:dev.fp.teleport(53,30)"  more steps once there (';'-separated: lua:, wait:N, ...)
# With LEVEL set you play a scratch copy of the test hero (fp-tools/selftest/out/save.orig, copied afresh to
# out/play-save each time), so your own heroes are left alone; REAL_SAVES=1 plays your own (its first hero).
set -euo pipefail
cd "$(dirname "$0")/.."
mise exec cmake@4.4.3 ninja@1.13.2 -- ninja -C build
# Daggerfall's first-person weapon art, read from your own install (never shipped); unset to draw no weapon.
export FP_DAGGERFALL_DIR="${FP_DAGGERFALL_DIR-$HOME/.wine/drive_c/GOG Games/Daggerfall/arena2}"
DATA="${FP_DATA_DIR:-$HOME/Downloads/devilutionX}"
EXTRA=()
if [ -n "${LEVEL:-}" ]; then
	OUT=fp-tools/selftest/out
	fp=$([ "${FP:-1}" = 0 ] && echo false || echo true)
	god=$([ "${GOD:-1}" = 0 ] && echo false || echo true)
	steps="wait:10;lua:dev=require('devilutionx.dev');lua:dev.fp.on($fp);lua:dev.player.god($god)"
	if [ -n "${SEED:-}" ] && [ "$LEVEL" != 0 ]; then
		steps="$steps;lua:dev.quests.activateAll();lua:dev.level.reset($LEVEL,$SEED)"
	fi
	steps="$steps;lua:dev.level.warp.dungeon($LEVEL)"
	if [ -n "${STEPS:-}" ]; then
		steps="$steps;wait:60;$STEPS"
	fi
	export FP_TEST_STEPS="$steps"
	export FP_TEST_MENU_KEYS="${KEYS:-3}"
	if [ "${REAL_SAVES:-0}" != 1 ]; then
		if [ ! -d "$OUT/save.orig" ]; then
			echo "no test hero in $OUT/save.orig (see fp-tools/README.md, section 4), or play your own: REAL_SAVES=1" >&2
			exit 1
		fi
		rm -rf "$OUT/play-save"
		cp -r "$OUT/save.orig" "$OUT/play-save"
		EXTRA+=(--save-dir "$OUT/play-save")
	fi
	if [ "$LEVEL" -ge 17 ] && [ -z "${FP_DATA_DIR:-}" ]; then
		# The Hellfire mod's files sit next to the binary; --data-dir makes the game look for them there instead.
		DATA="$OUT/hfdata"
		mkdir -p "$DATA"
		for f in "$HOME/Downloads/devilutionX"/*.[mM][pP][qQ]; do ln -sf "$f" "$DATA/"; done
		ln -sfn "$PWD/build/mods" "$DATA/mods"
	fi
fi
exec ./build/devilutionx --data-dir "$DATA" -n "${EXTRA[@]}" "$@"
