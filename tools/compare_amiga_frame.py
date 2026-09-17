#!/usr/bin/env python3
"""Compare a frame the Amiga drew against the same frame from the 3D renderer (task G9).

    python tools/compare_amiga_frame.py --amiga out/check/amiga/compare/amiga.png \
                                        --reference out/check/amiga/compare/ref_t120.png \
                                        --out out/check/amiga/compare/diff.png

WHY THIS IS NOT A PIXEL-EQUALITY TEST: the Amiga draws pre-rendered sprites from a 127-colour palette; the
reference draws the same scene as shaded triangles in full colour. They cannot match bit for bit and it would
be wrong to demand it. What CAN be checked is that the two pictures agree about where things are - that the
port did not put the road, the hero or the traffic somewhere else.

So this reports:
  * how much of the picture differs at all, and by how much on average,
  * the difference per horizontal band, which is where a projection or offset error shows up as one band
    being far worse than its neighbours,
  * a difference image, bright where the two disagree, to be looked at rather than summarised.

A uniform, modest difference is the expected result: sprites versus shading. A band that stands out is a bug.

MEASURED CAVEAT (and the reason this tool alone cannot close the question): a percentage of differing pixels
conflates three unrelated things - a different world state, missing objects, and shading. On this port it
read 78.8% for two completely different games, then 68.1% for the same scene, and the number moved for
reasons that had nothing to do with the renderer. What finally discriminated was a COLOUR HISTOGRAM: both
sides used the same grass, asphalt and water values, but in different amounts, and one side carried fourteen
colours the other did not. Compare histograms before believing a percentage.
"""
import argparse
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("needs Pillow: python -m pip install Pillow")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--amiga", required=True)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--out", help="write a difference image here")
    ap.add_argument("--bands", type=int, default=8)
    args = ap.parse_args()

    a = Image.open(args.amiga).convert("RGB")
    b = Image.open(args.reference).convert("RGB")
    if a.size != b.size:
        print("sizes differ: amiga %s, reference %s - cropping to the smaller" % (a.size, b.size))
        w = min(a.size[0], b.size[0])
        h = min(a.size[1], b.size[1])
        a = a.crop((0, 0, w, h))
        b = b.crop((0, 0, w, h))

    w, h = a.size
    diff = ImageChops.difference(a, b)
    px = diff.load()

    total = w * h
    differing = 0
    sum_delta = 0
    band_h = max(1, h // args.bands)
    bands = [[0, 0] for _ in range((h + band_h - 1) // band_h)]

    for y in range(h):
        row_band = bands[y // band_h]
        for x in range(w):
            r, g, bl = px[x, y]
            d = r + g + bl
            if d > 24:  # ignore the last bit or two of palette rounding
                differing += 1
                row_band[0] += 1
            sum_delta += d
            row_band[1] += d

    print("size %dx%d" % (w, h))
    print("pixels differing: %d of %d (%.1f%%)" % (differing, total, 100.0 * differing / total))
    print("mean difference: %.1f of 765" % (float(sum_delta) / total))
    print("")
    print("per band (a band far worse than its neighbours means a placement error, not shading):")
    for i, (count, delta) in enumerate(bands):
        rows = min(band_h, h - i * band_h)
        n = rows * w
        if n <= 0:
            continue
        print("  rows %3d-%3d: %5.1f%% differing, mean %.1f"
              % (i * band_h, i * band_h + rows - 1, 100.0 * count / n, float(delta) / n))

    if args.out:
        diff.point(lambda v: min(255, v * 3)).save(args.out)
        print("\ndifference image -> %s" % args.out)


if __name__ == "__main__":
    main()
