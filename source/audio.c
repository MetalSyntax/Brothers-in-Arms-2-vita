/*
 * Copyright (C) 2026 Brothers-in-Arms-2-vita contributors
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "audio.h"
#include "utils/logger.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb/stb_vorbis.c>

// GLMediaPlayer_nativeGetTotalSounds() in libbia2.so returns this constant.
#define NUM_SOUNDS 0x547

#define SOUNDS_DIR GAME_PATH "sounds/"

#define MIX_RATE     48000
#define MIX_GRAIN    1024
#define MAX_VOICES   32

// SFX whose decoded PCM would exceed this are streamed from disk on each
// play instead of kept in RAM (the long ones are speech/ambience loops).
#define MAX_DECODED_BYTES (1024 * 1024)

// Frames decoded per refill of a streaming voice.
#define STREAM_CHUNK 2048

// android.media.MediaPlayer states as tracked by GLMediaPlayer.hK[i].dE.
enum {
    MS_NONE      = 0,
    MS_RELEASED  = 1,
    MS_PREPARING = 4,
    MS_PREPARED  = 5,
    MS_PLAYING   = 6,
    MS_STOPPED   = 7,
    MS_PAUSED    = 8,
    MS_COMPLETED = 9,
};

typedef struct {
    int loaded;        // SoundPool hX[i] >= 0
    int16_t * pcm;     // NULL for streamed-on-play samples
    int frames;
    int channels;
    int rate;
    int duration_ms;   // GLMediaPlayer.hz[i]; -1 until first loadSound()
} Sample;

typedef struct {
    int active;
    int paused;
    int sound_id;
    float volume;
    float rate_mul;    // SoundPool.setRate()
    int loop;

    // Source: either a decoded sample...
    const Sample * sample;
    // ...or a streaming decoder.
    stb_vorbis * vorbis;
    int16_t stream_buf[STREAM_CHUNK * 2];
    int stream_frames;
    int eof;

    int channels;
    int rate;
    double pos;        // frame position within pcm / stream_buf
    uint64_t start_ms;
} Voice;

typedef struct {
    int state;
    int loop;
    float volume;
    int voice;         // index into s_voices, -1 if none
} Music;

static Sample s_samples[NUM_SOUNDS];
static Music s_music[NUM_SOUNDS];
static int s_sfx_voice[NUM_SOUNDS];  // voice playing hY[i][0], -1 if none
static int s_async_voice = -1;       // GLAsyncPlayer's current voice
static int s_pool_ready = 0;         // GLMediaPlayer.in
static int s_sound_ready[NUM_SOUNDS];

static Voice s_voices[MAX_VOICES];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_port = -1;

static uint64_t now_ms(void) {
    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    return tick.tick / 1000;
}

static int valid_id(int id) {
    if (id < 0 || id >= NUM_SOUNDS) {
        l_warn("audio: sound id %d out of range", id);
        return 0;
    }
    return 1;
}

static void sound_path(int id, char * buf, size_t size) {
    snprintf(buf, size, SOUNDS_DIR "sound_%04d.ogg", id);
}

/* ---- voices (caller holds s_lock) ---- */

static void voice_release(int v) {
    Voice * voice = &s_voices[v];
    if (voice->vorbis)
        stb_vorbis_close(voice->vorbis);
    int id = voice->sound_id;
    if (id >= 0 && id < NUM_SOUNDS) {
        if (s_sfx_voice[id] == v)
            s_sfx_voice[id] = -1;
        if (s_music[id].voice == v)
            s_music[id].voice = -1;
    }
    if (s_async_voice == v)
        s_async_voice = -1;
    memset(voice, 0, sizeof(*voice));
    voice->sound_id = -1;
}

static int voice_alloc(void) {
    for (int v = 0; v < MAX_VOICES; v++)
        if (!s_voices[v].active)
            return v;

    // Steal the oldest non-music voice.
    int oldest = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        int id = s_voices[v].sound_id;
        if (id >= 0 && id < NUM_SOUNDS && s_music[id].voice == v)
            continue;
        if (oldest < 0 || s_voices[v].start_ms < s_voices[oldest].start_ms)
            oldest = v;
    }
    if (oldest >= 0)
        voice_release(oldest);
    return oldest;
}

