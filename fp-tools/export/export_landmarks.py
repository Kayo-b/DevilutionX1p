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
       4 rim (the sides of the church's cross windows, lit by the lamp the game puts behind each);
       + 0x40 (WOUND) where the triangle's winding is reliable (counter-clockwise seen from outside): the game takes its
       normal from it (the mausoleum's, built here), not from which side of the building's middle it lies.
Texture coordinates are the OBJ's (v up, repeating past 0..1).
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


# --- The mausoleum by the cows -----------------------------------------------------------------------------------
# Nexus 67266 has no model of it, so it is built here after the town's iso art (tiles x 48-50, y 18-21): a stone
# platform of two steps under a stone house, pilasters at its corners and the middles of its long sides, a cornice,
# an iron grate over its front (the gable end towards +y) with the dark inside behind it, a pitched roof of slate
# diamonds along y, overhanging, stone knobs along both eaves and an iron ring over each end of the ridge.
# Units: tiles across (x 0..3) and along (y 0..4) its platform, heights in tiles of the art (39.2 rows each), as
# measured on the art through the iso projection: the eaves at 2.42, the ridge at 3.22.
# Textures: the church's stone, the kit's paving, column stone and steel, and a slate made here (no one's art).
KIT_DIR = os.environ.get('FP_DUNGEON_PIECES_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/dungeon_pieces')
MAUSOLEUM_SOURCES = {
    'stone': (CHURCH_DIR, 'Steinwand_D1_Diffuse.jpg', 0.62, TEXTURE_SIZE), 'base': (CHURCH_DIR, 'Pavement_01_UV_H_CM_1.jpg', 0.7, TEXTURE_SIZE),
    'trim': (KIT_DIR, 'd_column_extr.tga', 0.68, TEXTURE_SIZE), 'iron': (KIT_DIR, 'Steel_02_UV_H_CM_1.jpg', 0.4, 64),
    'inside': (CHURCH_DIR, 'Steinwand_D1_Diffuse.jpg', 0.12, 32), 'under': (CHURCH_DIR, 'Steinwand_D1_Diffuse.jpg', 0.4, 128),
}
# The art's stone is a cool blue-grey: the textures' warm greys are tinted to it (the game's dusk cools them a little more).
MAUSOLEUM_TINT = np.array([0.88, 0.92, 1.08])
M_EAVE = 2.42
M_RIDGE = 3.22
M_ROOF = 0.07       # the roof's thickness
M_WALL = (0.35, 2.65, 0.35, 3.65, 0.2, 2.27)   # x0, x1, y0, y1, z0, z1
M_CORNICE = (0.27, 2.73, 0.27, 3.73, 2.27, 2.43)
M_ROOF_X = (0.15, 2.85)
M_ROOF_Y = (0.18, 3.82)
M_STEPS = ((0.0, 3.0, 0.0, 4.0, 0.0, 0.1), (0.18, 2.82, 0.18, 3.82, 0.1, 0.2))


def slate_texture(size=TEXTURE_SIZE, diamonds=6):
    """Slate diamonds, a repeat of diamonds x diamonds, each its own tone, lighter towards its top, dark joints."""
    rng = np.random.default_rng(67266)
    tones = 0.85 + 0.3 * rng.random((diamonds * 2, diamonds * 2))
    y, x = np.mgrid[0:size, 0:size] / size * diamonds
    a = x + y
    b = x - y
    ia = np.floor(a).astype(int)
    ib = np.floor(b).astype(int)
    fa = a - ia
    fb = b - ib
    edge = np.minimum.reduce([fa, 1 - fa, fb, 1 - fb])
    joint = np.clip(edge / 0.06, 0, 1)
    shade = 0.8 + 0.35 * (1 - (fa + (1 - fb)) / 2)  # lit from above: lighter at the diamond's top corner
    tone = tones[ia % (diamonds * 2), ib % (diamonds * 2)]
    base = np.array([136, 140, 168], dtype=float)  # blue-grey slate, in daylight (the game dims it to dusk)
    rgb = base[None, None, :] * (tone * shade * (0.35 + 0.65 * joint))[..., None]
    return np.clip(rgb, 0, 255).astype(np.uint8)


_generated = {}


def generated_index(name):
    """A texture of the mausoleum: one of MAUSOLEUM_SOURCES at its brightness, or the slate made here."""
    if name in _generated:
        return _generated[name]
    from PIL import Image
    if name == 'slate':
        rgb = slate_texture()
    else:
        directory, file, gain, size = MAUSOLEUM_SOURCES[name]
        im = Image.open(os.path.join(directory, file)).convert('RGB').resize((size, size), Image.LANCZOS)
        rgb = np.asarray(im, dtype=float) * gain
        if name != 'iron':
            grey = rgb.mean(axis=2, keepdims=True)
            rgb = (grey + 0.5 * (rgb - grey)) * MAUSOLEUM_TINT  # half its own colour, the rest the art's
        rgb = np.clip(rgb, 0, 255).astype(np.uint8)
    _generated[name] = len(textures)
    textures.append(rgb)
    return _generated[name]


def mausoleum_triangles():
    out = []
    WOUND = 0x40

    def poly(points, outward, material, cls=0, uv=None, scale=1.0):
        """A convex polygon facing `outward`, wound counter-clockwise seen from there, fanned into triangles."""
        q = [np.array(c, dtype=float) for c in points]
        n = np.cross(q[1] - q[0], q[2] - q[0])
        if np.dot(n, outward) < 0:
            q = q[::-1]
        if uv is None:
            axis = int(np.argmax(np.abs(outward)))
            if axis == 2:
                uv = lambda p: (p[0] / scale, p[1] / scale)
            else:
                along = 1 if axis == 0 else 0
                uv = lambda p, along=along: (p[along] / scale, p[2] / scale)
        tex = generated_index(material)
        for k in range(1, len(q) - 1):
            tri = [q[0], q[k], q[k + 1]]
            out.append((np.array(tri), cls | WOUND, np.array([uv(t) for t in tri]), tex))

    def box(x0, x1, y0, y1, z0, z1, material, top=True, bottom=False, scale=1.0, sides=(True, True, True, True)):
        if sides[0]:
            poly([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0), material, scale=scale)
        if sides[1]:
            poly([(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], (-1, 0, 0), material, scale=scale)
        if sides[2]:
            poly([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0), material, scale=scale)
        if sides[3]:
            poly([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0), material, scale=scale)
        if top:
            poly([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1), material, scale=scale)
        if bottom:
            poly([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1), material, scale=scale)

    def sphere(c, r, material, segments=8, rings=5):
        c = np.array(c, dtype=float)
        at = lambda i, j: c + r * np.array([np.sin(np.pi * j / rings) * np.cos(2 * np.pi * i / segments), np.sin(np.pi * j / rings) * np.sin(2 * np.pi * i / segments), np.cos(np.pi * j / rings)])
        for j in range(rings):
            for i in range(segments):
                pts = [at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)]
                if j == 0:
                    pts = [pts[0], pts[2], pts[3]]
                elif j == rings - 1:
                    pts = [pts[0], pts[1], pts[2]]
                mid = np.mean(pts, axis=0)
                poly(pts, mid - c, material, uv=lambda p: (p[0] * 4, p[2] * 4))

    def ring(c, radius, thickness, material, segments=14, sides=6):
        """A ring standing in the x-z plane (facing along y), its middle at c."""
        c = np.array(c, dtype=float)
        def at(i, j):
            u = 2 * np.pi * i / segments
            v = 2 * np.pi * j / sides
            centre = c + radius * np.array([np.cos(u), 0, np.sin(u)])
            out_dir = np.array([np.cos(u), 0, np.sin(u)])
            return centre + thickness * (np.cos(v) * out_dir + np.sin(v) * np.array([0, 1, 0])), centre
        for i in range(segments):
            for j in range(sides):
                (a, ca), (b, _), (d, _), (e, _) = at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)
                mid = (a + b + d + e) / 4
                core = c + radius * (mid - c) * np.array([1, 0, 1]) / max(np.linalg.norm((mid - c) * np.array([1, 0, 1])), 1e-6)
                poly([a, b, d, e], mid - core, material, uv=lambda p: (p[0] * 4, p[2] * 4))

    # The platform, two steps of paving.
    for step in M_STEPS:
        box(*step, 'base')
    # The walls, and the dark inside behind the grate over the front.
    wx0, wx1, wy0, wy1, wz0, wz1 = M_WALL
    gx0, gx1, gz0, gz1 = 0.72, 2.28, wz0 + 0.04, 1.95
    box(wx0, wx1, wy0, wy1, wz0, wz1, 'stone', top=False, scale=1.2, sides=(True, True, False, True))
    # The front wall round the grate's opening.
    for x0, x1, z0, z1 in ((wx0, gx0, wz0, wz1), (gx1, wx1, wz0, wz1), (gx0, gx1, gz1, wz1)):
        poly([(x0, wy1, z0), (x1, wy1, z0), (x1, wy1, z1), (x0, wy1, z1)], (0, 1, 0), 'stone', scale=1.2)
    poly([(gx0, wy1 - 0.12, gz0), (gx1, wy1 - 0.12, gz0), (gx1, wy1 - 0.12, gz1), (gx0, wy1 - 0.12, gz1)], (0, 1, 0), 'inside', scale=1.2)
    for x in (gx0, gx1):  # its reveals
        poly([(x, wy1 - 0.12, gz0), (x, wy1, gz0), (x, wy1, gz1), (x, wy1 - 0.12, gz1)], (1 if x == gx0 else -1, 0, 0), 'stone', scale=1.2)
    poly([(gx0, wy1 - 0.12, gz1), (gx1, wy1 - 0.12, gz1), (gx1, wy1, gz1), (gx0, wy1, gz1)], (0, 0, -1), 'stone', scale=1.2)
    poly([(gx0, wy1 - 0.12, gz0), (gx1, wy1 - 0.12, gz0), (gx1, wy1, gz0), (gx0, wy1, gz0)], (0, 0, 1), 'base')
    # The grate: bars, three rails.
    bars = 12
    for k in range(bars + 1):
        x = gx0 + 0.06 + (gx1 - gx0 - 0.12) * k / bars
        box(x - 0.016, x + 0.016, wy1 - 0.07, wy1 - 0.035, gz0, gz1, 'iron')
    for z in (0.95, 1.6, 1.88):
        box(gx0, gx1, wy1 - 0.075, wy1 - 0.03, z - 0.02, z + 0.02, 'iron', bottom=True)
    # Between the upper rails a band of iron curls, as the art draws over the grate.
    curls = 6
    for k in range(curls):
        x = gx0 + (gx1 - gx0) * (k + 0.5) / curls
        ring((x, wy1 - 0.05, 1.74), 0.1, 0.013, 'iron', segments=10, sides=4)
    # Pilasters: the corners, the middles of the long sides, either side of the grate.
    pw = 0.15
    for x, y in ((wx0, wy0), (wx1, wy0), (wx0, wy1), (wx1, wy1)):
        box(x - pw, x + pw, y - pw, y + pw, wz0, wz1, 'trim')
    for x in (wx0, wx1):
        box(x - 0.08, x + 0.08, 2.0 - 0.14, 2.0 + 0.14, wz0, wz1, 'trim')
    for x in (gx0 - 0.08, gx1 + 0.08):
        box(x - 0.08, x + 0.08, wy1, wy1 + 0.06, wz0, wz1, 'trim')
    # The cornice round the top of the walls.
    box(*M_CORNICE, 'trim', top=True, bottom=True)
    # The gables, from the cornice up under the roof.
    slope = (M_RIDGE - M_EAVE) / (1.5 - M_ROOF_X[0])
    under = lambda x: M_EAVE - M_ROOF + slope * (min(x, 3 - x) - M_ROOF_X[0])
    for y, facing in ((wy0, -1), (wy1, 1)):
        poly([(wx0, y, M_CORNICE[5]), (wx1, y, M_CORNICE[5]), (1.5, y, under(1.5) + 0.01)], (0, facing, 0), 'stone', scale=1.2)
        # Its frame: a moulding up each raking edge, standing out of the gable.
        depth = 0.05 * facing
        for x0, x1 in ((wx0, 1.5), (wx1, 1.5)):
            z0 = under(x0) - 0.01
            z1 = under(x1) - 0.01
            dz = 0.09
            face_y = y + depth
            poly([(x0, face_y, z0 - dz), (x1, face_y, z1 - dz), (x1, face_y, z1), (x0, face_y, z0)], (0, facing, 0), 'trim')
            poly([(x0, y, z0 - dz), (x1, y, z1 - dz), (x1, face_y, z1 - dz), (x0, face_y, z0 - dz)], (0, 0, -1), 'trim')
        # A row of dark rosettes in the pediment.
        for k in range(5):
            x = 1.5 + (k - 2) * 0.28
            z = M_CORNICE[5] + 0.14 + (0.18 if k == 2 else (0.08 if k in (1, 3) else 0.0))
            r = 0.045
            poly([(x - r, y + 0.01 * facing, z - r), (x + r, y + 0.01 * facing, z - r), (x + r, y + 0.01 * facing, z + r), (x - r, y + 0.01 * facing, z + r)], (0, facing, 0), 'inside')
    # The roof: two slabs of slate, their undersides, the fascia along the eaves and their ends.
    ry0, ry1 = M_ROOF_Y
    for side in (-1, 1):
        xe = M_ROOF_X[0] if side < 0 else M_ROOF_X[1]
        top = [(xe, ry0, M_EAVE), (1.5, ry0, M_RIDGE), (1.5, ry1, M_RIDGE), (xe, ry1, M_EAVE)]
        length = np.hypot(1.5 - M_ROOF_X[0], M_RIDGE - M_EAVE)
        slat = lambda p, xe=xe: (p[1], abs(p[0] - xe) / (1.5 - M_ROOF_X[0]) * length)
        poly(top, (side * slope, 0, 1), 'slate', cls=1, uv=slat)
        bottom = [(x, y, z - M_ROOF) for x, y, z in top]
        poly(bottom, (-side * slope, 0, -1), 'under', cls=1, uv=slat)
        poly([(xe, ry0, M_EAVE - M_ROOF), (xe, ry1, M_EAVE - M_ROOF), (xe, ry1, M_EAVE), (xe, ry0, M_EAVE)], (side, 0, 0), 'trim')
        for y, facing in ((ry0, -1), (ry1, 1)):
            poly([(xe, y, M_EAVE - M_ROOF), (1.5, y, M_RIDGE - M_ROOF), (1.5, y, M_RIDGE), (xe, y, M_EAVE)], (0, facing, 0), 'trim')
    # The ridge's roll, the knobs along the eaves, the rings over the ridge's ends.
    box(1.44, 1.56, ry0 - 0.02, ry1 + 0.02, M_RIDGE - 0.04, M_RIDGE + 0.05, 'trim', bottom=False)
    for k in range(7):
        y = 0.45 + k * 0.5
        for x in (M_ROOF_X[0] + 0.07, M_ROOF_X[1] - 0.07):
            box(x - 0.025, x + 0.025, y - 0.025, y + 0.025, M_EAVE - 0.01, M_EAVE + 0.04, 'trim', top=False)
            sphere((x, y, M_EAVE + 0.095), 0.065, 'trim')
    for y in (ry0 + 0.04, ry1 - 0.04):
        ring((1.5, y, M_RIDGE + 0.05 + 0.15), 0.15, 0.032, 'iron')
    return out


def main():
    models = []
    for name, box in PLAZA_BUILDINGS + [('church', None), ('mausoleum', None)]:
        tris = church_triangles() if name == 'church' else (mausoleum_triangles() if name == 'mausoleum' else plaza_triangles(box))
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
