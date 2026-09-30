/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "watchdog.h"
#include "input.h"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

static void gl_init_downsample(void);

void gl_init() {
    // No MSAA: the engine renders into its own OES FBOs (LibEffects bloom /
    // motion blur), and MSAA + FBOs is a risky combination in vitaGL.
    // Dejamos PHYCONT libre (0x7FFFFFFF) para que sceVideodecInitLibrary /
    // sceAvcdecCreateDecoder y SceAvPlayer puedan alojar páginas físicas
    // continuas sin fallar con 0x80024302 (SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE).
    // Tras el video intro, gl_provide_phycont() le entrega el remanente a vitaGL.
    // Cached RAM pool (N.O.V.A 2): the engine streams every vertex array
    // through vitaGL's RAM pool; uncached writes are the slow path.
    vglUseCachedMem(GL_TRUE);
    vglInitWithCustomThreshold(0, REAL_SCREEN_W, REAL_SCREEN_H, 12 * 1024 * 1024,
                               0, 0x7FFFFFFF,
                               0x8C6000,
                               SCE_GXM_MULTISAMPLE_NONE);

    /*
     * The shipped data is the ATC pack (res/raw/data.txt: "..._ATC.zip");
     * Texture::LoadATCFromBuffer uploads GL_ATC_RGB_AMD /
     * GL_ATC_RGBA_EXPLICIT_ALPHA_AMD straight to glCompressedTexImage2D.
     * vitaGL decodes ATC in software; with this hint it re-encodes the result
     * to DXT1/DXT5 (same 4/8 bpp as ATC) instead of keeping RGBA8888, which
     * would multiply texture memory by 4-8x.
     */
    glHint(GL_TEXTURE_COMPRESSION_HINT, GL_NICEST);

    gl_init_downsample();
}

static GLuint s_ds_fbo = 0;
static GLuint s_ds_color_tex = 0;
static GLuint s_ds_depth_rb = 0;

/*
 * What the engine currently has bound (its own addressing: 0 = default).
 * Unlike the sibling Asphalt 5 port -- whose engine never binds a
 * framebuffer of its own -- libbia2.so DOES (LibEffects bloom/motion blur
 * render into their own OES FBOs, e.g. a fixed 1024x512 target), so an
 * engine bind of 0 is redirected to s_ds_fbo below while non-zero binds go
 * to the engine's real FBOs untouched.
 */
static GLuint s_engine_fb = 0;

static void gl_init_downsample(void) {
    glGenTextures(1, &s_ds_color_tex);
    glBindTexture(GL_TEXTURE_2D, s_ds_color_tex);
    // GL_RGBA, not GL_RGB -- GL_RGBA8 is the one format every GLES
    // implementation is required to support as an FBO color attachment.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OFFSCREEN_W, OFFSCREEN_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &s_ds_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s_ds_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, OFFSCREEN_W, OFFSCREEN_H);

    glGenFramebuffers(1, &s_ds_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_ds_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ds_color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_ds_depth_rb);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        l_error("Downsample FBO incomplete (0x%04x) -- rendering straight to "
                "the real framebuffer instead.", status);
        glDeleteFramebuffers(1, &s_ds_fbo);
        s_ds_fbo = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    l_success("Downsample FBO ready: %dx%d (reported %dx%d, native %dx%d).",
              OFFSCREEN_W, OFFSCREEN_H, SCREEN_W, SCREEN_H, REAL_SCREEN_W, REAL_SCREEN_H);

    // The color texture was allocated via glTexImage2D(..., NULL) -- its
    // initial content is undefined GPU memory, not guaranteed black. Force
    // a real clear now so the very first gl_swap() blits something
    // deterministic instead of whatever was already sitting in that memory.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Left bound on purpose: the engine's "FB 0" draws land here without it
    // knowing the difference (see glBindFramebuffer_soloader).
}

/*
 * Clamp a rect from the 800x480 space the engine was told the screen is
 * (SCREEN_W/H) into the OFFSCREEN_W/H attachment bounds, so a rect landing
 * exactly on an edge (or meant for one of LibEffects' own render targets)
 * can never produce an out-of-range viewport/scissor rect for GXM -- that
 * combination (real framebuffer smaller than the requested rect, or a rect
 * blindly upscaled onto a bigger panel) was the prime suspect for the
 * GPU crash after the sound pool (frame 335, OGL2DFlush/glDrawElements).
 * OFFSCREEN_W/H == SCREEN_W/H, so this is exact (no rounding) for the
 * common full-screen case.
 */
