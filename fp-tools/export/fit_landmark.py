#!/usr/bin/env python3
"""Fit 3D models of town buildings to the original iso art, by silhouette overlap.

The art comes from the game: dump the town's upright art (no ground) once, from the console or headless:
    lua:dev.fp.dumpiso(0, 0, 95, 95, '<out>/town_up.ppm', true)

A model point (x, y, z) in source units is placed in the town as
    (x', y') = the point after the variant's flips and swap (around the origin)
    world x = scale * x' + tx,  world y = scale * y' + ty,  world z = (z - ground) * zscale
and drawn into the art's frame with the game's iso projection:
    screen x = (x - y) * 32,  screen y = (x + y) * 16 - z * 39.2

Modes:
  global   one transform for a whole set of models (the plaza holds every house in its town layout)
  single   refine one building, starting from a transform

Prints the transform and the landmark placement (Source/fp/fp_buildings.cpp, Landmarks), and writes an overlay
image (model outline over the art) to check by eye.
"""
import argparse, itertools, json, os, sys
import numpy as np
from PIL import Image, ImageDraw

HEIGHT_PIXELS = 39.2


def load_obj(path, keep):
    """Triangles of the OBJ groups for which keep(group, material) is true: (n, 3, 3) array and group names."""
    verts, tris, names, cur, mat = [], [], [], None, None
    for line in open(path):
        if line.startswith('v '):
            verts.append(tuple(map(float, line.split()[1:4])))
        elif line.startswith('g '):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith('usemtl '):
            mat = line.split()[1]
        elif line.startswith('f ') and keep(cur, mat):
            idx = [int(t.split('/')[0]) for t in line.split()[1:]]
            idx = [i - 1 if i > 0 else len(verts) + i for i in idx]
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1]))
                names.append(cur)
    v = np.array(verts)
    return v[np.array(tris)], names