static int voice_start_stream(int id, float volume, int loop) {
    char path[256];
    sound_path(id, path, sizeof(path));

    int err = 0;
    stb_vorbis * vorbis = stb_vorbis_open_filename(path, &err, NULL);
    if (!vorbis) {
        l_warn("audio: can't open %s (stb_vorbis error %d)", path, err);
        return -1;
    }

    int v = voice_alloc();
    if (v < 0) {
        stb_vorbis_close(vorbis);
        return -1;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    Voice * voice = &s_voices[v];
    voice->active = 1;
    voice->sound_id = id;
    voice->volume = volume;
    voice->rate_mul = 1.0f;
    voice->loop = loop;
    voice->vorbis = vorbis;
    voice->channels = info.channels > 1 ? 2 : 1;
    voice->rate = info.sample_rate;
    voice->start_ms = now_ms();
    return v;
}

static int voice_start_sample(int id, float volume) {
    const Sample * sample = &s_samples[id];
    if (!sample->pcm)
        return voice_start_stream(id, volume, 0);

    int v = voice_alloc();
    if (v < 0)
        return -1;

    Voice * voice = &s_voices[v];
    voice->active = 1;
    voice->sound_id = id;
    voice->volume = volume;
    voice->rate_mul = 1.0f;
    voice->sample = sample;
    voice->channels = sample->channels;
    voice->rate = sample->rate;
    voice->start_ms = now_ms();
    return v;
}

static void stream_refill(Voice * voice) {
    int got = stb_vorbis_get_samples_short_interleaved(
            voice->vorbis, voice->channels, voice->stream_buf,
            STREAM_CHUNK * voice->channels);
    if (got <= 0 && voice->loop) {
        stb_vorbis_seek_start(voice->vorbis);
        got = stb_vorbis_get_samples_short_interleaved(
                voice->vorbis, voice->channels, voice->stream_buf,
                STREAM_CHUNK * voice->channels);
    }
    voice->stream_frames = got > 0 ? got : 0;
    voice->eof = got <= 0;
}

// Mix one voice into acc (stereo int32). Returns 0 once the voice finished.
static int voice_mix(Voice * voice, int32_t * acc, int frames) {
    double step = (double) voice->rate * voice->rate_mul / MIX_RATE;
    int vol = (int) (voice->volume * 256.0f);
    if (vol < 0) vol = 0;
    if (vol > 256) vol = 256;

    for (int f = 0; f < frames; f++) {
        const int16_t * src;
        int avail;

        if (voice->sample) {
            src = voice->sample->pcm;
            avail = voice->sample->frames;
        } else {
            if ((int) voice->pos >= voice->stream_frames) {
                voice->pos -= voice->stream_frames;
                stream_refill(voice);
                if (voice->eof)
                    return 0;
            }
            src = voice->stream_buf;
            avail = voice->stream_frames;
        }

        int i = (int) voice->pos;
        if (i >= avail)
            return 0;

        int l, r;
        if (voice->channels == 2) {
            l = src[i * 2];
            r = src[i * 2 + 1];
        } else {
            l = r = src[i];
        }

        acc[f * 2]     += (l * vol) >> 8;
        acc[f * 2 + 1] += (r * vol) >> 8;
        voice->pos += step;
    }
    return 1;
}

static int audio_thread(unsigned int args, void * argp) {
    static int32_t acc[MIX_GRAIN * 2];
    static int16_t out[2][MIX_GRAIN * 2];
    int buf = 0;

    for (;;) {
        memset(acc, 0, sizeof(acc));

        pthread_mutex_lock(&s_lock);
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice * voice = &s_voices[v];
            if (!voice->active || voice->paused)
                continue;
            if (!voice_mix(voice, acc, MIX_GRAIN)) {
                int id = voice->sound_id;
                if (id >= 0 && id < NUM_SOUNDS && s_music[id].voice == v)
                    s_music[id].state = MS_COMPLETED; // bp.onCompletion()
                voice_release(v);
            }
        }
        pthread_mutex_unlock(&s_lock);

        for (int i = 0; i < MIX_GRAIN * 2; i++) {
            int s = acc[i];
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out[buf][i] = (int16_t) s;
        }

        sceAudioOutOutput(s_port, out[buf]);
        buf ^= 1;
    }
    return 0;
}

void audio_init(void) {
    for (int i = 0; i < NUM_SOUNDS; i++) {
        s_sfx_voice[i] = -1;
        s_music[i].voice = -1;
        s_samples[i].duration_ms = -1;
    }
    for (int v = 0; v < MAX_VOICES; v++)
        s_voices[v].sound_id = -1;

    // GLMediaPlayer.init() (Java, run right after nativeInit()) always ends
    // with destroySoundPool(); initSoundPoolArray(); -- i.e. `in = true`.
    s_pool_ready = 1;

    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, MIX_GRAIN,
                                 MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) {
        l_error("audio: sceAudioOutOpenPort failed: 0x%08x -- no sound.", s_port);
        return;
    }
    int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);

    SceUID thid = sceKernelCreateThread("bia2_audio", audio_thread, 0x40,
                                        64 * 1024, 0, 0, NULL);
    if (thid < 0) {
        l_error("audio: mixer thread creation failed: 0x%08x", thid);
        return;
    }
    sceKernelStartThread(thid, 0, NULL);
    l_success("audio: mixer up (%d Hz, %d voices).", MIX_RATE, MAX_VOICES);
}

