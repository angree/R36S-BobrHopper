#!/usr/bin/env python3
"""F6: compares traces of the approximate 16.16 port (SF2000) with the original's traces with tolerances instead of
bit-exactness.

For every scenario in both directories it reports:
  - events: the ticks where state, score, rows, alive, moving, riding, hit and the sound list change, matched in order
    between the two traces; an event may happen up to --ticks ticks earlier or later (default 2),
  - positions: the largest deviation of hero / world / scene values (units), and the tick of the first deviation above
    --pos (default 0.05).
Exit code 1 when an event is missing, out of order or shifted beyond --ticks.

    python tools/trace_tolerance.py out/trace_ref out/trace_fixed [--ticks 2] [--pos 0.05] [--only name]
"""
import argparse
import os
import re
import sys

EVENT_KEYS = ["state", "score", "rows", "alive", "moving", "riding", "hit", "snd"]
NUM_KEYS = ["hero", "rot", "scale", "world", "scene"]


def parse(path):
    ticks = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("t="):
                continue
            fields = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
            ticks.append(fields)
    return ticks


def events(ticks):
    out = []
    prev = {}
    for i, t in enumerate(ticks):
        for k in EVENT_KEYS:
            v = t.get(k)
            if k == "snd":
                if v and v != "-":
                    out.append((i + 1, k, v))
                continue
            if k in prev and prev[k] != v:
                out.append((i + 1, k, v))
            prev[k] = v
    return out


def numbers(t, key):
    v = t.get(key, "")
    try:
        return [float(x) for x in v.split(",")]
    except ValueError:
        return []


def compare(ref_path, fix_path, max_shift, pos_tol):
    ref, fix = parse(ref_path), parse(fix_path)
    n = min(len(ref), len(fix))
    problems = []
    # events matched in order per key
    shifts = []
    for key in EVENT_KEYS:
        er = [e for e in events(ref) if e[1] == key]
        ef = [e for e in events(fix) if e[1] == key]
        j = 0
        for (tick, _, value) in er:
            if tick > n:
                break
            match = None
            while j < len(ef) and ef[j][0] <= tick + max_shift:
                if ef[j][2] == value and abs(ef[j][0] - tick) <= max_shift:
                    match = ef[j]
                    j += 1
                    break
                j += 1
            if match is None:
                problems.append("event %s=%s at t=%d not matched within +-%d" % (key, value, tick, max_shift))
            else:
                shifts.append(match[0] - tick)
    worst = 0.0
    first_off = None
    for i in range(n):
        for key in NUM_KEYS:
            a, b = numbers(ref[i], key), numbers(fix[i], key)
            for x, y in zip(a, b):
                d = abs(x - y)
                if d > worst:
                    worst = d
                if d > pos_tol and first_off is None:
                    first_off = (i + 1, key, x, y)
    return n, shifts, worst, first_off, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("fixed")
    ap.add_argument("--ticks", type=int, default=2)
    ap.add_argument("--pos", type=float, default=0.05)
    ap.add_argument("--only")
    args = ap.parse_args()
    names = sorted(f for f in os.listdir(args.fixed) if f.endswith(".txt") and os.path.exists(os.path.join(args.ref, f)))
    if args.only:
        names = [f for f in names if f.startswith(args.only)]
    failed = 0
    for name in names:
        n, shifts, worst, first_off, problems = compare(os.path.join(args.ref, name), os.path.join(args.fixed, name),
                                                        args.ticks, args.pos)
        exact = sum(1 for s in shifts if s == 0)
        shifted = [s for s in shifts if s != 0]
        line = "%-18s ticks=%d events=%d exact=%d shifted=%s worst_value_diff=%.4f" % (
            name, n, len(shifts), exact, ("%d (%+d..%+d)" % (len(shifted), min(shifted), max(shifted))) if shifted else "0",
            worst)
        if first_off:
            line += " first>%.2f at t=%d %s %.3f vs %.3f" % (args.pos, first_off[0], first_off[1], first_off[2], first_off[3])
        print(("FAIL " if problems else "ok   ") + line)
        for p in problems[:5]:
            print("       " + p)
        failed += 1 if problems else 0
    print("trace_tolerance: %d scenarios, %d with unmatched events" % (len(names), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
