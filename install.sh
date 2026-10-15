#!/usr/bin/env bash
# Diablo First-Person (a DevilutionX mod): the Linux installer.
#
# Builds the game from this checkout and installs it with your Diablo data and, if you want them, the first-person
# weapons drawn from your own copy of The Elder Scrolls II: Daggerfall. The game it installs starts in first person
# (F4 switches to the original view).
#
#   ./install.sh                 asks each question, with the answer it found as the default
#   ./install.sh --yes           takes every default: the data it finds, the weapons if Daggerfall is found
#   ./install.sh --uninstall     removes the game, its launcher and its menu entries (your saves are kept)
#
# Options (each answers its question):
#   --dir DIR             where to install (default: ~/.local/share/devilutionx-fp)
#   --diablo PATH         the folder with DIABDAT.MPQ (Hellfire: hellfire.mpq and hf*.mpq beside it), or GOG's
#                         offline installer (setup_diablo_*.exe; needs innoextract)
#   --game diablo|hellfire   Diablo alone, or Diablo + Hellfire (when Hellfire's data is there, the default)
#   --daggerfall DIR      Daggerfall's ARENA2 folder, or the folder Daggerfall is installed in
#   --no-daggerfall       no weapons in hand
#   --no-shortcuts        no devilutionx-fp command and no applications menu entries
#   --jobs N              build jobs (default: every core)
#
# The game data: Diablo + Hellfire at https://www.gog.com/en/game/diablo; Daggerfall, free, at
# https://www.gog.com/en/game/the_elder_scrolls_chapter_ii_daggerfall
#
# Your heroes and settings stay where DevilutionX keeps them (~/.local/share/diasurgical/devilution).
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD="$REPO/build-release"
DEFAULT_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/devilutionx-fp"
BIN_LINK="$HOME/.local/bin/devilutionx-fp"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
TOOLS="${XDG_CACHE_HOME:-$HOME/.cache}/devilutionx-fp/tools"
CMAKE_VERSION=4.4.3
NINJA_VERSION=1.13.2
GOG_DIABLO=https://www.gog.com/en/game/diablo
GOG_DAGGERFALL=https://www.gog.com/en/game/the_elder_scrolls_chapter_ii_daggerfall

YES=0 UNINSTALL=0 SHORTCUTS=1 JOBS=""
DIR="" DIABLO="" GAME="" DAGGERFALL="" NO_DAGGERFALL=0

if [ -t 1 ]; then
	B=$'\e[1m' DIM=$'\e[2m' RED=$'\e[31m' GRN=$'\e[32m' YEL=$'\e[33m' RST=$'\e[0m'
else
	B="" DIM="" RED="" GRN="" YEL="" RST=""
fi
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$RST"; }
info() { printf '    %s\n' "$*"; }
ok() { printf '    %s%s%s\n' "$GRN" "$*" "$RST"; }
warn() { printf '    %s%s%s\n' "$YEL" "$*" "$RST" >&2; }
die() {
	printf '\n%serror:%s %s\n' "$RED" "$RST" "$*" >&2
	exit 1
}

# ask "question" default -> REPLY (the default on Enter, with --yes, or once input runs out)
ask() {
	REPLY=""
	if [ "$YES" = 1 ]; then
		REPLY=$2
		return
	fi
	printf '    %s %s[%s]%s ' "$1" "$DIM" "$2" "$RST"
	read -r REPLY || true
	REPLY=${REPLY:-$2}
}
# yesno "question" Y|N -> status 0 for yes
yesno() {
	local hint=y/N
	[ "$2" = Y ] && hint=Y/n
	if [ "$YES" = 1 ]; then
		[ "$2" = Y ]
		return
	fi
	printf '    %s %s[%s]%s ' "$1" "$DIM" "$hint" "$RST"
	local answer=""
	read -r answer || true
	answer=${answer:-$2}
	case "$answer" in [yY]*) return 0 ;; *) return 1 ;; esac
}
# A path as typed: ~ expanded, quotes from a drag-and-drop taken off.
expand_path() {
	local p=$1
	p=${p#\'} p=${p%\'} p=${p#\"} p=${p%\"}
	case "$p" in "~") p=$HOME ;; "~/"*) p="$HOME/${p#\~/}" ;; esac
	printf '%s' "$p"
}

