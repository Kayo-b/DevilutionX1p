#!/usr/bin/env bash
# Voxel (or billboard) views from several town positions. usage: town_sweep.sh <tag> <voxel:true|false> "x,y x,y ..."
HERE=$(cd "$(dirname "$0")" && pwd)
S=${FP_SELFTEST_OUT:-$HERE/out}
mkdir -p "$S"
TAG=$1
VOX=$2
POS=$3
STEPS="lua:dev=require('devilutionx.dev');lua:dev.fp.on(true);lua:dev.fp.${TOGGLE:-voxel}($VOX)"
n=0
for p in $POS; do
	x=${p%,*}
	y=${p#*,}
	STEPS="$STEPS;lua:dev.fp.teleport($x,$y);wait:8"
	for yaw in 0 90 180 270; do
		STEPS="$STEPS;lua:dev.fp.view($yaw,0);wait:10;shot:$S/${TAG}_${n}_${yaw}.ppm"
	done
	n=$((n + 1))
done
STEPS="$STEPS;quit"
"$HERE/headless.sh" "$STEPS"
python3 - <<E
from PIL import Image
S="$S"; T="$TAG"; n=$n
w=h=None
rows=[]
for i in range(n):
    row=[Image.open(f"{S}/{T}_{i}_{y}.ppm") for y in (0,90,180,270)]
    rows.append(row)
w,h=rows[0][0].size
sheet=Image.new("RGB",(w*4,h*n))
for r,row in enumerate(rows):
    for c,im in enumerate(row): sheet.paste(im,(c*w,r*h))
sheet=sheet.resize((w*2,int(h*n/2)))
sheet.save(f"{S}/{T}_sweep.png")
E
