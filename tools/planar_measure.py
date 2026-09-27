"""Planar budget for the Amiga sprite sets: what the baked sprites would cost as blitter BOBs in chip RAM.

Reads data_amiga/sprites*.spr (format in src/amiga/sprites.h) and prints, per sprite family and in total:
colours used, pixel area, and the chip bytes for several storage schemes.
"""
import struct, sys, collections, re

IDS = open("src/amiga/sprite_ids.h").read()
names = {}
for m in re.finditer(r"#define SPR_(\w+)\s+(\d+)", IDS):
    names[int(m.group(2))] = m.group(1)

def family(n):
    n = re.sub(r"_R\d+_S\d+$", "", n)
    n = re.sub(r"_R\d+$", "", n)
    return n

def load(path):
    d = open(path, "rb").read()
    count, pal_used = struct.unpack(">HH", d[6:10])
    ents = []
    for i in range(count):
        w, h, ax, ay, off = struct.unpack(">HHhhI", d[780 + 12 * i: 792 + 12 * i])
        px = d[off: off + w * h]
        ents.append((w, h, px))
    return ents, pal_used

def words(w):
    return (w + 15) // 16

def analyse(path):
    ents, pal_used = load(path)
    fam = collections.OrderedDict()
    tot = collections.Counter()
    allcols = set()
    for i, (w, h, px) in enumerate(ents):
        f = family(names.get(i, "?"))
        cols = set(px) - {0}
        allcols |= cols
        # tight bounding box (the baker leaves margins)
        rows = [r for r in range(h) if any(px[r * w:(r + 1) * w])]
        colsx = [c for c in range(w) if any(px[r * w + c] for r in range(h))]
        if rows:
            bh = rows[-1] - rows[0] + 1
            bw = colsx[-1] - colsx[0] + 1
        else:
            bh = bw = 0
        opaque = sum(1 for p in px if p)
        W = words(bw) + 1  # one extra word so the blitter can shift
        plane = W * 2 * bh
        rec = dict(w=bw, h=bh, opaque=opaque, cols=len(cols),
                   p8=plane * 9,           # 8 planes + 1 mask (non-interleaved mask, per-plane blits)
                   p8i=plane * 16,         # interleaved: mask repeated for each plane
                   p5=plane * 6,           # 5 planes (32 colours) + mask
                   p4=plane * 5,           # 4 planes (16 colours, bank bits via mask) + mask
                   p3=plane * 4,
                   chunky=w * h)
        g = fam.setdefault(f, collections.Counter())
        g["n"] += 1
        g["maxcols"] = max(g["maxcols"], len(cols))
        g.setdefault("colset", 0)
        for k, v in rec.items():
            if k == "cols":
                continue
            g[k] += v
            tot[k] += v
        g["maxw"] = max(g["maxw"], bw)
        g["maxh"] = max(g["maxh"], bh)
        fam[f] = g
        fam[f]["_colors"] = fam[f].get("_colors", 0)
    # colours per family (union)
    famcols = collections.defaultdict(set)
    for i, (w, h, px) in enumerate(ents):
        famcols[family(names.get(i, "?"))] |= set(px) - {0}
    print("== %s: %d sprites, palette %d used, %d distinct in pixels" % (path, len(ents), pal_used, len(allcols)))
    print("%-24s %4s %5s %5s %6s %7s %8s %8s %8s %8s" % ("family", "n", "maxW", "maxH", "cols", "chunky", "8pl+m", "5pl+m", "4pl+m", "3pl+m"))
    for f, g in fam.items():
        print("%-24s %4d %5d %5d %6d %7d %8d %8d %8d %8d" % (f, g["n"], g["maxw"], g["maxh"], len(famcols[f]), g["chunky"], g["p8"], g["p5"], g["p4"], g["p3"]))
    print("%-24s %4d %5s %5s %6d %7d %8d %8d %8d %8d" % ("TOTAL", len(ents), "", "", len(allcols), tot["chunky"], tot["p8"], tot["p5"], tot["p4"], tot["p3"]))
    print("interleaved 8pl (mask per plane): %d" % tot["p8i"])
    return fam, famcols

for p in sys.argv[1:]:
    analyse(p)