usage() {
	sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
	case "$1" in
	-y | --yes) YES=1 ;;
	--uninstall) UNINSTALL=1 ;;
	--dir) DIR=$(expand_path "${2:?--dir needs a folder}") && shift ;;
	--diablo) DIABLO=$(expand_path "${2:?--diablo needs a folder}") && shift ;;
	--game)
		GAME=${2:?--game needs diablo or hellfire} && shift
		[[ "$GAME" =~ ^(diablo|hellfire)$ ]] || die "--game takes diablo or hellfire"
		;;
	--daggerfall) DAGGERFALL=$(expand_path "${2:?--daggerfall needs a folder}") && shift ;;
	--no-daggerfall) NO_DAGGERFALL=1 ;;
	--no-shortcuts) SHORTCUTS=0 ;;
	--jobs) JOBS=${2:?--jobs needs a number} && shift ;;
	-h | --help)
		usage
		exit 0
		;;
	*) die "unknown option: $1 (see ./install.sh --help)" ;;
	esac
	shift
done

[ "$(uname -s)" = Linux ] || die "this installer is for Linux; on Windows, use the release zip's Install.bat"

# ---------------------------------------------------------------------------------------------------------------------
# Uninstall

if [ "$UNINSTALL" = 1 ]; then
	DIR=${DIR:-$DEFAULT_DIR}
	step "Removing Diablo First-Person"
	[ -x "$DIR/devilutionx" ] || [ -d "$DIR/assets/fp" ] || die "no install in $DIR (pass --dir with the folder you installed to)"
	info "$DIR"
	[ "$YES" = 1 ] || yesno "Remove it, its launcher and its menu entries? Your heroes and settings are kept." N || exit 0
	rm -rf "$DIR"
	if [ -L "$BIN_LINK" ] && [ "$(readlink "$BIN_LINK")" = "$DIR/devilutionx" ]; then rm -f "$BIN_LINK"; fi
	for f in "$APPS/devilutionx-fp.desktop" "$APPS/devilutionx-fp-hellfire.desktop"; do
		if [ -f "$f" ] && grep -qF "$DIR/" "$f"; then rm -f "$f"; fi
	done
	ok "Removed."
	exit 0
fi

printf '%sDiablo First-Person%s: installer\n' "$B" "$RST"
info "Builds the game from this folder and installs it with your Diablo data."

# ---------------------------------------------------------------------------------------------------------------------
# Questions first, so the long build can then run without anyone watching.

step "Where to install"
if [ -z "$DIR" ]; then
	ask "Install folder:" "$DEFAULT_DIR"
	DIR=$(expand_path "$REPLY")
fi
case "$DIR" in /*) ;; *) DIR="$PWD/$DIR" ;; esac
[ "$DIR" != "$REPO" ] || die "pick a folder other than the source folder"
info "$DIR"

# Finds a file by name in any case: find_ci DIR name -> its path
find_ci() {
	find "$1" -maxdepth 1 -type f -iname "$2" -print -quit 2>/dev/null
}

# Folders that hold DIABDAT.MPQ or spawn.mpq, from the places Diablo is usually installed or unpacked, then GOG's
# offline installers lying about (unpacked with innoextract, if it's there).
detect_diablo() {
	local roots=("$DIR" "${XDG_DATA_HOME:-$HOME/.local/share}/diasurgical/devilution" "$HOME/.wine/drive_c"
		"$HOME/Games" "$HOME/Downloads" "$HOME/.local/share/Steam/steamapps/compatdata" "$HOME/GOG Games" "$HOME")
	local root
	for root in "${roots[@]}"; do
		[ -d "$root" ] || continue
		local depth=6
		[ "$root" = "$HOME" ] && depth=2
		[[ "$root" = */compatdata ]] && depth=8
		find "$root" -maxdepth "$depth" -type f \( -iname diabdat.mpq -o -iname spawn.mpq \) -printf '%h\n' 2>/dev/null
	done | awk '!seen[$0]++'
	if command -v innoextract >/dev/null; then
		find "$HOME/Downloads" "$HOME" -maxdepth 2 -type f -iname 'setup_diablo_1.09*.exe' 2>/dev/null | awk '!seen[$0]++'
	fi
}

