#!/usr/bin/env python3
"""Export pieces of the Diablo dungeon kit (Dungeon Pieces-67266-.7z, dungeon_pieces.obj) for the first-person renderer.

Same format as export_landmarks.py (FPL2: a texture table, then models with texture coordinates), file
`dungeonkit.bin`. u and w run 0..1 over the piece's x and y extent; z is model units above its lowest point.

A piece is either a named group, or an anchor group plus every group stacked on it (a column's shaft with its base
and capital): all groups whose extent lies within the anchor's x and y extent grown by a margin.

The model `surfaces` holds no geometry: its three triangles only name the textures the raycaster lays on walls,
floors and ceilings (in that order).
"""
import os, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# The extracted models live outside the repo; source and terms (free, non-commercial use): assets/fp/CREDITS.md.
FP_DUNGEON_PIECES_DIR = os.environ.get('FP_DUNGEON_PIECES_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/dungeon_pieces')
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
TEXTURE_SIZE = 256

# name, anchor group, margin (model units) for stacked parts (None: the group alone)
PIECES = [
    ('stairs_01', 'stairs_01', None),
    ('column', 'Cylinder012', 0.08),
    ('sarcophagus', 'sarcofag', 0.15),
    ('torch_stand', 'foot', 0.25),
    ('door', 'door_dun', 0.05),
]
TEXTURES = {
    'dungeon_wall': 'Modern-Cream-Blocks_seamless.tga', 'dungeon_wall_t': 'Modern-Cream-Blocks_seamless.tga',
    'podea_biserica': 'Pavement_01_UV_H_CM_1.jpg', 'podea_biserica_s': 'Pavement_01_UV_H_CM_1.jpg',
    'columns': 'marble_diablo.tga', 'Material__33': 'marble_diablo.tga', 'marble': 'marble_diablo.tga',
    'column_head': 'd_column_extr.tga', 'Metal_rust': 'Steel_02_UV_H_CM_1.jpg', 'hinge_bars': 'Steel_02_UV_H_CM_1.jpg',
    'door': 'door_diff.tga', 'black_Marble': 'Black marble seamless.tga', 'Wood_cabinet': 'seamless-wood-texture-free-6.jpg',
    'grinzi': 'grinda_02.jpg', 'capac': 'marble_diablo.tga',
}
SURFACES = ['dungeon_wall', 'podea_biserica', 'dungeon_wall']  # walls, floors, ceilings

textures = []
texture_ids = {}


def texture_index(material):
    name = TEXTURES.get(material)
    if name is None:
        return -1
    if name not in texture_ids:
        from PIL import Image
        im = Image.open(os.path.join(FP_DUNGEON_PIECES_DIR, name)).convert('RGB').resize((TEXTURE_SIZE, TEXTURE_SIZE), Image.LANCZOS)
        texture_ids[name] = len(textures)
        textures.append(np.asarray(im, dtype=np.uint8))
    return texture_ids[name]


def read_obj(path):
    verts, uvs, tris, cur, mat = [], [], [], None, None
    for line in open(path):
        if line.startswith('v '):
            verts.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith('vt '):
            uvs.append(tuple(map(float, line.split()[1:3])))
        elif line.startswith('g '):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith('usemtl '):
            mat = line.split()[1]
        elif line.startswith('f '):
            idx = []
            for tok in line.split()[1:]:
                parts = tok.split('/')
                vi = int(parts[0])
                ti = int(parts[1]) if len(parts) > 1 and parts[1] else 0
                idx.append((vi - 1 if vi > 0 else len(verts) + vi, (ti - 1 if ti > 0 else len(uvs) + ti) if ti else -1))
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1], mat, cur))
    return np.array(verts), np.array(uvs), tris


def main():
    v, uvs, faces = read_obj(os.path.join(FP_DUNGEON_PIECES_DIR, 'dungeon_pieces.obj'))
    extent = {}
    for a, b, c, m, g in faces:
        p = v[[a[0], b[0], c[0]]]
        lo, hi = p.min(0), p.max(0)
        e = extent.get(g)
        extent[g] = (np.minimum(e[0], lo), np.maximum(e[1], hi)) if e else (lo, hi)
    models = []
    for name, anchor, margin in PIECES:
        if margin is None:
            groups = {anchor}
        else:
            alo, ahi = extent[anchor]
            groups = {g for g, (lo, hi) in extent.items()
                      if lo[0] >= alo[0] - margin and hi[0] <= ahi[0] + margin and lo[1] >= alo[1] - margin and hi[1] <= ahi[1] + margin}
        tris = []
        for a, b, c, m, g in faces:
            if g in groups:
                uv = np.array([uvs[i[1]] if i[1] >= 0 else (0.0, 0.0) for i in (a, b, c)])
                tris.append((v[[a[0], b[0], c[0]]], uv, texture_index(m)))
        models.append((name, tris, sorted(groups)))
    surfaces = [(np.zeros((3, 3)), np.zeros((3, 2)), texture_index(m)) for m in SURFACES]
    out = open(os.path.join(ASSETS, 'dungeonkit.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPL2', len(textures)))
    for t in textures:
        out.write(struct.pack('<HH', t.shape[1], t.shape[0]) + t.tobytes())
    out.write(struct.pack('<I', len(models) + 1))
    for name, tris, groups in models + [('surfaces', surfaces, [])]:
        p = np.array([t[0] for t in tris])
        lo = p.reshape(-1, 3).min(axis=0)
        hi = p.reshape(-1, 3).max(axis=0)
        span = np.maximum(np.array([hi[0] - lo[0], hi[1] - lo[1], 1.0]), 1e-6)
        span[2] = 1.0
        q = (p - lo) / span
        records = b''.join(struct.pack('<9f6fBh', *pos.reshape(-1).tolist(), *uv.reshape(-1).tolist(), 0, tex) for pos, (_, uv, tex) in zip(q, tris))
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm + struct.pack('<fI', float(hi[2] - lo[2]), len(tris)) + records)
        print(f'{name}: {len(tris)} triangles, size {(hi - lo).round(2)}, groups {groups[:8]}{"..." if len(groups) > 8 else ""}')
    out.close()
    print(len(textures), 'textures; dungeonkit.bin', os.path.getsize(os.path.join(ASSETS, 'dungeonkit.bin')), 'bytes')


main()
