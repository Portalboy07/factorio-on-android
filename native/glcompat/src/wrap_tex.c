/* Texture upload, compressed-format fallback and desktop glGetTexImage.
 *
 * rev-2 error hygiene: every wrapper here that performs layer-internal GL
 * calls (unpack state save/restore, BGRA conversion, software decode, FBO
 * readback) brackets those calls with fsgl_err_begin_extra()/
 * fsgl_err_end_extra().  The caller's forwarded call is checked immediately
 * after it returns and before any restore, so the caller still sees exactly
 * the error its own call produced.
 *
 * rev-2 hot-path changes: real entry points come from the enum cache, the
 * BGRA swap is 32-bit word-wise, compressed-extension probes are cached, and
 * texture records live in a direct-index array.
 */
#include "glcompat.h"
#include "etc2.h"

#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif
#ifndef GL_ETC2_RGB8_OES
#define GL_ETC2_RGB8_OES 0x9274
#endif
#ifndef GL_ETC2_RGBA8_OES
#define GL_ETC2_RGBA8_OES 0x9278
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RG8
#define GL_RG8 0x822B
#endif
#ifndef GL_TEXTURE_COMPRESSED_IMAGE_SIZE
#define GL_TEXTURE_COMPRESSED_IMAGE_SIZE 0x86A0
#endif
#ifndef GL_TEXTURE_COMPRESSED
#define GL_TEXTURE_COMPRESSED 0x86A1
#endif
#ifndef GL_TEXTURE_INTERNAL_FORMAT
#define GL_TEXTURE_INTERNAL_FORMAT 0x8B8F
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RED_RGTC1
#define GL_COMPRESSED_RED_RGTC1 0x8DBB
#define GL_COMPRESSED_SIGNED_RED_RGTC1 0x8DBC
#define GL_COMPRESSED_RG_RGTC2 0x8DBD
#define GL_COMPRESSED_SIGNED_RG_RGTC2 0x8DBE
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#endif
#ifndef GL_TEXTURE_DEPTH
#define GL_TEXTURE_DEPTH 0x8071
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH32F_STENCIL8 0x8CAD
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER 0x8D99
#define GL_RG_INTEGER 0x8228
#define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_BGRA_INTEGER
#define GL_BGRA_INTEGER 0x8D9B
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
#define GL_UNPACK_SKIP_IMAGES 0x806D
#endif

#define TEX_TAG "glcompat"

/* ------------------------------------------------------------------ */
/* Real entry points                                                   */
/* ------------------------------------------------------------------ */

static PFN_glGenTextures get_gentextures(void) { return (PFN_glGenTextures)fsgl_real_id(FSGL_R_glGenTextures); }
static PFN_glTexImage2D get_teximage2d(void) { return (PFN_glTexImage2D)fsgl_real_id(FSGL_R_glTexImage2D); }
static PFN_glTexSubImage2D get_texsubimage2d(void) { return (PFN_glTexSubImage2D)fsgl_real_id(FSGL_R_glTexSubImage2D); }
static PFN_glTexImage3D get_teximage3d(void) { return (PFN_glTexImage3D)fsgl_real_id(FSGL_R_glTexImage3D); }
static PFN_glTexSubImage3D get_texsubimage3d(void) { return (PFN_glTexSubImage3D)fsgl_real_id(FSGL_R_glTexSubImage3D); }
static PFN_glCompressedTexImage2D get_cteximage2d(void) { return (PFN_glCompressedTexImage2D)fsgl_real_id(FSGL_R_glCompressedTexImage2D); }
static PFN_glCompressedTexSubImage2D get_ctexsubimage2d(void) { return (PFN_glCompressedTexSubImage2D)fsgl_real_id(FSGL_R_glCompressedTexSubImage2D); }
static PFN_glCompressedTexSubImage3D get_ctexsubimage3d(void) { return (PFN_glCompressedTexSubImage3D)fsgl_real_id(FSGL_R_glCompressedTexSubImage3D); }
static PFN_glGetTexLevelParameteriv get_texlevel(void) { return (PFN_glGetTexLevelParameteriv)fsgl_real_id(FSGL_R_glGetTexLevelParameteriv); }
static PFN_glGetCompressedTexImage get_getctex(void) { return (PFN_glGetCompressedTexImage)fsgl_real_id(FSGL_R_glGetCompressedTexImage); }
static PFN_glGetIntegerv get_int(void) { return (PFN_glGetIntegerv)fsgl_real_id(FSGL_R_glGetIntegerv); }
static PFN_glPixelStorei get_pixelstore(void) { return (PFN_glPixelStorei)fsgl_real_id(FSGL_R_glPixelStorei); }

/* ------------------------------------------------------------------ */
/* Unpack state and BGRA conversion                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    GLint alignment;
    GLint row_length;
    GLint skip_rows;
    GLint skip_pixels;
    GLint image_height;
    GLint skip_images;
} UnpackState;

static void unpack_save(UnpackState *s)
{
    PFN_glGetIntegerv gi = get_int();
    memset(s, 0, sizeof(*s));
    s->alignment = 4;
    s->row_length = 0;
    if (gi) {
        gi(GL_UNPACK_ALIGNMENT, &s->alignment);
        gi(GL_UNPACK_ROW_LENGTH, &s->row_length);
        gi(GL_UNPACK_SKIP_ROWS, &s->skip_rows);
        gi(GL_UNPACK_SKIP_PIXELS, &s->skip_pixels);
        gi(GL_UNPACK_IMAGE_HEIGHT, &s->image_height);
        gi(GL_UNPACK_SKIP_IMAGES, &s->skip_images);
    }
}

static void unpack_tight(void)
{
    PFN_glPixelStorei ps = get_pixelstore();
    if (!ps) {
        return;
    }
    ps(GL_UNPACK_ALIGNMENT, 1);
    ps(GL_UNPACK_ROW_LENGTH, 0);
    ps(GL_UNPACK_SKIP_ROWS, 0);
    ps(GL_UNPACK_SKIP_PIXELS, 0);
    ps(GL_UNPACK_IMAGE_HEIGHT, 0);
    ps(GL_UNPACK_SKIP_IMAGES, 0);
}

static void unpack_restore(const UnpackState *s)
{
    PFN_glPixelStorei ps = get_pixelstore();
    if (!ps) {
        return;
    }
    ps(GL_UNPACK_ALIGNMENT, s->alignment);
    ps(GL_UNPACK_ROW_LENGTH, s->row_length);
    ps(GL_UNPACK_SKIP_ROWS, s->skip_rows);
    ps(GL_UNPACK_SKIP_PIXELS, s->skip_pixels);
    ps(GL_UNPACK_IMAGE_HEIGHT, s->image_height);
    ps(GL_UNPACK_SKIP_IMAGES, s->skip_images);
}

static GLint align_up(GLint v, GLint a)
{
    return (v + a - 1) / a * a;
}

/* BGRA8 -> RGBA8, honouring the current unpack state.  The swap is a single
 * 32-bit operation per pixel instead of four byte loads/stores. */
static unsigned char *convert_bgra(const void *data, GLsizei w, GLsizei h, GLsizei d,
                                   const UnpackState *s)
{
    if (!data) {
        return NULL;
    }
    size_t out_size = (size_t)w * (size_t)h * (size_t)d * 4u;
    unsigned char *out = (unsigned char *)malloc(out_size);
    if (!out) {
        return NULL;
    }
    GLint row_pixels = s->row_length > 0 ? s->row_length : w;
    GLint row_bytes = row_pixels * 4;
    GLint stride = align_up(row_bytes, s->alignment > 0 ? s->alignment : 1);
    GLint img_h = s->image_height > 0 ? s->image_height : h;
    size_t slice_stride = (size_t)stride * (size_t)img_h;
    const unsigned char *src = (const unsigned char *)data
        + (size_t)s->skip_images * slice_stride
        + (size_t)s->skip_rows * (size_t)stride
        + (size_t)s->skip_pixels * 4u;
    for (GLsizei z = 0; z < d; z++) {
        for (GLsizei y = 0; y < h; y++) {
            const unsigned char *row = src + (size_t)z * slice_stride + (size_t)y * (size_t)stride;
            unsigned char *dst = out + ((size_t)z * (size_t)h + (size_t)y) * (size_t)w * 4u;
            for (GLsizei x = 0; x < w; x++) {
                uint32_t v;
                memcpy(&v, row + (size_t)x * 4u, 4);
                uint32_t o = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                             ((v & 0x000000FFu) << 16);
                memcpy(dst + (size_t)x * 4u, &o, 4);
            }
        }
    }
    return out;
}

static GLint map_internal_format(GLint internal)
{
    if (internal == GL_BGRA || internal == GL_BGRA8_EXT) {
        return GL_RGBA8;
    }
    return internal;
}

/* Approximate upload size for the profiler's bytes_in field. */
static uint64_t upload_bytes(GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei d)
{
    if (w <= 0 || h <= 0 || d <= 0) {
        return 0;
    }
    size_t bpp = 4;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        if (format == GL_RGBA || format == GL_BGRA) bpp = 4;
        else if (format == GL_RGB) bpp = 3;
        else if (format == GL_RG) bpp = 2;
        else if (format == GL_RED) bpp = 1;
        else if (format == GL_RGBA_INTEGER) bpp = 16;
        else if (format == GL_RG_INTEGER) bpp = 8;
        else if (format == GL_RED_INTEGER) bpp = 4;
        break;
    case GL_UNSIGNED_SHORT:
        if (format == GL_DEPTH_COMPONENT || format == GL_RED_INTEGER) bpp = 2;
        else if (format == GL_RGBA_INTEGER) bpp = 8;
        else if (format == GL_RG_INTEGER) bpp = 4;
        else bpp = 2;
        break;
    case GL_UNSIGNED_INT:
        if (format == GL_DEPTH_STENCIL) bpp = 4;
        else if (format == GL_RGBA_INTEGER) bpp = 16;
        else if (format == GL_RG_INTEGER) bpp = 8;
        else bpp = 4;
        break;
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_INT_24_8:
        bpp = 2;
        break;
    default:
        bpp = 4;
        break;
    }
    return (uint64_t)w * (uint64_t)h * (uint64_t)d * (uint64_t)bpp;
}

/* ------------------------------------------------------------------ */
/* S3TC / RGTC software decode                                         */
/* ------------------------------------------------------------------ */