/* ---- SoundPool side ---- */

static void sample_free_locked(int id) {
    Sample * sample = &s_samples[id];
    if (s_sfx_voice[id] >= 0)
        voice_release(s_sfx_voice[id]);
    // A streamed voice may still reference nothing of the sample; decoded
    // voices point at sample->pcm, so stop any of them before freeing.
    for (int v = 0; v < MAX_VOICES; v++)
        if (s_voices[v].active && s_voices[v].sample == sample)
            voice_release(v);
    free(sample->pcm);
    sample->pcm = NULL;
    sample->loaded = 0;
}

static void sample_load(int id) {
    if (!s_pool_ready || s_samples[id].loaded)
        return;

    char path[256];
    sound_path(id, path, sizeof(path));
    uint64_t t0 = now_ms();

    int err = 0;
    stb_vorbis * vorbis = stb_vorbis_open_filename(path, &err, NULL);
    if (!vorbis) {
        l_warn("audio: loadSound(%d): can't open %s (error %d)", id, path, err);
        return;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    int channels = info.channels > 1 ? 2 : 1;
    int frames = (int) stb_vorbis_stream_length_in_samples(vorbis);
    int16_t * pcm = NULL;

    if (frames > 0 && (size_t) frames * channels * 2 <= MAX_DECODED_BYTES) {
        pcm = malloc((size_t) frames * channels * 2);
        if (pcm) {
            int got = stb_vorbis_get_samples_short_interleaved(
                    vorbis, channels, pcm, frames * channels);
            frames = got > 0 ? got : 0;
        }
    }
    stb_vorbis_close(vorbis);

    pthread_mutex_lock(&s_lock);
    Sample * sample = &s_samples[id];
    sample->pcm = pcm;
    sample->frames = frames;
    sample->channels = channels;
    sample->rate = info.sample_rate;
    if (sample->duration_ms < 0 && info.sample_rate > 0)
        sample->duration_ms = (int) ((int64_t) frames * 1000 / info.sample_rate);
    sample->loaded = 1;
    pthread_mutex_unlock(&s_lock);

    l_debug("audio: loadSound(%d): %d frames @ %d Hz x%d, %s, %llu ms", id, frames,
            info.sample_rate, channels, pcm ? "decoded" : "streamed",
            (unsigned long long) (now_ms() - t0));
}

jint GLMediaPlayer_isSoundLoaded(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return -1;
    return (s_pool_ready && s_samples[snd].loaded) ? 0 : -1;
}

void GLMediaPlayer_loadSound(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (valid_id(snd))
        sample_load(snd);
}

void GLMediaPlayer_unloadSound(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd) || !s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    sample_free_locked(snd);
    pthread_mutex_unlock(&s_lock);
}

jint GLMediaPlayer_getSoundReady(jmethodID id, va_list args) {
    // Java: hZ[i] = (hZ[i] + 1) % hS, with hS = 3
    // (nativeGetTotalSoundsOfSameInstance()).
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return 0;
    s_sound_ready[snd] = (s_sound_ready[snd] + 1) % 3;
    return s_sound_ready[snd];
}

void GLMediaPlayer_registerSoundFile(jmethodID id, va_list args) {
    // Java's init() registers every id itself as sounds/sound_%04d; the fixed
    // naming is reproduced in sound_path().
}

