/**
 * @file  video_avc.c
 * @brief Intro MP4 (H.264 + AAC) sin convertir: demux MP4 propio + decoder
 *        H.264 por HARDWARE directo (sceAvcdec / SceVideodec) + AAC por
 *        software (libavcodec del SDK).
 *
 * Por qué no SceAvPlayer (video.cpp, el de Dungeon-Hunter-2-vita): con
 * GLVideo.mp4 (H.264 Constrained Baseline 640x360 @ 60 fps, AAC 44.1 kHz)
 * SceAvPlayer lee el archivo, pide sus buffers y se detiene en STATE_STOP con
 * 0 frames (logs 0016-0018, ya con memoria PHYCONT OK). DH2 reprodujo su
 * intro porque lo re-encodeó a 24 fps / 48 kHz (intro_orig_1280x720.mp4 ->
 * intro.mp4). Asphalt 5 / GT Racing decodifican por software con FFmpeg,
 * pero el libavcodec del vitasdk NO trae decoder H.264 (solo mpeg4/aac...).
 * sceAvcdec es el decoder hardware que usa Moonlight-Vita a 60 fps: decodifica
 * AU por AU, sin límite de fps del contenedor.
 *
 * Video: cada sample MP4 (NALs con prefijo de longitud, avcC) se pasa a
 * Annex-B (00 00 00 01) con SPS/PPS delante del primero, y el decoder escribe
 * RGBA8888 DIRECTO en la memoria de una textura vitaGL (CDRAM, contigua y ya
 * mapeada en GXM) -> sin copia. 3 texturas en rotación.
 * Audio: AAC -> float planar -> s16 intercalado -> ring -> hilo con
 * sceAudioOut VOICE (44.1 kHz; el BGM lo usa el juego).
 * Sincronía: reloj de pared desde el primer frame; si el video se atrasa más
 * de 2 frames se decodifica sin dibujar (el decoder necesita todas las AU).
 */

#include "video_avc.h"
#include "utils/logger.h"
#include "utils/glutil.h"

#include <psp2/videodec.h>
#include <psp2/sysmodule.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/gxm.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static inline uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// ---------------------------------------------------------------------------
// Demux MP4 (moov completo en memoria; GLVideo.mp4 lo tiene al final, ~40 KB)
// ---------------------------------------------------------------------------
typedef struct {
    int present;
    uint32_t timescale;
    uint32_t count;
    uint64_t *off;
    uint32_t *size;
    uint64_t *time;      // en unidades de timescale (dts; baseline sin B-frames)
    uint32_t max_size;
    // video
    int width, height, nal_len;
    uint8_t *spspps;     // SPS+PPS en Annex-B
    uint32_t spspps_len;
    // audio
    int channels, rate;
    uint8_t asc[2];      // AudioSpecificConfig (AAC-LC) armado desde stsd
} Track;

typedef struct {
    const uint8_t *p, *end;
    uint32_t type;
} Box;