static bool is_dxt(GLenum fmt)
{
    return fmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
           fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ||
           fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
           fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}

static bool is_rgtc(GLenum fmt)
{
    return fmt == GL_COMPRESSED_RED_RGTC1 || fmt == GL_COMPRESSED_RG_RGTC2;
}

/* Whether the DRIVER decodes the format.  The advertised list may carry a
 * synthetic S3TC/RGTC entry (fake_s3tc), so it must not be asked here. */
static bool driver_supports_compressed(GLenum fmt)
{
    if (is_dxt(fmt)) {
        return fsgl_ext_caps()->s3tc;
    }
    if (is_rgtc(fmt)) {
        return fsgl_ext_caps()->rgtc;
    }
    return true;
}

static void decode_565(unsigned short c, unsigned char *rgb)
{
    int r = (c >> 11) & 31;
    int g = (c >> 5) & 63;
    int b = c & 31;
    rgb[0] = (unsigned char)((r << 3) | (r >> 2));
    rgb[1] = (unsigned char)((g << 2) | (g >> 4));
    rgb[2] = (unsigned char)((b << 3) | (b >> 2));
}

static void decode_color_block(const unsigned char *src, bool force_four,
                               unsigned char out[16][4])
{
    unsigned short c0 = (unsigned short)(src[0] | (src[1] << 8));
    unsigned short c1 = (unsigned short)(src[2] | (src[3] << 8));
    unsigned char p[4][4];
    memset(p, 0, sizeof(p));
    decode_565(c0, p[0]);
    decode_565(c1, p[1]);
    p[0][3] = 255;
    p[1][3] = 255;
    if (force_four || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            p[2][k] = (unsigned char)((2 * p[0][k] + p[1][k] + 1) / 3);
            p[3][k] = (unsigned char)((p[0][k] + 2 * p[1][k] + 1) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int k = 0; k < 3; k++) {
            p[2][k] = (unsigned char)((p[0][k] + p[1][k]) / 2);
        }
        p[2][3] = 255;
        p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
    }
    unsigned int bits = (unsigned int)src[4] | ((unsigned int)src[5] << 8) |
                        ((unsigned int)src[6] << 16) | ((unsigned int)src[7] << 24);
    for (int i = 0; i < 16; i++) {
        unsigned int idx = (bits >> (2 * i)) & 3u;
        out[i][0] = p[idx][0];
        out[i][1] = p[idx][1];
        out[i][2] = p[idx][2];
        out[i][3] = p[idx][3];
    }
}

static void decode_dxt1_block(const unsigned char *src, unsigned char out[16][4])
{
    decode_color_block(src, false, out);
}

static void decode_dxt3_block(const unsigned char *src, unsigned char out[16][4])
{
    decode_color_block(src + 8, true, out);
    for (int i = 0; i < 16; i++) {
        unsigned int a = (src[i / 2] >> ((i & 1) * 4)) & 0xFu;
        out[i][3] = (unsigned char)((a << 4) | a);
    }
}

static void decode_dxt5_block(const unsigned char *src, unsigned char out[16][4])
{
    unsigned char a[8];
    a[0] = src[0];
    a[1] = src[1];
    if (a[0] > a[1]) {
        for (int i = 1; i <= 6; i++) {
            a[i + 1] = (unsigned char)(((7 - i) * a[0] + i * a[1] + 3) / 7);
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            a[i + 1] = (unsigned char)(((5 - i) * a[0] + i * a[1] + 2) / 5);
        }
        a[6] = 0;
        a[7] = 255;
    }
    unsigned long long bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (unsigned long long)src[2 + i] << (8 * i);
    }
    decode_color_block(src + 8, true, out);
    for (int i = 0; i < 16; i++) {
        unsigned int idx = (unsigned int)((bits >> (3 * i)) & 7u);
        out[i][3] = a[idx];
    }
}

static void decode_rgtc_block(const unsigned char *src, int channels, unsigned char out[16][4])
{
    unsigned char v[8];
    v[0] = src[0];
    v[1] = src[1];
    if (v[0] > v[1]) {
        for (int i = 1; i <= 6; i++) {
            v[i + 1] = (unsigned char)(((7 - i) * v[0] + i * v[1] + 3) / 7);
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            v[i + 1] = (unsigned char)(((5 - i) * v[0] + i * v[1] + 2) / 5);
        }
        v[6] = 0;
        v[7] = 255;
    }
    unsigned long long bits = 0;
    for (int i = 0; i < 6; i++) {
        bits |= (unsigned long long)src[2 + i] << (8 * i);
    }
    for (int i = 0; i < 16; i++) {
        unsigned int idx = (unsigned int)((bits >> (3 * i)) & 7u);
        out[i][0] = v[idx];
        out[i][1] = 0;
        out[i][2] = 0;
        out[i][3] = 255;
    }
    if (channels == 2) {
        const unsigned char *s2 = src + 8;
        v[0] = s2[0];
        v[1] = s2[1];
        if (v[0] > v[1]) {
            for (int i = 1; i <= 6; i++) {
                v[i + 1] = (unsigned char)(((7 - i) * v[0] + i * v[1] + 3) / 7);
            }
        } else {
            for (int i = 1; i <= 4; i++) {
                v[i + 1] = (unsigned char)(((5 - i) * v[0] + i * v[1] + 2) / 5);
            }
            v[6] = 0;
            v[7] = 255;
        }
        bits = 0;
        for (int i = 0; i < 6; i++) {
            bits |= (unsigned long long)s2[2 + i] << (8 * i);
        }
        for (int i = 0; i < 16; i++) {
            unsigned int idx = (unsigned int)((bits >> (3 * i)) & 7u);
            out[i][1] = v[idx];
        }
    }
}

static unsigned char *decode_compressed(GLenum fmt, GLsizei w, GLsizei h, const void *data,
                                        GLsizei image_size)
{
    if (!data || w <= 0 || h <= 0) {
        return NULL;
    }
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    int block_bytes;
    if (fmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ||
        fmt == GL_COMPRESSED_RED_RGTC1) {
        block_bytes = 8;
    } else if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT || fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ||
               fmt == GL_COMPRESSED_RG_RGTC2) {
        block_bytes = 16;
    } else {
        return NULL;
    }
    if (image_size > 0 && image_size < bw * bh * block_bytes) {
        return NULL;
    }
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) {
        return NULL;
    }
    const unsigned char *src = (const unsigned char *)data;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            unsigned char block[16][4];
            const unsigned char *bs = src + ((size_t)by * bw + bx) * block_bytes;
            if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || fmt == GL_COMPRESSED_RGB_S3TC_DXT1_EXT) {
                decode_dxt1_block(bs, block);
            } else if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) {
                decode_dxt3_block(bs, block);
            } else if (fmt == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) {
                decode_dxt5_block(bs, block);
            } else if (fmt == GL_COMPRESSED_RED_RGTC1) {
                decode_rgtc_block(bs, 1, block);
            } else {
                decode_rgtc_block(bs, 2, block);
            }
            for (int y = 0; y < 4; y++) {
                int py = by * 4 + y;
                if (py >= h) break;
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x;
                    if (px >= w) break;
                    unsigned char *dst = rgba + ((size_t)py * (size_t)w + (size_t)px) * 4u;
                    memcpy(dst, block[y * 4 + x], 4);
                }
            }
        }
    }
    return rgba;
}


/* Diagnostic probe: compares the layer's tracked binding with the driver's
 * actual binding on the active unit (texture-unit desync would route
 * uploads/updates to the wrong object).  Two glGetIntegerv per streamed tile,
 * so FSGL_DIAG builds only. */
static void probe_binding_mismatch(const char *where, GLenum target)
{
#if FSGL_DIAG
    PFN_glGetIntegerv gi = get_int();
    if (!gi) {
        return;
    }
    GLint real_binding = 0, active_unit = 0;
    GLenum pname = (target == GL_TEXTURE_3D) ? GL_TEXTURE_BINDING_3D
                 : GL_TEXTURE_BINDING_2D;
    gi(pname, &real_binding);
    gi(GL_ACTIVE_TEXTURE, &active_unit);
    GLuint tracked = fsgl_tex_bound(target);
    if ((GLuint)real_binding != tracked) {
        fsgl_log(ANDROID_LOG_ERROR,
                 "binding MISMATCH in %s: driver=%u tracked=%u unit=0x%x",
                 where, (unsigned)real_binding, tracked,
                 (unsigned)active_unit);
    }
    static GLint last_unit = -1;
    if (active_unit != last_unit) {
        last_unit = active_unit;
        fsgl_log(ANDROID_LOG_INFO, "active texture unit now 0x%x", active_unit);
    }
#else
    (void)where;
    (void)target;
#endif
}

/* ------------------------------------------------------------------ */
/* Uploads                                                             */
/* ------------------------------------------------------------------ */

static void record_upload(GLenum target, GLint level, GLint internal, GLenum format,
                          GLenum type, GLsizei w, GLsizei h, GLsizei d)
{
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, true);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (lvl) {
        lvl->internal_format = (GLenum)internal;
        lvl->format = format;
        lvl->type = type;
        lvl->width = w;
        lvl->height = h;
        lvl->depth = d;
        lvl->compressed = false;
        lvl->decoded = false;
        lvl->transcoded = false;
        lvl->src_format = 0;
        lvl->compressed_size = 0;
    }
}

/* Sub-update variant: patch the record without shrinking the level's
 * tracked dimensions (streamed tiles are smaller than the level). */
static void record_subupdate(GLenum target, GLint level, GLenum src_fmt,
                             uint32_t dest_fmt, GLsizei w, GLsizei h, size_t dest_sz)
{
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, false);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (lvl) {
        if (!lvl->transcoded || lvl->internal_format != (GLenum)dest_fmt) {
            lvl->internal_format = (GLenum)dest_fmt;
            lvl->format = (GLenum)dest_fmt;
            lvl->type = 0;
            lvl->compressed = (dest_fmt == FSGL_ETC2_RGB8 || dest_fmt == FSGL_ETC2_RGBA8);
            lvl->decoded = false;
            lvl->transcoded = true;
            lvl->src_format = src_fmt;
            lvl->compressed_size = lvl->compressed ? (GLsizei)dest_sz : 0;
        }
        if (lvl->width < w) {
            lvl->width = w;
        }
        if (lvl->height < h) {
            lvl->height = h;
        }
    }
}

