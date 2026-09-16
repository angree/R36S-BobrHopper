#!/usr/bin/env python3
"""Compares two dump directories line by line: words must match exactly, numbers within a tolerance.
Usage: python tools/compare_dumps.py out/mapdump_ref out/mapdump_port [--tol 0.002]
"""
import glob
import os
import re
import sys

NUM = re.compile(r"^-?\d+(\.\d+)?$")


def tokens(line):
    out = []
    for tok in line.replace(",", " , ").split():
        key, _, val = tok.partition("=")
        out.append((key, val) if _ else (None, tok))
    return out


def same(a, b, tol):
    if NUM.match(a) and NUM.match(b):
        return abs(float(a) - float(b)) <= tol
    return a == b


def main(argv):
    tol = 0.002
    if "--tol" in argv:
        tol = float(argv[argv.index("--tol") + 1])
    ref_dir, port_dir = argv[0], argv[1]
    failures = 0
    files = sorted(glob.glob(os.path.join(ref_dir, "*.txt")))
    if not files:
        print("no reference dumps")
        return 2
    for ref in files:
        port = os.path.join(port_dir, os.path.basename(ref))
        if not os.path.exists(port):
            print(f"MISSING {port}")
            failures += 1
            continue
        a = open(ref).read().splitlines()
        b = open(port).read().splitlines()
        bad = 0
        for i in range(max(len(a), len(b))):
            la = a[i] if i < len(a) else "<eof>"
            lb = b[i] if i < len(b) else "<eof>"
            ta, tb = tokens(la), tokens(lb)
            ok = len(ta) == len(tb) and all(ka == kb and same(va, vb, tol) for (ka, va), (kb, vb) in zip(ta, tb))
            if not ok:
                if bad < 5:
                    print(f"{os.path.basename(ref)}:{i + 1}\n  ref : {la}\n  port: {lb}")
                bad += 1
        print(f"{os.path.basename(ref)}: {len(a)} lines, {bad} differ")
        failures += bad
    print("IDENTICAL" if failures == 0 else f"FAILED: {failures} differing lines")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