static void clamp_to_offscreen(GLint x, GLint y, GLsizei w, GLsizei h,
                               GLint * out_x, GLint * out_y,
                               GLsizei * out_w, GLsizei * out_h) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    GLint rx = (x * OFFSCREEN_W) / SCREEN_W;
    GLint ry = (y * OFFSCREEN_H) / SCREEN_H;
    GLsizei rw = (w * OFFSCREEN_W) / SCREEN_W;
    GLsizei rh = (h * OFFSCREEN_H) / SCREEN_H;

    if (rx > OFFSCREEN_W) rx = OFFSCREEN_W;
    if (ry > OFFSCREEN_H) ry = OFFSCREEN_H;
    if (rw > OFFSCREEN_W - rx) rw = OFFSCREEN_W - rx;
    if (rh > OFFSCREEN_H - ry) rh = OFFSCREEN_H - ry;

    *out_x = rx;
    *out_y = ry;
    *out_w = rw;
    *out_h = rh;
}

/*
 * The engine's viewport/scissor, in its own terms, re-applied against
 * whatever target is REALLY bound. LibEffects::Manager::BeginRenderTargetGroup
 * calls glViewport(0, 0, rt_w, rt_h) *before* glBindFramebufferOES(rt) (and
 * glViewport(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT) before re-binding 0), so
 * deciding "clamp or not" at glViewport time -- as this file used to --
 * clamped the 1024x512 scene RT's viewport to 800x480 while passing the
 * screen one through untouched. Keeping the raw rects and re-resolving them
 * on every bind makes the order irrelevant, like on a real GLES driver.
 */
static GLint s_engine_vp[4] = { 0, 0, SCREEN_W, SCREEN_H };
static GLint s_engine_sc[4] = { 0, 0, SCREEN_W, SCREEN_H };

static void apply_engine_viewport(void) {
    if (s_engine_fb == 0 && s_ds_fbo) {
        GLint rx, ry;
        GLsizei rw, rh;
        clamp_to_offscreen(s_engine_vp[0], s_engine_vp[1], s_engine_vp[2], s_engine_vp[3],
                           &rx, &ry, &rw, &rh);
        glViewport(rx, ry, rw, rh);
    } else {
        // Targeting one of the engine's own FBOs (LibEffects RTs are in
        // texture pixels, not screen pixels): pass through untouched.
        glViewport(s_engine_vp[0], s_engine_vp[1], s_engine_vp[2], s_engine_vp[3]);
    }
}

static void apply_engine_scissor(void) {
    if (s_engine_fb == 0 && s_ds_fbo) {
        GLint rx, ry;
        GLsizei rw, rh;
        clamp_to_offscreen(s_engine_sc[0], s_engine_sc[1], s_engine_sc[2], s_engine_sc[3],
                           &rx, &ry, &rw, &rh);
        glScissor(rx, ry, rw, rh);
    } else {
        glScissor(s_engine_sc[0], s_engine_sc[1], s_engine_sc[2], s_engine_sc[3]);
    }
}

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    s_engine_vp[0] = x;
    s_engine_vp[1] = y;
    s_engine_vp[2] = width;
    s_engine_vp[3] = height;
    apply_engine_viewport();
}

void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height) {
    s_engine_sc[0] = x;
    s_engine_sc[1] = y;
    s_engine_sc[2] = width;
    s_engine_sc[3] = height;
    apply_engine_scissor();
}

// Only updated by the engine's own calls (routed here via dynlib.c's import
// table), never by this port's own GL calls, which link the real vitaGL
// entry points instead of these wrappers. GLES1.1 defaults: all disabled.
GLboolean g_shadow_depth_test   = GL_FALSE;
GLboolean g_shadow_blend        = GL_FALSE;
GLboolean g_shadow_scissor_test = GL_FALSE;
GLboolean g_shadow_cull_face    = GL_FALSE;
GLboolean g_shadow_vertex_array = GL_FALSE;

