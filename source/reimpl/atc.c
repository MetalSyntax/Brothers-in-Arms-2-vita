/*
 * AMD/Qualcomm ATC texture decoding for glCompressedTexImage2D.
 *
 * GLGame.getTextureCompression() is "atc" (GameRenderer.java:124 forces it)
 * and the downloaded .gla packs ship ATC textures ("[ATC] ATC Texture
 * Loading", "[IMAGE-LOADER] == ATC ==" in log 0006). vitaGL only knows
 * DXT/PVRTC/ETC1, so ATC uploads were rejected -> black screen.
 *
 * Block layouts (4x4 texels):
 *   ATC RGB (0x8C92), 8 bytes:
 *     u16 c0: bit15 = mode, RGB555     u16 c1: RGB565     u32 2-bit indices
 *     mode 0: { c0, (5*c0 + 3*c1)/8, (3*c0 + 5*c1)/8, c1 }
 *     mode 1: { 0,  c0 - c1/4,       c0,              c1 }
 *   ATC RGBA explicit alpha (0x8C93), 16 bytes:
 *     8 bytes of 4-bit alpha (same as DXT3), then an ATC RGB block
 *   ATC RGBA interpolated alpha (0x87EE), 16 bytes:
 *     8 bytes alpha block (same as DXT5/BC4), then an ATC RGB block
 *
 * Uploaded as DXT1/DXT3/DXT5 (see glCompressedTexImage2D_soloader).
 */

#include <vitaGL.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger.h"

#define GL_ATC_RGB_AMD                      0x8C92
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD      0x8C93
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD  0x87EE

static inline uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

// Decodes the 8-byte ATC color block into 16 RGB triplets.
static void atc_color_block(const uint8_t *b, uint8_t rgb[16][3]) {
    uint32_t c0 = b[0] | (b[1] << 8);
    uint32_t c1 = b[2] | (b[3] << 8);
    uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);

    int e0[3] = { expand5((c0 >> 10) & 0x1F), expand5((c0 >> 5) & 0x1F), expand5(c0 & 0x1F) };
    int e1[3] = { expand5((c1 >> 11) & 0x1F), expand6((c1 >> 5) & 0x3F), expand5(c1 & 0x1F) };
    int pal[4][3];

    for (int ch = 0; ch < 3; ch++) {
        if (c0 & 0x8000) {
            int v = e0[ch] - e1[ch] / 4;
            pal[0][ch] = 0;
            pal[1][ch] = v < 0 ? 0 : v;
            pal[2][ch] = e0[ch];
            pal[3][ch] = e1[ch];
        } else {
            pal[0][ch] = e0[ch];
            pal[1][ch] = (5 * e0[ch] + 3 * e1[ch]) / 8;
            pal[2][ch] = (3 * e0[ch] + 5 * e1[ch]) / 8;
            pal[3][ch] = e1[ch];
        }
    }

    for (int i = 0; i < 16; i++) {
        int k = (idx >> (2 * i)) & 3;
        rgb[i][0] = pal[k][0];
        rgb[i][1] = pal[k][1];
        rgb[i][2] = pal[k][2];
    }
}

static int is_atc(GLenum fmt) {
    return fmt == GL_ATC_RGB_AMD || fmt == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD ||
           fmt == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD;
}

/*
 * Transcode to S3TC instead of expanding (log 008: ~20 FPS in missions vs
 * 40-60 in log 006, VRAM free 80 -> 55 MB). ATC and DXT share the block
 * layout (u16 c0, u16 c1, 2-bit indices; the alpha halves are bit-identical
 * to DXT3/DXT5), so each ATC block becomes one DXT block: RGB -> DXT1,
 * explicit alpha -> DXT3, interpolated alpha -> DXT5. 4/8 bpp in VRAM and
 * GPU bandwidth instead of 16/32, no upload-time expansion.
 *
 * The colour block is re-fitted against the DXT palette {c0, c1, 2/3, 1/3}:
 * mode 0 keeps ATC's own endpoints (ATC's 5/8-3/8 midpoints land on the
 * nearest DXT third), mode 1 ({0, c0-c1/4, c0, c1}) takes the two farthest
 * colours actually used. c0 > c1 is enforced so DXT1 never falls into its
 * 3-colour/transparent mode.
 */
