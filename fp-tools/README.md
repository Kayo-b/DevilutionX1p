# Diablo + Hellfire First-Person — Modding Guide

This is a first-person mod of Diablo/Hellfire built on DevilutionX. The guide covers how the project is laid out,
how to build and run it, and how each kind of change was made, so you can carry on without me. For the original
design decisions, see `PLAN.md`.

**3D assets:** the meshes come from a fan rebuild of Diablo 1's assets (Nexus Mods, mod 67266), which may only be
used in products given away free of charge. The clean dungeon's textures (`dungeontex.bin`) are that release's dungeon
kit's, under the same terms. Source, terms and credits are in `assets/fp/CREDITS.md`; keep that file up to date when
you add models or textures.

---

## 1. Layout

Repository: `/home/kxyx/projects/DevilutionX1p`, the fork `Kayo-b/DevilutionX1p` of diasurgical/devilutionx.

| Branch | What it is |
|---|---|
| **`fp-buildings`** | **the working branch**: first person plus everything 3D (houses, church, dungeon structures, arcades, doors...) |
| `fp-dungeon-overhaul` | the clean dungeon for the cathedral, catacombs and crypt (fp_dungeon.cpp), on top of `fp-wall-depth` |
| `fp` | the earlier first-person base (`fp-buildings` is built on it, not merged back) |
| `fp-voxel` | paused experiment (voxel monsters) |
| `master` | tracks upstream DevilutionX |

| Path (in the repo) | What it is |
|---|---|
| `Source/fp/` | all first-person code |
| `assets/fp/` | the mod's generated mesh files |
| `fp-tools/run.sh` | build changed files and launch |
| `fp-tools/selftest/` | headless test scripts; output goes to `fp-tools/selftest/out/` (git-ignored) |
| `fp-tools/export/` | Python scripts that turn the 3D models into `assets/fp/*.bin` |

Outside the repo (not committed: big source files):