void GLMediaPlayer_playSound(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    float vol = (float) va_arg(args, double);
    (void) va_arg(args, double); // pitch: no effect on Android, see audio.h

    if (!valid_id(snd) || !s_pool_ready)
        return;
    if (!s_samples[snd].loaded)
        sample_load(snd);
    if (!s_samples[snd].loaded)
        return;

    pthread_mutex_lock(&s_lock);
    if (s_sfx_voice[snd] >= 0)
        voice_release(s_sfx_voice[snd]);
    s_sfx_voice[snd] = voice_start_sample(snd, vol);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_pauseSound(jmethodID id, va_list args) {
    // ac.pause() is empty on Android.
}

void GLMediaPlayer_resumeSound(jmethodID id, va_list args) {
    // ac.resume() is empty on Android.
}

void GLMediaPlayer_stopSound(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd) || !s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    if (s_sfx_voice[snd] >= 0)
        voice_release(s_sfx_voice[snd]);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_setVolume(jmethodID id, va_list args) {
    // GLMediaPlayer.setVolume() returns early whenever the pool is up
    // (`if (in) return;`), i.e. always in practice.
}

void GLMediaPlayer_setPitch(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    (void) va_arg(args, int); // instance: only hY[i][0] is ever played
    float rate = (float) va_arg(args, double);
    if (!valid_id(snd) || !s_pool_ready)
        return;
    // SoundPool clamps setRate() to [0.5, 2.0].
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    pthread_mutex_lock(&s_lock);
    if (s_sfx_voice[snd] >= 0)
        s_voices[s_sfx_voice[snd]].rate_mul = rate;
    pthread_mutex_unlock(&s_lock);
}

jboolean GLMediaPlayer_isSoundPlaying(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd) || !s_pool_ready || !s_samples[snd].loaded)
        return JNI_FALSE;
    pthread_mutex_lock(&s_lock);
    int v = s_sfx_voice[snd];
    int playing = v >= 0 && s_voices[v].active
                  && now_ms() <= s_voices[v].start_ms + (uint64_t) (s_samples[snd].duration_ms > 0 ? s_samples[snd].duration_ms : 0);
    pthread_mutex_unlock(&s_lock);
    return playing ? JNI_TRUE : JNI_FALSE;
}

static void stop_all_sfx_locked(void) {
    for (int i = 0; i < NUM_SOUNDS; i++)
        if (s_sfx_voice[i] >= 0)
            voice_release(s_sfx_voice[i]);
}

