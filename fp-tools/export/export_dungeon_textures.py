#!/usr/bin/env python3
"""Export the texture sets of the first-person dungeon (cathedral, catacombs, Hellfire's crypt): `dungeontex.bin`.

The textures are the Diablo 1 dungeon kit's: "Dungeon Pieces" of the fan rebuild of Diablo 1's assets (Nexus Mods,
mod 67266; terms and credits in assets/fp/CREDITS.md), read from where it was unpacked (FP_DUNGEON_PIECES_DIR; a
file missing there is extracted from the archive, FP_DUNGEON_PIECES_ARCHIVE, with 7z). Every tileset wears them the
way the kit does: its stone blocks on the walls, arches and ceilings, its church pavement underfoot, its Diablo
marble on the pillar shafts with its column-head stone above and below, its plank door with iron bands. The blocks
repeat once a tile, as on the kit's own wall pieces (a wall is about three repeats high there and here), and the
kit's normal and specular maps are baked in as light from above, so the blocks stand out of their joints like the
original's pre-rendered art. The cathedral's sarcophagus stone and torch stand bases are from Screaming Brain
Studios' "Tiny Texture Pack 2" (CC0), read from its 512 x 512 folder (FP_SBS_TEXTURES_DIR).

Every texture is then graded to the colours of its tileset's own art, measured on the original iso tiles (full
light, levels 1, 5 and 21): its own pattern of light and dark, scaled to the art's mean and spread, in the art's hue
with a little of its own (`match`). The game converts the RGB to the level's palette when the level loads
(fp_dungeon.cpp).

File format (little endian): "FPT1", uint32 texture count; per texture: uint8 name length, name, uint16 width,
uint16 height, float32 tiles across, float32 tiles down (the world size one repeat covers), then width * height RGB
bytes, rows top to bottom. Names are "<set>/<slot>".

usage: export_dungeon_textures.py [--preview DIR PALETTE...]   (preview: quantized contact sheets, see preview())
"""
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, '..', '..', 'assets', 'fp')
KIT = os.environ.get('FP_DUNGEON_PIECES_DIR', os.path.expanduser('~/projects/diablo-hellfire-fp/research/dungeon_pieces'))
ARCHIVE = os.environ.get('FP_DUNGEON_PIECES_ARCHIVE', os.path.expanduser('~/projects/diablo-hellfire-fp/Dungeon Pieces-67266-.7z'))
# Screaming Brain Studios' "Tiny Texture Pack 2" (CC0), its 512 x 512 folder: the sarcophagus's stone and the torch
# stands' bases.
SBS = os.environ.get('FP_SBS_TEXTURES_DIR', os.path.expanduser('~/Downloads/SBS - Tiny Texture Pack 2 - 512x512/512x512'))
# "Diablo's Hell stairs" of the same fan rebuild (Nexus Mods 67266): its bone texture, for hell's beams, posts and horns.
HELL_STAIRS = os.environ.get('FP_HELL_STAIRS_DIR', os.path.expanduser('~/projects/diablo-hellfire-fp/research/hell_stairs'))
HELL_STAIRS_ARCHIVE = os.environ.get('FP_HELL_STAIRS_ARCHIVE', os.path.expanduser("~/projects/diablo-hellfire-fp/Diablo's Hell stairs-67266-.rar"))

LUMA = np.array([0.30, 0.59, 0.11], dtype=np.float32)


def kit(name):
    """Path of one of the kit's files, extracted from the archive first if it is not unpacked yet."""
    path = os.path.join(KIT, name)
    if not os.path.exists(path):
        os.makedirs(KIT, exist_ok=True)
        subprocess.run(['7z', 'e', '-y', f'-o{KIT}', ARCHIVE, name], check=True, stdout=subprocess.DEVNULL)
    return path


def load(name):
    return np.asarray(Image.open(kit(name)).convert('RGB'), dtype=np.float32)


def hell_stairs(name):
    """One of the hell stairs' files, extracted from their archive first if it is not unpacked yet."""
    path = os.path.join(HELL_STAIRS, name)
    if not os.path.exists(path):
        os.makedirs(HELL_STAIRS, exist_ok=True)
        subprocess.run(['7z', 'e', '-y', f'-o{HELL_STAIRS}', HELL_STAIRS_ARCHIVE, name], check=True, stdout=subprocess.DEVNULL)
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)


