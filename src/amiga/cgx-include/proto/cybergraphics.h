#ifndef PROTO_CYBERGRAPHICS_H
#define PROTO_CYBERGRAPHICS_H

/* Repaired for GCC. The 1995 CGX developer kit shipped this header including
 * BOTH <clib/cybergraphics_protos.h> and <inline/cybergraphics.h>, which is a
 * straight contradiction under GCC: clib declares every entry point as an
 * ordinary extern function, inline then defines the same names "static
 * __inline", and GCC rejects all 21 of them with "static declaration of X
 * follows non-static declaration". The real NDK proto headers pick one or the
 * other, and so do we - the inline stubs, because those are what actually
 * generate the register-argument library calls this target needs. clib is kept
 * for the non-GCC case only, which never happens here but costs nothing to
 * leave correct.
 *
 * The inline/ header in this directory is itself already a repaired copy: the
 * FD2Inline original lists d0 both as the "=r" output operand and in the
 * clobber list, and declares "register _res" with no type at all.
 */

#include <exec/types.h>

#ifndef __NOLIBBASE__
extern struct Library *CyberGfxBase;
#endif

#ifdef __GNUC__
#include <inline/cybergraphics.h>
#else
#include <clib/cybergraphics_protos.h>
#endif

#endif
