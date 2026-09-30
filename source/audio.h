/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  audio.h
 * @brief Native replacement for `GLMediaPlayer` (SoundPool + MediaPlayer).
 *
 * `GLMediaPlayer.nativeInit()` in libbia2.so looks up 31 static methods on
 * the Java class (pseudo-C, see port_progress.md) and every sound the engine
 * makes goes through them. Their Android behaviour, from GLMediaPlayer.java /
 * ac.java (jadx), which this module reproduces:
 *
 *  - Sounds are ids 0..nativeGetTotalSounds()-1 (0x547), file
 *    `/sdcard/gameloft/games/bia2/sounds/sound_%04d.ogg` (registerSoundFile is
 *    done by the Java side itself, never by native code).
 *  - "Sound" = SoundPool: fully decoded, one voice per id (`hY[i][0]`);
 *    playSound(id, vol, pitch) restarts it at rate 1.0 -- the pitch argument
 *    goes to setRate() on the *previous* stream before play(), so it has no
 *    audible effect. setPitch(id, inst, rate) does change the live voice.
 *  - "Music" = MediaPlayer: streamed, optional loop, pause/resume/stop.
 *  - "Voice" = playSoundAsync(id, vol): GLAsyncPlayer, a streamed one-shot,
 *    stopped by stopVoice().
 *  - isSoundLoaded()/isMusicLoaded() return 0 when loaded and -1 when not.
 *
 * Mixing: one SceAudioOut BGM port at 48 kHz stereo, fed by a mixer thread
 * with linear resampling (the .ogg files are a mix of 22050 Hz mono and
 * 44100 Hz stereo).
 */

#ifndef SOLOADER_AUDIO_H
#define SOLOADER_AUDIO_H

#include <falso_jni/FalsoJNI.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);

// GLMediaPlayer static methods (FalsoJNI handlers, see java.c)
void GLMediaPlayer_update(jmethodID id, va_list args);
jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args);
jint GLMediaPlayer_isMusicLoaded(jmethodID id, va_list args);
void GLMediaPlayer_unloadSound(jmethodID id, va_list args);
void GLMediaPlayer_unloadMusic(jmethodID id, va_list args);
void GLMediaPlayer_loadSound(jmethodID id, va_list args);
void GLMediaPlayer_loadMusic(jmethodID id, va_list args);
jint GLMediaPlayer_getSoundReady(jmethodID id, va_list args);
void GLMediaPlayer_registerSoundFile(jmethodID id, va_list args);
void GLMediaPlayer_playSound(jmethodID id, va_list args);
void GLMediaPlayer_playSoundAsync(jmethodID id, va_list args);
void GLMediaPlayer_playMusic(jmethodID id, va_list args);
void GLMediaPlayer_pauseSound(jmethodID id, va_list args);
void GLMediaPlayer_pauseMusic(jmethodID id, va_list args);
void GLMediaPlayer_resumeSound(jmethodID id, va_list args);
void GLMediaPlayer_resumeMusic(jmethodID id, va_list args);
void GLMediaPlayer_resumeAllMusic(jmethodID id, va_list args);
void GLMediaPlayer_stopSound(jmethodID id, va_list args);
void GLMediaPlayer_stopMusic(jmethodID id, va_list args);
void GLMediaPlayer_setVolume(jmethodID id, va_list args);
void GLMediaPlayer_setVolumeMusic(jmethodID id, va_list args);
void GLMediaPlayer_resetSound(jmethodID id, va_list args);
void GLMediaPlayer_setPitch(jmethodID id, va_list args);
void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args);
void GLMediaPlayer_stopAllSFX(jmethodID id, va_list args);
void GLMediaPlayer_stopAllMusic(jmethodID id, va_list args);
void GLMediaPlayer_destroySoundPool(jmethodID id, va_list args);
void GLMediaPlayer_initSoundPoolArray(jmethodID id, va_list args);
jint GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args);
jboolean GLMediaPlayer_isSoundPlaying(jmethodID id, va_list args);
void GLMediaPlayer_stopVoice(jmethodID id, va_list args);
jint GLMediaPlayer_getVoiceDuration(jmethodID id, va_list args);
void GLMediaPlayer_resume(jmethodID id, va_list args);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_AUDIO_H
