/* Shared declarations for the Factorio GL 3.3 -> GLES 3.2 compatibility layer.
 *
 * All real GLES entry points are obtained privately through eglGetProcAddress
 * first and dlsym(libGLESv3.so) second.  fsgl_lookup() only ever returns one
 * of our wrapper functions; it must never return itself or a real GLES
 * entry point, because SDL treats NULL as "ask EGL".
 *
 * rev-2: every wrapper resolves its real entry point through fsgl_real_id(),
 * which caches the result in a small enum-indexed table.  fsgl_real() itself
 * (which does eglGetProcAddress/dlsym) must only be used at init time.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <GLES3/gl32.h>

/* Desktop map flags are not consistently exposed by all Android GLES
 * headers, although the GLES 3 entry point accepts the same values. */
#ifndef GL_MAP_INVALIDATE_RANGE_BIT
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#ifndef GL_MAP_FLUSH_EXPLICIT_BIT
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0040
#endif

/* Diagnostics build switch (CMake option GLCOMPAT_DIAG).
 *
 * FSGL_DIAG=1 keeps the full rev-2 instrumentation: per-wrapper profiler
 * (two clock reads per call), the call ring, a real glGetError() after every
 * forwarded call, the periodic stream report and the timing-only stall probes
 * (glClientWaitSync, glFinish, ...).
 *
 * FSGL_DIAG=0 (release) compiles all of that out.  Wrappers then do only the
 * translation work they exist for, and entry points that were wrapped purely
 * for bookkeeping are handed to the game as the driver's own pointers.  Layer
 * internal error hygiene (fsgl_err_begin_extra/end_extra) stays in both. */
#ifndef FSGL_DIAG
#define FSGL_DIAG 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Public, default-visibility entry point. */
void *fsgl_lookup(const char *name);

/* Private helpers. */
typedef void (*FSGLProc)(void);

FSGLProc fsgl_real(const char *name);
bool fsgl_have(const char *name);

/* ------------------------------------------------------------------ */
/* Real entry point cache                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    FSGL_R_glGetString,
    FSGL_R_glGetStringi,
    FSGL_R_glGetIntegerv,
    FSGL_R_glGetError,

    FSGL_R_glShaderSource,
    FSGL_R_glCompileShader,
    FSGL_R_glGetShaderiv,
    FSGL_R_glGetShaderInfoLog,
    FSGL_R_glDeleteShader,

    FSGL_R_glBindTexture,
    FSGL_R_glGenTextures,
    FSGL_R_glDeleteTextures,
    FSGL_R_glBindBuffer,
    FSGL_R_glBufferData,
    FSGL_R_glBufferSubData,
    FSGL_R_glDeleteBuffers,
    FSGL_R_glMapBufferRange,
    FSGL_R_glGetBufferParameteriv,
    FSGL_R_glUnmapBuffer,
    FSGL_R_glFlushMappedBufferRange,
    FSGL_R_glFenceSync,
    FSGL_R_glClientWaitSync,
    FSGL_R_glDeleteSync,
    FSGL_R_glBindVertexArray,
    FSGL_R_glVertexAttribPointer,
    FSGL_R_glEnableVertexAttribArray,
    FSGL_R_glDisableVertexAttribArray,

    FSGL_R_glDrawArrays,
    FSGL_R_glDrawElements,
    FSGL_R_glDrawArraysInstanced,
    FSGL_R_glUseProgram,
    FSGL_R_glDeleteProgram,
    FSGL_R_glUniform1i,
    FSGL_R_glGetUniformLocation,
    FSGL_R_glGetAttribLocation,
    FSGL_R_glLinkProgram,
    FSGL_R_glViewport,
    FSGL_R_glScissor,

    FSGL_R_glDebugMessageCallback,
    FSGL_R_glDebugMessageCallbackKHR,
    FSGL_R_glDebugMessageControl,
    FSGL_R_glDebugMessageControlKHR,
    FSGL_R_glColorMaski,
    FSGL_R_glCopyImageSubData,
    FSGL_R_glTexBuffer,

    FSGL_R_glTexImage2D,
    FSGL_R_glTexSubImage2D,
    FSGL_R_glTexImage3D,
    FSGL_R_glTexSubImage3D,
    FSGL_R_glCompressedTexImage2D,
    FSGL_R_glCompressedTexSubImage2D,
    FSGL_R_glCompressedTexSubImage3D,
    FSGL_R_glGetTexLevelParameteriv,
    FSGL_R_glGetCompressedTexImage,
    FSGL_R_glPixelStorei,
    FSGL_R_glGetTexParameteriv,

    FSGL_R_glTexParameteri,
    FSGL_R_glTexParameterf,
    FSGL_R_glTexParameterfv,
    FSGL_R_glTexParameteriv,
    FSGL_R_glSamplerParameteri,
    FSGL_R_glSamplerParameterf,
    FSGL_R_glSamplerParameterfv,
    FSGL_R_glSamplerParameteriv,

    FSGL_R_glGenFramebuffers,
    FSGL_R_glDeleteFramebuffers,
    FSGL_R_glBindFramebuffer,
    FSGL_R_glFramebufferTexture2D,
    FSGL_R_glFramebufferTextureLayer,
    FSGL_R_glCheckFramebufferStatus,
    FSGL_R_glReadPixels,
    FSGL_R_glDrawBuffers,

    FSGL_R_glQueryCounterEXT,
    FSGL_R_glGetQueryObjecti64vEXT,
    FSGL_R_glGetQueryObjectui64vEXT,
    FSGL_R_glGetQueryObjectivEXT,

    FSGL_R_glGenerateMipmap,

    FSGL_R_glWaitSync,
    FSGL_R_glFinish,
    FSGL_R_glFlush,

    /* frametrace.c (FSGL_DIAG) */
    FSGL_R_glClear,
    FSGL_R_glClearBufferfv,
    FSGL_R_glClearBufferfi,
    FSGL_R_glInvalidateFramebuffer,
    FSGL_R_glBlitFramebuffer,
    FSGL_R_glDrawElementsInstanced,
    FSGL_R_glDrawElementsBaseVertex,
    FSGL_R_glDrawRangeElements,
    FSGL_R_glAttachShader,

    FSGL_R_COUNT
} FsglRealId;