/* Fill the level record after a successful DXT/BC -> ETC2/R8/RG8
 * transcode of a full-image upload. */
static void record_transcoded(GLenum target, GLint level, GLenum src_fmt,
                              uint32_t dest_fmt, GLsizei w, GLsizei h, size_t dest_sz)
{
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, true);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (lvl) {
        lvl->internal_format = (GLenum)dest_fmt;
        lvl->format = (GLenum)dest_fmt;
        lvl->type = 0;
        lvl->width = w;
        lvl->height = h;
        lvl->depth = 1;
        lvl->compressed = (dest_fmt == FSGL_ETC2_RGB8 || dest_fmt == FSGL_ETC2_RGBA8);
        lvl->decoded = false;
        lvl->transcoded = true;
        lvl->src_format = src_fmt;
        lvl->compressed_size = lvl->compressed ? (GLsizei)dest_sz : 0;
    }
}

/* Storage-only allocation (no data) of a DXT/RGTC level the driver cannot
 * decode: allocate it in the transcoder's destination format so the game's
 * later glCompressedTexSubImage2D updates, which are transcoded, have a level
 * to land on.  Returns false for formats without a destination (DXT1A). */
static bool alloc_transcoded_level(GLenum target, GLint level, GLenum internalformat,
                                   GLsizei width, GLsizei height, GLint border)
{
    uint32_t alloc_fmt = fsgl_etc2_dest_format(internalformat);
    if (alloc_fmt == FSGL_ETC2_RGB8 || alloc_fmt == FSGL_ETC2_RGBA8) {
        PFN_glCompressedTexImage2D real_a = get_cteximage2d();
        size_t dsz = fsgl_etc2_dest_size(alloc_fmt, width, height);
        if (real_a) {
            real_a(target, level, (GLenum)alloc_fmt, width, height, border,
                   (GLsizei)dsz, NULL);
        }
        fsgl_err_check("glCompressedTexImage2D",
                       "target=0x%x level=%d (ETC2 alloc, no data)",
                       (unsigned)target, (int)level);
        record_transcoded(target, level, internalformat, alloc_fmt, width, height, dsz);
        fsgl_log_once("compressed-alloc",
                      "compressed: allocating 0x%04x %dx%d level %d as ETC2 "
                      "(no data)", internalformat, width, height, level);
        return true;
    }
    if (alloc_fmt == FSGL_R8 || alloc_fmt == FSGL_RG8) {
        PFN_glTexImage2D tex2d = get_teximage2d();
        if (tex2d) {
            tex2d(target, level, (GLint)alloc_fmt, width, height, border,
                  alloc_fmt == FSGL_R8 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, NULL);
        }
        fsgl_err_check("glCompressedTexImage2D",
                       "target=0x%x level=%d (R8/RG8 alloc, no data)",
                       (unsigned)target, (int)level);
        record_transcoded(target, level, internalformat, alloc_fmt, width, height, 0);
        fsgl_log_once("compressed-alloc-rgtc",
                      "compressed: allocating 0x%04x %dx%d level %d as %s "
                      "(no data)", internalformat, width, height, level,
                      alloc_fmt == FSGL_R8 ? "R8" : "RG8");
        return true;
    }
    return false;
}

