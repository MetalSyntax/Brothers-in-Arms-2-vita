/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  watchdog.h
 * @brief Keeps a frozen main thread from locking the console.
 *
 * First boot (brothersinarms2_001.log) froze on a black screen after the
 * Ubisoft/Gearbox logo, with the PS button dead and the log tail lost to the
 * forced power-off. This thread runs independently of the game loop and:
 *
 *  - logs a heartbeat (frames rendered, engine state `s_game+0x140`) once a
 *    second, and every state change as it happens;
 *  - exits the app when SELECT + START are held for 2 s, even if the main
 *    thread never returns from the engine;
 *  - when the main loop stops advancing for WATCHDOG_STALL_S, logs it and
 *    deliberately faults, so the system writes a .psp2dmp holding every
 *    thread's registers (where the main thread is stuck) and kills the app
 *    instead of leaving the console frozen.
 */

#ifndef SOLOADER_WATCHDOG_H
#define SOLOADER_WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/** Start the watchdog thread. Call once, before the render loop. */
void watchdog_init(void);

/** Main loop heartbeat: call once per rendered frame, from the main thread,
 *  after gl_init() (it also samples vitaGL/newlib memory). */
void watchdog_frame(void);

/** Name the step the main thread is in (shown if it stalls). Static strings only. */
void watchdog_phase(const char * phase);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_WATCHDOG_H
