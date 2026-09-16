#!/bin/bash
# Task 6.2: runs port/BobrHopper.sh against a fake PortMaster (control.txt, get_controls, pm_message, pm_finish),
# a fake game binary with chosen exit codes and a fake westonwrap.sh. Scenarios: native works; native video fails
# -> WestonPack; the remembered mode skips native; no WestonPack available; a crash does not trigger the fallback.
#   wsl.exe -d Ubuntu-22.04 -e bash $REPO_WSL/build/launcher_test.sh

# the repository as WSL sees it (C:\dir -> /mnt/c/dir), so this works wherever the repository was cloned.
# Two steps on purpose: `A 2>/dev/null || B && C` runs C after a SUCCESSFUL A too, and the variable then holds
# two lines - which reaches `sh -c` as a broken multi-line script.
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cd "$REPO_DIR" && pwd -W 2>/dev/null) || REPO_WIN=$REPO_DIR
REPO_DRIVE_UPPER=$(printf '%s' "$REPO_WIN" | cut -c1)
REPO_DRIVE=$(printf '%s' "$REPO_DRIVE_UPPER" | tr 'A-Z' 'a-z')
REPO_WSL="/mnt/$REPO_DRIVE$(printf '%s' "$REPO_WIN" | cut -c3- | tr '\\' '/')"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T=/tmp/crossy-launcher-test
fail=0
check() { # description, command...
  local what="$1"; shift
  if "$@"; then echo "  ok   $what"; else echo "  FAIL $what"; fail=1; fi
}
count() { grep -c "$1" "$2" 2>/dev/null || true; }

setup() { # native_rc weston_rc with_weston_runtime(1/0)
  rm -rf /tmp/crossy-launcher-test
  mkdir -p $T/ports/bobrhopper $T/pm/libs $T/weston
  tr -d '\r' < "$ROOT/port/BobrHopper.sh" > $T/ports/BobrHopper.sh
  cat > $T/pm/control.txt <<'EOF'
CFW_NAME=ArkOS
DEVICE_NAME=R36S
get_controls() { DISPLAY_WIDTH=640; DISPLAY_HEIGHT=480; sdl_controllerconfig="03000000fakepad,R36S Gamepad,a:b0"; ESUDO=""; }
pm_message() { echo "PM_MESSAGE $*" >> /tmp/crossy-launcher-test/pm.txt; }
pm_finish() { echo "PM_FINISH" >> /tmp/crossy-launcher-test/pm.txt; }
EOF
  [ "$3" = 1 ] && touch $T/pm/libs/weston_pkg_0.2.squashfs
  cat > $T/ports/bobrhopper/bobrhopper.aarch64 <<EOF
#!/bin/bash
echo "CALL \${WESTON_MARK:-native} args=\$* pad=\$SDL_GAMECONTROLLERCONFIG" >> /tmp/crossy-launcher-test/calls.txt
if [ -n "\$WESTON_MARK" ]; then exit $2; else exit $1; fi
EOF
  chmod +x $T/ports/bobrhopper/bobrhopper.aarch64
  cat > $T/weston/westonwrap.sh <<'EOF'
#!/bin/bash
[ "$1" = cleanup ] && exit 0
shift 4
env WESTON_MARK=weston "$@"
EOF
  chmod +x $T/weston/westonwrap.sh
}
LOGF=$T/ports/bobrhopper/bobrhopper-launcher.log
MODEF=$T/ports/bobrhopper/conf/video_mode
CALLS=$T/calls.txt

echo "launcher_test: native works"
setup 0 0 1
CROSSY_PM_ROOTS="$T/pm" CROSSY_WESTON_DIR="$T/weston" bash $T/ports/BobrHopper.sh
check "game runs once, natively" [ "$(count 'CALL native' $CALLS)" = 1 -a "$(count 'CALL weston' $CALLS)" = 0 ]
check "no extra arguments" grep -q -m1 'args= pad=' $CALLS
check "pad mapping exported" grep -q 'pad=03000000fakepad' $CALLS
check "video_mode=native" grep -qx native $MODEF
check "log reaches STEP9 and pm_finish ran" bash -c "grep -q STEP9 $LOGF && grep -q PM_FINISH $T/pm.txt"

echo "launcher_test: native video fails (rc 4) -> WestonPack"
setup 4 0 1
CROSSY_PM_ROOTS="$T/pm" CROSSY_WESTON_DIR="$T/weston" bash $T/ports/BobrHopper.sh
check "native once, then the game under weston" [ "$(count 'CALL native' $CALLS)" = 1 -a "$(count 'CALL weston' $CALLS)" = 1 ]
check "video_mode=weston" grep -qx weston $MODEF
check "fallback logged" grep -q 'STEP5b native video failed (rc=4)' $LOGF

echo "launcher_test: remembered weston mode skips native"
: > $CALLS
CROSSY_PM_ROOTS="$T/pm" CROSSY_WESTON_DIR="$T/weston" bash $T/ports/BobrHopper.sh
check "only weston" [ "$(count 'CALL native' $CALLS)" = 0 -a "$(count 'CALL weston' $CALLS)" = 1 ]

echo "launcher_test: native video fails and no WestonPack"
setup 5 0 0
CROSSY_PM_ROOTS="$T/pm" bash $T/ports/BobrHopper.sh
check "FATAL logged" grep -q 'FATAL  no working video' $LOGF
check "PortMaster message shown" grep -q 'PM_MESSAGE' $T/pm.txt
check "no video_mode remembered" [ ! -f $MODEF ]

echo "launcher_test: crash (rc 139) is not a video failure"
setup 139 0 1
CROSSY_PM_ROOTS="$T/pm" CROSSY_WESTON_DIR="$T/weston" bash $T/ports/BobrHopper.sh
check "no weston run after a crash" [ "$(count 'CALL weston' $CALLS)" = 0 ]

echo "launcher_test: file format"
check "port/BobrHopper.sh has LF line endings" bash -c "! grep -q $'\r' '$ROOT/port/BobrHopper.sh'"
check "bash syntax" bash -n "$ROOT/port/BobrHopper.sh"

rm -rf /tmp/crossy-launcher-test
if [ $fail -ne 0 ]; then echo "launcher_test: FAILED"; exit 1; fi
echo "launcher_test: OK"
