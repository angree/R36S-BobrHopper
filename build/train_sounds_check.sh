#!/bin/sh
# O3.3/O3.4: the game's train sounds (GameContext::originalBehaviour = false) against the original's, through the
# logic trace (out/pc/trace.exe; --game-sounds switches the harness to the game's sounds):
#   - the alarm plays when a near track's lights start, the pass sound ~93 steps later as the train arrives
#   - no train sound is requested twice in one step
#   - a track far ahead of an idle hero is silent (the original plays its pass sound every ~4.6 s)
#   - the original's mode still plays train_move_0 at every wrap (the traces compared with tools/webref)
#   sh build/train_sounds_check.sh      (needs out/pc/trace.exe: sh build/build_pc.sh trace)
cd "$(dirname "$0")/.." || exit 1
EXE=./out/pc/trace.exe
D=out/check/train_sounds
mkdir -p "$D"
fail=0
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }
# "t=<step> <sounds>" for the steps with a train sound (the step number from the start of the line: "hit=0 " also
# contains "t=")
sounds() { sed -n 's/^t=\([0-9]*\) .* snd=\([^ ]*\).*/t=\1 \2/p' "$1" | grep train; }

"$EXE" --seed 7 --steps 900 --script "w20 s w20 u w20 u w800" --game-sounds --out "$D/train_wait_game.txt" > /dev/null 2>&1
"$EXE" --seed 7 --steps 900 --script "w20 s w20 u w20 u w800" --out "$D/train_wait_orig.txt" > /dev/null 2>&1
sounds "$D/train_wait_game.txt" > "$D/train_wait_game_sounds.txt"
first_alarm=$(grep -m1 'train_alarm' "$D/train_wait_game_sounds.txt" | sed 's/t=\([0-9]*\).*/\1/')
first_pass=$(grep -m1 'train_move_0' "$D/train_wait_game_sounds.txt" | sed 's/t=\([0-9]*\).*/\1/')
if [ -n "$first_alarm" ] && [ -n "$first_pass" ] && [ $((first_pass - first_alarm)) -ge 85 ] && [ $((first_pass - first_alarm)) -le 100 ]; then
  ok "near track: alarm at step $first_alarm, pass sound at step $first_pass"
else
  bad "alarm/pass timing: alarm '$first_alarm' pass '$first_pass'"
fi
alarms=$(grep -c 'train_alarm' "$D/train_wait_game_sounds.txt")
passes=$(grep -c 'train_move_0' "$D/train_wait_game_sounds.txt")
[ "$alarms" -ge 3 ] && [ "$passes" -ge 3 ] && ok "every train warns and passes ($alarms alarms, $passes pass sounds in 900 steps)" || bad "alarms $alarms, passes $passes"
grep -o 'snd=[^ ]*' "$D/train_wait_orig.txt" | grep -q 'train_alarm' && bad "the original's mode plays the alarm" || ok "the original's mode never plays the alarm"

"$EXE" --seed 1 --steps 700 --script "w20 s w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u" \
  --game-sounds --out "$D/two_tracks_game.txt" > /dev/null 2>&1
"$EXE" --seed 1 --steps 700 --script "w20 s w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u w20 u" \
  --out "$D/two_tracks_orig.txt" > /dev/null 2>&1
grep -o 'snd=[^ ]*' "$D/two_tracks_orig.txt" | grep -q 'train_move_0+train_move_0' && ok "two tracks in phase: the original stacks the sound" || bad "the scenario lost its two tracks in phase"
grep -o 'snd=[^ ]*' "$D/two_tracks_game.txt" | grep -q 'train_alarm+train_alarm\|train_move_0+train_move_0' && bad "the game stacks identical train sounds" || ok "the game plays each train sound once per step"

# an idle hero on the start row (z 8) hears tracks up to z 18: from the row layout (--row-types), a map whose only
# tracks lie beyond that is silent, a map with a track in range plays its alarm
far=""
near=""
for seed in $(seq 1 60); do
  "$EXE" --seed "$seed" --steps 1500 --script "w20 s w1500" --game --row-types --out "$D/idle_game.txt" > /dev/null 2>&1
  tracks=$(sed -n 's/^rows //p' "$D/idle_game.txt" | tr ' ' '\n' | sed -n 's/^\([0-9]*\):railroad$/\1/p')
  [ -z "$tracks" ] && continue
  nearest=$(echo "$tracks" | sort -n | head -1)
  sounds=$(grep -o 'snd=[^ ]*' "$D/idle_game.txt" | grep -o 'train_[a-z_0-9]*' | wc -l)
  if [ -z "$far" ] && [ "$nearest" -gt 18 ]; then far="$seed:$nearest:$sounds"; fi
  if [ -z "$near" ] && [ "$nearest" -le 18 ]; then near="$seed:$nearest:$(grep -o 'snd=[^ ]*' "$D/idle_game.txt" | grep -c 'train_alarm')"; fi
  [ -n "$far" ] && [ -n "$near" ] && break
done
[ -n "$far" ] && [ "${far##*:}" -eq 0 ] && ok "far track (seed ${far%%:*}, nearest track at z $(echo "$far" | cut -d: -f2)), idle hero: silent" || bad "far track: '$far' (seed:track z:train sounds)"
[ -n "$near" ] && [ "${near##*:}" -gt 0 ] && ok "near track (seed ${near%%:*}, track at z $(echo "$near" | cut -d: -f2)), idle hero: ${near##*:} alarms" || bad "near track: '$near' (seed:track z:alarms)"

[ $fail -eq 0 ] && echo "train_sounds_check: OK" || { echo "train_sounds_check: FAILED"; exit 1; }