/* Resolve once (relaxed atomics; benign race if two threads race first use:
 * both resolve the same address).  The inline fast path is a single relaxed
 * load; only the first call per entry point enters fsgl_real_slow(). */
extern uintptr_t fsgl_real_cache[FSGL_R_COUNT];
FSGLProc fsgl_real_slow(FsglRealId id);

static inline FSGLProc fsgl_real_id(FsglRealId id)
{
    uintptr_t v = __atomic_load_n(&fsgl_real_cache[id], __ATOMIC_RELAXED);
    if (v == 0) {
        return fsgl_real_slow(id);
    }
    return v == 1u ? NULL : (FSGLProc)v;
}

void fsgl_log(int prio, const char *fmt, ...);
/* Logcat under `tag`; FSGL_DIAG builds also append to glcompat.log next to
 * glcompat.ini (logcat drops lines on some vendor builds). */
void fsgl_vlog(int prio, const char *tag, const char *fmt, va_list ap);
void fsgl_log_once_impl(const char *key, const char *fmt, ...);
void fsgl_err_log(int prio, const char *fmt, ...);

/* Log once per key.  Several wrappers call this on every invocation, so each
 * call site keeps its own flag and only the first visit pays for the key
 * table scan and argument formatting.  Keys shared between sites are still
 * de-duplicated by the table.  Use fsgl_log_once_impl() directly when one
 * site logs several distinct (non-literal) keys. */
#define fsgl_log_once(key, ...)                                                \
    do {                                                                       \
        static bool fsgl_once_done_;                                           \
        if (!fsgl_once_done_) {                                                \
            fsgl_once_done_ = true;                                            \
            fsgl_log_once_impl((key), __VA_ARGS__);                            \
        }                                                                      \
    } while (0)

