#!/usr/bin/env python3
"""Export a night skybox (six cube faces) as the town's first-person sky, `assets/fp/sky.bin`.

usage: export_sky.py [set] [face size]   (default d1e1m6, 1024)

Source: FP_SKY_DIR (default ~/projects/DevilutionX1p/skybox-night), six PNG files per set, named <set>{ft,lf,bk,rt,up,dn}
(2048 x 2048, no extension). Looking out from inside, turning right goes ft, lf, bk, rt; the first person view
puts ft at +x, lf at +y, bk at -x and rt at -y (world z up). The up face's columns run along +x and its rows along +y.
GZDoom skybox sets are read too, named <set>{N,E,S,W,U,D}.png: turning right goes N, E, S, W (taken as ft, lf, bk,
rt), and their up face lies transposed against the Quake-style one (found by matching its edges to the sides' tops).

A small source (BTSX's faces are 256 px) is upscaled without its stars, which are drawn back in afterwards as fine
points: blown up with the rest they would be blurred blobs.

FP_SKY_ERASE paints things out of a GZDoom set's faces, each an ellipse '<face>:cx,cy,rx,ry' in its source pixels
(';' between them; the side faces are painted as one strip round the horizon, so an ellipse may run over onto the
next face), filled with the patch of sky that best matches the sky round it, its edge feathered. FP_SKY_SATURATION and FP_SKY_GAMMA grade the sky towards black: less colour, and its
darker parts darker still.

The town's sky (assets/fp/sky.bin) is SevBox's BTSX sky 1 without its ringed planet and moon, nearly black:
  FP_SKY_DIR=~/Downloads/SevBox-BTSXv1.0.1/skybox/btsx1 FP_SKY_ERASE='S:175,64,11,11;S:232,66,54,46' \
    FP_SKY_GAIN=0.3 FP_SKY_SATURATION=0.5 FP_SKY_GAMMA=2.2 FP_SKY_HAZE=0.3 export_sky.py 1 1024

Only what can be seen above the town's ground is kept: the upper half of each side face (and a margin below the
horizon) and the up face; the down face is left out. The faces are brightened a little (the source is very dark),
hazed towards the town's fog colour near the horizon, and quantized to the town palette.

Binary format (little endian):
  4s magic 'FPS1', u16 size, u16 sideRows (rows kept of each side face, from the top),
  768 bytes: the palette the indices refer to (town.pal; the game maps them to its own palette when loading),
  u32 compressed size, then zlib data: ft, lf, bk, rt (sideRows x size each), up (size x size); one byte per texel.
"""
import os, sys, struct, zlib
import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
SKY_DIR = os.environ.get('FP_SKY_DIR', os.path.expanduser('~/projects/DevilutionX1p/skybox-night'))
TOWN_PAL = os.environ.get('FP_TOWN_PAL', os.path.expanduser('~/projects/diablo-hellfire-fp/research/tristram_plaza/town.pal'))
SET = sys.argv[1] if len(sys.argv) > 1 else 'd1e1m6'
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
SIDE_ROWS = SIZE // 2 + SIZE // 16  # the horizon is the middle row; a margin below it for looking down at the map's edge

GAIN = float(os.environ.get('FP_SKY_GAIN', 3.2))  # 3.2: d1e1m6 is nearly black; below 1 darkens a bright sky
FOG = np.array([13.0, 17.0, 27.0])     # the town's fog and old sky colour (palette 190, PAL16_BLUE + 14)
ZENITH = np.array([4.0, 6.0, 11.0])    # the dark sky overhead
SATURATION = float(os.environ.get('FP_SKY_SATURATION', 1.0))  # the sky's colour kept (0: grey)
GAMMA = float(os.environ.get('FP_SKY_GAMMA', 1.0))  # above 1: the dark parts of the sky darker, towards black
HAZE_AT_HORIZON = float(os.environ.get('FP_SKY_HAZE', 0.55))  # how much of the fog colour lies over the horizon, fading out by HAZE_TOP
HAZE_TOP = 0.22                        # elevation (tan) where the haze is gone

pal = np.frombuffer(open(TOWN_PAL, 'rb').read(), dtype=np.uint8).reshape(256, 3).astype(float)


BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 - 0.5
DITHER = 5.0  # amplitude of the ordered dither (RGB units), against banding in the dark gradient


def quantize(rgb):
    """Nearest palette colour (eye weights 3, 6, 1; colour 0 left out), with a light ordered dither."""
    h, w, _ = rgb.shape
    offset = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w, None] * DITHER
    q = np.clip(rgb + offset, 0, 255).astype(int) >> 2
    key = (q[..., 0] << 12) | (q[..., 1] << 6) | q[..., 2]
    unique, inverse = np.unique(key, return_inverse=True)
    colours = np.stack([(unique >> 12) & 63, (unique >> 6) & 63, unique & 63], -1) * 4 + 2
    weights = np.array([3.0, 6.0, 1.0])
    best = np.empty(len(unique), np.int64)
    for i in range(0, len(unique), 4096):
        d = (((colours[i:i + 4096, None, :] - pal[None, 1:, :]) ** 2) * weights).sum(-1)
        best[i:i + 4096] = d.argmin(1) + 1
    return best[inverse].reshape(h, w).astype(np.uint8)


