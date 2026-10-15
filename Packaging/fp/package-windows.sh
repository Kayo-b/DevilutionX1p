#!/usr/bin/env bash
# Builds Diablo First-Person for 64-bit Windows, on Linux, and packs it as the zip a player unpacks and sets up with
# Install.bat: devilutionx.exe, SDL2.dll, the game's assets (assets/fp/ loose: first person reads its files from the
# folder, not from devilutionx.mpq), the Hellfire mod, the installer and a README.
#
#   Packaging/fp/package-windows.sh                 -> build-windows/release/DevilutionX-FP-v1-windows-x64.zip
#   FP_VERSION=v1.1 Packaging/fp/package-windows.sh -> build-windows/release/DevilutionX-FP-v1.1-windows-x64.zip
#
# Needs cmake (3.22+), ninja, curl, tar, zip, and gettext's msgfmt for the translations. The cross-compiler
# (llvm-mingw, no install needed) and SDL2's MinGW build are downloaded once into ~/.cache/devilutionx-fp; set
# LLVM_MINGW and SDL2_MINGW to use copies of your own. The GitHub workflow fp_release.yml runs this.
set -euo pipefail

LLVM_MINGW_VERSION=20260922
SDL2_VERSION=2.32.10

cd "$(dirname "$0")/../.."
ROOT=$PWD
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/devilutionx-fp
VERSION=${FP_VERSION:-v1}
BUILD=${BUILD_DIR:-$ROOT/build-windows}
OUT=$BUILD/release # not dist/: CMake takes a dist/ folder for a source distribution's
NAME=DevilutionX-FP-$VERSION-windows-x64

mkdir -p "$CACHE"
if [ -z "${LLVM_MINGW:-}" ]; then
	LLVM_MINGW=$CACHE/llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-x86_64
	if [ ! -x "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" ]; then
		echo "Downloading llvm-mingw $LLVM_MINGW_VERSION..."
		curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_VERSION/llvm-mingw-$LLVM_MINGW_VERSION-ucrt-ubuntu-22.04-x86_64.tar.xz" |
			tar -xJ -C "$CACHE"
	fi
fi
export LLVM_MINGW
if [ -z "${SDL2_MINGW:-}" ]; then
	SDL2_MINGW=$CACHE/SDL2-$SDL2_VERSION
	if [ ! -d "$SDL2_MINGW/x86_64-w64-mingw32" ]; then
		echo "Downloading SDL2 $SDL2_VERSION..."
		curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-devel-$SDL2_VERSION-mingw.tar.gz" |
			tar -xz -C "$CACHE"
	fi
fi
SDL2_PREFIX=$SDL2_MINGW/x86_64-w64-mingw32

# As the Linux installer builds it (install.sh), plus every library built from source: the cross-compiler has none.
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/CMake/platforms/llvm-mingw64.toolchain.cmake" \
	-DSDL2_DIR="$SDL2_PREFIX/lib/cmake/SDL2" \
	-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DNONET=ON \
	-DDEVILUTIONX_SYSTEM_LUA=OFF -DDEVILUTIONX_STATIC_LUA=ON \
	-DDEVILUTIONX_DISABLE_EXCEPTIONS=ON -DDEVILUTIONX_DISABLE_RTTI=ON \
	-DDEVILUTIONX_SYSTEM_ZLIB=OFF -DDEVILUTIONX_SYSTEM_BZIP2=OFF -DDEVILUTIONX_SYSTEM_LIBPNG=OFF \
	-DBUILD_ASSETS_MPQ=OFF -DDISCORD_INTEGRATION=OFF -DSCREEN_READER_INTEGRATION=OFF
cmake --build "$BUILD"

# Windows' own text files want CRLF line ends (cmd.exe misreads a batch file without them).
crlf() { sed 's/\r*$/\r/' "$1" >"$2"; }

PKG=$OUT/$NAME
rm -rf "$PKG" "$OUT/$NAME.zip"
mkdir -p "$PKG"
cp "$BUILD/devilutionx.exe" "$SDL2_PREFIX/bin/SDL2.dll" "$PKG/"
cp -r "$BUILD/assets" "$PKG/assets"
cp -r "$BUILD/mods" "$PKG/mods"
crlf "$ROOT/Packaging/fp/windows/Install.bat" "$PKG/Install.bat"
crlf "$ROOT/Packaging/fp/windows/install.ps1" "$PKG/install.ps1"
crlf "$ROOT/Packaging/fp/windows/README.txt" "$PKG/README.txt"
crlf "$ROOT/LICENSE.md" "$PKG/LICENSE.txt"
crlf "$ROOT/assets/fp/CREDITS.md" "$PKG/CREDITS.txt"
crlf "$ROOT/Packaging/resources/LICENSE.zlib.txt" "$PKG/LICENSE-SDL2.txt"
(cd "$OUT" && zip -qr9 "$NAME.zip" "$NAME")
echo "Packed $OUT/$NAME.zip ($(du -h "$OUT/$NAME.zip" | cut -f1))"