class Art:
    def __init__(self, path, region, rows=256, scale=4):
        x0, y0, x1, y1 = region
        img = np.asarray(Image.open(path).convert('L')) > 0
        self.full = Image.open(path).convert('RGB')
        self.minx = (x0 - y1) * 32 - 32
        self.miny = (x0 + y0 + 2) * 16 - (rows - 1)
        self.s = scale
        h, w = img.shape
        self.mask = np.asarray(Image.fromarray(img.astype(np.uint8) * 255).resize((w // scale, h // scale))) > 127

    def screen(self, wx, wy, wz):
        return ((wx - wy) * 32 - self.minx) / self.s, ((wx + wy) * 16 - wz * HEIGHT_PIXELS - self.miny) / self.s


def variant_xy(p, variant):
    fx, fy, swap = variant
    x, y = p[..., 0].copy(), p[..., 1].copy()
    if fx:
        x = -x
    if fy:
        y = -y
    if swap:
        x, y = y, x
    return x, y


def place(tris, variant, t, ground):
    scale, tx, ty, zs = t
    x, y = variant_xy(tris, variant)
    return scale * x + tx, scale * y + ty, (tris[..., 2] - ground) * zs


def silhouette(art, tris, variant, t, ground):
    wx, wy, wz = place(tris, variant, t, ground)
    sx, sy = art.screen(wx, wy, wz)
    im = Image.new('L', (art.mask.shape[1], art.mask.shape[0]))
    d = ImageDraw.Draw(im)
    for tri in np.stack([sx, sy], -1):
        d.polygon([tuple(pt) for pt in tri], fill=255)
    return np.asarray(im) > 0


def score(art, tris, variant, t, ground, roi):
    s = silhouette(art, tris, variant, t, ground)[roi]
    a = art.mask[roi]
    return (a & s).sum() / max(1, (a | s).sum())


def roi_for(art, tris, variant, t, ground, margin=40):
    s = silhouette(art, tris, variant, t, ground)
    ys, xs = np.where(s)
    return (slice(max(0, ys.min() - margin), ys.max() + margin), slice(max(0, xs.min() - margin), xs.max() + margin))


def refine(art, tris, variant, t, ground, roi, steps, rounds=12, fixed=()):
    t = list(t)
    best = score(art, tris, variant, t, ground, roi)
    steps = list(steps)
    for _ in range(rounds):
        improved = False
        for i in range(4):
            if i in fixed:
                continue
            for d in (-steps[i], steps[i]):
                q = t.copy()
                q[i] += d
                sc = score(art, tris, variant, q, ground, roi)
                if sc > best:
                    best, t, improved = sc, q, True
        if not improved:
            steps = [s / 2 for s in steps]
    return t, best


def overlay(art, tris, variant, t, ground, path, crop=None):
    wx, wy, wz = place(tris, variant, t, ground)
    sx, sy = (wx - wy) * 32 - art.minx, (wx + wy) * 16 - wz * HEIGHT_PIXELS - art.miny
    ov = Image.new('RGB', art.full.size)
    d = ImageDraw.Draw(ov)
    for tri in np.stack([sx, sy], -1):
        d.polygon([tuple(pt) for pt in tri], outline=(80, 255, 255))
    im = Image.blend(art.full, ov, 0.45)
    if crop is not None:
        ys, xs = crop
        im = im.crop((xs.start * art.s, ys.start * art.s, xs.stop * art.s, ys.stop * art.s))
    im.save(path)


def placement(tris, variant, t, ground):
    """The Landmarks entry: world extent of the model's source x and y range, and the flips that map onto it."""
    lo = tris.reshape(-1, 3).min(0)
    hi = tris.reshape(-1, 3).max(0)
    corners = np.array([[lo[0], lo[1], ground], [hi[0], hi[1], ground]])
    wx, wy, _ = place(corners, variant, t, ground)
    fx, fy, swap = variant
    # u runs over source x, w over source y (export_landmarks.py). Without a swap, u -> world x and w -> world y.
    return {'x0': float(min(wx)), 'x1': float(max(wx)), 'y0': float(min(wy)), 'y1': float(max(wy)), 'zScale': float(t[3]),
            'flipU': bool(fx), 'flipW': bool(fy), 'swap': bool(swap)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('obj')
    ap.add_argument('art', help='upright-only iso dump (PPM)')
    ap.add_argument('--region', default='0,0,95,95', help='tile rectangle of the dump: x0,y0,x1,y1')
    ap.add_argument('--groups', default='', help='comma-separated OBJ groups to fit (default: all)')
    ap.add_argument('--bbox', default='', help='or: all groups inside this source box x0,y0,x1,y1')
    ap.add_argument('--ground', type=float, default=14.6, help='source height of the ground')
    ap.add_argument('--init', default='1,0,0,1', help='scale,tx,ty,zscale to start from')
    ap.add_argument('--variants', default='all', help="'all' or e.g. 0,1,0 (flip x, flip y, swap)")
    ap.add_argument('--search', type=float, default=0, help='coarse search of tx, ty over +- this many tiles first')
    ap.add_argument('--fix-scale', action='store_true')
    ap.add_argument('--margin', type=int, default=200, help='scored region around the model, in art pixels / 4 (use ~12 for one building)')
    ap.add_argument('--steps', default='0.05,0.5,0.5,0.1', help='first search steps for scale, tx, ty, zscale')
    ap.add_argument('--overlay', default='fit_overlay.png')
    args = ap.parse_args()

    groups = set(g for g in args.groups.split(',') if g)
    box = [float(v) for v in args.bbox.split(',')] if args.bbox else None
    if box:
        # Group extents first, to pick the groups that lie inside the box.
        allt, names = load_obj(args.obj, lambda g, m: True)
        ext = {}
        for tri, n in zip(allt, names):
            lo, hi = tri.min(0), tri.max(0)
            e = ext.get(n)
            ext[n] = (np.minimum(e[0], lo), np.maximum(e[1], hi)) if e else (lo, hi)
        groups |= {n for n, (lo, hi) in ext.items() if lo[0] >= box[0] and hi[0] <= box[2] and lo[1] >= box[1] and hi[1] <= box[3]}
    tris, _ = load_obj(args.obj, (lambda g, m: g in groups) if groups else (lambda g, m: True))
    tris = tris[tris[..., 2].max(-1) > args.ground + 0.05]  # nothing below ground
    art = Art(args.art, [int(v) for v in args.region.split(',')])
    t0 = [float(v) for v in args.init.split(',')]
    variants = list(itertools.product((False, True), repeat=3)) if args.variants == 'all' else [tuple(bool(int(v)) for v in args.variants.split(','))]
    fixed = (0,) if args.fix_scale else ()
    results = []
    for var in variants:
        t = list(t0)
        roi = roi_for(art, tris, var, t, args.ground, args.margin)
        if args.search > 0:
            best = -1
            r = np.arange(-args.search, args.search + 0.01, 1.0)
            for dx in r:
                for dy in r:
                    q = [t0[0], t0[1] + dx, t0[2] + dy, t0[3]]
                    sc = score(art, tris, var, q, args.ground, roi)
                    if sc > best:
                        best, t = sc, q
        t, sc = refine(art, tris, var, t, args.ground, roi, [float(v) for v in args.steps.split(',')], fixed=fixed)
        results.append((sc, var, t))
        print(f'variant {var}: iou {sc:.3f} transform {[round(v, 3) for v in t]}', file=sys.stderr)
    sc, var, t = max(results)
    crop = roi_for(art, tris, var, t, args.ground, 60)
    overlay(art, tris, var, t, args.ground, args.overlay, crop)
    print(json.dumps({'iou': round(float(sc), 3), 'variant': var, 'transform': [round(v, 4) for v in t], 'placement': placement(tris, var, t, args.ground)}))


if __name__ == '__main__':
    main()
