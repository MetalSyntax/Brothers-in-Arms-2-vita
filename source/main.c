#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/dialog.h"
#include "audio.h"
#include "input.h"
#include "video.h"
#include "watchdog.h"

#include <psp2/apputil.h>
#include <psp2/io/stat.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

// On Android the engine runs on GLSurfaceView's GLThread (~1 MB stack) and
// keeps multi-KB path buffers on the stack (e.g. the 1 KB sprintf in its
// file-open wrapper); the Vita's default main-thread stack is far smaller.
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

/*
 * Resolution reported to the engine (SCREEN_W/SCREEN_H, utils/glutil.h).
 * 800x480 is what libbia2.so is built around: SCREEN_WIDTH/SCREEN_HEIGHT
 * default to 800/480 in .data, a static initializer derives constants from SCREEN_HEIGHT before appInit() ever
 * runs, and LibEffects renders the scene into a fixed 1024x512 render
 * target -- a taller screen would be clipped. glutil.c scales the engine's
 * default-framebuffer viewports up to the Vita's 960x544.
 */

/*
 * All natives below come from GameRenderer.onSurfaceCreated() /
 * BIA2.onTouchEvent() etc. (jadx), signatures confirmed in the pseudo-C.
 *
 *   GameRenderer.nativeGetJNIEnv()            instance: (env, thiz) -> mEnv = env
 *   GLResLoader.nativeInit()                  static:   (env, clazz)
 *   GLMediaPlayer.nativeInit()                static:   (env, clazz)
 *   BIA2.nativeInit()                         static:   (env, clazz)
 *   GameRenderer.nativeInit(kb, 1, w, h, lang) static
 *   GameRenderer.nativeRender()               static
 */
typedef void (* fn_getjnienv)(void * env, void * thiz);
typedef void (* fn_class_init)(void * env, void * clazz);
typedef void (* fn_renderer_init)(void * env, void * clazz, int keyboard,
                                  int one, int width, int height, int lang);
typedef void (* fn_render)(void * env, void * clazz);

#define JNI_PREFIX "Java_com_gameloft_android_GAND_GloftBia2_bia2_"

static void * resolve(const char * name) {
    void * fn = (void *) so_symbol(&so_mod, name);
    if (!fn)
        l_error("Symbol not found: %s", name);
    else
        // Logged on purpose: these turn a PC in a .psp2dmp back into a
        // libbia2.so offset.
        l_info("Resolved %s -> %p", name, fn);
    return fn;
}

/*
 * BIA2.iG, from BIA2.c() on the ISO3 locale: 0 eng, 1 deu, 2 fra, 3 ita,
 * 4 esl, 5 jpn, 6 zho (7-9 are forced back to 0 by onCreate()). These match
 * the TEXT_EN/DE/FR/IT/SP/JP/CH files shipped in the data.
 */
static int game_language(void) {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_GERMAN:     return 1;
        case SCE_SYSTEM_PARAM_LANG_FRENCH:     return 2;
        case SCE_SYSTEM_PARAM_LANG_ITALIAN:    return 3;
        case SCE_SYSTEM_PARAM_LANG_SPANISH:    return 4;
        case SCE_SYSTEM_PARAM_LANG_JAPANESE:   return 5;
        case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
        case SCE_SYSTEM_PARAM_LANG_CHINESE_S:  return 6;
        default:                               return 0;
    }
}

