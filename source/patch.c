/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility and control opacity adjustments.
 */

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "patch.h"
#include "utils/glutil.h"
#include "utils/logger.h"

extern so_module so_mod;

/*
 * 1% opacity for the BOTTOM virtual controls only (joystick, fire, aim,
 * dash, stance). The top ones (pause, weapon/ammo panel) keep the game's
 * normal look.
 *
 * Main + 0x1b5c is the game's "controls opacity" option (Visuals menu, saved
 * in profile.sav, default 1.0). The disassembly reads it (0x1b40 + 28) as the
 * HUD sprite alpha all over Main::PaintIGNormal, RenderFireButton and
 * CInputJoystick::RenderJoystickButton -- the top HUD included. Forcing it to
 * 0.01 every frame (first version) dimmed the pause button and weapon panel
 * too and saved 0.01 into the profile. Now it is 0.01 only for the duration
 * of the two bottom-control render calls and restored right after.
 *
 * Hooks call the original through a trampoline (the 2 relocated ARM
 * prologue instructions + jump back) instead of SO_CONTINUE's
 * unpatch/call/repatch: that costs 4 kernel calls (2 memcpy + 2 cache
 * flushes) per call, and PaintGraphItem runs for every HUD item every frame.
 */

#define TRAMP_WORDS 4

// ARM-mode code slots rewritten at runtime by make_tramp().
#define DECLARE_TRAMP(name) \
    __attribute__((naked, noinline, target("arm"), aligned(16))) static void name(void) { \
        __asm__ volatile(".word 0xe320f000\n.word 0xe320f000\n.word 0xe320f000\n.word 0xe320f000\n"); \
    }

DECLARE_TRAMP(tramp_joy_btn)
DECLARE_TRAMP(tramp_fire_btn)
DECLARE_TRAMP(tramp_paint_graph)

/*
 * Hooks an ARM function whose first two instructions are position
 * independent (checked by the caller: push / sub sp) and returns a callable
 * pointer to the original.
 */
static uintptr_t hook_with_tramp(uintptr_t addr, uintptr_t dst, void (*tramp)(void)) {
    if (!addr || (addr & 1))
        return 0;
    so_hook h = hook_arm(addr, dst);
    uint32_t code[TRAMP_WORDS] = {
        h.orig_instr[0], h.orig_instr[1],
        0xe51ff004,              // LDR PC, [PC, #-4]
        (uint32_t)(addr + 8),
    };
    uintptr_t t = (uintptr_t)tramp & ~1u;
    kuKernelCpuUnrestrictedMemcpy((void *)t, code, sizeof(code));
    kuKernelFlushCaches((void *)t, sizeof(code));
    return t;
}

static int prologue_ok(uintptr_t addr) {
    // push {..., lr} ; sub sp, sp, #imm
    const uint32_t *p = (const uint32_t *)addr;
    return (p[0] & 0xFFFF0000) == 0xE92D0000 && (p[1] & 0xFFFFF000) == 0xE24DD000;
}

#define CONTROLS_ALPHA 0.01f

static uintptr_t s_game;
static inline float *opacity_ptr(void *main) { return (float *)((uintptr_t)main + 0x1b5c); }

typedef void (*fn_JoyBtn)(void *, int, int, int, int);
static fn_JoyBtn orig_joy_btn;
static void my_RenderJoystickButton(void *thiz, int p1, int p2, int p3, int p4) {
    // thiz + 0x6c = Main*; alpha = joy[0x5c] * Main[0x1b5c]
    void *main = thiz ? *(void **)((uintptr_t)thiz + 0x6c) : NULL;
    if (!main) {
        orig_joy_btn(thiz, p1, p2, p3, p4);
        return;
    }
    float *op = opacity_ptr(main);
    float saved = *op;
    *op = CONTROLS_ALPHA;
    orig_joy_btn(thiz, p1, p2, p3, p4);
    *op = saved;
}

typedef void (*fn_FireBtn)(void *, int);
static fn_FireBtn orig_fire_btn;
static void my_RenderFireButton(void *thiz, int p1) {
    float *op = opacity_ptr(thiz);
    float saved = *op;
    *op = CONTROLS_ALPHA;
    orig_fire_btn(thiz, p1);
    *op = saved;
}

/*
 * _ZN4Main14PaintGraphItemEiiiif = (this, group, item, x, y, float alpha).
 * The first version dropped `y` and went through SO_CONTINUE's unprototyped
 * pointer (float -> double), so the original read garbage for alpha: the
 * main menu background (RenderMenuBG, group 0, alpha 1.0) came out black
 * (log 007).
 *
 * Group 0x10 items 0x30..0x37 are the aim / dash / stance buttons, all at
 * the bottom; the item's GUI rect (params 3, 6 = y, h in a 480x320 base) is
 * checked too so nothing in the top band ever gets dimmed.
 */
