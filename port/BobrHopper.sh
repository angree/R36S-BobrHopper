#!/bin/bash
# BobrHopper launcher for R36S / ArkOS through PortMaster.
#
# Video: the game runs natively first (SDL2 KMSDRM + GLES2). If its video setup fails -- exit code 4 (no window
# or GL context) or 5 (renderer / assets) -- the same binary runs inside PortMaster's WestonPack, the way
# OpenSWOS runs on this device. The working mode is remembered in conf/video_mode.
# Input: no gptokeyb. The game reads the pad natively; gptokeyb on top doubled every press in OpenSWOS.
# Log: every step goes to bobrhopper/bobrhopper-launcher.log followed by sync, so it survives pulling the card.

progdir="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$progdir/bobrhopper"
BIN="$GAMEDIR/bobrhopper.aarch64"
CONFDIR="$GAMEDIR/conf"
LOG="$GAMEDIR/bobrhopper-launcher.log"

log() { echo "[$(date '+%H:%M:%S' 2>/dev/null)] $*" >> "$LOG"; sync 2>/dev/null; }

mkdir -p "$CONFDIR"
: > "$LOG"; sync
log "STEP0  launcher start progdir=$progdir arch=$(uname -m 2>/dev/null) user=$(id -un 2>/dev/null)"

# --- PortMaster ----------------------------------------------------------------------------------------------
# Test hooks (build/launcher_test.sh): CROSSY_PM_ROOTS replaces the search list, CROSSY_WESTON_DIR is an
# already mounted WestonPack runtime.
PM_ROOTS=${CROSSY_PM_ROOTS:-"/roms/ports/PortMaster /roms2/ports/PortMaster $HOME/.local/share/PortMaster /opt/system/Tools/PortMaster /opt/tools/PortMaster /storage/roms/ports/PortMaster"}
controlfolder=""
cf_fallback=""
WESTON_SQUASHFS=""
for cf in $PM_ROOTS; do
  [ -d "$cf" ] || continue
  # some devices have two PortMasters; prefer the one holding the WestonPack runtime
  if [ -z "$WESTON_SQUASHFS" ] && [ -f "$cf/libs/weston_pkg_0.2.squashfs" ]; then
    WESTON_SQUASHFS="$cf/libs/weston_pkg_0.2.squashfs"
    [ -f "$cf/control.txt" ] && controlfolder="$cf"
  fi
  [ -z "$cf_fallback" ] && [ -f "$cf/control.txt" ] && cf_fallback="$cf"
done
[ -z "$controlfolder" ] && controlfolder="$cf_fallback"
OUR_CTRL="$controlfolder"   # sourcing control.txt may overwrite $controlfolder
log "STEP1  portmaster=${OUR_CTRL:-NONE} weston_runtime=${WESTON_SQUASHFS:-NONE}"

if [ -n "$OUR_CTRL" ]; then
  source "$OUR_CTRL/control.txt"
  [ -f "$OUR_CTRL/mod_${CFW_NAME}.txt" ] && source "$OUR_CTRL/mod_${CFW_NAME}.txt"
  get_controls
  controlfolder="$OUR_CTRL"
else
  ESUDO=""
fi
: "${DISPLAY_WIDTH:=640}"; : "${DISPLAY_HEIGHT:=480}"
# pad mappings: the string form when control.txt gives one; ArkOS's get_controls gives a file instead
# (SDL_GAMECONTROLLERCONFIG_FILE), which the game loads itself
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
[ -n "$SDL_GAMECONTROLLERCONFIG_FILE" ] && export SDL_GAMECONTROLLERCONFIG_FILE
log "STEP2  cfw=$CFW_NAME device=$DEVICE_NAME res=${DISPLAY_WIDTH}x${DISPLAY_HEIGHT} ESUDO='$ESUDO' pad_mapping=$([ -n "$sdl_controllerconfig" ] && echo yes || echo no) pad_mapping_file=${SDL_GAMECONTROLLERCONFIG_FILE:-none}"
log "STEP3  libSDL2: $(ls /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0* /usr/lib/libSDL2-2.0.so.0* 2>/dev/null | tr '\n' ' ') dri: $(ls /dev/dri 2>/dev/null | tr '\n' ' ') $(ldd --version 2>/dev/null | head -1)"

