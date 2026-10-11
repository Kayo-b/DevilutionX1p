#!/usr/bin/env python3
"""Export the plaza meshes of Tristram's landmark buildings (the smithy with its open porch, Ogden's tavern).

These are placed over fixed town footprints by the game (see Landmarks in Source/fp/fp_buildings.cpp) instead of
being stretched over a detected footprint like the cottages of housemesh.bin.

Binary format (little endian), file `landmarks.bin`:
  u32 magic 'FPL2', u32 textureCount
  per texture: u16 width, u16 height, width * height * 3 bytes RGB (rows top to bottom)
  u32 modelCount
  per model: u8 nameLen, name bytes, f32 height (plaza units, ground to top), u32 triangleCount,
             then triangleCount * { 9 x f32 (three vertices u, w, z), 6 x f32 (texture u, v per vertex),
                                     u8 class, i16 texture (-1: none) }
u and w run 0..1 over the model's plaza x and y extent; z is plaza units above its lowest point.
class: 0 wall, 1 roof, 2 door. Texture coordinates are the OBJ's (v up, repeating past 0..1).
"""
import os, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'voxelize.py')).read().replace('\nmain()\n', '\n')
ns = {'__file__': os.path.join(HERE, 'voxelize.py')}
exec(compile(src, 'voxelize.py', 'exec'), ns)
load = ns['load']

PLAZA_DIR = ns['FP_PLAZA_DIR']
CHURCH_DIR = os.environ.get('FP_CHURCH_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/church')
TEXTURE_SIZE = 256
# Diffuse texture of each material (from the models' .mtl files), per model file.
PLAZA_TEXTURES = {
    'stone_wall': 'wall_diff.png', 'arteziana': 'Jagged+Stone+seamless+texture-1_diff.tga', 'straw_roof_ext': 'str_roof_dif.tga',
    'straw_roof_int': 'straw_int_diff.tga', 'Roof_straw': 'str_roof_dif.tga', 'roof_int': 'straw_int_diff.tga', 'grinzi': 'grinda_02.jpg',
    'door': 'door_diff.tga',
}
CHURCH_TEXTURES = {
    'church_wall': 'Steinwand_D1_Diffuse.jpg', 'ch_roof': 'roof_03.jpg', 'door': 'door_diff.tga',
    'vitraliu': 'stained_glass_1_by_shadowresident.tga', 'ch_roof_int': 'wood_texture_by_ftisis_stock.tga', 'grinzi': 'grinda_02.jpg', 'ch_floor': 'Pavement_01_UV_H_CM_1.jpg',
}
textures = []      # RGB arrays
texture_ids = {}   # path -> index


def texture_index(directory, table, material):
    name = table.get(material)
    if name is None:
        return -1
    path = os.path.join(directory, name)
    if path not in texture_ids:
        from PIL import Image
        im = Image.open(path).convert('RGB')
        im = im.resize((TEXTURE_SIZE, TEXTURE_SIZE), Image.LANCZOS)
        texture_ids[path] = len(textures)
        textures.append(np.asarray(im, dtype=np.uint8))
    return texture_ids[path]


def read_obj(path):
    """Vertices, texture coordinates and triangles (vertex and uv indices, material, group) of an OBJ."""
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
    return np.array(verts), np.array(uvs) if uvs else np.zeros((1, 2)), tris


def tri_uv(uvs, a, b, c):
    return np.array([uvs[i] if i >= 0 else (0.0, 0.0) for i in (a[1], b[1], c[1])])


CHURCH = os.path.join(os.environ.get('FP_CHURCH_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/church'), 'church.obj')
# Interior and below-ground parts of the church model (the cathedral's first level under it) are left out.
# The inner roof surfaces and inner walls stay: they close the slits between the roof slabs and the window openings.
CHURCH_SKIP = ('rope', 'bell', 'column', 'tuscan', 'hinge', 'altar_cloth', 'Box4', 'Box5', 'Box6', 'Cylinder')
CHURCH_MATS = ('church_wall', 'ch_roof', 'ch_roof_int', 'door', 'door_iron', 'vitraliu', 'grinzi', 'ch_floor')