/* ------------------------------------------------------------------ */
/* Profiler                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    FSGL_P_glGetString,
    FSGL_P_glGetStringi,
    FSGL_P_glGetIntegerv,
    FSGL_P_glGetError,

    FSGL_P_glShaderSource,
    FSGL_P_glCompileShader,
    FSGL_P_glDeleteShader,

    FSGL_P_glBindTexture,
    FSGL_P_glDeleteTextures,
    FSGL_P_glBindBuffer,
    FSGL_P_glBufferData,
    FSGL_P_glDeleteBuffers,
    FSGL_P_glMapBuffer,
    FSGL_P_glMapBufferRange,
    FSGL_P_glUnmapBuffer,
    FSGL_P_glFlushMappedBufferRange,
    FSGL_P_glBindVertexArray,
    FSGL_P_glVertexAttribPointer,

    FSGL_P_glDrawArrays,
    FSGL_P_glDrawElements,
    FSGL_P_glQueryCounter,
    FSGL_P_glGetQueryObjecti64v,
    FSGL_P_glDrawArraysInstancedBaseInstance,
    FSGL_P_glUseProgram,
    FSGL_P_glDeleteProgram,
    FSGL_P_glGetUniformLocation,
    FSGL_P_glGetAttribLocation,
    FSGL_P_glLinkProgram,
    FSGL_P_glViewport,
    FSGL_P_glScissor,
    FSGL_P_glPixelStorei,

    FSGL_P_glDebugMessageCallback,
    FSGL_P_glDebugMessageControl,
    FSGL_P_glColorMaski,
    FSGL_P_glCopyImageSubData,
    FSGL_P_glTexBuffer,

    FSGL_P_glTexImage2D,
    FSGL_P_glTexSubImage2D,
    FSGL_P_glTexImage3D,
    FSGL_P_glTexSubImage3D,
    FSGL_P_glCompressedTexImage2D,
    FSGL_P_glCompressedTexSubImage2D,
    FSGL_P_glCompressedTexSubImage3D,
    FSGL_P_glGetTexImage,
    FSGL_P_glGetCompressedTexImage,
    FSGL_P_glGetTexLevelParameteriv,

    FSGL_P_glTexParameteri,
    FSGL_P_glTexParameterf,
    FSGL_P_glTexParameterfv,
    FSGL_P_glTexParameteriv,
    FSGL_P_glSamplerParameteri,
    FSGL_P_glSamplerParameterf,
    FSGL_P_glSamplerParameterfv,
    FSGL_P_glSamplerParameteriv,

    /* Timing-only stall probes (FSGL_DIAG builds).  These calls are not
     * otherwise wrapped, so a CPU<->GPU sync hidden in them would never show
     * up in the profile. */
    FSGL_P_glClientWaitSync,
    FSGL_P_glWaitSync,
    FSGL_P_glFinish,
    FSGL_P_glFlush,
    FSGL_P_glReadPixels,
    FSGL_P_glBufferSubData,

    FSGL_P_COUNT
} FsglProfId;

typedef struct {
    const char *name;
    uint64_t calls;
    uint64_t total_ns;
    uint64_t max_ns;
    uint64_t bytes_in;
    uint64_t bytes_out;
    bool has_bytes;
} FsglProf;

extern FsglProf fsgl_prof_table[FSGL_P_COUNT];

uint64_t fsgl_now_ns(void);

/* Starts the 10 s dump thread once; safe to call repeatedly. */
void fsgl_prof_start(void);