static int box_next(const uint8_t **cur, const uint8_t *end, Box *b) {
    const uint8_t *p = *cur;
    if (end - p < 8)
        return 0;
    uint64_t size = be32(p);
    uint32_t hdr = 8;
    if (size == 1) {
        if (end - p < 16)
            return 0;
        size = be64(p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = end - p;
    }
    if (size < hdr || size > (uint64_t)(end - p))
        return 0;
    b->type = be32(p + 4);
    b->p = p + hdr;
    b->end = p + size;
    *cur = p + size;
    return 1;
}

static int find_child(const uint8_t *p, const uint8_t *end, uint32_t type, Box *out) {
    Box b;
    while (box_next(&p, end, &b))
        if (b.type == type) {
            *out = b;
            return 1;
        }
    return 0;
}

static void parse_avcC(const uint8_t *p, const uint8_t *end, Track *t) {
    if (end - p < 7)
        return;
    t->nal_len = (p[4] & 3) + 1;
    uint8_t buf[1024];
    uint32_t n = 0;
    const uint8_t *q = p + 5;
    for (int pass = 0; pass < 2; pass++) {
        if (q >= end)
            break;
        int cnt = pass == 0 ? (*q & 0x1f) : *q;
        q++;
        for (int i = 0; i < cnt && q + 2 <= end; i++) {
            uint32_t len = be16(q);
            q += 2;
            if (q + len > end || n + 4 + len > sizeof(buf))
                return;
            buf[n++] = 0; buf[n++] = 0; buf[n++] = 0; buf[n++] = 1;
            memcpy(buf + n, q, len);
            n += len;
            q += len;
        }
    }
    t->spspps = malloc(n);
    if (t->spspps) {
        memcpy(t->spspps, buf, n);
        t->spspps_len = n;
    }
}

static int parse_stbl(const uint8_t *p, const uint8_t *end, Track *t, int is_video) {
    Box stsd, stts, stsc, stsz, stco;
    int co64 = 0;
    if (!find_child(p, end, FOURCC('s', 't', 's', 'd'), &stsd) ||
        !find_child(p, end, FOURCC('s', 't', 't', 's'), &stts) ||
        !find_child(p, end, FOURCC('s', 't', 's', 'c'), &stsc) ||
        !find_child(p, end, FOURCC('s', 't', 's', 'z'), &stsz))
        return 0;
    if (!find_child(p, end, FOURCC('s', 't', 'c', 'o'), &stco)) {
        if (!find_child(p, end, FOURCC('c', 'o', '6', '4'), &stco))
            return 0;
        co64 = 1;
    }

    // stsd: primera sample entry
    const uint8_t *e = stsd.p + 8;
    if (stsd.end - e < 16)
        return 0;
    uint32_t esize = be32(e), fmt = be32(e + 4);
    const uint8_t *eend = e + esize <= stsd.end ? e + esize : stsd.end;
    if (is_video) {
        if (fmt != FOURCC('a', 'v', 'c', '1') && fmt != FOURCC('a', 'v', 'c', '3')) {
            l_error("video_avc: codec de video 0x%08x no es H.264", (unsigned)fmt);
            return 0;
        }
        if (eend - e < 8 + 78)
            return 0;
        t->width = be16(e + 8 + 24);
        t->height = be16(e + 8 + 26);
        Box avcC;
        if (find_child(e + 8 + 78, eend, FOURCC('a', 'v', 'c', 'C'), &avcC))
            parse_avcC(avcC.p, avcC.end, t);
        if (!t->spspps)
            return 0;
    } else {
        if (fmt != FOURCC('m', 'p', '4', 'a') || eend - e < 8 + 28)
            return 0;
        t->channels = be16(e + 8 + 16);
        t->rate = be32(e + 8 + 24) >> 16;
        static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                     22050, 16000, 12000, 11025, 8000, 7350 };
        int fi = 4;
        for (int i = 0; i < 13; i++)
            if (rates[i] == t->rate) fi = i;
        // AAC-LC (2) | freq idx | canales
        t->asc[0] = (uint8_t)((2 << 3) | (fi >> 1));
        t->asc[1] = (uint8_t)(((fi & 1) << 7) | ((t->channels & 0xf) << 3));
    }

    // stsz
    uint32_t fixed = be32(stsz.p + 4);
    t->count = be32(stsz.p + 8);
    if (!t->count)
        return 0;
    t->off = malloc(t->count * sizeof(uint64_t));
    t->size = malloc(t->count * sizeof(uint32_t));
    t->time = malloc(t->count * sizeof(uint64_t));
    if (!t->off || !t->size || !t->time)
        return 0;
    for (uint32_t i = 0; i < t->count; i++) {
        t->size[i] = fixed ? fixed : be32(stsz.p + 12 + i * 4);
        if (t->size[i] > t->max_size)
            t->max_size = t->size[i];
    }

    // stts -> time
    uint32_t n = be32(stts.p + 4), s = 0;
    uint64_t tm = 0;
    for (uint32_t i = 0; i < n && s < t->count; i++) {
        uint32_t cnt = be32(stts.p + 8 + i * 8), delta = be32(stts.p + 12 + i * 8);
        for (uint32_t k = 0; k < cnt && s < t->count; k++, s++) {
            t->time[s] = tm;
            tm += delta;
        }
    }
    for (; s < t->count; s++)
        t->time[s] = tm;

    // stsc + stco -> offsets
    uint32_t nchunks = be32(stco.p + 4);
    uint32_t nstsc = be32(stsc.p + 4);
    s = 0;
    for (uint32_t i = 0; i < nstsc && s < t->count; i++) {
        uint32_t first = be32(stsc.p + 8 + i * 12);
        uint32_t spc = be32(stsc.p + 12 + i * 12);
        uint32_t last = (i + 1 < nstsc) ? be32(stsc.p + 8 + (i + 1) * 12) : nchunks + 1;
        for (uint32_t c = first; c < last && c <= nchunks && s < t->count; c++) {
            uint64_t o = co64 ? be64(stco.p + 8 + (c - 1) * 8) : be32(stco.p + 8 + (c - 1) * 4);
            for (uint32_t k = 0; k < spc && s < t->count; k++, s++) {
                t->off[s] = o;
                o += t->size[s];
            }
        }
    }
    if (s < t->count)
        t->count = s;
    t->present = t->count > 0;
    return t->present;
}