def stains(size, radius, seed):
    """Soft blotches that tile, 0..1 (1 the darkest): blurred noise, its upper part kept."""
    rng = np.random.default_rng(seed)
    noise = np.repeat(rng.random((size, size, 1), dtype=np.float32) * 255, 3, axis=2)
    soft = blur(noise, radius)[..., 0]
    soft = (soft - soft.mean()) / max(float(soft.std()), 1e-3)
    return np.clip(soft * 0.6, 0, 1)


def sbs(name):
    """One of the Tiny Texture Pack's textures (they tile as they are)."""
    return np.asarray(Image.open(os.path.join(SBS, name)).convert('RGB'), dtype=np.float32)


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


def match(rgb, color, spread, chroma=0.2):
    """Grades a texture to a colour of the original art: its luminance keeps its own pattern, brought to the mean of
    `color` (as the art looks fully lit) and to a standard deviation of `spread`; its hue is `color`'s, plus `chroma`
    of its own variation around its grey."""
    luma = rgb @ LUMA
    m = float(luma.mean())
    k = spread / max(float(luma.std()), 1.0)
    color = np.array(color, dtype=np.float32)
    target = float(color @ LUMA)
    out = color * ((target + (luma - m) * k) / target)[..., None] + chroma * k * (rgb - luma[..., None])
    return np.clip(out, 0, 255)


def colour_ramp(rgb):
    """A texture's colours by brightness: per luminance step (0..255), its mean colour divided by that luminance (a
    hue of luminance 1), steps it has no texels at filled in from their neighbours; with the texture's luminance mean
    and standard deviation, which say where along the ramp a texel of another texture falls (`recolor`)."""
    luma = rgb @ LUMA
    steps = np.clip(luma, 0, 255).astype(np.int32).ravel()
    sums = np.zeros((256, 3), dtype=np.float64)
    np.add.at(sums, steps, rgb.reshape(-1, 3))
    counts = np.bincount(steps, minlength=256).astype(np.float64)
    known = np.flatnonzero(counts >= 16)
    hue = sums[known] / counts[known, None] / np.maximum(known[:, None], 1.0)
    table = np.stack([np.interp(np.arange(256), known, hue[:, c]) for c in range(3)], axis=1)
    return table.astype(np.float32), float(luma.mean()), max(float(luma.std()), 1.0)


def recolor(rgb, ramp, mean, spread):
    """Paints a texture in another one's colours (a `colour_ramp`): its own pattern of light and dark, brought to a
    luminance `mean` and standard deviation `spread`, each texel in the ramp's colour as dark relative to the ramp's
    texture as the texel is relative to its own."""
    table, ramp_mean, ramp_spread = ramp
    luma = rgb @ LUMA
    z = (luma - float(luma.mean())) / max(float(luma.std()), 1.0)
    at = np.clip(ramp_mean + z * ramp_spread, 0, 255).astype(np.int32)
    return np.clip(table[at] * (mean + z * spread)[..., None], 0, 255)


# The original's sarcophagus (objects\sarc.cel, closed): its body neutral grey stone (its greys from 52 to 118 by
# brightness, the darkest sixth, its shadows and outlines, left out: mean 86), its lid an olive bronze plate, (77, 68,
# 43) +-19 with the dark lines of its carving (the plate itself a little lighter). The grey half a palette step darker
# than the art's: the game shades the sunken panels, under the rim and towards the foot darker still. The stone is
# the Tiny Texture Pack's mottled white (Elements_11, Elements_12) brought down to that grey, its mottling kept.
SARCOPHAGUS_GREY = (81, 81, 81)
SARCOPHAGUS_SPREAD = 10
SARCOPHAGUS_BRONZE = (86, 76, 48)
# The torch stands' bases: a pale, faintly warm grey (the art's are a pale grey; any brighter and they stand out of
# the dark room), worn stone (the Tiny Texture Pack's Stone_07, Stone_08) in that colour.
IVORY = (100, 97, 90)
IVORY_SPREAD = 10
# The plain margin of the lid's texture round the carving, in texels; the lid's edges take their colour from it
# (LidPlainU, LidPlainV in fp_buildings.cpp).
LID_MARGIN = 16

