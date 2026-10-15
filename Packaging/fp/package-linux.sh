#!/usr/bin/env bash
# Builds Diablo First-Person for Linux and packs it as the tarball a player unpacks and installs with its install.sh
# (the same script as the source checkout's, which then skips the build): the game, its assets, the Hellfire mod,
# install.sh, the icons and a README. The game needs only SDL2 from the player's system, and a C library as recent as
# the build machine's (Ubuntu 24.04's, glibc 2.39): every other library is built into it.
#
#   Packaging/fp/package-linux.sh                 -> build-linux/release/DevilutionX-FP-v1-linux-x86_64.tar.gz
#   FP_VERSION=v1.1 Packaging/fp/package-linux.sh
#
# Needs cmake (3.22+), ninja, a C++23 compiler, SDL2's development files and gettext's msgfmt. The GitHub workflow
# fp_release.yml runs this.
set -euo pipefail

cd "$(dirname "$0")/../.."
ROOT=$PWD
VERSION=${FP_VERSION:-v1}
BUILD=${BUILD_DIR:-$ROOT/build-linux}
OUT=$BUILD/release
NAME=DevilutionX-FP-$VERSION-linux-$(uname -m)

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DNONET=ON \
	-DDEVILUTIONX_SYSTEM_LUA=OFF -DDEVILUTIONX_STATIC_LUA=ON \
	-DDEVILUTIONX_DISABLE_EXCEPTIONS=ON -DDEVILUTIONX_DISABLE_RTTI=ON \
	-DDEVILUTIONX_SYSTEM_ZLIB=OFF -DDEVILUTIONX_SYSTEM_BZIP2=OFF -DDEVILUTIONX_SYSTEM_LIBPNG=OFF \
	-DDEVILUTIONX_SYSTEM_SDL_IMAGE=OFF -DBUILD_ASSETS_MPQ=OFF -DDISCORD_INTEGRATION=OFF
cmake --build "$BUILD"

PKG=$OUT/$NAME
rm -rf "$PKG" "$OUT/$NAME.tar.gz"
mkdir -p "$PKG"
cp "$BUILD/devilutionx" "$ROOT/install.sh" "$PKG/"
cp -r "$BUILD/assets" "$PKG/assets"
cp -r "$BUILD/mods" "$PKG/mods"
cp "$ROOT/Packaging/resources/icon.png" "$ROOT/Packaging/resources/hellfire.png" "$PKG/"
cp "$ROOT/Packaging/fp/linux/README.txt" "$PKG/README.txt"
cp "$ROOT/LICENSE.md" "$PKG/LICENSE.md"
cp "$ROOT/assets/fp/CREDITS.md" "$PKG/CREDITS.md"
tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "The game's libraries from the system:"
readelf -d "$PKG/devilutionx" | awk '/NEEDED/ { print "  " $NF }'
echo "Packed $OUT/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1))"
