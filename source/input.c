/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "input.h"
#include "patch.h"
#include "utils/logger.h"
#include "utils/glutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/io/fcntl.h>
#include <psp2/motion.h>

#include <so_util/so_util.h>

extern so_module so_mod;

// Front panel resolution in SceTouchReport units.
#define PANEL_W 1920
#define PANEL_H 1088

/*
 * Slots allocation:
 * 0..2: Front touchscreen fingers
 * 3:    Left analog stick (movement)
 * 4:    Right analog stick (camera look)
 * 5:    Fire button (R1 / RTRIGGER)
 * 6:    Dash / sprint (CIRCLE, gameplay only)
 * 7:    Gestures / Taps (Aim, Action, Crouch, Weapon, Pause)
 * 8:    Camera look, alternate id (see syn_look)
 */
#define MAX_TOUCH_SLOTS 3
#define MOVE_SLOT       3
#define LOOK_SLOT       4
#define FIRE_SLOT       5
#define SPRINT_SLOT     6
#define GEST_SLOT       7
#define LOOK_SLOT_ALT   8
#define N_SLOTS         9

// Android KeyCodes
#define AKEYCODE_BACK       4
#define AKEYCODE_MENU       82
#define AKEYCODE_DPAD_UP    19
#define AKEYCODE_DPAD_DOWN  20
#define AKEYCODE_DPAD_LEFT  21
#define AKEYCODE_DPAD_RIGHT 22

// Virtual HUD button positions in engine coordinates (800x480)
#define MOVE_ANCHOR_X 121
#define MOVE_ANCHOR_Y 354
#define STICK_RADIUS  50
#define STICK_ENGAGE  0.25f
#define STICK_RELEASE 0.12f
#define SYN_STEP      10

#define LOOK_ANCHOR_X 520
#define LOOK_ANCHOR_Y 240
/*
 * Camera: Main::UpdateCameraYaw/Pitch take a per-frame delta (current minus
 * last pos of the owning touch) and never clamp yaw outside cover; the gain
 * saturates at 16 px/frame and is linear above. The press position picks the
 * GUI area (0x26 = drag camera), later moves can go anywhere, so the box can
 * be large -> few wraps.
 */
#define LOOK_BOX_X    180
#define LOOK_BOX_Y    150
#define LOOK_DZ       0.15f
#define LOOK_SPEED_X  24.0f
#define LOOK_SPEED_Y  14.0f

#define CAM_LEVEL_DEFAULT 4
#define CAM_LEVEL_MAX     10
#define CAM_CFG           DATA_PATH "camera_sens.txt"

#define FIRE_X   660
#define FIRE_Y   356
#define AIM_X    782
#define AIM_Y    469
#define ACT_X    398
#define ACT_Y    49
// Fallback only: the real dash button rect is read from the HUD GUI data
// (dash_center()).
#define SPRINT_X 762
#define SPRINT_Y 272
#define CROUCH_X 17
#define CROUCH_Y 456
#define WEAPON_X 689
#define WEAPON_Y 35
#define PAUSE_X  43
#define PAUSE_Y  39

#define BTN_FIRE (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)
#define BTN_AIM  (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)

#define GEST_TAP_FRAMES 4

// Main::Render switches on *(int *)(GetGame() + 0x140); 0x14 = in a mission.
#define ENGINE_STATE_INGAME 0x14

static fn_touch_evt s_pressed, s_moved, s_released;
static fn_key_evt s_key_down, s_key_up;
static int s_screen_w = 800, s_screen_h = 480;
static void *s_env = NULL, *s_thiz = NULL;

static int slot_hw[N_SLOTS];
static int slot_x[N_SLOTS];
static int slot_y[N_SLOTS];

static int syn_owned[N_SLOTS];
static int syn_x[N_SLOTS];
static int syn_y[N_SLOTS];

static float look_fx = LOOK_ANCHOR_X, look_fy = LOOK_ANCHOR_Y;
static int cam_level = CAM_LEVEL_DEFAULT;
static uint64_t cam_osd_until = 0;