void GL_APIENTRY fsgl_wrap_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLenum format, GLenum type, const void *pixels)
{
    FSGL_PROF_BEGIN(glTexImage2D);
    fsgl_ring_push("glTexImage2D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)internalformat, (uintptr_t)width);

    PFN_glTexImage2D real = get_teximage2d();
    if (!real) {
        FSGL_PROF_END(0, 0);
        return;
    }
    /* Desktop GL accepts a DXT/RGTC internalformat in glTexImage2D and
     * allocates compressed storage; ES rejects it with INVALID_VALUE.  The
     * game allocates its compressed atlases (shadows, alpha masks, and every
     * colour atlas once S3TC is advertised) exactly this way, with no data,
     * then fills them with glCompressedTexSubImage2D. */
#if FSGL_DIAG
    if (!pixels && level == 0) {
        /* Render targets are allocated without data; their formats decide
         * the per-pass bandwidth on a tiler. */
        static struct { GLint ifmt; GLsizei w, h; } seen[64];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen; i++) {
            known |= seen[i].ifmt == internalformat && seen[i].w == width && seen[i].h == height;
        }
        if (!known && nseen < 64) {
            seen[nseen].ifmt = internalformat;
            seen[nseen].w = width;
            seen[nseen].h = height;
            nseen++;
            fsgl_log(ANDROID_LOG_INFO, "alloc: texture %u ifmt=0x%x fmt=0x%x type=0x%x %dx%d",
                     fsgl_tex_bound(target), (unsigned)internalformat, (unsigned)format,
                     (unsigned)type, (int)width, (int)height);
        }
    }
#endif
    bool decode_later = false;
    if ((is_dxt((GLenum)internalformat) || is_rgtc((GLenum)internalformat)) &&
        !driver_supports_compressed((GLenum)internalformat)) {
        if (!pixels && alloc_transcoded_level(target, level, (GLenum)internalformat,
                                              width, height, border)) {
            FSGL_PROF_END(0, 0);
            return;
        }
        /* With data (or DXT1A): keep it uncompressed in the caller's format;
         * later compressed sub-updates take the RGBA decode path. */
        fsgl_log_once("teximage-compressed-ifmt",
                      "upload: glTexImage2D internalformat 0x%04x stored uncompressed (%s)",
                      internalformat, pixels ? "with data" : "no transcode target");
        internalformat = (GLint)format;
        decode_later = true;
    }
    GLint mapped = map_internal_format(internalformat);
    if (mapped != internalformat) {
        fsgl_log_once("bgra-internal", "upload: BGRA internalformat 0x%04x -> GL_RGBA8", internalformat);
    }
    uint64_t bytes = upload_bytes(format, type, width, height, 1);
    if (format == GL_BGRA && type == GL_UNSIGNED_BYTE && pixels) {
        UnpackState s;
        fsgl_err_begin_extra("glTexImage2D");
        unpack_save(&s);
        fsgl_err_end_extra("glTexImage2D");
        unsigned char *rgba = convert_bgra(pixels, width, height, 1, &s);
        if (rgba) {
            fsgl_err_begin_extra("glTexImage2D");
            unpack_tight();
            fsgl_err_end_extra("glTexImage2D");
            real(target, level, mapped, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            FSGL_PROF_END(bytes, 0);
            fsgl_err_check("glTexImage2D",
                           "target=0x%x level=%d ifmt=0x%x w=%d h=%d border=%d fmt=0x%x type=0x%x",
                           (unsigned)target, (int)level, (unsigned)mapped, (int)width,
                           (int)height, (int)border, (unsigned)GL_RGBA,
                           (unsigned)GL_UNSIGNED_BYTE);
            fsgl_err_begin_extra("glTexImage2D");
            unpack_restore(&s);
            fsgl_err_end_extra("glTexImage2D");
            free(rgba);
            record_upload(target, level, mapped, GL_RGBA, GL_UNSIGNED_BYTE, width, height, 1);
            fsgl_log_once("bgra-upload", "upload: BGRA8 -> RGBA8 conversion path");
            return;
        }
        /* Conversion failed; unpack state was not modified. */
    }
    real(target, level, mapped, width, height, border, format, type, pixels);
    FSGL_PROF_END(bytes, 0);
    fsgl_err_check("glTexImage2D",
                   "target=0x%x level=%d ifmt=0x%x w=%d h=%d border=%d fmt=0x%x type=0x%x",
                   (unsigned)target, (int)level, (unsigned)mapped, (int)width, (int)height,
                   (int)border, (unsigned)format, (unsigned)type);
    record_upload(target, level, mapped, format, type, width, height, 1);
    if (decode_later) {
        FsglTexLevel *lvl = fsgl_tex_level(fsgl_tex_get(fsgl_tex_bound(target), false), level);
        if (lvl) {
            lvl->decoded = true;
        }
    }
}

void GL_APIENTRY fsgl_wrap_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                        GLsizei width, GLsizei height, GLenum format,
                                        GLenum type, const void *pixels)
{
    FSGL_PROF_BEGIN(glTexSubImage2D);
    fsgl_ring_push("glTexSubImage2D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)xoffset, (uintptr_t)yoffset);

    PFN_glTexSubImage2D real = get_texsubimage2d();
    if (!real) {
        FSGL_PROF_END(0, 0);
        return;
    }
    uint64_t bytes = upload_bytes(format, type, width, height, 1);
    if (format == GL_BGRA && type == GL_UNSIGNED_BYTE && pixels) {
        UnpackState s;
        fsgl_err_begin_extra("glTexSubImage2D");
        unpack_save(&s);
        fsgl_err_end_extra("glTexSubImage2D");
        unsigned char *rgba = convert_bgra(pixels, width, height, 1, &s);
        if (rgba) {
            fsgl_err_begin_extra("glTexSubImage2D");
            unpack_tight();
            fsgl_err_end_extra("glTexSubImage2D");
            real(target, level, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            FSGL_PROF_END(bytes, 0);
            fsgl_err_check("glTexSubImage2D",
                           "target=0x%x level=%d x=%d y=%d w=%d h=%d fmt=0x%x type=0x%x",
                           (unsigned)target, (int)level, (int)xoffset, (int)yoffset,
                           (int)width, (int)height, (unsigned)GL_RGBA,
                           (unsigned)GL_UNSIGNED_BYTE);
            fsgl_err_begin_extra("glTexSubImage2D");
            unpack_restore(&s);
            fsgl_err_end_extra("glTexSubImage2D");
            free(rgba);
            return;
        }
    }
    real(target, level, xoffset, yoffset, width, height, format, type, pixels);
    FSGL_PROF_END(bytes, 0);
    fsgl_err_check("glTexSubImage2D",
                   "target=0x%x level=%d x=%d y=%d w=%d h=%d fmt=0x%x type=0x%x",
                   (unsigned)target, (int)level, (int)xoffset, (int)yoffset,
                   (int)width, (int)height, (unsigned)format, (unsigned)type);
}

void GL_APIENTRY fsgl_wrap_glTexImage3D(GLenum target, GLint level, GLint internalformat,
                                     GLsizei width, GLsizei height, GLsizei depth, GLint border,
                                     GLenum format, GLenum type, const void *pixels)
{
    FSGL_PROF_BEGIN(glTexImage3D);
    fsgl_ring_push("glTexImage3D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)internalformat, (uintptr_t)width);

    PFN_glTexImage3D real = get_teximage3d();
    if (!real) {
        FSGL_PROF_END(0, 0);
        return;
    }
    GLint mapped = map_internal_format(internalformat);
    uint64_t bytes = upload_bytes(format, type, width, height, depth);
    if (format == GL_BGRA && type == GL_UNSIGNED_BYTE && pixels) {
        UnpackState s;
        fsgl_err_begin_extra("glTexImage3D");
        unpack_save(&s);
        fsgl_err_end_extra("glTexImage3D");
        unsigned char *rgba = convert_bgra(pixels, width, height, depth, &s);
        if (rgba) {
            fsgl_err_begin_extra("glTexImage3D");
            unpack_tight();
            fsgl_err_end_extra("glTexImage3D");
            real(target, level, mapped, width, height, depth, border, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
            FSGL_PROF_END(bytes, 0);
            fsgl_err_check("glTexImage3D",
                           "target=0x%x level=%d ifmt=0x%x w=%d h=%d d=%d border=%d fmt=0x%x type=0x%x",
                           (unsigned)target, (int)level, (unsigned)mapped, (int)width,
                           (int)height, (int)depth, (int)border, (unsigned)GL_RGBA,
                           (unsigned)GL_UNSIGNED_BYTE);
            fsgl_err_begin_extra("glTexImage3D");
            unpack_restore(&s);
            fsgl_err_end_extra("glTexImage3D");
            free(rgba);
            record_upload(target, level, mapped, GL_RGBA, GL_UNSIGNED_BYTE, width, height, depth);
            return;
        }
    }
    real(target, level, mapped, width, height, depth, border, format, type, pixels);
    FSGL_PROF_END(bytes, 0);
    fsgl_err_check("glTexImage3D",
                   "target=0x%x level=%d ifmt=0x%x w=%d h=%d d=%d border=%d fmt=0x%x type=0x%x",
                   (unsigned)target, (int)level, (unsigned)mapped, (int)width, (int)height,
                   (int)depth, (int)border, (unsigned)format, (unsigned)type);
    record_upload(target, level, mapped, format, type, width, height, depth);
}

void GL_APIENTRY fsgl_wrap_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                        GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                        GLenum format, GLenum type, const void *pixels)
{
    FSGL_PROF_BEGIN(glTexSubImage3D);
    fsgl_ring_push("glTexSubImage3D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)xoffset, (uintptr_t)yoffset);

    PFN_glTexSubImage3D real = get_texsubimage3d();
    if (!real) {
        FSGL_PROF_END(0, 0);
        return;
    }
    uint64_t bytes = upload_bytes(format, type, width, height, depth);
    if (format == GL_BGRA && type == GL_UNSIGNED_BYTE && pixels) {
        UnpackState s;
        fsgl_err_begin_extra("glTexSubImage3D");
        unpack_save(&s);
        fsgl_err_end_extra("glTexSubImage3D");
        unsigned char *rgba = convert_bgra(pixels, width, height, depth, &s);
        if (rgba) {
            fsgl_err_begin_extra("glTexSubImage3D");
            unpack_tight();
            fsgl_err_end_extra("glTexSubImage3D");
            real(target, level, xoffset, yoffset, zoffset, width, height, depth,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            FSGL_PROF_END(bytes, 0);
            fsgl_err_check("glTexSubImage3D",
                           "target=0x%x level=%d x=%d y=%d z=%d w=%d h=%d d=%d fmt=0x%x type=0x%x",
                           (unsigned)target, (int)level, (int)xoffset, (int)yoffset,
                           (int)zoffset, (int)width, (int)height, (int)depth,
                           (unsigned)GL_RGBA, (unsigned)GL_UNSIGNED_BYTE);
            fsgl_err_begin_extra("glTexSubImage3D");
            unpack_restore(&s);
            fsgl_err_end_extra("glTexSubImage3D");
            free(rgba);
            return;
        }
    }
    real(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
    FSGL_PROF_END(bytes, 0);
    fsgl_err_check("glTexSubImage3D",
                   "target=0x%x level=%d x=%d y=%d z=%d w=%d h=%d d=%d fmt=0x%x type=0x%x",
                   (unsigned)target, (int)level, (int)xoffset, (int)yoffset, (int)zoffset,
                   (int)width, (int)height, (int)depth, (unsigned)format, (unsigned)type);
}

void GL_APIENTRY fsgl_wrap_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                               GLsizei width, GLsizei height, GLint border,
                                               GLsizei imageSize, const void *data)
{
    FSGL_PROF_BEGIN(glCompressedTexImage2D);
    fsgl_ring_push("glCompressedTexImage2D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)internalformat, (uintptr_t)width);

    PFN_glCompressedTexImage2D real = get_cteximage2d();
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, true);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (lvl) {
        lvl->internal_format = internalformat;
        lvl->format = internalformat;
        lvl->type = 0;
        lvl->width = width;
        lvl->height = height;
        lvl->depth = 1;
        lvl->compressed = true;
        lvl->decoded = false;
        lvl->compressed_size = imageSize;
    }
    if ((is_dxt(internalformat) || is_rgtc(internalformat)) &&
        !driver_supports_compressed(internalformat)) {
        /* data == NULL: storage allocation only.  Allocate the level in the
         * destination format so later CompressedTexSubImage2D streaming has
         * something to land on (the game allocates its mip chain this way;
         * forwarding the DXT format fails and leaves the level undefined,
         * which made every streamed mip update fail with
         * INVALID_OPERATION). */
        if (!data && alloc_transcoded_level(target, level, internalformat, width, height,
                                            border)) {
            FSGL_PROF_END(0, 0);
            return;
        }
        fsgl_err_begin_extra("glCompressedTexImage2D");
        uint32_t dest_fmt = 0;
        size_t dest_sz = 0;
        unsigned char *dest = fsgl_transcode_to_etc2(internalformat, width, height,
                                                     data, imageSize > 0 ? (size_t)imageSize : 0,
                                                     &dest_fmt, &dest_sz);
        if (dest && (dest_fmt == FSGL_ETC2_RGB8 || dest_fmt == FSGL_ETC2_RGBA8)) {
            PFN_glCompressedTexImage2D real = get_cteximage2d();
            if (real) {
                real(target, level, (GLenum)dest_fmt, width, height, border,
                     (GLsizei)dest_sz, dest);
                /* Verify the level actually landed: some drivers accept the
                 * call without error yet keep the uncompressed (GL_RGBA
                 * placeholder) definition, which makes every later streamed
                 * tile update fail.  On either a raised error or a failed
                 * verification, retry on a FRESH texture object that is then
                 * served under the game's name. */
                PFN_glGetError geterr = (PFN_glGetError)fsgl_real_id(FSGL_R_glGetError);
                GLenum e = geterr ? geterr() : 0;
                GLint rif = 0, rw = 0;
                PFN_glGetTexLevelParameteriv getlevel_v = get_texlevel();
                if (getlevel_v) {
                    getlevel_v(target, level, GL_TEXTURE_INTERNAL_FORMAT, &rif);
                    getlevel_v(target, level, GL_TEXTURE_WIDTH, &rw);
                }
                bool landed = (e == 0) && (uint32_t)rif == dest_fmt && rw == width;
                if (!landed) {
                    PFN_glGenTextures gen = get_gentextures();
                    PFN_glBindTexture bind_t =
                        (PFN_glBindTexture)fsgl_real_id(FSGL_R_glBindTexture);
                    GLuint alt = 0;
                    bool ok = false;
                    if (gen && bind_t) {
                        gen(1, &alt);
                        if (alt) {
                            bind_t(target, alt);
                            real(alt, level, (GLenum)dest_fmt, width, height, border,
                                 (GLsizei)dest_sz, dest);
                            GLint rif2 = 0, rw2 = 0;
                            if (getlevel_v) {
                                getlevel_v(target, level, GL_TEXTURE_INTERNAL_FORMAT, &rif2);
                                getlevel_v(target, level, GL_TEXTURE_WIDTH, &rw2);
                            }
                            ok = (geterr() == 0) && (uint32_t)rif2 == dest_fmt &&
                                 rw2 == width;
                        }
                    }
                    if (ok) {
                        fsgl_tex_redirect_add(target, id, alt);
                        fsgl_log(ANDROID_LOG_WARN,
                                 "compressed: ETC2 level did not land on texture %u "
                                 "(err=0x%x ifmt=0x%x w=%d); replaced by fresh object %u",
                                 id, e, rif, rw, alt);
                    } else {
                        fsgl_log(ANDROID_LOG_ERROR,
                                 "cteximage FAIL fmt=0x%04x dest=0x%04x lvl=%d %dx%d "
                                 "isz=%zu err=0x%x ifmt=0x%x w=%d (redirect %s)",
                                 internalformat, dest_fmt, level, width, height,
                                 dest_sz, e, rif, rw, ok ? "ok" : "failed");
                    }
                }
            }
            free(dest);
            fsgl_err_end_extra("glCompressedTexImage2D");
            record_transcoded(target, level, internalformat, dest_fmt,
                              width, height, dest_sz);
            FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
            fsgl_log_once("compressed-transcode",
                          "compressed: transcoding S3TC/RGTC format 0x%04x %dx%d -> %s "
                          "(driver lacks extension)",
                          internalformat, width, height,
                          dest_fmt == FSGL_ETC2_RGB8 ? "ETC2 RGB8" : "ETC2 RGBA8");
            return;
        }
        if (dest && (dest_fmt == FSGL_R8 || dest_fmt == FSGL_RG8)) {
            PFN_glTexImage2D tex2d = get_teximage2d();
            if (tex2d) {
                tex2d(target, level, (GLint)dest_fmt, width, height, border,
                      dest_fmt == FSGL_R8 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, dest);
            }
            free(dest);
            fsgl_err_end_extra("glCompressedTexImage2D");
            record_transcoded(target, level, internalformat, dest_fmt,
                              width, height, dest_sz);
            FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
            fsgl_log_once("compressed-transcode-rgtc",
                          "compressed: decoding RGTC format 0x%04x %dx%d -> %s "
                          "(driver lacks extension)",
                          internalformat, width, height,
                          dest_fmt == FSGL_R8 ? "R8" : "RG8");
            return;
        }
        if (data && !dest) {
            fsgl_log(ANDROID_LOG_ERROR,
                     "transcode NULL fmt=0x%04x %dx%d isz=%d (falling back to RGBA8)",
                     internalformat, width, height, imageSize);
        }
        free(dest);
        /* Transcode failed (e.g. DXT1A or a short imageSize): fall back to
         * the rev-2 RGBA8 decode, which every driver accepts. */
        unsigned char *rgba = decode_compressed(internalformat, width, height, data, imageSize);
        PFN_glTexImage2D tex2d = get_teximage2d();
        if (rgba && tex2d) {
            tex2d(target, level, GL_RGBA8, width, height, border, GL_RGBA,
                  GL_UNSIGNED_BYTE, rgba);
            free(rgba);
            fsgl_err_end_extra("glCompressedTexImage2D");
            if (lvl) {
                lvl->internal_format = GL_RGBA8;
                lvl->format = GL_RGBA8;
                lvl->type = GL_UNSIGNED_BYTE;
                lvl->compressed = false;
                lvl->decoded = true;
                lvl->transcoded = false;
            }
            FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
            fsgl_log_once("compressed-decode",
                          "compressed: software-decoding S3TC/RGTC format 0x%04x %dx%d "
                          "to RGBA8 (transcode unavailable)",
                          internalformat, width, height);
            return;
        }
        free(rgba);
        fsgl_err_end_extra("glCompressedTexImage2D");
        fsgl_log(ANDROID_LOG_ERROR, "compressed: cannot decode format 0x%04x %dx%d",
                 internalformat, width, height);
    }
    if (real) {
        real(target, level, internalformat, width, height, border, imageSize, data);
        FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
        fsgl_err_check("glCompressedTexImage2D",
                       "target=0x%x level=%d ifmt=0x%x w=%d h=%d border=%d imageSize=%d",
                       (unsigned)target, (int)level, (unsigned)internalformat,
                       (int)width, (int)height, (int)border, (int)imageSize);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

void GL_APIENTRY fsgl_wrap_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset,
                                                  GLint yoffset, GLsizei width, GLsizei height,
                                                  GLenum format, GLsizei imageSize, const void *data)
{
    FSGL_PROF_BEGIN(glCompressedTexSubImage2D);
    fsgl_ring_push("glCompressedTexSubImage2D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)xoffset, (uintptr_t)yoffset);

    /* rev-3: transcode whenever the source format is one the driver cannot
     * take, regardless of the level record.  The destination format follows
     * from the call's own format, so a lost or stale record can no longer
     * route a compressed update into an uncompressed texture (the rev-2
     * source of repeating INVALID_OPERATION). */
    if ((is_dxt(format) || is_rgtc(format)) && !driver_supports_compressed(format)) {
        if ((xoffset & 3) || (yoffset & 3) || (width & 3) || (height & 3)) {
            /* ETC2 sub-rects must be block-aligned; the game's streamed
             * atlas tiles are, so this should never fire. */
            fsgl_log_once("ctexsub-unaligned",
                          "compressed: skipping unaligned sub-update "
                          "(x=%d y=%d w=%d h=%d fmt=0x%04x)",
                          xoffset, yoffset, width, height, format);
            FSGL_PROF_END(0, 0);
            return;
        }
        fsgl_err_begin_extra("glCompressedTexSubImage2D");
        uint32_t dest_fmt = 0;
        size_t dest_sz = 0;
        unsigned char *dest = fsgl_transcode_to_etc2(format, width, height, data,
                                                     imageSize > 0 ? (size_t)imageSize : 0,
                                                     &dest_fmt, &dest_sz);
        if (dest && (dest_fmt == FSGL_ETC2_RGB8 || dest_fmt == FSGL_ETC2_RGBA8)) {
            PFN_glCompressedTexSubImage2D real = get_ctexsubimage2d();
            if (real) {
                probe_binding_mismatch("ctexsub-etc2", target);
                real(target, level, xoffset, yoffset, width, height,
                     (GLenum)dest_fmt, (GLsizei)dest_sz, dest);
                /* One-shot diagnostic: on failure, log the call plus the
                 * driver's view of the level (internal format + extents).
                 * Release builds leave the error to fsgl_err_end_extra(). */
                PFN_glGetError geterr = FSGL_DIAG
                    ? (PFN_glGetError)fsgl_real_id(FSGL_R_glGetError) : NULL;
                GLenum e = geterr ? geterr() : 0;
                if (e != 0) {
                    static const char *last_key;
                    static int last_key_len;
                    char key[64];
                    int kn = snprintf(key, sizeof(key), "f%04x_%dx%d", format, width, height);
                    if (!last_key || kn != last_key_len || memcmp(key, last_key, kn) != 0) {
                        last_key = (const char *)memcpy(malloc(kn + 1), key, kn + 1);
                        last_key_len = kn;
                        PFN_glGetTexLevelParameteriv getlevel = get_texlevel();
                        GLint rif = 0, rw = 0, rh = 0;
                        if (getlevel) {
                            getlevel(target, level, GL_TEXTURE_INTERNAL_FORMAT, &rif);
                            getlevel(target, level, GL_TEXTURE_WIDTH, &rw);
                            getlevel(target, level, GL_TEXTURE_HEIGHT, &rh);
                        }
                        FsglTexRec *rec_f = fsgl_tex_get(fsgl_tex_bound(target), false);
                        FsglTexLevel *lf = fsgl_tex_level(rec_f, level);
                        fsgl_log(ANDROID_LOG_ERROR,
                                 "ctexsub FAIL fmt=0x%04x dest=0x%04x lvl=%d "
                                 "rect=(%d,%d %dx%d) isz=%zu err=0x%x "
                                 "driver_level=(ifmt=0x%x %dx%d) rec=(ifmt=0x%x %dx%d %s)",
                                 format, dest_fmt, level, xoffset, yoffset,
                                 width, height, dest_sz, e, rif, rw, rh,
                                 lf ? lf->internal_format : 0,
                                 lf ? lf->width : 0, lf ? lf->height : 0,
                                 lf && lf->transcoded ? "tr" : "-");
                    }
                }
            }
            free(dest);
            fsgl_err_end_extra("glCompressedTexSubImage2D");
            record_subupdate(target, level, format, dest_fmt, width, height, dest_sz);
            FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
            return;
        }
        if (dest && (dest_fmt == FSGL_R8 || dest_fmt == FSGL_RG8)) {
            PFN_glTexSubImage2D sub = get_texsubimage2d();
            if (sub) {
                sub(target, level, xoffset, yoffset, width, height,
                    dest_fmt == FSGL_R8 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, dest);
            }
            free(dest);
            fsgl_err_end_extra("glCompressedTexSubImage2D");
            record_subupdate(target, level, format, dest_fmt, width, height, dest_sz);
            FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
            return;
        }
        free(dest);
        fsgl_err_end_extra("glCompressedTexSubImage2D");
    }
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, false);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (lvl && lvl->decoded) {
        fsgl_err_begin_extra("glCompressedTexSubImage2D");
        unsigned char *rgba = decode_compressed(format, width, height, data, imageSize);
        PFN_glTexSubImage2D sub = get_texsubimage2d();
        if (rgba && sub) {
            probe_binding_mismatch("ctexsub-rgba", target);
            sub(target, level, xoffset, yoffset, width, height, GL_RGBA,
                GL_UNSIGNED_BYTE, rgba);
            PFN_glGetError geterr_d = FSGL_DIAG
                ? (PFN_glGetError)fsgl_real_id(FSGL_R_glGetError) : NULL;
            GLenum e_d = geterr_d ? geterr_d() : 0;
            if (e_d != 0) {
                static const char *dk;
                static int dkl;
                char key[64];
                int kn = snprintf(key, sizeof(key), "f%04x_%dx%d", format, width, height);
                if (!dk || kn != dkl || memcmp(key, dk, kn) != 0) {
                    dk = (const char *)memcpy(malloc(kn + 1), key, kn + 1);
                    dkl = kn;
                    PFN_glGetTexLevelParameteriv getlevel_d = get_texlevel();
                    GLint rif = 0, rw = 0, rh = 0;
                    if (getlevel_d) {
                        getlevel_d(target, level, GL_TEXTURE_INTERNAL_FORMAT, &rif);
                        getlevel_d(target, level, GL_TEXTURE_WIDTH, &rw);
                        getlevel_d(target, level, GL_TEXTURE_HEIGHT, &rh);
                    }
                    fsgl_log(ANDROID_LOG_ERROR,
                             "ctexsub-rgba FAIL fmt=0x%04x lvl=%d rect=(%d,%d %dx%d) "
                             "err=0x%x driver_level=(ifmt=0x%x %dx%d) "
                             "rec=(ifmt=0x%x %dx%d tr=%d dec=%d)",
                             format, level, xoffset, yoffset, width, height, e_d,
                             rif, rw, rh, lvl->internal_format, lvl->width,
                             lvl->height, lvl->transcoded, lvl->decoded);
                }
            }
            free(rgba);
            fsgl_err_end_extra("glCompressedTexSubImage2D");
            FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
            return;
        }
        free(rgba);
        fsgl_err_end_extra("glCompressedTexSubImage2D");
    }
    PFN_glCompressedTexSubImage2D real = get_ctexsubimage2d();
    if (real) {
        real(target, level, xoffset, yoffset, width, height, format, imageSize, data);
        FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
        fsgl_err_check("glCompressedTexSubImage2D",
                       "target=0x%x level=%d x=%d y=%d w=%d h=%d fmt=0x%x imageSize=%d",
                       (unsigned)target, (int)level, (int)xoffset, (int)yoffset,
                       (int)width, (int)height, (unsigned)format, (int)imageSize);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

void GL_APIENTRY fsgl_wrap_glCompressedTexSubImage3D(GLenum target, GLint level, GLint xoffset,
                                                  GLint yoffset, GLint zoffset, GLsizei width,
                                                  GLsizei height, GLsizei depth, GLenum format,
                                                  GLsizei imageSize, const void *data)
{
    FSGL_PROF_BEGIN(glCompressedTexSubImage3D);
    fsgl_ring_push("glCompressedTexSubImage3D", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)xoffset, (uintptr_t)yoffset);

    PFN_glCompressedTexSubImage3D real = get_ctexsubimage3d();
    if (real) {
        real(target, level, xoffset, yoffset, zoffset, width, height, depth, format,
             imageSize, data);
        FSGL_PROF_END(imageSize > 0 ? (uint64_t)imageSize : 0, 0);
        fsgl_err_check("glCompressedTexSubImage3D",
                       "target=0x%x level=%d x=%d y=%d z=%d w=%d h=%d d=%d fmt=0x%x imageSize=%d",
                       (unsigned)target, (int)level, (int)xoffset, (int)yoffset,
                       (int)zoffset, (int)width, (int)height, (int)depth,
                       (unsigned)format, (int)imageSize);
    } else {
        fsgl_log_once("ctexsub3d-absent", "compressed: glCompressedTexSubImage3D unsupported; skipped");
        FSGL_PROF_END(0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* glGetTexLevelParameteriv / glGetCompressedTexImage                  */
/* ------------------------------------------------------------------ */

void GL_APIENTRY fsgl_wrap_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname,
                                                 GLint *params)
{
    FSGL_PROF_BEGIN(glGetTexLevelParameteriv);
    fsgl_ring_push("glGetTexLevelParameteriv", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)pname, (uintptr_t)params);

    if (!params) {
        FSGL_PROF_END(0, 0);
        return;
    }
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, false);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (pname == GL_TEXTURE_COMPRESSED_IMAGE_SIZE) {
        if (lvl && lvl->transcoded && lvl->compressed) {
            *params = (GLint)lvl->compressed_size;
            fsgl_log_once("texlevel-size",
                          "texlevel: GL_TEXTURE_COMPRESSED_IMAGE_SIZE -> transcode size "
                          "(level %d)", level);
            FSGL_PROF_END(0, 0);
            return;
        }
        *params = (lvl && lvl->compressed) ? (GLint)lvl->compressed_size : 0;
        fsgl_log_once("texlevel-size",
                      "texlevel: GL_TEXTURE_COMPRESSED_IMAGE_SIZE -> tracked size (level %d)", level);
        FSGL_PROF_END(0, 0);
        return;
    }
    if (pname == GL_TEXTURE_COMPRESSED) {
        if (lvl && lvl->transcoded) {
            *params = lvl->compressed ? GL_TRUE : GL_FALSE;
            fsgl_log_once("texlevel-compressed",
                          "texlevel: GL_TEXTURE_COMPRESSED -> transcode flag (%d)", *params);
            FSGL_PROF_END(0, 0);
            return;
        }
        if (lvl) {
            *params = (lvl->compressed && !lvl->decoded) ? GL_TRUE : GL_FALSE;
            fsgl_log_once("texlevel-compressed",
                          "texlevel: GL_TEXTURE_COMPRESSED -> tracked flag (%d)", *params);
            FSGL_PROF_END(0, 0);
            return;
        }
        PFN_glGetTexLevelParameteriv real = get_texlevel();
        if (real) {
            real(target, level, pname, params);
            FSGL_PROF_END(0, 0);
            fsgl_err_check("glGetTexLevelParameteriv", "target=0x%x level=%d pname=0x%x",
                           (unsigned)target, (int)level, (unsigned)pname);
        } else {
            *params = GL_FALSE;
            FSGL_PROF_END(0, 0);
        }
        return;
    }
    PFN_glGetTexLevelParameteriv real = get_texlevel();
    if (real) {
        real(target, level, pname, params);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glGetTexLevelParameteriv", "target=0x%x level=%d pname=0x%x",
                       (unsigned)target, (int)level, (unsigned)pname);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

void GL_APIENTRY fsgl_wrap_glGetCompressedTexImage(GLenum target, GLint level, void *pixels)
{
    FSGL_PROF_BEGIN(glGetCompressedTexImage);
    fsgl_ring_push("glGetCompressedTexImage", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)pixels, 0);

    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, false);
    FsglTexLevel *lvl = fsgl_tex_level(rec, level);
    if (lvl && (lvl->decoded || lvl->transcoded) && pixels) {
        if (lvl->compressed_size > 0) {
            memset(pixels, 0, (size_t)lvl->compressed_size);
        }
        fsgl_log_once("getctex-decoded",
                      "readback: glGetCompressedTexImage on software-decoded texture %u level %d; "
                      "returning zeros (compressed source bytes are not retained)", id, level);
        FSGL_PROF_END(0, lvl->compressed_size > 0 ? (uint64_t)lvl->compressed_size : 0);
        return;
    }
    PFN_glGetCompressedTexImage real = get_getctex();
    if (real) {
        real(target, level, pixels);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glGetCompressedTexImage", "target=0x%x level=%d",
                       (unsigned)target, (int)level);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* glGenerateMipmap emulation for transcoded ETC2 textures             */
/* ------------------------------------------------------------------ */

/* GLES forbids glGenerateMipmap on compressed textures (it is legal on
 * desktop GL for S3TC, which is what the game expects), so for textures
 * this layer transcoded to ETC2 the chain is built in software: decode
 * the source level to RGBA8, box-filter, re-encode every level to ETC2
 * and upload it with the real glCompressedTexImage2D. */

void GL_APIENTRY fsgl_wrap_glGenerateMipmap(GLenum target)
{
    fsgl_ring_push("glGenerateMipmap", (uintptr_t)target, 0, 0, 0);

    PFN_glGenerateMipmap real = (PFN_glGenerateMipmap)fsgl_real_id(FSGL_R_glGenerateMipmap);
    if (!real) {
        return;
    }
    if (target != GL_TEXTURE_2D) {
        real(target);
        return;
    }
    GLuint id = fsgl_tex_bound(target);
    FsglTexRec *rec = fsgl_tex_get(id, false);
    FsglTexLevel *lvl0 = fsgl_tex_level(rec, 0);
    if (!lvl0 || !lvl0->transcoded || !lvl0->compressed ||
        (lvl0->internal_format != GL_ETC2_RGB8_OES &&
         lvl0->internal_format != GL_ETC2_RGBA8_OES)) {
        /* Uncompressed (R8/RG8 etc.) or untouched textures: the driver
         * generates mips natively. */
        real(target);
        fsgl_err_check("glGenerateMipmap", "target=0x%x", (unsigned)target);
        return;
    }

    PFN_glGetCompressedTexImage gct = get_getctex();
    PFN_glCompressedTexImage2D cti = get_cteximage2d();
    PFN_glGetTexParameteriv getp = (PFN_glGetTexParameteriv)fsgl_real_id(FSGL_R_glGetTexParameteriv);
    if (!gct || !cti) {
        real(target); /* will raise INVALID_OPERATION; the caller sees it */
        fsgl_err_check("glGenerateMipmap", "target=0x%x (no transcode entry points)",
                       (unsigned)target);
        return;
    }

    fsgl_err_begin_extra("glGenerateMipmap");

    int w = lvl0->width, h = lvl0->height;
    uint32_t dest_fmt = (uint32_t)lvl0->internal_format;
    GLint base_level = 0, max_level = 13;
    if (getp) {
        getp(target, GL_TEXTURE_BASE_LEVEL, &base_level);
        getp(target, GL_TEXTURE_MAX_LEVEL, &max_level);
        if (base_level < 0) base_level = 0;
        if (base_level > 13) base_level = 13;
        if (max_level > 13) max_level = 13;
        if (max_level < base_level) max_level = base_level;
    }
    int max_chain = 0, tw = w, th = h;
    while ((tw > 1 || th > 1) && max_chain < 13) {
        tw = tw > 1 ? tw / 2 : 1;
        th = th > 1 ? th / 2 : 1;
        max_chain++;
    }
    if (max_level > max_chain) {
        max_level = max_chain;
    }

    int ok = 0;
    size_t src_sz = fsgl_etc2_dest_size(dest_fmt, w, h);
    unsigned char *etc2 = (unsigned char *)malloc(src_sz ? src_sz : 1);
    unsigned char *cur = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    if (etc2 && cur) {
        memset(etc2, 0, src_sz);
        gct(target, base_level, etc2);
        if (fsgl_etc2_decode_rgba(dest_fmt, w, h, etc2, src_sz, cur) == 0) {
            ok = 1;
            int cw = w, ch = h;
            for (GLint l = base_level + 1; l <= max_level && ok; l++) {
                int nw = cw > 1 ? cw / 2 : 1;
                int nh = ch > 1 ? ch / 2 : 1;
                unsigned char *next = (unsigned char *)malloc((size_t)nw * (size_t)nh * 4u);
                if (!next) {
                    ok = 0;
                    break;
                }
                fsgl_etc2_box_filter_rgba(cur, cw, ch, next, nw, nh);
                size_t dsz = fsgl_etc2_dest_size(dest_fmt, nw, nh);
                unsigned char *blocks = (unsigned char *)malloc(dsz ? dsz : 1);
                if (blocks &&
                    fsgl_etc2_encode_rgba(dest_fmt, nw, nh, next, blocks) == 0) {
                    cti(target, (GLint)l, (GLint)dest_fmt, nw, nh, 0,
                        (GLsizei)dsz, blocks);
                    record_transcoded(target, (GLint)l, lvl0->src_format,
                                      dest_fmt, nw, nh, dsz);
                } else {
                    ok = 0;
                }
                free(blocks);
                free(cur);
                cur = next;
                cw = nw;
                ch = nh;
            }
            fsgl_log_once("genmipmap-etc2",
                          "mipmap: generated %d levels for transcoded ETC2 texture %u "
                          "(%dx%d fmt 0x%04x)", (int)(max_level - base_level), id,
                          w, h, dest_fmt);
        }
    }
    free(etc2);
    free(cur);
    fsgl_err_end_extra("glGenerateMipmap");
    if (!ok) {
        fsgl_log_once("genmipmap-fail",
                      "mipmap: software generation failed for texture %u "
                      "(out of memory or decode error)", id);
    }
}

/* ------------------------------------------------------------------ */
/* glGetTexImage via an FBO                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    SRC_RGBA8,
    SRC_RGBA32UI,
    SRC_DEPTH32,
    SRC_DEPTH24_STENCIL8
} SrcKind;

static bool is_integer_format(GLenum f)
{
    switch (f) {
    case 0x8D70: /* RGBA32UI */
    case 0x8D71: /* RGB32UI */
    case 0x8D76: /* RGBA16UI */
    case 0x8D77: /* RGB16UI */
    case 0x8D7C: /* RGBA8UI */
    case 0x8D7D: /* RGB8UI */
    case 0x8232: /* R8UI */
    case 0x8234: /* R16UI */
    case 0x8236: /* R32UI */
    case 0x8238: /* RG8UI */
    case 0x823A: /* RG16UI */
    case 0x823C: /* RG32UI */
        return true;
    default:
        return false;
    }
}

static bool is_depth_format(GLenum f)
{
    return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 ||
           f == GL_DEPTH_COMPONENT32F || f == GL_DEPTH24_STENCIL8 ||
           f == GL_DEPTH32F_STENCIL8;
}

static bool is_depth_stencil_format(GLenum f)
{
    return f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8;
}

static size_t out_pixel_size(GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_BYTE) {
        if (format == GL_RGBA || format == GL_BGRA) return 4;
        if (format == GL_RGB) return 3; /* not used for reads, but be explicit */
        if (format == GL_RG || format == GL_RG_INTEGER) return 2;
        if (format == GL_RED || format == GL_RED_INTEGER) return 1;
    }
    if (type == GL_UNSIGNED_SHORT) {
        if (format == GL_DEPTH_COMPONENT || format == GL_RED_INTEGER) return 2;
        if (format == GL_BGRA_INTEGER) return 8;
    }
    if (type == GL_UNSIGNED_INT) {
        if (format == GL_BGRA_INTEGER) return 16;
        if (format == GL_RG_INTEGER) return 8;
        if (format == GL_RED_INTEGER) return 4;
    }
    if (type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB) return 2;
    if (type == GL_UNSIGNED_INT_24_8 && format == GL_DEPTH_STENCIL) return 4;
    return 0;
}

static void convert_out(unsigned char *dst, const unsigned char *src, size_t pixels,
                        SrcKind kind, GLenum format, GLenum type, size_t out_bpp)
{
    if (kind == SRC_RGBA8) {
        if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
            memcpy(dst, src, pixels * 4);
        } else if (format == GL_BGRA && type == GL_UNSIGNED_BYTE) {
            for (size_t i = 0; i < pixels; i++) {
                uint32_t v;
                memcpy(&v, src + i * 4, 4);
                uint32_t o = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                             ((v & 0x000000FFu) << 16);
                memcpy(dst + i * 4, &o, 4);
            }
        } else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
            unsigned short *o = (unsigned short *)dst;
            for (size_t i = 0; i < pixels; i++) {
                unsigned int r = src[i * 4 + 0] >> 3;
                unsigned int g = src[i * 4 + 1] >> 2;
                unsigned int b = src[i * 4 + 2] >> 3;
                o[i] = (unsigned short)((r << 11) | (g << 5) | b);
            }
        } else if (format == GL_RED && type == GL_UNSIGNED_BYTE) {
            for (size_t i = 0; i < pixels; i++) dst[i] = src[i * 4];
        } else if (format == GL_RG && type == GL_UNSIGNED_BYTE) {
            for (size_t i = 0; i < pixels; i++) {
                dst[i * 2 + 0] = src[i * 4 + 0];
                dst[i * 2 + 1] = src[i * 4 + 1];
            }
        } else {
            memset(dst, 0, pixels * out_bpp);
        }
        return;
    }
    if (kind == SRC_RGBA32UI) {
        const unsigned int *s = (const unsigned int *)src;
        if (format == GL_BGRA_INTEGER && type == GL_UNSIGNED_INT) {
            unsigned int *o = (unsigned int *)dst;
            for (size_t i = 0; i < pixels; i++) {
                o[i * 4 + 0] = s[i * 4 + 2];
                o[i * 4 + 1] = s[i * 4 + 1];
                o[i * 4 + 2] = s[i * 4 + 0];
                o[i * 4 + 3] = s[i * 4 + 3];
            }
        } else if (format == GL_BGRA_INTEGER && type == GL_UNSIGNED_SHORT) {
            unsigned short *o = (unsigned short *)dst;
            for (size_t i = 0; i < pixels; i++) {
                o[i * 4 + 0] = (unsigned short)s[i * 4 + 2];
                o[i * 4 + 1] = (unsigned short)s[i * 4 + 1];
                o[i * 4 + 2] = (unsigned short)s[i * 4 + 0];
                o[i * 4 + 3] = (unsigned short)s[i * 4 + 3];
            }
        } else if (format == GL_RG_INTEGER && type == GL_UNSIGNED_INT) {
            unsigned int *o = (unsigned int *)dst;
            for (size_t i = 0; i < pixels; i++) {
                o[i * 2 + 0] = s[i * 4 + 0];
                o[i * 2 + 1] = s[i * 4 + 1];
            }
        } else if (format == GL_RED_INTEGER && type == GL_UNSIGNED_SHORT) {
            unsigned short *o = (unsigned short *)dst;
            for (size_t i = 0; i < pixels; i++) o[i] = (unsigned short)s[i * 4];
        } else {
            memset(dst, 0, pixels * out_bpp);
        }
        return;
    }
    if (kind == SRC_DEPTH32) {
        const unsigned int *s = (const unsigned int *)src;
        if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT) {
            unsigned short *o = (unsigned short *)dst;
            for (size_t i = 0; i < pixels; i++) o[i] = (unsigned short)(s[i] >> 16);
        } else if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_INT) {
            memcpy(dst, src, pixels * 4);
        } else {
            memset(dst, 0, pixels * out_bpp);
        }
        return;
    }
    if (kind == SRC_DEPTH24_STENCIL8) {
        if (format == GL_DEPTH_STENCIL && type == GL_UNSIGNED_INT_24_8) {
            memcpy(dst, src, pixels * 4);
        } else if (format == GL_DEPTH_COMPONENT && type == GL_UNSIGNED_SHORT) {
            const unsigned int *s = (const unsigned int *)src;
            unsigned short *o = (unsigned short *)dst;
            for (size_t i = 0; i < pixels; i++) o[i] = (unsigned short)((s[i] >> 8) >> 8);
        } else {
            memset(dst, 0, pixels * out_bpp);
        }
    }
}

