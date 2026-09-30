/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Resolution reported to the engine (see main.c) and the real backbuffer.
#define SCREEN_W 800
#define SCREEN_H 480
#define REAL_SCREEN_W 960
#define REAL_SCREEN_H 544

/*
 * Internal size of the offscreen downsample FBO that the engine's "FB 0"
 * draws into (see gl_init()/gl_swap() in glutil.c). Matches SCREEN_W/H
 * exactly, so the engine's 800x480-space glViewport/glScissor rects need no
 * rescaling at all (identity + clamp into the attachment bounds) -- unlike
 * the previous approach of scaling them up onto the real 960x544 panel,
 * which could hand GXM an out-of-range rect (e.g. a viewport meant for one
 * of LibEffects' own render targets still pending when FB 0 is re-bound)
 * and GPU-crash the console. Same technique as the sibling Asphalt 5 port
 * (Bug #8/#10 there): render small, upscale-blit once per frame in gl_swap().
 */
#define OFFSCREEN_W 800
#define OFFSCREEN_H 480

void gl_init();

void gl_preload();

void gl_swap();

void gl_provide_phycont();

void glCompileShader_soloader(GLuint shader);

void glViewport_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glGetIntegerv_soloader(GLenum pname, GLint * data);
void glBindFramebuffer_soloader(GLenum target, GLuint fb);
void glTexImage2D_soloader(GLenum target, GLint level, GLint internalformat,
                            GLsizei width, GLsizei height, GLint border,
                            GLenum format, GLenum type, const void * pixels);

/*
 * Shadow copies of engine-set fixed-function state, updated only by the
 * wrappers below (i.e. only by libbia2.so's own calls via dynlib.c), never
 * by this port's own GL calls. Lets gl_swap()'s blit save/restore without
 * glIsEnabled() round-trips every frame. Same pattern as Asphalt 5.
 */
void glEnable_soloader(GLenum cap);
void glActiveTexture_soloader(GLenum texture);
void glBindTexture_soloader(GLenum target, GLuint texture);
GLboolean glIsEnabled_soloader(GLenum cap);
void glDisable_soloader(GLenum cap);
void glEnableClientState_soloader(GLenum array);
void glDisableClientState_soloader(GLenum array);

extern GLboolean g_shadow_depth_test;
extern GLboolean g_shadow_blend;
extern GLboolean g_shadow_scissor_test;
extern GLboolean g_shadow_cull_face;
extern GLboolean g_shadow_vertex_array;
void glFramebufferRenderbuffer_soloader(GLenum target, GLenum attachment,
                                        GLenum rbtarget, GLuint rb);
void glFramebufferTexture2D_soloader(GLenum target, GLenum attachment,
                                     GLenum textarget, GLuint texture, GLint level);
void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLsizei imageSize, const void * data);
void glGetFramebufferAttachmentParameteriv_soloader(GLenum target, GLenum attachment,
                                                    GLenum pname, GLint * params);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