static inline uint16_t pack565(const uint8_t *c) {
    return (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
}

static inline void unpack565(uint16_t v, int *c) {
    c[0] = expand5((v >> 11) & 0x1F);
    c[1] = expand6((v >> 5) & 0x3F);
    c[2] = expand5(v & 0x1F);
}

static inline int dist2(const int *a, const uint8_t *b) {
    int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

static void atc_to_dxt_color(const uint8_t *b, uint8_t *out) {
    uint8_t rgb[16][3];
    atc_color_block(b, rgb);

    uint32_t c0 = b[0] | (b[1] << 8);
    uint16_t e0, e1;
    if (!(c0 & 0x8000)) {
        // RGB555 -> RGB565
        e0 = (uint16_t)(((c0 & 0x7FE0) << 1) | ((c0 >> 4) & 0x20) | (c0 & 0x1F));
        e1 = (uint16_t)(b[2] | (b[3] << 8));
    } else {
        int best = -1, bi = 0, bj = 0;
        for (int i = 0; i < 16; i++)
            for (int j = i + 1; j < 16; j++) {
                int c[3] = { rgb[i][0], rgb[i][1], rgb[i][2] };
                int d = dist2(c, rgb[j]);
                if (d > best) { best = d; bi = i; bj = j; }
            }
        e0 = pack565(rgb[bi]);
        e1 = pack565(rgb[bj]);
    }
    if (e0 < e1) { uint16_t t = e0; e0 = e1; e1 = t; }

    uint32_t idx = 0;
    if (e0 != e1) {
        int p0[3], p1[3], pal[4][3];
        unpack565(e0, p0);
        unpack565(e1, p1);
        for (int ch = 0; ch < 3; ch++) {
            pal[0][ch] = p0[ch];
            pal[1][ch] = p1[ch];
            pal[2][ch] = (2 * p0[ch] + p1[ch]) / 3;
            pal[3][ch] = (p0[ch] + 2 * p1[ch]) / 3;
        }
        for (int i = 0; i < 16; i++) {
            int bk = 0, bd = dist2(pal[0], rgb[i]);
            for (int k = 1; k < 4; k++) {
                int d = dist2(pal[k], rgb[i]);
                if (d < bd) { bd = d; bk = k; }
            }
            idx |= (uint32_t)bk << (2 * i);
        }
    }
    out[0] = e0 & 0xFF; out[1] = e0 >> 8;
    out[2] = e1 & 0xFF; out[3] = e1 >> 8;
    out[4] = idx & 0xFF; out[5] = (idx >> 8) & 0xFF;
    out[6] = (idx >> 16) & 0xFF; out[7] = idx >> 24;
}

void glCompressedTexImage2D_soloader(GLenum target, GLint level, GLenum internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLsizei imageSize, const void *data) {
    if (!is_atc(internalformat)) {
        glCompressedTexImage2D(target, level, internalformat, width, height,
                               border, imageSize, data);
        return;
    }

    int has_alpha = internalformat != GL_ATC_RGB_AMD;
    int block_bytes = has_alpha ? 16 : 8;
    int bw = (width + 3) / 4, bh = (height + 3) / 4;

    if (!data || imageSize < bw * bh * block_bytes) {
        l_warn("ATC: level %d %dx%d fmt 0x%x: imageSize %d < %d, uploading empty",
               level, width, height, internalformat, imageSize, bw * bh * block_bytes);
        glTexImage2D(target, level, has_alpha ? GL_RGBA : GL_RGB, width, height, 0,
                     has_alpha ? GL_RGBA : GL_RGB,
                     has_alpha ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT_5_6_5, NULL);
        return;
    }

    GLenum dxt = internalformat == GL_ATC_RGB_AMD ? GL_COMPRESSED_RGB_S3TC_DXT1_EXT
               : internalformat == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
               : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    size_t size = (size_t)bw * bh * block_bytes;
    uint8_t *out = malloc(size);
    if (!out) {
        l_error("ATC: out of memory transcoding %dx%d", width, height);
        return;
    }
    const uint8_t *src = data;
    uint8_t *dst = out;
    for (int n = bw * bh; n > 0; n--, src += block_bytes, dst += block_bytes) {
        if (has_alpha) {
            memcpy(dst, src, 8);
            atc_to_dxt_color(src + 8, dst + 8);
        } else {
            atc_to_dxt_color(src, dst);
        }
    }
    glCompressedTexImage2D(target, level, dxt, width, height, border, (GLsizei)size, out);
    free(out);

    static int logged = 0;
    if (logged < 5) {
        l_info("ATC: level %d %dx%d fmt 0x%x -> %s", level, width, height, internalformat,
               dxt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? "DXT1"
               : dxt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ? "DXT3" : "DXT5");
        logged++;
    }
}
