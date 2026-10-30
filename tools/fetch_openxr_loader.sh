#!/bin/sh
# Downloads Khronos' 64-bit openxr_loader.dll (Apache-2.0) for the OpenXR helper and puts it in
# <out>/openxr_loader.dll. The helper loads it from its own folder (bin\infravr\openxr).
# Usage: tools/fetch_openxr_loader.sh <out folder> [version]
set -e
OUT=${1:?usage: fetch_openxr_loader.sh <out folder> [version]}
VER=${2:-1.1.43}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/loader.zip" \
    "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$VER/openxr_loader_windows-$VER.zip"
unzip -q "$TMP/loader.zip" -d "$TMP/x"
DLL=$(find "$TMP/x" -path '*x64*' -name openxr_loader.dll | head -n 1)
[ -n "$DLL" ] || { echo "openxr_loader.dll (x64) not found in the archive" >&2; exit 1; }
mkdir -p "$OUT"
cp "$DLL" "$OUT/openxr_loader.dll"
echo "openxr_loader.dll $VER -> $OUT"