static void parse_moov(const uint8_t *p, const uint8_t *end, Track *v, Track *a) {
    Box trak;
    while (box_next(&p, end, &trak)) {
        if (trak.type != FOURCC('t', 'r', 'a', 'k'))
            continue;
        Box mdia, mdhd, hdlr, minf, stbl;
        if (!find_child(trak.p, trak.end, FOURCC('m', 'd', 'i', 'a'), &mdia) ||
            !find_child(mdia.p, mdia.end, FOURCC('m', 'd', 'h', 'd'), &mdhd) ||
            !find_child(mdia.p, mdia.end, FOURCC('h', 'd', 'l', 'r'), &hdlr) ||
            !find_child(mdia.p, mdia.end, FOURCC('m', 'i', 'n', 'f'), &minf) ||
            !find_child(minf.p, minf.end, FOURCC('s', 't', 'b', 'l'), &stbl))
            continue;
        uint32_t ts = mdhd.p[0] == 1 ? be32(mdhd.p + 20) : be32(mdhd.p + 12);
        uint32_t handler = be32(hdlr.p + 8);
        if (handler == FOURCC('v', 'i', 'd', 'e') && !v->present) {
            v->timescale = ts;
            parse_stbl(stbl.p, stbl.end, v, 1);
        } else if (handler == FOURCC('s', 'o', 'u', 'n') && !a->present) {
            a->timescale = ts;
            parse_stbl(stbl.p, stbl.end, a, 0);
        }
    }
}

static void track_free(Track *t) {
    free(t->off);
    free(t->size);
    free(t->time);
    free(t->spspps);
    memset(t, 0, sizeof(*t));
}

static int mp4_load(SceUID fd, Track *v, Track *a) {
    SceOff fsize = sceIoLseek(fd, 0, SCE_SEEK_END);
    uint64_t pos = 0;
    uint8_t hdr[16];
    while (pos + 8 <= (uint64_t)fsize) {
        if (sceIoPread(fd, hdr, 16, pos) < 8)
            return 0;
        uint64_t size = be32(hdr);
        uint32_t type = be32(hdr + 4), h = 8;
        if (size == 1) { size = be64(hdr + 8); h = 16; }
        else if (size == 0) size = fsize - pos;
        if (size < h)
            return 0;
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            uint32_t len = (uint32_t)(size - h);
            uint8_t *moov = malloc(len);
            if (!moov)
                return 0;
            if (sceIoPread(fd, moov, len, pos + h) != (int)len) {
                free(moov);
                return 0;
            }
            parse_moov(moov, moov + len, v, a);
            free(moov);
            return v->present;
        }
        pos += size;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Audio: ring s16 intercalado + hilo sceAudioOut
// ---------------------------------------------------------------------------
#define AUDIO_GRAIN 1024
#define RING_SAMPLES (44100 * 2 * 2)   // ~2 s estéreo

static int16_t *g_ring;
static volatile uint32_t g_ring_w, g_ring_r;   // en samples (int16), monótonos
static volatile int g_audio_quit;
static int g_audio_port = -1;
static int g_audio_ch = 2;

static uint32_t ring_used(void) { return g_ring_w - g_ring_r; }

static void ring_push(const int16_t *s, uint32_t n) {
    while (RING_SAMPLES - ring_used() < n && !g_audio_quit)
        sceKernelDelayThread(1000);
    for (uint32_t i = 0; i < n; i++)
        g_ring[(g_ring_w + i) % RING_SAMPLES] = s[i];
    __sync_synchronize();
    g_ring_w += n;
}

static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int16_t out[AUDIO_GRAIN * 2];
    uint32_t chunk = AUDIO_GRAIN * g_audio_ch;
    for (;;) {
        if (ring_used() >= chunk) {
            for (uint32_t i = 0; i < chunk; i++)
                out[i] = g_ring[(g_ring_r + i) % RING_SAMPLES];
            __sync_synchronize();
            g_ring_r += chunk;
            sceAudioOutOutput(g_audio_port, out);
        } else if (g_audio_quit) {
            break;
        } else {
            sceKernelDelayThread(1000);
        }
    }
    return 0;
}

