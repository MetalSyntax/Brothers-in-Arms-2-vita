/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "watchdog.h"
#include "utils/logger.h"

#include <malloc.h>
#include <stdatomic.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <so_util/so_util.h>
#include <vitaGL.h>

extern so_module so_mod;

#define WATCHDOG_TICK_US     250000
// Longest the main loop may go without finishing a frame. Loads are split
// over many frames by the engine (the log shows ~1 s gaps between asset
// opens), so a 30 s gap is a hang, not a slow load.
#define WATCHDOG_STALL_S     30
#define EXIT_COMBO           (SCE_CTRL_SELECT | SCE_CTRL_START)
#define EXIT_HOLD_S          2

static atomic_uint s_frames = ATOMIC_VAR_INIT(0);

/*
 * Memory figures, sampled by the MAIN thread (watchdog_frame) and only
 * printed here. Calling vglMemFree()/mallinfo() from this thread crashed
 * (Bug #2, brothersinarms2_002.log + psp2dmp: data abort in
 * vgl_mem_get_free_space while the main thread was still inside
 * vglInitExtended) -- vitaGL's heaps are neither initialized that early nor
 * safe to walk from another thread.
 */
static atomic_uint s_mem_vram_kb, s_mem_ram_kb, s_mem_phycont_kb, s_mem_newlib_kb;
static atomic_bool s_mem_valid = ATOMIC_VAR_INIT(false);
static const char * volatile s_phase = "boot";

// libbia2.so: s_game is what GetGame() returns; Main::Render switches on
// *(int *)(s_game + 0x140), the engine's top-level state.
static int ** s_game;
static unsigned int * s_engine_frame;   // g_dwCurrentFrm, bumped in appRender

static int engine_state(void) {
    if (!s_game || !*s_game)
        return -1;
    return *(int *)((char *) *s_game + 0x140);
}

static void force_exit(const char * why) {
    l_fatal("watchdog: %s -- exiting.", why);
    log_shutdown();
    sceKernelExitProcess(0);
}

static int watchdog_thread(SceSize args, void * argp) {
    unsigned int last_frames = 0;
    SceUInt64 last_progress = sceKernelGetProcessTimeWide();
    SceUInt64 last_beat = 0;
    SceUInt64 combo_since = 0;
    int last_state = -2;

    for (;;) {
        sceKernelDelayThread(WATCHDOG_TICK_US);
        SceUInt64 now = sceKernelGetProcessTimeWide();

        // Emergency exit, independent of the (possibly stuck) main thread.
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & EXIT_COMBO) == EXIT_COMBO) {
            if (!combo_since)
                combo_since = now;
            else if (now - combo_since >= EXIT_HOLD_S * 1000000ULL)
                force_exit("SELECT+START held");
        } else {
            combo_since = 0;
        }

        unsigned int frames = atomic_load(&s_frames);
        int state = engine_state();

        if (state != last_state) {
            l_info("watchdog: engine state %d -> %d (frame %u)", last_state, state, frames);
            last_state = state;
        }

        if (frames != last_frames) {
            last_frames = frames;
            last_progress = now;
        }

        if (now - last_beat >= 1000000ULL) {
            last_beat = now;
            // Memory (sampled by the main thread): running vitaGL out of
            // RAM/CDRAM mid-load (~140 MB of ATC textures) is a prime suspect
            // for a whole-console freeze.
            if (atomic_load(&s_mem_valid))
                l_info("watchdog: hb frames=%u engine_frm=%u state=%d phase=%s | "
                       "free KB: vram=%u ram=%u phycont=%u | newlib used=%u KB",
                       frames, s_engine_frame ? *s_engine_frame : 0, state, s_phase,
                       atomic_load(&s_mem_vram_kb), atomic_load(&s_mem_ram_kb),
                       atomic_load(&s_mem_phycont_kb), atomic_load(&s_mem_newlib_kb));
            else
                l_info("watchdog: hb frames=%u engine_frm=%u state=%d phase=%s",
                       frames, s_engine_frame ? *s_engine_frame : 0, state, s_phase);
        }

        if (now - last_progress >= WATCHDOG_STALL_S * 1000000ULL) {
            l_fatal("watchdog: main loop stalled for %d s in phase '%s' (frame %u, "
                    "engine state %d). Faulting on purpose so the system writes a "
                    ".psp2dmp with the main thread's backtrace.",
                    WATCHDOG_STALL_S, s_phase, frames, state);
            log_sync();
            // Deliberate data abort: the coredump captures all threads.
            *(volatile int *) 0 = 0xDEAD;
            force_exit("stall"); // not reached
        }
    }
    return 0;
}

void watchdog_init(void) {
    s_game = (int **) so_symbol(&so_mod, "s_game");
    s_engine_frame = (unsigned int *) so_symbol(&so_mod, "g_dwCurrentFrm");

    // Higher priority than the game thread so a busy-looping main thread
    // can't starve it; it sleeps almost all the time.
    SceUID thid = sceKernelCreateThread("bia2_watchdog", watchdog_thread, 0x40,
                                        16 * 1024, 0, 0, NULL);
    if (thid < 0) {
        l_error("watchdog: thread creation failed: 0x%08x", thid);
        return;
    }
    sceKernelStartThread(thid, 0, NULL);
    l_info("watchdog: up (stall %d s, exit = hold SELECT+START %d s)",
           WATCHDOG_STALL_S, EXIT_HOLD_S);
}

void watchdog_frame(void) {
    unsigned int f = atomic_fetch_add(&s_frames, 1) + 1;

    // Once a second at 60 fps; only ever called after gl_init().
    if (f % 60 == 1) {
        struct mallinfo mi = mallinfo();
        atomic_store(&s_mem_vram_kb, (unsigned) (vglMemFree(VGL_MEM_VRAM) / 1024));
        atomic_store(&s_mem_ram_kb, (unsigned) (vglMemFree(VGL_MEM_RAM) / 1024));
        atomic_store(&s_mem_phycont_kb, (unsigned) (vglMemFree(VGL_MEM_PHYCONT) / 1024));
        atomic_store(&s_mem_newlib_kb, (unsigned) (mi.uordblks / 1024));
        atomic_store(&s_mem_valid, true);
    }
}

void watchdog_phase(const char * phase) {
    s_phase = phase;
}
