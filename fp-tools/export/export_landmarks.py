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
class: 0 wall, 1 roof, 2 door, 3 inside (the church's entrance hall, seen through its open door: lit by its lamps),
       4 rim (the sides of the church's cross windows, lit by the lamp the game puts behind each),
       5 slit (a gap between the planks of Adria's walls: drawn glowing with the light inside, as window glass is);
       plus 128 (ART_COLOURS): its texture is in the original art's colours already, which the game keeps as they are
       (Adria's shack and wares) instead of dimming it to dusk as it does the daylight photographs.
Texture coordinates are the OBJ's (v up, repeating past 0..1).
"""
import os, struct, subprocess
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


# The front door stands open: its two leaves (with their iron and the battens on their inside) swung into the church
# on their hinges at the doorway's sides.
DOOR_HINGE_Y = 286.22
DOOR_LEAVES = {'ch_front_door001': (46.70, 1), 'ch_door_iron001': (46.70, 1), 'Box434': (46.70, 1), 'Box435': (46.70, 1),
               'ch_front_door': (48.50, -1), 'ch_door_iron': (48.50, -1), 'Box432': (48.50, -1), 'Box433': (48.50, -1)}
DOOR_SWING = 95.0  # degrees
# Behind it a small entrance hall (the nave is not modelled inside): x, y, z extent in the plaza's units.
HALL = (45.95, 49.25, 286.36, 288.1, 14.63, 17.6)


def swing(p, hx, hy, degrees):
    """Vertices turned about a vertical hinge at (hx, hy) by an angle (counterclockwise seen from above)."""
    a = np.radians(degrees)
    c, s_ = np.cos(a), np.sin(a)
    q = p.copy()
    dx, dy = p[:, 0] - hx, p[:, 1] - hy
    q[:, 0] = hx + c * dx - s_ * dy
    q[:, 1] = hy + s_ * dx + c * dy
    return q


def open_door(group, p):
    """A door leaf's vertices turned about its hinge (vertical, at the doorway's side) into the church."""
    hinge = DOOR_LEAVES.get(group)
    if hinge is None:
        return p
    hx, sign = hinge
    return swing(p, hx, DOOR_HINGE_Y, DOOR_SWING * sign)


def face(out, corners, inward, material, cls, scale=0.5):
    """A quad (two triangles) wound so it faces `inward`, textured in world space (scale texture repeats per unit)."""
    q = [np.array(c, dtype=float) for c in corners]
    if np.dot(np.cross(q[1] - q[0], q[2] - q[0]), inward) < 0:
        q = q[::-1]
    axis = int(np.argmax(np.abs(inward)))
    keep = [k for k in range(3) if k != axis]
    uv = lambda p: (p[keep[0]] * scale, p[keep[1]] * scale)
    tex = texture_index(CHURCH_DIR, CHURCH_TEXTURES, material)
    out.append((np.array([q[0], q[1], q[2]]), cls, np.array([uv(q[0]), uv(q[1]), uv(q[2])]), tex))
    out.append((np.array([q[0], q[2], q[3]]), cls, np.array([uv(q[0]), uv(q[2]), uv(q[3])]), tex))


def entrance_hall(out):
    """The entrance hall behind the open door: a stone floor, stone walls and a timber ceiling, all facing in."""
    x0, x1, y0, y1, z0, z1 = HALL
    face(out, [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, 1), 'ch_floor', 3)
    face(out, [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, -1), 'ch_roof_int', 3)
    face(out, [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], (1, 0, 0), 'church_wall', 3)
    face(out, [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (-1, 0, 0), 'church_wall', 3)
    face(out, [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, -1, 0), 'church_wall', 3)


# The nave's side walls with their four cross windows each, and how far across (x) each wall reaches.
CROSS_WALLS = {'ch_wall_cross_02': (43.0, 43.5), 'ch_wall_cross_01': (51.7, 52.2)}


def cross_rim(p, across):
    """Whether a triangle of a side wall lines one of its cross windows: across the wall's thickness, facing along it
    or up or down, at the windows' height, clear of the wall's ends (and of the annex's doorway at its front)."""
    n = np.cross(p[1] - p[0], p[2] - p[0])
    length = np.linalg.norm(n)
    if length < 1e-12:
        return False
    lo, hi = p.min(0), p.max(0)
    return (abs(n[0]) / length < 0.3 and lo[2] > 15.5 and hi[2] < 17.6 and lo[1] > 288.0 and hi[1] < 297.3
            and lo[0] >= across[0] - 0.01 and hi[0] <= across[1] + 0.01)


def church_triangles():
    v, uvs, tris = read_obj(CHURCH)
    out = []
    for a, b, c, m, g in tris:
        if g not in DOOR_LEAVES and (any(t in g for t in CHURCH_SKIP) or m not in CHURCH_MATS):
            continue  # (the door's battens are Box43x: kept with their leaves)
        p = open_door(g, v[[a[0], b[0], c[0]]])
        if p[:, 2].max() <= 14.7:
            continue
        cls = 1 if m in ('ch_roof', 'ch_roof_int') else (3 if g in DOOR_LEAVES else 0)
        if g in CROSS_WALLS and cross_rim(p, CROSS_WALLS[g]):
            cls = 4
        out.append((p, cls, tri_uv(uvs, a, b, c), texture_index(CHURCH_DIR, CHURCH_TEXTURES, m)))
    entrance_hall(out)
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
    # The front: either side of the entrance hall (which stands where it would be).
    hx0, hx1 = HALL[0], HALL[1]
    quad((hx0, y0, z0), (x0, y0, z0), (x0, y0, z1), (hx0, y0, z1), along_x)
    quad((x1, y0, z0), (hx1, y0, z0), (hx1, y0, z1), (x1, y0, z1), along_x)
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
# Open doors of the plaza's buildings: their parts, turned about a hinge (x, y) by an angle. Ogden's tavern door
# stands open behind him, swung out on its left edge as the original art has it.
_OGDEN_DOOR = (78.69, 249.89, -100.0)
PLAZA_OPEN_DOORS = {g: _OGDEN_DOOR for g in ('door_generic_lower015', 'door_generic_upper007', 'door_tr028', 'door_tr029', 'door_tr030', 'door_tr031')}
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
        if g in PLAZA_OPEN_DOORS:
            p = swing(p, *PLAZA_OPEN_DOORS[g])
        if p[:, 2].max() <= PLAZA_GROUND + 0.05:
            continue
        cls = 1 if (m in ROOF_MATERIALS or 'roof' in g.lower()) else (2 if m == 'door' else 0)
        out.append((p, cls, tri_uv(uvs, a, b, c), texture_index(PLAZA_DIR, PLAZA_TEXTURES, m)))
    return out


# Adria's shack (research/adria, "Adria's Shack" of the same rebuild, extracted from its archive if missing): a hut of
# planks on a frame, its roof sloping down to the back, shelves and the door on the side facing the path, and her
# cauldron hanging from a frame beside it. The file is in the plaza's coordinates and also holds the hinges of every
# door in the plaza (left out), so the shack and the cauldron are each the groups inside a box.
ADRIA_DIR = os.environ.get('FP_ADRIA_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/adria')
ADRIA_ARCHIVE = os.environ.get('FP_ADRIA_ARCHIVE', os.path.expanduser("~/projects/diablo-hellfire-fp/Adria's Shack-67266-.7z"))
ADRIA_SHACK = (99.5, 290.4, 104.7, 295.3)
ADRIA_CAULDRON = (106.0, 292.5, 108.5, 294.0)
# Left out: the hinges (a few hundred triangles each for a strap a few pixels long), and the cauldron's chains, a
# few hundred triangles a link: two thin rods stand in for them (ADRIA_CHAINS: x, y, from z up to z).
ADRIA_SKIP = ('hinge', 'za_', 'hook')
ADRIA_CHAINS = [(107.085, 293.355, 15.84, 16.52), (107.655, 293.355, 15.84, 16.52)]
ADRIA_CHAIN_HALF = 0.014
# The door stands open, swung into the shack on its hinges (the art shows the doorway lit, the leaf inside).
ADRIA_DOOR = {f'Box{n}' for n in range(424, 432)}
ADRIA_DOOR_HINGE = (104.24, 292.44, -95.0)
# The roof's planks (the same wood as the walls): class 1, what the game hangs the rooms' ceilings under.
ADRIA_ROOF = {f'Box{n}' for n in range(385, 394)}


_adria = None


def adria_obj():
    global _adria
    if _adria is None:
        path = os.path.join(ADRIA_DIR, 'Adria_shack_hinges.obj')
        if not os.path.exists(path):
            subprocess.run(['7z', 'x', '-y', f'-o{ADRIA_DIR}', ADRIA_ARCHIVE], check=True, stdout=subprocess.DEVNULL)
        _adria = read_obj(path)
    return _adria


ART_COLOURS = 128  # added to a triangle's class: its texture is in the art's colours already (see the format above)


# The shack's wood in the art is dark slate, its roof a lighter blue-grey with patches of green-grey moss, the
# cauldron's iron a blue-black: the model's textures with their shades put on ramps of those colours (dark to light;
# picked from the art's pixels, a little lighter, as the iso view shows them in the hero's light). The planks'
# texture runs across its planks' length in the model's texture coordinates: turned, its grain runs along them.
ART_RAMPS = {
    'adria_wall': ('grinda_02.jpg', True, [(3, 6, 9), (10, 11, 20), (17, 18, 30), (25, 26, 40), (37, 36, 50), (54, 50, 62)]),
    'adria_roof': ('grinda_02.jpg', True, [(12, 13, 21), (22, 24, 36), (31, 34, 52), (40, 44, 62), (52, 54, 76), (68, 66, 92)]),
    'adria_iron': ('Dirty_Steel_02_UV_H_CM_1.jpg', False, [(3, 4, 9), (8, 10, 20), (14, 16, 30), (22, 24, 40), (34, 36, 56), (52, 54, 78)]),
}
WOOD_MOSS = (30, 42, 38)
_art_textures = {}


def adria_texture(material):
    """The texture of a material of the Adria model: the planks and the iron on the art's ramps (ART_COLOURS)."""
    return ramp_texture({'grinzi': 'adria_wall', 'cauldron': 'adria_iron', '02___Default': 'adria_iron'}[material])


def ramp_texture(name):
    """A texture of the model on one of ART_RAMPS: each texel the ramp's colour at its shade's rank."""
    if name not in _art_textures:
        from PIL import Image
        source, turn, ramp = ART_RAMPS[name]
        im = Image.open(os.path.join(ADRIA_DIR, source)).convert('RGB').resize((TEXTURE_SIZE, TEXTURE_SIZE), Image.LANCZOS)
        rgb = np.asarray(im, dtype=float)
        if turn:
            rgb = rgb.transpose(1, 0, 2)
        lum = rgb @ np.array([0.3, 0.59, 0.11])
        rank = np.argsort(np.argsort(lum.ravel())).reshape(lum.shape) / (lum.size - 1)
        ramp = np.array(ramp, dtype=float)
        pos = rank * (len(ramp) - 1)
        k = np.minimum(pos.astype(int), len(ramp) - 2)
        f = (pos - k)[..., None]
        out = ramp[k] * (1 - f) + ramp[k + 1] * f
        if name == 'adria_roof':
            # Moss: soft patches (smooth noise, the same every export) pulling the shade towards green-grey.
            rng = np.random.default_rng(67266)
            noise = np.asarray(Image.fromarray((rng.random((6, 6)) * 255).astype(np.uint8)).resize((TEXTURE_SIZE, TEXTURE_SIZE), Image.BICUBIC), dtype=float) / 255
            moss = np.clip((noise - 0.55) / 0.25, 0, 1)[..., None] * 0.6
            out = out * (1 - moss) + np.array(WOOD_MOSS) * (0.6 + 0.8 * rank[..., None]) * moss
        _art_textures[name] = len(textures)
        textures.append(np.clip(out, 0, 255).astype(np.uint8))
    return _art_textures[name]


def adria_triangles(box):
    """The groups of the Adria model inside box (x0, y0, x1, y1), as plaza_triangles gives them."""
    v, uvs, faces = adria_obj()
    extent = {}
    for a, b, c, m, g in faces:
        p = v[[a[0], b[0], c[0]]]
        e = extent.get(g)
        extent[g] = (np.minimum(e[0], p.min(0)), np.maximum(e[1], p.max(0))) if e else (p.min(0), p.max(0))
    inside = {g for g, (lo, hi) in extent.items() if lo[0] >= box[0] and hi[0] <= box[2] and lo[1] >= box[1] and hi[1] <= box[3]}
    out = []
    for a, b, c, m, g in faces:
        if g not in inside or any(g.startswith(s) for s in ADRIA_SKIP):
            continue
        p = v[[a[0], b[0], c[0]]]
        if g in ADRIA_DOOR:
            p = swing(p, *ADRIA_DOOR_HINGE)
        if p[:, 2].max() <= PLAZA_GROUND + 0.05:
            continue
        roof = g in ADRIA_ROOF
        out.append((p, (1 if roof else 0) | ART_COLOURS, tri_uv(uvs, a, b, c), ramp_texture('adria_roof') if roof else adria_texture(m)))
    return out


def plank_slits(out, v, faces, groups):
    """Glowing strips (class 5) in the gaps between the upright planks of the shack's walls, in each wall's middle:
    the art shows the light inside between them. A wall is the thin, tall planks in one plane."""
    extent = {}
    for a, b, c, m, g in faces:
        if g in groups:
            p = v[[a[0], b[0], c[0]]]
            e = extent.get(g)
            extent[g] = (np.minimum(e[0], p.min(0)), np.maximum(e[1], p.max(0))) if e else (p.min(0), p.max(0))
    middle = np.mean([(lo + hi) / 2 for lo, hi in extent.values()], axis=0)
    count = 0
    for axis in (0, 1):  # the wall's normal: x or y
        along = 1 - axis
        walls = {}
        for g, (lo, hi) in extent.items():
            if hi[axis] - lo[axis] < 0.05 and hi[2] - lo[2] > 1.0 and hi[along] - lo[along] < 1.0:
                walls.setdefault(round((lo[axis] + hi[axis]) / 2, 1), []).append((lo, hi))
        for planks in walls.values():
            planks.sort(key=lambda e: e[0][along])
            plane = np.mean([(lo[axis] + hi[axis]) / 2 for lo, hi in planks])
            for (lo0, hi0), (lo1, hi1) in zip(planks, planks[1:]):
                a0, a1 = hi0[along], lo1[along]
                z0, z1 = max(lo0[2], lo1[2]) + 0.04, min(hi0[2], hi1[2]) - 0.04
                if not 0.005 < a1 - a0 < 0.08 or z1 - z0 < 0.3:
                    continue
                def at(t, z):
                    q = np.zeros(3)
                    q[axis], q[along], q[2] = plane, t, z
                    return q
                q = [at(a0, z0), at(a1, z0), at(a1, z1), at(a0, z1)]
                outward = np.zeros(3)
                outward[axis] = np.sign(plane - middle[axis])
                if np.dot(np.cross(q[1] - q[0], q[2] - q[0]), outward) < 0:
                    q = q[::-1]
                uv = np.zeros((3, 2))
                out.append((np.array([q[0], q[1], q[2]]), 5, uv, -1))
                out.append((np.array([q[0], q[2], q[3]]), 5, uv, -1))
                count += 1
    print(f'adria: {count} slits between planks')


def box_prism(out, x, y, half, z0, z1, tex, cls=0):
    """A thin upright square rod, its four sides wound outward, textured along its length."""
    corners = [(x - half, y - half), (x + half, y - half), (x + half, y + half), (x - half, y + half)]
    for k in range(4):
        (ax, ay), (bx, by) = corners[k], corners[(k + 1) % 4]
        q = [np.array(p) for p in ((ax, ay, z0), (bx, by, z0), (bx, by, z1), (ax, ay, z1))]
        uv = [(0.0, z0), (0.1, z0), (0.1, z1), (0.0, z1)]
        out.append((np.array([q[0], q[1], q[2]]), cls, np.array([uv[0], uv[1], uv[2]]), tex))
        out.append((np.array([q[0], q[2], q[3]]), cls, np.array([uv[0], uv[2], uv[3]]), tex))


# Adria's wares on her shelves, as the original art has them: flasks, bottles and jugs of glass and clay in its
# colours (each a base and a highlight, taken from the art's pixels; the game keeps them as they are, ART_COLOURS).
# Shapes: a profile of (radius, height) pairs, as fractions of the piece's widest radius and its height.
WARE_SHAPES = {
    'flask': (0.42, [(0.0, 0.0), (0.55, 0.0), (0.9, 0.12), (1.0, 0.3), (0.86, 0.5), (0.42, 0.63), (0.3, 0.86), (0.38, 0.93), (0.34, 1.0), (0.0, 1.0)]),
    'bottle': (0.22, [(0.0, 0.0), (1.0, 0.0), (1.0, 0.56), (0.8, 0.68), (0.36, 0.78), (0.32, 0.94), (0.4, 1.0), (0.0, 1.0)]),
    'jug': (0.38, [(0.0, 0.0), (0.7, 0.0), (0.95, 0.2), (1.0, 0.45), (0.86, 0.7), (0.55, 0.82), (0.6, 0.93), (0.64, 1.0), (0.0, 1.0)]),
}
WARE_COLOURS = {
    'yellow': ((50, 36, 13), (104, 76, 30)), 'green': ((18, 29, 18), (44, 62, 34)), 'red': ((50, 12, 9), (96, 30, 18)),
    'clay': ((42, 21, 10), (80, 44, 22)), 'blue': ((13, 15, 30), (34, 38, 62)),
}
# The shelves (their tops, the plaza's z) and what stands on each: (shape, colour, y, height).
WARE_SHELF_X = 104.42  # the middle of the shelves' depth
WARES = [
    (16.58, [('jug', 'blue', 291.18, 0.30), ('bottle', 'green', 291.62, 0.34)]),                  # over the door
    (15.66, [('flask', 'yellow', 291.05, 0.34)]),                                                 # beside it
    (16.58, [('jug', 'clay', 292.52, 0.32), ('bottle', 'green', 292.92, 0.36)]),                  # the middle shelves
    (15.89, [('flask', 'yellow', 292.84, 0.36), ('bottle', 'green', 293.22, 0.32), ('bottle', 'red', 293.44, 0.24)]),
    (15.18, [('flask', 'yellow', 292.78, 0.24), ('bottle', 'green', 293.12, 0.3), ('jug', 'red', 293.5, 0.3), ('bottle', 'green', 293.86, 0.26)]),
    (15.89, [('jug', 'blue', 294.36, 0.34), ('bottle', 'green', 294.76, 0.36)]),                  # past the brace
]
WARE_SEGMENTS = 8
_ware_textures = {}


def ware_texture(colour):
    """A small texture of a ware's colour: its highlight on the side towards the path, darker round the back and at
    the foot (rows top to bottom, u around the piece)."""
    if colour not in _ware_textures:
        base, light = (np.array(c, dtype=float) for c in WARE_COLOURS[colour])
        w, h = 16, 16
        u = (np.arange(w) + 0.5) / w
        t = 1.0 - (np.arange(h) + 0.5) / h  # height up the piece
        around = np.clip(1.0 - np.abs(((u - 0.875 + 0.5) % 1.0) - 0.5) / 0.18, 0, 1)  # the highlight, facing +x, -y
        shade = 0.75 + 0.25 * np.clip(t * 3, 0, 1)
        img = (base[None, None, :] * shade[:, None, None] + (light - base)[None, None, :] * around[None, :, None] * shade[:, None, None])
        _ware_textures[colour] = len(textures)
        textures.append(np.clip(img, 0, 255).astype(np.uint8))
    return _ware_textures[colour]


def lathe(out, shape, x, y, z, height, tex):
    """A ware turned on a lathe (WARE_SEGMENTS sides), standing at (x, y, z), its faces wound outward."""
    radius, profile = WARE_SHAPES[shape]
    radius *= height
    rings = [(r * radius, z + t * height, t) for r, t in profile]
    for k in range(len(rings) - 1):
        (r0, z0, t0), (r1, z1, t1) = rings[k], rings[k + 1]
        for s in range(WARE_SEGMENTS):
            a0, a1 = 2 * np.pi * s / WARE_SEGMENTS, 2 * np.pi * (s + 1) / WARE_SEGMENTS
            p = lambda r, zz, a: np.array([x + r * np.cos(a), y + r * np.sin(a), zz])
            uv = lambda i, t: (i / WARE_SEGMENTS, 0.02 + 0.96 * t)
            q = [p(r0, z0, a0), p(r0, z0, a1), p(r1, z1, a1), p(r1, z1, a0)]
            quv = [uv(s, t0), uv(s + 1, t0), uv(s + 1, t1), uv(s, t1)]
            # Going up the profile the radius may grow or shrink; a ring of radius 0 makes a fan (one triangle).
            if r0 > 0:
                out.append((np.array([q[0], q[1], q[2]]), ART_COLOURS, np.array([quv[0], quv[1], quv[2]]), tex))
            if r1 > 0:
                out.append((np.array([q[0], q[2], q[3]]), ART_COLOURS, np.array([quv[0], quv[2], quv[3]]), tex))
    return out


def adria_shack():
    out = adria_triangles(ADRIA_SHACK)
    v, _, faces = adria_obj()
    walls = {g for a, b, c, m, g in faces if m == 'grinzi' and g not in ADRIA_DOOR and g not in ADRIA_ROOF}
    inside = {g for a, b, c, m, g in faces if all(ADRIA_SHACK[k] <= x <= ADRIA_SHACK[k + 2] for k, x in enumerate(v[a[0]][:2]))}
    plank_slits(out, v, faces, walls & inside)
    for top, wares in WARES:
        for shape, colour, y, height in wares:
            lathe(out, shape, WARE_SHELF_X, y, top, height, ware_texture(colour))
    return out


def adria_cauldron():
    out = adria_triangles(ADRIA_CAULDRON)
    iron = adria_texture('02___Default')
    for x, y, z0, z1 in ADRIA_CHAINS:
        box_prism(out, x, y, ADRIA_CHAIN_HALF, z0, z1, iron, ART_COLOURS)
    return out


def main():
    models = []
    for name, box in PLAZA_BUILDINGS + [('church', None)]:
        tris = church_triangles() if name == 'church' else plaza_triangles(box)
        models.append((name, tris))
    models.append(('adria', adria_shack()))
    models.append(('cauldron', adria_cauldron()))
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
        print(f'{name}: {len(records)} triangles ({textured} textured), size {(hi - lo).round(2)}, from {lo.round(3)}')
    out.close()
    print(len(textures), 'textures; landmarks.bin', os.path.getsize(os.path.join(ns['ASSETS'], 'landmarks.bin')), 'bytes')

main()
