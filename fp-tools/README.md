# Diablo + Hellfire First-Person — Modding Guide

This is a first-person mod of Diablo/Hellfire built on DevilutionX. The guide covers how the project is laid out,
how to build and run it, and how each kind of change was made, so you can carry on without me. For the original
design decisions, see `PLAN.md`.

**3D assets:** the meshes come from a fan rebuild of Diablo 1's assets (Nexus Mods, mod 67266), which may only be
used in products given away free of charge. Source, terms and credits are in `assets/fp/CREDITS.md`; keep that file
up to date when you add models.

---

## 1. Layout

Repository: `/home/kxyx/projects/DevilutionX1p`, the fork `Kayo-b/DevilutionX1p` of diasurgical/devilutionx.

| Branch | What it is |
|---|---|
| **`fp-buildings`** | **the working branch**: first person plus everything 3D (houses, church, dungeon structures, arcades, doors...) |
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
| `~/projects/diablo-hellfire-fp/research/{tristram_plaza,church,dungeon_pieces}/` | the extracted models the export scripts read (override with `FP_PLAZA_DIR`, `FP_CHURCH_DIR`, `FP_DUNGEON_PIECES_DIR`) |
| `~/projects/diablo-hellfire-fp/` | the old working copy (`devilutionx/`, `devilutionx-buildings/`), kept as a backup; `PLAN.md` holds the original design decisions |

All first-person code lives in `Source/fp/` of each checkout:

| File | Responsibility |
|---|---|
| `fp_view.cpp` | the frame: floor/ceiling pass, wall raycaster (with recesses and doors), sprite pass, phase timers |
| `fp_raycast.cpp/.hpp` | camera projection, `CastRay`, what counts as a wall (`IsOpaque`), `WallHeight()`, `VerticalScale()` |
| `fp_textures.cpp/.hpp` | turns each tile piece's iso art into wall-face, floor and column textures; detects pillars, lava and recesses; supplies the filler wall pair |
| `fp_doors.cpp/.hpp` | doorways (the door tile stays a wall with an opening through it) and the door leaf's swing |
| `fp_buildings.cpp/.hpp` | **all 3D meshes**: town houses (voxel/mesh), landmarks (smithy, tavern, church), dungeon structures (columns, stairs, arcades, sarcophagus boxes), the triangle rasterizer |
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
| `dungeonkit.bin` | dungeon kit pieces (the entrance stairs) | `fp-tools/export/export_kit.py` |

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
| `dev.fp.buildings / housemodels / housemeshes / housetextures / recesses (bool)` | switch individual features off to compare |
| `dev.fp.walkto(x,y)`, `dev.fp.where()` | test click-walks; print the camera position |
| `dev.fp.operate(x, y)` | operate the object at a tile as a click would (open a sarcophagus, a door, a chest) |
| `dev.fp.weapon(file, record, frame)` | show one Daggerfall weapon frame (`WEAPON<file>.CIF`; record 0 rest, 1-6 strikes); no arguments to follow the player again |
| `dev.fp.modkit(bool)` | global kit walls, floors, ceilings, columns, and arcade pillars (default off); entrance stairs and sarcophagus remain on |
| `dev.fp.modeltextures(bool)` | town buildings with the 3D models' own textures (default) or their projected iso art |
| `dev.fp.sarcophagus(bool)` | use the 3D mod-kit sarcophagus model independently of `dev.fp.modkit` (default on) |
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