/*
 * GL_TEXTURE_2D per unit, as the ENGINE sees it, plus whether the name bound
 * there is one vitaGL can't hold. DrawFillRect() does
 * OGL2DBindTexture(0xffffffff, 1, 1), so OGL2DFlush -> EnableTexMap ends in
 * glBindTexture(GL_TEXTURE_2D, 0xFFFFFFFF) with texturing enabled. On GLES
 * that's just an incomplete texture: the unit behaves as if disabled and the
 * quad keeps its vertex color (the title screen's translucent panel). vitaGL
 * built with NO_DEBUG skips its `texture >= TEXTURES_NUM` check, stores the
 * id, and the next draw hands texture_slots[0xFFFFFFFF] -- the memory just
 * before the table -- to sceGxmSetFragmentTexture as a descriptor: a garbage
 * texture the GPU never comes back from (Bug #6, stall in vglSwapBuffers on
 * the first engine-state-2 frame, brothersinarms2_005.log). So an
 * out-of-range bind really binds 0 and really disables the unit, while the
 * engine keeps seeing the state it set.
 */
#define VGL_TEXTURES_NUM 16384 // lib/vitagl/source/shared.h TEXTURES_NUM
#define TRACKED_UNITS    8

static GLuint    s_active_unit = 0;
static GLboolean s_tex2d_wanted[TRACKED_UNITS];
static GLboolean s_tex_bad[TRACKED_UNITS];

void glActiveTexture_soloader(GLenum texture) {
    GLuint unit = texture - GL_TEXTURE0;
    if (unit < TRACKED_UNITS)
        s_active_unit = unit;
    glActiveTexture(texture);
}

void glBindTexture_soloader(GLenum target, GLuint texture) {
    if (target != GL_TEXTURE_2D) {
        glBindTexture(target, texture);
        return;
    }
    GLuint u = s_active_unit;
    GLboolean bad = texture >= VGL_TEXTURES_NUM;
    if (bad) {
        static int s_warned = 0;
        if (!s_warned) {
            s_warned = 1;
            l_warn("glBindTexture(GL_TEXTURE_2D, 0x%08x) on unit %u: out of vitaGL's "
                   "range -- treated as an incomplete texture (unit disabled).", texture, u);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        if (s_tex2d_wanted[u])
            glDisable(GL_TEXTURE_2D);
    } else {
        glBindTexture(GL_TEXTURE_2D, texture);
        if (s_tex_bad[u] && s_tex2d_wanted[u])
            glEnable(GL_TEXTURE_2D);
    }
    s_tex_bad[u] = bad;
}

GLboolean glIsEnabled_soloader(GLenum cap) {
    if (cap == GL_TEXTURE_2D)
        return s_tex2d_wanted[s_active_unit];
    return glIsEnabled(cap);
}

void glEnable_soloader(GLenum cap) {
    if (cap == GL_TEXTURE_2D) {
        s_tex2d_wanted[s_active_unit] = GL_TRUE;
        if (s_tex_bad[s_active_unit])
            return;
    }
    switch (cap) {
        case GL_DEPTH_TEST:    g_shadow_depth_test   = GL_TRUE; break;
        case GL_BLEND:         g_shadow_blend        = GL_TRUE; break;
        case GL_SCISSOR_TEST:  g_shadow_scissor_test = GL_TRUE; break;
        case GL_CULL_FACE:     g_shadow_cull_face    = GL_TRUE; break;
        default: break;
    }
    glEnable(cap);
}

void glDisable_soloader(GLenum cap) {
    if (cap == GL_TEXTURE_2D)
        s_tex2d_wanted[s_active_unit] = GL_FALSE;
    switch (cap) {
        case GL_DEPTH_TEST:    g_shadow_depth_test   = GL_FALSE; break;
        case GL_BLEND:         g_shadow_blend        = GL_FALSE; break;
        case GL_SCISSOR_TEST:  g_shadow_scissor_test = GL_FALSE; break;
        case GL_CULL_FACE:     g_shadow_cull_face    = GL_FALSE; break;
        default: break;
    }
    glDisable(cap);
}

void glEnableClientState_soloader(GLenum array) {
    if (array == GL_VERTEX_ARRAY) g_shadow_vertex_array = GL_TRUE;
    glEnableClientState(array);
}

void glDisableClientState_soloader(GLenum array) {
    if (array == GL_VERTEX_ARRAY) g_shadow_vertex_array = GL_FALSE;
    glDisableClientState(array);
}

void glGetIntegerv_soloader(GLenum pname, GLint * data) {
    if (pname == GL_FRAMEBUFFER_BINDING) {
        // The engine thinks in its own FB numbers (0 = default); the real
        // binding underneath may be s_ds_fbo.
        *data = (GLint) s_engine_fb;
        return;
    }
    // The engine reads GL_VIEWPORT back (LibEffects::Manager::TakeScreenShot):
    // hand it its own rect, not the clamped one.
    if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) {
        const GLint * r = (pname == GL_VIEWPORT) ? s_engine_vp : s_engine_sc;
        data[0] = r[0]; data[1] = r[1]; data[2] = r[2]; data[3] = r[3];
        return;
    }
    glGetIntegerv(pname, data);
}

