# Diablo First-Person (DevilutionX mod), version 1

This fork of [DevilutionX](https://github.com/diasurgical/devilutionX) adds a **first-person view** to Diablo and
Hellfire. You walk through Tristram and the dungeon at eye level, aim with a crosshair, and fight with your weapon
in hand. Press **F4** to switch between first person and the original isometric view at any time. The game itself
is unchanged: same levels, monsters, items, saves and rules.

## What's in version 1

- **A software 3D view of the live level.** Walls, floors and ceilings are rebuilt from the game's own tile art.
  Light comes from the game's lighting (torches, your light radius, spells). Arches, recesses, doors, columns,
  arcades, stairs and sarcophagi are 3D. A sarcophagus's lid slides off in 3D when you open it.
- **Monsters, townsfolk, items, objects and missiles** are shown as sprites at their positions in the world.
  Up close they're smoothed instead of drawn as big square pixels. Monsters and townsfolk are drawn as if a little
  farther away, so an enemy right in front of you doesn't fill the screen.
- **A 3D Tristram.** The houses, Griswold's smithy, Ogden's tavern, the fountain and the church are 3D models.
  The town fades into fog from about 20 tiles away.
- **Free movement:** walk with WASD and look with the mouse. Clicks go to whatever the crosshair points at, so
  attacking, talking, opening doors and picking things up all work as in the original.
- **Weapons in hand (optional).** If you own The Elder Scrolls II: Daggerfall, its first-person weapon art is used
  for the weapon you have equipped (swords, axes, maces, flails, hammers, staves, bows, bare fists), and it swings
  with your attacks.
- Single player only.

## Requirements

- The data files of the original game: `DIABDAT.MPQ`, plus `hellfire.mpq` and the `hf*.mpq` files for Hellfire.
  You can get them from the [GOG.com release of Diablo](https://www.gog.com/game/diablo).
- To build: CMake 3.22+, Ninja, a C++23 compiler and SDL3 (or SDL2), as for upstream DevilutionX. See
  [Building from Source](#building-from-source) for the packages on your platform.
- Optional: a Daggerfall install, for the weapons (the free GOG/Bethesda release works). The mod reads its
  `ARENA2` folder at run time; **no Daggerfall files are included in this repository**.

## Build

```bash
git clone https://github.com/Kayo-b/DevilutionX1p.git
cd DevilutionX1p
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDEBUG=ON \
  -DBUILD_TESTING=OFF -DNONET=ON -DDEVILUTIONX_SYSTEM_LUA=OFF -DDEVILUTIONX_STATIC_LUA=ON \
  -DDEVILUTIONX_DISABLE_EXCEPTIONS=ON -DDEVILUTIONX_DISABLE_RTTI=ON
cmake --build build
```

- `DEVILUTIONX_SYSTEM_LUA=OFF` builds the bundled Lua. Newer system Lua versions don't work with it.
- `DEBUG=ON` turns on the developer console, which has the `dev.fp.*` commands. This is the configuration the mod
  is tested with.

## Run

```bash
./build/devilutionx --data-dir /path/to/folder/with/DIABDAT.MPQ
```

On Linux, `fp-tools/run.sh` rebuilds what changed and starts the game. It reads the game data from
`FP_DATA_DIR` (default `~/Downloads/devilutionX`) and Daggerfall's weapon art from `FP_DAGGERFALL_DIR` (default:
the GOG install under Wine, `~/.wine/drive_c/GOG Games/Daggerfall/arena2`).

To get the weapons when you start the game yourself, point `FP_DAGGERFALL_DIR` at Daggerfall's `ARENA2` folder:

```bash
FP_DAGGERFALL_DIR="/path/to/Daggerfall/ARENA2" ./build/devilutionx --data-dir /path/to/diablo-data
```

Without it, everything works except that no weapon is drawn.

## Controls (first person)

| Input | Does |
|---|---|
| **F4** | switch between first person and the isometric view (rebindable in the keymapper settings) |
| **W A S D** | walk forward, left, back and right |
| **Mouse** | look around; the crosshair marks what you'll act on |
| **Arrow keys** | turn and look up or down without the mouse |
| **Left click** | attack, talk, open, pick up: the usual action on whatever is under the crosshair |
| **Right click** | cast the selected spell at the crosshair |
| **T** | spell book (the Speedbook; its usual key `S` is now used for walking) |
| Inventory, character, quest and spell keys | as usual. While a panel is open, the mouse cursor comes back so you can use it |

## Recommended settings

The bottom control panel is a fixed 640 × 128 pixels, so its share of the screen depends on the game's render
resolution. A render height of **640** keeps the panel small and the 3D view sharp. In `diablo.ini`, under
`[Graphics]`:

```ini
Width=853
Height=640
Fit to Screen=1
Upscale=1
```

This takes about 17 ms per frame (about 60 fps) on the development machine. A height of 480 is faster (about
11 ms), but the panel then covers over a quarter of the screen (20% at 640).

## Known limitations

- Single player only. Multiplayer is turned off in this build (`NONET`).
- Everything is drawn in software on the CPU. Large render resolutions cost frame time.
- Some dungeon art is still flat: plain walls with painted arches have only a shallow recess.
- Daggerfall's weapon art was drawn for a 4:3 screen, so the widest sideways swings are cut off a quarter of the
  way across a widescreen view.

## For modders

[`fp-tools/README.md`](fp-tools/README.md) is the full guide. It covers how the code is laid out (`Source/fp/`),
the console commands, headless screenshot tests, how each 3D feature works and how to change it, and the
performance budget.

## Credits and licences

**DevilutionX.** The engine this mod is built on: [DevilutionX](https://github.com/diasurgical/devilutionX) by
diasurgical and its contributors, under its own licence (see [`LICENSE.md`](LICENSE.md)). The original README
follows below.

**3D models: "Rebuild of Diablo 1 assets in FBX and OBJ format".** The town (the Tristram plaza's houses,
Griswold's smithy, Ogden's tavern, the fountain, the church) and the dungeon pieces (entrance stairs, column,
sarcophagus and its lid, candle stand, door, ceiling vault) come from this fan-made rebuild of Diablo 1's assets on
Nexus Mods, [mod 67266](https://www.nexusmods.com/skyrim/mods/67266), by its author on Nexus Mods.<!-- TODO: add the author's Nexus Mods name -->
The models were converted (voxelized, re-meshed, rescaled and fitted to the game's tile layout) by the scripts in
`fp-tools/export/`. The author's terms:

> You may use the meshes any way you like as long as your final product is provided free of charge. You are not
> allowed to use the meshes in commercial products or to sell them.

So this mod is free and non-commercial, and must stay that way while it includes these models; anything built from
it is under the same restriction.

The rebuild's textures, which this mod uses on the town buildings and dungeon pieces, are credited by its author to
their creators, who made them available free of charge: Dmitriy Chugai ([texturelib.com](http://www.texturelib.com)),
[lughertexture.com](http://www.lughertexture.com), [seamlesstextures.deviantart.com](http://seamlesstextures.deviantart.com),
[10ravens.com](http://www.10ravens.com/textures/), [Spiral Graphics](http://www.spiralgraphics.biz),
[mb3d.co.uk](http://www.mb3d.co.uk/mb3d), [texturez.com](http://texturez.com/), and borysses and Obsidian Dawn on
DeviantArt. Details: [`assets/fp/CREDITS.md`](assets/fp/CREDITS.md).

**First-person weapons: The Elder Scrolls II: Daggerfall.** The weapon art (`WEAPON*.CIF`) and its palette
(`ART_PAL.COL`) are from *The Elder Scrolls II: Daggerfall* by Bethesda Softworks (© 1996 Bethesda Softworks LLC,
a ZeniMax Media company), which Bethesda has made available for free. None of it is included in this
repository: the game reads it at run time from your own Daggerfall install (`FP_DAGGERFALL_DIR`).

**Diablo and Hellfire** are © Blizzard Entertainment (Hellfire by Sierra / Synergistic Software). You need your own
copy of the game data.

This mod is not affiliated with or endorsed by Blizzard Entertainment, Bethesda Softworks or ZeniMax Media.

---

*Original DevilutionX README:*

<p align="center">
  <img width="560" height="144" alt="image" src="https://github.com/user-attachments/assets/7ac73801-ef7b-4cc1-8442-a191a2a0a1ce" />
</p>

---

[![Discord Channel](https://img.shields.io/discord/518540764754608128?color=%237289DA&logo=discord&logoColor=%23FFFFFF)](https://discord.gg/devilutionx-518540764754608128)
[![Downloads](https://img.shields.io/github/downloads/diasurgical/devilutionX/total.svg)](https://github.com/diasurgical/devilutionX/releases/latest)
[![Codecov](https://codecov.io/gh/diasurgical/devilutionX/branch/master/graph/badge.svg)](https://codecov.io/gh/diasurgical/devilutionX)

<p align="center">
<img width="853" height="480" alt="image" src="https://github.com/user-attachments/assets/ee902926-6382-4ee5-b1c2-7947e8b434e9" />
</p>

<sub>*(The health-bar and XP-bar are off by default but can be enabled in the [game settings](https://github.com/diasurgical/DevilutionX/wiki/Config-File). Widescreen can also be disabled if preferred.)*</sub>

# What is DevilutionX

DevilutionX is a port of Diablo and Hellfire that strives to make it simple to run the game while providing engine improvements, bug fixes, and some optional quality of life features.

Check out the [manual](https://github.com/diasurgical/devilutionX/wiki) for available features and how to take advantage of them.

For a full list of changes, see our [changelog](docs/CHANGELOG.md).

# How to Install

Note: You'll need access to the data from the original game. If you don't have an original CD, you can [buy Diablo from GoG.com](https://www.gog.com/game/diablo) or Battle.net. Alternatively, you can use `spawn.mpq` from the [shareware](https://github.com/diasurgical/devilutionx-assets/releases/latest/download/spawn.mpq) [[2]](http://ftp.blizzard.com/pub/demos/diablosw.exe) version, in place of `DIABDAT.MPQ`, to play the shareware portion of the game.

Download the latest [DevilutionX release](https://github.com/diasurgical/devilutionX/releases/latest) and extract the contents to a location of your choosing or [build from source](#building-from-source).

- Copy `DIABDAT.MPQ` from the CD or Diablo installation (or [extract it from the GoG installer](https://github.com/diasurgical/devilutionX/wiki/Extracting-MPQs-from-the-GoG-installer)) to the DevilutionX folder.
- To run the Diablo: Hellfire expansion, you will also need to copy `hellfire.mpq`, `hfmonk.mpq`, `hfmusic.mpq`, and `hfvoice.mpq`.

For more detailed instructions: [Installation Instructions](./docs/installing.md).

# Contributing

We are always looking for more people to help with [coding](docs/CONTRIBUTING.md), [documentation](https://github.com/diasurgical/devilutionX/wiki), [testing the latest builds](#test-builds), spreading the word, or simply just hanging out on our [Discord server](https://discord.gg/devilutionx-518540764754608128).

# Mods

We hope to provide a good starting point for mods. In addition to the full Devilution source code, we also provide modding tools. Check out the list of known [mods based on DevilutionX](https://github.com/diasurgical/DevilutionX/wiki/Community-Mods).

# Test Builds

If you want to help test the latest development version (make sure to back up your files, as these may contain bugs), you can fetch the test build artifact from one of the build servers:

*Note: You must be logged into GitHub to download the attachments!*

[![Linux x86_64](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_x86_64.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_x86_64.yml?query=branch%3Amaster)
[![Linux AArch64](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_aarch64.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_aarch64.yml?query=branch%3Amaster)
[![Linux x86](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_x86.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_x86.yml?query=branch%3Amaster)
[![Linux x86_64 SDL1](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_x86_64_SDL1.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Linux_x86_64_SDL1.yml?query=branch%3Amaster)
[![macOS x86_64](https://github.com/diasurgical/devilutionX/actions/workflows/macOS_x86_64.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/macOS_x86_64.yml?query=branch%3Amaster)
[![Windows MinGW x64](https://github.com/diasurgical/devilutionX/actions/workflows/Windows_MinGW_x64.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Windows_MinGW_x64.yml?query=branch%3Amaster)
[![Windows MinGW x86](https://github.com/diasurgical/devilutionX/actions/workflows/Windows_MinGW_x86.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Windows_MinGW_x86.yml?query=branch%3Amaster)
[![Windows MSVC x64](https://github.com/diasurgical/devilutionX/actions/workflows/Windows_MSVC_x64.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Windows_MSVC_x64.yml?query=branch%3Amaster)
[![Android](https://github.com/diasurgical/devilutionX/actions/workflows/Android.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/Android.yml?query=branch%3Amaster)
[![iOS](https://github.com/diasurgical/devilutionX/actions/workflows/iOS.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/iOS.yml?query=branch%3Amaster)
[![PS4](https://github.com/diasurgical/devilutionX/actions/workflows/PS4.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/PS4.yml?query=branch%3Amaster)
[![Original Xbox](https://github.com/diasurgical/devilutionX/actions/workflows/xbox_nxdk.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/xbox_nxdk.yml?query=branch%3Amaster)
[![Xbox One/Series](https://github.com/diasurgical/devilutionX/actions/workflows/xbox_one.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/xbox_one.yml?query=branch%3Amaster)
[![Nintendo Switch](https://github.com/diasurgical/devilutionX/actions/workflows/switch.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/switch.yml)
[![Sony PlayStation Vita](https://github.com/diasurgical/devilutionX/actions/workflows/vita.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/vita.yml)
[![Nintendo 3DS](https://github.com/diasurgical/devilutionX/actions/workflows/3ds.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/3ds.yml)
[![Amiga M68K](https://github.com/diasurgical/devilutionX/actions/workflows/amiga-m68k.yml/badge.svg)](https://github.com/diasurgical/devilutionX/actions/workflows/amiga-m68k.yml)

# Building from Source

Want to compile the program by yourself? Great! Simply follow the [build instructions](./docs/building.md).

# Credits

- The original Devilution project: [Devilution](https://github.com/diasurgical/devilution#credits)
- [Everyone](https://github.com/diasurgical/devilutionX/graphs/contributors) who worked on Devilution/DevilutionX
- [Nikolay Popov](https://www.instagram.com/nikolaypopovz/) for UI and graphics
- [WiAParker](https://wiaparker.pl/projekty/diablo-hellfire/) for the Polish voice pack
- And thanks to all who support the project, report bugs, and help spread the word ❤️

# Legal

DevilutionX is made publicly available and released under the Sustainable Use License (see [LICENSE](LICENSE.md)).

The source code in this repository is for non-commercial use only. If you use the source code, you may not charge others for access to it or any derivative work thereof.

Diablo® - Copyright © 1996 Blizzard Entertainment, Inc. All rights reserved. Diablo and Blizzard Entertainment are trademarks or registered trademarks of Blizzard Entertainment, Inc. in the U.S. and/or other countries.

DevilutionX and any of its maintainers are in no way associated with or endorsed by Blizzard Entertainment®.
