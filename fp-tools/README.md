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
| `fp-cave-overhaul` | the caves (levels 9-12) in the clean dungeon too, on top of `fp-dungeon-overhaul` (hell, 13-16: `fp-hell-overhaul`) |
| `fp-hell-overhaul` | hell (levels 13-16) rebuilt from its own art (fp_hell.cpp, 5.1.2), from `master` |
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
| `fp-tools/d1cli/` | a headless driver for `d1-graphics-tool`'s format code: exports a level's tileset as PNGs (5.1.2) |

Outside the repo (not committed: big source files):

| Path | What it is |
|---|---|
| `~/Downloads/devilutionX/` | game data: `DIABDAT.MPQ`, `hellfire.mpq`, `hf*.mpq` (override with `FP_DATA_DIR`) |
| `~/.wine/drive_c/GOG Games/Daggerfall/arena2/` | Daggerfall's first-person weapon art (`WEAPON*.CIF`, `ART_PAL.COL`), read at run time from your own copy and **never committed or shipped** (override with `FP_DAGGERFALL_DIR`; empty: no weapon drawn) |
| `~/projects/diablo-hellfire-fp/*.7z, *.rar` | fan-made 3D models (plaza, church, dungeon pieces, Adria's shack, hell stairs) from Nexus Mods 67266; **free, non-commercial use only**: see `assets/fp/CREDITS.md` |
| `~/projects/diablo-hellfire-fp/research/{tristram_plaza,church,dungeon_pieces}/` | the extracted models and textures the export scripts read (override with `FP_PLAZA_DIR`, `FP_CHURCH_DIR`, `FP_DUNGEON_PIECES_DIR`; `export_dungeon_textures.py` extracts the kit textures it misses from `FP_DUNGEON_PIECES_ARCHIVE`, the `.7z`, with `7z`) |
| `~/projects/diablo-hellfire-fp/research/hell_stairs/` | "Diablo's Hell stairs" (Nexus 67266) unpacked: `hell_stairs.obj`, `bone_bump.jpg` (`FP_HELL_STAIRS_DIR`; extracted from the `.rar` beside it if missing) |
| `~/projects/diablo-hellfire-fp/research/hell/` | hell's palettes `l4_1.pal` .. `l4_4.pal`, dumped from the game data (`FP_HELL_PAL` for the sky's exporter) |
| `~/projects/d1-graphics-tool/` | the Diablo 1 Graphics Tool (Qt), whose format code `fp-tools/d1cli` builds against (`D1GT_DIR`) |
| `~/projects/diablo-hellfire-fp/` | the old working copy (`devilutionx/`, `devilutionx-buildings/`), kept as a backup; `PLAN.md` holds the original design decisions |

All first-person code lives in `Source/fp/` of each checkout:

| File | Responsibility |
|---|---|
| `fp_view.cpp` | the frame: floor/ceiling pass, wall raycaster for town and the levels drawn from their art (Hellfire's nest: art faces, recesses, doors), sprite pass, phase timers; outlines (as the iso view: the item under the crosshair in its quality's colour; enemies in melee range, next to the player, in red: `OutlineColor`) and the items' name labels (the game's item highlighting key and option, `AddItemToLabelQueueAt`) |
| `fp_dungeon.cpp/.hpp` | **the clean dungeon** (cathedral, catacombs, caves, crypt, and hell's kinds and texture set): tile kinds, texture sets, its floor/ceiling and wall passes, the geometry of its pillars, arches and stairwells, the caves' crest, stalagmites, fences, gates and shoring |
| `fp_hell.cpp/.hpp` | **hell** (5.1.2): its tiles by the map's tiles, the textures cut out of its art (flesh, blood, floors, column crowns), its floor pass and low-wall pass, its bone geometry (beams, spine posts, columns, horns) |
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
| `dungeonkit.bin` | dungeon kit pieces (the entrance stairs, the sarcophagus, the caves' door wood, hell's rib-cage stairs) | `fp-tools/export/export_kit.py` |
| `dungeontex.bin` | the clean dungeon's texture sets (cathedral, catacombs, caves, crypt, hell's bone), made from the dungeon kit's textures and the Tiny Texture Pack's | `fp-tools/export/export_dungeon_textures.py` |
| `hellsky.bin` | hell's dark red haze (a cube map, as `sky.bin`) | `fp-tools/export/export_hell_sky.py` |

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