def elevation_side(rows, size):
    """tan(elevation) of each row of a side face, at the face's middle column."""
    v = (np.arange(rows) + 0.5) / size * 2 - 1
    return -v


def desaturate(rgb):
    grey = (rgb @ np.array([0.3, 0.59, 0.11]))[..., None]
    return grey + (rgb - grey) * SATURATION


def grade(img, stars, tan_elev):
    """Brightened sky over a gradient from the haze at the horizon to the dark zenith, its stars over it."""
    t = np.clip(tan_elev, 0, None)[..., None]
    base = FOG + (ZENITH - FOG) * np.clip(t / 1.2, 0, 1)
    img = 255.0 * (np.clip(desaturate(img), 0, 255) / 255.0) ** GAMMA
    sky = base + img * GAIN + desaturate(stars) * STAR_GAIN
    haze = HAZE_AT_HORIZON * np.clip(1 - t / HAZE_TOP, 0, 1)
    return sky * (1 - haze) + FOG * haze


GZDOOM_FACES = {'ft': 'N', 'lf': 'E', 'bk': 'S', 'rt': 'W', 'up': 'U'}
STAR_GAIN = float(os.environ.get('FP_SKY_STAR_GAIN', 0.9))  # the redrawn stars' brightness (the sky's is GAIN)
STAR_ABOVE = 16.0  # how much brighter than the sky around it a source pixel is to be a star


def upscale(im):
    """A face blown up to SIZE: its sky smoothly, and its stars (small bright spots) as points over it, apart."""
    src = np.asarray(im).astype(float)
    if im.width * 2 > SIZE:
        return np.asarray(im.resize((SIZE, SIZE), Image.LANCZOS)).astype(float), np.zeros((SIZE, SIZE, 3))
    sky = np.asarray(im.filter(ImageFilter.MedianFilter(7))).astype(float)
    lum = src @ np.array([0.3, 0.59, 0.11])
    star = lum - sky @ np.array([0.3, 0.59, 0.11]) > STAR_ABOVE
    plain = np.where(star[..., None], sky, src)
    out = np.asarray(Image.fromarray(plain.astype(np.uint8)).resize((SIZE, SIZE), Image.BICUBIC)).astype(float)
    points = np.zeros((SIZE, SIZE, 3))
    scale = SIZE / im.width
    h, w = star.shape
    seen = np.zeros_like(star)
    for y0, x0 in zip(*np.nonzero(star)):
        if seen[y0, x0]:
            continue
        # The star's pixels: its light over the sky's, and where its middle is.
        stack = [(y0, x0)]
        seen[y0, x0] = True
        pixels = []
        while stack:
            y, x = stack.pop()
            pixels.append((y, x))
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                ny, nx = y + dy, x + dx
                if 0 <= ny < h and 0 <= nx < w and star[ny, nx] and not seen[ny, nx]:
                    seen[ny, nx] = True
                    stack.append((ny, nx))
        light = np.array([src[y, x] - sky[y, x] for y, x in pixels]).clip(0)
        flux = light.sum(0)
        weight = light.sum(1) + 1e-6
        cy = sum(wt * (y + 0.5) for wt, (y, x) in zip(weight, pixels)) / weight.sum()
        cx = sum(wt * (x + 0.5) for wt, (y, x) in zip(weight, pixels)) / weight.sum()
        peak = light.max(0)
        # One texel for a faint star, a small cross for a bright one.
        px, py = int(cx * scale), int(cy * scale)
        points[py % SIZE, px % SIZE] += peak
        if flux.sum() > 3 * 255:
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                points[(py + dy) % SIZE, (px + dx) % SIZE] += peak * 0.35
    return out, points


