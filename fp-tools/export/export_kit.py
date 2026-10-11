#!/usr/bin/env python3
"""Export pieces of the Diablo dungeon kit (Dungeon Pieces-67266-.7z, dungeon_pieces.obj) for the first-person renderer.

Same format as research/tristram_plaza/export_landmarks.py, file `dungeonkit.bin`:
  u32 magic 'FPL1', u32 modelCount
  per model: u8 nameLen, name bytes, f32 height (model units, ground to top), u32 triangleCount,
             then triangleCount * { 9 x f32 (three vertices u, w, z), u8 class }
u and w run 0..1 over the model's x and y extent; z is model units above its lowest point. class: 0 for all.
"""
import os, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# The extracted models live outside the repo; source and terms (free, non-commercial use): assets/fp/CREDITS.md.
FP_DUNGEON_PIECES_DIR = os.environ.get('FP_DUNGEON_PIECES_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/dungeon_pieces')
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
PIECES = ['stairs_01']


def load(groups):
    verts, tris, cur = [], {g: [] for g in groups}, None
    for line in open(os.path.join(FP_DUNGEON_PIECES_DIR, 'dungeon_pieces.obj')):
        if line.startswith('v '):
            verts.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith('g '):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith('f ') and cur in tris:
            idx = []
            for tok in line.split()[1:]:
                vi = int(tok.split('/')[0])
                idx.append(vi - 1 if vi > 0 else len(verts) + vi)
            for k in range(1, len(idx) - 1):
                tris[cur].append((idx[0], idx[k], idx[k + 1]))
    return np.array(verts), tris


def main():
    verts, tris = load(PIECES)
    out = open(os.path.join(ASSETS, 'dungeonkit.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPL1', len(PIECES)))
    for name in PIECES:
        p = np.array([verts[list(t)] for t in tris[name]])
        lo = p.reshape(-1, 3).min(axis=0)
        hi = p.reshape(-1, 3).max(axis=0)
        q = (p - lo) / np.array([hi[0] - lo[0], hi[1] - lo[1], 1.0])
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm + struct.pack('<fI', float(hi[2] - lo[2]), len(q)))
        for t in q:
            out.write(struct.pack('<9fB', *t.reshape(-1).tolist(), 0))
        print(f'{name}: {len(q)} triangles, size {(hi - lo).round(2)}')
    out.close()


main()