# The original's marble: the shafts of the cathedral's pillars (level 1, seed 777: the six at 52,52 50,26 62,26 38,32
# 80,40 86,40, read with dev.fp.dumpiso). A third of their texels are teal, the darker ones; the rest pinkish grey.
# Each hue's colours from dark to light, each the mean of an eighth of its texels (the grey's darkest eighth, the
# shaded edge of the round shaft, left out).
DIABLO_MARBLE = [
    (0.33, [(30, 51, 51), (44, 66, 66), (49, 72, 72), (52, 76, 76), (57, 81, 81), (61, 87, 87), (63, 89, 89), (70, 97, 97)]),
    (0.67, [(83, 69, 69), (89, 79, 81), (93, 92, 96), (97, 96, 101), (97, 99, 104), (108, 104, 106), (116, 108, 110)]),
]


def rank_recolor(rgb, parts):
    """Paints a texture in colours measured by brightness rank (`DIABLO_MARBLE`): its texels sorted from dark to light
    are shared out between the parts (share, colours) in order, and within its part each texel takes the colour at its
    rank. So the texture keeps its pattern (here its dark veins and what lies around them become the teal) and takes on
    the measured texture's share of every hue, and its brightness."""
    h, w = rgb.shape[:2]
    luma = (rgb @ LUMA).ravel()
    rank = np.empty(luma.size, dtype=np.float32)
    rank[np.argsort(luma, kind='stable')] = (np.arange(luma.size) + 0.5) / luma.size
    out = np.zeros((luma.size, 3), dtype=np.float32)
    start = 0.0
    for share, colours in parts:
        inside = (rank >= start) & (rank < start + share + 1e-6)
        local = (rank[inside] - start) / share
        colours = np.array(colours, dtype=np.float32)
        centres = (np.arange(len(colours)) + 0.5) / len(colours)
        out[inside] = np.stack([np.interp(local, centres, colours[:, c]) for c in range(3)], axis=1)
        start += share
    return out.reshape(h, w, 3)


def relief(rgb, normal, strength, light=(-0.3, 0.75, 0.6)):
    """Bakes light from one of the kit's normal maps (red right, green down) into a texture: lit from above and a
    little from the left, a flat surface unchanged; `strength` 1 is the full shading."""
    n = normal / 127.5 - 1.0
    n[..., 1] = -n[..., 1]
    n = n / np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-3)
    lv = np.array(light, dtype=np.float32)
    lv = lv / np.linalg.norm(lv)
    shade = np.maximum(n @ lv, 0.0) / lv[2]
    return rgb * (1.0 + strength * (shade - 1.0))[..., None]


def joint_mask(spec):
    """The mortar joints of the kit's blocks, 0..1: black in their specular map."""
    s = spec @ LUMA
    return np.clip((60.0 - s) / 50.0, 0.0, 1.0)


def block_tones(joints, variation, seed):
    """A tone per block of the kit's running bond (0 joints, else 1 +- `variation`): the courses lie between joint
    rows, the blocks between joint columns within a course. Courses and blocks cut by the texture's edges wrap round,
    so the tones tile too."""
    h, w = joints.shape
    rng = np.random.default_rng(seed)
    row_joint = joints.mean(axis=1) > 0.5
    # Courses: runs of rows that are not joints; a course cut by the top and bottom edges is one course.
    courses, y = [], 0
    while y < h:
        if row_joint[y]:
            y += 1
            continue
        start = y
        while y < h and not row_joint[y]:
            y += 1
        courses.append([np.arange(start, y)])
    if len(courses) > 1 and not row_joint[0] and not row_joint[-1]:
        courses[0] = courses.pop() + courses[0]
    tones = np.ones((h, w), dtype=np.float32)
    for parts in courses:
        rows = np.concatenate(parts)
        col_joint = joints[rows].mean(axis=0) > 0.5
        x = 0
        blocks = []
        while x < w:
            if col_joint[x]:
                x += 1
                continue
            start = x
            while x < w and not col_joint[x]:
                x += 1
            blocks.append((start, x))
        if len(blocks) > 1 and not col_joint[0] and not col_joint[-1]:
            (a0, a1), (b0, b1) = blocks[0], blocks.pop()
            tone = 1.0 + variation * (rng.random() * 2 - 1)
            tones[np.ix_(rows, np.arange(a0, a1))] = tone
            tones[np.ix_(rows, np.arange(b0, b1))] = tone
            blocks = blocks[1:]
        for x0, x1 in blocks:
            tones[np.ix_(rows, np.arange(x0, x1))] = 1.0 + variation * (rng.random() * 2 - 1)
    return tones


