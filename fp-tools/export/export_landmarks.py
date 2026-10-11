#!/usr/bin/env python3
"""Export the plaza meshes of Tristram's landmark buildings (the smithy with its open porch, Ogden's tavern).

These are placed over fixed town footprints by the game (see Landmarks in Source/fp/fp_buildings.cpp) instead of
being stretched over a detected footprint like the cottages of housemesh.bin.

Binary format (little endian), file `landmarks.bin`:
  u32 magic 'FPL1', u32 modelCount
  per model: u8 nameLen, name bytes, f32 height (plaza units, ground to top), u32 triangleCount,
             then triangleCount * { 9 x f32 (three vertices u, w, z), u8 class }
u and w run 0..1 over the model's plaza x and y extent; z is plaza units above its lowest point.
class: 0 wall, 1 roof, 2 door.
"""
import os, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'voxelize.py')).read().replace('\nmain()\n', '\n')
ns = {'__file__': os.path.join(HERE, 'voxelize.py')}
exec(compile(src, 'voxelize.py', 'exec'), ns)
load = ns['load']

CHURCH = os.path.join(os.environ.get('FP_CHURCH_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/church'), 'church.obj')
# Interior and below-ground parts of the church model (the cathedral's first level under it) are left out.
CHURCH_SKIP = ('_int', 'arch_int', 'rope', 'bell', 'column', 'tuscan', 'hinge', 'altar_cloth', 'Box4', 'Box5', 'Box6', 'Cylinder')
CHURCH_MATS = ('church_wall', 'ch_roof', 'door', 'door_iron', 'vitraliu', 'grinzi', 'ch_floor')


def church_triangles():
    verts, tris, cur, mat = [], [], None, None
    for line in open(CHURCH):
        if line.startswith('v '):
            verts.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith('g '):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith('usemtl '):
            mat = line.split()[1]
        elif line.startswith('f '):
            idx = [int(t.split('/')[0]) for t in line.split()[1:]]
            idx = [i - 1 if i > 0 else len(verts) + i for i in idx]
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1], mat, cur))
    v = np.array(verts)
    out = []
    for a, b, c, m, g in tris:
        if any(s in g for s in CHURCH_SKIP) or m not in CHURCH_MATS:
            continue
        p = v[[a, b, c]]
        if p[:, 2].max() <= 14.7:
            continue
        out.append((p, 1 if m == 'ch_roof' else 0))
    return out


LANDMARKS = [
    ('smithy', ['hsmithy_walls'], ['hsmithy_roof']),
    ('ogden', ['house_Ogden'], ['Ogden_roof', 'Ogden_tower_roof']),
]


def main():
    out = open(os.path.join(ns['ASSETS'], 'landmarks.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPL1', len(LANDMARKS) + 1))
    for name, walls, roofs in LANDMARKS + [('church', None, None)]:
        if name == 'church':
            ct = church_triangles()
            p = np.array([t[0] for t in ct])
            classes = [t[1] for t in ct]
        else:
            verts, uvs, tris = load(set(walls + roofs))
            p = np.array([verts[[a[0], b[0], c[0]]] for a, b, c, _, _ in tris])
            classes = [1 if grp in roofs else (2 if mat == 'door' else 0) for (a, b, c, mat, grp) in tris]
        lo = p.reshape(-1, 3).min(axis=0)
        hi = p.reshape(-1, 3).max(axis=0)
        q = (p - lo) / np.array([hi[0] - lo[0], hi[1] - lo[1], 1.0])
        records = []
        for t, cls in zip(q, classes):
            records.append(struct.pack('<9fB', *t.reshape(-1).tolist(), cls))
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm + struct.pack('<fI', float(hi[2] - lo[2]), len(records)) + b''.join(records))
        print(f'{name}: {len(records)} triangles, plaza size {(hi - lo).round(2)}')
    out.close()
    print('landmarks.bin', os.path.getsize(os.path.join(ns['ASSETS'], 'landmarks.bin')), 'bytes')


main()