# The MPQs of a Diablo folder worth copying: the game's, Hellfire's (also in a hellfire/ folder, as GOG installs them)
# and the extra fonts. Never devilutionx.mpq: another DevilutionX's own files would hide this build's.
diablo_files() {
	local dir=$1 sub
	for sub in "$dir" "$dir/hellfire" "$dir/Hellfire"; do
		[ -d "$sub" ] || continue
		find "$sub" -maxdepth 1 -type f \( -iname diabdat.mpq -o -iname spawn.mpq -o -iname hellfire.mpq \
			-o -iname 'hf*.mpq' -o -iname fonts.mpq \) 2>/dev/null
	done
}
is_hellfire_file() {
	case "$(basename "${1,,}")" in hellfire.mpq | hf*.mpq) return 0 ;; *) return 1 ;; esac
}

# What a source holds: "diablo hellfire", "diablo" or "spawn" (the shareware).
contents_of() {
	local files
	if [ -f "$1" ]; then
		files=$(innoextract -l "$1" 2>/dev/null | grep -io '[a-z0-9_/]*\.mpq' || true)
		files=$(sed 's|^|/|' <<<"$files")
	else
		files=$(diablo_files "$1")
	fi
	if grep -qi '/diabdat\.mpq$' <<<"$files"; then
		if grep -qi '/hellfire\.mpq$' <<<"$files"; then echo "diablo hellfire"; else echo diablo; fi
	elif grep -qi '/spawn\.mpq$' <<<"$files"; then
		echo spawn
	fi
}
describe_contents() {
	case "$1" in
	"diablo hellfire") echo "Diablo + Hellfire" ;;
	diablo) echo "Diablo" ;;
	spawn) echo "the shareware Diablo (spawn.mpq)" ;;
	*) echo "no game data" ;;
	esac
}
describe_source() {
	local what
	what=$(describe_contents "$(contents_of "$1")")
	if [ -f "$1" ]; then echo "GOG installer: $what"; else echo "$what"; fi
}

GOG_INSTALLER=""
pick_diablo() {
	local found=()
	mapfile -t found < <(detect_diablo)
	if [ ${#found[@]} -gt 0 ]; then
		info "Found the game's data (DIABDAT.MPQ) in:"
		local i
		for i in "${!found[@]}"; do
			printf '      %s%d)%s %s %s(%s)%s\n' "$B" $((i + 1)) "$RST" "${found[$i]}" "$DIM" "$(describe_source "${found[$i]}")" "$RST"
		done
		ask "Pick a number, or type the folder that holds DIABDAT.MPQ:" 1
	else
		info "DIABDAT.MPQ wasn't found. It's the data of the original game, which you need to own:"
		info "GOG.com sells Diablo with its expansion Hellfire: ${B}$GOG_DIABLO${RST}"
		info "Type the folder that holds DIABDAT.MPQ (Hellfire's hellfire.mpq and hf*.mpq too, if you have them),"
		info "or the path of GOG's offline installer (setup_diablo_1.09_*.exe; unpacking it needs innoextract)."
		[ "$YES" = 1 ] && die "no Diablo data found; pass --diablo with its folder (get the game at $GOG_DIABLO)"
		ask "Folder:" ""
	fi
	if [[ "$REPLY" =~ ^[0-9]+$ ]] && [ "$REPLY" -ge 1 ] && [ "$REPLY" -le ${#found[@]} ]; then
		DIABLO=${found[$((REPLY - 1))]}
	else
		DIABLO=$(expand_path "$REPLY")
	fi
}

check_diablo() {
	[ -n "$DIABLO" ] || die "no Diablo data given (get the game at $GOG_DIABLO)"
	if [ -f "$DIABLO" ]; then
		case "${DIABLO,,}" in
		*.exe)
			GOG_INSTALLER=$DIABLO
			command -v innoextract >/dev/null ||
				die "unpacking GOG's installer needs innoextract: install it (every distribution has it) and run this again"
			[ -n "$(contents_of "$GOG_INSTALLER")" ] ||
				die "innoextract found no DIABDAT.MPQ in $GOG_INSTALLER (a newer innoextract may be needed)"
			return
			;;
		*) DIABLO=$(dirname "$DIABLO") ;;
		esac
	fi
	[ -d "$DIABLO" ] || die "no such folder: $DIABLO"
	[ -n "$(contents_of "$DIABLO")" ] || die "no DIABDAT.MPQ (or the shareware spawn.mpq) in $DIABLO"
}

step "Diablo's data"
[ -n "$DIABLO" ] || pick_diablo
check_diablo
CONTENTS=$(contents_of "${GOG_INSTALLER:-$DIABLO}")
info "${GOG_INSTALLER:-$DIABLO} ($(describe_source "${GOG_INSTALLER:-$DIABLO}"))"

step "Which game"
if [ "$CONTENTS" = "diablo hellfire" ]; then
	if [ -z "$GAME" ]; then
		info "Hellfire's data is there too. Install:"
		printf '      %s1)%s Diablo\n' "$B" "$RST"
		printf '      %s2)%s Diablo + Hellfire %s(the menu gets an entry for each; started on its own, the game asks which)%s\n' "$B" "$RST" "$DIM" "$RST"
		ask "Pick:" 2
		case "$REPLY" in 1 | [dD]*) GAME=diablo ;; 2 | [hH]*) GAME=hellfire ;; *) die "pick 1 or 2" ;; esac
	fi
