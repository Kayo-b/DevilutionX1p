# 3D asset credits

The mesh files in this folder (`houses.bin`, `housemesh.bin`, `landmarks.bin`, `dungeonkit.bin`) are derived from a
fan-made rebuild of Diablo 1's assets:

- **Source:** "Rebuild of Diablo 1 assets in FBX and OBJ format", Nexus Mods (Skyrim), mod 67266:
  https://www.nexusmods.com/skyrim/mods/67266
- **Contents of the original release:** stairs to hell, all the houses of the town, the fountain, Adria's shack,
  the church, and a set of dungeon pieces.
- **Used here:** the Tristram plaza houses (the cottages, Griswold's smithy, Ogden's tavern), the church exterior,
  and the dungeon kit's entrance stairs (`stairs_01`), and on branch `fp-modkit` its column, sarcophagus (also split into body and lid, for the lid opening), candle stand
  and door, with the kit's stone block, pavement, marble and iron textures (`dungeonkit.bin`). They were converted by the scripts in `fp-tools/export/`:
  voxelized, re-meshed, rescaled and fitted to the original game's tile layout.

## Terms

The author's instructions:

> You may use the meshes any way you like as long as your final product is provided free of charge. You are not
> allowed to use the meshes in commercial products or to sell them.

This first-person mod is free and non-commercial. It must stay that way while it includes these meshes; anything
built from it is under the same restriction.

## Textures

`landmarks.bin` also carries the release's diffuse textures for the town buildings, shrunk to 256 x 256 (converted
to Diablo's palette and darkened in game): the plaza's stone wall, straw roof (outside and inside), timber, door
and fountain stone, and the church's stone wall, slate roof, wooden ceiling, doors, pavement and stained glass.
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

# Dungeon texture credits (`dungeontex.bin`)

`dungeontex.bin` holds the texture sets of the first-person dungeon: the cathedral (levels 1-4), the catacombs (5-8)
and Hellfire's crypt (21-24). Walls, floors, ceilings, pillars, the inside of arches and doorways, and the door leaves
wear them instead of the original game's art. They were cut, scaled to 128 texels per tile, colour graded and softened
by `fp-tools/export/export_dungeon_textures.py` from two packs released under **CC0 1.0** (public domain, free to use,
change and redistribute, commercially or not, with no attribution required; credited here all the same):

- **"Modular Dungeon Kit v1.0"** by Kevin Barany / SigilsVault (05/31/2026), https://sigilsvault.itch.io/ and
  https://sigilsvault.gumroad.com/. License: CC0 1.0 (`LICENSE.txt` in the pack,
  https://creativecommons.org/publicdomain/zero/1.0/legalcode.txt). Used: the trim sheet
  (`TrimSheet_1024x1024.png`: the ashlar block band, the moulded base band, the frieze, the plain stone) and the door
  texture (`Door_Diffuse_256x512.png`).
- **"Tiny Texture Pack 2" (512x512)** by Screaming Brain Studios. License: CC0 1.0 (`License.txt` in the pack: "All
  Screaming Brain Studios assets have been released under the CC0/Public Domain License"). Used: `Tile_03` (cathedral
  flagstones), `Brick_10`, `Dirt_08`, `Stone_05`, `Stone_11` (catacombs), `Brick_17`, `Tile_16`, `Stone_16`,
  `Stone_17` (crypt).

No Daggerfall data and no original Diablo art is in this file.
