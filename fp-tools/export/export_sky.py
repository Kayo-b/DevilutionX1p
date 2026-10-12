#!/usr/bin/env python3
"""Export a night skybox (six cube faces) as the town's first-person sky, `assets/fp/sky.bin`.

usage: export_sky.py [set] [face size]   (default d1e1m6, 1024)

Source: FP_SKY_DIR (default ~/projects/DevilutionX1p/skybox-night), six PNG files per set, named <set>{ft,lf,bk,rt,up,dn}
(2048 x 2048, no extension). Looking out from inside, turning right goes ft, lf, bk, rt; the first person view
puts ft at +x, lf at +y, bk at -x and rt at -y (world z up). The up face's columns run along +x and its rows along +y.

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
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
SKY_DIR = os.environ.get('FP_SKY_DIR', os.path.expanduser('~/projects/DevilutionX1p/skybox-night'))
TOWN_PAL = os.environ.get('FP_TOWN_PAL', os.path.expanduser('~/projects/diablo-hellfire-fp/research/tristram_plaza/town.pal'))
SET = sys.argv[1] if len(sys.argv) > 1 else 'd1e1m6'
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
SIDE_ROWS = SIZE // 2 + SIZE // 16  # the horizon is the middle row; a margin below it for looking down at the map's edge

GAIN = 3.2                             # the source sky is nearly black: stars and the milky way brought up
FOG = np.array([13.0, 17.0, 27.0])     # the town's fog and old sky colour (palette 190, PAL16_BLUE + 14)
ZENITH = np.array([4.0, 6.0, 11.0])    # the dark sky overhead
HAZE_AT_HORIZON = 0.55                 # how much of the fog colour lies over the horizon, fading out by HAZE_TOP
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


def grade(img, tan_elev):
    """Brightened sky over a gradient from the haze at the horizon to the dark zenith."""
    t = np.clip(tan_elev, 0, None)[..., None]
    base = FOG + (ZENITH - FOG) * np.clip(t / 1.2, 0, 1)
    sky = base + img * GAIN
    haze = HAZE_AT_HORIZON * np.clip(1 - t / HAZE_TOP, 0, 1)
    return sky * (1 - haze) + FOG * haze


def load(face):
    im = Image.open(os.path.join(SKY_DIR, SET + face)).convert('RGB').resize((SIZE, SIZE), Image.LANCZOS)
    return np.asarray(im).astype(float)


def main():
    faces = []
    for face in ('ft', 'lf', 'bk', 'rt'):
        img = load(face)[:SIDE_ROWS]
        # tan(elevation) per texel: -v / sqrt(1 + u^2) (the face is a plane at distance 1)
        u = (np.arange(SIZE) + 0.5) / SIZE * 2 - 1
        v = (np.arange(SIDE_ROWS) + 0.5) / SIZE * 2 - 1
        tan_elev = -v[:, None] / np.sqrt(1 + u[None, :] ** 2)
        faces.append(quantize(grade(img, tan_elev)))
        print(face, 'done')
    img = load('up')
    a = (np.arange(SIZE) + 0.5) / SIZE * 2 - 1
    tan_elev = 1 / np.sqrt(a[None, :] ** 2 + a[:, None] ** 2 + 1e-9)
    faces.append(quantize(grade(img, tan_elev)))
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