def grain(name, width, height, strength):
    """A texture's luminance as a multiplier around 1 (`strength` its relative spread), scaled to width x height."""
    rgb = resize(load(name), width, height)
    luma = rgb @ LUMA
    return 1.0 + strength * (luma - luma.mean()) / max(float(luma.std()), 1.0)


# --- The kit's surfaces ----------------------------------------------------------------------------------------------

def blocks(variation, seed, depth=0.55, rough=None):
    """The kit's wall blocks (dungeon_wall): per-block tones, its normal map baked in as light from above, the joints
    darkened by its specular map; optionally the kit's rough stone laid over them (`rough`: its strength). One repeat:
    2 blocks across, 8 courses down (a running bond)."""
    albedo = load('Modern-Cream-Blocks_seamless.tga')
    normal = load('Modern-Cream-Blocks_seamless_nrm.tga')
    joints = joint_mask(load('Modern-Cream-Blocks_seamless_spec.tga'))
    rgb = albedo * block_tones(joints, variation, seed)[..., None]
    rgb = relief(rgb, normal, 0.8)
    rgb = rgb * (1.0 - depth * joints)[..., None]
    if rough:
        h, w = rgb.shape[:2]
        rgb = rgb * grain('yellow stone seamless.tga', w, h, rough)[..., None]
    return rgb


