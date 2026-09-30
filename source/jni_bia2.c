/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "jni_bia2.h"
#include "reimpl/io.h"
#include "utils/logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include <falso_jni/FalsoJNI.h>

static jbyteArray make_byte_array(const void * data, int len) {
    jbyteArray arr = jni->NewByteArray(&jni, len);
    if (arr && len > 0)
        jni->SetByteArrayRegion(&jni, arr, 0, len, (const jbyte *) data);
    return arr;
}

static const char * jstring_arg(va_list * args) {
    jstring str = (jstring) va_arg(*args, jobject);
    if (!str)
        return NULL;
    return jni->GetStringUTFChars(&jni, str, NULL);
}

/* ---- BIA2 ---- */

void BIA2_sendAppToBackground(jmethodID id, va_list args) {
    l_info("BIA2.sendAppToBackground(): ignored.");
}

void BIA2_Exit(jmethodID id, va_list args) {
    // Java: finish() the activity and restart the package. Called from the
    // in-game "Quit" confirmation.
    l_info("BIA2.Exit() called -- shutting down.");
    log_shutdown();
    sceKernelExitProcess(0);
    while (1);
}

jint BIA2_IsWifiEnabled(jmethodID id, va_list args) {
    // Keeps GLLive/online code paths off; the port has no network stack up.
    return 0;
}

void BIA2_launchGetGames(jmethodID id, va_list args) {
    l_info("BIA2.launchGetGames(): no Android market, ignored.");
}

void BIA2_launchIGP(jmethodID id, va_list args) {
    int lang = va_arg(args, int);
    l_info("BIA2.launchIGP(%d): no IGP activity, ignored.", lang);
}

jobject BIA2_getHostName(jmethodID id, va_list args) {
    // Java: (Build.MODEL + "_" + Build.PRODUCT).getBytes()
    static const char host[] = "PSVita_PSVita";
    return make_byte_array(host, sizeof(host) - 1);
}

void BIA2_OpenGLive(jmethodID id, va_list args) {
    int lang = va_arg(args, int);
    l_info("BIA2.OpenGLive(%d): no GLLive activity, ignored.", lang);
}

void BIA2_NotifyTrophy(jmethodID id, va_list args) {
    int trophy = va_arg(args, int);
    l_info("BIA2.NotifyTrophy(%d)", trophy);
}

#include "video.h"
#include "utils/glutil.h"

/* ---- GLMediaPlayer ---- */

void GLMediaPlayer_loadMovie(jmethodID id, va_list args) {
    const char * name = jstring_arg(&args);
    if (name && strlen(name) > 0) {
        l_info("GLMediaPlayer.loadMovie(%s): starting video playback", name);
        video_play(name);
        l_info("GLMediaPlayer.loadMovie(%s): playback finished", name);
        // gl_init() dejó la PHYCONT fuera de vitaGL para el decoder
        // (sceVideodecInitLibrary daba 0x80024302, log 007); ahora sí se la da.
        gl_provide_phycont();
    } else {
        l_warn("GLMediaPlayer.loadMovie: null or empty name");
    }
    free((void *) name);
}

/* ---- GLResLoader ----
 *
 * None of these are called by libbia2.so's own code paths (its assets are
 * all fopen()ed directly -- no callers of GetNativeResource* in the
 * pseudo-C), but they are registered by GLResLoader.nativeInit(), so they get
 * a straightforward file-backed implementation of the Java fallback path
 * (`new File(path)`) instead of a silent NULL.
 */

static const char * strip_dot_slash(const char * s) {
    if (strncmp(s, ".//", 3) == 0)
        return s + 3;
    if (strncmp(s, "./", 2) == 0)
        return s + 2;
    return s;
}

static FILE * open_resource(const char * name) {
    char buf[PATH_MAX];
    const char * path = vita_path(strip_dot_slash(name), buf, sizeof(buf), false);
    FILE * f = fopen(path, "rb");
    if (!f) {
        // Relative names resolve against the engine's data dir.
        char rel[PATH_MAX];
        snprintf(rel, sizeof(rel), GAME_PATH "%s", path);
        f = fopen(rel, "rb");
    }
    return f;
}

static long file_size(FILE * f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return size;
}

jobject GLResLoader_getResourceFull(jmethodID id, va_list args) {
    const char * name = jstring_arg(&args);
    if (!name)
        return NULL;

    jobject ret = NULL;
    FILE * f = open_resource(name);
    if (f) {
        long size = file_size(f);
        void * data = malloc(size > 0 ? size : 1);
        if (data && fread(data, 1, size, f) == (size_t) size)
            ret = make_byte_array(data, (int) size);
        free(data);
        fclose(f);
    } else {
        l_warn("GLResLoader.getResourceFull(%s): not found", name);
    }
    free((void *) name);
    return ret;
}

jobject GLResLoader_getResourceBytes(jmethodID id, va_list args) {
    const char * name = jstring_arg(&args);
    int offset = va_arg(args, int);
    int len = va_arg(args, int);
    if (!name)
        return NULL;

    jobject ret = NULL;
    FILE * f = open_resource(name);
    if (f && len >= 0) {
        void * data = calloc(1, len > 0 ? len : 1);
        if (data) {
            if (offset > 0)
                fseek(f, offset, SEEK_SET);
            fread(data, 1, len, f);
            ret = make_byte_array(data, len);
            free(data);
        }
    } else {
        l_warn("GLResLoader.getResourceBytes(%s, %d, %d): not found", name, offset, len);
    }
    if (f)
        fclose(f);
    free((void *) name);
    return ret;
}

jint GLResLoader_getResourceLength(jmethodID id, va_list args) {
    const char * name = jstring_arg(&args);
    if (!name)
        return 0;
    FILE * f = open_resource(name);
    jint size = 0;
    if (f) {
        size = (jint) file_size(f);
        fclose(f);
    }
    free((void *) name);
    return size;
}

// GLResLoader: openRawResource(R.raw.raw_000 + i) -> RES_PATH/raw/raw_%03d.ogg
// (only raw_000.ogg, the installer music, ships in the APK).
static FILE * open_raw(int i) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), RES_PATH "raw/raw_%03d.ogg", i);
    FILE * f = fopen(path, "rb");
    if (!f)
        l_warn("GLResLoader: %s not found", path);
    return f;
}

jobject GLResLoader_getSoundRaw(jmethodID id, va_list args) {
    int i = va_arg(args, int);
    FILE * f = open_raw(i);
    if (!f)
        return NULL;

    jobject ret = NULL;
    long size = file_size(f);
    void * data = malloc(size > 0 ? size : 1);
    if (data && fread(data, 1, size, f) == (size_t) size)
        ret = make_byte_array(data, (int) size);
    free(data);
    fclose(f);
    return ret;
}

jint GLResLoader_getResourceLengthSoundRaw(jmethodID id, va_list args) {
    int i = va_arg(args, int);
    FILE * f = open_raw(i);
    if (!f)
        return 0;
    jint size = (jint) file_size(f);
    fclose(f);
    return size;
}