| Path | What it is |
|---|---|
| `~/Downloads/devilutionX/` | game data: `DIABDAT.MPQ`, `hellfire.mpq`, `hf*.mpq` (override with `FP_DATA_DIR`) |
| `~/.wine/drive_c/GOG Games/Daggerfall/arena2/` | Daggerfall's first-person weapon art (`WEAPON*.CIF`, `ART_PAL.COL`), read at run time from your own copy and **never committed or shipped** (override with `FP_DAGGERFALL_DIR`; empty: no weapon drawn) |
| `~/projects/diablo-hellfire-fp/*.7z, *.rar` | fan-made 3D models (plaza, church, dungeon pieces, Adria's shack, hell stairs) from Nexus Mods 67266; **free, non-commercial use only**: see `assets/fp/CREDITS.md` |
| `~/projects/diablo-hellfire-fp/research/{tristram_plaza,church,dungeon_pieces}/` | the extracted models and textures the export scripts read (override with `FP_PLAZA_DIR`, `FP_CHURCH_DIR`, `FP_DUNGEON_PIECES_DIR`; `export_dungeon_textures.py` extracts the kit textures it misses from `FP_DUNGEON_PIECES_ARCHIVE`, the `.7z`, with `7z`) |
| `~/projects/diablo-hellfire-fp/` | the old working copy (`devilutionx/`, `devilutionx-buildings/`), kept as a backup; `PLAN.md` holds the original design decisions |

All first-person code lives in `Source/fp/` of each checkout:

| File | Responsibility |
|---|---|
| `fp_view.cpp` | the frame: floor/ceiling pass, wall raycaster for town, caves and hell (art faces, recesses, doors), sprite pass, phase timers |
| `fp_dungeon.cpp/.hpp` | **the clean dungeon** (cathedral, catacombs, crypt): tile kinds, texture sets, its floor/ceiling and wall passes, the geometry of its pillars, arches and stairwells |
| `fp_raycast.cpp/.hpp` | camera projection, `CastRay`, what counts as a wall (`IsOpaque`), `WallHeight()`, `VerticalScale()` |
| `fp_textures.cpp/.hpp` | turns each tile piece's iso art into wall-face, floor and column textures; detects pillars, lava and recesses; supplies the filler wall pair (the clean dungeon reads it only to classify tiles) |
| `fp_doors.cpp/.hpp` | doorways (the door tile stays a wall with an opening through it: a clean arched one, or read off the caves' art) and the door leaf's swing |
| `fp_buildings.cpp/.hpp` | **all 3D meshes**: town houses (voxel/mesh), landmarks (smithy, tavern, church), dungeon structures (the clean dungeon's pillars, arches and stairwells, the cathedral's stairs, torch stands, the caves' and hell's columns and arcades, sarcophagus boxes, door leaves), the triangle rasterizer |
| `fp_world.cpp/.hpp` | collects the sprites to draw (monsters, NPCs, objects, items, missiles, corpses, props); sizes and grounding |
| `fp_controls.cpp` | movement, mouse/gamepad look, pitch limit, taking over click-to-walk |
| `fp_pick.cpp` | what the crosshair points at |
| `fp_weapon.cpp/.hpp` | the weapon in hand: reads Daggerfall's `WEAPON*.CIF`, picks one for the equipped Diablo weapon, swings it with the attack |
| `fp_autotest.cpp` | headless scripted runs (`FP_TEST_STEPS`) |
| `Source/lua/modules/dev/fp.cpp` | the `dev.fp.*` console commands |

Assets the game loads come from `assets/fp/` and are listed in `CMake/Assets.cmake`:

| File | Contents | Made by |
|---|---|---|
| `houses.bin` | voxel cottages | `fp-tools/export/export_houses.py` |
| `housemesh.bin` | cottage triangle meshes | `fp-tools/export/export_meshes.py` |
| `landmarks.bin` | every town building: the plaza's houses, smithy, tavern, fountain, and the church | `fp-tools/export/export_landmarks.py` |
| `dungeonkit.bin` | dungeon kit pieces (the entrance stairs, the sarcophagus, the caves' door wood) | `fp-tools/export/export_kit.py` |
| `dungeontex.bin` | the clean dungeon's texture sets (cathedral, catacombs, crypt), made from the dungeon kit's textures | `fp-tools/export/export_dungeon_textures.py` |

---

## 2. Build and run

Configure once (from the repo root, on branch `fp-buildings`):
```bash
mise exec cmake@4.4.3 ninja@1.13.2 -- cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDEBUG=ON \
  -DBUILD_TESTING=OFF -DNONET=ON -DDEVILUTIONX_SYSTEM_LUA=OFF -DDEVILUTIONX_STATIC_LUA=ON \
  -DDEVILUTIONX_DISABLE_EXCEPTIONS=ON -DDEVILUTIONX_DISABLE_RTTI=ON
```
- `DEBUG=ON` defines `_DEBUG`; the console and the headless autotest only exist in that mode.
- `DEVILUTIONX_SYSTEM_LUA=OFF` matters: the system Lua is 5.5, which the bundled sol2 rejects.
- `NONET=ON`: single player only.

Then:
```bash
fp-tools/run.sh          # builds only the files that changed, then launches
fp-tools/run.sh -f       # extra arguments go to the game (-f shows FPS)
```
In game, **F4** toggles first person. Single player only.

**When you add a new asset file**, put it in `assets/fp/` and add it to the `fp/...` list in `CMake/Assets.cmake`.
The build copies it to `build/assets/fp/`.

**Git workflow (fork + upstream):**
- `origin` is the fork on GitHub; `upstream` is diasurgical/devilutionx.
- Work and commit on `fp-buildings`.
- Pull upstream fixes when you want them:
  ```bash
  git fetch upstream
  git checkout master && git merge upstream/master            # keep master equal to upstream
  git checkout fp-buildings && git merge master               # bring them into the mod
  ```
  Conflicts are most likely in the few upstream files the mod touches outside `Source/fp/` (cursor, diablo, engine
  render hooks, `CMake/Assets.cmake`, the Lua dev modules).
- Publish with `git push origin fp-buildings` (and `fp`, `fp-voxel`).

---

## 3. Console commands (debug build)

Open the console with the backtick key, then run `dev = require('devilutionx.dev')`. Useful commands:

| Command | Does |
|---|---|
| `dev.fp.on(true/false)` | first person on or off |
| `dev.fp.teleport(x, y)` | move to a tile (the HUD shows your current tile) |
| `dev.fp.view(yawDeg, pitch)` | set the view angle (0 = +x, 90 = +y) |
| `dev.level.reset(n, seed)` + `dev.level.warp.dungeon(n)` | load dungeon level `n` with a fixed seed (to reproduce a layout) |
| `dev.level.seed()` | print the current level's seed |
| `dev.fp.features()` | list stairs triggers, column tiles, doors and objects |
| `dev.fp.tilemap(x0,y0,x1,y1)` | ASCII map: `#` solid with tall art, `s` solid flat, `a` tall art, `B` building |
| `dev.fp.dumpiso(x0,y0,x1,y1,path)` | save the **original iso art** of a tile rectangle as a PPM image |
| `dev.fp.dumppiece(x,y,path)` / `dev.fp.dumpfaces(...)` | one tile's iso column, or its extracted wall faces (openings in red) |
| `dev.fp.buildings / housemodels / housemeshes / housetextures / recesses (bool)` | switch individual features off to compare (recesses: caves and hell) |
| `dev.fp.cleanmap(x0,y0,x1,y1)` | ASCII map of the clean dungeon's tile kinds: `#` wall, `l` low wall, `o` pillar, `t` torch stand, `D` doorway, `S` stairwell, `~` lava, `^` arch over the tile |
| `dev.fp.dumpfile(gamePath, outPath)` | copy a file out of the game data (a palette: `levels/l1data/l1_1.pal`), for offline tools |
| `dev.quests.activateAll()` | activate every quest: a level and seed then always give the same layout (a new game picks its quests at random, and they change the levels' set pieces) |
| `dev.fp.walkto(x,y)`, `dev.fp.where()` | test click-walks; print the camera position |
| `dev.fp.operate(x, y)` | operate the object at a tile as a click would (open a sarcophagus, a door, a chest) |
| `dev.fp.weapon(file, record, frame)` | show one Daggerfall weapon frame (`WEAPON<file>.CIF`; record 0 rest, 1-6 strikes); no arguments to follow the player again |
| `dev.fp.modeltextures(bool)` | town buildings with the 3D models' own textures (default) or their projected iso art |
| `dev.fp.sarcophagus(bool)` | use the 3D mod-kit sarcophagus model (default on), or a box of its sprite |
| `dev.level.warp.dungeon(0)` | go to town (any `n` for dungeon level `n`) |

Debug environment variables:

| Variable | Does |
|---|---|
| `FP_PHASE_TIMES=1` | logs ms per phase: floor, walls, meshes, sprites |
| `FP_DUMP_BUILDINGS=dir` | dumps each house's art and texture patches |
| `FP_DUMP_PROFILES=1` | logs column profiles |
| `FP_NO_CULL=1` | turns off back-face culling |
| `FP_FLAT_SHADE=1` | draws meshes in one colour, to time rasterization alone |

---

## 4. Testing without touching the desktop

`fp-tools/selftest/headless.sh '<steps>'` runs the game off-screen, clicks through the menu and executes `;`-separated
steps, each one frame apart:

```
wait:N        lua:<console line>      shot:/path.ppm (first-person frame)
iso:/path.ppm (original view; switch with lua:dev.fp.on(false) a few frames earlier)
bench:N       quit
```

The run needs a scratch hero: put a single-player save in `fp-tools/selftest/out/save/` (and a `diablo.ini` in
`out/config/`). The copies in use came from `~/projects/diablo-hellfire-fp/selftest/{save,config}`. `KEYS=` sets how
many Return presses get through the menu (default 3).

`GAME=` chooses the binary (default: the repo's `build/devilutionx`), `ARGS=` adds game arguments. Screenshots and
the log (`headless.log`) go to `fp-tools/selftest/out/`, and saves go to `out/save`, so your real saves are never
touched. Every run saves the game (and its settings): keep pristine copies in `out/save.orig` and `out/config.orig`
and each run starts from them, so the same steps give the same levels. Add `lua:dev.quests.activateAll()` before
`dev.level.reset` too: a new game picks its quests at random, and their set pieces change the layout.

**Hellfire (the crypt, levels 21-24):** `--data-dir` moves where the game looks for the Hellfire mod (`mods/hf`, built
next to the binary), so point `FP_DATA_DIR` at a folder holding links to the MPQs and to `build/mods`
(`levelsweep.sh` makes one, `out/hfdata`, when `HF=1`).

Ready-made scripts:
```bash
T=fp-tools/selftest
TOGGLE=housemeshes $T/town_sweep.sh tag true "66,62 58,72"        # 4 views from each town spot
LEVEL=2 SEED=4242 $T/dsweep.sh tag "55,42 54,37"                  # 4 views from each dungeon spot
LEVEL=1 $T/dungeon_cmp.sh tag 777 "lua:dev.fp.teleport(48,32)"    # FP views + original iso view side by side
$T/levelsweep.sh tag 5 500 "47,26 29,88"                          # every quest active, labelled sheet out/tag_sheet.png
HF=1 GAME=/path/to/old/devilutionx $T/levelsweep.sh old 21 2100 "71,51"   # the crypt, with another build
```
The `.ppm` files can be opened directly, or turned into a contact sheet with Python/PIL (every script does this).

**The working loop:** reproduce the issue with a fixed seed → shoot FP and iso views of the same spot → change the
code → rebuild → shoot again → compare. Run a benchmark before committing:
```bash
FP_PHASE_TIMES=1 fp-tools/selftest/headless.sh "lua:dev=require('devilutionx.dev');lua:dev.fp.on(true);lua:dev.fp.teleport(26,34);wait:8;bench:250;quit"
grep "phases\|bench" fp-tools/selftest/out/headless.log
```
Keep the frame time near or under ~16 ms. The dungeon is currently ~12–14 ms; the town near the church is ~24 ms.

Known test spots:

| Spot | What to look at |
|---|---|
| Town: around (26,34), (34,32) | church |
| Town: (62,67) | smithy porch |
| Town: (55,64) | tavern |
| Level 1, seed 777 | stairs down at trigger (81,78); stairs up near (58,47); column (52,52); torch stand (86,44); sarcophagus (63,48); door (53,78) |
| Level 1, seed 777, all quests | arcade and door (85,62): stand at (85,65), yaw 270; hall with pillars and torch stands (53,52) |
| Level 2, seed 4242 | colonnade: pillars x = 52/58, y = 38–44; view from (55,42) |
| Level 5, seed 500 | catacombs: stairs up (46,25), town stairs (48,31), stairs down (30,87) (look from (30,89), yaw 270, pitch -0.3) |
| Level 21, seed 2100 (Hellfire) | crypt: town stairs (70,50) and stairs down (73,68), both in notches of the wall; pillared hall (59,58) |
| Levels 9 / 13 | caves (lava) / hell: still drawn from their art |

---

## 5. How things work, and how to change them

### 5.1 The clean dungeon: the cathedral, the catacombs and the crypt
`fp_dungeon.cpp` draws levels 1-8 and Hellfire's 21-24 without their iso art: plain stone with new textures, simple
geometry. The art is only read to tell what a tile is.

- **Tile kinds** (`ClassifyTiles`, per level and whenever the map changes: doors, levers; `dev.fp.cleanmap` shows them):
  a solid tile is a `Wall` (a block up to the ceiling, or a low wall as tall as its art), a solid tile with no solid
  neighbour a `Pillar`, one carrying a torch's flame (`OBJ_L1LIGHT`) a `Stand` (the kept 3D torch stand), a door tile
  a `Doorway`; lava in the crypt is `Flat` (its own floor art, colour cycled). `IsOpaque` passes pillars, stands, lava
  and the cathedral's stairs (meshes).
- **Walls** (`DrawCleanWalls`): one ray per column; a face's texture runs along the world axis, so the courses carry on
  from tile to tile. Bands: the texture set's `base` (plinth) at the foot and `top` (frieze) under the ceiling, the
  `wall` texture tiled between. Low walls are looked over, their tops drawn.
- **Openings** (doorways, stairwells): a straight-sided round arch centred on the tile (`CleanOpening`; doors
  `CleanDoorOpening`: `DoorHalfWidth`, `DoorCrown`). A ray entering one is followed through the wall's thickness, solved
  exactly: it leaves through a jamb, the arched soffit (both faced with the `arch` texture), the floor, or the back (a
  doorway: on to whatever is behind; a stairwell: its dark back wall). The door's leaf (fp_doors / `DrawDoorLeaves`)
  hangs in the middle with the set's `door` texture; `IsDoorwayHole` lets aim and sight through an open one.
- **Arches** over walkable tiles (Diablo's special arch cels, `dSpecial`): `BuildCleanGeometry` takes the run of
  walkable tiles under the arch between two supports (walls or pillars, along whichever axis is bounded) and builds a
  slab above a half circle (flattened where it would not fit, `ArchLintel` below the ceiling), as thick as the wall
  between walls, as the pillars in an arcade. In the cathedral an arch resting on a pillar springs from the top of its
  flared block and is as thick as the block's top.
- **Pillars** (`PillarStyleFor`, `addPillar`): in the catacombs (ten-sided) and the crypt (square), a plinth, a shaft
  and a capital, floor to ceiling. In the cathedral, as its art draws them: a plinth, a round (ten-sided) shaft, a thin
  collar, and a block flaring out on top, its sides straight (the art's are curved) from a little narrower than the
  collar to nearly the tile's width, from 1.42 to 2.0 high; over it a square pier goes on up to the ceiling (its sides
  but those inside an arch's masonry). The pillars are built after the arches: under an arch the block is of the
  arch's bricks (`wall`, laid on in the same courses, so it reads as part of the arch), a lone pillar's is of `stone`;
  the pier is of the bricks. The shaft wears the set's `pillar` texture, the plinth, the collar and the capital its
  `stone`.
- **Stairs:** the cathedral's are the kept models (5.6), re-textured with the clean stone (`RetextureClean`). Stairs
  without a model (the catacombs', the crypt's, quest entrances and exits: `IsUnmodelledStairs`) are a stairwell in the
  solid tile beside the trigger with the most solid tiles around it (`FindStairs`): a wide arched opening facing the
  trigger, steps inside going down into a pit (`StairsDrop`, the floor cut open over it) or up into the dark
  (`StairsRise`), darker towards the back. Aiming at it walks to the trigger.
- **Textures** (`dungeontex.bin`, `fp-tools/export/export_dungeon_textures.py`; terms and credits in
  `assets/fp/CREDITS.md`): the Diablo 1 dungeon kit's textures (the Nexus rebuild's `Dungeon Pieces`), worn as the kit
  wears them. The cathedral: its church pavement underfoot, its stone blocks on the walls (a coping of bigger blocks
  under the ceiling, its pavement as blue-grey cobbles at the foot) and the ceiling, its Diablo marble on the pillar
  shafts and its column-head stone on the pillars' plinths and capitals; the blocks, the ceiling
  and the plain stone are painted in the pavement's own blue-greys (`colour_ramp`, `recolor`: each texel takes the
  pavement's colour at the same relative darkness), as bright as the original's walls, and the marble in the colours
  of the original's marble pillar shafts (`DIABLO_MARBLE`, `rank_recolor`: measured on six shafts of level 1 with
  `dev.fp.dumpiso`; the marble's darkest third, its veins and what lies around them, takes their teal, the rest their
  pinkish grey, each texel the colour at its rank of brightness); the catacombs: the blocks as grey rubble (a tone per
  stone, the kit's rough yellow stone over them) on a teal-grey floor of its black marble; the crypt: the blocks at
  twice the size in cool grey, pillars of its white marble, a red-brown floor of its yellow stone. Doors are its planks
  with two iron bands of its steel. The blocks repeat once a tile as on the kit's own walls, and its normal and
  specular maps are baked in as light from above (raised blocks, dark joints, like the original's pre-rendered art).
  The other textures are graded to the colours of their tileset's original art (`match`: the art's mean and spread, measured on
  the iso tiles of levels 1, 5 and 21 with `dev.fp.dumpiso`, in the art's hue). Slots per tileset: `wall`, `base`,
  `top`, `floor`, `ceiling`, `pillar` (shafts), `stone` (plinths and capitals, the tops of low walls, the treads of
  steps; falls back to `pillar`), `arch`, `door`, and for the sarcophagus `marble` (Screaming Brain Studios'
  mottled white, Tiny Texture Pack 2's `Elements_11`, brought down to the original sarcophagus's grey a little darker:
  `SARCOPHAGUS_GREY`, measured on its sprite; falls back to `pillar`) and `marble2` (the same of `Elements_12`), `lid` (its lid's bronze plate,
  `SARCOPHAGUS_BRONZE`, with the kit's carving inset: the saint under an arch of its lid's relief map, lit as relief;
  the plain margin round it, `LID_MARGIN`, colours the lid's edges) and `iron` (its handles, the kit's steel), and `ivory`, `ivory2` (the torch stands' bases: the same pack's worn `Stone_07` and `Stone_08` in a pale,
  faintly warm grey, `IVORY`: lighter than the floor, not so bright it stands out). The pack is CC0, read from `FP_SBS_TEXTURES_DIR`. Each sarcophagus and
  each stand wears one version or the other by its tile (`SecondVersionAt`; the second falls back to the first); 64 to 512 texels a side, powers of two, softened by a light blur.
  When a level loads, `EnsureTextures` converts each to the level palette *once per light level*: the RGB darkened by the share
  of brightness the game's own light tables leave the colours its art uses (`LightFactors`), then the nearest allowed
  colour (`IsAllowedColor`: not the near-blacks 112-127, the UI ramps 128-159, white, nor the crypt's cycled lava 1-31).
  So walls fade smoothly, without the hue speckle of moving texels along palette ramps, and match the sprites' darkness.
  Detail levels (mips) are picked by the footprint of a pixel; up close each pixel jitters its sample point by a 4 x 4
  ordered dither (`Sampler`), which blends the texels instead of showing them as squares. To change a set, edit
  `cathedral()`, `catacombs()` or `crypt()` in the export script and run it (`--preview DIR PALETTE...` writes
  palette-converted sheets; `dev.fp.dumpfile` gets the palettes).
- **Light:** `CleanLightAt` interpolates `dLight` between tile centres (the game's light falls off with distance, walls
  included), and every pass dithers it to the nearest light levels: smooth light, no tile squares. The ceiling is
  `CeilingDarkness` levels darker, undersides and the insides of openings a little darker too.
- **Proportions** at the top of `fp_dungeon.cpp` (drawn tiles: walls are `WallHeight() * VerticalScale()` high, about
  3.1 in the cathedral).

### 5.1.1 Art walls: the caves and hell
- Every solid tile is a cube. Each face uses its tile's own iso art, unsheared into a texture (`ExtractFace` in
  `fp_textures.cpp`).
- Faces the iso camera never drew use the **filler pair**: two neighbouring faces from the level's most common
  straight wall, alternated so their arches stay whole (`GetFallbackFace(parity)`). Hell has no plain wall, so it
  uses each tile's own sparse art with the gaps left open.
- **Wall foot and cap:** a Diablo wall stands on the middle of its tile, half a tile in front of the tile's edge: in iso its
  face reaches 16 rows below the edge line (`FaceBaseRows`), where the plinths and column bases are, and the top of its
  thickness shows as a lit band above it (the cap; `WallCapRows` measures it on the plain wall pair: none in caves and
  hell; the cathedral's 16 rows and the catacombs' 21 still set those levels' wall height). Faces are read down to that foot and drawn without the cap,
  at the art's own proportions (`WallArtScale` is 1). The far end of a face's foot lies outside the piece's diamond and
  is drawn by the next tile along the wall: each piece keeps that part of the previous tile's face (`spillX`/`spillY`,
  `cutX`/`cutY`), and the wall pass takes it from the neighbour. `WallArtRow`/`WallArtHeight` map a height to a face row;
  use them wherever wall art is sampled.
- **Recesses:** blind arches and niches are found by `FindArchOutline`: below the cornice, the first run of dark
  texels in each column (the shadow under the arch's ring), shaped into an arch. They are drawn as a second plane
  behind the wall, with side reveals (in `DrawWalls`). The filler walls carry the plain pair's arch too
  (`GetFallbackRecess`). Deep niches use `NicheDepth` and plain blind arches `BlindArchDepth`.
- **Doors** (`fp_doors.cpp`): the door tile is a wall with an opening through its whole thickness, open or shut, so
  an opened door stays a doorway (`IsOpaque` keeps it; rays inside the opening carry on, and where they meet its sides
  inside the wall they draw the jambs and soffit, faced with the plain wall's stone read across the wall's thickness and up the side or along the top). The opening is `FindArchOutline` on the closed door's art (`DefaultOpening` if none).
  The leaf (`BuildDoorLeaf`, drawn by `DrawDoorLeaves`) is a low-poly slab whose top follows the arch, hinged on a jamb
  in the middle of the wall, textured with the kit's door-wood texture (the clean dungeon: its texture set's `door`), its brightness mapped by `DoorLeafColor` onto the palette's brown ramp using wood colours measured from Daggerfall's `TEXTURE.074` "Door Set" (measured once, nothing read from Daggerfall at run time). When the door's state
  changes it swings (`DoorSwing`: `OpenAngle`, `SwingMilliseconds`) away from the camera's side, and stays open.
- **Heights:** `VerticalScale()` (1.5 in the dungeon) stretches walls, ceiling, structures and tile-art sprites. Art
  heights stay in art units (`SpritePixelsPerTileY` = 39.2 px per unit), and the stretch is applied only when drawing.
- Lava and water count as solid, but they are drawn as floor (`isFlatSolid`). The ceiling is the plain ground
  texture. Hell has no ceiling.

### 5.2 Sprites (monsters, NPCs, objects)
- Sizes are `MonsterScale` and `TownerScale` in `fp_world.cpp`.
- Objects whose sprite is mostly empty below (torch flames) keep the original alignment. Others are "grounded" and
  sink to the middle of their base (`BaseSinkPixels`).
- To make an object a 3D box instead of a billboard, add its type to `IsBoxObject` in `fp_buildings.cpp` (that is how
  sarcophagi work). The box is measured from the sprite.

### 5.2.1 Sprite size and smoothing
- Monsters and townsfolk are sized as if at `sqrt(d² + CreatureNearDepth²)` tiles (`ProjectBillboard`,
  `fp_world.cpp`; 1.3 now): up close they look farther off, far away nothing changes. Walls still hide them by their
  real depth. Raise the constant to push them back more.
- A sprite drawn at 1.5 times its art size or more is sampled "sharp bilinear" (`DrawSpriteBillboard`; the blend spans
  only the middle half between texels, `Sharpen`, so it is half as soft as plain bilinear): its outline is where
  the coverage crosses one half, colours are blended in RGB and mapped back to the palette by `NearestColor` (a cache
  of 32768 entries, rebuilt on palette changes; colours 1-31 are left out because the caves and hell cycle them).
  Smaller sprites keep plain nearest sampling.

### 5.2.2 Upscaled textures up close
- Where the art is blown up (fewer than `UpscaleBelow` = 0.6 texels per screen pixel), walls, recesses, floors and
  ceilings, arches, wall-face and arcade meshes, and structure art are drawn from a copy at `TextureUpscale` (4) times
  the resolution (`UpscaledTexture`, `fp_textures.cpp`): xBR level 1 applied twice (`Xbr2x`). Where an edge runs
  diagonally across a texel's corner, the corner takes half the colour across it, so outlines and shapes come out
  smooth while the texels in between stay sharp. Dithering (checkerboards) is left as it is, by design.
- Mixed colours stay on the palette and keep their hue (`ComputeMixColor`): the middle of the ramp if both colours
  share one, else the nearest colour of either's ramp. Distances and mixes for all colour pairs are tabled once per
  level (`GetColorPairs`).
- Copies are made on first use and cached per level, keyed by the texture's storage (`ForgetUpscaledTexture` when it
  is freed: `BuildStructures` does it for the structure art). Structure art is only upscaled in town, the cathedral
  and the catacombs (`FineCanvas`; in the clean dungeon that is only the torch stands): the caves and hell cycle their
  palettes, which rebuilds their structures every frame. The clean dungeon's textures are not upscaled: they are
  finer than the art to begin with, and blend up close by their dithered sampling (5.1).

### 5.3 Controls
`fp_controls.cpp`:
- **Speed and look limits:** `MoveSpeed`, and `MaxPitch` (the tan of the max up/down angle).
- **Click-to-walk:** walks the game starts itself are taken over (`TakeOverWalk` / `GlideToHeldStep`) so they run at
  FP speed.

### 5.4 Town houses (cottages)
1. The town's wall-art tiles are grouped into houses and given a fitted box-and-roof shape (`BuildAll`, `FitBuilding`).
2. Each house gets the closest-proportioned cottage from the plaza model, stretched to its footprint.
3. Textures are a stone patch and a thatch patch cut from that house's own iso art (`BuildTextures`), scattered over
   the mesh (`ScatteredTexel`).

To re-export the cottage models after changing the scripts (they write straight into `assets/fp/`):
```bash
python3 fp-tools/export/export_houses.py && python3 fp-tools/export/export_meshes.py
```

### 5.5 Landmarks: adding or fixing a hand-placed building
Landmarks (smithy, tavern, church) are full models placed by hand, listed in `Landmarks` in `fp_buildings.cpp`. To
add one, for example Adria's hut:

1. **Extract the model.**
   `7z x ~/projects/diablo-hellfire-fp/"Adria's Shack-67266-.7z" -o ~/projects/diablo-hellfire-fp/research/adria`
2. **List its groups and materials** to find the exterior parts and leave out the interior and anything below ground.
   The church section of `fp-tools/export/export_landmarks.py` shows how. Check the parts by rendering them
   with a small PIL script (draw the triangles from a few angles).
3. **Add it to `fp-tools/export/export_landmarks.py`** (in `LANDMARKS`, or with a loader like `church_triangles()`)
   and run it; it writes `assets/fp/landmarks.bin`.
4. **Find its town tiles:** use `dev.fp.tilemap` and `dev.fp.dumpiso` over the area.
5. **Fit the placement to the original art** with `fp-tools/export/fit_landmark.py`. It projects the model into
   iso and maximises the overlap of its silhouette with the art. First dump the town's upright art (no ground):
   ```bash
   fp-tools/selftest/headless.sh "lua:dev=require('devilutionx.dev');lua:dev.fp.on(true);wait:3;lua:dev.fp.dumpiso(0,0,95,95,'$PWD/fp-tools/selftest/out/town_up.ppm',true);quit"
   ```
   Then fit. For the plaza, one transform places every house (scale 1, flip y); refine each building from it:
   ```bash
   python3 fp-tools/export/fit_landmark.py <obj> fp-tools/selftest/out/town_up.ppm \
     --bbox 61.7,243.0,68.4,248.3 --init 1,-24.794,311.938,0.931 --variants 0,1,0 \
     --margin 12 --fix-scale --steps 0,0.25,0.25,0.05 --overlay out.png
   ```
   For a model from another file, start with `--variants all --search 8` and a rough `--init`. Check the overlay
   image by eye; the printed `placement` is the `x0, x1, y0, y1, zScale` for the `Landmarks` entry (with `flipW`).
6. **Add a `LandmarkPlacement` entry:**
   - `claimMin/claimMax`: tiles whose auto-detected houses are dropped.
   - `x0..y1, zScale, flipW`: the fitted numbers.
   - `eaveIn/zScaleAbove`: lift low eaves without making the ridge too tall (the smithy porch uses this).
   - `posts`: wooden posts under an open roof.
   - `projective = true`: texture it by projecting its own iso art (best for buildings whose model matches the art
     closely, like the church). Otherwise it gets wall/thatch patches.
7. Rebuild and look at it from 4–6 angles. Compare with `iso:`.

Landmarks wear the model's own textures by default (exported with texture coordinates in `landmarks.bin`, converted
to the level palette and darkened by `TextureDim` in `QuantizeMeshTextures`, shaded by the sun in `MakeProjective`).
`modelTextures = false` in a `Landmarks` entry keeps a building on its projected art (the fountain does).
Projective landmarks also bake their art colours once at load (`BakeColors`), used when textures are off. They hide the billboards of tiles under
them and of art tiles that are mostly the building (`ClaimProjectiveTiles`), and draw plain ground underneath.

### 5.6 Dungeon structures
All in `fp_buildings.cpp`, built per level in `BuildStructures`:

| Structure | How it's made | Code |
|---|---|---|
| **Clean pillars, arches, stairwells** (cathedral, catacombs, crypt) | the quads of `BuildCleanGeometry` (fp_dungeon.cpp, 5.1), gathered per 4 x 4 tiles, textured with the set's surfaces by their texture coordinates | `BuildCleanStructures`, `MeshClean` shader |
| **Torch stands** (the cathedral's) and **free-standing columns** (caves, hell) | shape read off the art row by row (the widest pixels give the size at each height; square or round) and turned into a lathe mesh with the art projected onto it, clamped at the ceiling. In the clean dungeon only the torch stands are built this way, and their base is a broad, flat square pyramid of the set's `ivory` instead of what the art reads as (the art's pale pyramid, read as round with the floor around it, came out a wide flared blob): `StandBaseHalf`, `StandBaseHeight`, its texture laid on from above, its sides turned from the art's light (from the left) a light level darker | `ColumnProfile`, `BuildColumn`, `AddCleanPyramid` |
| **Arcade pillars** (caves, hell) | a shaft from the symmetric lower art; neighbouring pillars joined by **arch walls** wearing the filler wall pair, the painted arch stretched to spring from the capitals and span the gap (`FitArcade`), cut open and an underside added | `BuildArcadePillar`, `AddArcadeWall`, `MeshArcade` shader |
| **Stairs down (cathedral)** | built by hand from measurements of the art; the floor is cut open over the pit (the exported kit currently has no down-stair mesh); worn with the clean stone by facing (`RetextureClean`) | `BuildStairsDown`, `FloorHoles` |
| **Stairs up / main entrance (cathedral)** | the kit's `stairs_01` mesh fitted over the stair tiles, worn with the clean stone | `BuildStairsUp` |
| **Sarcophagus** | the kit's model over a box measured from its first (closed) frame; while it opens, the kit's body plus its lid slid off, turned and tipped over the edge, following the object's animation frame (`sarcophagus_body`, `sarcophagus_lid`, exported in the closed sarcophagus's frame). In the clean dungeon it wears the texture set's `marble` or `marble2` (mottled grey stone), `lid` (a bronze plate, the carving over its top, its edges plain: `LidPlainU`, `LidPlainV`) and `iron` handles, told apart by the triangle classes `export_kit.py` writes (`CLASSES`: 1 the lid, 2 iron; by shape, `sarcophagus_parts`: 3 its sunken panels, 4 its hollow), and shaded by a light from above and where less light gets in (`CleanSarcophagus`): its sunken panels and its hollow darker, a shadow under its rim and towards its foot (`RimShade`, `FootShade`: per-pixel ramps over height, `WorldTriangle::occlusion`); elsewhere its sprite's art. Every part of the model is closed and wound outward, so it is one-sided (`AddKitModelOriented` takes each face's normal from its winding), and it is drawn nearest first: close up, that halves the pixels it shades. `dev.fp.sarcophagus(false)` draws the plain box of its sprite instead | `BuildObjectBox` |

To add a new piece from `Dungeon Pieces-67266-.7z`:
1. Add its group name to `PIECES` in `fp-tools/export/export_kit.py` and run it; it writes `assets/fp/dungeonkit.bin`.
2. Load it with `KitMesh("name")`, write a `Build...` function modelled on `BuildStairsUp`, and call it from
   `BuildStructures`.

Mark the tiles a structure replaces in `StructureTiles`:
- **`1`:** drawn as a mesh; rays pass through, there is no billboard, and the floor is plain ground.
- **`2`:** as `1`, plus a floor hole.

Texturing modes for structure faces (`AddStructureFace(..., mapping)`):

| Mode | Texture |
|---|---|
| `MeshProjective` | the iso art, mirrored on the back side |
| `MeshProjectiveCut` | the same, but see-through where the art is empty |
| `MeshPit` | repeated wall art that darkens with depth |
| `MeshFloorArt` | the floor art under the point (the stairs' fan) |
| `MeshWallFace` | one wall-face texture |
| `MeshArcade` | the arch wall |
| `MeshClean` | a surface of the clean dungeon's texture set, by the triangle's texture coordinates (`cleanSurface`, `uv`) |

### 5.6.1 The weapon in hand
- Daggerfall's first-person weapon art, drawn over the view (`fp_weapon.cpp`). The CIF format is described at the
  top of the file. Colours map to the palette's fixed half (128-255), so they are the same on every level.
- Which art for which Diablo weapon: `WeaponFileFor`. Dagger and short sword: 02 (gauntlet and blade); other swords:
  04 (uniques: the gilt 00); axes: 08; maces, clubs, morning stars: 05; flail: 06; war hammer, maul: 07; staves: 01;
  bows: 09; nothing in hand: 10 (fists). 03 (a long blade cut off at Daggerfall's HUD) and 11 (werecreature claws)
  are unused.
- A melee attack plays one of the six strike records (going round them) over Diablo's attack animation; a bow
  plays its seven frames over the ranged attack.

### 5.7 Palette pitfalls (learned the hard way)
- Darkening colours with the game's light tables turns some colours **red**. For mesh shading, use
  `ShadeColor(color, darkness)`, which stays within the colour's own 16-entry ramp. Use `LightTables` only for the
  tile light level.
- Palette entries **112–127** are special near-blacks that also turn red. Run art through `PatchColor()` before
  storing it in textures.

### 5.8 Performance tips
- Turn on `FP_PHASE_TIMES` first to see which phase is slow.
- Structures and the church use **back-face culling**. Faces need outward normals; `AddStructureFace` takes them
  explicitly.
- Big meshes are drawn front to back, and the rasterizer fills row spans. If a new big model is slow, check the
  triangle count, culling, and whether its colours can be baked.
- Kit models (`AddKitModelOriented`, the sarcophagus) are culled by their winding too: a new one must be closed and
  wound the same way all over, or it shows holes (check with `FP_NO_CULL=1`).

---

## 6. Typical workflows

**"Something looks wrong at spot X"**
1. Ask for the level and seed (`dev.level.seed()`) and the tile shown in the HUD.
2. Reproduce it with `dungeon_cmp.sh` or `dsweep.sh` and compare the FP view against the `iso` view.
3. Find what draws it:
   - Is the tile a wall (`tilemap`, `#`/`s`), a structure, or a sprite?
   - Turn features off one at a time with `dev.fp.*(false)`.
4. Fix it, rebuild, shoot the same views again, benchmark, then commit.

**Commit style:** describe the change in plain sentences (see `git log` on `fp-buildings`). One feature or fix per
commit.

---

## 7. Open items / ideas
- **Town:**
  - Adria's hut, Gillian's and the other hand-modelled buildings from the `.7z` archives as landmarks.
  - Forge and anvil props under the smithy porch.
  - Remaining dark slabs on the ground north of the church (possibly original art).
- **Dungeon:**
  - The caves and hell in the clean style (fp_dungeon.cpp is written to take more tilesets: a texture set in the export
    script, a `SetName`, a `PillarStyleFor`, and the tileset in `IsCleanDungeon`); their stairs have no model either.
  - The clean dungeon's floors are one texture: the art's floor markings (blood, the pentagram of Lazarus' lair) are
    gone; wall decorations painted in the art too (banners and torches that are objects stay, as sprites).
  - The black capital top on plain fallback pillars (caves, hell).
  - The mirrored "V" pattern very close to cave walls.
  - `dungeonkit.bin` still carries the kit pieces the clean dungeon no longer uses (column, torch stand, wall models,
    vault, surfaces): re-export it with only `stairs_01`, the sarcophagus and the door to shrink it.
- **General:**
  - Merge `fp-buildings` into `fp` (or make it the main branch of the fork) once you're happy with it.