typedef int (*fn_PaintGraphItem)(void *, int, int, int, int, float);
typedef int (*fn_GetParam)(void *, int, int);
static fn_PaintGraphItem orig_paint_graph;
static fn_GetParam get_param;

static int item_in_top_band(void *main, int group, int item) {
    if (!get_param)
        return 0;
    void **levels = *(void ***)((uintptr_t)main + 0x1360);
    if (!levels || !levels[group])
        return 0;
    void *gl = levels[group];
    if (item >= *(int *)((uintptr_t)gl + 8))
        return 0;
    int y = get_param(gl, item, 3), h = get_param(gl, item, 6);
    return y + h / 2 < 110;
}

static int my_PaintGraphItem(void *thiz, int group, int item, int x, int y, float alpha) {
    if (group == 0x10 && item >= 0x30 && item <= 0x37 && !item_in_top_band(thiz, group, item))
        alpha = CONTROLS_ALPHA;
    return orig_paint_graph(thiz, group, item, x, y, alpha);
}

/*
 * Once per frame: builds before this one forced 0.01 into the controls
 * opacity every frame and it got saved to profile.sav. The Visuals slider
 * never goes that low, so anything below 0.3 is that leftover -> back to 1.0.
 */
void patch_dim_tick(void) {
    typedef void *(*fn_GetGame)(void);
    static fn_GetGame pGetGame = NULL;
    if (!pGetGame)
        pGetGame = (fn_GetGame)so_symbol(&so_mod, "_Z7GetGamev");
    if (!pGetGame)
        return;
    s_game = (uintptr_t)pGetGame();
    if (s_game) {
        float *op = opacity_ptr((void *)s_game);
        if (*op < 0.3f)
            *op = 1.0f;
    }
}

static void hook_one(const char *sym, uintptr_t dst, void (*tramp)(void),
                     uintptr_t *orig, const char *what) {
    uintptr_t addr = so_symbol(&so_mod, sym);
    if (!addr) {
        l_warn("patch: %s symbol not found", what);
        return;
    }
    if (!prologue_ok(addr)) {
        l_warn("patch: %s has an unexpected prologue, not hooked", what);
        return;
    }
    *orig = hook_with_tramp(addr, dst, tramp);
    l_success("patch: hooked %s", what);
}

static void patch_dim_controls(void) {
    get_param = (fn_GetParam)so_symbol(&so_mod, "_ZN8GUILevel13GetParamValueEii");
    hook_one("_ZN14CInputJoystick20RenderJoystickButtonEiiii", (uintptr_t)&my_RenderJoystickButton,
             tramp_joy_btn, (uintptr_t *)&orig_joy_btn, "CInputJoystick::RenderJoystickButton (1% opacity)");
    hook_one("_ZN4Main16RenderFireButtonEii", (uintptr_t)&my_RenderFireButton,
             tramp_fire_btn, (uintptr_t *)&orig_fire_btn, "Main::RenderFireButton (1% opacity)");
    hook_one("_ZN4Main14PaintGraphItemEiiiif", (uintptr_t)&my_PaintGraphItem,
             tramp_paint_graph, (uintptr_t *)&orig_paint_graph, "Main::PaintGraphItem (1% bottom HUD buttons)");
}

void so_patch(void) {
    /*
     * so_patch() runs before so_initialize(), i.e. before libbia2.so's
     * INIT_ARRAY. One of its static initializers derives UI constants from
     * SCREEN_HEIGHT (`50 / (SCREEN_HEIGHT / 320)`, pseudo-C), long before
     * appInit() assigns the real value. The .data defaults already are
     * 800x480; set them explicitly so the two can never drift apart.
     */
    int * screen_w = (int *) so_symbol(&so_mod, "SCREEN_WIDTH");
    int * screen_h = (int *) so_symbol(&so_mod, "SCREEN_HEIGHT");
    if (screen_w && screen_h) {
        l_info("SCREEN_WIDTH/HEIGHT defaults %dx%d -> %dx%d",
               *screen_w, *screen_h, SCREEN_W, SCREEN_H);
        *screen_w = SCREEN_W;
        *screen_h = SCREEN_H;
    } else {
        l_warn("SCREEN_WIDTH/SCREEN_HEIGHT symbols not found.");
    }

    patch_dim_controls();
}
