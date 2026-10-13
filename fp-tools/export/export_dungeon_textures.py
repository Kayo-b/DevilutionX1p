#!/usr/bin/env python3
"""Export the texture sets of the first-person dungeon (cathedral, catacombs, Hellfire's crypt): `dungeontex.bin`.

The textures come from two CC0 packs (sources and terms in assets/fp/CREDITS.md), read from where they were
unpacked (override with the environment variables below):
- SigilsVault "Modular Dungeon Kit v1.0" (FP_SIGILS_KIT_DIR): its trim sheet (ashlar blocks, mouldings, plain
  stone) and the door texture.
- Screaming Brain Studios "Tiny Texture Pack 2", 512 x 512 (FP_SBS_DIR): bricks, stones and floor tiles.

Every texture is cut out, made to tile, scaled to TEXELS_PER_TILE texels per tile (the game's world unit; a wall
face is one tile wide and about three tall) and graded for its tileset: colour, brightness, contrast and a light
blur, so it reads as soft painted stone rather than as a photo or as big pixels. The game converts the RGB to the
level's palette when the level loads (fp_dungeon.cpp).

File format (little endian): "FPT1", uint32 texture count; per texture: uint8 name length, name, uint16 width,
uint16 height, float32 tiles across, float32 tiles down (the world size one repeat covers), then width * height RGB
bytes, rows top to bottom. Names are "<set>/<slot>".

usage: export_dungeon_textures.py [--preview DIR PALETTE...]   (preview: quantized contact sheets, see preview())
"""
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
DOWNLOADS = os.path.expanduser('~/Downloads')
SIGILS = os.environ.get('FP_SIGILS_KIT_DIR', os.path.join(DOWNLOADS, 'modular dungeon kit v1.0', 'modular dungeon kit'))
SBS = os.environ.get('FP_SBS_DIR', os.path.join(DOWNLOADS, 'SBS - Tiny Texture Pack 2 - 512x512', '512x512'))

# Texels per tile (world unit). Walls, floors and ceilings all use this density, so a wall face is 128 texels wide.
TEXELS_PER_TILE = 128


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)


def trim_sheet():
    return load(os.path.join(SIGILS, 'Modular Dungeon Pieces', 'Textures', '1024 x 1024', 'TrimSheet_1024x1024.png'))


def mossy_trim_sheet():
    im = Image.open(os.path.join(SIGILS, 'Modular Dungeon Pieces', 'Textures', '1024 x 1024', 'TrimSheet_Mossy_1024x1024.png')).convert('RGB')
    return np.asarray(im.resize((1024, 1024), Image.LANCZOS), dtype=np.float32)


def sbs(group, number):
    return load(os.path.join(SBS, group, f'{group}_{number:02d}-512x512.png'))


def door_texture():
    return load(os.path.join(SIGILS, 'Props', 'Textures', '512 x 512', 'Door_Diffuse_256x512.png'))


def resize(rgb, width, height):
    """Area-filtered resize (the sources are larger than the targets: this softens them)."""
    im = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    return np.asarray(im.resize((width, height), Image.LANCZOS), dtype=np.float32)


