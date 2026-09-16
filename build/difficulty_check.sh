#!/bin/sh
# O8: the easier start (game/difficulty.h) on the generated rows. trace --difficulty-check builds the map of 1000 seeds
# (~307 rows each) and checks the limits from the rows' own content - dangerous rows in a row (roads, railroads, water
# with moving logs), railroads per 10 rows, car and log speeds against the open baskets:
#   - the game (GameContext::originalBehaviour = false): no violation, PC double and SF2000 16.16 logic
#   - the original's map breaks them (the check can see what the limits change)
#   sh build/difficulty_check.sh   (needs out/pc/trace.exe and out/sf2000/host/trace_fixed.exe:
#                                   sh build/build_pc.sh trace; sh build/build_host.sh pc-trace)
cd "$(dirname "$0")/.." || exit 1
D=out/check/difficulty
mkdir -p "$D"
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

for exe in out/pc/trace.exe out/sf2000/host/trace_fixed.exe; do
  name=$(basename "$exe" .exe)
  "$exe" --difficulty-check 1000 --steps 300 --seed 1 --game > "$D/${name}_game.txt" 2>/dev/null
  "$exe" --difficulty-check 1000 --steps 300 --seed 1 > "$D/${name}_original.txt" 2>/dev/null
  game=$(tail -1 "$D/${name}_game.txt")
  orig=$(tail -1 "$D/${name}_original.txt")
  echo "$game" | grep -q ' violations=0 ' && ok "$name game: $game" || bad "$name game: $game"
  echo "$orig" | grep -q 'violations=[1-9]' && ok "$name original: $orig" || bad "$name original map keeps every limit: $orig"
done

[ $fail -eq 0 ] && echo "difficulty_check: OK" || { echo "difficulty_check: FAILED"; exit 1; }