To start straight on a level, in first person and god mode (no hits taken; a missile or spell kills), set `LEVEL`:
the menu is clicked through for you, the dev commands run (the autotest's steps, section 4), then the game is yours.
```bash
LEVEL=9 fp-tools/run.sh                  # caves, level 9 (0: town; 17-24: Hellfire, its data set up for you)
LEVEL=9 SEED=900 fp-tools/run.sh         # always the same layout (every quest active, as in the selftests)
GOD=0 FP=0 LEVEL=5 fp-tools/run.sh       # without god mode, in the original view
LEVEL=9 SEED=900 STEPS="lua:dev.fp.teleport(53,30)" fp-tools/run.sh   # more steps once there
```
With `LEVEL` set you play a scratch copy of the test hero (`fp-tools/selftest/out/save.orig`, copied afresh to
`out/play-save` each run), so your own heroes are left alone; `REAL_SAVES=1` plays your own (the first hero).

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
| `dev.fp.buildings / housemodels / housemeshes / housetextures / recesses (bool)` | switch individual features off to compare (recesses: the caves' and hell's, and the cathedral's blind arches) |
| `dev.fp.cleanmap(x0,y0,x1,y1)` | ASCII map of the clean dungeon's tile kinds: `#` wall, `l` low wall, `c` wall with a corner cut (the caves'), `o` pillar (the caves': a stalagmite), `t` torch stand, `D` doorway (the caves': a gate), `S` stairwell, `~` lava or water, `f` fence, `^` arch over the tile |
| `dev.level.warp.quest(n)` | go to a quest level (4: the Poisoned Water Supply, on the caves' tiles) |
| `dev.fp.dumpfile(gamePath, outPath)` | copy a file out of the game data (a palette: `levels/l1data/l1_1.pal`), for offline tools |
| `dev.fp.dumpgrid(path)` | write the level's grid, a line per tile: `x y piece solid megatile special object` (the map tile, `dungeon`, is what hell is classified by) |
| `dev.quests.activateAll()` | activate every quest: a level and seed then always give the same layout (a new game picks its quests at random, and they change the levels' set pieces) |
| `dev.fp.walkto(x,y)`, `dev.fp.where()` | test click-walks; print the camera position |
| `dev.fp.pick()` | what the crosshair aims at (a monster, a door, an object, an item), as a click there takes it |
| `dev.fp.operate(x, y)` | operate the object at a tile as a click would (open a sarcophagus, a door, a chest) |
| `dev.fp.townwarps(mask)` | open the town's warps (1 catacombs, 2 caves, 4 hell): the town shows their entrances once entered again |
| `dev.fp.weapon(file, record, frame)` | show one Daggerfall weapon frame (`WEAPON<file>.CIF`; record 0 rest, 1-6 strikes); no arguments to follow the player again |
| `dev.fp.modeltextures(bool)` | town buildings with the 3D models' own textures (default) or their projected iso art |
| `dev.fp.sarcophagus(bool)` | use the 3D mod-kit sarcophagus model (default on), or a box of its sprite |
| `dev.fp.sky(bool)` | the town's night skybox (default) or a flat sky colour |
| `dev.fp.windowlight(bool)` | the lamps inside the tavern, the smithy and the church lit (default) or out |
| `dev.fp.floormap(x0,y0,x1,y1,path)` | the town's floor from above, each tile as first person draws it (PPM, with a grid) |
| `dev.level.warp.dungeon(0)` | go to town (any `n` for dungeon level `n`) |

Debug environment variables:

| Variable | Does |
|---|---|
| `FP_PHASE_TIMES=1` | logs ms per phase: floor, walls, meshes, sprites |
| `FP_DUMP_BUILDINGS=dir` | dumps each house's art and texture patches |
| `FP_DUMP_PROFILES=1` | logs column profiles |
| `FP_NO_CULL=1` | turns off back-face culling |
| `FP_FLAT_SHADE=1` | draws meshes in one colour, to time rasterization alone |
| `FP_NO_HULLS=1` | leaves out the town's prop meshes (boulders, low walls, graves), to time them |
| `FP_LOG_TREES=1` | logs each town tree: its foot and its sprite's size |
| `FP_DUMP_TREES=dir` | writes each town tree's sprite to `dir/tree_N.ppm`, and each group's art before and after cleaning |
| `FP_KEEP_INSIDES=1` | keeps the landmarks' hidden inner surfaces (see `CullHiddenSurfaces`) |
| `FP_DUMP_PROPS=dir` | writes each group of boulders' and gravestones' art (ground taken out) to `dir/` |
| `FP_LAMP_SEEN=1` | shows the lamps' light outside whichever way surfaces face (where their light gets to) |

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
| Level 9, seed 900, all quests | caves: stairs up (37,60), seen from (37,61) yaw 270; lava and stalagmites from (53,30); a gate in a fence (45,30): from (46,30) yaw 180; shoring (62,66) yaw 270 |
| Level 10, seed 1000, all quests | caves on a green palette; lava pools from (62,27) yaw 150 |
| Quest level 4 (`dev.level.warp.quest(4)`) | the Poisoned Water Supply: the caves' tiles, water, its own palette |
| Level 13, seed 1300, all quests | hell: start by the stairs up's rib cage (53,64); troughs of blood (50,58) yaw 0 and 270; a room of flesh walls (62,64); stairs down (trigger 32,41) seen from (34,41) yaw 180; spikes (42,24) |
| Level 16, seed 1600, all quests | Diablo's lair: the ring of spine columns from (36,70) yaw 200-340; the bone pentagram and Diablo (73,77) yaw 290 |
| Level 15, seed 1500, all quests | the red pentagram (36,28) yaw 270 |

---

## 5. How things work, and how to change them

### 5.1 The clean dungeon: the cathedral, the catacombs and the crypt
`fp_dungeon.cpp` draws levels 1-12 and Hellfire's 21-24 without their iso art: plain stone with new textures, simple
geometry. The art is only read to tell what a tile is. (The caves differ in places: 5.1.1; hell has a kit of its own: 5.1.2.)

- **Tile kinds** (`ClassifyTiles`, per level and whenever the map changes: doors, levers; `dev.fp.cleanmap` shows them):
  a solid tile is a `Wall` (a block up to the ceiling, or a low wall as tall as its art), a solid tile with no solid
  neighbour a `Pillar`, one carrying a torch's flame (`OBJ_L1LIGHT`) a `Stand` (the kept 3D torch stand), a door tile
  a `Doorway`; lava in the crypt is `Flat` (its own floor art, colour cycled). `IsOpaque` passes pillars, stands, lava
  and the cathedral's stairs (meshes).
- **Walls** (`DrawCleanWalls`): one ray per column; a face's texture runs along the world axis, so the courses carry on
  from tile to tile. Bands: the texture set's `base` (plinth) at the foot and `top` (frieze) under the ceiling, the
  `wall` texture tiled between. Low walls are looked over, their tops drawn.
- **Blind arches** (the cathedral; `FindBlindArches`, drawn in `DrawCleanWalls`, constants `BlindArch*`): as its art
  draws them along every wall (one every two tiles there, 1.4 wide, the crown about 2.6 up), every stretch of
  full-height wall face with open floor in front, on either side of a wall, has a row of round-topped arches spread
  evenly about `BlindArchSpacing` (1.7) apart, so they come out whole at its ends, each 1.02 wide (narrower where a
  short stretch leaves too little wall between them), from the top of the base band up, ringed with the wall's bricks
  set on end like voussoirs (the set's `band`: `soldier_course` in the exporter; as many whole bricks as fit round it). They are drawn flat on the face and shaded to look sunk in: a
  rim of `arch` stone just inside the outline, a `stone` sill, a back a light level darker with its bricks shifted off
  the wall's, darker still under the top. Drawn as real 3D recesses (rays followed into them) they cost about 2 ms with
  a wall of them on screen; flat, about 1 ms, the ring's place on its band worked out every `RingStep` rows and drawn
  straight in between. `dev.fp.recesses(false)` turns them off.
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

### 5.1.1 The caves
The caves (levels 9-12, and the Poisoned Water Supply) are the clean dungeon's fourth tileset, with their own pieces
(constants at the top of `fp_dungeon.cpp`, geometry in `AddCaveGeometry`):

- **No ceiling** (`CleanHasCeiling`): the dark above, as the original shows nothing over its cliffs. The walls are
  `CaveWallHeight` (2.5) high instead of their art's four tiles, so their tops stay in sight.
- **Rock walls:** flat faces of rock (the set's `wall`), shaded darker towards their top (`CaveTopShadow`), each way a
  face looks shifted along by its own `CaveFaceShifts`, so inner corners and the two sides of a passage do not mirror
  each other.
- **Cut corners** (`FindChamfers`, `c` in `cleanmap`): a rock tile with open ground on two sides and between them,
  the rock going on behind it both ways, has that corner cut off along the diagonal, so cliffs stepping across the
  grid run diagonally and the corners of rock round off (not on timber or stairwells). The tile stays a wall for the
  rays: the wall pass follows a ray into it to the diagonal (`DrawCleanWalls`), or on through the corner cut off.
- **The crest** along the top of the rock: a ragged rim of darker rock (`crest`) jutting out over the face
  (`CrestOut*`), its lip hanging down (`CrestDrop*`), its top edge rising into the dark (`CrestRiseMax`), its middle
  bulging. It follows the outline of the rock (`AddCaveGeometry`: an edge along each open face, a cut corner's
  diagonal instead of its two faces); each point's shape comes from where it is (`CaveNoise`, `CrestSegments` points a
  tile), so edges share their corner points, and there both rims are pushed out to meet (`mitre`, at any angle). Over
  a stairwell's opening the lip stays high (`CrestOverOpening`). About 18 triangles a tile of edge.
- **Roots** hang from the crest's lip on a few of its segments (`Root*`): one to three thin strands swaying out from
  the rock and wandering along it, each two ribbons crossed, seen from both sides (`CleanQuad::twoSided`).
- **Lava and water** (`Flat`): solid pieces whose floor art is in good part the colours the level cycles
  (`IsCycledFloor`), so whole pools are found, not just their edges. They wear the art of the level's commonest piece
  that is lava or water through and through (`FindCleanPools`: the original paints dark crust islands in its pools,
  flat blotches up close), its empty texels filled, still in the cycled colours (so still animated), bent by smooth
  noise as it is laid on (`PoolWarp`, worked out every 8 pixels of a row) so it flows and has no straight seams, and
  sampled at a dithered point up close, glowing (`PoolGlow`: drawn brighter than the light there, its colours never
  going black). Their shape is drawn round a skeleton (`PoolCover`): `PoolRadius` about their tiles' centres and the
  lines between neighbouring ones along the grid (and to the rock they run up to), `PoolJoinRadius` about the lines
  between diagonal neighbours, so a river running diagonally across the grid (as the caves' do, a staircase of tiles
  touching at their corners) is one stream, not a row of patches; there it reaches onto a corner of the ground beside
  it, where the game will not let anyone step across anyway. The shore wanders `PoolRagged` either way (`PoolNoise`):
  no straight lines or right angles; a thin dark crust of cooled rock along it (`PoolCrust`).
- **Stalagmites:** lone rock tiles (`Pillar`), a leaning spike of rings narrowing to a tip, most with one or two
  smaller ones beside it (`Stalagmite*`).
- **Fences** (`Fence`, `f` in `cleanmap`): the solid pieces of the megatiles the generator lays fences with (130-140,
  146, 147; `dungeon`, which the automap keeps) whose art is sparse (`ArtDensity` under `FenceArtDensity`: a fence's
  end shares its megatile with the rock it runs into). Posts at each tile's middle and edge and two rails
  (`Fence*`), along their fence neighbours, the rails reaching into the rock or the gates they meet. Rays pass them.
- **Gates** (`CleanDoorwaysAreGates`): the caves' doors stand in their fences. A doorway there is no wall
  (`IsOpaque`): a frame of two posts and a lintel (`Gate*` in fp_dungeon.cpp) round the door leaf (fp_doors), whose
  opening is flat-topped (`CleanOpening::flat`). The leaf is a see-through lattice of the fences' beams
  (`BuildGateLattice`, `Gate*` in fp_buildings.cpp): two stiles, pickets, three rails and a brace, wearing the `wood` by
  each beam's texture positions. With no wall face to aim at, `PickAt` (fp_pick.cpp) selects a gate where the
  crosshair's ray crosses the line through its tile's middle below its top; an open gate only when nothing else is
  aimed at (what is seen through it comes first).
- **Shoring:** the megatiles of timber propping a cliff (121-129, 150-152; their pieces alternate timber and rock
  along the cliff, so all are drawn as rock): posts and two beams along the rock's open faces (`Shoring*`).
- **Stairwells:** as in the other tilesets (5.1) but like a mine's: their opening flat-topped (`CaveStairsTop`), in
  a timber frame of two posts and a lintel standing out of the face (`StairsFrame*`), their steps of wood.
- **Colours:** the levels draw one of four palettes at random (red-brown, green, ...). The rock and the floor take
  their hue from the level's own art (`ArtHue`: its commonest wall face, its commonest floor), each texel keeping its
  brightness and a little of its own colour (`OwnChroma`), so a green cave is green. The palette cycles colours 1-31
  (lava): no texture uses them (`IsCycledColor`).
- **Textures** (`caves()` in the export script): the Tiny Texture Pack's craggy `Stone_13` on the walls (512 x 512 for
  two tiles, sharpened: `sharpen`, the pack's textures are soft up close), the stairwells' insides and the
  stalagmites, its `Stone_14` darker on the crest, `Dirt_05` underfoot (512 for two tiles, sharpened) and `Dirt_06`
  on steps; the kit's weathered beams (`grinda_02`) on the fences, gate frames and
  shoring, its plank door on the gates. Graded to the art of level 9 (seed 900): walls (39, 29, 26), floor
  (55, 42, 38).
- **Speed:** level 9 ran at about 410 ms a frame, the lava's colour cycling rebuilding all the art structures every
  frame (the palette fix: 5.7); drawn clean, the caves take 17-26 ms (1280 x 720, headless), about 26 ms looking down
  at a pool and 32 ms right in front of a stairwell (the wall and its frame fill the view).

### 5.1.2 Hell (levels 13-16)
`fp_hell.cpp` draws hell as part of the clean dungeon (`IsCleanDungeon` takes it in; `IsHell`). Its art is sculpture,
not masonry, so its kit is built from that art and drawn low, under the eye, open to a dark red sky.

- **Tiles** (`ClassifyHellTiles`, called by `ClassifyTiles`): by the tile of the level's map each tile belongs to
  (`dungeon`, `MegatileAt`; ids from 1 as in `l4.til`, `ClassOf`), not by its art: 18-30, 116-128 and 137 are troughs
  of blood (30 also the lake round the rooms and past the map's edge), 31-46 and 129-136 the stairs' rib cages,
  95-97 bone spikes, 47-48 floor with a wall's shadow painted on. A solid tile of any other is a flesh wall, or a
  spine column if it is lone. Walls are `CleanKind::Wall` at `HellWallTop` (0.92; the art's are about 1.8 tiles of
  art, which stretched like the other levels would hide the blood); blood on top of the troughs, the lake, and the
  flesh walls along them (`IsHellBlood`). Spikes and rib cages are `CleanKind::Prop`. `dev.fp.cleanmap` shows `~`
  for walls with blood on top, `p` for props.
- **What is cut out of the art** when a level loads (nothing of it is shipped): the flesh of the walls' sides
  (`CutFlesh`: two panels, under the beams of the left faces of map tiles 1 and 63's top pieces, each with braided
  bone veins, 20 x 35 texels; laid as the first, it mirrored, the second, it mirrored, cross-faded where they meet,
  they repeat every five tiles without a seam; slot `wall` of the `hell` set, in the level's palette); the blood (`CutBlood`: the top of the lake's map tile put together from its four pieces,
  `ComposeMegatile`, unrotated to 64 x 64 over 2 x 2 tiles; kept in the art's colours 1-31, which hell cycles through
  its light tables, so it shimmers as in the original, and `BloodGlow` light levels brighter); the floors (each
  tile its own floor art, the painted shadows and the floor under rib cages and columns the plain floor's,
  `FloorPieces`); the spikes' billboards without their ground (`HellPropArt`); and the columns' crowns (`CrownFor`:
  the crossed bones and horns above the narrow neck of the column's spine, in its map tile's art, drawn as a
  billboard on the column's mesh spine, `CollectHellCrowns`, at `ColumnArtScale`); the wall posts' crowns
  (`CutPostCrown`: the lyre of horns and the knob where the beams cross on map tile 9's corner post, without the art's
  beams, drawn on a card facing out of the wall, `HellSprite`, `MeshSprite`); and the spikes' horns (`TraceSpikes`:
  each horn followed up its art row by row, its middle and width, as a tapering tube).
- **Rows of hell's art:** the game draws only 12 of hell's 16 micros a piece (`MicroTileLen`), so its columns are
  192 rows, not the files' 256: count fixed rows of the art up from its bottom (`PostCrownTop` and the rest), never
  from the top as the Graphics Tool's PNGs show them.
- **Each hell level has one of four palettes** (`l4_1` .. `l4_4`: the bone tan, pink, grey-blue or olive). What is
  cut from the art follows it; the bone texture from `dungeontex.bin` is scaled from `l4_1`'s bone to the level's
  (`HellTintTexture`).
- **Floor** (`DrawHellFloor`): the art per tile, lit smoothly like the clean dungeon (the game's light tables, so
  the colours stay the art's), drawn from its upscaled copy up close; nothing above the horizon (the sky's). It is
  drawn after the walls, only where they leave it to be seen.
- **Walls** (`DrawHellWalls`): every wall is lower than the eye, so each column is walked through the grid once,
  front to back: a wall entered from the floor draws its side (flesh, `FaceYShade` darker along y), then its top up
  to where the ray leaves it (blood, or flesh a light level darker), each only above what is drawn already. The
  lake past the rooms is the same blood, in the dark.
- **Bone** (`BuildHellGeometry`, quads handed to the mesh pass as the clean dungeon's): beams (round, `BeamRadius`)
  along every run of wall edge facing anything but wall, running on past a wall's end like crossed logs; spine
  posts (vertebrae under the art's crown, on a card facing out of the faces they stand on, a corner's between its
  two) at both ends of each run and about every `PostSpacing` along it, each corner once; lone columns (a taller
  spine on a spread foot, crowned by the art's crown); the spikes' horns, standing across the view as the art draws
  them, leaning towards it and away by turns. `addTube` makes every round part (a ring of quads along a path, its
  frame carried along it). A card is made only of the rectangles of its cut-out that have art (`HellSprite::rects`,
  bands six texels wide), so its clear texels cost nothing; up close its outline is smoothed (bilinear coverage).
- **Stairs** (`BuildHellStairs`, fp_buildings.cpp): the stairs to hell model (`hell_stairs` in `dungeonkit.bin`: the
  rib cage, the rims round its pit, the spine going down into it, the ribs under the floor) over each stairs down,
  the floor cut open over its pit; the cage and rims alone (`hell_ribs`) over the stairs up and to town. Laid on the
  middle of the flight's tiles (all the stairs tiles touching those by the trigger), its entrance towards the
  trigger, `Scale` 0.6 tiles a model unit, in the bone of the set. The pit under a stairs down has flesh sides
  going down in bands, darker the deeper, and a floor at the bottom, all but black.
- **Sky:** `hellsky.bin` (fp_sky.cpp: `FP_HELL_SKY`), remapped to the level's palette without the cycled blood
  colours. **Picking** looks over the low walls (`IsHoleAt`), and there is no ceiling to stop it.
- **Distance:** the bone is drawn only within `HellMeshReach` (26 tiles; beyond, it stands in the dark) and nearest
  first (`DrawStructures` sorts every level's structures so: the depth test then turns hidden pixels away first).
- **Performance** (Ryzen 5 5600H, 1280 x 720): 15-25 ms a frame in hell (the cathedral about 17, the catacombs
  14): floor 2-4, walls 2-5, meshes 6-14 (a post's crown or the rib cage right in front of the camera cover much of
  the screen), sprites 1-9 (Diablo's lair is crowded). What paid: walls before the floor; posts' crowns as cards made
  of their art's rectangles (not horn meshes); `FloorToInt` instead of `std::floor` in per-pixel loops (the build
  targets plain x86-64, where `std::floor` is a call into libm; this sped up the cathedral too); small clean
  triangles lit once a frame at their middle (`SmallForLight`, `CleanTriangleLight`); big structures drawn front to
  back within themselves.

**Getting at the art.** `fp-tools/d1cli` builds the Diablo 1 Graphics Tool's format code (`~/projects/d1-graphics-tool`,
Qt 6) into a command line exporter: every subtile, map tile (2.5D) and CEL frame of a level tileset as PNG, with
`subtiles.csv` (each subtile's SOL flags and frames) and `tiles.csv` (each map tile's automap type and subtiles):
```bash
mise exec cmake@4.4.3 ninja@1.13.2 -- cmake -S fp-tools/d1cli -B /tmp/d1cli -G Ninja && mise exec cmake@4.4.3 ninja@1.13.2 -- ninja -C /tmp/d1cli
fp-tools/selftest/headless.sh "lua:dev=require('devilutionx.dev');lua:dev.fp.dumpfile('levels\\l4data\\l4.cel','/tmp/l4/l4.cel');..."  # also .min .til .sol .amp _1.pal
QT_QPA_PLATFORM=offscreen /tmp/d1cli/d1cli /tmp/l4/l4.cel /tmp/l4/l4_1.pal /tmp/l4x
```
Subtile n of the tool is piece n - 1 of the game (`dPiece`, `GetPieceTextures`); a map tile's pieces are its top,
right, left and bottom (`DRLG_LPass3`). The cuts above were found that way: compare the PNGs, then cut the same
pieces at run time from `GetPieceTextures(piece).column`.

### 5.1.3 Art walls: Hellfire's nest (levels 17-20)
The one tileset still drawn from its art (the caves and hell were, before `fp-cave-overhaul` and `fp-hell-overhaul`).

- Every solid tile is a cube. Each face uses its tile's own iso art, unsheared into a texture (`ExtractFace` in
  `fp_textures.cpp`).
- Faces the iso camera never drew use the **filler pair**: two neighbouring faces from the level's most common
  straight wall, alternated so their arches stay whole (`GetFallbackFace(parity)`).
- **Wall foot and cap:** a Diablo wall stands on the middle of its tile, half a tile in front of the tile's edge: in iso its
  face reaches 16 rows below the edge line (`FaceBaseRows`), where the plinths and column bases are, and the top of its
  thickness shows as a lit band above it (the cap; `WallCapRows` measures it on the plain wall pair: none in the
  caves; the cathedral's 16 rows and the catacombs' 21 still set those levels' wall height). Faces are read down to that foot and drawn without the cap,
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
  texture.

### 5.2 Sprites (monsters, NPCs, objects)
- Sizes are `MonsterScale` and `TownerScale` in `fp_world.cpp`.
- Objects whose sprite is mostly empty below (torch flames) keep the original alignment. Others are "grounded" and
  sink to the middle of their base (`BaseSinkPixels`), but chests: their bottom edge is a shallow V that would all go
  under the floor, so their front corner stands on it.
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
  and the catacombs (`FineCanvas`; in the clean dungeon that is only the torch stands): not in the caves, whose art
  cycles its lava colours, which the smoothing would mix as they stand at one moment, nor in hell. The clean
  dungeon's textures are not upscaled: they are finer than the art to begin with, and blend up close by their
  dithered sampling (5.1).

### 5.3 Controls
`fp_controls.cpp`:
- **Speed and look limits:** `MoveSpeed`, and `MaxPitch` (the tan of the max up/down angle).
- **Click-to-walk:** walks the game starts itself are taken over (`TakeOverWalk` / `GlideToHeldStep`) so they run at
  FP speed.

### 5.4 Town houses (cottages)
1. The town's wall-art tiles are grouped into houses and given a fitted box-and-roof shape (`BuildAll`, `FitBuilding`).
   A group made only of tree pieces (`IsTreePiece`: a dense crown passes for wall art) is no house: beside the
   stream south-east of the town, the solid stream tiles made it a footprint, and a cottage stood over the river.
2. Each house gets the closest-proportioned cottage from the plaza model, stretched to its footprint.
3. Textures are a stone patch and a thatch patch cut from that house's own iso art (`BuildTextures`), scattered over
   the mesh (`ScatteredTexel`).

To re-export the cottage models after changing the scripts (they write straight into `assets/fp/`):
```bash
python3 fp-tools/export/export_houses.py && python3 fp-tools/export/export_meshes.py
```

### 5.4.1 Town at night: trees and lamplight
**Trees** (`BuildTrees`, `CollectTreeSprites`): each group of tree tiles is cut out of the iso art into one sprite per
trunk (every art pixel goes to the nearest trunk whose foot is below it). A tree's tiles that a house's art claimed
still count as the tree's (`IsTreePiece`), so trees beside houses are whole. The feet of the trunks are drawn in the
floor diamonds in the ground's own colours: they are grown down from the bark above, row by row, through the art
with its ground, never wider than the trunk above (nor than 9 pixels). The rest of the diamonds (ground, shadows,
fallen leaves, twigs) is left out, and a trunk is a piece of bark that reaches the ground. A tree stands on its trunk's foot as an
upright quad turned to face the camera, each screen column at its own depth (`Entity::facing`,
`DrawFacingSprite`), so a wide tree beside you recedes instead of filling the view. Boulders, low walls, graves and
tufts stay meshes (`BuildTownProps`).

The landmarks' model surfaces no one can see from outside (insides of walls and roofs, floors, beams behind closed
doors and glass) are dropped at load (`CullHiddenSurfaces`: a voxel grid of the model, and rays out from each
surface; about a quarter of the houses' triangles, in about half a second). What can be seen from where one
stands around a house (rays from eye height all round it, a tenth of a tile apart where they reach it) stays too:
the insides seen through small holes, like the gap in Gillian's thatch, which a surface's own few rays can miss.
The church is left whole (its glass shows through slits finer than the grid). `FP_KEEP_INSIDES=1` keeps every
surface, to compare.

Ogden's tavern door stands open (`PLAZA_OPEN_DOORS` in the exporter swings it out on its left edge, as the art has
it). Boulders beside a building that the original art shows (not drawn over by the building's art) stay props even
where a replaced house's art claimed their tiles (`ShowsInArt`, `Building::claimed`).

**Boulders, gravestones, low walls** (`BuildRocks`, `BuildGraves`, `BuildTownWallTile`): groups of neighbouring rock
or grave tiles are cut out of the art together, without the ground in their floor diamonds (`BuildPropCanvas`: only
the stones' blue there). A heap of boulders is a few big stones (`PackStones`): the original draws a boulder
about every `BoulderArtArea` (640) pixels of its heap, and there is a stone for every two of them (one for up to
three, two for four or five, ...); the same for the field of boulders round the town's west side. Where they lie:
the biggest circles inside the heap's outline, then a few rounds of k-means over its pixels so that between them
they cover it; each as big as a disc of its pixels' area (`StoneFill`: a little more, so they overlap), its middle
on the ray of the disc's (the iso view has the same scale across and down). Each is a lumpy low-poly stone
(`AddStone`: a squashed sphere pushed in and out by a few waves, 80 triangles near, 20 far), its flattened underside
on the ground, or up on the stones in front where its disc is above the heap's foot, never higher than the biggest,
and only if a stone placed before it lies under it there (`supportAt`); else as high as one does, or on the ground,
further back along the ray (stones used to hang in the air over the field where nothing was under them). (The town
had 2822 stones, most of them small fillers; it has about 815.) Heaps of more than 600 triangles are drawn nearest
first, as the buildings are. A gravestone is made as
what its outline is (`ShapeGrave`): a headstone (round-topped where its top corners are cut), a cross, a post or a
slab fallen flat, as wide as its body and standing on the ground under its foot (the ground's removal leaves a
headstone's shaded foot a thin stem, and cuts some in two: pieces one above the other are put back together). Both
take the art's colours where the iso camera saw them, and are drawn from the front only. The low walls are dry stone
walls along their runs, narrowing upwards, their top running on from tile to tile at about the walls' common height,
in the plaza cottages' stone texture turned dark neutral grey (`MeshTiled`, `StoneTexture`, `WallStoneColor`:
the palette's true greys only, `WallStoneGrey`); pieces 1049 and 1050, a wall's crumbled end, are wall too (not
gravestones). Each straight run stands at one place across its tiles, the median of where its tiles' art has it
(`WallAcross`, `WallArtAcross`: tiles of one run differ, and placed one by one they left offset pieces with gaps
between), and at a corner a wall ends in the one it meets. Corners the art draws across two diagonal tiles (the
walls ending either side of an empty tile) get that tile as their corner, so the walls meet.

**The caves' entrance** (`BuildCaveMound`; the town's warp to level 9, its trigger at (17, 69)): a mound of 15 big
boulders (`CaveStones`, laid out round a middle `CaveMoundBack` behind the trigger tile's corner, along the mouth's
axis and across it) about 2.6 tiles high, as the art draws it: a heap of big angular boulders with a dark mouth low on
its south-east side, onto the trigger's tile. The mouth is carved through the stones (`StoneCarve` in `AddStone`):
a ragged half ellipse (`CaveMouthHalf`, `CaveMouthTop`, `CaveMouthRagged`) whose inside is dark rock, black deep in,
the faces the stones turned out of it dropped, a black back and floor. While the warp is closed (the art then fills
the mouth with boulders), one more boulder blocks it. Its tiles are the boulder tiles within `CaveMoundReach` and the
mouth's own pieces (1199-1212, which `IsTreePiece` would otherwise take for trees); its stones take their colours
from the art of those tiles (`BuildPropCanvas`), and the field round it is packed as before.
`lua:dev.fp.townwarps(7)` and entering town again (`dev.level.warp.dungeon(1)` then `(0)`) opens the warps to test it.

**Lamplight** (the `LitLandmarks` section of `fp_buildings.cpp`):
1. Every house is lit. The tavern, smithy and church have rooms measured by hand; the cottages' are fitted to their
   walls from inside (`autoRoom`, `AddAutoRoom`). Each lit landmark has rooms (`LitLandmarks`, in the model's frame). `FitRoom` moves
   their sides out to the model's walls and puts their ceiling under its roof; `FindStoreyFloor` splits a room at an
   upper floor the model has (the tavern has one at about 2 tiles). The church has its nave inside the stained-glass
   box (`AddGlassRoom`) and its tower. Its front door stands open (`export_landmarks.py` swings the leaves and their
   battens in) onto a small entrance hall (`entrance_hall`, class 3; the nave itself is not modelled inside), lit by
   a lamp of its own (`AddInsideLamp`, `LandmarkInsideLight`), whose light comes out of the doorway.
2. Lamps hang under each room's ceiling (`AddRoomLamps`), at most `LampHang` above its floor, and one more every
   `LampStack` up a tall room (the tower, one for each band of its openings).
3. `BuildLampGrids` (on a thread of its own, about 2 s, after the town is built) voxelizes every town mesh
   around each building (`Occluders`, 1/16 tile; the church's glass lets `GlassTransmission` through) and, at the
   points of a grid around it (1/8 tile across, 1/4 up), sums each lamp's light that reaches there (a few points of
   its flame, so the edges are soft) as a vector towards the lamps. Inside the rooms it stays dark. Each opening in
   a room's walls (`AddWindowLamps`, found by looking out from the room's sides through the voxels) gets a small
   light of its own just outside, shining out, for the glow around and under it.
4. `ApplyWindowGlow` lights each pixel by the grid at its point, dotted with the surface's normal (from the depth of
   the pixels around it): so walls and roofs facing away from the lamps, the building's own included, get nothing.
   It is worked out every 4 pixels and blended between them on smooth surfaces (about 2.5 ms a frame).
5. Through the windows one sees the room's inside (`AddRoomInside`: plank floor, beamed ceiling, panelling and
   plaster), lit by its own lamps (`InteriorColor`). The church's glass, seen through its cross windows, glows from
   deep red through orange to yellow as the original art's lit windows do (`GlassGlow`). Behind each cross window
   is a red lamp shining out (`AddCrossLamps`, `CrossLamps`): it lights the window's sides (class 4 in
   `landmarks.bin`, `WorldTriangle::rim`: `cross_rim` in the exporter), their stone in its light (`CrossLight`:
   the sides turned to it and nearest it brightest), and the ground before the window. The sides are left out of
   the light's occluders, or the openings, finer than the grid, would be closed to it.
6. The art's painted lamplight on the ground before the tavern and the church is replaced by plain ground
   (`ClearPaintedLight`).
7. Windows are glazed: a glowing pane (`MeshPane`, `PaneColor`: the house's light, brightest in the middle of each
   light) in a dark oak frame of four lights, a border and a cross (`MeshFrame`, `AddWindowPane`). Window holes the
   models have are found by looking out from the rooms' walls (`FindWindows`; doors and the smithy's open front are
   left open), and each edge fitted to the hole's by finer rays (also past the room's ceiling, which can be lower
   than a window's top under the eaves). A house whose model has none gets a few set into its ground floor's walls (`AddSetWindows`: one per
   wall, two in a long one, sized to fit under its eaves, clear of its door), each with a light just outside. The panes let
   `WindowGlassTransmission` of the lamps through, and the frames cast their cross on the ground. The glass glows at
   `PaneBrightness` (half) of its light's colour, in warm palette colours only (`NearestWarmColor`; the nearest of
   all can be an olive). The little windows in closed doors (Pepin's, Gillian's grilles) get a pane behind the leaf
   (`GlazeDoors`), so the emptied inside does not show through them.

Each lit landmark has its light's `color` (fire and candles red-orange; the church a deep red, through its glass) and
`strength`. Tune with those, `LampPower`, `LampHalf`, `LampReach`, `LampFlicker` and `WindowLampStrength`; the
church's glass with the ramp in `GlassGlow`; the crosses' lamps with `CrossLampStrength`, `CrossLampHalf`,
`CrossLampReach` and `CrossLampBehind`.

**Town floor** (`ChooseFloorSources`, `FloorSourceMap`): the iso art paints into the floor what first person cannot
use: the feet of house walls past the edge of their models, of rocks, walls and barrels now drawn as meshes, and the
houses' shadows (with no sun to cast them). Those tiles (a building's art or model tiles, prop tiles, uncovered house
footprints, shadowed tiles next to buildings, and the boulders' feet painted on the solid tiles beside them: small
groups of solid floor-only tiles touching a boulder, where the river's are one long chain) show the floor of the
nearest tile of plain open ground instead. Then `CleanTownFloors` paints out, texel by texel, what is left on
walkable ground, the boulders' tiles and the plain ground itself (`CleanTownFloor`, `CleanGroundFloor`): clusters of
the boulders' deep blue (3 texels or more) and of the art's painted lamplight and window light (`IsPaintedLightColor`:
red down to a dark red, orange, yellow; 5 or more), each taking the plain ground's texel there. Smaller specks stay:
the ground's own leaves and stones. The floor textures' edges sample the iso diamond's outline, where every other
texel falls outside it; `ExtractFloor` gives those the texel inside, or they were dotted black lines along every
tile's edge. `dev.fp.floormap` shows the result from above.

**The town palette has no shade ramps.** Dungeon palettes run in ramps of 16 colours from light to dark, and the
game's light tables and the textures' upscaler (`ComputeMixColor`: the middle of two colours of one ramp is the
index between them) count on that. The town palette's blocks of 16 mix unrelated colours: the index between two
greys can be a blood red, and the game's light tables (never used darker than fully lit in town) turn greys into
reds, greens and blacks. So in town the upscaler mixes by the nearest colour of the whole palette (else the close-up
floor was sprinkled with red), and first person shades through `FpLightTable`: in town tables made from the palette
(each colour darkened to the nearest palette colour, by the old tables' average darkening), the game's elsewhere.

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

**A landmark with no model in the release** is built by the exporter itself: the mausoleum by the cows (tiles x
48-50, y 18-21), `mausoleum_triangles` in `export_landmarks.py`, measured on the town's art through the iso
projection (its platform the full 3 x 4 tiles, eaves at 2.42 and ridge at 3.22 tiles of art height): a platform
of two steps, stone walls with pilasters and a cornice, an iron grate over its front (the gable end towards +y)
with curls along its top and the dark inside behind, pediments framed and with rosettes, a slate roof with stone
knobs along its eaves and an iron ring over each end of the ridge. It is laid out in tiles of its platform and
art heights, so its `Landmarks` entry maps it straight onto them (`zScale` 1, not flipped; no fit needed). Its
triangles carry the class bit 0x40: their winding is reliable, so the game takes their normals from it
(`LandmarkMesh::wound`, `AddMeshTriangle`) instead of pointing them away from the building's middle, which would
turn the roof's undersides and the grate's reveals the wrong way. Its textures are the church's stone and
pavement and the kit's column stone and steel, tinted to the art's cool grey (`MAUSOLEUM_TINT`), and a slate made
by the script (`slate_texture`). The dead tree and the creeper the art draws against its long side are not
rebuilt (with `dev.fp.modeltextures(false)` the projected art shows them).

### 5.6 Dungeon structures
All in `fp_buildings.cpp`, built per level in `BuildStructures`:

| Structure | How it's made | Code |
|---|---|---|
| **Clean pillars, arches, stairwells** (cathedral, catacombs, crypt) | the quads of `BuildCleanGeometry` (fp_dungeon.cpp, 5.1), gathered per 4 x 4 tiles, textured with the set's surfaces by their texture coordinates | `BuildCleanStructures`, `MeshClean` shader |
| **Torch stands** (the cathedral's) and **free-standing columns** (caves) | shape read off the art row by row (the widest pixels give the size at each height; square or round) and turned into a lathe mesh with the art projected onto it, clamped at the ceiling. In the clean dungeon only the torch stands are built this way, and their base is a broad, flat square pyramid of the set's `ivory` instead of what the art reads as (the art's pale pyramid, read as round with the floor around it, came out a wide flared blob): `StandBaseHalf`, `StandBaseHeight`, its texture laid on from above, its sides turned from the art's light (from the left) a light level darker | `ColumnProfile`, `BuildColumn`, `AddCleanPyramid` |
| **Arcade pillars** (caves) | a shaft from the symmetric lower art; neighbouring pillars joined by **arch walls** wearing the filler wall pair, the painted arch stretched to spring from the capitals and span the gap (`FitArcade`), cut open and an underside added | `BuildArcadePillar`, `AddArcadeWall`, `MeshArcade` shader |
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
- The caves (and the crypt, the hive) **cycle colours 0–31** every frame: their lava and water (`IsCycledColor`).
  Tables and caches built from the palette check `StablePaletteVersion()`, which ignores those colours, not
  `PaletteVersion()`, which changes every frame there: keyed on it, the caves rebuilt all their structures each
  frame (about 400 ms a frame). `DrawFirstPersonView` notes palette changes once a frame (`NotePaletteChange`).

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
  - Hellfire's nest in the clean style (fp_dungeon.cpp is written to take more tilesets: a texture set in the export
    script, a `SetName`, a `PillarStyleFor`, and the tileset in `IsCleanDungeon`).
  - The caves: a cut corner's diagonal face and the axis faces beside it do not carry the rock's texture on from one to
    the other.
  - Hell (5.1.2): the meshes' cost close up (the software rasterizer's per-pixel cost); the spikes' rocky mounds as
    meshes (their art is on the floor); the stairs up's rib cage could show steps going up.
  - The clean dungeon's floors are one texture: the art's floor markings (blood, the pentagram of Lazarus' lair) are
    gone; wall decorations painted in the art too (banners and torches that are objects stay, as sprites).
  - The black capital top on plain fallback pillars (caves).
  - The mirrored "V" pattern very close to cave walls.
  - `dungeonkit.bin` still carries the kit pieces the clean dungeon no longer uses (column, torch stand, wall models,
    vault, surfaces): re-export it with only `stairs_01`, the sarcophagus and the door to shrink it.
- **General:**
  - Merge `fp-buildings` into `fp` (or make it the main branch of the fork) once you're happy with it.