def erase(src, letter, wrap):
    """FP_SKY_ERASE's ellipses on a face (or the strip of the side faces, wrap) painted out with the sky elsewhere."""
    h, w, _ = src.shape
    yy, xx = np.mgrid[0:h, 0:w]
    out = src.copy()
    for entry in filter(None, os.environ.get('FP_SKY_ERASE', '').split(';')):
        face, numbers = entry.split(':')
        cx, cy, rx, ry = (float(n) for n in numbers.split(','))
        if wrap and face in 'NESW':
            cx += 'NESW'.index(face) * (w // 4)
        elif face != letter:
            continue
        ex = (xx + 0.5 - cx + w / 2) % w - w / 2 if wrap else xx + 0.5 - cx
        r = np.sqrt((ex / rx) ** 2 + ((yy + 0.5 - cy) / ry) ** 2)
        mask = np.clip((1.15 - r) / 0.15, 0, 1)[..., None]  # feathered over the outer part
        # The patch to fill with: of the shifts that keep it on the face and clear of the ellipse, the one whose
        # sky round the ellipse's edge (a ring just outside it, blurred) best matches what is there.
        ring = (r > 1.0) & (r < 1.4)
        smooth = np.asarray(Image.fromarray(out.astype(np.uint8)).filter(ImageFilter.GaussianBlur(4))).astype(float)
        inside = np.nonzero(r < 1.4)
        best = None
        for dy in range(-140, 141, 4):
            for dx in range(-200, 201, 4):
                sy, sx = inside[0] + dy, inside[1] + dx
                if sy.min() < 0 or sy.max() >= h or (not wrap and (sx.min() < 0 or sx.max() >= w)):
                    continue
                sex = (sx + 0.5 - cx + w / 2) % w - w / 2 if wrap else sx + 0.5 - cx
                if np.hypot(sex / rx, (sy + 0.5 - cy) / ry).min() < 1.4:
                    continue  # the patch must not take in what is being painted out
                score = np.abs(np.roll(smooth, (-dy, -dx), axis=(0, 1))[ring] - smooth[ring]).mean()
                if best is None or score < best[0]:
                    best = (score, dy, dx)
        fill = np.roll(out, (-best[1], -best[2]), axis=(0, 1))
        print(f'erase {entry}: filled from {best[2]:+d}, {best[1]:+d} (mismatch {best[0]:.1f})')
        out = out * (1 - mask) + fill * mask
    return out


SIDES = ('ft', 'lf', 'bk', 'rt')


def load_all():
    """Each face (ft, lf, bk, rt, up): its sky and its stars, at SIZE."""
    images = {}
    gzdoom = os.path.exists(os.path.join(SKY_DIR, SET + 'N.png'))
    for face in SIDES + ('up',):
        path = os.path.join(SKY_DIR, SET + GZDOOM_FACES[face] + '.png') if gzdoom else os.path.join(SKY_DIR, SET + face)
        images[face] = np.asarray(Image.open(path).convert('RGB')).astype(float)
    if gzdoom:
        # The side faces as one strip round the horizon, so what is painted out across a seam stays one piece.
        strip = erase(np.concatenate([images[f] for f in SIDES], axis=1), None, True)
        for k, face in enumerate(SIDES):
            images[face] = np.split(strip, 4, axis=1)[k]
        images['up'] = erase(images['up'], 'U', False)
    result = {}
    for face, rgb in images.items():
        im = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
        if gzdoom and face == 'up':
            im = im.transpose(Image.Transpose.TRANSPOSE)
        result[face] = upscale(im)
    return result


def main():
    faces = []
    loaded = load_all()
    for face in SIDES:
        img, stars = loaded[face]
        img, stars = img[:SIDE_ROWS], stars[:SIDE_ROWS]
        # tan(elevation) per texel: -v / sqrt(1 + u^2) (the face is a plane at distance 1)
        u = (np.arange(SIZE) + 0.5) / SIZE * 2 - 1
        v = (np.arange(SIDE_ROWS) + 0.5) / SIZE * 2 - 1
        tan_elev = -v[:, None] / np.sqrt(1 + u[None, :] ** 2)
        faces.append(quantize(grade(img, stars, tan_elev)))
        print(face, 'done')
    img, stars = loaded['up']
    a = (np.arange(SIZE) + 0.5) / SIZE * 2 - 1
    tan_elev = 1 / np.sqrt(a[None, :] ** 2 + a[:, None] ** 2 + 1e-9)
    faces.append(quantize(grade(img, stars, tan_elev)))
    data = b''.join(f.tobytes() for f in faces)
    packed = zlib.compress(data, 9)
    path = os.path.join(ASSETS, 'sky.bin')
    with open(path, 'wb') as out:
        out.write(struct.pack('<4sHH', b'FPS1', SIZE, SIDE_ROWS))
        out.write(pal.astype(np.uint8).tobytes())
        out.write(struct.pack('<I', len(packed)))
        out.write(packed)
    print(f'{SET}: {SIZE} px faces, {SIDE_ROWS} rows of each side; sky.bin {os.path.getsize(path)} bytes')
    if os.environ.get('FP_SKY_PREVIEW'):
        sheet = Image.new('RGB', (SIZE * 5, SIZE))
        for i, f in enumerate(faces):
            sheet.paste(Image.fromarray(pal[f].astype(np.uint8)), (i * SIZE, 0))
        sheet.save(os.environ['FP_SKY_PREVIEW'])


main()