def blur(rgb, radius):
    if radius <= 0:
        return rgb
    # Wrap around before blurring so the texture still tiles.
    h, w = rgb.shape[:2]
    pad = int(radius * 3) + 1
    tiled = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode='wrap')
    im = Image.fromarray(np.clip(tiled, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(im, dtype=np.float32)[pad:pad + h, pad:pad + w]


def seamless(rgb, border):
    """Makes a crop tile: each edge cross-fades into the opposite one over `border` pixels."""
    h, w = rgb.shape[:2]
    out = rgb.copy()
    for k in range(border):
        t = 0.5 * (1.0 - k / border)  # 0.5 at the very edge, 0 inside
        out[:, k] = rgb[:, k] * (1 - t) + rgb[:, w - 1 - k] * t
        out[:, w - 1 - k] = rgb[:, w - 1 - k] * (1 - t) + rgb[:, k] * t
    rgb = out.copy()
    for k in range(border):
        t = 0.5 * (1.0 - k / border)
        out[k] = rgb[k] * (1 - t) + rgb[h - 1 - k] * t
        out[h - 1 - k] = rgb[h - 1 - k] * (1 - t) + rgb[k] * t
    return out


def grade(rgb, mean, contrast=1.0, saturation=0.5, tint=(1.0, 1.0, 1.0), gamma=1.0):
    """Colour grading: desaturate towards grey, set the contrast around the mean, tint, then bring the mean
    luminance to `mean` (0-255, as the texture looks fully lit)."""
    luma = rgb @ np.array([0.30, 0.59, 0.11], dtype=np.float32)
    out = luma[..., None] + saturation * (rgb - luma[..., None])
    m = float(luma.mean())
    out = m + contrast * (out - m)
    out = out * np.array(tint, dtype=np.float32)
    if gamma != 1.0:
        out = 255.0 * np.power(np.clip(out, 0, 255) / 255.0, gamma)
    m2 = float((out @ np.array([0.30, 0.59, 0.11], dtype=np.float32)).mean())
    out = out * (mean / max(m2, 1.0))
    return np.clip(out, 0, 255)


def even_squares(rgb, count, variation=0.06):
    """Evens out the tones of a texture's `count` x `count` squares (a checkerboard of flagstones), each to the mean
    within +-`variation`, so the floor reads as one stone with its joints instead of a checkerboard."""
    h, w = rgb.shape[:2]
    out = rgb.copy()
    luma = rgb @ np.array([0.30, 0.59, 0.11], dtype=np.float32)
    mean = float(luma.mean())
    rng = np.random.default_rng(7)
    for j in range(count):
        for i in range(count):
            ys, xs = slice(j * h // count, (j + 1) * h // count), slice(i * w // count, (i + 1) * w // count)
            m = float(np.median(luma[ys, xs]))
            target = mean * (1.0 + variation * (rng.random() * 2 - 1))
            out[ys, xs] = rgb[ys, xs] * (target / max(m, 1.0))
    return out


def tiles(rgb, across, down):
    """Scales a texture covering `across` x `down` tiles to TEXELS_PER_TILE."""
    return resize(rgb, int(round(across * TEXELS_PER_TILE)), int(round(down * TEXELS_PER_TILE)))


# --- The sets ---------------------------------------------------------------------------------------------------
# Each slot: (rgb, tiles across, tiles down). Slots: wall (the body, tiled), base (the plinth band at the foot of a
# wall, tiled along it), top (the band under the ceiling), floor, ceiling, pillar, arch (an arch's underside),
# door (a door leaf: stretched over the whole leaf, not tiled).

# The trim sheet's ashlar band: four courses of blocks (rows 613-919, 77 px a course), seamless both ways at 1024
# px across, laid over 4 tiles across and 1 down (a block a little over half a tile long, a course a quarter tile
# high). The game wraps textures with bit masks: every size is a power of two.
ASHLAR = (613, 920)


def band(sheet, top, bottom, height, tiles_down):
    """A strip of the trim sheet across its whole width, 4 tiles long, scaled to 512 x `height` texels."""
    return resize(sheet[top:bottom], 4 * TEXELS_PER_TILE, height), 4.0, tiles_down


def cathedral():
    sheet = trim_sheet()
    out = {}
    # Church stonework: warm grey ashlar, a moulded plinth and a frieze under the ceiling, square flagstones.
    warm = (1.06, 1.0, 0.9)
    rgb, w, h = band(sheet, *ASHLAR, 128, 1.0)
    out['wall'] = (blur(grade(rgb, 56, 1.05, 0.0, warm), 0.5), w, h)
    rgb, w, h = band(sheet, 920, 1024, 64, 0.4)
    out['base'] = (blur(grade(rgb, 50, 1.0, 0.0, warm), 0.5), w, h)
    rgb, w, h = band(sheet, 512, 612, 64, 0.4)
    out['top'] = (blur(grade(rgb, 54, 1.0, 0.35, warm), 0.5), w, h)
    floor = even_squares(sbs('Tile', 3), 2)
    out['floor'] = (blur(grade(tiles(floor, 2, 2), 46, 0.8, 0.15, (1.0, 0.98, 0.94)), 0.6), 2.0, 2.0)
    plain = seamless(sheet[0:480, 0:480], 48)
    out['ceiling'] = (blur(grade(tiles(plain, 2, 2), 36, 1.1, 0.0, warm), 0.6), 2.0, 2.0)
    out['pillar'] = (blur(grade(tiles(plain, 2, 2), 60, 1.15, 0.0, warm), 0.5), 2.0, 2.0)
    out['arch'] = (blur(grade(tiles(plain, 2, 2), 50, 1.1, 0.0, warm), 0.5), 2.0, 2.0)
    out['door'] = (blur(grade(resize(door_texture(), 128, 256), 70, 1.0, 0.8, (1.0, 0.92, 0.8)), 0.4), 1.0, 1.0)
    return out


def catacombs():
    out = {}
    # A burial crypt: dark, cold, rough stone blocks; cobbles underfoot; rough rock overhead.
    cold = (0.92, 0.97, 1.08)
    wall = sbs('Brick', 12)
    out['wall'] = (blur(grade(tiles(wall, 2, 2), 44, 1.0, 0.25, cold), 0.6), 2.0, 2.0)
    floor = sbs('Tile', 6)
    out['floor'] = (blur(grade(tiles(floor, 2, 2), 38, 0.7, 0.2, cold), 0.6), 2.0, 2.0)
    ceiling = sbs('Stone', 11)
    out['ceiling'] = (blur(grade(tiles(ceiling, 2, 2), 28, 1.1, 0.4, cold), 0.6), 2.0, 2.0)
    pillar = sbs('Stone', 5)
    out['pillar'] = (blur(grade(tiles(pillar, 2, 2), 48, 1.0, 0.25, cold), 0.5), 2.0, 2.0)
    out['arch'] = (blur(grade(tiles(wall, 2, 2), 40, 1.0, 0.25, cold), 0.6), 2.0, 2.0)
    out['door'] = (blur(grade(resize(door_texture(), 128, 256), 60, 1.0, 0.6, (0.95, 0.92, 0.85)), 0.4), 1.0, 1.0)
    return out


def crypt():
    out = {}
    # Hellfire's crypt: pale bone-coloured marble blocks veined with red, dark red rock above.
    bone = (1.04, 0.97, 0.9)
    wall = sbs('Brick', 18)
    out['wall'] = (blur(grade(tiles(wall, 2, 2), 54, 0.9, 0.5, bone), 0.6), 2.0, 2.0)
    floor = sbs('Tile', 14)
    out['floor'] = (blur(grade(tiles(floor, 2, 2), 44, 0.8, 0.45, bone), 0.6), 2.0, 2.0)
    ceiling = sbs('Stone', 17)
    out['ceiling'] = (blur(grade(tiles(ceiling, 2, 2), 30, 1.0, 0.6, (1.0, 0.9, 0.85)), 0.6), 2.0, 2.0)
    pillar = sbs('Stone', 15)
    out['pillar'] = (blur(grade(tiles(pillar, 2, 2), 58, 0.9, 0.45, bone), 0.5), 2.0, 2.0)
    out['arch'] = (blur(grade(tiles(wall, 2, 2), 48, 0.9, 0.5, bone), 0.6), 2.0, 2.0)
    out['door'] = (blur(grade(resize(door_texture(), 128, 256), 64, 1.0, 0.7, (1.0, 0.9, 0.8)), 0.4), 1.0, 1.0)
    return out


SETS = {'cathedral': cathedral, 'catacombs': catacombs, 'crypt': crypt}


def build():
    textures = []
    for set_name, make in SETS.items():
        for slot, (rgb, across, down) in make().items():
            h, w = rgb.shape[:2]
            assert w & (w - 1) == 0 and h & (h - 1) == 0, f'{set_name}/{slot} is {w}x{h}: not a power of two'
            textures.append((f'{set_name}/{slot}', np.clip(rgb + 0.5, 0, 255).astype(np.uint8), across, down))
    return textures


def write(textures, path):
    with open(path, 'wb') as f:
        f.write(b'FPT1')
        f.write(struct.pack('<I', len(textures)))
        for name, rgb, across, down in textures:
            h, w = rgb.shape[:2]
            encoded = name.encode()
            f.write(struct.pack('<B', len(encoded)))
            f.write(encoded)
            f.write(struct.pack('<HHff', w, h, across, down))
            f.write(rgb.tobytes())
    print(f'wrote {path}: {len(textures)} textures, {os.path.getsize(path)} bytes')


# --- Preview: the textures as the game shows them (in a level palette, at a few light levels) --------------------

def palette_indices():
    """The colours the game quantizes the textures to (fp_dungeon.cpp): the level's own ramps but the near-blacks
    112-127, and the shared ramps 160-254 (not the bright UI colours 128-159, not white 255)."""
    return [i for i in range(1, 112)] + [i for i in range(160, 255)]


def quantize(rgb, palette, allowed):
    pal = palette[allowed].astype(np.float32)
    flat = rgb.reshape(-1, 3)
    out = np.empty(len(flat), dtype=np.int32)
    weights = np.array([3.0, 6.0, 1.0], dtype=np.float32)
    for start in range(0, len(flat), 4096):
        chunk = flat[start:start + 4096]
        d = (((chunk[:, None, :] - pal[None, :, :]) ** 2) * weights).sum(axis=2)
        out[start:start + 4096] = np.array(allowed)[d.argmin(axis=1)]
    return out.reshape(rgb.shape[:2])


def light(index, level):
    """Diablo's light tables (MakeLightTable): a colour moves `level` steps darker along its 16-colour ramp (the
    8-colour ramps 128-159 half as fast), black past the ramp's end."""
    table = np.zeros(256, dtype=np.int32)
    for i in range(1, 256):
        if 128 <= i < 160:
            steps, start = 8, 128 + (i - 128) // 8 * 8
        else:
            steps, start = 16, i - i % 16
        c = i + level * steps // 16
        table[i] = 0 if c > start + steps - 1 or c == 255 else c
    return table[index]


def preview(directory, palettes):
    os.makedirs(directory, exist_ok=True)
    textures = build()
    for pal_path in palettes:
        palette = np.frombuffer(open(pal_path, 'rb').read(), dtype=np.uint8).reshape(256, 3)
        allowed = palette_indices()
        for set_name in SETS:
            group = [t for t in textures if t[0].startswith(set_name + '/')]
            rows = []
            for name, rgb, across, down in group:
                q = quantize(rgb.astype(np.float32), palette, allowed)
                cells = []
                for level in (0, 3, 6, 9):
                    lit = light(q, level)
                    # Two repeats across, at twice the size, like a close wall.
                    img = palette[lit]
                    img = np.concatenate([img, img], axis=1)
                    cells.append(np.asarray(Image.fromarray(img).resize((img.shape[1] * 2 // 2, img.shape[0] * 2 // 2), Image.NEAREST)))
                height = max(c.shape[0] for c in cells)
                row = np.concatenate([np.pad(c, ((0, height - c.shape[0]), (0, 8), (0, 0))) for c in cells], axis=1)
                rows.append(row)
            width = max(r.shape[1] for r in rows)
            sheet = np.concatenate([np.pad(r, ((0, 8), (0, width - r.shape[1]), (0, 0))) for r in rows], axis=0)
            out = os.path.join(directory, f'{set_name}_{os.path.splitext(os.path.basename(pal_path))[0]}.png')
            Image.fromarray(sheet.astype(np.uint8)).save(out)
            print('preview', out)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--preview':
        preview(sys.argv[2], sys.argv[3:])
    else:
        write(build(), os.path.join(ASSETS, 'dungeontex.bin'))
