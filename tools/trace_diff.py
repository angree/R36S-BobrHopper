#!/usr/bin/env python3
"""First real divergence between a reference and a port trace (numbers compared with a tolerance).
Usage: python tools/trace_diff.py <scenario> [--tol 0.01] [--context 4]
"""
import sys


def same(a, b, tol):
    ta, tb = a.split(), b.split()
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        ka, _, va = x.partition("=")
        kb, _, vb = y.partition("=")
        if ka != kb:
            return False
        pa, pb = va.split(","), vb.split(",")
        if len(pa) != len(pb):
            return False
        for u, v in zip(pa, pb):
            try:
                if abs(float(u) - float(v)) > tol:
                    return False
            except ValueError:
                if u != v:
                    return False
    return True


def main(argv):
    name = argv[0]
    tol = float(argv[argv.index("--tol") + 1]) if "--tol" in argv else 0.01
    ctx = int(argv[argv.index("--context") + 1]) if "--context" in argv else 4
    ref = open(f"out/trace_ref/{name}.txt").read().splitlines()
    port = open(f"out/trace_port/{name}.txt").read().splitlines()
    diffs = [i for i, (a, b) in enumerate(zip(ref, port)) if not same(a, b, tol)]
    print(f"{name}: {len(diffs)} differing lines" + (f", first at line {diffs[0]}" if diffs else ""))
    if not diffs:
        return 0
    first = diffs[0]
    for k in range(max(0, first - ctx), min(first + ctx + 1, len(ref))):
        mark = "*" if k in diffs else " "
        print(f"{mark}R {ref[k]}")
        print(f"{mark}P {port[k]}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
