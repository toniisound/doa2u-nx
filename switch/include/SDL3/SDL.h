/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The two SDL clock calls portable runtime files make (perf_overlay.c,
   free_camera_adapter.c); the Switch build has no SDL. */
#ifndef DOA2U_NX_SDL_SHIM_H
#define DOA2U_NX_SDL_SHIM_H

#include <stdint.h>

#ifdef __SWITCH__
#include <switch.h>
static inline uint64_t SDL_GetTicksNS(void) { return armTicksToNs(armGetSystemTick()); }
#else
#include <time.h>
static inline uint64_t SDL_GetTicksNS(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}
#endif
static inline uint64_t SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000u; }

#endif
