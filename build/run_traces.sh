#!/bin/sh
# Runs the scenarios of build/trace_scenarios.txt through the original (tools/webref/trace.mjs) and the
# port (out/pc/trace.exe), then compares the per-tick traces. The original runs first: its trace header
# says how many ticks it ran before the animation-frame loop, and the port replays exactly that many.
# Usage: sh build/run_traces.sh [--port-only | --ref-only] [scenario-name]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MODE=both
ONLY=""
for a in "$@"; do
  case "$a" in
    --port-only) MODE=port ;;
    --ref-only) MODE=ref ;;
    *) ONLY="$a" ;;
  esac
done

mkdir -p out/trace_ref out/trace_port
grep -v '^#' build/trace_scenarios.txt | while IFS='|' read -r name seed steps script; do
  [ -z "$name" ] && continue
  [ -n "$ONLY" ] && [ "$ONLY" != "$name" ] && continue
  if [ "$MODE" != port ]; then
    node tools/webref/trace.mjs --seed "$seed" --steps "$steps" --script "$script" --out "out/trace_ref/$name.txt"
  fi
  if [ "$MODE" != ref ]; then
    pre=$(sed -n 's/^pre_ticks //p' "out/trace_ref/$name.txt" 2>/dev/null)
    ./out/pc/trace.exe --seed "$seed" --steps "$steps" --pre "${pre:-0}" --script "$script" \
      --out "out/trace_port/$name.txt" >/dev/null
  fi
done
if [ "$MODE" = both ] || [ "$MODE" = port ]; then
  if [ -n "$ONLY" ]; then
    mkdir -p out/trace_cmp_ref out/trace_cmp_port
    rm -f out/trace_cmp_ref/*.txt out/trace_cmp_port/*.txt
    cp "out/trace_ref/$ONLY.txt" out/trace_cmp_ref/ 2>/dev/null
    cp "out/trace_port/$ONLY.txt" out/trace_cmp_port/
    python tools/compare_dumps.py out/trace_cmp_ref out/trace_cmp_port --tol 0.01
  else
    python tools/compare_dumps.py out/trace_ref out/trace_port --tol 0.01
  fi
fi
