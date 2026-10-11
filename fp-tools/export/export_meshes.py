#!/usr/bin/env python3
"""Export the house triangle meshes for the first-person renderer, in the same frame as houses.bin.

Binary format (little endian), file `housemesh.bin`:
  u32 magic 'FPM1', u32 modelCount
  per model (same order and names as houses.bin): u8 nameLen, name bytes, u32 triangleCount,
             then triangleCount * { 9 x f32 (three vertices i, j, k in voxel units), u8 class }
Vertices are in the canonical voxel frame of export_houses.py: i along the ridge, j across with the door on +j,
k up, measured in voxels (a voxel spans [n, n + 1)). class: 0 wall, 1 roof, 2 door.
"""
import os, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'export_houses.py')).read().replace('\nmain()\n', '\n')
ns = {'__file__': os.path.join(HERE, 'export_houses.py')}
exec(compile(src, 'export_houses.py', 'exec'), ns)
MODELS, VS, voxelize, door_side = ns['MODELS'], ns['VS'], ns['voxelize'], ns['door_side']
load = ns['ns']['load']  # voxelize.py's loader, run inside export_houses.py


def main():
    out = open(os.path.join(ns['ns']['ASSETS'], 'housemesh.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPM1', len(MODELS)))
    for name, wg, rg in MODELS:
        keys, colors, isroof = voxelize(wg[0], rg[0], VS)
        side = door_side(keys, isroof)
        verts, uvs, tris = load({wg[0], rg[0]})
        lo = verts[[t[0][0] for t in tris]].min(axis=0)  # as voxelize() does
        kmin = keys.min(axis=0)
        ext = keys.max(axis=0) - kmin + 1
        swap = ext[1] > ext[0]
        if swap:
            ext = ext[[1, 0, 2]]
            side = {0: 2, 1: 3, 2: 0, 3: 1}[side]
        records = []
        for (a, b, c, mat, grp) in tris:
            p = np.array([verts[a[0]], verts[b[0]], verts[c[0]]])
            q = (p - lo) / VS - kmin  # continuous voxel coordinates
            if swap:
                q = q[:, [1, 0, 2]]
            if side == 3:  # door on -j: mirror like canonical()
                q[:, 1] = ext[1] - q[:, 1]
            cls = 1 if grp == rg[0] else (2 if mat == 'door' else 0)
            records.append(struct.pack('<9fB', *q.reshape(-1).tolist(), cls))
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm + struct.pack('<I', len(records)) + b''.join(records))
        print(f'{name}: {len(records)} triangles, ext {tuple(ext)}')
    out.close()
    print('housemesh.bin', os.path.getsize(os.path.join(ns['ns']['ASSETS'], 'housemesh.bin')), 'bytes')


main()
