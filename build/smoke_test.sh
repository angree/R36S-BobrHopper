#!/bin/sh
# Tasks 3.7 + 2.6: a bot plays N logic steps headless (no crash, no pool/animation leak), its input is recorded,
# the recording replays to the same state digest, and a second bot run with the same seed gives it too.
#   sh build/smoke_test.sh [steps=20000] [seed=9]
cd "$(dirname "$0")/.." || exit 1
N=${1:-20000}
SEED=${2:-9}
EXE=./out/pc/bobrhopper.exe
D=out/smoke
mkdir -p "$D"

run() { # name, args...
  name=$1; shift
  "$EXE" --headless --seed "$SEED" "$@" > "$D/$name.txt" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "smoke_test: $name failed rc=$rc"
    tail -5 "$D/$name.txt"
    exit 1
  fi
  grep -o 'digest=[0-9a-f]*' "$D/$name.txt" | tail -1
}

A=$(run bot1 --smoke "$N" --record "$D/bot.rec") || { echo "$A"; exit 1; }
B=$(run replay --replay "$D/bot.rec") || { echo "$B"; exit 1; }
C=$(run bot2 --smoke "$N") || { echo "$C"; exit 1; }
grep 'smoke: steps=' "$D/bot1.txt" | sed 's/^.*smoke:/smoke:/'
echo "bot1 $A / replay $B / bot2 $C"
if [ -z "$A" ] || [ "$A" != "$B" ] || [ "$A" != "$C" ]; then
  echo "smoke_test: FAILED (digests differ)"
  exit 1
fi
echo "smoke_test: OK"
