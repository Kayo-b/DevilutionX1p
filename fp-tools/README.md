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
| `~/projects/diablo-hellfire-fp/*.7z, *.rar` | fan-made 3D models (plaza, church, dungeon pieces, Adria's shack, hell stairs) from Nexus Mods 67266; **free, non-commercial use only**: see `assets/fp/CREDITS.md` |
| `~/projects/diablo-hellfire-fp/research/{tristram_plaza,church,dungeon_pieces}/` | the extracted models the export scripts read (override with `FP_PLAZA_DIR`, `FP_CHURCH_DIR`, `FP_DUNGEON_PIECES_DIR`) |
| `~/projects/diablo-hellfire-fp/` | the old working copy (`devilutionx/`, `devilutionx-buildings/`), kept as a backup; `PLAN.md` holds the original design decisions |

All first-person code lives in `Source/fp/` of each checkout:

| File | Responsibility |
|---|---|
| `fp_view.cpp` | the frame: floor/ceiling pass, wall raycaster (with recesses and doors), sprite pass, phase timers |
| `fp_raycast.cpp/.hpp` | camera projection, `CastRay`, what counts as a wall (`IsOpaque`), `WallHeight()`, `VerticalScale()` |
| `fp_textures.cpp/.hpp` | turns each tile piece's iso art into wall-face, floor and column textures; detects pillars, lava and recesses; supplies the filler wall pair |
| `fp_buildings.cpp/.hpp` | **all 3D meshes**: town houses (voxel/mesh), landmarks (smithy, tavern, church), dungeon structures (columns, stairs, arcades, sarcophagus boxes), the triangle rasterizer |
| `fp_world.cpp/.hpp` | collects the sprites to draw (monsters, NPCs, objects, items, missiles, corpses, props); sizes and grounding |
| `fp_controls.cpp` | movement, mouse/gamepad look, pitch limit, taking over click-to-walk |
| `fp_pick.cpp` | what the crosshair points at |
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
| `dev.fp.modkit(bool)` | (branch `fp-modkit`) cathedral in the 3D mod's stone and models, or its original art |
| `dev.fp.modeltextures(bool)` | town buildings with the 3D models' own textures (default) or their projected iso art |
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
- **Recesses:** blind arches and niches are found from the dark run in the art (`FindRecess`). They are drawn as a
  second plane behind the wall, with lit side reveals (in `DrawWalls`). Deep niches use `NicheDepth` and plain blind
  arches `BlindArchDepth`.
- **Doors:** in the mod-kit dungeon closed doors are the kit's door model (`BuildDoorModel`, drawn by `DrawDoorModels`; the wall pass leaves the panel open). Otherwise closed door tiles (`IsClosedDoorTile`) get a shallow recess and a brightened panel. Tune them with
  `DoorDepth` and `DoorBrightening` in `fp_view.cpp`; the colour boost (`* 1.7F`) is in `DoorColor`.
- **Mod-kit ceiling:** `BuildCeilingVaults` lays the kit's groin vault (the `vault` model, a 6x6 resample of `ceiling_big_master` made by `export_kit.py`) over each open tile at the wall tops, flat-shaded (`MeshSolid`), drawn only within `VaultDrawRange`. `FP_NO_VAULT=1` turns it off.
- **Heights:** `VerticalScale()` (1.25 in the dungeon) stretches walls, ceiling, structures and tile-art sprites. Art
  heights stay in art units (`SpritePixelsPerTileY` = 39.2 px per unit), and the stretch is applied only when drawing.
- Lava and water count as solid, but they are drawn as floor (`isFlatSolid`). The ceiling is the plain ground
  texture. Hell has no ceiling.

### 5.2 Sprites (monsters, NPCs, objects)
- Sizes are `MonsterScale` and `TownerScale` in `fp_world.cpp`.
- Objects whose sprite is mostly empty below (torch flames) keep the original alignment. Others are "grounded" and
  sink to the middle of their base (`BaseSinkPixels`).
- To make an object a 3D box instead of a billboard, add its type to `IsBoxObject` in `fp_buildings.cpp` (that is how
  sarcophagi work). The box is measured from the sprite.

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
| **Free-standing columns / torch stands** | shape read off the art row by row (the widest pixels give the size at each height; square or round) and turned into a lathe mesh with the art projected onto it, clamped at the ceiling | `ColumnProfile`, `BuildColumn` |
| **Arcade pillars** | a shaft from the symmetric lower art; neighbouring pillars joined by **arch walls** wearing the filler wall pair, with the painted arch cut open and an underside added | `BuildArcadePillar`, `AddArcadeWall`, `MeshArcade` shader |
| **Stairs down (cathedral)** | built by hand from measurements of the art; the floor is cut open over the pit | `BuildStairsDown`, `FloorHoles` |
| **Stairs up / main entrance** | the kit's `stairs_01` mesh fitted over the stair tiles | `BuildStairsUp` |
| **Sarcophagus** | a box measured from its sprite | `BuildObjectBox` |

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