/* Save/restore texture parameters used to make a non-mipmapped level complete. */
typedef struct {
    GLint base, max, minf, magf, compare, compare_func;
} TexParams;

static void tex_params_save(GLenum target, TexParams *p)
{
    PFN_glGetTexParameteriv getp =
        (PFN_glGetTexParameteriv)fsgl_real_id(FSGL_R_glGetTexParameteriv);
    memset(p, 0, sizeof(*p));
    if (!getp) return;
    getp(target, GL_TEXTURE_BASE_LEVEL, &p->base);
    getp(target, GL_TEXTURE_MAX_LEVEL, &p->max);
    getp(target, GL_TEXTURE_MIN_FILTER, &p->minf);
    getp(target, GL_TEXTURE_MAG_FILTER, &p->magf);
    getp(target, GL_TEXTURE_COMPARE_MODE, &p->compare);
    getp(target, GL_TEXTURE_COMPARE_FUNC, &p->compare_func);
}

static void tex_params_restore(GLenum target, const TexParams *p)
{
    PFN_glTexParameteri setp = (PFN_glTexParameteri)fsgl_real_id(FSGL_R_glTexParameteri);
    if (!setp) return;
    setp(target, GL_TEXTURE_BASE_LEVEL, p->base);
    setp(target, GL_TEXTURE_MAX_LEVEL, p->max);
    setp(target, GL_TEXTURE_MIN_FILTER, p->minf);
    setp(target, GL_TEXTURE_MAG_FILTER, p->magf);
    setp(target, GL_TEXTURE_COMPARE_MODE, p->compare);
    setp(target, GL_TEXTURE_COMPARE_FUNC, p->compare_func);
}

