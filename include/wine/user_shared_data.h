/*
 * User shared data address.
 *
 * Windows normally maps KUSER_SHARED_DATA at 0x7ffe0000. On Apple Silicon
 * macOS, arm64 Mach-O binaries require the low 4GB __PAGEZERO reservation, so
 * Wine cannot reserve the Windows address there. Keep the Wine-private mapping
 * above page zero on that host only.
 */

#ifndef __WINE_USER_SHARED_DATA_H
#define __WINE_USER_SHARED_DATA_H

#if defined(__WINE_DARWIN_ARM64_HOST) || (defined(__APPLE__) && defined(__aarch64__))
# define WINE_USER_SHARED_DATA_ADDRESS 0x600000000000ULL
#else
# define WINE_USER_SHARED_DATA_ADDRESS 0x7ffe0000UL
#endif

#endif
