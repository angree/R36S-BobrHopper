// Stand-ins for the SF2000 firmware functions (sf2000_fw.h) when the core runs inside apps/sf2000_host.cpp on the PC or
// under qemu. The clock is driven by the host, so runs are deterministic unless --realtime is given.
#include <cstdarg>
#include <cstdio>

#include "sf2000/sf2000_fw.h"

uint32_t g_hostTickMs = 0;
int g_hostSyncCount = 0;
bool g_hostQuietLog = false;

extern "C" {

uint32_t os_get_tick_count(void) { return g_hostTickMs; }

int fs_sync(const char *path)
{
    g_hostSyncCount++;
    std::printf("host: fs_sync %s\n", path);
    return 0;
}

void xlog(const char *fmt, ...)
{
    if (g_hostQuietLog) return;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("xlog: ", stdout);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

} // extern "C"
