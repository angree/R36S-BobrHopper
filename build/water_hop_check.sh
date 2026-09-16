#!/bin/sh
# O12: crossing a river in a chain of hops (user report after v017: "da sie tez szybko przez wode przeskoczyc - tak
# samo jak wczesniej przez pociag"). WaterRow::update drowns a hero that stands still only. In the original the
# previous hop's animations clear `moving` in the middle of the next hop, so the river still catches the hero; the
# game stops those animations (O4.1), which let a fast pair of hops carry the hero over a whole water row.
# The hero walks to the row before a water row, then hops twice `gap` steps apart: water row -> far bank. Per run the
# trace (--water prints, for the hero's row, whether it is water and whether a log or a lily pad is under the hero)
# tells whether the hero:
#   - reached the far bank alive without one step of support over the water -> the bug,
#   - drowned -> the fix works.
# The game must show no unsupported crossing; the original drowns the hero for its own reason, so only the game's
# numbers decide (before the fix the game crossed 144 of 360 runs, docs/PROGRESS.md).
#   sh build/water_hop_check.sh [trace.exe]   (default out/pc/trace.exe: sh build/build_pc.sh trace;
#                                              out/sf2000/host/trace_fixed.exe checks the SF2000's 16.16 logic)
cd "$(dirname "$0")/.." || exit 1
mkdir -p out/check/water_hop
CR_TRACE=${1:-./out/pc/trace.exe} python - <<'EOF'
import os, re, subprocess, sys

exe = os.environ.get("CR_TRACE", "./out/pc/trace.exe")
out = "out/check/water_hop/trace.txt"
rows_re = re.compile(r"^rows (.*)$")
line_re = re.compile(r"t=(\d+) state=(\w+) alive=(\d) moving=(\d) riding=(\d) hit=(\d) hero=([-\d.]+),([-\d.]+),"
                     r"([-\d.]+).*snd=(\S+) water=(\d),(\d)")

def row_types(seed, mode):
    subprocess.run([exe, "--seed", str(seed), "--steps", "1", "--script", "w1", "--out", out, "--row-types"] + mode,
                   capture_output=True)
    for line in open(out):
        m = rows_re.match(line)
        if m:
            return {int(p.split(":")[0]): p.split(":")[1] for p in m.group(1).split()}
    return {}

# seeds whose row 10 is water and row 11 is grass: the hero hops 9 -> 10 -> 11 across the river
def seeds_for(mode, want=6):
    found = []
    for seed in range(1, 200):
        rows = row_types(seed, mode)
        if rows.get(10) == "water" and rows.get(11) == "grass":
            found.append(seed)
            if len(found) == want:
                break
    return found

result = {}
for name, mode in (("game", ["--game"]), ("original", [])):
    seeds = seeds_for(mode)
    runs = flew_over = drowned = supported = 0
    for seed in seeds:
        for wait in range(0, 60, 5):
            for gap in (2, 4, 6, 8, 10):
                # start (row 8) -> hop to 9 and settle, then the fast pair 9 -> 10 (water) -> 11 (far bank)
                script = f"w20 s w30 u w30 w{wait + 1} u w{gap} u w120"
                subprocess.run([exe, "--seed", str(seed), "--steps", str(200 + wait), "--script", script, "--out", out,
                                "--water"] + mode, capture_output=True)
                had_support = crossed_alive = drown = False
                for line in open(out):
                    m = line_re.match(line)
                    if not m:
                        continue
                    t, st, alive, moving, riding, hit, x, y, z, snd, is_water, under = m.groups()
                    alive, z = alive == "1", float(z)
                    if is_water == "1" and under == "1":
                        had_support = True
                    if "water" in snd:
                        drown = True
                    if alive and st == "playing" and moving == "0" and z > 10.9:
                        crossed_alive = True
                runs += 1
                if crossed_alive and not had_support:
                    flew_over += 1
                if crossed_alive and had_support:
                    supported += 1
                if drown:
                    drowned += 1
    result[name] = (len(seeds), runs, flew_over, drowned, supported)
    print(f"  {os.path.basename(exe)} {name} behaviour, seeds {seeds}, {runs} double hops over water: "
          f"crossed with no support in {flew_over}, crossed on a lily pad or log in {supported}, drowned in {drowned}")

fail = result["game"][2] != 0 or result["game"][3] == 0
print("water_hop_check: " + ("FAILED" if fail else "OK"))
sys.exit(1 if fail else 0)
EOF