static int gest_frames = 0;
static int gest_dx = 0;

static uint32_t s_old_buttons = 0;

static void cam_load(void) {
    SceUID fd = sceIoOpen(CAM_CFG, SCE_O_RDONLY, 0);
    if (fd < 0)
        return;
    char b[8] = { 0 };
    sceIoRead(fd, b, sizeof(b) - 1);
    sceIoClose(fd);
    int v = atoi(b);
    if (v >= 1 && v <= CAM_LEVEL_MAX)
        cam_level = v;
}

static void cam_save(void) {
    SceUID fd = sceIoOpen(CAM_CFG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return;
    char b[8];
    int n = sceClibSnprintf(b, sizeof(b), "%d\n", cam_level);
    sceIoWrite(fd, b, n);
    sceIoClose(fd);
}

static void cam_adjust(int d) {
    int v = cam_level + d;
    if (v < 1) v = 1;
    if (v > CAM_LEVEL_MAX) v = CAM_LEVEL_MAX;
    cam_level = v;
    cam_save();
    cam_osd_until = sceKernelGetProcessTimeWide() + 1500000;
    l_info("cam: sensitivity %d/%d", cam_level, CAM_LEVEL_MAX);
}

void cam_osd_draw(void) {
    if (!cam_osd_until || sceKernelGetProcessTimeWide() > cam_osd_until)
        return;

    GLint fbo, box[4];
    GLfloat cc[4];
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);

    if (fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_SCISSOR_TEST);

    const int bw = 22, bh = 14, gap = 4;
    int x0 = (REAL_SCREEN_W - (CAM_LEVEL_MAX * (bw + gap) - gap)) / 2;
    int y0 = REAL_SCREEN_H - 20 - bh;

    glScissor(x0 - 4, y0 - 4, CAM_LEVEL_MAX * (bw + gap) - gap + 8, bh + 8);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    for (int i = 0; i < CAM_LEVEL_MAX; i++) {
        glScissor(x0 + i * (bw + gap), y0, bw, bh);
        if (i < cam_level)
            glClearColor(0.2f, 0.8f, 1.0f, 1.0f);
        else
            glClearColor(0.25f, 0.25f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glScissor(box[0], box[1], box[2], box[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    if (!sc)
        glDisable(GL_SCISSOR_TEST);
    if (fbo)
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

static void gest_start(int x, int y, int dx, int frames) {
    if (gest_frames > 0 && syn_owned[GEST_SLOT]) {
        if (s_released)
            s_released(s_env, s_thiz, slot_x[GEST_SLOT], slot_y[GEST_SLOT], GEST_SLOT);
    }
    syn_owned[GEST_SLOT] = 1;
    slot_hw[GEST_SLOT] = -2;
    slot_x[GEST_SLOT] = x;
    slot_y[GEST_SLOT] = y;
    gest_dx = dx;
    gest_frames = frames;
    if (s_pressed)
        s_pressed(s_env, s_thiz, x, y, GEST_SLOT);
}

static void gest_update(void) {
    if (gest_frames <= 0)
        return;
    if (gest_dx) {
        slot_x[GEST_SLOT] += gest_dx;
        if (s_moved)
            s_moved(s_env, s_thiz, slot_x[GEST_SLOT], slot_y[GEST_SLOT], GEST_SLOT);
    }
    if (--gest_frames == 0) {
        if (s_released)
            s_released(s_env, s_thiz, slot_x[GEST_SLOT], slot_y[GEST_SLOT], GEST_SLOT);
        syn_owned[GEST_SLOT] = 0;
        slot_hw[GEST_SLOT] = -1;
        slot_x[GEST_SLOT] = -1;
        slot_y[GEST_SLOT] = -1;
    }
}

static void syn_stick(int slot, int cx, int cy, float dx, float dy) {
    float mag = sqrtf(dx * dx + dy * dy);
    if (!syn_owned[slot]) {
        if (mag < STICK_ENGAGE)
            return;
        syn_owned[slot] = 1;
        syn_x[slot] = cx;
        syn_y[slot] = cy;
        slot_hw[slot] = -2;
        slot_x[slot] = cx;
        slot_y[slot] = cy;
        if (s_pressed)
            s_pressed(s_env, s_thiz, cx, cy, slot);
    }
    if (mag < STICK_RELEASE) {
        syn_owned[slot] = 0;
        if (s_released)
            s_released(s_env, s_thiz, syn_x[slot], syn_y[slot], slot);
        slot_hw[slot] = -1;
        slot_x[slot] = -1;
        slot_y[slot] = -1;
        return;
    }
    if (mag > 1.0f) {
        dx /= mag;
        dy /= mag;
    }
    int tx = cx + (int)(dx * STICK_RADIUS);
    int ty = cy + (int)(dy * STICK_RADIUS);
    int nx = syn_x[slot], ny = syn_y[slot];
    int ddx = tx - nx, ddy = ty - ny;
    if (ddx > SYN_STEP) nx += SYN_STEP; else if (ddx < -SYN_STEP) nx -= SYN_STEP; else nx = tx;
    if (ddy > SYN_STEP) ny += SYN_STEP; else if (ddy < -SYN_STEP) ny -= SYN_STEP; else ny = ty;
    if (nx != syn_x[slot] || ny != syn_y[slot]) {
        syn_x[slot] = nx;
        syn_y[slot] = ny;
        slot_x[slot] = nx;
        slot_y[slot] = ny;
        if (s_moved)
            s_moved(s_env, s_thiz, nx, ny, slot);
    }
}

static float look_curve(float v) {
    float a = v < 0 ? -v : v;
    if (a <= LOOK_DZ)
        return 0.0f;
    float t = (a - LOOK_DZ) / (1.0f - LOOK_DZ);
    if (t > 1.0f)
        t = 1.0f;
    t = 0.35f * t + 0.65f * t * t;
    return v < 0 ? -t : t;
}

// Touch id currently owning the camera drag (LOOK_SLOT or LOOK_SLOT_ALT).
static int look_id = LOOK_SLOT;

static void look_release(int id) {
    if (!syn_owned[id])
        return;
    if (s_released)
        s_released(s_env, s_thiz, syn_x[id], syn_y[id], id);
    syn_owned[id] = 0;
    slot_hw[id] = -1;
    slot_x[id] = -1;
    slot_y[id] = -1;
}

static void look_press(int id) {
    syn_owned[id] = 1;
    slot_hw[id] = -2;
    syn_x[id] = slot_x[id] = LOOK_ANCHOR_X;
    syn_y[id] = slot_y[id] = LOOK_ANCHOR_Y;
    if (s_pressed)
        s_pressed(s_env, s_thiz, LOOK_ANCHOR_X, LOOK_ANCHOR_Y, id);
}

/*
 * Right stick -> continuous drag with velocity, wrapping back to the anchor
 * at the box edge so the camera turns without limit.
 *
 * The wrap used to release and re-press the SAME id in one frame. Both events
 * land in the same engine touch slot: the press sets it active (+0x8ec) but
 * the release flag (+0x874) is still set, so Main::PostProcessKeys kills the
 * new press at the end of the frame and the camera stops dead until the stick
 * is centered again ("la cámara está limitada"). Now the wrap presses the
 * other id at the anchor and releases the old one: two different slots.
 */
static void syn_look(float dx, float dy) {
    float k = (float)cam_level / (float)CAM_LEVEL_MAX;
    float vx = look_curve(dx) * LOOK_SPEED_X * k;
    float vy = look_curve(dy) * LOOK_SPEED_Y * k;
    if (vx == 0.0f && vy == 0.0f) {
        look_release(LOOK_SLOT);
        look_release(LOOK_SLOT_ALT);
        return;
    }
    if (!syn_owned[look_id]) {
        look_fx = (float)LOOK_ANCHOR_X;
        look_fy = (float)LOOK_ANCHOR_Y;
        look_press(look_id);
        return;   // first frame after a press always yields delta 0
    }
    look_fx += vx;
    look_fy += vy;
    int nx = (int)look_fx, ny = (int)look_fy;
    if (nx != syn_x[look_id] || ny != syn_y[look_id]) {
        syn_x[look_id] = slot_x[look_id] = nx;
        syn_y[look_id] = slot_y[look_id] = ny;
        if (s_moved)
            s_moved(s_env, s_thiz, nx, ny, look_id);
    }
    if (nx < LOOK_ANCHOR_X - LOOK_BOX_X || nx > LOOK_ANCHOR_X + LOOK_BOX_X ||
        ny < LOOK_ANCHOR_Y - LOOK_BOX_Y || ny > LOOK_ANCHOR_Y + LOOK_BOX_Y) {
        int old = look_id;
        look_id = old == LOOK_SLOT ? LOOK_SLOT_ALT : LOOK_SLOT;
        look_fx = (float)LOOK_ANCHOR_X;
        look_fy = (float)LOOK_ANCHOR_Y;
        look_press(look_id);
        look_release(old);
    }
}

// ---------------------------------------------------------------------------
// Engine helpers
// ---------------------------------------------------------------------------
static uintptr_t game_ptr(void) {
    typedef void *(*fn_GetGame)(void);
    static fn_GetGame f = NULL;
    if (!f)
        f = (fn_GetGame)so_symbol(&so_mod, "_Z7GetGamev");
    return f ? (uintptr_t)f() : 0;
}

static int engine_state(void) {
    uintptr_t g = game_ptr();
    return g ? *(int *)(g + 0x140) : -1;
}

/*
 * Running = holding the HUD "dash" button: Main::UpdateIGNormal calls
 * GetAreaTouchId(0x10, 9) every frame and MC::SetDashMode(1) while a touch is
 * inside it. Its rect lives in the level GUI data (GUILevel params 2,3,5,6 =
 * x, y, w, h in a 480x320 base), same maths as Main::GetAreaTouchId.
 * Hold at the centre: UpdateMCViewInputDash steers if the finger is >20 px
 * off it.
 */
static int dash_center(int *cx, int *cy) {
    typedef int (*fn_GetParam)(void *, int, int);
    static fn_GetParam get_param = NULL;
    static int logged = 0;
    if (!get_param)
        get_param = (fn_GetParam)so_symbol(&so_mod, "_ZN8GUILevel13GetParamValueEii");
    uintptr_t g = game_ptr();
    if (!get_param || !g)
        return 0;
    void **levels = *(void ***)(g + 0x1360);
    if (!levels || !levels[0x10])
        return 0;
    void *hud = levels[0x10];
    if (9 >= *(int *)((uintptr_t)hud + 8))
        return 0;
    int x = get_param(hud, 9, 2), y = get_param(hud, 9, 3);
    int w = get_param(hud, 9, 5), h = get_param(hud, 9, 6);
    *cx = (x + w / 2) * s_screen_w / 480;
    *cy = (y + h / 2) * s_screen_h / 320;
    if (!logged) {
        logged = 1;
        l_info("input: dash button rect (480x320) x=%d y=%d w=%d h=%d -> centre (%d,%d)",
               x, y, w, h, *cx, *cy);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Accelerometer (BIA2.onSensorChanged -> nativeAccelerometer, raw m/s^2)
// ---------------------------------------------------------------------------
/*
 * The engine only reads Z for "shake" QTEs (Main::ProcessPhoneGesturesInGame:
 * |z - z_prev| > 0.3 in one frame -> Main::IsPhoneShaked, used by
 * MC::UpdateMelee); X/Y feed the glider tilt. Nothing called it before, so
 * the values stayed 0 and a melee "SHAKE" prompt could never be passed.
 *
 * Real Vita motion sensor (sceMotion, in g, gravity-signed) converted to the
 * Android frame of a landscape phone: phone X = screen up, phone Y = screen
 * left, Z out of the screen, reaction sign (+9.81 face up).
 * Plus a button shake: SELECT alone pulses Z for SHAKE_FRAMES.
 */
#define G_MS2        9.80665f
#define SHAKE_FRAMES 16

typedef void (*fn_accel)(void *env, void *thiz, float x, float y, float z);
static fn_accel s_accel;
static int s_motion_ok;
static int s_shake_frames;
static float acc_x, acc_y, acc_z = G_MS2;

static void accel_init(void) {
    s_accel = (fn_accel)so_symbol(&so_mod,
        "Java_com_gameloft_android_GAND_GloftBia2_bia2_BIA2_nativeAccelerometer");
    s_motion_ok = sceMotionStartSampling() >= 0;
    l_info("input: accelerometer %s, motion sensor %s",
           s_accel ? "hooked" : "MISSING", s_motion_ok ? "on" : "unavailable");
}

static void accel_poll(void) {
    if (!s_accel)
        return;
    if (s_motion_ok) {
        SceMotionState ms;
        if (sceMotionGetState(&ms) >= 0) {
            float rx = -ms.acceleration.x * G_MS2;
            float ry = -ms.acceleration.y * G_MS2;
            float rz = -ms.acceleration.z * G_MS2;
            // Light low-pass: resting jitter must stay far below 0.3/frame.
            acc_x += 0.5f * (ry - acc_x);
            acc_y += 0.5f * (-rx - acc_y);
            acc_z += 0.5f * (rz - acc_z);
            static int logged = 0;
            if (!logged && (ms.acceleration.x || ms.acceleration.y || ms.acceleration.z)) {
                logged = 1;
                l_info("input: motion first sample g=(%.2f, %.2f, %.2f) -> android (%.2f, %.2f, %.2f)",
                       ms.acceleration.x, ms.acceleration.y, ms.acceleration.z, acc_x, acc_y, acc_z);
            }
        }
    }
    float z = acc_z;
    if (s_shake_frames > 0) {
        z = (s_shake_frames & 1) ? acc_z - 6.0f : acc_z + 6.0f;
        s_shake_frames--;
    }
    s_accel(s_env, s_thiz, acc_x, acc_y, z);
}

void input_init(fn_touch_evt pressed, fn_touch_evt moved, fn_touch_evt released,
                fn_key_evt key_down, fn_key_evt key_up,
                int screen_w, int screen_h) {
    s_pressed = pressed;
    s_moved = moved;
    s_released = released;
    s_key_down = key_down;
    s_key_up = key_up;
    s_screen_w = screen_w;
    s_screen_h = screen_h;

    for (int s = 0; s < N_SLOTS; s++) {
        slot_hw[s] = -1;
        slot_x[s] = -1;
        slot_y[s] = -1;
        syn_owned[s] = 0;
        syn_x[s] = 0;
        syn_y[s] = 0;
    }

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    cam_load();
    accel_init();
    l_info("input: touch -> %dx%d, dual-analog controls initialized (cam sens %d/%d)",
           screen_w, screen_h, cam_level, CAM_LEVEL_MAX);
}

static void poll_touch(void * env, void * thiz) {
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 1)
        return;

    int seen[MAX_TOUCH_SLOTS] = { 0 };

    for (int r = 0; r < (int) touch.reportNum; r++) {
        int hw = touch.report[r].id;
        int x = touch.report[r].x * s_screen_w / PANEL_W;
        int y = touch.report[r].y * s_screen_h / PANEL_H;

        int slot = -1;
        for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
            if (slot_hw[s] == hw) {
                slot = s;
                break;
            }
        }

        if (slot == -1) {
            for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
                if (slot_hw[s] == -1) {
                    slot = s;
                    break;
                }
            }
            if (slot == -1)
                continue; // Fingers exceed touch slots

            slot_hw[slot] = hw;
            slot_x[slot] = x;
            slot_y[slot] = y;
            seen[slot] = 1;
            if (s_pressed)
                s_pressed(env, thiz, x, y, slot);
            continue;
        }

        seen[slot] = 1;
        if (x != slot_x[slot] || y != slot_y[slot]) {
            slot_x[slot] = x;
            slot_y[slot] = y;
            if (s_moved)
                s_moved(env, thiz, x, y, slot);
        }
    }

    for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
        if (slot_hw[s] != -1 && !seen[s]) {
            if (s_released)
                s_released(env, thiz, slot_x[s], slot_y[s], s);
            slot_hw[s] = -1;
            slot_x[s] = -1;
            slot_y[s] = -1;
        }
    }
}

static void poll_buttons(void * env, void * thiz) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    uint32_t down = pad.buttons & ~s_old_buttons;
    uint32_t up   = ~pad.buttons & s_old_buttons;

    // SELECT alone = shake the console (melee QTE "SHAKE")
    if (down & SCE_CTRL_SELECT)
        s_shake_frames = SHAKE_FRAMES;

    // Camera sensitivity adjustment via SELECT + D-Pad UP/DOWN
    if (pad.buttons & SCE_CTRL_SELECT) {
        if (down & SCE_CTRL_UP)
            cam_adjust(+1);
        else if (down & SCE_CTRL_DOWN)
            cam_adjust(-1);
    }

    // D-Pad navigation for menus (when SELECT is not held)
    if (!(pad.buttons & SCE_CTRL_SELECT)) {
        if (s_key_down && s_key_up) {
            if (down & SCE_CTRL_UP)    s_key_down(env, thiz, AKEYCODE_DPAD_UP);
            if (up   & SCE_CTRL_UP)    s_key_up(env, thiz, AKEYCODE_DPAD_UP);
            if (down & SCE_CTRL_DOWN)  s_key_down(env, thiz, AKEYCODE_DPAD_DOWN);
            if (up   & SCE_CTRL_DOWN)  s_key_up(env, thiz, AKEYCODE_DPAD_DOWN);
            if (down & SCE_CTRL_LEFT)  s_key_down(env, thiz, AKEYCODE_DPAD_LEFT);
            if (up   & SCE_CTRL_LEFT)  s_key_up(env, thiz, AKEYCODE_DPAD_LEFT);
            if (down & SCE_CTRL_RIGHT) s_key_down(env, thiz, AKEYCODE_DPAD_RIGHT);
            if (up   & SCE_CTRL_RIGHT) s_key_up(env, thiz, AKEYCODE_DPAD_RIGHT);
        }
    }

    // Update ongoing tap/gesture
    gest_update();

    // FIRE (R-Trigger / R1, hold)
    int firing = (pad.buttons & BTN_FIRE) != 0;
    if (firing) {
        if (!syn_owned[FIRE_SLOT]) {
            syn_owned[FIRE_SLOT] = 1;
            slot_hw[FIRE_SLOT] = -2;
            slot_x[FIRE_SLOT] = FIRE_X;
            slot_y[FIRE_SLOT] = FIRE_Y;
            if (s_pressed)
                s_pressed(env, thiz, FIRE_X, FIRE_Y, FIRE_SLOT);
        }
    } else {
        if (syn_owned[FIRE_SLOT]) {
            syn_owned[FIRE_SLOT] = 0;
            if (s_released)
                s_released(env, thiz, FIRE_X, FIRE_Y, FIRE_SLOT);
            slot_hw[FIRE_SLOT] = -1;
            slot_x[FIRE_SLOT] = -1;
            slot_y[FIRE_SLOT] = -1;
        }
    }

    // CIRCLE: hold = dash/run in a mission; BACK everywhere else (in a
    // mission BACK opens the pause menu -- that's START's job).
    int ingame = engine_state() == ENGINE_STATE_INGAME;
    int sprinting = ingame && (pad.buttons & SCE_CTRL_CIRCLE) != 0;
    if (sprinting) {
        int cx = SPRINT_X, cy = SPRINT_Y;
        dash_center(&cx, &cy);
        if (!syn_owned[SPRINT_SLOT]) {
            syn_owned[SPRINT_SLOT] = 1;
            slot_hw[SPRINT_SLOT] = -2;
            slot_x[SPRINT_SLOT] = cx;
            slot_y[SPRINT_SLOT] = cy;
            if (s_pressed)
                s_pressed(env, thiz, cx, cy, SPRINT_SLOT);
        }
    } else if (syn_owned[SPRINT_SLOT]) {
        syn_owned[SPRINT_SLOT] = 0;
        if (s_released)
            s_released(env, thiz, slot_x[SPRINT_SLOT], slot_y[SPRINT_SLOT], SPRINT_SLOT);
        slot_hw[SPRINT_SLOT] = -1;
        slot_x[SPRINT_SLOT] = -1;
        slot_y[SPRINT_SLOT] = -1;
    }
    static int back_down = 0;
    if ((down & SCE_CTRL_CIRCLE) && !ingame) {
        if (s_key_down) s_key_down(env, thiz, AKEYCODE_BACK);
        back_down = 1;
    }
    if ((up & SCE_CTRL_CIRCLE) && back_down) {
        if (s_key_up) s_key_up(env, thiz, AKEYCODE_BACK);
        back_down = 0;
    }

    // Context Action (CROSS)
    if (down & SCE_CTRL_CROSS) {
        gest_start(ACT_X, ACT_Y, 0, GEST_TAP_FRAMES);
    }

    // Aim / Iron Sights (L-Trigger / L1, tap toggle)
    if (down & BTN_AIM) {
        gest_start(AIM_X, AIM_Y, 0, GEST_TAP_FRAMES);
    }

    // Crouch / Stance (SQUARE)
    if (down & SCE_CTRL_SQUARE) {
        gest_start(CROUCH_X, CROUCH_Y, 0, GEST_TAP_FRAMES);
    }

    // Weapon Switch / Reload (TRIANGLE)
    if (down & SCE_CTRL_TRIANGLE) {
        gest_start(WEAPON_X, WEAPON_Y, 0, GEST_TAP_FRAMES);
    }

    // Pause / Menu (START)
    if (down & SCE_CTRL_START) {
        gest_start(PAUSE_X, PAUSE_Y, 0, GEST_TAP_FRAMES);
        if (s_key_down) s_key_down(env, thiz, AKEYCODE_MENU);
    }
    if (up & SCE_CTRL_START) {
        if (s_key_up) s_key_up(env, thiz, AKEYCODE_MENU);
    }

    // Analog Sticks
    float mdx = ((float)pad.lx - 128.0f) / 128.0f;
    float mdy = ((float)pad.ly - 128.0f) / 128.0f;

    // Supplement left stick with D-Pad when SELECT is not held
    if (!(pad.buttons & SCE_CTRL_SELECT)) {
        if (fabsf(mdx) < 0.1f) {
            if (pad.buttons & SCE_CTRL_LEFT)  mdx = -1.0f;
            if (pad.buttons & SCE_CTRL_RIGHT) mdx = 1.0f;
        }
        if (fabsf(mdy) < 0.1f) {
            if (pad.buttons & SCE_CTRL_UP)    mdy = -1.0f;
            if (pad.buttons & SCE_CTRL_DOWN)  mdy = 1.0f;
        }
    }

    float ldx = ((float)pad.rx - 128.0f) / 128.0f;
    float ldy = ((float)pad.ry - 128.0f) / 128.0f;

    syn_stick(MOVE_SLOT, MOVE_ANCHOR_X, MOVE_ANCHOR_Y, mdx, mdy);
    syn_look(ldx, ldy);
    accel_poll();

    s_old_buttons = pad.buttons;
}

void input_poll(void * env, void * thiz) {
    s_env = env;
    s_thiz = thiz;

    patch_dim_tick();
    poll_touch(env, thiz);
    poll_buttons(env, thiz);
}
