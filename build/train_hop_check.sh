#!/bin/sh
# O10: a hop through a passing train (user report after v014: "sometimes it kills me, sometimes not"). The original checks
# the train against a standing hero only, so a hero in the air over the track survives a train that covers it.
# Seeds with a railroad at row 10 and grass at row 11 in both behaviours; the hero walks to row 9, waits W steps (one whole
# train cycle in steps of 6) and hops twice (2 or 5 steps apart) over the track. The logic trace (out/pc/trace.exe --rails
# prints the train of the hero's railroad) gives, per run, the steps where the living, playing hero is in the railroad's
# row (by jsRound, as the check does) inside the train's collision box, and the heroes the train killed:
#   - the game (--game): no step alive inside a train, and some heroes killed (the runs do meet the train)
#   - the original's behaviour (the trace default): heroes alive inside a train (the check sees the bug)
#   sh build/train_hop_check.sh      (needs out/pc/trace.exe: sh build/build_pc.sh trace)
cd "$(dirname "$0")/.." || exit 1
mkdir -p out/check/train_hop
python - <<'EOF'
import re, subprocess, sys
exe = "./out/pc/trace.exe"
pat = re.compile(r"t=(\d+) state=(\w+) alive=(\d) moving=(\d) riding=(\d) hit=(\d) hero=([-\d.]+),([-\d.]+),([-\d.]+).*snd=(\S+) train=(\S+)")
result = {}
for name, mode in (("game", ["--game"]), ("original", [])):
    runs = survived_runs = killed_runs = 0
    for seed in (7, 30, 36, 42):
        for wait in range(0, 280, 6):
            for gap in (2, 5):
                script = f"w20 s w30 u w30 w{wait + 1} u w{gap} u w120"
                out = "out/check/train_hop/trace.txt"
                subprocess.run([exe, "--seed", str(seed), "--steps", str(260 + wait), "--script", script, "--out", out,
                                "--rails"] + mode, capture_output=True)
                prev_alive, prev_inside = True, False
                survived = killed = False
                for line in open(out):
                    m = pat.match(line)
                    if not m:
                        continue
                    t, st, alive, moving, riding, hit, x, y, z, snd, train = m.groups()
                    alive = alive == "1"
                    inside = False
                    if train != "-" and st == "playing":
                        tx, box = (float(v) for v in train.split(","))
                        inside = abs(float(x) - tx) < box - 0.002
                    if inside and alive:
                        survived = True
                    # a hero hit in the air is pushed half a row off the track (its line shows no train) and plays
                    # train_die; one crushed standing on the track stays in the railroad's row
                    if prev_alive and not alive and (inside or prev_inside or train != "-" or "train_die" in snd):
                        killed = True
                    prev_alive, prev_inside = alive, inside
                runs += 1
                survived_runs += survived
                killed_runs += killed
    result[name] = (survived_runs, killed_runs)
    print(f"  {name} behaviour, {runs} hops over a railroad: alive inside the train in {survived_runs}, killed on the track in {killed_runs}")
fail = result["game"][0] != 0 or result["game"][1] == 0 or result["original"][0] == 0
print("train_hop_check: " + ("FAILED" if fail else "OK"))
sys.exit(1 if fail else 0)
EOF
