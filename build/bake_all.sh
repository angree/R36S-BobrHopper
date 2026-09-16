#!/bin/sh
# Regenerates data/ from the pristine upstream tarball. Run from Git Bash.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
SRC=upstream/expo-crossy-road/assets
if [ ! -d "$SRC" ]; then
  mkdir -p upstream && tar -xzf upstream/expo-crossy-road-6f2e84e5-noaudio.tar.gz -C upstream
fi
rm -rf data
python tools/bake_models.py --all "$SRC" data
python tools/bake_textures.py --all "$SRC" data
python tools/bake_audio.py --all "$SRC" data
python tools/bake_font.py --all "$SRC" data
python tools/bake_music.py --all Music data
python tools/write_manifest.py data/manifest.txt
echo "bake_all: $(find data -type f | wc -l) files, $(du -sh data | cut -f1)"
