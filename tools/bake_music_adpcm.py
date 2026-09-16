#!/usr/bin/env python3
"""Music/*.mp3 -> <data_dir>/music/<name>.wav for the SF2000: Microsoft ADPCM, mono, 22050 Hz, 1024-byte blocks.

The core streams these from the card block by block (src/engine/adpcm.*), looping - the way the user's SF2000 game
Santa plays its music; Ogg Vorbis needs floating point, which the SF2000 build does not have.

Usage: python tools/bake_music_adpcm.py --all <music_dir> <data_dir>   (names from tools/assets_manifest.py MUSIC)
"""
import os
import subprocess
import sys


def bake(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-map_metadata", "-1", "-vn", "-ac", "1", "-ar", "22050",
                    "-c:a", "adpcm_ms", "-block_size", "1024", dst], check=True)
    return os.path.getsize(dst)


def main(argv):
    if len(argv) == 3 and argv[0] == "--all":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from assets_manifest import MUSIC
        total = 0
        for name, rel in MUSIC.items():
            size = bake(os.path.join(argv[1], rel), os.path.join(argv[2], "music", name + ".wav"))
            total += size
            print(f"music {name:8s} {size / 1024:7.0f} KB  <- {rel}")
        print(f"music total {total / 1024 / 1024:.1f} MB")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
