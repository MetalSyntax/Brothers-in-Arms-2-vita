/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_bia2.h
 * @brief Native replacements for the `BIA2` and `GLResLoader` static methods
 *        libbia2.so calls back into, plus `GLMediaPlayer.loadMovie`.
 *
 * Registered by `BIA2.nativeInit()` / `GLResLoader.nativeInit()` /
 * `GLMediaPlayer.nativeInit()` via GetStaticMethodID (pseudo-C). Behaviour
 * mirrors BIA2.java / GLResLoader.java (jadx) where it makes sense on a Vita;
 * everything that opens an Android activity (IGP, GLLive, market, browser)
 * is a logged no-op.
 */

#ifndef SOLOADER_JNI_BIA2_H
#define SOLOADER_JNI_BIA2_H

#include <falso_jni/FalsoJNI_ImplBridge.h>

#ifdef __cplusplus
extern "C" {
#endif

// BIA2
void BIA2_sendAppToBackground(jmethodID id, va_list args);
void BIA2_Exit(jmethodID id, va_list args);
jint BIA2_IsWifiEnabled(jmethodID id, va_list args);
void BIA2_launchGetGames(jmethodID id, va_list args);
void BIA2_launchIGP(jmethodID id, va_list args);
jobject BIA2_getHostName(jmethodID id, va_list args);
void BIA2_OpenGLive(jmethodID id, va_list args);
void BIA2_NotifyTrophy(jmethodID id, va_list args);

// GLMediaPlayer
void GLMediaPlayer_loadMovie(jmethodID id, va_list args);

// GLResLoader
jobject GLResLoader_getResourceFull(jmethodID id, va_list args);
jobject GLResLoader_getResourceBytes(jmethodID id, va_list args);
jint GLResLoader_getResourceLength(jmethodID id, va_list args);
jobject GLResLoader_getSoundRaw(jmethodID id, va_list args);
jint GLResLoader_getResourceLengthSoundRaw(jmethodID id, va_list args);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_JNI_BIA2_H