#if FSGL_DIAG
/* Exactly two clock reads per intercepted call.  No logging in the hot path. */
#define FSGL_PROF_BEGIN(pid)                                                   \
    FsglProf *fsgl_pf = &fsgl_prof_table[FSGL_P_##pid];                        \
    uint64_t fsgl_t0 = fsgl_now_ns()

#define FSGL_PROF_END(_bin, _bout)                                             \
    do {                                                                       \
        uint64_t fsgl_dt = fsgl_now_ns() - fsgl_t0;                            \
        fsgl_pf->calls++;                                                      \
        fsgl_pf->total_ns += fsgl_dt;                                          \
        if (fsgl_dt > fsgl_pf->max_ns) {                                       \
            fsgl_pf->max_ns = fsgl_dt;                                         \
        }                                                                      \
        fsgl_pf->bytes_in += (uint64_t)(_bin);                                 \
        fsgl_pf->bytes_out += (uint64_t)(_bout);                               \
    } while (0)
#else
#define FSGL_PROF_BEGIN(pid) ((void)0)
#define FSGL_PROF_END(_bin, _bout) ((void)(_bin), (void)(_bout))
#endif

/* ------------------------------------------------------------------ */
/* Error attribution                                                   */
/* ------------------------------------------------------------------ */

/* Per-thread ring of the last 8 intercepted calls: name + up to 4 integer
 * arguments.  No formatting in the hot path. */
#if FSGL_DIAG
void fsgl_ring_push(const char *name, uintptr_t a0, uintptr_t a1,
                    uintptr_t a2, uintptr_t a3);
#else
static inline void fsgl_ring_push(const char *name, uintptr_t a0, uintptr_t a1,
                                  uintptr_t a2, uintptr_t a3)
{
    (void)name; (void)a0; (void)a1; (void)a2; (void)a3;
}
#endif

/* Wrapper for glGetError: returns queued errors first, logs the ring once per
 * distinct caller return address (rate limited), then the driver error.
 * `caller` must be the return address of the game's call into the wrapper. */
GLenum fsgl_err_wrap_get_error(uintptr_t caller);

/* For wrappers that perform layer-internal GL calls.  begin drains any
 * pre-existing error (preserving it for the caller's next glGetError);
 * end drains errors produced by our own internal calls and logs them at
 * DEBUG without returning them to the caller. */
void fsgl_err_begin_extra(const char *name);
void fsgl_err_end_extra(const char *name);

/* Call immediately after the caller's forwarded driver call (and before any
 * internal restore calls).  Consumes that call's error, logs it with the
 * supplied full parameters (rate limited per (name,error) pair), and queues
 * it so the caller still observes it from glGetError.
 *
 * Release builds compile this out: the driver keeps the error in its own
 * flag, the game reads it through the glGetError wrapper as before, and the
 * extra glGetError per forwarded call disappears. */
#if FSGL_DIAG
void fsgl_err_check(const char *name, const char *params_fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
#define fsgl_err_check(...) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Extension list                                                      */
/* ------------------------------------------------------------------ */

void fsgl_ext_ensure(void);
GLuint fsgl_ext_count(void);
const char *fsgl_ext_at(GLuint index);
const char *fsgl_ext_string(void);
bool fsgl_has_ext(const char *name);

/* Extension predicates that the upload path needs, resolved once while the
 * extension list is built from the DRIVER's extensions (before any synthetic
 * entry is added).  No per-upload strcmp walk. */
typedef struct {
    bool s3tc;
    bool rgtc;
} FsglExtCaps;
const FsglExtCaps *fsgl_ext_caps(void);

/* Shader translation.  Returns a malloc()ed string (caller frees) or NULL on
 * allocation failure.  *changed is set when the source was modified. */
char *fsgl_translate_shader(const char *source, GLenum stage, bool *changed);

/* ------------------------------------------------------------------ */
/* Texture/buffer/program state (single GL thread in the game)         */
/* ------------------------------------------------------------------ */
#define FSGL_MAX_TEX_LEVELS 16

typedef struct {
    GLenum internal_format;
    GLenum format;
    GLenum type;
    GLsizei width;
    GLsizei height;
    GLsizei depth;
    GLsizei compressed_size;
    bool compressed;
    bool decoded;
    /* rev-3: level was rewritten from a DXT/BC source by the ETC2
     * transcoder.  internal_format holds the destination format
     * (ETC2 RGB8/RGBA8 for colour, R8/RG8 for single/dual-channel);
     * src_format is the original DXT/BC format of the last upload. */
    bool transcoded;
    GLenum src_format;
} FsglTexLevel;

/* Records live in a growable array indexed by GL name: one realloc when the
 * table grows, never a malloc per texture.  `id != 0` marks a live record.
 * The array may move when it grows, so do not hold a pointer across another
 * fsgl_tex_get() call that could create a new texture. */
typedef struct FsglTexRec {
    GLuint id;
    FsglTexLevel levels[FSGL_MAX_TEX_LEVELS];
} FsglTexRec;

FsglTexRec *fsgl_tex_get(GLuint id, bool create);
void fsgl_tex_remove(GLuint id);
GLuint fsgl_tex_bound(GLenum target);
void fsgl_tex_bind(GLenum target, GLuint id);
FsglTexLevel *fsgl_tex_level(FsglTexRec *rec, GLint level);

/* Texture name redirection: when the driver rejects a compressed
 * re-definition of a level, the layer uploads to a fresh texture object
 * and binds the replacement under the game's name. */
void fsgl_tex_redirect_add(GLenum target, GLuint orig, GLuint alt);
GLuint fsgl_tex_redirect_lookup(GLenum target, GLuint orig);
void fsgl_tex_redirect_remove(GLuint orig);

GLuint fsgl_buf_bound(GLenum target);
void fsgl_buf_bind(GLenum target, GLuint id);
void fsgl_buf_set_size(GLenum target, GLsizeiptr size);
GLsizeiptr fsgl_buf_get_size(GLuint id);
void fsgl_buf_remove(GLuint id);

/* ------------------------------------------------------------------ */
/* Buffer streaming (vertex/index ring buffers)                        */
/* ------------------------------------------------------------------ */
/* Adreno hands UNSYNCHRONIZED maps straight into the buffer object, and
 * its tiler consumes vertex data lazily, so the vendor driver's own
 * synchronisation is both slow (it drains the pipeline on every plain map)
 * and, for draws issued while a range is mapped, unsafe.  stream.c keeps
 * the bookkeeping needed to (a) diagnose that pattern and (b) make reuse
 * safe without the per-map driver stall. */

/* Called before/after the real glMapBufferRange.  Returns an opaque slot
 * (>= 0) for the open map, or -1.  fsgl_stream_adjust_access() applies the
 * glcompat.ini access knobs ("safe", "invalidate_range"). */
GLbitfield fsgl_stream_adjust_access(GLbitfield access);
void fsgl_stream_trace(char kind, GLuint buf, long long off, long long len,
                      long long arg);
int fsgl_stream_begin_map(GLenum target, GLuint buf, GLintptr offset, GLsizeiptr length,
                          GLbitfield access);
void fsgl_stream_mapped(int slot, void *ptr);
int fsgl_stream_find_map(GLenum target, GLuint buf);
int fsgl_stream_find_map_recent(GLenum target);
GLuint fsgl_stream_map_buffer(int slot);
bool fsgl_stream_map_driver_mapped(int slot);
void fsgl_stream_end_map(int slot, GLboolean ok);
void fsgl_stream_flush(GLenum target, GLuint buf, GLintptr offset, GLsizeiptr length);

void fsgl_stream_note_buffer_data(GLenum target, GLuint buf, GLsizeiptr size);
void fsgl_stream_drop_buffer(GLuint buf);

/* Vertex-array tracking, needed to know which buffers a draw reads. */
void fsgl_stream_bind_vao(GLuint vao);
GLuint fsgl_stream_current_vao(void);
void fsgl_stream_attrib_pointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                                const void *ptr);
void fsgl_stream_attrib_enable(GLuint index, bool on);
void fsgl_stream_element_binding(GLenum target, GLuint buf);

/* Draw bookkeeping.  ebo is the element buffer used by the draw (0 for
 * glDrawArrays); it is read from the tracked VAO. */
void fsgl_stream_note_draw(const char *kind, GLuint ebo, GLsizei count,
                           const void *indices);

void fsgl_stream_report(void);

/* frametrace.c (FSGL_DIAG): draw hook for the wrapped draw calls. */
void fsgl_ft_draw(bool instanced, bool base_vertex);

/* Translated source and stage of a shader the layer has seen, or NULL. */
const char *fsgl_shader_source(GLuint shader, GLenum *stage);

/* glcompat.ini "fake_s3tc": advertise GL_EXT_texture_compression_s3tc and
 * _rgtc although the driver lacks them; uploads are transcoded (wrap_tex.c).
 * Loads the config on first use. */
bool fsgl_config_fake_s3tc(void);

/* glcompat.ini "invalidate_ds": invalidate depth/stencil of the draw
 * framebuffer whenever the game switches to another one, so a tiler neither
 * stores them at the end of the pass nor reloads them when it resumes. */
bool fsgl_config_invalidate_ds(void);

/* Pixel-store shadow state.  GL defaults are queried once, then kept in sync
 * by the glPixelStorei wrapper and by the layer's own tight/restore calls. */
typedef struct {
    GLint alignment;
    GLint row_length;
    GLint skip_rows;
    GLint skip_pixels;
    GLint image_height;
    GLint skip_images;
} FsglPixelState;

void fsgl_pixel_state_get_pack(FsglPixelState *s);
void fsgl_pixel_state_get_unpack(FsglPixelState *s);
/* Calls the real glPixelStorei and updates the shadow state on success. */
void fsgl_pixel_store_apply(GLenum pname, GLint param);

void fsgl_use_program(GLuint program);
GLuint fsgl_current_program(void);

/* Viewport/scissor shadow state, updated by the wrappers. */
void fsgl_viewport_set(GLint x, GLint y, GLsizei w, GLsizei h);
bool fsgl_viewport_get(GLint out[4]);
void fsgl_scissor_set(GLint x, GLint y, GLsizei w, GLsizei h);
bool fsgl_scissor_get(GLint out[4]);

/* Per-program uniform/attribute location cache.  Keyed by (program, name,
 * kind); invalidated on glLinkProgram and glDeleteProgram.  A hit performs no
 * driver call. */
GLint fsgl_uniform_location(GLuint program, const char *name);
GLint fsgl_attrib_location(GLuint program, const char *name);
void fsgl_program_locations_invalidate(GLuint program);

/* fsgl_BaseInstance emulation is only queried once a nonzero baseinstance has
 * actually been used, so the common path performs no driver call per draw. */
void fsgl_baseinstance_mark_used(void);
bool fsgl_baseinstance_used(void);
bool fsgl_baseinstance_dirty(void);
void fsgl_baseinstance_set_dirty(bool dirty);
GLint fsgl_baseinstance_location(GLuint program);

/* Real GLES helpers used by more than one wrapper.  These are resolved lazily
 * and never point at a wrapper. */
typedef void (GL_APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void (GL_APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (GL_APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (GL_APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (GL_APIENTRY *PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (GL_APIENTRY *PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef void (GL_APIENTRY *PFN_glGetIntegerv)(GLenum, GLint *);
typedef GLenum (GL_APIENTRY *PFN_glGetError)(void);
typedef const GLubyte *(GL_APIENTRY *PFN_glGetString)(GLenum);
typedef const GLubyte *(GL_APIENTRY *PFN_glGetStringi)(GLenum, GLuint);
typedef void (GL_APIENTRY *PFN_glPixelStorei)(GLenum, GLint);
typedef void (GL_APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (GL_APIENTRY *PFN_glTexParameterf)(GLenum, GLenum, GLfloat);
typedef void (GL_APIENTRY *PFN_glTexParameterfv)(GLenum, GLenum, const GLfloat *);
typedef void (GL_APIENTRY *PFN_glTexParameteriv)(GLenum, GLenum, const GLint *);
typedef void (GL_APIENTRY *PFN_glSamplerParameteri)(GLuint, GLenum, GLint);
typedef void (GL_APIENTRY *PFN_glSamplerParameterf)(GLuint, GLenum, GLfloat);
typedef void (GL_APIENTRY *PFN_glSamplerParameterfv)(GLuint, GLenum, const GLfloat *);
typedef void (GL_APIENTRY *PFN_glSamplerParameteriv)(GLuint, GLenum, const GLint *);
typedef void (GL_APIENTRY *PFN_glGetTexParameteriv)(GLenum, GLenum, GLint *);
typedef void (GL_APIENTRY *PFN_glGetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
typedef void (GL_APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (GL_APIENTRY *PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (GL_APIENTRY *PFN_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (GL_APIENTRY *PFN_glTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (GL_APIENTRY *PFN_glCompressedTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (GL_APIENTRY *PFN_glCompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
typedef void (GL_APIENTRY *PFN_glCompressedTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *);
typedef void (GL_APIENTRY *PFN_glGetTexImage)(GLenum, GLint, GLenum, GLenum, void *);
typedef void (GL_APIENTRY *PFN_glGetCompressedTexImage)(GLenum, GLint, void *);
typedef void *(GL_APIENTRY *PFN_glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (GL_APIENTRY *PFN_glUnmapBuffer)(GLenum);
typedef void (GL_APIENTRY *PFN_glFlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
typedef GLsync (GL_APIENTRY *PFN_glFenceSync)(GLenum, GLbitfield);
typedef GLenum (GL_APIENTRY *PFN_glClientWaitSync)(GLsync, GLbitfield, GLuint64);
typedef void (GL_APIENTRY *PFN_glDeleteSync)(GLsync);
typedef void (GL_APIENTRY *PFN_glBindVertexArray)(GLuint);
typedef void (GL_APIENTRY *PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (GL_APIENTRY *PFN_glEnableVertexAttribArray)(GLuint);
typedef void (GL_APIENTRY *PFN_glDisableVertexAttribArray)(GLuint);
typedef void (GL_APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (GL_APIENTRY *PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (GL_APIENTRY *PFN_glDrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
typedef void (GL_APIENTRY *PFN_glUseProgram)(GLuint);
typedef void (GL_APIENTRY *PFN_glDeleteProgram)(GLuint);
typedef void (GL_APIENTRY *PFN_glDeleteShader)(GLuint);
typedef void (GL_APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (GL_APIENTRY *PFN_glQueryCounterEXT)(GLuint, GLenum);
typedef void (GL_APIENTRY *PFN_glGetQueryObjecti64vEXT)(GLuint, GLenum, GLint64 *);
typedef void (GL_APIENTRY *PFN_glGetQueryObjectui64vEXT)(GLuint, GLenum, GLuint64 *);
typedef void (GL_APIENTRY *PFN_glGetQueryObjectivEXT)(GLuint, GLenum, GLint *);
typedef void (GL_APIENTRY *PFN_glDebugMessageCallback)(GLDEBUGPROC, const void *);
typedef void (GL_APIENTRY *PFN_glDebugMessageCallbackKHR)(GLDEBUGPROC, const void *);
typedef void (GL_APIENTRY *PFN_glDebugMessageControl)(GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean);
typedef void (GL_APIENTRY *PFN_glDebugMessageControlKHR)(GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean);
typedef void (GL_APIENTRY *PFN_glUniform1i)(GLint, GLint);
typedef GLint (GL_APIENTRY *PFN_glGetUniformLocation)(GLuint, const char *);
typedef GLint (GL_APIENTRY *PFN_glGetAttribLocation)(GLuint, const char *);
typedef void (GL_APIENTRY *PFN_glLinkProgram)(GLuint);
typedef void (GL_APIENTRY *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (GL_APIENTRY *PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (GL_APIENTRY *PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (GL_APIENTRY *PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
typedef void (GL_APIENTRY *PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void (GL_APIENTRY *PFN_glCompileShader)(GLuint);
typedef void (GL_APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (GL_APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (GL_APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (GL_APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (GL_APIENTRY *PFN_glFramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
typedef GLenum (GL_APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void (GL_APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
typedef void (GL_APIENTRY *PFN_glGetBufferParameteriv)(GLenum, GLenum, GLint *);
typedef void (GL_APIENTRY *PFN_glDrawBuffers)(GLsizei, const GLenum *);
typedef void (GL_APIENTRY *PFN_glColorMaski)(GLuint, GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (GL_APIENTRY *PFN_glCopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint,
                                                   GLuint, GLenum, GLint, GLint, GLint, GLint,
                                                   GLsizei, GLsizei, GLsizei);
typedef void (GL_APIENTRY *PFN_glTexBuffer)(GLenum, GLenum, GLuint);
typedef void (GL_APIENTRY *PFN_glGenerateMipmap)(GLenum target);
typedef void (GL_APIENTRY *PFN_glWaitSync)(GLsync, GLbitfield, GLuint64);
typedef void (GL_APIENTRY *PFN_glFinish)(void);
typedef void (GL_APIENTRY *PFN_glFlush)(void);

#ifdef __cplusplus
}
#endif
