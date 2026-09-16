#!/usr/bin/env python3
"""Composes the frames of build/framing_sheet.sh into one labelled sheet per moment.

  python tools/framing_sheet.py out/framing

Reads out/framing/metrics.txt ("framing <shot> scale=.. shift=.. heroY=.. heroPx=.. ahead=.. behind=..
laneHero=.. laneTop=..") and the matching <shot>.png files. Rows of the sheet are view scales, columns are
shifts; every tile is a half-size frame with its numbers underneath, and a thin line marks the lower 35% of the
screen (where the plan wants the hero). Also prints the table.
"""
import os
import re
import sys

from PIL import Image, ImageDraw

LINE = re.compile(r"^framing (\S+) scale=(\S+) shift=(\S+) heroY=(\S+) heroPx=(\d+) ahead=(\d+) behind=(\d+) "
                  r"laneHero=(\d) laneTop=(\d)")


def main(d):
    shots = []
    with open(os.path.join(d, "metrics.txt"), encoding="utf-8") as f:
        for line in f:
            m = LINE.match(line.strip())
            if not m:
                continue
            name, scale, shift, hero_y, hero_px, ahead, behind, lane_hero, lane_top = m.groups()
            # scenario names contain "_s<seed>": the moment is everything before "_s<scale>_h<shift>_t<step>"
            moment = re.sub(r"_s-?[0-9.]+_h-?[0-9.]+_t\d+$", "", name)
            shots.append(dict(name=name, moment=moment, scale=float(scale), shift=float(shift),
                              hero_y=float(hero_y), hero_px=int(hero_px), ahead=int(ahead), behind=int(behind),
                              lane_hero=lane_hero == "1", lane_top=lane_top == "1"))
    if not shots:
        print("no framing metrics")
        return 1

    tile_w, tile_h, label_h = 320, 240, 34
    for moment in sorted({s["moment"] for s in shots}):
        group = [s for s in shots if s["moment"] == moment]
        scales = sorted({s["scale"] for s in group})
        shifts = sorted({s["shift"] for s in group})
        sheet = Image.new("RGB", (len(shifts) * tile_w, len(scales) * (tile_h + label_h)), (20, 20, 20))
        draw = ImageDraw.Draw(sheet)
        print(f"\n{moment}")
        print("  scale shift  heroY heroPx ahead behind laneHero laneTop")
        for s in sorted(group, key=lambda s: (s["scale"], s["shift"])):
            col, row = shifts.index(s["shift"]), scales.index(s["scale"])
            x, y = col * tile_w, row * (tile_h + label_h)
            path = os.path.join(d, s["name"] + ".png")
            if os.path.exists(path):
                img = Image.open(path).convert("RGB").resize((tile_w, tile_h), Image.BILINEAR)
                sheet.paste(img, (x, y))
            line_y = y + int(tile_h * 0.65)
            draw.line([(x, line_y), (x + tile_w - 1, line_y)], fill=(255, 0, 255), width=1)
            ok = s["lane_hero"] and s["lane_top"] and s["hero_y"] >= 0.65
            draw.text((x + 4, y + tile_h + 2),
                      f"scale {s['scale']:g} shift {s['shift']:g}  hero {s['hero_y'] * 100:.0f}% {s['hero_px']}px",
                      fill=(255, 255, 255))
            draw.text((x + 4, y + tile_h + 17),
                      f"ahead {s['ahead']} behind {s['behind']} lane {'Y' if s['lane_hero'] else 'n'}/"
                      f"{'Y' if s['lane_top'] else 'n'}{'  <- meets plan' if ok else ''}",
                      fill=(120, 255, 120) if ok else (255, 200, 120))
            print(f"  {s['scale']:5g} {s['shift']:5g} {s['hero_y']:6.2f} {s['hero_px']:6d} {s['ahead']:5d} "
                  f"{s['behind']:6d} {str(s['lane_hero']):>8} {str(s['lane_top']):>7}")
        out = os.path.join(d, f"sheet_{moment}.png")
        sheet.save(out)
        print(f"  -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "out/framing"))
