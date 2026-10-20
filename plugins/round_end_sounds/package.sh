#!/bin/bash
# Assembles build/package/round_end_sounds: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/round_end_sounds"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/round_end_sounds/libround_end_sounds.so" "$OUT/bin/"
cp "$ROOT/build/plugins/round_end_sounds/round_end_sounds.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