// Bind what the engine believes is bound (engine 0 -> s_ds_fbo) and
// re-resolve its viewport/scissor against that target.
static void bind_engine_target(void) {
    if (s_engine_fb == 0 && s_ds_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_ds_fbo);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, s_engine_fb);
    }
    apply_engine_viewport();
    apply_engine_scissor();
}

void glBindFramebuffer_soloader(GLenum target, GLuint fb) {
    (void) target; // GLES1 OES: GL_FRAMEBUFFER_OES is the only valid target
    s_engine_fb = fb;
    bind_engine_target();
}

// One upscale blit from the downsample FBO onto the real 960x544 screen,
// then present. A plain non-uniform stretch (OFFSCREEN is 5:3, the real
// screen is ~16:9) -- same stretch the old viewport-scaling approach
// produced, so it looks the same, just genuinely cheaper to render (~26%
// fewer pixels rasterized per frame) and with no out-of-range rects.
static void gl_blit_downsample_to_screen() {
    // The engine uses multitexturing all over (SetActiveTextureSlot /
    // g_ClientActiveTexture in the .so), so at swap time the active server
    // and client units are frequently NOT 0. Pin both to TEXTURE0 for the
    // blit: otherwise we'd bind our color texture on the wrong unit while
    // unit 0 keeps a stale COMBINE setup (red flicker on boot) or nothing
    // samplable at all (black title screen).
    GLint old_active, old_client_active;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &old_client_active);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);

    // SAVE STATE (shadow copies where available, real queries otherwise --
    // the engine uses multitexturing, so texture-unit state stays queried).
    GLboolean depth_test = g_shadow_depth_test;
    GLboolean blend = g_shadow_blend;
    GLboolean scissor = g_shadow_scissor_test;
    GLboolean cull = g_shadow_cull_face;
    GLboolean tex2d = glIsEnabled(GL_TEXTURE_2D);

    GLint old_tex;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);

    GLint old_env;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &old_env);

    GLfloat old_color[4];
    glGetFloatv(GL_CURRENT_COLOR, old_color);

    GLboolean v_array = g_shadow_vertex_array;
    GLboolean t_array = glIsEnabled(GL_TEXTURE_COORD_ARRAY);

    // BLIT
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, REAL_SCREEN_W, REAL_SCREEN_H);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s_ds_color_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    static const GLfloat verts[8] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    static const GLfloat uvs[8] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // RESTORE STATE (Bug #15 lesson from Asphalt 5: the blit must not
    // clobber the engine's fixed-function state).
    if (!v_array) glDisableClientState(GL_VERTEX_ARRAY);
    if (!t_array) glDisableClientState(GL_TEXTURE_COORD_ARRAY);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    glColor4f(old_color[0], old_color[1], old_color[2], old_color[3]);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, old_env);
    glBindTexture(GL_TEXTURE_2D, old_tex);

    if (!tex2d) glDisable(GL_TEXTURE_2D);
    if (cull) glEnable(GL_CULL_FACE);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (blend) glEnable(GL_BLEND);
    if (depth_test) glEnable(GL_DEPTH_TEST);
    // Viewport/scissor: re-applied by bind_engine_target() after the swap.

    glActiveTexture(old_active);
    glClientActiveTexture(old_client_active);
}

/*
 * Finer watchdog phases than main.c's "swap": GXM is asynchronous, so a GPU
 * hang poisoned by an earlier draw surfaces in whichever of these waits
 * first -- the phase name in the stall line says which.
 */