else
	[ "$GAME" != hellfire ] || die "no Hellfire data (hellfire.mpq, hfmonk.mpq, hfmusic.mpq, hfvoice.mpq) in ${GOG_INSTALLER:-$DIABLO}"
	GAME=diablo
	[ "$CONTENTS" = diablo ] && info "Hellfire's data (hellfire.mpq and hf*.mpq) isn't there; GOG's Diablo comes with it: $GOG_DIABLO"
fi
if [ "$GAME" = hellfire ]; then info "Diablo + Hellfire"; else info "$(describe_contents "${CONTENTS%% *}")"; fi

# Daggerfall: the folder (ARENA2) with WEAPON00.CIF in it, from the folder given or the places it's usually installed.
arena2_in() {
	find "$1" -maxdepth 4 -type f -iname weapon00.cif -printf '%h\n' 2>/dev/null | head -n 1
}
detect_daggerfall() {
	local roots=("$DIR/daggerfall" "$HOME/.wine/drive_c" "$HOME/.local/share/Steam/steamapps/common"
		"$HOME/.steam/steam/steamapps/common" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common"
		"$HOME/Games" "$HOME/GOG Games" "$HOME/Downloads" "$HOME/dosgames" "$HOME")
	local vdf
	for vdf in "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" "$HOME/.steam/steam/steamapps/libraryfolders.vdf"; do
		[ -f "$vdf" ] || continue
		while read -r lib; do roots+=("$lib/steamapps/common"); done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' "$vdf")
	done
	local root
	for root in "${roots[@]}"; do
		[ -d "$root" ] || continue
		local depth=7
		[ "$root" = "$HOME" ] && depth=3
		find "$root" -maxdepth "$depth" -type f -iname weapon00.cif -printf '%h\n' 2>/dev/null
	done | awk '!seen[$0]++' | head -n 5
}

step "Weapons in hand (optional)"
if [ "$NO_DAGGERFALL" = 1 ]; then
	info "None."
elif [ -n "$DAGGERFALL" ]; then
	found=$(arena2_in "$DAGGERFALL")
	[ -n "$found" ] || die "no Daggerfall weapon art (WEAPON00.CIF) in $DAGGERFALL"
	DAGGERFALL=$found
	info "$DAGGERFALL"
