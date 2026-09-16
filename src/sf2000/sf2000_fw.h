// The few SF2000 firmware / multicore framework functions the core calls (addresses in the framework's
// bisrv_08_03-core.ld, implementations in debug.c). The PC and qemu hosts provide their own versions.
#pragma once

#include <cstdint>

extern "C" {
uint32_t os_get_tick_count(void);    // milliseconds since boot
int fs_sync(const char *path);        // flush a written file to the SD card
void xlog(const char *fmt, ...);      // appends to /mnt/sda1/log.txt, only when that file already exists (slow)
}
