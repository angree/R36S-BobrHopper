#!/bin/sh
# Task 4.3: the whole loop from scripted pad input, headless: home -> play -> hit by a car -> game over -> A ->
# home -> play -> Start (pause) -> settings (volume -1) -> back -> EXIT. Checks the state sequence, that the menu
# ended the run, that the setting was saved, and that the recorded input replays to the same end and digest.
#   sh build/loop_test.sh
cd "$(dirname "$0")/.." || exit 1
EXE=./out/pc/bobrhopper.exe
mkdir -p out/check/loop
rm -f out/check/loop/loop.rec out/check/loop/loop.cfg out/check/loop/run.txt out/check/loop/replay.txt

# Every input goes through btn: tokens (the real Input path), because --record only stores Input state; the
# `s`/`u` tokens call the game directly and would not replay. Seed 26 has two roads right ahead: hopping back
# and forth across them gets the chicken run over, then a long wait before A. GameState: 0 none, 1 playing, 3 over
SCRIPT="w20 btn:up w20 btn:up w20 btn:up w10 btn:up w10 btn:down w10 btn:up w10 btn:down w10 btn:up w10 btn:down \
w10 btn:up w10 btn:down w10 btn:up w10 btn:down w200 btn:a w40 btn:up w30 btn:start w5 btn:down w3 btn:a \
w5 btn:left w5 btn:b w5 btn:down w3 btn:down w3 btn:a"
FRAMES=2000

$EXE --headless --seed 26 --frames $FRAMES --auto "$SCRIPT" --record out/check/loop/loop.rec \
  --conf out/check/loop/loop.cfg > out/check/loop/run.txt 2>&1
rc=$?
fail=0
check() { # description, condition result (0 = ok)
  if [ "$2" -eq 0 ]; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}
echo "loop_test: run rc=$rc"
check "exit code 0" "$rc"
states=$(grep -o 'state [0-9] -> [0-9]' out/check/loop/run.txt | sed 's/state //; s/ -> /-/' | tr '\n' ' ')
echo "  states: $states"
case "$states" in
  "0-1 1-3 3-0 0-1 "*) r=0 ;;
  *) r=1 ;;
esac
check "state sequence none>playing>gameOver>none>playing" $r
steps=$(sed -n 's/.*end: steps=\([0-9]*\).*/\1/p' out/check/loop/run.txt)
[ -n "$steps" ] && [ "$steps" -lt $FRAMES ]
check "EXIT from the pause menu ended the run (step $steps < $FRAMES)" $?
grep -q '^volume=9$' out/check/loop/loop.cfg
check "settings saved volume=9" $?

$EXE --headless --seed 26 --replay out/check/loop/loop.rec --conf out/check/loop/loop.cfg \
  > out/check/loop/replay.txt 2>&1
d1=$(grep -o 'digest=[0-9a-f]*' out/check/loop/run.txt)
d2=$(grep -o 'digest=[0-9a-f]*' out/check/loop/replay.txt)
s2=$(sed -n 's/.*end: steps=\([0-9]*\).*/\1/p' out/check/loop/replay.txt)
[ -n "$d1" ] && [ "$d1" = "$d2" ] && [ "$steps" = "$s2" ]
check "replay: same end step ($s2) and $d2" $?

if [ $fail -ne 0 ]; then
  echo "loop_test: FAILED"
  exit 1
fi
echo "loop_test: OK"