`GAME=` chooses the binary (default: the repo's `build/devilutionx`). Screenshots and the log
(`headless.log`) go to `fp-tools/selftest/out/`, and saves go to `out/save`, so your real saves are never touched.

Ready-made scripts:
```bash
T=fp-tools/selftest
TOGGLE=housemeshes $T/town_sweep.sh tag true "66,62 58,72"        # 4 views from each town spot
LEVEL=2 SEED=4242 $T/dsweep.sh tag "55,42 54,37"                  # 4 views from each dungeon spot
LEVEL=1 $T/dungeon_cmp.sh tag 777 "lua:dev.fp.teleport(48,32)"    # FP views + original iso view side by side
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
| Level 2, seed 4242 | colonnade: pillars x = 52/58, y = 38–44; view from (55,42) |
| Levels 5 / 9 / 13 | catacombs / caves (lava) / hell |

---

## 5. How things work, and how to change them

### 5.1 Walls, floor and ceiling (raycaster)
- Every solid tile is a cube. Each face uses its tile's own iso art, unsheared into a texture (`ExtractFace` in
  `fp_textures.cpp`).
- Faces the iso camera never drew use the **filler pair**: two neighbouring faces from the level's most common
  straight wall, alternated so their arches stay whole (`GetFallbackFace(parity)`). Hell has no plain wall, so it
  uses each tile's own sparse art with the gaps left open.
- **Wall foot and cap:** a Diablo wall stands on the middle of its tile, half a tile in front of the tile's edge: in iso its
  face reaches 16 rows below the edge line (`FaceBaseRows`), where the plinths and column bases are, and the top of its
  thickness shows as a lit band above it (the cap; `WallCapRows` measures it on the plain wall pair: 16 rows in the
  cathedral, 21 in the catacombs, none in caves and hell). Faces are read down to that foot and drawn without the cap,
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
  inside the wall they draw the jambs and soffit, faced with the plain wall's stone read across the wall's thickness and up the side or along the top). The cathedral paints a door across two tiles, the door tile and its
  partner at -x or -y: the doorway is drawn on the door tile from both halves of the *closed* art joined and centred on
  it, and the partner's face is plain wall. The opening is `FindArchOutline` on that art (`DefaultOpening` if none).
  The leaf (`BuildDoorLeaf`, drawn by `DrawDoorLeaves`) is a low-poly slab whose top follows the arch, hinged on a jamb
  in the middle of the wall, textured with the kit's door-wood texture (or the closed door's art without the kit), its brightness mapped by `DoorLeafColor` onto the palette's brown ramp using wood colours measured from Daggerfall's `TEXTURE.074` "Door Set" (measured once, nothing read from Daggerfall at run time). When the door's state
  changes it swings (`DoorSwing`: `OpenAngle`, `SwingMilliseconds`) away from the camera's side, and stays open.
- **Mod-kit walls (experiment, too heavy: off by default):** within `KitWallModelRange` the wall pass skips each face and `KitWallModel` registers the kit's `wall_concave` (or `wall_door` for faces with openings) in its place; Off by default: `FP_KITWALLS=1` turns it on. About +35 ms per frame in a room.
- **Mod-kit ceiling:** `BuildCeilingVaults` lays the kit's groin vault (the `vault` model, a 6x6 resample of `ceiling_big_master` made by `export_kit.py`) over each open tile at the wall tops, flat-shaded (`MeshSolid`), drawn only within `VaultDrawRange`. Off by default (flat ceiling): `FP_VAULT=1` turns it on.
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
  and the catacombs (`FineCanvas`): the caves and hell cycle their palettes, which rebuilds their structures every
  frame. Cost: about 3 ms a frame at 1280x720 in a close cathedral view.

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
the stones' blue there). A heap of boulders is its outline filled with round stones, biggest first (`PackStones`: each
where the outline is widest and as wide as it is there; the iso view has the same scale across and down, so a
circle of art is a stone of that size on the ray of its middle). Each is a lumpy low-poly stone (`AddStone`: a
squashed sphere pushed in and out by a few waves, 80 triangles near, 20 far), half behind the heap's foot and half up
on the stones in front where its circle is above the foot, never higher than the biggest. A gravestone is made as
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

### 5.6 Dungeon structures
All in `fp_buildings.cpp`, built per level in `BuildStructures`:

| Structure | How it's made | Code |
|---|---|---|
| **Free-standing columns / torch stands** | shape read off the art row by row (the widest pixels give the size at each height; square or round) and turned into a lathe mesh with the art projected onto it, clamped at the ceiling | `ColumnProfile`, `BuildColumn` |
| **Arcade pillars** | a shaft from the symmetric lower art; neighbouring pillars joined by **arch walls** wearing the filler wall pair, the painted arch stretched to spring from the capitals and span the gap (`FitArcade`), cut open and an underside added | `BuildArcadePillar`, `AddArcadeWall`, `MeshArcade` shader |
| **Arches over walkable tiles** (cathedral arcades) | Diablo draws them with special cels on the tiles between pillars (`dSpecial`): each cel holds the half arch rising from the pillar after the tile; the pillar before it draws the other half. Both are read on the arch's plane (`SampleArchPlane`, `GetSpecialArch`, `PieceArchFace`) into one face spanning pillar centre to pillar centre (moved, with the slab, to where the pillars' axes are in the art: the cathedral's arcade pillars stand 7.5 px right of their tiles' middles, as their 3D columns do), built as a solid slab as thick as the capitals are wide (`ArchSlab::half`, measured from the art), with its underside along the opening (textured with the ring's lowest rows across its thickness) and the capitals' undersides closed. Over the pillars only what is above the springing is drawn (the springing is measured on the cel, ignoring art in the plinth rows), and the pillars' meshes are clipped there (`ClipTriangleBelow`). Where the art is empty above the opening the slab wears the plain wall, so it is never hollow; where an arcade ends on a pillar the capital block is completed past the pillar's middle and closed with an end face; surfaces over a pillar's (solid, unlit) tile take the light of the walkable tiles around it. Levels with these arches skip the arch walls above | `BuildSpecialArches`, `MeshArch` shader |
| **Stairs down (between levels)** | built by hand from measurements of the art; the floor is cut open over the pit (the exported kit currently has no down-stair mesh) | `BuildStairsDown`, `FloorHoles` |
| **Stairs up / main entrance** | the kit's `stairs_01` mesh fitted over the stair tiles by default, independently of the global kit surfaces | `BuildStairsUp` |
| **Sarcophagus** | a box measured from its first (closed) frame; while it opens, the kit's body plus its lid slid off, turned and tipped over the edge, following the object's animation frame (`sarcophagus_body`, `sarcophagus_lid`, exported in the closed sarcophagus's frame); the 3D mod-kit model can be toggled independently with `dev.fp.sarcophagus(bool)`, even while `dev.fp.modkit(false)` is active | `BuildObjectBox` |

To add a new piece from `Dungeon Pieces-67266-.7z`:
1. Add its group name to `PIECES` in `fp-tools/export/export_kit.py` and run it; it writes `assets/fp/dungeonkit.bin`.
2. Load it with `KitMesh("name")`, write a `Build...` function modelled on `BuildStairsUp`, and call it from
   `BuildStructures`.

Mark the tiles a structure replaces in `StructureTiles`:
- **`1`:** drawn as a mesh; rays pass through, there is no billboard, and the floor is plain ground.
- **`2`:** as `1`, plus a floor hole.
- **`3`:** an open wall drawn from the tile's own face art.

Texturing modes for structure faces (`AddStructureFace(..., mapping)`):

| Mode | Texture |
|---|---|
| `MeshProjective` | the iso art, mirrored on the back side |
| `MeshProjectiveCut` | the same, but see-through where the art is empty |
| `MeshPit` | repeated wall art that darkens with depth |
| `MeshFloorArt` | the floor art under the point (the stairs' fan) |
| `MeshWallFace` | one wall-face texture |
| `MeshArcade` | the arch wall |

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
  - Stairs for catacombs, caves and hell (only cathedral stairs are modelled).
  - The black capital top on plain fallback pillars.
  - The mirrored "V" pattern very close to cave walls.
- **General:**
  - Merge `fp-buildings` into `fp` (or make it the main branch of the fork) once you're happy with it.
