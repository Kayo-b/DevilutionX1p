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


# Buildings of the Tristram plaza model, each the OBJ groups lying inside a box of the plaza's coordinates (walls,
# roofs, timber framing, doors, steps). The plaza is laid out like the town; fit_landmark.py fitted each box's parts
# to the town art with these same selections, and its placements are in Landmarks (Source/fp/fp_buildings.cpp).
PLAZA_BUILDINGS = [
    ('cottage_long', (69.9, 264.9, 80.6, 270.3)),
    ('farnham', (92.2, 229.0, 98.9, 234.4)),
    ('gillian', (61.7, 243.0, 68.4, 248.3)),
    ('ogden', (72.7, 247.0, 81.3, 255.9)),
    ('pepin', (72.4, 230.5, 80.1, 238.2)),
    ('cottage_small', (49.6, 258.3, 55.7, 264.2)),
    ('cottage_tall', (94.3, 238.2, 100.5, 246.1)),
    ('smithy', (85.5, 247.2, 97.2, 255.5)),
    ('fountain', (84.2, 239.7, 87.0, 242.5)),
]
PLAZA_GROUND = 14.6
ROOF_MATERIALS = ('straw_roof_ext', 'straw_roof_int', 'Roof_straw', 'roof_int')


def plaza_triangles(box):
    """Triangles (and classes: 0 wall, 1 roof, 2 door) of the plaza groups inside box, above the ground."""
    plaza = os.path.join(ns['FP_PLAZA_DIR'], 'Tristram_plaza.obj')
    verts, faces, cur, mat = [], [], None, None
    for line in open(plaza):
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
                faces.append((idx[0], idx[k], idx[k + 1], cur, mat))
    v = np.array(verts)
    extent = {}
    for a, b, c, g, m in faces:
        p = v[[a, b, c]]
        lo, hi = p.min(0), p.max(0)
        e = extent.get(g)
        extent[g] = (np.minimum(e[0], lo), np.maximum(e[1], hi)) if e else (lo, hi)
    inside = {g for g, (lo, hi) in extent.items() if lo[0] >= box[0] and hi[0] <= box[2] and lo[1] >= box[1] and hi[1] <= box[3]}
    out = []
    for a, b, c, g, m in faces:
        if g not in inside:
            continue
        p = v[[a, b, c]]
        if p[:, 2].max() <= PLAZA_GROUND + 0.05:
            continue
        cls = 1 if (m in ROOF_MATERIALS or 'roof' in g.lower()) else (2 if m == 'door' else 0)
        out.append((p, cls))
    return out


def main():
    out = open(os.path.join(ns['ASSETS'], 'landmarks.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPL1', len(PLAZA_BUILDINGS) + 1))
    for name, box in PLAZA_BUILDINGS + [('church', None)]:
        tris = church_triangles() if name == 'church' else plaza_triangles(box)
        p = np.array([t[0] for t in tris])
        classes = [t[1] for t in tris]
        lo = p.reshape(-1, 3).min(axis=0)
        hi = p.reshape(-1, 3).max(axis=0)
        q = (p - lo) / np.array([hi[0] - lo[0], hi[1] - lo[1], 1.0])
        records = []
        for t, cls in zip(q, classes):
            records.append(struct.pack('<9fB', *t.reshape(-1).tolist(), cls))
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm + struct.pack('<fI', float(hi[2] - lo[2]), len(records)) + b''.join(records))
        print(f'{name}: {len(records)} triangles, size {(hi - lo).round(2)}, base {lo[2]:.2f}')
    out.close()
    print('landmarks.bin', os.path.getsize(os.path.join(ns['ASSETS'], 'landmarks.bin')), 'bytes')


main()
