Diablo First-Person, for Linux
==============================

A first-person view for Diablo and Hellfire, built on DevilutionX. You walk through Tristram and the dungeon
at eye level, aim with a crosshair and fight with your weapon in hand. Press F4 at any time to switch between
first person and the original view. The game itself is unchanged: same levels, monsters, items and saves.

Project page: https://github.com/Kayo-b/DevilutionX1p


What you need
-------------

- Diablo (and, if you want it, its expansion Hellfire). This mod contains none of the game's data: you need
  your own copy. GOG.com sells Diablo together with Hellfire:
      https://www.gog.com/en/game/diablo
  Install it (with Wine, Lutris or Heroic), or keep GOG's offline installer: install.sh unpacks the data from
  it if innoextract is installed.

- Optional, for the weapon in your hand: The Elder Scrolls II: Daggerfall, which is free on GOG:
      https://www.gog.com/en/game/the_elder_scrolls_chapter_ii_daggerfall
  Its first-person weapon art is copied from your own install; this mod includes none of it.

- SDL2 (install.sh offers to install it if it's missing) and a recent Linux: glibc 2.39 or later, as in
  Ubuntu 24.04, Debian 13, Fedora 40 or Arch. On an older one, build the game from source (below).


Installing
----------

In this folder, run:

    ./install.sh

It finds your Diablo data or asks for it, asks whether to install Diablo alone or Diablo + Hellfire, and
whether to use Daggerfall's weapons, then installs the game in ~/.local/share/devilutionx-fp with a
devilutionx-fp command and applications menu entries. ./install.sh --help lists the options for an unattended
install; ./install.sh --uninstall removes it (your heroes and settings are kept).

From source instead (any recent distribution; install.sh builds it):

    git clone https://github.com/Kayo-b/DevilutionX1p.git
    cd DevilutionX1p
    ./install.sh


Playing
-------

The game starts in first person (Settings > Gameplay > Start in First Person turns that off).

  F4                 switch between first person and the original view
  H                  third person: see your hero from behind (Settings > Graphics > Third Person)
  W A S D            walk
  Mouse              look around; the crosshair marks what you'll act on
  Arrow keys         turn and look up or down without the mouse
  Left click         attack, talk, open, pick up
  Right click        cast the selected spell at the crosshair
  T                  spell book (S, its usual key, now walks)
  + / -              zoom in or out: the field of view in first person, the camera's distance
                     in third (Settings > Graphics > Field of View, Camera Distance)

Your heroes and settings are kept in ~/.local/share/diasurgical/devilution (shared with DevilutionX).


Credits and licences
--------------------

DevilutionX by diasurgical and its contributors (LICENSE.md). The 3D town and dungeon pieces come from a fan
rebuild of Diablo 1's assets (Nexus Mods, mod 67266), which may only be used free of charge: this mod is free
and must stay so. Details in CREDITS.md. Diablo and Hellfire are (c) Blizzard Entertainment; The Elder Scrolls
II: Daggerfall is (c) Bethesda Softworks. This mod is not affiliated with or endorsed by either.