static void tex_params_force_level(GLenum target, GLint level, bool depth)
{
    PFN_glTexParameteri setp = (PFN_glTexParameteri)fsgl_real_id(FSGL_R_glTexParameteri);
    if (!setp) return;
    setp(target, GL_TEXTURE_BASE_LEVEL, level);
    setp(target, GL_TEXTURE_MAX_LEVEL, level);
    setp(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    setp(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (depth) {
        setp(target, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }
}

#define TEXI_DONE(bytes)                                                       \
    do {                                                                       \
        FSGL_PROF_END(0, (bytes));                                             \
        fsgl_err_end_extra("glGetTexImage");                                   \
        return;                                                                \
    } while (0)

void GL_APIENTRY fsgl_wrap_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type,
                                      void *pixels)
{
    FSGL_PROF_BEGIN(glGetTexImage);
    fsgl_ring_push("glGetTexImage", (uintptr_t)target, (uintptr_t)level,
                   (uintptr_t)format, (uintptr_t)type);
    fsgl_err_begin_extra("glGetTexImage");

    PFN_glGetTexLevelParameteriv getlevel = get_texlevel();
    if (!getlevel) {
        fsgl_log_once("getteximage-nolevel", "readback: glGetTexImage unavailable (no glGetTexLevelParameteriv)");
        TEXI_DONE(0);
    }
    GLint internal = 0, width = 0, height = 0, depth = 0;
    GLenum level_target = (target == GL_TEXTURE_CUBE_MAP) ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : target;
    getlevel(level_target, level, GL_TEXTURE_INTERNAL_FORMAT, &internal);
    getlevel(level_target, level, GL_TEXTURE_WIDTH, &width);
    getlevel(level_target, level, GL_TEXTURE_HEIGHT, &height);
    if (target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY) {
        getlevel(level_target, level, GL_TEXTURE_DEPTH, &depth);
    }
    if (target == GL_TEXTURE_CUBE_MAP) {
        depth = 6;
    }
    if (depth <= 0) {
        depth = 1;
    }
    if (width <= 0 || height <= 0) {
        fsgl_log_once("getteximage-empty", "readback: glGetTexImage target 0x%04x level %d has no extent", target, level);
        TEXI_DONE(0);
    }

    SrcKind kind;
    GLenum read_format = GL_RGBA;
    GLenum read_type = GL_UNSIGNED_BYTE;
    GLenum attach_point = GL_COLOR_ATTACHMENT0;
    size_t src_bpp = 4;
    bool depth_tex = is_depth_format((GLenum)internal);
    if (depth_tex) {
        attach_point = is_depth_stencil_format((GLenum)internal) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        read_format = is_depth_stencil_format((GLenum)internal) ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
        read_type = is_depth_stencil_format((GLenum)internal) ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT;
        kind = is_depth_stencil_format((GLenum)internal) ? SRC_DEPTH24_STENCIL8 : SRC_DEPTH32;
    } else if (is_integer_format((GLenum)internal)) {
        kind = SRC_RGBA32UI;
        read_format = GL_RGBA_INTEGER;
        read_type = GL_UNSIGNED_INT;
        src_bpp = 16;
    } else {
        kind = SRC_RGBA8;
        src_bpp = 4;
    }

    size_t pixels_count = (size_t)width * (size_t)height * (size_t)depth;
    size_t src_size = pixels_count * src_bpp;

    /* ETC2 textures cannot attach to an FBO: read the compressed bytes and
     * decode them in software.  Only 2-D images (the atlases) are handled;
     * anything else returns zeros like the rev-2 decoded path. */
    {
        GLuint id_rb = fsgl_tex_bound(target);
        FsglTexRec *rec_rb = fsgl_tex_get(id_rb, false);
        FsglTexLevel *lvl_rb = fsgl_tex_level(rec_rb, level);
        if (lvl_rb && lvl_rb->transcoded && lvl_rb->compressed) {
            size_t out_bpp = out_pixel_size(format, type);
            PFN_glGetCompressedTexImage gct = get_getctex();
            if (!gct || out_bpp == 0 || depth != 1) {
                if (depth == 1 && out_bpp > 0 && pixels) {
                    memset(pixels, 0, pixels_count * out_bpp);
                }
                fsgl_log_once("getteximage-etc2-short",
                              "readback: glGetTexImage on ETC2 texture depth=%d fmt=0x%x: "
                              "no software path; returning zeros", depth, format);
                TEXI_DONE(depth == 1 && out_bpp > 0 ? pixels_count * out_bpp : 0);
            }
            size_t blocks = (size_t)((width + 3) / 4) * (size_t)((height + 3) / 4);
            size_t etc2_sz = blocks * (lvl_rb->internal_format == GL_ETC2_RGBA8_OES ? 16u : 8u);
            unsigned char *etc2 = (unsigned char *)malloc(etc2_sz);
            unsigned char *rgba = (unsigned char *)malloc(pixels_count * 4u);
            if (etc2 && rgba) {
                memset(etc2, 0, etc2_sz);
                gct(level_target, level, etc2);
                fsgl_err_check("glGetTexImage", "target=0x%x level=%d (ETC2 readback)",
                               (unsigned)target, (int)level);
                if (fsgl_etc2_decode_rgba(lvl_rb->internal_format, width, height,
                                          etc2, etc2_sz, rgba) == 0) {
                    if (pixels) {
                        convert_out((unsigned char *)pixels, rgba, pixels_count,
                                    SRC_RGBA8, format, type, out_bpp);
                    }
                    free(etc2);
                    free(rgba);
                    TEXI_DONE(pixels_count * out_bpp);
                }
            }
            free(etc2);
            free(rgba);
            if (pixels) {
                memset(pixels, 0, pixels_count * out_bpp);
            }
            fsgl_log_once("getteximage-etc2-fail",
                          "readback: glGetTexImage ETC2 decode failed; returning zeros");
            TEXI_DONE(pixels_count * out_bpp);
        }
    }

    unsigned char *src = (unsigned char *)malloc(src_size);
    if (!src) {
        TEXI_DONE(0);
    }
    memset(src, 0, src_size);

    PFN_glGenFramebuffers gen_fbo = (PFN_glGenFramebuffers)fsgl_real_id(FSGL_R_glGenFramebuffers);
    PFN_glDeleteFramebuffers del_fbo = (PFN_glDeleteFramebuffers)fsgl_real_id(FSGL_R_glDeleteFramebuffers);
    PFN_glBindFramebuffer bind_fbo = (PFN_glBindFramebuffer)fsgl_real_id(FSGL_R_glBindFramebuffer);
    PFN_glFramebufferTexture2D attach2d = (PFN_glFramebufferTexture2D)fsgl_real_id(FSGL_R_glFramebufferTexture2D);
    PFN_glFramebufferTextureLayer attach_layer = (PFN_glFramebufferTextureLayer)fsgl_real_id(FSGL_R_glFramebufferTextureLayer);
    PFN_glCheckFramebufferStatus check_fbo = (PFN_glCheckFramebufferStatus)fsgl_real_id(FSGL_R_glCheckFramebufferStatus);
    PFN_glReadPixels read_pixels = (PFN_glReadPixels)fsgl_real_id(FSGL_R_glReadPixels);
    PFN_glGetIntegerv gi = get_int();
    PFN_glPixelStorei ps = get_pixelstore();

    if (!gen_fbo || !del_fbo || !bind_fbo || !check_fbo || !read_pixels || !gi || !ps) {
        free(src);
        fsgl_log_once("getteximage-nofbo", "readback: FBO readback entry points missing; skipped");
        TEXI_DONE(0);
    }

    GLint old_read = 0, old_draw = 0, old_pack = 0, old_pbo = 0;
    GLint old_row = 0, old_skip_rows = 0, old_skip_pixels = 0;
    gi(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    gi(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    gi(GL_PACK_ALIGNMENT, &old_pack);
    gi(GL_PIXEL_PACK_BUFFER_BINDING, &old_pbo);
    gi(GL_PACK_ROW_LENGTH, &old_row);
    gi(GL_PACK_SKIP_ROWS, &old_skip_rows);
    gi(GL_PACK_SKIP_PIXELS, &old_skip_pixels);

    GLuint fbo = 0;
    GLuint texture = fsgl_tex_bound(target);
    if (!texture) {
        free(src);
        fsgl_log_once("getteximage-notexture", "readback: glGetTexImage target 0x%04x has no bound texture; skipped", target);
        TEXI_DONE(0);
    }
    gen_fbo(1, &fbo);
    bind_fbo(GL_FRAMEBUFFER, fbo);
    if (depth_tex) {
        /* A depth/depth-stencil-only FBO is incomplete while the default draw
         * buffer still points at COLOR_ATTACHMENT0.  The draw-buffer state is
         * per-FBO, so this does not disturb the caller's FBO. */
        PFN_glDrawBuffers draw_buffers = (PFN_glDrawBuffers)fsgl_real_id(FSGL_R_glDrawBuffers);
        if (draw_buffers) {
            GLenum none = GL_NONE;
            draw_buffers(1, &none);
        }
    }
    ps(GL_PACK_ALIGNMENT, 1);
    ps(GL_PACK_ROW_LENGTH, 0);
    ps(GL_PACK_SKIP_ROWS, 0);
    ps(GL_PACK_SKIP_PIXELS, 0);
    if (old_pbo) {
        PFN_glBindBuffer bind_buf = (PFN_glBindBuffer)fsgl_real_id(FSGL_R_glBindBuffer);
        if (bind_buf) bind_buf(GL_PIXEL_PACK_BUFFER, 0);
    }

    TexParams params;
    bool params_saved = false;
    for (GLsizei z = 0; z < depth; z++) {
        bool attached = false;
        if (target == GL_TEXTURE_CUBE_MAP && attach2d) {
            GLenum face = (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + z);
            attach2d(GL_FRAMEBUFFER, attach_point, face, texture, level);
            attached = true;
        } else if ((target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY) && attach_layer) {
            attach_layer(GL_FRAMEBUFFER, attach_point, texture, level, z);
            attached = true;
        } else if (attach2d) {
            attach2d(GL_FRAMEBUFFER, attach_point, target, texture, level);
            attached = true;
        }
        if (!attached) {
            fsgl_log_once("getteximage-attach", "readback: cannot attach target 0x%04x level %d", target, level);
            break;
        }
        GLenum status = check_fbo(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            if (!params_saved) {
                tex_params_save(target, &params);
                params_saved = true;
            }
            tex_params_force_level(target, level, depth_tex);
            status = check_fbo(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                fsgl_log(ANDROID_LOG_WARN, "readback: FBO incomplete 0x%04x for target 0x%04x level %d (depth=%d)",
                         status, target, level, z);
            }
        }
        read_pixels(0, 0, width, height, read_format, read_type,
                    src + (size_t)z * (size_t)width * (size_t)height * src_bpp);
    }

    if (params_saved) {
        tex_params_restore(target, &params);
    }

    del_fbo(1, &fbo);
    bind_fbo(GL_READ_FRAMEBUFFER, (GLuint)old_read);
    bind_fbo(GL_DRAW_FRAMEBUFFER, (GLuint)old_draw);
    ps(GL_PACK_ALIGNMENT, old_pack);
    ps(GL_PACK_ROW_LENGTH, old_row);
    ps(GL_PACK_SKIP_ROWS, old_skip_rows);
    ps(GL_PACK_SKIP_PIXELS, old_skip_pixels);
    if (old_pbo) {
        PFN_glBindBuffer bind_buf = (PFN_glBindBuffer)fsgl_real_id(FSGL_R_glBindBuffer);
        if (bind_buf) bind_buf(GL_PIXEL_PACK_BUFFER, (GLuint)old_pbo);
    }

    size_t out_bpp = out_pixel_size(format, type);
    if (out_bpp == 0) {
        fsgl_log_once("getteximage-unsupported",
                      "readback: unsupported output format 0x%04x type 0x%04x; skipped", format, type);
        free(src);
        TEXI_DONE(0);
    }
    size_t out_size = pixels_count * out_bpp;
    unsigned char *out = (unsigned char *)malloc(out_size);
    if (!out) {
        free(src);
        TEXI_DONE(0);
    }
    convert_out(out, src, pixels_count, kind, format, type, out_bpp);

    if (pixels) {
        memcpy(pixels, out, out_size);
    } else if (old_pbo) {
        PFN_glBufferSubData sub = (PFN_glBufferSubData)fsgl_real_id(FSGL_R_glBufferSubData);
        if (sub) {
            sub(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)out_size, out);
        }
    }

    fsgl_log_once("getteximage-path",
                  "readback: glGetTexImage via FBO (target 0x%04x level %d %dx%dx%d fmt 0x%04x/0x%04x src %d pbo %d)",
                  target, level, width, height, depth, format, type, (int)kind, old_pbo);
    free(out);
    free(src);
    TEXI_DONE(out_size);
}
