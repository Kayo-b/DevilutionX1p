#!/usr/bin/env bash
# Dungeon level 1 (seed 777) first-person views from several spots, 4 yaws each. usage: dsweep.sh <tag> "x,y x,y ..." [extra lua steps]
HERE=$(cd "$(dirname "$0")" && pwd)
S=${FP_SELFTEST_OUT:-$HERE/out}
mkdir -p "$S"
TAG=$1
POS=$2
STEPS="lua:dev=require('devilutionx.dev');lua:dev.fp.on(true);lua:dev.player.invisible(true);lua:dev.player.god(true);lua:dev.level.reset(${LEVEL:-1},${SEED:-777});lua:dev.level.warp.dungeon(${LEVEL:-1});wait:90;${3:-wait:1}"
n=0
for p in $POS; do
	STEPS="$STEPS;lua:dev.fp.teleport(${p%,*},${p#*,});wait:8"
	for yaw in 0 90 180 270; do
		STEPS="$STEPS;lua:dev.fp.view($yaw,0);wait:6;shot:$S/${TAG}_${n}_${yaw}.ppm"
	done
	n=$((n + 1))
done
"$HERE/headless.sh" "$STEPS;quit" >/dev/null
python3 - <<E
from PIL import Image
S="$S"; T="$TAG"; n=$n
rows=[[Image.open(f"{S}/{T}_{i}_{y}.ppm") for y in (0,90,180,270)] for i in range(n)]
w,h=rows[0][0].size
sheet=Image.new("RGB",(w*4,h*n))
for r,row in enumerate(rows):
    for c,im in enumerate(row): sheet.paste(im,(c*w,r*h))
sheet.resize((w*2,h*n//2)).save(f"{S}/{T}_sweep.png")
E
