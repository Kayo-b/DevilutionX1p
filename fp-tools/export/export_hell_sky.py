#!/usr/bin/env python3
"""Export hell's sky (levels 13-16 have no ceiling in first person): a dark red haze, `assets/fp/hellsky.bin`.

usage: export_hell_sky.py [face size]   (default 512)

Nothing is read but the palette: the sky is made here. Low over the horizon a dull red glow, as of fires far off,
darkening to near black overhead; smoke drifts across it in a layer high above (fractal noise seen in perspective, so
it bunches up towards the horizon), lit red from below where the glow is, and here and there a brighter glow on the
horizon. Quantized with a light ordered dither to hell's palette (FP_HELL_PAL, levels/l4data/l4_1.pal of the game's
data: `dev.fp.dumpfile('levels\\\\l4data\\\\l4_1.pal', path)`), leaving out the blood colours 1-31 (hell cycles them) and
the bright UI ramps 128-159; the game maps the indices to whichever of hell's palettes a level has.

The cube's faces are as export_sky.py and fp_sky.cpp have them: ft (+x), lf (+y), bk (-x), rt (-y), up (+z), world
z up; the side faces keep their upper half and a margin below the horizon. Same file format (FPS1).
"""
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
HELL_PAL = os.environ.get('FP_HELL_PAL', os.path.expanduser('~/projects/diablo-hellfire-fp/research/hell/l4_1.pal'))
SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 512
SIDE_ROWS = SIZE // 2 + SIZE // 16

ZENITH = np.array([14.0, 3.0, 3.0])      # overhead: near black, a little red
HIGH = np.array([34.0, 7.0, 5.0])        # halfway up
HORIZON = np.array([66.0, 15.0, 6.0])    # the glow low over the horizon
FIRE = np.array([120.0, 38.0, 8.0])      # a fire's glow far off
SMOKE_LIT = np.array([96.0, 24.0, 10.0])  # smoke lit from below where the glow is
FIRES = 5                                 # glows along the horizon
SEED = 666

pal = np.frombuffer(open(HELL_PAL, 'rb').read(), dtype=np.uint8).reshape(256, 3).astype(np.float32)
ALLOWED = np.array([i for i in range(32, 255) if not 128 <= i < 160])
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 - 0.5
DITHER = 4.0


def directions(face, rows):
    """Unit view directions through each texel of a face (the inverse of fp_sky.cpp's Lookup)."""
    half = SIZE / 2.0
    col = (np.arange(SIZE) + 0.5) / half - 1.0
    row = (np.arange(rows) + 0.5) / half - 1.0
    u, v = np.meshgrid(col, row)
    one = np.ones_like(u)
    d = {
        'ft': (one, u, -v),
        'bk': (-one, -u, -v),
        'lf': (-u, one, -v),
        'rt': (u, -one, -v),
        'up': (u, v, one),
    }[face]
    d = np.stack(d, axis=-1)
    return d / np.linalg.norm(d, axis=-1, keepdims=True)


def hash01(ix, iy, seed):
    h = (ix.astype(np.int64) * 374761393 + iy.astype(np.int64) * 668265263 + seed * 1442695041) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFFFF) / float(0x1000000)


def value_noise(x, y, seed):
    xi = np.floor(x)
    yi = np.floor(y)
    fx = x - xi
    fy = y - yi
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    xi = xi.astype(np.int64)
    yi = yi.astype(np.int64)
    a = hash01(xi, yi, seed)
    b = hash01(xi + 1, yi, seed)
    c = hash01(xi, yi + 1, seed)
    d = hash01(xi + 1, yi + 1, seed)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy


def fbm(x, y, octaves, seed):
    total = np.zeros_like(x)
    amplitude = 0.5
    for i in range(octaves):
        total += amplitude * value_noise(x, y, seed + i * 17)
        x, y = x * 2.03 + 3.7, y * 2.03 - 1.3
        amplitude *= 0.5
    return total