def joint_middles(is_joint):
    """The middles of the runs of joint lines (rows or columns) in a mask along one axis."""
    lines = np.flatnonzero(is_joint)
    starts = [lines[0]] + [r for p, r in zip(lines, lines[1:]) if r != p + 1]
    ends = [p for p, r in zip(lines, lines[1:]) if r != p + 1] + [lines[-1]]
    return [(a + b) // 2 for a, b in zip(starts, ends)]


def course_band(rgb, courses=1):
    """`courses` whole courses of a block texture (from the top of its first full course), all the way across."""
    joints = joint_mask(load('Modern-Cream-Blocks_seamless_spec.tga'))
    middles = joint_middles(joints.mean(axis=1) > 0.5)
    return rgb[middles[0]:middles[courses]]


def soldier_course(variation, seed, depth=0.55):
    """The kit's wall blocks stood on end (the ring of the blind arches): one course of `blocks` (its arguments),
    cut at its joints, each block turned upright, side by side; its normal map turned with it, so the light from above
    is baked in as on the wall. As tall as a block is long; as wide as a course is high times its blocks (four)."""
    albedo = load('Modern-Cream-Blocks_seamless.tga')
    normal = load('Modern-Cream-Blocks_seamless_nrm.tga')
    joints = joint_mask(load('Modern-Cream-Blocks_seamless_spec.tga'))
    albedo = albedo * block_tones(joints, variation, seed)[..., None]
    rows = joint_middles(joints.mean(axis=1) > 0.5)
    top, bottom = rows[0], rows[1]
    course = [a[top:bottom] for a in (albedo, normal, joints)]
    # Its blocks, from the middle of one joint to the next (rolled round so the first starts at a joint).
    cuts = joint_middles(course[2].mean(axis=0) > 0.3)  # (some of the kit's joints are faint across a course)
    course = [np.roll(a, -cuts[0], axis=1) for a in course]
    cuts = [c - cuts[0] for c in cuts] + [course[0].shape[1]]
    # Each block brought to their common length (they differ by a texel or so), then stood up.
    length = int(round(np.mean(np.diff(cuts))))
    height = course[0].shape[0]
    parts = [[], [], []]
    for x0, x1 in zip(cuts, cuts[1:]):
        for k, a in enumerate(course):
            block = a[:, x0:x1]
            block = resize(block, length, height) if block.ndim == 3 else resize(np.repeat(block[..., None] * 255.0, 3, axis=2), length, height)[..., 0] / 255.0
            parts[k].append(np.rot90(block, 1))
    rgb, nrm, jnt = (np.concatenate(p, axis=1) for p in parts)
    # Turned a quarter anticlockwise: what faced right faces up, what faced down faces right (red right, green down).
    nrm = np.stack([nrm[..., 1], 255.0 - nrm[..., 0], nrm[..., 2]], axis=2)
    rgb = relief(rgb, nrm, 0.8)
    return rgb * (1.0 - depth * jnt)[..., None]


def carving(stone, width, height):
    """The kit's sarcophagus lid (capac): the saint under an arch of its relief map (capac_bump, white raised, its
    engraved lines dark), cut to its panel, the lines thickened so they survive the scaling, and lit as relief from
    the head end over `stone` (width x height)."""
    bump = load('capac_bump.tga') @ LUMA
    # The map's grain smoothed away first (thickening would darken it too), the background brought to 1.
    panel = Image.fromarray(np.clip(bump[44:2012, 540:1536], 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2))
    panel = panel.filter(ImageFilter.MinFilter(5))
    lines = np.asarray(panel.resize((width, height), Image.LANCZOS), dtype=np.float32)
    lines = np.clip(lines / np.percentile(lines, 75), 0.0, 1.0)
    raised = np.asarray(panel.filter(ImageFilter.GaussianBlur(6)).resize((width, height), Image.LANCZOS), dtype=np.float32) / 255.0
    # Light from the head end (the top of the texture) and a little from the left.
    dy = np.gradient(raised, axis=0)
    dx = np.gradient(raised, axis=1)
    shade = 1.0 - 6.0 * (dy * 0.8 + dx * 0.3)
    return stone * (shade * (0.45 + 0.55 * lines))[..., None]


def lid_plate(colour, width, height, margin):
    """The sarcophagus's bronze lid, width x height: the carving inset in a plain plate, `margin` texels of it all
    round."""
    plate = np.full((height, width, 3), colour, dtype=np.float32)
    inner = (height - 2 * margin, width - 2 * margin)
    plate[margin:height - margin, margin:width - margin] = carving(np.full(inner + (3,), colour, dtype=np.float32), inner[1], inner[0])
    return plate


def door(color, spread):
    """The kit's door (door_dun): its planks, relief from its normal map, two iron bands across (its hinge straps, of
    the kit's steel) and the dark ring of its handle; stretched over a whole leaf, 128 x 256."""
    rgb = relief(load('door_diff.tga'), load('door_nrm.tga'), 0.7)
    rgb = match(rgb, color, spread, 0.3)
    h, w = rgb.shape[:2]
    steel = match(seamless(load('Steel_02_UV_H_CM_1.jpg'), 48), (52, 52, 56), 10, 0.2)
    for centre in (0.2, 0.8):
        y0, y1 = int(h * (centre - 0.035)), int(h * (centre + 0.035))
        rgb[y0:y1] = steel[y0:y1, :w]
        rgb[y0:y0 + 3] *= 1.35  # lit top edge
        rgb[y1 - 3:y1] *= 0.55  # shadow under it
        for x in range(w // 10, w, w // 5):  # rivets
            rgb[(y0 + y1) // 2 - 6:(y0 + y1) // 2 + 6, x - 6:x + 6] *= 1.4
    yy, xx = np.mgrid[0:h, 0:w]
    r = np.hypot(xx - w * 0.82, yy - h * 0.5)
    ring = (r > h * 0.035) & (r < h * 0.05)
    rgb[ring] = rgb[ring] * 0.35
    return resize(rgb, 128, 256)


def sized(rgb, texels_across, texels_down=None):
    return resize(rgb, texels_across, texels_down or texels_across)


# --- The sets ---------------------------------------------------------------------------------------------------
# Each slot: (rgb, tiles across, tiles down). Slots: wall (the body, tiled), base (the plinth band at the foot of a
# wall, tiled along it), top (the band under the ceiling), floor, ceiling, pillar (a pillar's shaft), stone (dressed
# stone: pillar plinths and capitals, the tops of low walls, the treads of steps), arch (the inside of arches and
# doorways), door (a door leaf: stretched over the whole leaf, not tiled), marble, lid and iron (the sarcophagus: its
# stone, its bronze lid with the carving stretched over its top, its handles), ivory (the bases of the torch stands);
# marble2 and ivory2 are second versions of those, each sarcophagus or stand wearing one or the other; band (the
# cathedral's blind arches' ring: the wall's bricks stood on end, set round the arch). Every size is a power of two
# (the game wraps textures with bit masks).
#
# The colours (R, G, B) and spreads are the original art's, fully lit:
#   cathedral: walls (77, 62, 62) +-26 (luminance 66: the brightness its walls keep in the floor's colours),
#              floor (47, 52, 60) +-17, the marble shafts' green (39, 54, 55) +-20, the sarcophagus grey 86 +-21 and
#              its lid's bronze (77, 68, 43) +-19
#   catacombs: walls (58, 57, 58) +-29, floor (61, 68, 74) +-18
#   crypt:     walls (64, 61, 73) +-20, floor (59, 32, 21) +-18
#   hell:      the bone of its beams and posts (74, 63, 51) +-33 with their shading (the bone ramps 80-127 of l4.cel's
#              wall pieces); lit evenly, a little brighter: BONE

def cathedral():
    out = {}
    # Blue-grey pavement, and brick, a coping of bigger blocks under the ceiling, the ceiling and the plain stone
    # painted in the pavement's colours (as bright as the original's mauve walls); blue-grey cobbles at the foot of
    # the walls, marble shafts in the original's marble colours, the sarcophagus in its grey and bronze.
    pavement = load('Pavement_01_UV_H_CM_1.jpg')
    floor = match(sized(pavement, 256), (47, 52, 60), 15, 0.2)
    out['floor'] = (blur(floor, 0.4), 1.0, 1.0)
    out['base'] = (blur(match(resize(pavement[:512], 256, 128), (50, 55, 63), 16, 0.2), 0.4), 0.5, 0.25)
    grey = colour_ramp(floor)
    wall = blocks(0.08, 1)
    out['wall'] = (blur(recolor(sized(wall, 256), grey, 66, 22), 0.4), 1.0, 1.0)
    out['arch'] = (blur(recolor(sized(wall, 128), grey, 58, 20), 0.4), 1.0, 1.0)
    out['ceiling'] = (blur(recolor(sized(wall, 128), grey, 51, 16), 0.5), 1.0, 1.0)
    coping = course_band(blocks(0.06, 2))
    out['top'] = (blur(recolor(resize(coping, 256, 32), grey, 76, 18), 0.4), 2.0, 0.25)
    # The ring of the walls' blind arches: the wall's bricks stood on end, at the wall's scale (half a tile across, four
    # bricks; a brick's length, a quarter tile, high), in the wall's colours; the game sets them round the arch.
    out['band'] = (blur(recolor(resize(soldier_course(0.08, 1), 128, 64), grey, 66, 22), 0.4), 0.5, 0.25)
    # The kit's Diablo marble in the original's marble colours on the pillar shafts. The sarcophagus as the original
    # paints it: grey stone (mottled, two versions), the kit's carving on its bronze lid, the kit's iron handles. The
    # torch stands' bases in pale grey worn stone (two versions).
    marble = rank_recolor(sized(load('marble_diablo.tga'), 128), DIABLO_MARBLE)
    out['pillar'] = (blur(marble, 0.6), 0.5, 0.5)
    for slot, name in (('marble', 'Elements_11'), ('marble2', 'Elements_12')):
        out[slot] = (blur(match(sized(sbs(f'Elements/{name}-512x512.png'), 256), SARCOPHAGUS_GREY, SARCOPHAGUS_SPREAD, 0.0), 0.6), 1.0, 1.0)
    out['lid'] = (blur(lid_plate(SARCOPHAGUS_BRONZE, 256, 512, LID_MARGIN), 0.5), 1.0, 1.0)
    for slot, name in (('ivory', 'Stone_07'), ('ivory2', 'Stone_08')):
        out[slot] = (blur(match(sized(sbs(f'Stone/{name}-512x512.png'), 256), IVORY, IVORY_SPREAD, 0.0), 0.4), 1.0, 1.0)
    out['iron'] = (blur(match(sized(seamless(load('Steel_02_UV_H_CM_1.jpg'), 48), 64), (50, 48, 50), 12, 0.3), 0.4), 0.25, 0.25)
    out['stone'] = (blur(recolor(sized(seamless(load('d_column_extr.tga'), 64), 128), grey, 68, 12), 0.4), 1.0, 1.0)
    out['door'] = (blur(door((78, 54, 40), 18), 0.3), 1.0, 1.0)
    return out


def catacombs():
    out = {}
    # Dark grey rubble: the kit's blocks, each stone its own tone, rough with the kit's yellow stone; teal-grey
    # speckled floor (the kit's black marble), round pillars of the same rubble.
    wall = blocks(0.3, 3, depth=0.7, rough=0.35)
    out['wall'] = (blur(match(sized(wall, 256), (58, 57, 58), 24, 0.1), 0.4), 1.0, 1.0)
    out['pillar'] = out['wall']
    out['arch'] = (blur(match(sized(wall, 128), (50, 49, 50), 22, 0.1), 0.4), 1.0, 1.0)
    out['ceiling'] = (blur(match(sized(wall, 128), (40, 39, 41), 16, 0.1), 0.5), 1.0, 1.0)
    out['floor'] = (blur(match(sized(load('Black marble seamless.tga'), 256), (61, 68, 74), 18, 0.15), 0.4), 1.0, 1.0)
    out['stone'] = (blur(match(sized(seamless(load('d_column_extr.tga'), 64), 128), (62, 60, 62), 14, 0.1), 0.4), 1.0, 1.0)
    out['door'] = (blur(door((62, 50, 42), 16), 0.3), 1.0, 1.0)
    return out


def crypt():
    out = {}
    # Hellfire's crypt: big cool grey blocks (the kit's at twice the size), pillars of its white marble gone grey,
    # dark red-brown rough floor (its yellow stone). (Its palette cycles colours 1-31: they are left out.)
    wall = blocks(0.1, 4, depth=0.6)
    out['wall'] = (blur(match(sized(wall, 256), (64, 61, 73), 20, 0.1), 0.4), 2.0, 2.0)
    out['arch'] = (blur(match(sized(wall, 256), (56, 53, 64), 18, 0.1), 0.4), 2.0, 2.0)
    out['ceiling'] = (blur(match(sized(wall, 128), (42, 40, 48), 14, 0.1), 0.5), 2.0, 2.0)
    out['floor'] = (blur(match(sized(load('yellow stone seamless.tga'), 256), (50, 30, 22), 14, 0.15), 0.4), 1.0, 1.0)
    out['pillar'] = (blur(match(sized(load('marble_06.jpg'), 128), (70, 67, 78), 16, 0.1), 0.4), 1.0, 1.0)
    out['stone'] = (blur(match(sized(seamless(load('d_column_extr.tga'), 64), 128), (66, 63, 74), 12, 0.1), 0.4), 1.0, 1.0)
    out['door'] = (blur(door((64, 50, 42), 16), 0.3), 1.0, 1.0)
    return out


BONE = (86, 74, 59)
BONE_STAIN = np.array([0.78, 0.66, 0.55], dtype=np.float32)  # what a stain leaves of the bone's colour: browner


def hell():
    out = {}
    # Hell's flesh, blood and floors are cut out of its own art when a level loads (fp_hell.cpp). Its bone (the beams
    # round the troughs, the spine posts, the horns) is the hell stairs' bone, in the art's bone colours with its
    # brown stains; the skulls and the cut ends of the beams older, darker bone.
    bone = sized(hell_stairs('bone_bump.jpg'), 128)
    spots = stains(128, 5, 13)[..., None]
    stained = lambda rgb: rgb * (1 - spots * (1 - BONE_STAIN))
    out['pillar'] = (blur(stained(match(bone, BONE, 13, 0.25)), 0.4), 0.5, 0.5)
    out['stone'] = (blur(stained(match(bone, (70, 58, 46), 12, 0.25)), 0.4), 0.5, 0.5)
    return out


SETS = {'cathedral': cathedral, 'catacombs': catacombs, 'crypt': crypt, 'hell': hell}


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
