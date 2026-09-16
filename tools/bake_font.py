#!/usr/bin/env python3
"""retro.ttf (8BIT WONDER) -> .fnt bitmap atlases, rendered without anti-aliasing (it is a pixel font).

.fnt (little endian):
  char[4] "CRF1"
  u16 atlas_w, u16 atlas_h, u16 pixel_size, u16 line_height, u16 glyph_count
  glyph_count x { u16 codepoint, u16 x, u16 y, u16 w, u16 h, i16 xoff, i16 yoff, u16 advance }
  atlas_w * atlas_h x u8 coverage (0 or 255), rows top-first
xoff/yoff place the glyph box relative to the pen position on the text's top line (negative above it: accents).
codepoint is Unicode; the engine maps the Polish letters onto glyph slots 128+ (engine/assets.h glyphSlot).

Usage: python tools/bake_font.py --all <upstream_assets_dir> <data_dir> [--sizes 6,7,8,9,16,24]
       (the SF2000 bakes half sizes: its TextRenderer.glyphScale 2 draws them for the 640x480 layouts on 320x240)
"""
import io
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

# the font's own letters and digits
CHARS = list(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
# sizes the original's screens use: 48/14 ScoreText (score, "TOP n"), 18 game over banners, 12 settings labels;
# the pixel font stays crisp only at baked sizes
SIZES = (12, 14, 16, 18, 32, 48)

# O11.6: the font has no Polish letters and no punctuation. Both are drawn here in the font's design units (300 per em,
# letters in a 250x250 box on the baseline, strokes 100 and gaps 50) with edges on a 50-unit grid, so they stay whole
# at the SF2000's 6 px. A Polish letter is the font's own letter with a diacritic drawn over it (the letter's pixels do
# not change); lower case uses the lower-case letter, whose shape is the capital's, as everywhere in this font.
ACCENTS = {
    # the accents sit right above the letter: a wider gap made the stroke over Ó look unattached in the logo
    "acute": [(75, 275, 175, 325), (125, 325, 225, 375)],
    "dot": [(100, 275, 200, 375)],
    # the tail and the stroke stay inside the letter's 0..250 box: sticking out would eat the 50 units that keep
    # letters apart, and at 6 px that is the whole gap (the user saw letters running into each other)
    "ogonek": [(125, -50, 175, 0), (150, -100, 250, -50)],
    "stroke": [(50, 75, 150, 125), (100, 125, 200, 175)],
}
POLISH = {
    "Ą": ("A", "ogonek"), "Ć": ("C", "acute"), "Ę": ("E", "ogonek"), "Ł": ("L", "stroke"), "Ń": ("N", "acute"),
    "Ó": ("O", "acute"), "Ś": ("S", "acute"), "Ź": ("Z", "acute"), "Ż": ("Z", "dot"),
    "ą": ("a", "ogonek"), "ć": ("c", "acute"), "ę": ("e", "ogonek"), "ł": ("l", "stroke"), "ń": ("n", "acute"),
    "ó": ("o", "acute"), "ś": ("s", "acute"), "ź": ("z", "acute"), "ż": ("z", "dot"),
}
# punctuation: (ink width, rectangles from x = 0). A letter's ink ends 50 units before its advance, so punctuation is
# drawn 50 units in from the pen and its advance leaves 50 after it: both neighbours then keep the letters' own gap
# even at 6 px, where a letter's 50 units are the single pixel that separates the glyphs.
PUNCTUATION = {
    ".": (100, [(0, 0, 100, 100)]),
    ",": (100, [(0, 0, 100, 100), (0, -50, 50, 0)]),
    ":": (100, [(0, 0, 100, 100), (0, 150, 100, 250)]),
    "'": (100, [(0, 150, 100, 250)]),
    "!": (100, [(0, 0, 100, 50), (0, 100, 100, 250)]),
    "-": (200, [(0, 100, 200, 150)]),
    "?": (250, [(25, 200, 225, 250), (0, 150, 100, 200), (150, 150, 250, 200), (100, 100, 225, 150), (75, 0, 175, 50)]),
    "/": (300, [(0, 0, 100, 50), (50, 50, 150, 100), (100, 100, 200, 150), (150, 150, 250, 200), (200, 200, 300, 250)]),
    "%": (300, [(0, 150, 100, 250), (200, 0, 300, 100),
                (0, 0, 75, 50), (50, 50, 125, 100), (112, 100, 188, 150), (175, 150, 250, 200), (225, 200, 300, 250)]),
}
PUNCTUATION_BEARING = 50  # the ink's offset from the pen, and the gap left after it
PRIVATE = 0xE000  # accents get private code points in the in-memory font


def augmented_font(ttf):
    """retro.ttf with the accents and the punctuation added as glyphs, as bytes (nothing is written to disk)."""
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib import TTFont

    font = TTFont(ttf)
    # per-glyph device tables would need entries for the new glyphs; advances are whole pixels without them
    for tag in ("hdmx", "LTSH", "VDMX"):
        if tag in font:
            del font[tag]
    glyf, hmtx = font["glyf"], font["hmtx"]
    cmaps = [t for t in font["cmap"].tables if t.isUnicode()]

    def add(name, codepoint, advance, rects):
        pen = TTGlyphPen(None)
        for x0, y0, x1, y1 in rects:  # clockwise, as the font's outer contours
            pen.moveTo((x0, y0))
            pen.lineTo((x0, y1))
            pen.lineTo((x1, y1))
            pen.lineTo((x1, y0))
            pen.closePath()
        glyph = pen.glyph()
        order = font.getGlyphOrder()
        order.append(name)
        font.setGlyphOrder(order)
        glyf[name] = glyph
        glyph.recalcBounds(glyf)
        hmtx[name] = (advance, glyph.xMin)
        for t in cmaps:
            t.cmap[codepoint] = name

    for i, (name, rects) in enumerate(ACCENTS.items()):
        add("cr_" + name, PRIVATE + i, 300, rects)
    for ch, (width, rects) in PUNCTUATION.items():
        b = PUNCTUATION_BEARING
        add("cr_u%04x" % ord(ch), ord(ch), width + 2 * b, [(x0 + b, y0, x1 + b, y1) for x0, y0, x1, y1 in rects])
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


def ink(font, text, size):
    """(image with the text drawn at pen (size, size), the font's box of the text relative to the pen or None)"""
    img = Image.new("L", (size * 5, size * 3), 0)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((size, size), text, font=font, fill=255)
    return img, font.getbbox(text, mode="1") if img.getbbox() else None


def bake(ttf, size, dst):
    font = ImageFont.truetype(io.BytesIO(augmented_font(ttf)), size)
    ascent, descent = font.getmetrics()
    # characters the font lacks render as its .notdef box; skip them
    notdef = bytes(font.getmask("", mode="1"))
    accent_chars = {name: chr(PRIVATE + i) for i, name in enumerate(ACCENTS)}
    glyphs = []
    for ch in CHARS + list(PUNCTUATION) + list(POLISH):
        # the font's zero has a bar across its middle and reads as an 8 on small screens (user report, both ports):
        # the digit is drawn with the letter O's shape, which has the same box and advance
        shape = "O" if ch == "0" else POLISH[ch][0] if ch in POLISH else ch
        if ch != " " and bytes(font.getmask(shape, mode="1")) == notdef:
            continue
        advance = int(round(font.getlength(shape)))
        img, box = ink(font, shape, size)
        if ch in POLISH:
            mark, mark_box = ink(font, accent_chars[POLISH[ch][1]], size)
            img = ImageChops.lighter(img, mark)
            box = (min(box[0], mark_box[0]), min(box[1], mark_box[1]), max(box[2], mark_box[2]), max(box[3], mark_box[3]))
        if box is None:
            glyphs.append((ch, None, 0, 0, advance))
            continue
        # the font's box (the same records as before O11.6 for the font's own characters, blank columns included)
        l, t, r, b = box
        # O11.9: the font's own "1" draws wider than its advance and hangs left of the pen, so it ran into both its
        # neighbours ("V016" read as one blob on the console). Every glyph now keeps the gap the font's letters keep
        # (50 design units of 300 per em), measured on the ink itself - checked by out/check/font/gaps.py.
        inkBox = img.getbbox()
        if inkBox:
            gap = max(1, int(round(size / 6.0)))
            shift = max(0, size - inkBox[0])  # ink starting left of the pen
            if shift:
                img = img.transform(img.size, Image.AFFINE, (1, 0, -shift, 0, 1, 0))
                inkBox = img.getbbox()
            advance = max(advance, inkBox[2] - size + gap)
            r = max(r, inkBox[2] - size)
            l = min(l, inkBox[0] - size)
        glyphs.append((ch, img.crop((l + size, t + size, r + size, b + size)), l, t, advance))

    pad = 1
    atlas_w = 256 if size <= 16 else 512
    x = y = row_h = 0
    placed = []
    for ch, img, xoff, yoff, adv in glyphs:
        if img is None:
            placed.append((ch, 0, 0, 0, 0, 0, 0, adv))
            continue
        w, h = img.size
        if x + w + pad > atlas_w:
            x, y, row_h = 0, y + row_h + pad, 0
        placed.append((ch, x, y, w, h, xoff, yoff, adv, img))
        x += w + pad
        row_h = max(row_h, h)
    atlas_h = 1
    while atlas_h < y + row_h + pad:
        atlas_h *= 2
    atlas = Image.new("L", (atlas_w, atlas_h), 0)
    for g in placed:
        if len(g) == 9:
            atlas.paste(g[8], (g[1], g[2]))

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(b"CRF1")
        f.write(struct.pack("<HHHHH", atlas_w, atlas_h, size, ascent + descent, len(placed)))
        for g in placed:
            ch, gx, gy, w, h, xoff, yoff, adv = g[:8]
            f.write(struct.pack("<HHHHHhhH", ord(ch), gx, gy, w, h, xoff, yoff, adv))
        f.write(atlas.tobytes())
    return atlas_w, atlas_h, ascent + descent


def main(argv):
    sizes = SIZES
    if "--sizes" in argv:
        at = argv.index("--sizes")
        sizes = tuple(int(s) for s in argv[at + 1].split(","))
        argv = argv[:at] + argv[at + 2:]
    if len(argv) == 3 and argv[0] == "--all":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from assets_manifest import FONT
        for size in sizes:
            w, h, lh = bake(os.path.join(argv[1], FONT), size, os.path.join(argv[2], "fonts", f"retro_{size}.fnt"))
            print(f"font retro_{size}: atlas {w}x{h}, line height {lh}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