else
	info "First person can show the weapon you hold, using the first-person weapon art of"
	info "The Elder Scrolls II: Daggerfall, which is free: ${B}$GOG_DAGGERFALL${RST}"
	info "It's read from your own copy of Daggerfall: this mod includes none of it."
	mapfile -t found < <(detect_daggerfall)
	if [ ${#found[@]} -gt 0 ]; then
		info "Found Daggerfall's art in:"
		for i in "${!found[@]}"; do printf '      %s%d)%s %s\n' "$B" $((i + 1)) "$RST" "${found[$i]}"; done
		if yesno "Use Daggerfall's weapons?" Y; then
			ask "Pick a number, or type Daggerfall's folder:" 1
			if [[ "$REPLY" =~ ^[0-9]+$ ]] && [ "$REPLY" -ge 1 ] && [ "$REPLY" -le ${#found[@]} ]; then
				DAGGERFALL=${found[$((REPLY - 1))]}
			else
				DAGGERFALL=$(arena2_in "$(expand_path "$REPLY")")
				[ -n "$DAGGERFALL" ] || die "no Daggerfall weapon art (WEAPON00.CIF) in $(expand_path "$REPLY")"
			fi
		fi
	elif [ "$YES" != 1 ]; then
		info "Daggerfall wasn't found. Install it (GOG's installer runs under Wine) and run this again,"
		if yesno "or type the folder it's in now?" N; then
			ask "Daggerfall's folder (the one with ARENA2 in it, or ARENA2 itself):" ""
			DAGGERFALL=$(arena2_in "$(expand_path "$REPLY")")
			[ -n "$DAGGERFALL" ] || die "no Daggerfall weapon art (WEAPON00.CIF) in $(expand_path "$REPLY")"
		fi
	fi
	if [ -n "$DAGGERFALL" ]; then info "Using $DAGGERFALL"; else info "No weapons in hand (run this again to add them)."; fi
fi

# ---------------------------------------------------------------------------------------------------------------------
# Build tools

step "Checking the build tools"

# A tool that runs: a version manager's shim with no version set is on the PATH but doesn't.
works() { "$@" --version >/dev/null 2>&1; }

CMAKE="" NINJA="" MISSING=()
version_at_least() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]; }
find_cmake() {
	local c
	for c in cmake "$TOOLS/cmake/bin/cmake"; do
		if works "$c" && version_at_least "$("$c" --version | head -n 1 | awk '{print $3}')" 3.22; then
			CMAKE=$(command -v "$c")
			return 0
		fi
	done
	return 1
}
find_ninja() {
	local n
	for n in ninja ninja-build "$TOOLS/ninja"; do
		if works "$n"; then
			NINJA=$(command -v "$n")
			return 0
		fi
	done
	return 1
}
CXX_FOUND=""
find_cxx() {
	local c
	for c in ${CXX:-} g++ clang++ c++; do
		if works "$c"; then
			CXX_FOUND=$(command -v "$c")
			return 0
		fi
	done
	return 1
}
have_sdl2() { pkg-config --exists sdl2 2>/dev/null || works sdl2-config; }

check_tools() {
	MISSING=()
	find_cxx || MISSING+=(compiler)
	find_cmake || MISSING+=(cmake)
	find_ninja || MISSING+=(ninja)
	command -v pkg-config >/dev/null || MISSING+=(pkg-config)
	have_sdl2 || MISSING+=(sdl2)
}

DISTRO=""
if [ -r /etc/os-release ]; then
	DISTRO=$(. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}")
fi
# The distribution's packages for what's missing.
packages_for() {
	local want=("$@") pkgs=() w
	case " $DISTRO " in
	*" arch "*)
		for w in "${want[@]}"; do case $w in compiler) pkgs+=(base-devel) ;; cmake) pkgs+=(cmake) ;; ninja) pkgs+=(ninja) ;; pkg-config) pkgs+=(pkgconf) ;; sdl2) pkgs+=(sdl2) ;; esac done
		[ ${#pkgs[@]} -gt 0 ] && echo "sudo pacman -S --needed ${pkgs[*]}"
		;;
	*" debian "* | *" ubuntu "*)
		for w in "${want[@]}"; do case $w in compiler) pkgs+=(g++) ;; cmake) pkgs+=(cmake) ;; ninja) pkgs+=(ninja-build) ;; pkg-config) pkgs+=(pkg-config) ;; sdl2) pkgs+=(libsdl2-dev) ;; esac done
		[ ${#pkgs[@]} -gt 0 ] && echo "sudo apt-get install -y ${pkgs[*]}"
		;;
	*" fedora "* | *" rhel "*)
		for w in "${want[@]}"; do case $w in compiler) pkgs+=(gcc-c++ libstdc++-static) ;; cmake) pkgs+=(cmake) ;; ninja) pkgs+=(ninja-build) ;; pkg-config) pkgs+=(pkgconf-pkg-config) ;; sdl2) pkgs+=(SDL2-devel) ;; esac done
		[ ${#pkgs[@]} -gt 0 ] && echo "sudo dnf install -y ${pkgs[*]}"
		;;
	*" suse "* | *" opensuse "*)
		for w in "${want[@]}"; do case $w in compiler) pkgs+=(gcc-c++) ;; cmake) pkgs+=(cmake) ;; ninja) pkgs+=(ninja) ;; pkg-config) pkgs+=(pkg-config) ;; sdl2) pkgs+=(SDL2-devel) ;; esac done
		[ ${#pkgs[@]} -gt 0 ] && echo "sudo zypper install -y ${pkgs[*]}"
		;;
	esac
}

# CMake and Ninja need no root: their own builds go in ~/.cache/devilutionx-fp/tools.
download() {
	if command -v curl >/dev/null; then curl -fsSL -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
}
fetch_cmake_ninja() {
	local arch
	arch=$(uname -m)
	case "$arch" in x86_64 | aarch64) ;; *) die "no ready-made CMake for $arch: install cmake and ninja with your package manager" ;; esac
	command -v curl >/dev/null || command -v wget >/dev/null || die "downloading CMake needs curl or wget"
	mkdir -p "$TOOLS"
	local tmp
	tmp=$(mktemp -d)
	if [ -z "$CMAKE" ]; then
		info "Downloading CMake $CMAKE_VERSION..."
		download "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-$arch.tar.gz" "$tmp/cmake.tar.gz"
		rm -rf "$TOOLS/cmake" && mkdir -p "$TOOLS/cmake"
		tar -xzf "$tmp/cmake.tar.gz" -C "$TOOLS/cmake" --strip-components=1
	fi
	if [ -z "$NINJA" ]; then
		info "Downloading Ninja $NINJA_VERSION..."
		local zip=ninja-linux.zip
		[ "$arch" = aarch64 ] && zip=ninja-linux-aarch64.zip
		download "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/$zip" "$tmp/ninja.zip"
		if command -v unzip >/dev/null; then
			unzip -o -q "$tmp/ninja.zip" -d "$TOOLS"
		else
			python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$tmp/ninja.zip" "$TOOLS"
		fi
		chmod +x "$TOOLS/ninja"
	fi
	rm -rf "$tmp"
}

