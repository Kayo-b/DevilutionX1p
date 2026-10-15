A first-person view for Diablo and Hellfire, built on DevilutionX. The game starts in first person; **F4** switches
to the original view at any time.

### What you need

- **Diablo** (and, if you want it, its expansion **Hellfire**). None of the game's data is included: GOG sells
  Diablo together with Hellfire, [Diablo + Hellfire on GOG](https://www.gog.com/en/game/diablo).
- Optional, for the weapon in your hand: **The Elder Scrolls II: Daggerfall**, free on GOG:
  [Daggerfall on GOG](https://www.gog.com/en/game/the_elder_scrolls_chapter_ii_daggerfall). Its weapon art is copied
  from your own install; none of it is included.

### Windows

1. Download `DevilutionX-FP-…-windows-x64.zip` below and unpack it into a folder of your choice.
2. Double-click **Install.bat**. It finds your Diablo data, asks whether to set up Diablo alone or Diablo + Hellfire,
   whether to use Daggerfall's weapons, then makes desktop and Start menu shortcuts.
3. Start **Diablo First-Person** (or **Hellfire First-Person**).

### Android (handhelds such as the AYN Thor)

1. Download `DevilutionX-FP-…-android.apk` below, open it on the device and allow installing it. It installs as an
   app of its own, beside DevilutionX if you have that.
2. Start it once, then put `DIABDAT.MPQ` (for Hellfire also `hellfire.mpq`, `hfmonk.mpq`, `hfmusic.mpq`,
   `hfvoice.mpq`) in `Android/data/org.diasurgical.devilutionx.fp/files/`: from a PC over USB, or with the import
   screen the app shows when it can't find the data.
3. For the weapons, copy Daggerfall's `WEAPON00.CIF` to `WEAPON11.CIF` and `ART_PAL.COL` (from its `ARENA2` folder) into
   a `daggerfall` folder there **with adb from a PC**: a file manager on the device leaves a folder the game can't read.
   The README's [Android section](https://github.com/Kayo-b/DevilutionX1p#android-ayn-thor-and-other-handhelds) has the
   commands.

With a gamepad, the left stick walks and the right stick looks. Each release's APK is signed with a new key, so a newer
one won't install over an older one: copy your saves (`*.sv`) out of that folder, uninstall (which deletes the folder),
then install the new one and put the files back.

### Linux

1. Download `DevilutionX-FP-…-linux-x86_64.tar.gz` below and unpack it (`tar -xzf DevilutionX-FP-*-linux-x86_64.tar.gz`).
2. In its folder, run `./install.sh`. It asks the same questions and adds a `devilutionx-fp` command and
   applications menu entries (one for Diablo, one for Hellfire).

It needs SDL2 (`install.sh` offers to install it) and a recent Linux (glibc 2.39 or later: Ubuntu 24.04, Debian 13,
Fedora 40, Arch). On an older system, or another processor, build it from source; `install.sh` does that too:

```bash
git clone https://github.com/Kayo-b/DevilutionX1p.git
cd DevilutionX1p
./install.sh
```

The full guide, controls and credits are in the [README](https://github.com/Kayo-b/DevilutionX1p#readme).
