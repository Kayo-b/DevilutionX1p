#!/usr/bin/env python3
"""Export house voxel models for the first-person renderer.

Binary format (little endian), file `houses.bin`:
  u32 magic 'FPH1', u32 modelCount
  per model: u8 nameLen, name bytes, f32 shortUnits, f32 longUnits (model units, walls only), f32 voxelSize,
             u16 nx, u16 ny, u16 nz, u32 count, then count * { u8 i, u8 j, u8 k, u8 class, u8 shade }
Canonical orientation: i runs along the long side (the ridge), j across it with the door on the +j side, k up.
class: 0 wall, 1 roof. shade: 0..255, rank of the voxel's texture brightness within its class.
"""
import os, struct, sys
import numpy as np
sys.argv = [sys.argv[0], 'x', 'y', 'x']
import importlib.util
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('vox', os.path.join(HERE, 'voxelize.py'))
src = open(os.path.join(HERE, 'voxelize.py')).read().replace('\nmain()\n', '\n')
ns = {'__file__': os.path.join(HERE, 'voxelize.py')}
exec(compile(src, 'voxelize.py', 'exec'), ns)
ASSETS = ns['ASSETS']
voxelize = ns['voxelize']

MODELS = [
    ('house_01', ['house_01'], ['house_01_roof']),
    ('gillian', ['house_Gillian'], ['house_Gillian_roof']),
    ('pepin', ['house_Pepin_'], ['pepin_roof_']),
    ('house004', ['house_004'], ['roof_003']),
    ('house02', ['house_02'], ['roof_02']),
    ('smithy', ['hsmithy_walls'], ['hsmithy_roof']),
]
VS = 0.1

def door_side(keys, isroof):
    """Side (0:+i 1:-i 2:+j 3:-j) of the wall ring with the biggest gap near the ground."""
    wall = keys[~isroof]
    lo, hi = wall.min(axis=0), wall.max(axis=0)
    best, side = -1, 3
    ground = wall[(wall[:, 2] >= lo[2] + 2) & (wall[:, 2] <= lo[2] + 12)]
    for s, (axis, val) in enumerate(((0, hi[0]), (0, lo[0]), (1, hi[1]), (1, lo[1]))):
        ring = ground[np.abs(ground[:, axis] - val) <= 1]
        other = 1 - axis
        span = hi[other] - lo[other] + 1
        cells = len({(int(p[other]), int(p[2])) for p in ring})
        gap = span * 11 - cells
        if gap > best:
            best, side = gap, s
    return side

def canonical(keys, isroof, side):
    """Rotate/flip so the long axis is i and the door side is +j."""
    k = keys - keys.min(axis=0)
    ext = k.max(axis=0) + 1
    if ext[1] > ext[0]:  # long axis is j: swap i and j (a reflection), door side swaps with it
        k = k[:, [1, 0, 2]]
        side = {0: 2, 1: 3, 2: 0, 3: 1}[side]
    ext = k.max(axis=0) + 1
    i, j = k[:, 0].copy(), k[:, 1].copy()
    if side == 3:  # door on -j: mirror j so it is on +j (doors on a gable end stay where they are)
        j = ext[1] - 1 - j
    k[:, 0], k[:, 1] = i, j
    return k

def shade_rank(colors, mask):
    lum = colors.mean(axis=1)
    out = np.zeros(len(colors), np.uint8)
    idx = np.where(mask)[0]
    if len(idx):
        order = lum[idx].argsort().argsort()
        out[idx] = (order * 255 // max(1, len(idx) - 1)).astype(np.uint8)
    return out

def main():
    out = open(os.path.join(ASSETS, 'houses.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPH1', len(MODELS)))
    for name, wg, rg in MODELS:
        keys, colors, isroof = voxelize(wg[0], rg[0], VS)
        side = door_side(keys, isroof)
        kc = canonical(keys, isroof, side)
        ext = kc.max(axis=0) + 1
        wall = kc[~isroof]
        wext = wall.max(axis=0) - wall.min(axis=0) + 1
        shade = np.zeros(len(kc), np.uint8)
        shade += shade_rank(colors, ~isroof)
        shade += shade_rank(colors, isroof)
        assert ext.max() < 256 and len(kc) < 4_000_000
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm)
        out.write(struct.pack('<3fHHHI', float(wext[1]) * VS, float(wext[0]) * VS, VS, int(ext[0]), int(ext[1]), int(ext[2]), len(kc)))
        rec = np.zeros((len(kc), 5), np.uint8)
        rec[:, 0:3] = kc
        rec[:, 3] = isroof.astype(np.uint8)
        rec[:, 4] = shade
        out.write(rec.tobytes())
        print(f'{name}: {len(kc)} voxels ext {tuple(ext)} walls short {wext[1]*VS:.2f} long {wext[0]*VS:.2f} door side {side}')
    out.close()
    print('houses.bin', os.path.getsize(os.path.join(ASSETS, 'houses.bin')), 'bytes')

main()
