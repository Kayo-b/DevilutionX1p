#!/usr/bin/env bash
# First-person views of a dungeon level from several spots, 4 yaws each, as one labelled contact sheet.
# usage: levelsweep.sh <tag> <level> <seed> "x,y x,y ..." [extra lua steps after arriving]
#   GAME=path/to/devilutionx  another build (e.g. an older one, for before/after sheets)
#   HF=1                      Hellfire data (levels 17-24): uses out/hfdata, made here from FP_DATA_DIR's MPQs
#   YAWS="0 90 180 270"       the yaws shot at each spot; PITCH=0 the pitch
# Every quest is activated first, so a level and seed always give the same layout (the quests a new game picks at
# random change their levels' set pieces). Sheet: out/<tag>_sheet.png (half size).
HERE=$(cd "$(dirname "$0")" && pwd)
S=${FP_SELFTEST_OUT:-$HERE/out}
mkdir -p "$S"
TAG=$1
LEVEL=$2
SEED=$3
POS=$4
EXTRA=${5:-wait:1}
YAWS=${YAWS:-0 90 180 270}
PITCH=${PITCH:-0}
if [ "${HF:-0}" = 1 ]; then
	# The Hellfire mod's files sit next to the binary; --data-dir makes the game look for them there instead.
	D="$S/hfdata"
	mkdir -p "$D"
	for f in "${FP_DATA_DIR:-$HOME/Downloads/devilutionX}"/*.[mM][pP][qQ]; do ln -sf "$f" "$D/"; done
	ln -sfn "$(dirname "${GAME:-$HERE/../../build/devilutionx}")/mods" "$D/mods"
	export FP_DATA_DIR="$D"
fi
STEPS="wait:10;lua:dev=require('devilutionx.dev');lua:dev.fp.on(true);lua:dev.player.invisible(true);lua:dev.player.god(true);lua:dev.quests.activateAll();lua:dev.level.reset($LEVEL,$SEED);lua:dev.level.warp.dungeon($LEVEL);wait:90;$EXTRA"
n=0
for p in $POS; do
	STEPS="$STEPS;lua:dev.fp.teleport(${p%,*},${p#*,});wait:8"
	for yaw in $YAWS; do
		STEPS="$STEPS;lua:dev.fp.view($yaw,$PITCH);wait:6;shot:$S/${TAG}_${n}_${yaw}.ppm"
	done
	n=$((n + 1))
done
"$HERE/headless.sh" "$STEPS;lua:dev.fp.where();quit" >/dev/null
grep -h "autotest lua: tile\|fp: clean dungeon tiles\|dungeon structures" "$S/headless.log" | tail -3
python3 - "$S" "$TAG" "$n" "$LEVEL" "$SEED" "$POS" "$YAWS" <<'E'
import sys
from PIL import Image, ImageDraw
S, T, n, level, seed, pos, yaws = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6].split(), sys.argv[7].split()
rows = [[Image.open(f"{S}/{T}_{i}_{y}.ppm") for y in yaws] for i in range(n)]
w, h = rows[0][0].size
sheet = Image.new("RGB", (w * len(yaws) // 2, h * n // 2))
d = ImageDraw.Draw(sheet)
for r, row in enumerate(rows):
    for c, im in enumerate(row):
        sheet.paste(im.resize((w // 2, h // 2)), (c * w // 2, r * h // 2))
        d.text((c * w // 2 + 6, r * h // 2 + 14), f"L{level} s{seed} {pos[r]} yaw {yaws[c]}", fill=(255, 220, 0))
sheet.save(f"{S}/{T}_sheet.png")
print(f"{S}/{T}_sheet.png")
E
