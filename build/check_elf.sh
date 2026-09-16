#!/bin/sh
# Fails unless the aarch64 binary needs glibc <= 2.28 and only the libraries ArkOS surely has.
set -e
BIN=$1
MAX_GLIBC=2.28
ALLOWED="libSDL2-2.0.so.0 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 ld-linux-aarch64.so.1 librt.so.1"

needed=$(readelf -d "$BIN" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
bad=0
for lib in $needed; do
  case " $ALLOWED " in
    *" $lib "*) ;;
    *) echo "check_elf: unexpected NEEDED $lib"; bad=1 ;;
  esac
done

top=$(readelf -V "$BIN" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
if [ -n "$top" ] && [ "$(printf '%s\n%s\n' "$top" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]; then
  echo "check_elf: needs GLIBC_$top > $MAX_GLIBC"; bad=1
fi
echo "check_elf: $(basename "$BIN") NEEDED=[$(echo $needed)] max GLIBC_$top"
[ $bad = 0 ] && echo "check_elf: OK" || { echo "check_elf: FAILED"; exit 1; }