int main() {
    soloader_init_all();

    // The Android cache (gameloft/games/BIA2/*) has to be in GAME_PATH;
    // without it ApplicationImpl::Init() fails and the engine exit(0)s with
    // nothing on screen.
    if (!file_exists(GAME_PATH "obj") || !file_exists(GAME_PATH "sounds/sound_0000.ogg")) {
        fatal_error("Game data not found. Copy the contents of the Android "
                    "cache folder gameloft/games/BIA2/ (obj, sprites, sounds/, "
                    "TEXT_*, ...) into %s", GAME_PATH);
    }
    // Where BIA2.data & co. get written (see vita_path() in reimpl/io.c).
    sceIoMkdir(SAVE_PATH, 0777);

    // Before anything that can hang: SELECT+START exit and stall detection
    // must work even if the engine never returns (see watchdog.h).
    watchdog_init();

    // GameRenderer.nativeInit() runs appInit() -> ApplicationImpl::Init(),
    // which starts issuing GL calls, so the context has to exist first (on
    // Android onSurfaceCreated() only runs once EGL has a surface).
    watchdog_phase("gl_init");
    gl_init();
    l_success("OpenGL initialized.");
    video_init();

    watchdog_phase("audio_init");
    audio_init();

    fn_getjnienv     GetJNIEnv          = resolve(JNI_PREFIX "GameRenderer_nativeGetJNIEnv");
    fn_class_init    GLResLoader_init   = resolve(JNI_PREFIX "GLResLoader_nativeInit");
    fn_class_init    GLMediaPlayer_init = resolve(JNI_PREFIX "GLMediaPlayer_nativeInit");
    fn_class_init    BIA2_init          = resolve(JNI_PREFIX "BIA2_nativeInit");
    fn_renderer_init Renderer_init      = resolve(JNI_PREFIX "GameRenderer_nativeInit");
    fn_render        Renderer_render    = resolve(JNI_PREFIX "GameRenderer_nativeRender");

    if (!GetJNIEnv || !GLResLoader_init || !GLMediaPlayer_init || !BIA2_init
        || !Renderer_init || !Renderer_render) {
        l_fatal("Required native entry points are missing.");
        return -1;
    }

    input_init((fn_touch_evt) resolve(JNI_PREFIX "BIA2_nativeTouchPressed"),
               (fn_touch_evt) resolve(JNI_PREFIX "BIA2_nativeTouchMoved"),
               (fn_touch_evt) resolve(JNI_PREFIX "BIA2_nativeTouchReleased"),
               (fn_key_evt) resolve(JNI_PREFIX "BIA2_nativeSetOnKeyDown"),
               (fn_key_evt) resolve(JNI_PREFIX "BIA2_nativeSetOnKeyUp"),
               SCREEN_W, SCREEN_H);

    int lang = game_language();

    /*
     * Same order as GameRenderer.onSurfaceCreated(). It matters: every
     * native reads the engine's cached `mEnv` global rather than the env it
     * is handed, and only nativeGetJNIEnv() sets it. The *_nativeInit calls
     * then look up their jmethodIDs through it (NewGlobalRef(clazz) +
     * GetStaticMethodID), and nativeInit() goes into appInit().
     */
    watchdog_phase("nativeInit");
    GetJNIEnv(&jni, &jni);
    l_success("mEnv primed via GameRenderer.nativeGetJNIEnv.");

    GLResLoader_init(&jni, &jni);
    l_success("GLResLoader.nativeInit done.");

    GLMediaPlayer_init(&jni, &jni);
    l_success("GLMediaPlayer.nativeInit done.");

    BIA2_init(&jni, &jni);
    l_success("BIA2.nativeInit done.");

    // nativeInit(0, 1, width, height, BIA2.iG): 0 = no hardware keyboard
    // (mHasKeyboard = arg > 1), the 1 is a literal in the Java caller.
    Renderer_init(&jni, &jni, 0, 1, SCREEN_W, SCREEN_H, lang);
    l_success("GameRenderer.nativeInit done (%dx%d, lang %d).", SCREEN_W, SCREEN_H, lang);

    // appInit() sets mbAppRequestExit when ApplicationImpl::Init() fails;
    // the next appRender() then just calls exit(0). Say why beforehand.
    const char * app_path = (const char *) so_symbol(&so_mod, "g_appPath");
    int * request_exit = (int *) so_symbol(&so_mod, "mbAppRequestExit");
    l_info("Engine data path (g_appPath): %s", app_path ? app_path : "?");
    if (request_exit && *request_exit)
        l_fatal("ApplicationImpl::Init() failed -- are the game files in "
                GAME_PATH "? (obj, sprites, sounds/, TEXT_*, ...)");

    l_info("Entering render loop. Log: %s", log_current_path());

    while (1) {
        // Keep the Vita's idle power-save (dim -> screen off -> suspend) from
        // kicking in while the game runs; nothing else resets that timer.
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        watchdog_phase("input");
        input_poll(&jni, &jni);
        watchdog_phase("nativeRender");
        Renderer_render(&jni, &jni);
        watchdog_phase("swap");
        gl_swap();
        watchdog_frame();
    }

    log_shutdown();
    sceKernelExitDeleteThread(0);
}
