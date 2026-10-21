Diablo First-Person
===================

A first-person view for Diablo and Hellfire, built on DevilutionX. You walk through Tristram and the dungeon
at eye level, aim with a crosshair and fight with your weapon in hand. Press F4 at any time to switch between
first person and the original view. The game itself is unchanged: same levels, monsters, items and saves.

Project page: https://github.com/Kayo-b/DevilutionX1p


What you need
-------------

- Diablo (and, if you want it, its expansion Hellfire). This mod contains none of the game's data: you need
  your own copy. GOG.com sells Diablo together with Hellfire:
      https://www.gog.com/en/game/diablo
  Install it with GOG Galaxy or GOG's offline installer first.

- Optional, for the weapon in your hand: The Elder Scrolls II: Daggerfall, which is free on GOG:
      https://www.gog.com/en/game/the_elder_scrolls_chapter_ii_daggerfall
  Its first-person weapon art is copied from your own install; this mod includes none of it.


Setting it up
-------------

1. Unpack this zip into a folder of your choice (for example C:\Games\Diablo First-Person).
2. Double-click Install.bat. It:
   - finds your Diablo data (DIABDAT.MPQ) or asks for its folder,
   - asks whether to set up Diablo alone or Diablo + Hellfire (when Hellfire's data is there),
   - asks whether to use Daggerfall's weapons, and finds Daggerfall or asks for its folder,
   - copies those files into this folder, and makes desktop and Start menu shortcuts.
3. Start the game from the shortcuts, or with devilutionx.exe.

You can run Install.bat again at any time to change your answers (for example to add Hellfire or the weapons).

Without Install.bat: copy DIABDAT.MPQ (and for Hellfire: hellfire.mpq, hfmonk.mpq, hfmusic.mpq, hfvoice.mpq)
next to devilutionx.exe, and Daggerfall's WEAPON00.CIF to WEAPON11.CIF and ART_PAL.COL (from its ARENA2 folder)
into a folder named daggerfall next to devilutionx.exe.

If Windows SmartScreen warns about an unrecognised app, choose "More info" and "Run anyway": the game isn't signed.


Controls (first person)
-----------------------

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
  Other keys         as usual; while a panel is open, the mouse cursor comes back

The game starts in first person. To start in the original view instead:
Settings > Gameplay > Start in First Person.

Your heroes and settings are kept in %APPDATA%\diasurgical\devilution (shared with DevilutionX).


Credits and licences
--------------------

- DevilutionX by diasurgical and its contributors: https://github.com/diasurgical/devilutionX
  (LICENSE.txt).
- The 3D town and dungeon pieces come from a fan rebuild of Diablo 1's assets (Nexus Mods, mod 67266), which
  may only be used free of charge: this mod is free and must stay so. Details in CREDITS.txt.
- SDL2: LICENSE-SDL2.txt.
- Diablo and Hellfire are (c) Blizzard Entertainment. The Elder Scrolls II: Daggerfall is (c) Bethesda
  Softworks, a ZeniMax Media company. This mod is not affiliated with or endorsed by either.
