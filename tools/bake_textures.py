#!/usr/bin/env python3
"""PNG -> .tex baker (run-length encoded, rows top-first, like three.js flipY with t = 1 - v).

.tex (little endian):
  char[4] "CRT1"
  u16 width, u16 height, u8 channels (3 = RGB, 4 = RGBA), u8 reserved, u32 run_count
  run_count x { u16 length, u8[channels] colour }

The model textures are MagicaVoxel palettes (2-10 colours on up to 1024x512), so RLE makes them tiny.
Usage: python tools/bake_textures.py <src_png> <dst_tex>
       python tools/bake_textures.py --all <upstream_assets_dir> <data_dir>
"""
import os
import struct
import sys

from PIL import Image


def bake(src, dst):
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    alpha_min, _ = im.getchannel("A").getextrema()
    channels = 4 if alpha_min < 255 else 3
    data = im.tobytes() if channels == 4 else im.convert("RGB").tobytes()
    runs = []
    prev, length = None, 0
    for i in range(0, len(data), channels):
        px = data[i:i + channels]
        if px == prev and length < 65535:
            length += 1
        else:
            if prev is not None:
                runs.append((length, prev))
            prev, length = px, 1
    runs.append((length, prev))

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(b"CRT1")
        f.write(struct.pack("<HHBBI", w, h, channels, 0, len(runs)))
        for n, px in runs:
            f.write(struct.pack("<H", n))
            f.write(px)
    return w, h, channels, len(runs)


def main(argv):
    if len(argv) == 3 and argv[0] == "--all":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from assets_manifest import IMAGES, TEXTURES
        for folder, table in (("textures", TEXTURES), ("images", IMAGES)):
            for name, rel in table.items():
                w, h, c, runs = bake(os.path.join(argv[1], rel), os.path.join(argv[2], folder, name + ".tex"))
                print(f"{folder} {name:28s} {w}x{h} ch={c} runs={runs}")
        return 0
    if len(argv) == 2:
        print(bake(argv[0], argv[1]))
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
