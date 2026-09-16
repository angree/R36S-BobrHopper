#!/usr/bin/env python3
"""The game's own logo: BÓBR in white over HOPPER in red, the colours of the Polish flag (O15).

Drawn from the baked pixel font (data/fonts) so it matches every letter in the game, then tilted into the same
kind of isometric block the original's wordmark used: each word is sheared, extruded into a solid body and given
a thick black outline. Nothing from the original's logo is used - that one is a trademark of Hipster Whale.

    python tools/make_logo.py                 -> assets_extra/images/title.png (+ title_preview.png)
"""
import os
import struct
import sys

from PIL import Image, ImageChops

WHITE = (255, 255, 255)
RED = (214, 28, 44)
OUTLINE = (12, 12, 12)
SIZE = (800, 464)      # the box the title image is drawn into
SCALE = 3              # pixel size of the font in the logo
SHEAR = 0.30           # how far the text leans: a column at x rises by SHEAR * x
DEPTH = 14             # how thick the block is, in pixels
OUT_W = 8              # outline radius
OVERLAP = 0.42         # how much of the upper word's height the lower one climbs into


def load_font(path):
    buf = open(path, "rb").read()
    aw, ah, ps, lh, n = struct.unpack_from("<5H", buf, 4)
    pos = 14
    glyphs = {}
    for _ in range(n):
        cp, x, y, w, h, xo, yo, adv = struct.unpack_from("<HHHHHhhH", buf, pos)
        pos += 16
        glyphs[cp] = (x, y, w, h, xo, yo, adv)
    return glyphs, Image.frombytes("L", (aw, ah), buf[pos:pos + aw * ah])


def text_mask(font, text):
    """1-bit mask of the text, tightly cropped, with room for accents above the line"""
    glyphs, atlas = font
    used = [glyphs[ord(c)] for c in text if ord(c) in glyphs]
    width = sum(g[6] for g in used)
    margin = max(16, max(-g[5] for g in used) + 8)
    height = max(g[3] + g[5] for g in used) + 2 * margin
    mask = Image.new("L", (width + 2 * margin, height), 0)
    pen = margin
    for c in text:
        g = glyphs.get(ord(c))
        if not g:
            continue
        x, y, w, h, xo, yo, adv = g
        if w and h:
            mask.paste(atlas.crop((x, y, x + w, y + h)), (pen + xo, margin + yo))
        pen += adv
    return mask.crop(mask.getbbox())


def lean(mask):
    """Shear the whole word downwards to the right - the left side sits high, as the original's wordmark did.

    The canvas grows by the full drop first: shearing in place used to push the right-hand letters out of the
    image, which tore holes in HOPPER and left loose pieces floating around the logo."""
    w, h = mask.size
    drop = int(w * SHEAR) + 2
    big = Image.new("L", (w, h + drop), 0)
    big.paste(mask, (0, 0))             # room below for the right side to fall into
    # AFFINE maps output -> input: subtracting SHEAR * x from the sampled y drops column x by that much
    out = big.transform((w, h + drop), Image.AFFINE, (1, 0, 0, -SHEAR, 1, 0), Image.NEAREST)
    return out.crop(out.getbbox())


def spread(mask, radius):
    """mask grown in every direction - the outline"""
    out = mask.copy()
    for dx in range(-radius, radius + 1):
        for dy in range(-radius, radius + 1):
            if dx * dx + dy * dy <= radius * radius:
                out = ImageChops.lighter(out, ImageChops.offset(mask, dx, dy))
    return out


def block(face, colour):
    """one word as a solid block: body extruded down-right, thick outline, flat face on top"""
    w, h = face.size
    pad = OUT_W + DEPTH + 2
    canvas = Image.new("L", (w + 2 * pad, h + 2 * pad), 0)
    canvas.paste(face, (pad, pad))

    body = canvas.copy()
    for i in range(1, DEPTH + 1):
        body = ImageChops.lighter(body, ImageChops.offset(canvas, i, i))

    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.paste(OUTLINE, (0, 0), spread(body, OUT_W))
    layer.paste(tuple(int(c * 0.45) for c in colour), (0, 0), body)
    layer.paste(colour, (0, 0), canvas)
    return layer.crop(layer.getbbox())


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    font = load_font(os.path.join(root, "data", "fonts", "retro_32.fnt"))

    words = []
    for text, colour in (("BÓBR", WHITE), ("HOPPER", RED)):
        m = text_mask(font, text)
        m = m.crop(m.getbbox())            # no empty margin: it only ate into how large the logo could be
        m = m.resize((m.width * SCALE, m.height * SCALE), Image.NEAREST)
        words.append(block(lean(m), colour))

    top, bottom = words
    # the words sit one on the other, the lower one climbing well into the upper one's slant
    gap = -int(top.height * OVERLAP)
    total_h = top.height + gap + bottom.height
    # fill the frame: whichever of width and height runs out first decides, and the logo may also grow
    scale = min((SIZE[0] - 16) / max(top.width, bottom.width), (SIZE[1] - 16) / total_h)
    if abs(scale - 1.0) > 0.01:
        top = top.resize((max(1, int(top.width * scale)), max(1, int(top.height * scale))), Image.NEAREST)
        bottom = bottom.resize((max(1, int(bottom.width * scale)), max(1, int(bottom.height * scale))), Image.NEAREST)
        gap = -int(top.height * OVERLAP)
        total_h = top.height + gap + bottom.height

    canvas = Image.new("RGBA", SIZE, (0, 0, 0, 0))
    y0 = (SIZE[1] - total_h) // 2
    canvas.alpha_composite(top, ((SIZE[0] - top.width) // 2, y0))
    canvas.alpha_composite(bottom, ((SIZE[0] - bottom.width) // 2, y0 + top.height + gap))

    out = os.path.join(root, "assets_extra", "images")
    os.makedirs(out, exist_ok=True)
    canvas.save(os.path.join(out, "title.png"))
    preview = Image.new("RGB", SIZE, (135, 198, 255))
    preview.paste(canvas, (0, 0), canvas)
    preview.save(os.path.join(out, "title_preview.png"))
    print(f"logo: BÓBR {top.size} + HOPPER {bottom.size}, skos {SHEAR}, grubosc {DEPTH}, obwodka {OUT_W} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