void gl_swap() {
    if (s_ds_fbo) {
        watchdog_phase("swap:blit");
        gl_blit_downsample_to_screen();
        cam_osd_draw();
        watchdog_phase("swap:vglSwapBuffers");
        vglSwapBuffers(GL_FALSE);
        // Re-bind what the ENGINE has bound, not unconditionally s_ds_fbo
        // (a swap doesn't change the FBO binding on real GLES).
        // LibEffects::Manager::Begin() binds its scene RT and leaves it bound;
        // if a frame ever ended there, the old code made the engine's
        // FRAMEBUFFER_BINDING (the RT) disagree with the real one (s_ds_fbo),
        // and Begin() would later "restore" the RT as the screen and sample
        // it while rendering into it.
        watchdog_phase("swap:rebind");
        bind_engine_target();
    } else {
        watchdog_phase("swap:vglSwapBuffers");
        vglSwapBuffers(GL_FALSE);
    }
}

void gl_provide_phycont(void) {
    static int s_provided = 0;
    if (s_provided)
        return;
    s_provided = 1;
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    // Se deja 1 MB de margen para el sistema; el pool va en múltiplos de 1 MB.
    size_t sz = info.size_phycont > 0x100000 ? (info.size_phycont - 0x100000) & ~0xFFFFF : 0;
    if (sz)
        vglPhycontMemLazyInit(sz);
    l_success("vitaGL: PHYCONT pool %u KB handed over after intro video", (unsigned)(sz / 1024));
}


/*
 * LibEffects::Manager::BeginRenderTargetGroup(NULL) re-binds the framebuffer
 * it saved (0 = default) and then calls glFramebufferRenderbufferOES on it.
 * On real GLES that is just GL_INVALID_OPERATION; vitaGL built with NO_DEBUG
 * (SKIP_ERROR_HANDLING) skips its `if (!fb)` check and dereferences NULL.
 * Same guard for the texture variant. The check is on the engine's FB number:
 * "0" really means the downsample FBO underneath, which must never gain an
 * engine attachment.
 */
void glFramebufferRenderbuffer_soloader(GLenum target, GLenum attachment,
                                        GLenum rbtarget, GLuint rb) {
    if (s_engine_fb == 0)
        return;
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
}

void glFramebufferTexture2D_soloader(GLenum target, GLenum attachment,
                                     GLenum textarget, GLuint texture, GLint level) {
    if (s_engine_fb == 0)
        return;
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

/*
 * vitaGL only answers GL_COLOR_ATTACHMENT0 queries and returns early without
 * touching *params otherwise (LibEffects::Manager::Begin asks for the depth
 * attachment of the current framebuffer). Report "none" like GLES would.
 */
void glGetFramebufferAttachmentParameteriv_soloader(GLenum target, GLenum attachment,
                                                    GLenum pname, GLint * params) {
    if (attachment != GL_COLOR_ATTACHMENT0 || s_engine_fb == 0) {
        *params = 0;
        return;
    }
    glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}

/*
 * Same root cause as Asphalt 5's Bug #23: glTexImage2D() with an S3TC/DXT
 * internalformat and *uncompressed* pixel data leaves vitaGL's tex->write_cb
 * NULL, routing the upload through gpu_alloc_compressed_texture() ->
 * dxt_compress() -- a real synchronous per-4x4-block software DXT encoder
 * with no time budget, run on the calling thread (up to seconds per upload,
 * triaged there as a "GPU crash" dump with stop reason 0x0). The game's
 * normal assets go through glCompressedTexImage2D (fast path, untouched
 * here); this only catches an uncompressed upload asking for DXT storage
 * (e.g. a render-to-texture result). Remapping to GL_RGBA keeps the caller's
 * raw pixels valid bit-for-bit for a few KB of extra VRAM.
 */
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                           GLsizei width, GLsizei height, GLint border,
                           GLenum format, GLenum type, const void * pixels) {
    switch (internalformat) {
        case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        case GL_COMPRESSED_SRGB_S3TC_DXT1:
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1:
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5:
        case GL_COMPRESSED_SRGB:
        case GL_COMPRESSED_SRGB_ALPHA:
            internalformat = GL_RGBA;
            break;
        default:
            break;
    }
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    load_shader(shader, str, total_length);

    free(str);
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        glCompileShader(shader);
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
    }
    skip_next_compile = GL_FALSE;
#endif
}

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
