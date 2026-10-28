#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the game's TLS layout on the calling thread.
 *
 * bionic keeps its TCB at TP+0 and the game was compiled with glibc local-exec
 * TLS offsets starting at TP+0.  We allocate a private block, copy the bionic
 * TCB/TLS area to the front of it (same positive offsets), point TPIDR_EL0 at
 * the copy and leave FACTORIO_TLS_BASE bytes of zeroed memory behind it for the
 * game.  Returns the game TLS base, or NULL on failure.
 */
void *fshim_install_tls(void);

/* dlopen the patched game image, wire up the dynapi override and call its
 * main(argc, argv).  Returns the game's exit code (or 70 on setup failure).
 */
int fshim_run(const char *game_path, int argc, char **argv);

/* Load range of libfactorio_game.so, set by fshim_run() before main();
 * crash logs print addresses relative to the base. */
extern uintptr_t fshim_game_base;
extern uintptr_t fshim_game_end;

#ifdef __cplusplus
}
#endif
