#!/bin/sh
# O4.1: a hop started while an earlier hop's animations still run (a quick second press, or right after a hop blocked
# by a tree). In the original the earlier timeline's end clears `moving` in the middle of the new hop; a water row
# can then attach the hero to a log it only flies over, and the hero drifts with that log on the next row (user
# report). Random quick hops over many seeds through the logic trace (out/pc/trace.exe), counting
#   - hops that end mid-flight: moving 1 -> 0 while the hero's z still changes over 3 still steps
#   - rides held on another row: a ride that started on one row still held, standing, on another
# The game (--game) must have none; the original's behaviour (the trace default) must still show them.
#   sh build/hop_check.sh      (needs out/pc/trace.exe: sh build/build_pc.sh trace)
cd "$(dirname "$0")/.." || exit 1
mkdir -p out/check/hop_check
python - <<'EOF'
import random, re, subprocess, sys
exe = "./out/pc/trace.exe"
pat = re.compile(r"t=(\d+) state=(\w+) alive=(\d) moving=(\d) riding=(\d) hit=(\d) hero=([-\d.]+),([-\d.]+),([-\d.]+)")
result = {}
for name, mode in (("game", ["--game"]), ("original", [])):
    rr = random.Random(11)
    early_runs = ghost_runs = 0
    for seed in range(1, 121):
        toks = ["w20", "s"]
        for k in range(60):
            toks += ["w%d" % rr.choice([8, 12, 20]), rr.choice(["u", "u", "u", "l", "r"])]
            if rr.random() < 0.7:
                toks += ["w%d" % rr.choice([2, 3, 4, 5, 6, 7]), rr.choice(["u", "u", "l", "r"])]
        out = "out/check/hop_check/trace.txt"
        subprocess.run([exe, "--seed", str(seed), "--steps", "1500", "--script", " ".join(toks), "--out", out] + mode,
                       capture_output=True)
        rows = []
        for line in open(out):
            m = pat.match(line)
            if m:
                t, st, alive, moving, riding, hit, x, y, z = m.groups()
                rows.append((int(t), st, alive == "1", moving == "1", riding == "1", float(z)))
        early = ghost = False
        ride_z = None
        for i in range(1, len(rows) - 3):
            t, st, alive, moving, riding, z = rows[i]
            prev = rows[i - 1]
            if st != "playing" or not alive:
                ride_z = None
                continue
            still = all(not rows[i + k][3] for k in (1, 2, 3))
            if prev[3] and not moving and still and abs(rows[i + 3][5] - z) > 0.05:
                early = True
            if riding and not prev[4]:
                ride_z = round(z)
            if not riding:
                ride_z = None
            if riding and not moving and still and ride_z is not None and round(z) != ride_z:
                ghost = True
        early_runs += early
        ghost_runs += ghost
    result[name] = (early_runs, ghost_runs)
    print(f"  {name} behaviour, 120 runs: hops ended mid-flight in {early_runs}, rides held on another row in {ghost_runs}")
fail = result["game"] != (0, 0) or result["original"][0] == 0
print("hop_check: " + ("FAILED" if fail else "OK"))
sys.exit(1 if fail else 0)
EOF