cd "$GAMEDIR" || { log "FATAL  no game directory $GAMEDIR"; exit 1; }
chmod +x "$BIN" 2>/dev/null
if [ ! -x "$BIN" ]; then
  log "FATAL  binary missing: $BIN"
  pm_message "BobrHopper: bobrhopper.aarch64 is missing." 2>/dev/null
  sleep 3
  exit 1
fi

# --- running the game ----------------------------------------------------------------------------------------
video_failed() { [ "$1" -eq 4 ] || [ "$1" -eq 5 ]; }

run_native() {
  log "STEP4  native run: $BIN $*"
  echo "----- game output (native) -----" >> "$LOG"; sync
  "$BIN" "$@" >> "$LOG" 2>&1
  local rc=$?
  log "STEP5  native exit rc=$rc"
  return $rc
}

weston_dir="${CROSSY_WESTON_DIR:-/tmp/weston}"
run_weston() {
  if [ -z "$CROSSY_WESTON_DIR" ]; then
    if [ -z "$WESTON_SQUASHFS" ]; then
      log "STEP6  WestonPack runtime not installed"
      return 99
    fi
    $ESUDO mkdir -p "$weston_dir"
    [[ "$PM_CAN_MOUNT" != "N" ]] && $ESUDO umount "$weston_dir" 2>/dev/null
    $ESUDO mount "$WESTON_SQUASHFS" "$weston_dir"
    log "STEP6  weston mount rc=$?"
  fi
  if [ ! -f "$weston_dir/westonwrap.sh" ]; then
    log "STEP6  westonwrap.sh missing in $weston_dir"
    return 99
  fi
  log "STEP7  weston run: $BIN $*"
  echo "----- game output (weston) -----" >> "$LOG"; sync
  $ESUDO env "$weston_dir/westonwrap.sh" headless noop kiosk crusty_x11egl \
    SDL_GAMECONTROLLERCONFIG="$SDL_GAMECONTROLLERCONFIG" SDL_GAMECONTROLLERCONFIG_FILE="$SDL_GAMECONTROLLERCONFIG_FILE" \
    "$BIN" "$@" >> "$LOG" 2>&1
  local rc=$?
  log "STEP8  weston exit rc=$rc"
  $ESUDO "$weston_dir/westonwrap.sh" cleanup >> "$LOG" 2>&1
  if [ -z "$CROSSY_WESTON_DIR" ] && [[ "$PM_CAN_MOUNT" != "N" ]]; then
    $ESUDO umount "$weston_dir" 2>/dev/null
  fi
  return $rc
}

run_game() {
  local mode rc
  mode=$(cat "$CONFDIR/video_mode" 2>/dev/null)
  if [ "$mode" != "weston" ]; then
    run_native "$@"
    rc=$?
    if ! video_failed $rc; then
      echo native > "$CONFDIR/video_mode"; sync
      return $rc
    fi
    log "STEP5b native video failed (rc=$rc), trying WestonPack"
  fi
  run_weston "$@"
  rc=$?
  if [ $rc -eq 99 ]; then
    log "FATAL  no working video: native failed and WestonPack is not available"
    pm_message "BobrHopper: video did not start. Bring bobrhopper/bobrhopper-launcher.log to the PC." 2>/dev/null
    sleep 5
  elif ! video_failed $rc; then
    echo weston > "$CONFDIR/video_mode"; sync
  fi
  return $rc
}

run_game
log "STEP9  done"
sync
pm_finish 2>/dev/null
