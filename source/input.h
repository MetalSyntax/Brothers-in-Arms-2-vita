/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  input.h
 * @brief Drives the engine's touch/key native entry points from the Vita's
 *        front touch panel, physical buttons, and dual analog sticks.
 */

#ifndef SOLOADER_INPUT_H
#define SOLOADER_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (* fn_touch_evt)(void * env, void * thiz, int x, int y, int id);
typedef void (* fn_key_evt)(void * env, void * clazz, int keycode);

/**
 * Store the resolved native entry points and configure touch + pad sampling.
 *
 * @param screen_w,screen_h Resolution reported to the engine (800x480).
 */
void input_init(fn_touch_evt pressed, fn_touch_evt moved, fn_touch_evt released,
                fn_key_evt key_down, fn_key_evt key_up,
                int screen_w, int screen_h);

/** Poll touch + pad, update synthetic controls and fire callbacks. Once per frame. */
void input_poll(void * env, void * thiz);

/** Draws the camera sensitivity OSD bar if recently adjusted. */
void cam_osd_draw(void);

#ifdef __cplusplus
}
#endif

#endif // SOLOADER_INPUT_H
