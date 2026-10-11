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
    ('wall_door', 'd_wall_door', None),
    ('wall_concave', 'd_wall_concave', None),
]
TEXTURES = {
    'dungeon_wall': 'Modern-Cream-Blocks_seamless.tga', 'dungeon_wall_t': 'Modern-Cream-Blocks_seamless.tga',
    'podea_biserica': 'Pavement_01_UV_H_CM_1.jpg', 'podea_biserica_s': 'Pavement_01_UV_H_CM_1.jpg',
    'columns': 'marble_diablo.tga', 'Material__33': 'marble_diablo.tga', 'marble': 'marble_diablo.tga',
    'column_head': 'd_column_extr.tga', 'Metal_rust': 'Steel_02_UV_H_CM_1.jpg', 'hinge_bars': 'Steel_02_UV_H_CM_1.jpg',
    'door': 'door_diff.tga', 'black_Marble': 'Black marble seamless.tga', 'Wood_cabinet': 'seamless-wood-texture-free-6.jpg',
    'grinzi': 'grinda_02.jpg', 'capac': 'marble_diablo.tga',
}
# Textures that come out too dark in the dim dungeon light (the door is meant to stand out in its arch).
BRIGHTEN = {'door_diff.tga': 2.4}
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
        pixels = np.asarray(im, dtype=np.uint8)
        if name in BRIGHTEN:
            pixels = np.clip(pixels.astype(np.float32) * BRIGHTEN[name], 0, 255).astype(np.uint8)
        textures.append(pixels)
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


VAULT_CELLS = 6


def build_vault(faces, v, anchor='ceiling_big_master017'):
    """The ceiling_big_master groin vault, resampled as a low-poly heightfield of its underside (VAULT_CELLS squared
    cells): the original has about 1900 fine strips, far too many for one vault per ceiling tile."""
    tris = np.array([v[[a[0], b[0], c[0]]] for a, b, c, m, g in faces if g == anchor])
    lo, hi = tris.reshape(-1, 3).min(0), tris.reshape(-1, 3).max(0)
    n = VAULT_CELLS
    # Only the shell: the faces that are not near-vertical (the arches' sides and the springers are left out).
    normal = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    normal /= np.linalg.norm(normal, axis=1)[:, None] + 1e-12
    shell = tris[np.abs(normal[:, 2]) > 0.3]
    heights = np.full((n + 1, n + 1), np.nan)
    eps = 2e-3
    for i in range(n + 1):
        for j in range(n + 1):
            px = lo[0] + (hi[0] - lo[0]) * i / n
            py = lo[1] + (hi[1] - lo[1]) * j / n
            best = np.inf
            for t in shell:
                d = (t[1, 1] - t[2, 1]) * (t[0, 0] - t[2, 0]) + (t[2, 0] - t[1, 0]) * (t[0, 1] - t[2, 1])
                if abs(d) < 1e-12:
                    continue
                a = ((t[1, 1] - t[2, 1]) * (px - t[2, 0]) + (t[2, 0] - t[1, 0]) * (py - t[2, 1])) / d
                b = ((t[2, 1] - t[0, 1]) * (px - t[2, 0]) + (t[0, 0] - t[2, 0]) * (py - t[2, 1])) / d
                c = 1 - a - b
                if a >= -eps and b >= -eps and c >= -eps:
                    best = min(best, a * t[0, 2] + b * t[1, 2] + c * t[2, 2])
            if np.isfinite(best):
                heights[i, j] = best
    # Grid points the shell does not cover (the steep springers at the corners) take the lowest height around them.
    while np.isnan(heights).any():
        filled = heights.copy()
        for i in range(n + 1):
            for j in range(n + 1):
                if np.isnan(heights[i, j]):
                    near = heights[max(0, i - 1):i + 2, max(0, j - 1):j + 2]
                    if not np.isnan(near).all():
                        filled[i, j] = np.nanmin(near)
        heights = filled
    # Four-fold symmetric (the original's sides differ), so that neighbouring bays meet along their edges.
    heights = sum(np.rot90(h, k) for h in (heights, heights.T) for k in range(4)) / 8.0
    tex = texture_index('dungeon_wall')
    out = []
    for i in range(n):
        for j in range(n):
            corner = lambda a, b: np.array([lo[0] + (hi[0] - lo[0]) * a / n, lo[1] + (hi[1] - lo[1]) * b / n, heights[a, b]])
            uvf = lambda a, b: (a / n * 1.5, b / n * 1.5)
            for quad in (((i, j), (i + 1, j), (i + 1, j + 1)), ((i, j), (i + 1, j + 1), (i, j + 1))):
                pos = np.array([corner(*q) for q in quad])
                uv = np.array([uvf(*q) for q in quad])
                out.append((pos, uv, tex))
    return out


def front_to_max_x(tris):
    """Wall pieces are slabs seen from one side: mirror x so that the slab's flat front is at the highest x (the
    game puts it on the face towards the room; a niche or doorway bulges back towards lower x)."""
    xs = np.array([t[0][:, 0] for t in tris]).reshape(-1)
    lo, hi = xs.min(), xs.max()
    near_lo = np.count_nonzero(xs < lo + 0.2)
    near_hi = np.count_nonzero(xs > hi - 0.2)
    if near_hi >= near_lo:
        return tris
    return [(np.array([[2 * (lo + hi) / 2 - p[0], p[1], p[2]] for p in pos]), uv, tex) for pos, uv, tex in tris]


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
        if name.startswith('wall_'):
            tris = front_to_max_x(tris)
        models.append((name, tris, sorted(groups)))
    models.append(('vault', build_vault(faces, v), []))
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
