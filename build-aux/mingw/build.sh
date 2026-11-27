#!/usr/bin/env bash
# Builds the Windows release of Grouwdi-OBS-FrameGen on Linux with MinGW-w64:
# the plugin DLL, the manual-install zip, and (if makensis is installed) the installer.
#
# Needs: x86_64-w64-mingw32-gcc / -dlltool / -windres / -strip, zip, and optionally
# makensis (NSIS 3). Compiles against the OBS Studio source headers; OBS itself is not
# built. The import libraries for obs.dll and w32-pthreads.dll are generated from the
# .def files in this folder, so no OBS binaries are needed either.
#
# Usage: build-aux/mingw/build.sh /path/to/obs-studio-31.1.1 [simde-include-dir]
#   obs-studio source: https://github.com/obsproject/obs-studio/archive/refs/tags/31.1.1.tar.gz
#   simde (only if your OBS headers need it): https://github.com/simd-everywhere/simde
set -euo pipefail

OBS_SRC=${1:?usage: build.sh /path/to/obs-studio-source [simde-include-dir]}
SIMDE=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NAME=$(sed -n 's/.*"name": *"\([^"]*\)".*/\1/p' "$ROOT/buildspec.json" | head -1)
# "version" is the numeric x.y.z the OBS template's CMake needs; "versionLabel" (if set)
# is what people see: the OBS log, file names, installer and file properties.
VERSION=$(sed -n 's/.*"versionLabel": *"\([^"]*\)".*/\1/p' "$ROOT/buildspec.json" | head -1)
[ -n "$VERSION" ] || VERSION=$(sed -n 's/.*"version": *"\([0-9][0-9.]*\)".*/\1/p' "$ROOT/buildspec.json" | tail -1)
OUT="$ROOT/release"
B="$ROOT/build_mingw"
CC=x86_64-w64-mingw32-gcc

[ -f "$OBS_SRC/libobs/obs-module.h" ] || { echo "not an OBS source tree: $OBS_SRC" >&2; exit 1; }
echo "Building $NAME $VERSION"
rm -rf "$B"
mkdir -p "$B/gen" "$OUT"

# generated headers OBS normally creates during its own CMake configure
printf '#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n' > "$B/gen/obsconfig.h"
[ -n "$SIMDE" ] && ln -s "$SIMDE" "$B/gen/simde"

# import libraries
x86_64-w64-mingw32-dlltool -d "$HERE/obs.def" -l "$B/libobs-import.a" -D obs.dll
x86_64-w64-mingw32-dlltool -d "$HERE/w32-pthreads.def" -l "$B/libpth-import.a" -D w32-pthreads.dll

# plugin-support.c from the template, with name, version and copyright filled in
sed -e "s/@CMAKE_PROJECT_NAME@/$NAME/; s/@CMAKE_PROJECT_VERSION@/$VERSION/" \
    -e "s/^Plugin Name\$/$NAME/" \
    -e 's/^Copyright (C) <Year> <Developer> <Email Address>$/Copyright (C) 2026 Grouwdi/' \
    "$ROOT/src/plugin-support.c.in" > "$B/plugin-support.c"

# Windows version resource
# Windows wants a four-part numeric version: 1.0 -> 1,0,0 (the template adds the last ,0)
IFS=. read -r V1 V2 V3 _ <<< "$VERSION"
VERSION_COMMAS="${V1:-0},${V2:-0},${V3:-0}"
sed -e "s/@NAME@/$NAME/g; s/@VERSION@/$VERSION/g; s/@VERSION_COMMAS@/$VERSION_COMMAS/g" \
    "$HERE/version.rc.in" > "$B/version.rc"
x86_64-w64-mingw32-windres "$B/version.rc" -O coff -o "$B/version.o"

T="$ROOT/third_party"
CFLAGS=(-O2 -DWIN32 -D_WIN32_WINNT=0x0A00 -include "$HERE/msvc_shim.h"
	-I"$ROOT/src" -I"$OBS_SRC/libobs" -I"$OBS_SRC/deps/w32-pthreads" -I"$B/gen"
	-I"$T/cuda-shim" -I"$T/ffnvcodec" -I"$T/nvof"
	-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers)
for f in plugin-main capture-smoother-filter sc-core sc-timing sc-flowmath motion-smoother-filter sc-gpu; do
	$CC "${CFLAGS[@]}" -c "$ROOT/src/$f.c" -o "$B/$f.o"
done
$CC "${CFLAGS[@]}" -c "$B/plugin-support.c" -o "$B/plugin-support.o"
$CC -shared -o "$B/$NAME.dll" "$B"/*.o "$HERE/plugin.def" "$B/libobs-import.a" "$B/libpth-import.a" \
	-ldxguid -ladvapi32 -Wl,--exclude-libs,ALL -static-libgcc
x86_64-w64-mingw32-strip "$B/$NAME.dll"

# staging folder: plugin folder as OBS expects it, plus documents (Windows line endings)
S="$B/stage"
mkdir -p "$S/$NAME/bin/64bit" "$S/$NAME/data/locale"
cp "$B/$NAME.dll" "$S/$NAME/bin/64bit/"
cp "$ROOT/data/motion-smoother.effect" "$S/$NAME/data/"
cp "$ROOT/data/locale/en-US.ini" "$S/$NAME/data/locale/"
crlf() { sed 's/\r$//; s/$/\r/' "$1" > "$2"; }
crlf "$ROOT/README.md" "$S/README.txt"
crlf "$ROOT/LICENSE" "$S/LICENSE.txt"
crlf "$ROOT/THIRD-PARTY-NOTICES.txt" "$S/THIRD-PARTY-NOTICES.txt"
crlf "$ROOT/installer/INSTALL.txt" "$S/INSTALL.txt"

rm -f "$OUT/$NAME-$VERSION-Windows.zip"
(cd "$S" && zip -qr "$OUT/$NAME-$VERSION-Windows.zip" INSTALL.txt README.txt LICENSE.txt THIRD-PARTY-NOTICES.txt "$NAME")
echo "  $OUT/$NAME-$VERSION-Windows.zip"

if command -v makensis >/dev/null; then
	makensis -V2 -DSTAGE="$S" -DOUTFILE="$OUT/$NAME-$VERSION-Setup.exe" "$ROOT/installer/installer.nsi"
	rm -f "$OUT/$NAME-$VERSION-Setup.zip"
	(cd "$OUT" && zip -q "$NAME-$VERSION-Setup.zip" "$NAME-$VERSION-Setup.exe")
	echo "  $OUT/$NAME-$VERSION-Setup.exe (and .zip)"
else
	echo "  makensis not found: installer skipped"
fi
