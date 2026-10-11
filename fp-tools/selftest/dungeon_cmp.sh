#!/usr/bin/env bash
# First-person views vs the original iso view at the same spot in dungeon level 1.
# usage: dungeon_cmp.sh <tag> [seed] [extra lua steps run after arriving, e.g. "lua:dev.fp.teleport(41,54)"]
HERE=$(cd "$(dirname "$0")" && pwd)
S=${FP_SELFTEST_OUT:-$HERE/out}
mkdir -p "$S"
TAG=${1:-d}
SEED=${2:-777}
EXTRA=${3:-wait:1}
STEPS="lua:dev=require('devilutionx.dev');lua:dev.fp.on(true);lua:dev.player.invisible(true);lua:dev.player.god(true);lua:dev.level.reset(${LEVEL:-1},$SEED);lua:dev.level.warp.dungeon(${LEVEL:-1});wait:90;$EXTRA;wait:20"
for yaw in 0 90 180 270; do
	STEPS="$STEPS;lua:dev.fp.view($yaw,0);wait:8;shot:$S/${TAG}_$yaw.ppm"
done
STEPS="$STEPS;lua:dev.fp.on(false);wait:8;iso:$S/${TAG}_iso.ppm;quit"
"$HERE/headless.sh" "$STEPS"
python3 - <<E
import subprocess
S="$S"; T="$TAG"
files=[f"{S}/{T}_{y}.ppm" for y in (0,90,180,270)]+[f"{S}/{T}_iso.ppm"]
subprocess.run(["montage","-background","#202020","-geometry","640x360+4+4","-tile","3x2"]+files+[f"{S}/{T}_sheet.png"],check=False,stderr=subprocess.DEVNULL)
E