check_tools
if [ ${#MISSING[@]} -gt 0 ]; then
	warn "Missing: ${MISSING[*]}"
	cmd=$(packages_for "${MISSING[@]}" || true)
	if [ -n "$cmd" ]; then
		info "Your package manager can install them:"
		info "  $cmd"
		# Never unattended (--yes): it needs root.
		if [ "$YES" != 1 ] && yesno "Run that now (it asks for your password)?" Y; then
			eval "$cmd" || warn "The install didn't finish."
			check_tools
		fi
	fi
fi
# Still no CMake or Ninja (no package manager, or no root): their ready-made builds will do.
if [ ${#MISSING[@]} -gt 0 ] && [ -z "$(printf '%s\n' "${MISSING[@]}" | grep -v -x -e cmake -e ninja)" ]; then
	if yesno "Download CMake and Ninja for this build only (into $TOOLS)?" Y; then
		fetch_cmake_ninja
		check_tools
	fi
fi
if [ ${#MISSING[@]} -gt 0 ]; then
	hint=$(packages_for "${MISSING[@]}" || true)
	die "still missing: ${MISSING[*]}${hint:+ (install with: $hint)}"
fi
info "compiler: $CXX_FOUND ($("$CXX_FOUND" --version | head -n 1))"
info "cmake:    $CMAKE ($("$CMAKE" --version | head -n 1 | awk '{print $3}'))"
info "ninja:    $NINJA"

# ---------------------------------------------------------------------------------------------------------------------
# Build

step "Building the game (the first build takes a few minutes)"
# Libraries the system has are used; the others are built with the game.
system_lib() {
	if pkg-config --exists "$1" 2>/dev/null || { [ -n "${2:-}" ] && [ -f "$2" ]; }; then echo ON; else echo OFF; fi
}
CONFIG=(
	-G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA"
	-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DNONET=ON
	-DDEVILUTIONX_SYSTEM_LUA=OFF -DDEVILUTIONX_STATIC_LUA=ON
	-DDEVILUTIONX_DISABLE_EXCEPTIONS=ON -DDEVILUTIONX_DISABLE_RTTI=ON
	-DDEVILUTIONX_SYSTEM_ZLIB="$(system_lib zlib)"
	-DDEVILUTIONX_SYSTEM_LIBPNG="$(system_lib libpng)"
	-DDEVILUTIONX_SYSTEM_BZIP2="$(system_lib bzip2 /usr/include/bzlib.h)"
)
# A build folder made with another generator or compiler would refuse the settings: start it afresh.
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -q "^CMAKE_GENERATOR:INTERNAL=Ninja$" "$BUILD/CMakeCache.txt"; then
	rm -rf "$BUILD"
fi
"$CMAKE" -S "$REPO" -B "$BUILD" "${CONFIG[@]}" >"$REPO/build-release.log" 2>&1 ||
	die "configuring the build failed; the log is in build-release.log"
# Ninja's step count as it goes, on one line in a terminal.
progress() {
	if [ -t 1 ]; then
		grep --line-buffered -E '^\[[0-9]+/[0-9]+\]' | awk '{ printf "\r    %s ", $1; fflush() } END { print "" }'
	else
		cat >/dev/null
	fi
}
set +e
"$CMAKE" --build "$BUILD" ${JOBS:+--parallel "$JOBS"} 2>&1 | tee -a "$REPO/build-release.log" | progress
status=${PIPESTATUS[0]}
set -e
[ "$status" = 0 ] || die "the build failed; the end of build-release.log says why"
ok "Built."

# ---------------------------------------------------------------------------------------------------------------------
# Install

step "Installing to $DIR"
mkdir -p "$DIR"
install -m 755 "$BUILD/devilutionx" "$DIR/devilutionx"
rm -rf "$DIR/assets" "$DIR/mods"
cp -r "$BUILD/assets" "$DIR/assets"
[ -d "$BUILD/mods" ] && cp -r "$BUILD/mods" "$DIR/mods"
[ -f "$BUILD/devilutionx.mpq" ] && cp "$BUILD/devilutionx.mpq" "$DIR/"
cp "$REPO/Packaging/resources/icon.png" "$DIR/icon.png"
cp "$REPO/Packaging/resources/hellfire.png" "$DIR/hellfire.png"
cp "$REPO/LICENSE.md" "$DIR/LICENSE.md"
cp "$REPO/assets/fp/CREDITS.md" "$DIR/CREDITS.md"
ok "The game"

# Copies a file under the name the game looks for (the MPQs are found in any case; ours are as GOG names them).
copy_as() {
	if [ "$(realpath "$1")" != "$(realpath -m "$2")" ]; then cp "$1" "$2"; fi
}
unpacked=""
if [ -n "$GOG_INSTALLER" ]; then
	info "Unpacking $(basename "$GOG_INSTALLER")..."
	unpacked=$(mktemp -d)
	include=(-I DIABDAT.MPQ -I spawn.mpq)
	[ "$GAME" = hellfire ] && include+=(-I hellfire)
	innoextract -s -d "$unpacked" "${include[@]}" "$GOG_INSTALLER" >/dev/null ||
		die "innoextract couldn't unpack $GOG_INSTALLER (a newer innoextract may be needed)"
	DIABLO=$unpacked
fi
while read -r f; do
	[ -n "$f" ] || continue
	[ "$GAME" = hellfire ] || ! is_hellfire_file "$f" || continue
	name=$(basename "$f")
	case "${name,,}" in diabdat.mpq) name=DIABDAT.MPQ ;; *) name=${name,,} ;; esac
	copy_as "$f" "$DIR/$name"
done < <(diablo_files "$DIABLO")
[ -z "$unpacked" ] || rm -rf "$unpacked"
# Diablo alone: Hellfire's data from an earlier install goes, or the game would still offer it.
if [ "$GAME" = diablo ]; then
	while read -r f; do
		if is_hellfire_file "$f"; then rm -f "$f"; fi
	done < <(diablo_files "$DIR")
fi
ok "$(describe_contents "$(contents_of "$DIR")")"

# The weapon art goes in daggerfall/ next to the game, where it looks for it; the names in capitals, as it asks for them.
if [ -n "$DAGGERFALL" ]; then
	mkdir -p "$DIR/daggerfall"
	count=0
	for name in WEAPON00.CIF WEAPON01.CIF WEAPON02.CIF WEAPON03.CIF WEAPON04.CIF WEAPON05.CIF WEAPON06.CIF \
		WEAPON07.CIF WEAPON08.CIF WEAPON09.CIF WEAPON10.CIF WEAPON11.CIF ART_PAL.COL; do
		f=$(find_ci "$DAGGERFALL" "$name")
		if [ -n "$f" ]; then
			copy_as "$f" "$DIR/daggerfall/$name"
			count=$((count + 1))
		else
			warn "Not in Daggerfall's folder: $name"
		fi
	done
	ok "Daggerfall's weapons ($count of 13 files)"
elif [ "$NO_DAGGERFALL" = 1 ]; then
	rm -rf "$DIR/daggerfall"
fi

if [ "$SHORTCUTS" = 1 ]; then
	mkdir -p "$(dirname "$BIN_LINK")" "$APPS"
	ln -sfn "$DIR/devilutionx" "$BIN_LINK"
	desktop_entry() { # file name exec-args icon comment
		cat >"$1" <<-EOF
			[Desktop Entry]
			Type=Application
			Name=$2
			Comment=$5
			Exec="$DIR/devilutionx"$3
			Path=$DIR
			Icon=$4
			Terminal=false
			Categories=Game;RolePlaying;
		EOF
	}
	if [ "$GAME" = hellfire ]; then
		desktop_entry "$APPS/devilutionx-fp.desktop" "Diablo First-Person" " --diablo" "$DIR/icon.png" "Diablo in first person (DevilutionX)"
		desktop_entry "$APPS/devilutionx-fp-hellfire.desktop" "Hellfire First-Person" " --hellfire" "$DIR/hellfire.png" "Diablo: Hellfire in first person (DevilutionX)"
	else
		desktop_entry "$APPS/devilutionx-fp.desktop" "Diablo First-Person" "" "$DIR/icon.png" "Diablo in first person (DevilutionX)"
		rm -f "$APPS/devilutionx-fp-hellfire.desktop"
	fi
	command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" >/dev/null 2>&1 || true
	ok "Launcher: devilutionx-fp, and the applications menu"
	case ":$PATH:" in *":$(dirname "$BIN_LINK"):"*) ;; *) warn "$(dirname "$BIN_LINK") isn't on your PATH: start it as $DIR/devilutionx" ;; esac
fi

step "Done"
if [ "$SHORTCUTS" != 1 ]; then
	info "Start it as $DIR/devilutionx${B}${RST}$([ "$GAME" = hellfire ] && echo " (it asks which game; --diablo or --hellfire picks one)")."
elif [ "$GAME" = hellfire ]; then
	info "Start it from your applications menu (Diablo First-Person, Hellfire First-Person), with ${B}devilutionx-fp${RST}"
	info "(it asks which game; devilutionx-fp --diablo or --hellfire picks one), or as $DIR/devilutionx."
else
	info "Start it from your applications menu (Diablo First-Person), with ${B}devilutionx-fp${RST}, or as $DIR/devilutionx."
fi
info "It starts in first person: WASD walks, the mouse looks, F4 switches to the original view."
info "To start in the original view instead: Settings > Gameplay > Start in First Person."
info "To update after pulling new changes, run ./install.sh again; ./install.sh --uninstall removes it."
if [ "$YES" != 1 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && yesno "Start the game now?" Y; then
	cd "$DIR" && nohup ./devilutionx >/dev/null 2>&1 &
fi
