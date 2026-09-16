#!/bin/sh
# R1.7: UI screenshots in fixed situations: home (title animation), settings, pause, game over banners, restart fade.
#   sh build/ui_shots.sh gles <outdir> [WxH] [view-scale]  -> out/pc/bobrhopper.exe (the R36S build: before/after a UI
#                                                             change the PNGs must be byte-identical)
#   sh build/ui_shots.sh sw <outdir> [WxH] [view-scale] [ui-scale] -> out/pc/sw_game.exe --hud (the SF2000 software
#                                                             renderer; ui-scale 2 = the core's 640x480 layouts on
#                                                             320x240 with the half-size fonts of data_sf2000)
# The GLES scripts press pad buttons (btn:), sw_game opens the same menu on the same step instead. bobrhopper spends a
# step on every token (shot: too), game/script.cpp takes shot: requests with the next token, hence `shot:x w1` there.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1
KIND=$1
OUT=$2
SIZE=${3:-640x480}
SCALE=${4:-3}
UISCALE=${5:-1}
UIOPTS=""
[ "$UISCALE" != 1 ] && UIOPTS="--ui-scale $UISCALE --ui-data data_sf2000"
[ -n "$KIND" ] && [ -n "$OUT" ] || { echo "usage: ui_shots.sh gles|sw <outdir> [WxH] [view-scale]"; exit 1; }
mkdir -p "$OUT" out/check/ui_conf

# name|seed|frames|gles script|sw script|sw options
CASES="home|3|133|w19 shot:home_a w40 shot:home_b w70 shot:home_c|w19 shot:home_a w1 w40 shot:home_b w1 w70 shot:home_c w1|
settings|3|43|w30 btn:select w10 shot:settings|w30 w1 w10 shot:settings w1|--settings-at 33
pause|3|74|w20 s w40 btn:start w10 shot:pause|w20 s w40 w1 w10 shot:pause w1|--pause-at 63
gameover|26|262|w20 s w20 u w20 u w10 u w10 d w10 u w30 shot:gameover_a w40 shot:gameover_b w60 shot:gameover_c a w5 shot:fade_a w12 shot:fade_b|w20 s w20 u w20 u w10 u w10 d w10 u w30 shot:gameover_a w1 w40 shot:gameover_b w1 w60 shot:gameover_c w1 a w5 shot:fade_a w1 w12 shot:fade_b w1|"

echo "$CASES" | while IFS='|' read -r name seed frames gles sw opts; do
  if [ "$KIND" = gles ]; then
    # a fresh config file per run: the defaults, whatever the user's out/pc/conf holds
    printf '' > out/check/ui_conf/$name.cfg
    ./out/pc/bobrhopper.exe --hidden --seed "$seed" --frames "$frames" --auto "$gles" --shot-dir "$OUT" --size "$SIZE" \
      --view-scale "$SCALE" --view-shift -0.15 --shadows full --character chicken \
      --conf out/check/ui_conf/$name.cfg >/dev/null 2>&1
  else
    # shellcheck disable=SC2086
    ./out/pc/sw_game.exe --seed "$seed" --frames "$frames" --script "$sw" --shot-dir "$OUT" --size "$SIZE" \
      --view-scale "$SCALE" --view-shift -0.15 --shadows full --hud $opts $UIOPTS >/dev/null
  fi
done
rm -f "$OUT/final.png"
ls "$OUT" | wc -l
