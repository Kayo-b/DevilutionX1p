# 3D asset credits

The mesh files in this folder (`houses.bin`, `housemesh.bin`, `landmarks.bin`, `dungeonkit.bin`) and the dungeon's
textures (`dungeontex.bin`) are derived from a fan-made rebuild of Diablo 1's assets:

- **Source:** "Rebuild of Diablo 1 assets in FBX and OBJ format", Nexus Mods (Skyrim), mod 67266:
  https://www.nexusmods.com/skyrim/mods/67266
- **Contents of the original release:** stairs to hell, all the houses of the town, the fountain, Adria's shack,
  the church, and a set of dungeon pieces.
- **Used here:** the Tristram plaza houses (the cottages, Griswold's smithy, Ogden's tavern), the church exterior,
  Adria's shack and her cauldron (`landmarks.bin`, `adria` and `cauldron`), and the dungeon kit's entrance stairs (`stairs_01`), and on branch `fp-modkit` its column, sarcophagus (also split into body and lid, for the lid opening), candle stand
  and door, with the kit's stone block, pavement, marble and iron textures (`dungeonkit.bin`); and the stairs to hell
  ("Diablo's Hell stairs", `hell_stairs.obj`: a rib cage over a pit, the spine going down into it), hell's stairs in
  `dungeonkit.bin` (`hell_stairs`, `hell_ribs`), with its bone texture (`bone_bump.jpg`) in `dungeontex.bin`. They were converted by the scripts in `fp-tools/export/`:
  voxelized, re-meshed, rescaled and fitted to the original game's tile layout.

## Terms

The author's instructions:

> You may use the meshes any way you like as long as your final product is provided free of charge. You are not
> allowed to use the meshes in commercial products or to sell them.

This first-person mod is free and non-commercial. It must stay that way while it includes these meshes and
textures; anything built from it is under the same restriction.

## Textures

`landmarks.bin` also carries the release's diffuse textures for the town buildings, shrunk to 256 x 256 (converted
to Diablo's palette and darkened in game): the plaza's stone wall, straw roof (outside and inside), timber, door
and fountain stone, and the church's stone wall, slate roof, wooden ceiling, doors, pavement and stained glass.
Adria's shack wears the release's weathered planks (`grinda_02`) and its cauldron the release's dirty steel
(`Dirty_Steel_02_UV_H_CM_1`), and the fountain its stone, all graded to the colours of the same buildings in the
original art (measured, none copied) by `fp-tools/export/export_landmarks.py`; the fountain's water is made from
nothing but its colours.
The original author credits the textures (some made seamless by the author, some made from reference photos) to
their creators, who made them available free of charge:

- Dmitriy Chugai, www.texturelib.com
- http://www.lughertexture.com
- http://seamlesstextures.deviantart.com
- http://www.10ravens.com/textures/
- http://www.spiralgraphics.biz
- http://www.mb3d.co.uk/mb3d
- http://texturez.com/
- Deviant Art: borysses, Obsidian Dawn

## Sky

`sky.bin` is the town's night sky: sky 1 of SevBox's BTSX skyboxes with its ringed planet and moon painted out,
graded nearly black, its stars redrawn finer and quantized to the town palette by `fp-tools/export/export_sky.py`.

- **Original assets** (the Back to Saturn X texture resource, https://esselfortium.net/wasd/mysteryproject-tex-phase1.zip):
  Sarah "Esselfortium" Mancuso, Sven "ptoing" Ruthner, moekki, Xaser Acheron
- **Skybox conversion:** SeventhSentinel (SevBox-BTSX v1.0.1)

`hellsky.bin` is hell's dark red haze (levels 13-16 have no ceiling in first person). It is made from nothing but
noise and gradients by `fp-tools/export/export_hell_sky.py`, quantized to hell's palette; no other work is in it.

# Dungeon texture credits (`dungeontex.bin`)

`dungeontex.bin` holds the texture sets of the first-person dungeon: the cathedral (levels 1-4), the catacombs (5-8),
the caves (9-12), Hellfire's crypt (21-24) and the bone of hell (13-16). Walls, floors, ceilings, pillars, the inside of arches and doorways, and the door leaves
wear them instead of the original game's art. They are (but for the four below) the textures of the same release's dungeon kit
(`Dungeon Pieces-67266-.7z`), under the terms above: its stone blocks (`Modern-Cream-Blocks_seamless`, with its
normal and specular maps), church pavement (`Pavement_01_UV_H_CM_1`), Diablo marble (`marble_diablo`), column-head
stone (`d_column_extr`), door planks (`door_diff`, with its normal map), steel (`Steel_02_UV_H_CM_1`), black marble
(`Black marble seamless`), white marble (`marble_06`), yellow stone (`yellow stone seamless`), weathered beams
(`grinda_02`: the caves' fences, gate frames and shoring) and the carving of its sarcophagus lid (`capac_bump`). They were scaled, lit by their normal and relief maps and colour graded to the
original tilesets' colours (measured on the original art, none of which is copied into the file) by
`fp-tools/export/export_dungeon_textures.py`; their creators are the ones credited under Textures above.

Hell's bone (`hell/pillar`, `hell/stone`: its beams, posts, horns, columns and stairs) is the stairs to hell's
`bone_bump.jpg` (same release, same terms), graded to the colour of the bone in hell's art (measured, none copied)
with soft brown stains. Hell's flesh, blood and floors are not in this file: the game cuts them out of the player's
own copy of the original art when a level loads (`Source/fp/fp_hell.cpp`).

Two pairs of the cathedral's textures are not the kit's: the sarcophagus's stone (`marble`, `marble2`) and the torch
stands' bases (`ivory`, `ivory2`) are Screaming Brain Studios' "Tiny Texture Pack 2" (`Elements_11`, `Elements_12`,
`Stone_07`, `Stone_08`, 512 x 512), released under CC0 1.0 (public domain, credit optional), graded by the same
script. So are the caves' rock and ground: their walls, the inside of their stairwells and their stalagmites
(`Stone_13`), the crest along their walls' tops (`Stone_14`), their floor (`Dirt_05`) and their steps (`Dirt_06`).

No Daggerfall data and no original Diablo art is in this file.
