#!/usr/bin/env python3
"""Compares screenshots of the original and the port pixel by pixel.

  python tools/compare_images.py out/shots_ref out/shots_port [--diff out/shots_diff] [--max-mean 4 --max-over 1.5]

Pairs files with the same name. Per image: mean absolute difference over RGB (0-255) and the percentage of
pixels whose largest channel difference exceeds 40. With --diff, writes an image per pair: the port darkened,
with differing pixels in red. Exit code 1 when any pair exceeds the limits (or a file is missing).
"""
import argparse
import os
import sys

from PIL import Image, ImageChops


def to_rgb565(im):
    """Quantises to RGB565 and widens back by bit replication (what the SF2000 software renderer stores)."""
    r, g, b = im.split()
    five = r.point(lambda v: (v >> 3 << 3) | (v >> 6))
    return Image.merge("RGB", (five, g.point(lambda v: (v >> 2 << 2) | (v >> 6)),
                               b.point(lambda v: (v >> 3 << 3) | (v >> 6))))


def compare(a_path, b_path, diff_path, rgb565=False):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if rgb565:
        a, b = to_rgb565(a), to_rgb565(b)
    if a.size != b.size:
        return None, None, f"size {a.size} vs {b.size}"
    d = ImageChops.difference(a, b)
    hist_total = 0
    for band in d.split():
        h = band.histogram()
        hist_total += sum(i * n for i, n in enumerate(h))
    w, h = a.size
    mean = hist_total / (w * h * 3)
    # largest channel difference per pixel
    r, g, bl = d.split()
    mx = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    over = sum(mx.histogram()[41:])
    over_pct = 100.0 * over / (w * h)
    if diff_path:
        mask = mx.point(lambda v: 255 if v > 40 else 0)
        base = Image.blend(b, Image.new("RGB", b.size, (0, 0, 0)), 0.6)
        base.paste(Image.new("RGB", b.size, (255, 0, 0)), mask=mask)
        base.save(diff_path)
    return mean, over_pct, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("port")
    ap.add_argument("--diff")
    ap.add_argument("--max-mean", type=float, default=4.0)
    ap.add_argument("--max-over", type=float, default=1.5)
    ap.add_argument("--rgb565", action="store_true", help="quantise both images to RGB565 first (SF2000 shots)")
    args = ap.parse_args()
    if args.diff:
        os.makedirs(args.diff, exist_ok=True)
    names = sorted(n for n in os.listdir(args.ref) if n.lower().endswith(".png"))
    failed = 0
    for n in names:
        p = os.path.join(args.port, n)
        if not os.path.exists(p):
            print(f"{n}: MISSING in port")
            failed += 1
            continue
        mean, over, err = compare(os.path.join(args.ref, n), p, os.path.join(args.diff, n) if args.diff else None,
                                  args.rgb565)
        if err:
            print(f"{n}: {err}")
            failed += 1
            continue
        bad = mean > args.max_mean or over > args.max_over
        failed += bad
        print(f"{n}: mean {mean:.2f}/255, {over:.2f}% pixels > 40{'  EXCEEDS' if bad else ''}")
    if not names:
        print("no reference images")
        return 1
    print("OK" if not failed else f"FAILED: {failed} image(s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
