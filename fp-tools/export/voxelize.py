#!/usr/bin/env python3
"""Voxelize houses of the Tristram plaza OBJ: surface voxels coloured from the model textures, mapped to the Diablo palette.

usage: voxelize.py <house_walls_group> <house_roof_group> <out_prefix> [voxel_size]
"""
import sys, os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# The extracted models live outside the repo; source and terms (free, non-commercial use): assets/fp/CREDITS.md.
FP_PLAZA_DIR = os.environ.get('FP_PLAZA_DIR', '/home/kxyx/projects/diablo-hellfire-fp/research/tristram_plaza')
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
Image.MAX_IMAGE_PIXELS = None

TEXTURES = {
    'stone_wall': 'wall_diff.png', 'straw_roof_ext': 'str_roof_dif.tga', 'Roof_straw': 'str_roof_dif.tga',
    'straw_roof_int': 'str_roof_dif.tga', 'roof_int': 'str_roof_dif.tga', 'door': 'door_diff.tga',
}
_tex = {}

def texture(name):
    f = TEXTURES.get(name)
    if f is None:
        return None
    if f not in _tex:
        im = Image.open(os.path.join(FP_PLAZA_DIR, f)).convert('RGB')
        im.thumbnail((1024, 1024))
        _tex[f] = np.asarray(im, dtype=np.float32)
    return _tex[f]

def sample(tex, uv):
    h, w, _ = tex.shape
    u = np.mod(uv[:, 0], 1.0)
    v = 1.0 - np.mod(uv[:, 1], 1.0)
    return tex[np.clip((v * h).astype(int), 0, h - 1), np.clip((u * w).astype(int), 0, w - 1)]

def load(groups):
    verts, uvs = [], []
    tris = []  # (v0,v1,v2, t0,t1,t2, material)
    cur, mat = None, None
    for line in open(os.path.join(FP_PLAZA_DIR, 'Tristram_plaza.obj')):
        if line.startswith('v '):
            verts.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith('vt '):
            uvs.append(tuple(map(float, line.split()[1:3])))
        elif line.startswith('g '):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith('usemtl '):
            mat = line.split()[1]
        elif line.startswith('f ') and cur in groups:
            idx = []
            for tok in line.split()[1:]:
                p = tok.split('/')
                vi = int(p[0]); vi = vi - 1 if vi > 0 else len(verts) + vi
                ti = None
                if len(p) > 1 and p[1]:
                    ti = int(p[1]); ti = ti - 1 if ti > 0 else len(uvs) + ti
                idx.append((vi, ti))
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1], mat, cur))
    return np.array(verts, dtype=np.float64), np.array(uvs, dtype=np.float64), tris

def diablo_palette():
    path = os.path.join(FP_PLAZA_DIR, 'town.pal')
    raw = np.frombuffer(open(path, 'rb').read(), dtype=np.uint8).reshape(256, 3).astype(np.float32)
    return raw

def voxelize(walls, roof, vs):
    verts, uvs, tris = load({walls, roof})
    lo = verts[[t[0][0] for t in tris]].min(axis=0)
    cells = {}
    for (a, b, c, mat, grp) in tris:
        p = [verts[a[0]], verts[b[0]], verts[c[0]]]
        t = [uvs[x[1]] if x[1] is not None and len(uvs) else np.zeros(2) for x in (a, b, c)]
        e1, e2 = p[1] - p[0], p[2] - p[0]
        n = max(2, int(np.ceil(max(np.linalg.norm(e1), np.linalg.norm(e2)) / (vs * 0.45))))
        us, ws = np.meshgrid(np.linspace(0, 1, n + 1), np.linspace(0, 1, n + 1))
        m = us + ws <= 1.0
        us, ws = us[m], ws[m]
        pts = p[0] + np.outer(us, e1) + np.outer(ws, e2)
        uv = t[0] + np.outer(us, t[1] - t[0]) + np.outer(ws, t[2] - t[0])
        tex = texture(mat)
        col = sample(tex, uv) if tex is not None else np.tile(np.array([90, 70, 50], np.float32), (len(pts), 1))
        ij = np.floor((pts - lo) / vs).astype(int)
        for key, c3 in zip(map(tuple, ij), col):
            s = cells.get(key)
            if s is None:
                cells[key] = [c3.copy(), 1, grp == roof]
            else:
                s[0] += c3
                s[1] += 1
    keys = np.array(list(cells.keys()))
    colors = np.array([cells[tuple(k)][0] / cells[tuple(k)][1] for k in keys])
    isroof = np.array([cells[tuple(k)][2] for k in keys])
    return keys, colors, isroof

def quantize(colors, pal, light):
    # Diablo town palette ramp (skip the translucent/reserved ends); match the game's dim, low-contrast look.
    c = colors * light
    d = ((c[:, None, :] - pal[None, :, :]) ** 2).sum(axis=2)
    return pal[d.argmin(axis=1)]

def render(keys, colors, view_yaw, view_pitch, scale, out_path):
    """Z-buffered splat render: camera looks at the model centre from yaw (about Z, degrees) and pitch above horizontal."""
    k = keys.astype(np.float64)
    k -= k.mean(axis=0)
    yaw, pit = np.radians(view_yaw), np.radians(view_pitch)
    # rotate about Z by yaw
    x = k[:, 0] * np.cos(yaw) - k[:, 1] * np.sin(yaw)
    y = k[:, 0] * np.sin(yaw) + k[:, 1] * np.cos(yaw)
    z = k[:, 2]
    # right = x, depth axis = y (away from camera), up tilted by pitch
    sx = x
    sy = z * np.cos(pit) + y * np.sin(pit)   # up on screen
    dep = y * np.cos(pit) - z * np.sin(pit)  # depth (bigger = farther)
    W = int((sx.max() - sx.min() + 6) * scale)
    H = int((sy.max() - sy.min() + 6) * scale)
    img = np.full((H, W, 3), 30, np.uint8)
    zb = np.full((H, W), 1e9)
    px = ((sx - sx.min() + 3) * scale).astype(int)
    py = ((sy.max() - sy + 3) * scale).astype(int)
    r = max(1, int(round(scale * 0.8)))
    for i in np.argsort(-dep):
        x0, y0 = px[i], py[i]
        for dy in range(r):
            for dx in range(r):
                xx, yy = x0 + dx, y0 + dy
                if 0 <= xx < W and 0 <= yy < H and dep[i] < zb[yy, xx]:
                    zb[yy, xx] = dep[i]
                    img[yy, xx] = np.clip(colors[i], 0, 255)
    Image.fromarray(img).save(out_path)

def main():
    walls, roof, out = sys.argv[1], sys.argv[2], sys.argv[3]
    vs = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
    keys, colors, isroof = voxelize(walls, roof, vs)
    print(f'{len(keys)} surface voxels, grid {keys.max(axis=0) - keys.min(axis=0) + 1}')
    pal = diablo_palette()
    q = quantize(colors, pal, 0.55)
    np.savez_compressed(out + '.npz', keys=keys, colors=q, isroof=isroof, voxel=vs)
    # Diablo iso: camera looks along -x/-y with 30 degrees elevation -> yaw 45
    for name, yaw in (('iso', 45), ('front', 0), ('side', 90), ('back', 225)):
        render(keys, q, yaw, 30, 6, f'{out}_{name}.png')
    render(keys, colors, 45, 30, 6, f'{out}_iso_truecolor.png')

main()