def colour(d):
    """The sky's colour in a direction (RGB, 0..255)."""
    elevation = np.clip(d[..., 2], -1.0, 1.0)
    e = np.maximum(elevation, 0.0)
    # The gradient: the glow over the horizon fading quickly, then the dark red to the zenith.
    glow = np.exp(-e / 0.13)
    high = np.exp(-e / 0.6)
    base = ZENITH + (HIGH - ZENITH) * high[..., None] + (HORIZON - HIGH) * glow[..., None]
    # Fires on the horizon: wide, low glows at a few bearings.
    rng = np.random.default_rng(SEED)
    bearing = np.arctan2(d[..., 1], d[..., 0])
    fire = np.zeros_like(e)
    for _ in range(FIRES):
        at = rng.uniform(-np.pi, np.pi)
        width = rng.uniform(0.25, 0.6)
        strength = rng.uniform(0.35, 0.8)
        gap = np.angle(np.exp(1j * (bearing - at)))
        fire += strength * np.exp(-(gap / width) ** 2) * np.exp(-e / 0.07)
    base += (FIRE - base) * np.clip(fire, 0, 0.85)[..., None]
    # Smoke in a layer high above: where a ray meets it, in perspective (it bunches up towards the horizon).
    lift = e + 0.12
    px = d[..., 0] / lift * 1.6
    py = d[..., 1] / lift * 1.6
    smoke = fbm(px, py, 6, SEED)
    smoke = np.clip((smoke - 0.38) / 0.32, 0, 1) ** 1.2
    smoke *= np.clip(elevation / 0.03 + 1.0, 0, 1)  # none below the horizon
    lit = np.clip(glow * 0.9 + fire * 0.8 + 0.25, 0, 1)
    out = base * (1 - 0.45 * smoke[..., None]) + SMOKE_LIT * (smoke * lit * 0.6)[..., None]
    # Below the horizon (seen past the map's edge): the glow's colour, darker.
    below = np.clip(-elevation / 0.1, 0, 1)[..., None]
    return out * (1 - below * 0.5)


def quantize(rgb):
    h, w, _ = rgb.shape
    offset = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w, None] * DITHER
    flat = np.clip(rgb + offset, 0, 255).reshape(-1, 3)
    choices = pal[ALLOWED]
    weights = np.array([3.0, 6.0, 1.0], dtype=np.float32)
    out = np.empty(len(flat), dtype=np.uint8)
    for start in range(0, len(flat), 8192):
        chunk = flat[start:start + 8192]
        dist = (((chunk[:, None, :] - choices[None, :, :]) ** 2) * weights).sum(-1)
        out[start:start + 8192] = ALLOWED[dist.argmin(1)]
    return out.reshape(h, w)


def main():
    faces = []
    for face in ('ft', 'lf', 'bk', 'rt'):
        faces.append(quantize(colour(directions(face, SIDE_ROWS))))
    faces.append(quantize(colour(directions('up', SIZE))))
    data = b''.join(f.tobytes() for f in faces)
    packed = zlib.compress(data, 9)
    path = os.path.join(ASSETS, 'hellsky.bin')
    with open(path, 'wb') as f:
        f.write(b'FPS1')
        f.write(struct.pack('<HH', SIZE, SIDE_ROWS))
        f.write(pal.astype(np.uint8).tobytes())
        f.write(struct.pack('<I', len(packed)))
        f.write(packed)
    print(f'wrote {path}: {SIZE} px faces, {os.path.getsize(path)} bytes')
    if os.environ.get('FP_SKY_PREVIEW'):
        from PIL import Image
        strip = np.concatenate(faces[:4], axis=1)
        Image.fromarray(pal[strip].astype(np.uint8)).save(os.environ['FP_SKY_PREVIEW'])
        print('preview', os.environ['FP_SKY_PREVIEW'])


if __name__ == '__main__':
    main()