static void audio_decode_push(AVCodecContext *ctx, AVPacket *pkt, AVFrame *fr, int16_t *tmp) {
    if (avcodec_send_packet(ctx, pkt) < 0)
        return;
    while (avcodec_receive_frame(ctx, fr) == 0) {
        int n = fr->nb_samples, ch = g_audio_ch;
        int srcch = fr->ch_layout.nb_channels > 0 ? fr->ch_layout.nb_channels : 1;
        if (n > 4096)
            n = 4096;
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < ch; c++) {
                int sc = c < srcch ? c : srcch - 1;
                float f;
                switch (fr->format) {
                case AV_SAMPLE_FMT_FLTP: f = ((float *)fr->extended_data[sc])[i]; break;
                case AV_SAMPLE_FMT_FLT:  f = ((float *)fr->extended_data[0])[i * srcch + sc]; break;
                case AV_SAMPLE_FMT_S16P: f = ((int16_t *)fr->extended_data[sc])[i] / 32768.0f; break;
                case AV_SAMPLE_FMT_S16:  f = ((int16_t *)fr->extended_data[0])[i * srcch + sc] / 32768.0f; break;
                default: f = 0.0f; break;
                }
                int v = (int)(f * 32767.0f);
                tmp[i * ch + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        ring_push(tmp, (uint32_t)(n * ch));
    }
}

// ---------------------------------------------------------------------------
// Dibujo: textura RGBA (escrita por el decoder) a pantalla completa 16:9
// ---------------------------------------------------------------------------
static GLuint g_prog;
static GLint g_tex_loc;
#define ATTR_POS 14
#define ATTR_UV 15

// Estado GL que el intro toca. El motor (glitch) cachea el estado GL de su
// lado: si al volver no está EXACTAMENTE como lo dejó, dibuja negro (log
// 0019: bindings de textura/programa/depth cambiados por el video).
typedef struct {
    GLint prog, active_tex, tex0, array_buf, fbo, viewport[4];
    GLboolean blend, depth, cull, scissor;
    GLfloat clear[4];
} GlState;

static void gl_save(GlState *st) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &st->prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &st->active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &st->tex0);
    glActiveTexture((GLenum)st->active_tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &st->array_buf);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &st->fbo);
    glGetIntegerv(GL_VIEWPORT, st->viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, st->clear);
    st->blend = glIsEnabled(GL_BLEND);
    st->depth = glIsEnabled(GL_DEPTH_TEST);
    st->cull = glIsEnabled(GL_CULL_FACE);
    st->scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void gl_restore(const GlState *st) {
    glDisableVertexAttribArray(ATTR_POS);
    glDisableVertexAttribArray(ATTR_UV);
    glUseProgram(st->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, st->tex0);
    glActiveTexture((GLenum)st->active_tex);
    glBindBuffer(GL_ARRAY_BUFFER, st->array_buf);
    glBindFramebuffer(GL_FRAMEBUFFER, st->fbo);
    glViewport(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3]);
    glClearColor(st->clear[0], st->clear[1], st->clear[2], st->clear[3]);
    if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (st->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

static void ensure_prog(void) {
    if (g_prog)
        return;
    static const char *vs_src =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; }\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "uniform sampler2D uTex;\n"
        "varying vec2 vUV;\n"
        "void main() { gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }\n";
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    // Atributos altos: el motor glitch cachea el estado de 0..N y no se
    // enteraría si se los pisamos (pantalla negra tras el intro, log 0019).
    glBindAttribLocation(g_prog, ATTR_POS, "aPos");
    glBindAttribLocation(g_prog, ATTR_UV, "aUV");
    glLinkProgram(g_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    g_tex_loc = glGetUniformLocation(g_prog, "uTex");
}

static void draw_tex(GLuint tex, int vw, int vh, int th) {
    // Letterbox a 960x544 conservando el aspecto del video.
    float sx = 1.0f, sy = 1.0f;
    float va = (float)vw / (float)vh, sa = 960.0f / 544.0f;
    if (va > sa) sy = sa / va; else sx = va / sa;
    float vmax = (float)vh / (float)th;
    const GLfloat pos[] = { -sx, -sy, sx, -sy, -sx, sy, sx, sy };
    const GLfloat uv[] = { 0.0f, vmax, 1.0f, vmax, 0.0f, 0.0f, 1.0f, 0.0f };

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(g_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(g_tex_loc, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(ATTR_POS);
    glEnableVertexAttribArray(ATTR_UV);
    glVertexAttribPointer(ATTR_POS, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(ATTR_UV, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    vglSwapBuffers(GL_FALSE);
}

// ---------------------------------------------------------------------------
// Decoder hardware
// ---------------------------------------------------------------------------
static SceUID alloc_phycont(uint32_t size, void **base) {
    size = ALIGN(size, 0x100000);
    SceUID b = sceKernelAllocMemBlock("vavc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, NULL);
    if (b < 0)
        b = sceKernelAllocMemBlock("vavc_cd", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, ALIGN(size, 0x40000), NULL);
    if (b >= 0)
        sceKernelGetMemBlockBase(b, base);
    return b;
}

#define N_TEX 3

int video_play_avc(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        l_error("video_avc: open %s -> 0x%08X", path, (unsigned)fd);
        return 0;
    }
    Track v, a;
    memset(&v, 0, sizeof(v));
    memset(&a, 0, sizeof(a));
    if (!mp4_load(fd, &v, &a)) {
        l_error("video_avc: %s sin pista H.264 utilizable", path);
        track_free(&v);
        track_free(&a);
        sceIoClose(fd);
        return 0;
    }
    l_success("video_avc: %dx%d, %u frames (ts %u), nal_len %d; audio %s %d Hz %d ch, %u samples",
              v.width, v.height, v.count, v.timescale, v.nal_len,
              a.present ? "AAC" : "no", a.rate, a.channels, a.count);

    int ret = 0;
    uint32_t aw = ALIGN(v.width, 16), ah = ALIGN(v.height, 16);
    SceAvcdecCtrl dec;
    memset(&dec, 0, sizeof(dec));
    SceUID dec_blk = -1, es_blk = -1;
    int lib_ok = 0, dec_ok = 0;
    GLuint tex[N_TEX] = { 0 };
    void *texmem[N_TEX] = { 0 };
    uint8_t *es = NULL;
    uint8_t *sample = NULL;
    AVCodecContext *actx = NULL;
    AVPacket *apkt = NULL;
    AVFrame *afr = NULL;
    int16_t *atmp = NULL;
    SceUID athread = -1;
    GlState gst;
    int gl_saved = 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);

    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof(init));
    init.size = sizeof(init);
    init.horizontal = aw;
    init.vertical = ah;
    init.numOfRefFrames = 5;
    init.numOfStreams = 1;
    int r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (r < 0) {
        l_error("video_avc: sceVideodecInitLibrary -> 0x%08X", (unsigned)r);
        goto out;
    }
    lib_ok = 1;

    SceAvcdecQueryDecoderInfo q = { aw, ah, 5 };
    SceAvcdecDecoderInfo di = { 0 };
    r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &di);
    if (r < 0) {
        l_error("video_avc: sceAvcdecQueryDecoderMemSize -> 0x%08X", (unsigned)r);
        goto out;
    }
    void *dec_mem = NULL;
    dec_blk = alloc_phycont(di.frameMemSize, &dec_mem);
    if (dec_blk < 0) {
        l_error("video_avc: sin memoria para el decoder (%u bytes): 0x%08X", di.frameMemSize, (unsigned)dec_blk);
        goto out;
    }
    dec.frameBuf.pBuf = dec_mem;
    dec.frameBuf.size = di.frameMemSize;
    r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &dec, &q);
    if (r < 0) {
        l_error("video_avc: sceAvcdecCreateDecoder -> 0x%08X", (unsigned)r);
        goto out;
    }
    dec_ok = 1;

    // Buffer ES (Annex-B) en memoria contigua: sample + start codes + SPS/PPS.
    uint32_t es_cap = v.max_size * 2 + v.spspps_len + 1024;
    es_blk = alloc_phycont(es_cap, (void **)&es);
    sample = malloc((v.max_size > a.max_size ? v.max_size : a.max_size) + 64);
    if (es_blk < 0 || !sample) {
        l_error("video_avc: sin memoria para ES/sample (%u)", es_cap);
        goto out;
    }

    // Texturas destino: el decoder escribe RGBA8888 en su memoria vitaGL.
    gl_save(&gst);
    gl_saved = 1;
    ensure_prog();
    glGenTextures(N_TEX, tex);
    for (int i = 0; i < N_TEX; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        texmem[i] = vglGetTexDataPointer(GL_TEXTURE_2D);
        if (!texmem[i]) {
            l_error("video_avc: textura %d sin memoria", i);
            goto out;
        }
    }

    // Audio
    if (a.present) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        actx = codec ? avcodec_alloc_context3(codec) : NULL;
        if (actx) {
            actx->sample_rate = a.rate;
            av_channel_layout_default(&actx->ch_layout, a.channels > 0 ? a.channels : 2);
            actx->extradata = av_mallocz(2 + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(actx->extradata, a.asc, 2);
            actx->extradata_size = 2;
            if (avcodec_open2(actx, codec, NULL) < 0)
                avcodec_free_context(&actx);
        }
        g_audio_ch = a.channels >= 2 ? 2 : 1;
        if (actx) {
            // VOICE: el único puerto BGM ya lo abrió el AudioTrack del juego
            // (port 256) -> BGM daba 0x80260005 (log 0019). VOICE acepta 44.1 kHz.
            SceAudioOutMode mode = g_audio_ch == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO;
            g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_VOICE, AUDIO_GRAIN, a.rate, mode);
            if (g_audio_port < 0)
                g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, a.rate, mode);
            if (g_audio_port >= 0) {
                int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
                sceAudioOutSetVolume(g_audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
            }
            g_ring = malloc(RING_SAMPLES * sizeof(int16_t));
            apkt = av_packet_alloc();
            afr = av_frame_alloc();
            atmp = malloc(4096 * 2 * sizeof(int16_t));
        }
        if (!actx || g_audio_port < 0 || !g_ring || !apkt || !afr || !atmp) {
            l_warn("video_avc: audio deshabilitado (ctx=%p port=0x%08X)", actx, (unsigned)g_audio_port);
            if (g_audio_port >= 0) sceAudioOutReleasePort(g_audio_port);
            g_audio_port = -1;
        } else {
            g_ring_w = g_ring_r = 0;
            g_audio_quit = 0;
            athread = sceKernelCreateThread("intro_audio", audio_thread, 0x10000100, 0x4000, 0, 0, NULL);
            if (athread >= 0)
                sceKernelStartThread(athread, 0, NULL);
        }
    }

    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t old_buttons = pad.buttons;
    uint32_t ai = 0;
    int cur = 0, drawn = 0, skipped_draw = 0, dec_err = 0;
    uint64_t t0 = 0;
    uint64_t frame_us = v.count > 1 ? (v.time[1] - v.time[0]) * 1000000ull / v.timescale : 16667;

    for (uint32_t vi = 0; vi < v.count; vi++) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~old_buttons;
        old_buttons = pad.buttons;
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
            l_info("video_avc: saltado por el usuario en frame %u", vi);
            break;
        }

        uint64_t vt_us = v.time[vi] * 1000000ull / v.timescale;

        // Audio adelantado 300 ms respecto del frame actual.
        if (g_audio_port >= 0) {
            while (ai < a.count && a.time[ai] * 1000000ull / a.timescale <= vt_us + 300000) {
                if (sceIoPread(fd, sample, a.size[ai], a.off[ai]) == (int)a.size[ai]) {
                    apkt->data = sample;
                    apkt->size = a.size[ai];
                    audio_decode_push(actx, apkt, afr, atmp);
                }
                ai++;
            }
        }

        // Sample -> Annex-B
        if (sceIoPread(fd, sample, v.size[vi], v.off[vi]) != (int)v.size[vi])
            break;
        uint32_t n = 0;
        if (vi == 0) {
            memcpy(es, v.spspps, v.spspps_len);
            n = v.spspps_len;
        }
        for (uint32_t p = 0; p + v.nal_len <= v.size[vi];) {
            uint32_t len = 0;
            for (int k = 0; k < v.nal_len; k++)
                len = (len << 8) | sample[p + k];
            p += v.nal_len;
            if (p + len > v.size[vi] || n + 4 + len > es_cap)
                break;
            es[n++] = 0; es[n++] = 0; es[n++] = 0; es[n++] = 1;
            memcpy(es + n, sample + p, len);
            n += len;
            p += len;
        }

        SceAvcdecAu au;
        memset(&au, 0, sizeof(au));
        au.es.pBuf = es;
        au.es.size = n;
        au.pts.upper = au.pts.lower = 0xFFFFFFFF;
        au.dts.upper = au.dts.lower = 0xFFFFFFFF;

        SceAvcdecPicture pic;
        memset(&pic, 0, sizeof(pic));
        pic.size = sizeof(pic);
        pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
        pic.frame.framePitch = aw;
        pic.frame.frameWidth = aw;
        pic.frame.frameHeight = ah;
        pic.frame.pPicture[0] = texmem[cur];
        SceAvcdecPicture *pp = &pic;
        SceAvcdecArrayPicture arr;
        memset(&arr, 0, sizeof(arr));
        arr.numOfElm = 1;
        arr.pPicture = &pp;

        r = sceAvcdecDecode(&dec, &au, &arr);
        if (r < 0) {
            if (dec_err++ < 5)
                l_error("video_avc: sceAvcdecDecode frame %u -> 0x%08X", vi, (unsigned)r);
            if (dec_err > 30)
                break;
            continue;
        }
        if (arr.numOfOutput == 0)
            continue;

        uint64_t now = sceKernelGetProcessTimeWide();
        if (!t0)
            t0 = now - vt_us;
        int64_t ahead = (int64_t)vt_us - (int64_t)(now - t0);
        if (ahead < -(int64_t)(2 * frame_us)) {
            skipped_draw++;   // atrasado: se decodificó pero no se dibuja
            continue;
        }
        if (ahead > 2000)
            sceKernelDelayThread((SceUInt)(ahead - 1000));
        draw_tex(tex[cur], v.width, v.height, ah);
        cur = (cur + 1) % N_TEX;
        if (++drawn == 1)
            l_success("video_avc: primer frame en pantalla");
    }
    l_success("video_avc: fin (%d frames dibujados, %d salteados por atraso, %d errores)",
              drawn, skipped_draw, dec_err);
    ret = 1;   // el decoder arrancó: no caer a SceAvPlayer aunque se salte

out:
    if (athread >= 0) {
        g_audio_quit = 1;
        sceKernelWaitThreadEnd(athread, NULL, NULL);
        sceKernelDeleteThread(athread);
    }
    if (g_audio_port >= 0) {
        sceAudioOutReleasePort(g_audio_port);
        g_audio_port = -1;
    }
    free(g_ring);
    g_ring = NULL;
    free(atmp);
    if (afr) av_frame_free(&afr);
    if (apkt) av_packet_free(&apkt);
    if (actx) avcodec_free_context(&actx);
    glFinish();
    for (int i = 0; i < N_TEX; i++)
        if (tex[i]) glDeleteTextures(1, &tex[i]);
    if (gl_saved) {
        // Un frame negro limpio antes de devolverle el control al motor.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);
        gl_restore(&gst);
    }
    if (dec_ok) sceAvcdecDeleteDecoder(&dec);
    if (lib_ok) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    if (es_blk >= 0) sceKernelFreeMemBlock(es_blk);
    if (dec_blk >= 0) sceKernelFreeMemBlock(dec_blk);
    free(sample);
    track_free(&v);
    track_free(&a);
    sceIoClose(fd);
    return ret;
}
