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

# BUILD FIRST. This used to run whatever binary happened to be lying in out/pc, so a change that was never compiled
# read as "no regression" - I reported an unchanged digest three times from a stale exe.
sh build/build_pc.sh bobrhopper >/dev/null || { echo "smoke_test: build failed"; exit 1; }

# ...AND ON A CONFIG OF ITS OWN. The game reads out/pc/conf/crossy.cfg, which is the author's own settings on this
# machine, so anything they (or a screenshot run) changed there changed the result: "ask_players=1" left behind by a
# screenshot put the "how many players?" page in front of the bot's every A press, and 68 games became 57 with a
# different digest. It looked exactly like a regression in the shared logic and it was not one. Defaults only now.
CONF="$D/smoke.cfg"
: > "$CONF"

run() { # name, args...
  name=$1; shift
  "$EXE" --headless --conf "$CONF" --seed "$SEED" "$@" > "$D/$name.txt" 2>&1
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
