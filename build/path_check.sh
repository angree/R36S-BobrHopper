#!/bin/sh
# O6.1: a way forward through every generated row. trace --path-check builds the map of 1000 seeds, 300 rows each from
# the starting row, and walks it forward with the game's own collision tests (trees, lily pads) - independently of
# GameMap's RowRef::reach:
#   - the game (GameContext::originalBehaviour = false): no row may close the way, PC double and SF2000 16.16 logic
#   - the original's map still closes it (the check can see the problem the user reported)
#   sh build/path_check.sh      (needs out/pc/trace.exe and out/sf2000/host/trace_fixed.exe:
#                                sh build/build_pc.sh trace; sh build/build_host.sh pc-trace)
cd "$(dirname "$0")/.." || exit 1
D=out/check/path
mkdir -p "$D"
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

for exe in out/pc/trace.exe out/sf2000/host/trace_fixed.exe; do
  name=$(basename "$exe" .exe)
  "$exe" --path-check 1000 --steps 300 --seed 1 --game > "$D/${name}_game.txt" 2>/dev/null
  "$exe" --path-check 1000 --steps 300 --seed 1 > "$D/${name}_original.txt" 2>/dev/null
  game=$(tail -1 "$D/${name}_game.txt")
  orig=$(tail -1 "$D/${name}_original.txt")
  echo "$game" | grep -q 'rows=300000 blocked=0 ' && ok "$name game: $game" || bad "$name game: $game"
  echo "$orig" | grep -q 'blocked=[1-9]' && ok "$name original: $orig" || bad "$name original map lost its closed rows: $orig"
  # O23: in the two-player mode every row must offer TWO ways through, so neither player waits for the other's square
  "$exe" --path-check 1000 --steps 300 --seed 1 --game --two-paths > "$D/${name}_two.txt" 2>/dev/null
  two=$(tail -1 "$D/${name}_two.txt")
  echo "$two" | grep -q 'rows=300000 blocked=0 ' && ok "$name two players: $two" || bad "$name two players: $two"
done

[ $fail -eq 0 ] && echo "path_check: OK" || { echo "path_check: FAILED"; exit 1; }