def church_triangles():
    v, uvs, tris = read_obj(CHURCH)
    out = []
    for a, b, c, m, g in tris:
        if any(t in g for t in CHURCH_SKIP) or m not in CHURCH_MATS:
            continue
        p = v[[a[0], b[0], c[0]]]
        if p[:, 2].max() <= 14.7:
            continue
        out.append((p, 1 if m in ('ch_roof', 'ch_roof_int') else 0, tri_uv(uvs, a, b, c), texture_index(CHURCH_DIR, CHURCH_TEXTURES, m)))
    # Behind the cross windows of the nave's side walls: the interior is not modelled, so a box of stained glass a
    # little inside the walls shows through them (the original art has them glow red).
    glass = texture_index(CHURCH_DIR, CHURCH_TEXTURES, 'vitraliu')
    x0, x1, y0, y1, z0, z1 = 43.7, 51.5, 286.6, 297.3, 14.6, 17.7
    def quad(a, b, c, d, uv):
        out.append((np.array([a, b, c]), 0, np.array([uv(a), uv(b), uv(c)]), glass))
        out.append((np.array([a, c, d]), 0, np.array([uv(a), uv(c), uv(d)]), glass))
    along_y = lambda p: (p[1] * 0.6, p[2] * 0.6)
    along_x = lambda p: (p[0] * 0.6, p[2] * 0.6)
    quad((x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1), along_y)
    quad((x1, y1, z0), (x1, y0, z0), (x1, y0, z1), (x1, y1, z1), along_y)
    quad((x1, y0, z0), (x0, y0, z0), (x0, y0, z1), (x1, y0, z1), along_x)
    quad((x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1), along_x)
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


_plaza = None


def plaza_triangles(box):
    """Triangles (with class: 0 wall, 1 roof, 2 door; uvs; texture) of the plaza groups inside box, above the ground."""
    global _plaza
    if _plaza is None:
        _plaza = read_obj(os.path.join(PLAZA_DIR, 'Tristram_plaza.obj'))
    v, uvs, faces = _plaza
    extent = {}
    for a, b, c, g_m in ((f[0], f[1], f[2], (f[4], f[3])) for f in faces):
        g = g_m[0]
        p = v[[a[0], b[0], c[0]]]
        lo, hi = p.min(0), p.max(0)
        e = extent.get(g)
        extent[g] = (np.minimum(e[0], lo), np.maximum(e[1], hi)) if e else (lo, hi)
    inside = {g for g, (lo, hi) in extent.items() if lo[0] >= box[0] and hi[0] <= box[2] and lo[1] >= box[1] and hi[1] <= box[3]}
    out = []
    for a, b, c, m, g in faces:
        if g not in inside:
            continue
        p = v[[a[0], b[0], c[0]]]
        if p[:, 2].max() <= PLAZA_GROUND + 0.05:
            continue
        cls = 1 if (m in ROOF_MATERIALS or 'roof' in g.lower()) else (2 if m == 'door' else 0)
        out.append((p, cls, tri_uv(uvs, a, b, c), texture_index(PLAZA_DIR, PLAZA_TEXTURES, m)))
    return out


def main():
    models = []
    for name, box in PLAZA_BUILDINGS + [('church', None)]:
        tris = church_triangles() if name == 'church' else plaza_triangles(box)
        models.append((name, tris))
    out = open(os.path.join(ns['ASSETS'], 'landmarks.bin'), 'wb')
    out.write(struct.pack('<4sI', b'FPL2', len(textures)))
    for t in textures:
        out.write(struct.pack('<HH', t.shape[1], t.shape[0]) + t.tobytes())
    out.write(struct.pack('<I', len(models)))
    for name, tris in models:
        p = np.array([t[0] for t in tris])
        lo = p.reshape(-1, 3).min(axis=0)
        hi = p.reshape(-1, 3).max(axis=0)
        q = (p - lo) / np.array([hi[0] - lo[0], hi[1] - lo[1], 1.0])
        records = []
        for pos, (_, cls, uv, tex) in zip(q, tris):
            records.append(struct.pack('<9f6fBh', *pos.reshape(-1).tolist(), *uv.reshape(-1).tolist(), cls, tex))
        nm = name.encode()
        out.write(struct.pack('<B', len(nm)) + nm + struct.pack('<fI', float(hi[2] - lo[2]), len(records)) + b''.join(records))
        textured = sum(1 for t in tris if t[3] >= 0)
        print(f'{name}: {len(records)} triangles ({textured} textured), size {(hi - lo).round(2)}')
    out.close()
    print(len(textures), 'textures; landmarks.bin', os.path.getsize(os.path.join(ns['ASSETS'], 'landmarks.bin')), 'bytes')

main()