void GLMediaPlayer_stopAllSFX(jmethodID id, va_list args) {
    if (!s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    stop_all_sfx_locked();
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_destroySoundPool(jmethodID id, va_list args) {
    if (!s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    stop_all_sfx_locked();
    for (int i = 0; i < NUM_SOUNDS; i++)
        if (s_samples[i].loaded)
            sample_free_locked(i);
    s_pool_ready = 0;
    pthread_mutex_unlock(&s_lock);
    l_info("audio: sound pool destroyed.");
}

void GLMediaPlayer_initSoundPoolArray(jmethodID id, va_list args) {
    if (s_pool_ready)
        return;
    s_pool_ready = 1;
    l_info("audio: sound pool ready.");
}

/* ---- GLAsyncPlayer (voices) ---- */

void GLMediaPlayer_playSoundAsync(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    float vol = (float) va_arg(args, double);
    if (!valid_id(snd))
        return;
    pthread_mutex_lock(&s_lock);
    if (s_async_voice >= 0)
        voice_release(s_async_voice);
    s_async_voice = voice_start_stream(snd, vol, 0);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_stopVoice(jmethodID id, va_list args) {
    pthread_mutex_lock(&s_lock);
    if (s_async_voice >= 0)
        voice_release(s_async_voice);
    pthread_mutex_unlock(&s_lock);
}

jint GLMediaPlayer_getVoiceDuration(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return -1;
    return s_samples[snd].duration_ms;
}

/* ---- MediaPlayer side (music) ---- */

static int music_playable(int state) {
    return state == MS_PREPARED || state == MS_PLAYING
           || state == MS_PAUSED || state == MS_COMPLETED;
}

static void music_stop_locked(int snd) {
    Music * m = &s_music[snd];
    if (m->voice >= 0)
        voice_release(m->voice);
    m->state = MS_STOPPED;
}

void GLMediaPlayer_loadMusic(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return;
    pthread_mutex_lock(&s_lock);
    Music * m = &s_music[snd];
    if (m->state == MS_NONE || m->state == MS_RELEASED || m->state == MS_STOPPED)
        m->state = MS_PREPARED;
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_unloadMusic(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return;
    pthread_mutex_lock(&s_lock);
    if (s_music[snd].voice >= 0)
        voice_release(s_music[snd].voice);
    s_music[snd].state = MS_RELEASED;
    pthread_mutex_unlock(&s_lock);
}

jint GLMediaPlayer_isMusicLoaded(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return -1;
    int st = s_music[snd].state;
    // Java: hD != null && dE not in {0, 1, 4, 7, 2} ? 0 : -1
    return (st != MS_NONE && st != MS_RELEASED && st != MS_PREPARING
            && st != MS_STOPPED && st != 2) ? 0 : -1;
}

static void music_play_locked(int snd, float vol, int loop) {
    Music * m = &s_music[snd];
    if (m->state == MS_NONE || m->state == MS_RELEASED || m->state == MS_STOPPED)
        m->state = MS_PREPARED;
    if (!music_playable(m->state))
        return;

    m->volume = vol;
    m->loop = loop;

    if (m->state == MS_PAUSED && m->voice >= 0) {
        s_voices[m->voice].paused = 0;
    } else if (m->state == MS_PLAYING && m->voice >= 0) {
        // MediaPlayer.start() on a playing player keeps playing.
    } else {
        // Prepared, or completed: start() plays from the beginning.
        if (m->voice >= 0)
            voice_release(m->voice);
        m->voice = voice_start_stream(snd, vol, loop);
        if (m->voice < 0)
            return;
    }

    s_voices[m->voice].volume = vol;
    s_voices[m->voice].loop = loop;
    m->state = MS_PLAYING;
}

void GLMediaPlayer_playMusic(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    float vol = (float) va_arg(args, double);
    int loop = va_arg(args, int);
    if (!valid_id(snd) || !s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    music_play_locked(snd, vol, loop > 0);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_pauseMusic(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return;
    pthread_mutex_lock(&s_lock);
    Music * m = &s_music[snd];
    if (m->state == MS_PLAYING || m->state == MS_PAUSED) {
        if (m->voice >= 0)
            s_voices[m->voice].paused = 1;
        m->state = MS_PAUSED;
    }
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_resumeMusic(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    float vol = (float) va_arg(args, double);
    if (!valid_id(snd) || !s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    if (s_music[snd].state == MS_PAUSED)
        music_play_locked(snd, vol, s_music[snd].loop);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_resumeAllMusic(jmethodID id, va_list args) {
    float vol = (float) va_arg(args, double);
    if (!s_pool_ready)
        return;
    pthread_mutex_lock(&s_lock);
    for (int i = NUM_SOUNDS - 1; i >= 0; i--) {
        int st = s_music[i].state;
        if (st == MS_PAUSED || st == MS_PLAYING)
            music_play_locked(i, vol, s_music[i].loop);
    }
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_stopMusic(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return;
    pthread_mutex_lock(&s_lock);
    if (music_playable(s_music[snd].state))
        music_stop_locked(snd);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_stopAllMusic(jmethodID id, va_list args) {
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < NUM_SOUNDS; i++)
        if (music_playable(s_music[i].state))
            music_stop_locked(i);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_stopAllSounds(jmethodID id, va_list args) {
    pthread_mutex_lock(&s_lock);
    if (s_pool_ready)
        stop_all_sfx_locked();
    for (int i = 0; i < NUM_SOUNDS; i++)
        if (music_playable(s_music[i].state))
            music_stop_locked(i);
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_setVolumeMusic(jmethodID id, va_list args) {
    float vol = (float) va_arg(args, double);
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < NUM_SOUNDS; i++) {
        if (s_music[i].state == MS_NONE || s_music[i].state == MS_RELEASED)
            continue;
        s_music[i].volume = vol;
        if (s_music[i].voice >= 0)
            s_voices[s_music[i].voice].volume = vol;
    }
    pthread_mutex_unlock(&s_lock);
}

void GLMediaPlayer_resetSound(jmethodID id, va_list args) {
    // MediaPlayer.seekTo(0) on a prepared/playing/paused/completed player.
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return;
    pthread_mutex_lock(&s_lock);
    Music * m = &s_music[snd];
    if (music_playable(m->state) && m->voice >= 0) {
        Voice * voice = &s_voices[m->voice];
        stb_vorbis_seek_start(voice->vorbis);
        voice->stream_frames = 0;
        voice->pos = 0;
    }
    pthread_mutex_unlock(&s_lock);
}

jint GLMediaPlayer_isMediaPlaying(jmethodID id, va_list args) {
    int snd = va_arg(args, int);
    if (!valid_id(snd))
        return 0;
    pthread_mutex_lock(&s_lock);
    Music * m = &s_music[snd];
    int playing = m->state == MS_PLAYING && m->voice >= 0
                  && !s_voices[m->voice].paused;
    pthread_mutex_unlock(&s_lock);
    return playing;
}

void GLMediaPlayer_update(jmethodID id, va_list args) {
    // Empty in GLMediaPlayer.java.
}

void GLMediaPlayer_resume(jmethodID id, va_list args) {
    // Java re-inits the pool and reloads what suspend() tore down; this port
    // never suspends the pool, so re-arming it is all that is left.
    s_pool_ready = 1;
}
