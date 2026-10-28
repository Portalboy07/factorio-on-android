/* Core of the Factorio GL 3.3 -> GLES 3.2 compatibility layer.
 *
 * fsgl_lookup() is the only public symbol.  It returns wrappers for the small
 * set of entry points that need translation; every other name returns NULL so
 * SDL falls back to eglGetProcAddress exactly as it did before.
 *
 * rev-2: fsgl_real() (which performs eglGetProcAddress/dlsym) is used only
 * during initialization.  Every wrapper resolves through fsgl_real_id(), an
 * enum-indexed table filled on first use with relaxed atomics, so the hot path
 * has no loader lock and no symbol-table walk.
 *
 * Texture and buffer bookkeeping uses growable direct-index pointer arrays
 * instead of per-call hash lookups and per-object malloc.  Factorio issues all
 * GL calls from one thread (the render thread); these tables are deliberately
 * lock-free and must stay that way.
 */
#include "glcompat.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_BGRA8_EXT
#define GL_BGRA8_EXT 0x93A1
#endif
#ifndef GL_TEXTURE_COMPRESSED_IMAGE_SIZE
#define GL_TEXTURE_COMPRESSED_IMAGE_SIZE 0x86A0
#endif
#ifndef GL_TEXTURE_COMPRESSED
#define GL_TEXTURE_COMPRESSED 0x86A1
#endif
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#define GL_WRITE_ONLY 0x88B9
#define GL_READ_WRITE 0x88BA
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_IMAGE_HEIGHT 0x806E
#define GL_UNPACK_SKIP_IMAGES 0x806D
#endif
#ifndef GL_PACK_ALIGNMENT
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif

#define TAG "glcompat"

#if FSGL_DIAG
static FILE *g_logfile;
static pthread_once_t g_logfile_once = PTHREAD_ONCE_INIT;

static void logfile_open(void)
{
    const char *path = getenv("FSGL_LOG_FILE");
    if (!path || !*path) {
        path = "/storage/emulated/0/Android/data/org.factorio.android/files/glcompat.log";
    }
    g_logfile = fopen(path, "w");
    if (g_logfile) {
        setvbuf(g_logfile, NULL, _IOLBF, 0);
    }
}
#endif

void fsgl_vlog(int prio, const char *tag, const char *fmt, va_list ap)
{
#if FSGL_DIAG
    va_list copy;
    va_copy(copy, ap);
    pthread_once(&g_logfile_once, logfile_open);
    if (g_logfile) {
        fprintf(g_logfile, "%.3f %s: ", (double)fsgl_now_ns() / 1e9, tag);
        vfprintf(g_logfile, fmt, copy);
        fputc('\n', g_logfile);
    }
    va_end(copy);
#endif
    __android_log_vprint(prio, tag, fmt, ap);
}

void fsgl_log(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fsgl_vlog(prio, TAG, fmt, ap);
    va_end(ap);
}

void fsgl_log_once_impl(const char *key, const char *fmt, ...)
{
    static const char *seen[256];
    static int nseen = 0;
    for (int i = 0; i < nseen; i++) {
        if (strcmp(seen[i], key) == 0) {
            return;
        }
    }
    if (nseen < (int)(sizeof(seen) / sizeof(seen[0]))) {
        seen[nseen++] = key;
    }
    va_list ap;
    va_start(ap, fmt);
    fsgl_vlog(ANDROID_LOG_INFO, TAG, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Real GLES resolution                                                */
/* ------------------------------------------------------------------ */

static void *g_egl_handle;
static void *g_gles_handle;
static PFNEGLGETPROCADDRESSPROC g_egl_get_proc;
static pthread_once_t g_libs_once = PTHREAD_ONCE_INIT;

static void load_libs_once(void)
{
    g_egl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (g_egl_handle) {
        *(void **)(&g_egl_get_proc) = dlsym(g_egl_handle, "eglGetProcAddress");
    }
    g_gles_handle = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_gles_handle) {
        g_gles_handle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    }
    fsgl_log(ANDROID_LOG_INFO,
             "resolver: libEGL=%p eglGetProcAddress=%p libGLES=%p",
             g_egl_handle, (void *)g_egl_get_proc, g_gles_handle);
}

FSGLProc fsgl_real(const char *name)
{
    pthread_once(&g_libs_once, load_libs_once);
    void *p = NULL;
    if (g_egl_get_proc) {
        p = (void *)g_egl_get_proc(name);
    }
    if (!p && g_gles_handle) {
        p = dlsym(g_gles_handle, name);
    }
    return (FSGLProc)p;
}

bool fsgl_have(const char *name)
{
    return fsgl_real(name) != NULL;
}

/* Enum-indexed real entry point cache.  0 = unresolved, 1 = known missing,
 * anything else = resolved address.  Relaxed atomics: two threads racing on
 * first use both resolve the same address, which is benign.  The header's
 * fsgl_real_id() inlines the load, so wrappers pay one ldr. */
uintptr_t fsgl_real_cache[FSGL_R_COUNT];

static const char *const k_real_names[FSGL_R_COUNT] = {
    [FSGL_R_glGetString] = "glGetString",
    [FSGL_R_glGetStringi] = "glGetStringi",
    [FSGL_R_glGetIntegerv] = "glGetIntegerv",
    [FSGL_R_glGetError] = "glGetError",

    [FSGL_R_glShaderSource] = "glShaderSource",
    [FSGL_R_glCompileShader] = "glCompileShader",
    [FSGL_R_glGetShaderiv] = "glGetShaderiv",
    [FSGL_R_glGetShaderInfoLog] = "glGetShaderInfoLog",
    [FSGL_R_glDeleteShader] = "glDeleteShader",

    [FSGL_R_glBindTexture] = "glBindTexture",
    [FSGL_R_glDeleteTextures] = "glDeleteTextures",
    [FSGL_R_glBindBuffer] = "glBindBuffer",
    [FSGL_R_glBufferData] = "glBufferData",
    [FSGL_R_glBufferSubData] = "glBufferSubData",
    [FSGL_R_glDeleteBuffers] = "glDeleteBuffers",
    [FSGL_R_glMapBufferRange] = "glMapBufferRange",
    [FSGL_R_glGetBufferParameteriv] = "glGetBufferParameteriv",
    [FSGL_R_glUnmapBuffer] = "glUnmapBuffer",
    [FSGL_R_glFlushMappedBufferRange] = "glFlushMappedBufferRange",
    [FSGL_R_glFenceSync] = "glFenceSync",
    [FSGL_R_glClientWaitSync] = "glClientWaitSync",
    [FSGL_R_glDeleteSync] = "glDeleteSync",
    [FSGL_R_glBindVertexArray] = "glBindVertexArray",
    [FSGL_R_glVertexAttribPointer] = "glVertexAttribPointer",
    [FSGL_R_glEnableVertexAttribArray] = "glEnableVertexAttribArray",
    [FSGL_R_glDisableVertexAttribArray] = "glDisableVertexAttribArray",

    [FSGL_R_glDrawArrays] = "glDrawArrays",
    [FSGL_R_glDrawElements] = "glDrawElements",
    [FSGL_R_glDrawArraysInstanced] = "glDrawArraysInstanced",
    [FSGL_R_glUseProgram] = "glUseProgram",
    [FSGL_R_glDeleteProgram] = "glDeleteProgram",
    [FSGL_R_glUniform1i] = "glUniform1i",
    [FSGL_R_glGetUniformLocation] = "glGetUniformLocation",
    [FSGL_R_glGetAttribLocation] = "glGetAttribLocation",
    [FSGL_R_glLinkProgram] = "glLinkProgram",
    [FSGL_R_glViewport] = "glViewport",
    [FSGL_R_glScissor] = "glScissor",

    [FSGL_R_glDebugMessageCallback] = "glDebugMessageCallback",
    [FSGL_R_glDebugMessageCallbackKHR] = "glDebugMessageCallbackKHR",
    [FSGL_R_glDebugMessageControl] = "glDebugMessageControl",
    [FSGL_R_glDebugMessageControlKHR] = "glDebugMessageControlKHR",
    [FSGL_R_glColorMaski] = "glColorMaski",
    [FSGL_R_glCopyImageSubData] = "glCopyImageSubData",
    [FSGL_R_glTexBuffer] = "glTexBuffer",

    [FSGL_R_glTexImage2D] = "glTexImage2D",
    [FSGL_R_glTexSubImage2D] = "glTexSubImage2D",
    [FSGL_R_glTexImage3D] = "glTexImage3D",
    [FSGL_R_glTexSubImage3D] = "glTexSubImage3D",
    [FSGL_R_glCompressedTexImage2D] = "glCompressedTexImage2D",
    [FSGL_R_glCompressedTexSubImage2D] = "glCompressedTexSubImage2D",
    [FSGL_R_glCompressedTexSubImage3D] = "glCompressedTexSubImage3D",
    [FSGL_R_glGetTexLevelParameteriv] = "glGetTexLevelParameteriv",
    [FSGL_R_glGetCompressedTexImage] = "glGetCompressedTexImage",
    [FSGL_R_glPixelStorei] = "glPixelStorei",
    [FSGL_R_glGetTexParameteriv] = "glGetTexParameteriv",

    [FSGL_R_glTexParameteri] = "glTexParameteri",
    [FSGL_R_glTexParameterf] = "glTexParameterf",
    [FSGL_R_glTexParameterfv] = "glTexParameterfv",
    [FSGL_R_glTexParameteriv] = "glTexParameteriv",
    [FSGL_R_glSamplerParameteri] = "glSamplerParameteri",
    [FSGL_R_glSamplerParameterf] = "glSamplerParameterf",
    [FSGL_R_glSamplerParameterfv] = "glSamplerParameterfv",
    [FSGL_R_glSamplerParameteriv] = "glSamplerParameteriv",

    [FSGL_R_glGenFramebuffers] = "glGenFramebuffers",
    [FSGL_R_glDeleteFramebuffers] = "glDeleteFramebuffers",
    [FSGL_R_glBindFramebuffer] = "glBindFramebuffer",
    [FSGL_R_glFramebufferTexture2D] = "glFramebufferTexture2D",
    [FSGL_R_glFramebufferTextureLayer] = "glFramebufferTextureLayer",
    [FSGL_R_glCheckFramebufferStatus] = "glCheckFramebufferStatus",
    [FSGL_R_glReadPixels] = "glReadPixels",
    [FSGL_R_glDrawBuffers] = "glDrawBuffers",

    [FSGL_R_glQueryCounterEXT] = "glQueryCounterEXT",
    [FSGL_R_glGetQueryObjecti64vEXT] = "glGetQueryObjecti64vEXT",
    [FSGL_R_glGetQueryObjectui64vEXT] = "glGetQueryObjectui64vEXT",
    [FSGL_R_glGetQueryObjectivEXT] = "glGetQueryObjectivEXT",

    [FSGL_R_glGenerateMipmap] = "glGenerateMipmap",
    [FSGL_R_glGenTextures] = "glGenTextures",

    [FSGL_R_glWaitSync] = "glWaitSync",
    [FSGL_R_glFinish] = "glFinish",
    [FSGL_R_glFlush] = "glFlush",

    [FSGL_R_glClear] = "glClear",
    [FSGL_R_glClearBufferfv] = "glClearBufferfv",
    [FSGL_R_glClearBufferfi] = "glClearBufferfi",
    [FSGL_R_glInvalidateFramebuffer] = "glInvalidateFramebuffer",
    [FSGL_R_glBlitFramebuffer] = "glBlitFramebuffer",
    [FSGL_R_glDrawElementsInstanced] = "glDrawElementsInstanced",
    [FSGL_R_glDrawElementsBaseVertex] = "glDrawElementsBaseVertex",
    [FSGL_R_glDrawRangeElements] = "glDrawRangeElements",
    [FSGL_R_glAttachShader] = "glAttachShader",
};

FSGLProc fsgl_real_slow(FsglRealId id)
{
    if ((unsigned)id >= (unsigned)FSGL_R_COUNT) {
        return NULL;
    }
    uintptr_t v = __atomic_load_n(&fsgl_real_cache[id], __ATOMIC_RELAXED);
    if (v != 0) {
        return v == 1u ? NULL : (FSGLProc)v;
    }
    FSGLProc p = fsgl_real(k_real_names[id]);
    v = p ? (uintptr_t)p : 1u;
    __atomic_store_n(&fsgl_real_cache[id], v, __ATOMIC_RELAXED);
    return p;
}

/* ------------------------------------------------------------------ */
/* Extension list                                                      */
/* ------------------------------------------------------------------ */

static char **g_exts;
static GLuint g_ext_count;
static char *g_ext_joined;
static bool g_ext_ready;
static FsglExtCaps g_ext_caps;

static bool ext_list_contains(const char *name)
{
    for (GLuint i = 0; i < g_ext_count; i++) {
        if (strcmp(g_exts[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void ext_add(const char *name)
{
    if (!name || !*name || ext_list_contains(name)) {
        return;
    }
    char **next = (char **)realloc(g_exts, sizeof(char *) * (g_ext_count + 1));
    if (!next) {
        return;
    }
    g_exts = next;
    g_exts[g_ext_count] = strdup(name);
    if (g_exts[g_ext_count]) {
        g_ext_count++;
    }
}

static void ext_add_synthetic(const char *name, const char *probe_a, const char *probe_b)
{
    if (fsgl_have(probe_a) || (probe_b && fsgl_have(probe_b))) {
        ext_add(name);
        fsgl_log_once_impl(name, "extension: advertising synthetic %s (backed by %s%s%s)",
                           name, probe_a, probe_b ? "/" : "", probe_b ? probe_b : "");
    }
}

static void ext_build(void)
{
    if (g_ext_ready) {
        return;
    }
    PFN_glGetIntegerv get_int = (PFN_glGetIntegerv)fsgl_real_id(FSGL_R_glGetIntegerv);
    PFN_glGetString get_str = (PFN_glGetString)fsgl_real_id(FSGL_R_glGetString);
    PFN_glGetStringi get_str_i = (PFN_glGetStringi)fsgl_real_id(FSGL_R_glGetStringi);
    GLint n = 0;

    /* Do not cache an empty list if this is called before a context exists;
     * GLAD and SDL both ask again once the context is current. */
    if (!get_str || !get_str(GL_VERSION)) {
        return;
    }
    if (get_int) {
        get_int(GL_NUM_EXTENSIONS, &n);
    }
    if (n > 0 && get_str_i) {
        for (GLint i = 0; i < n; i++) {
            const char *e = (const char *)get_str_i(GL_EXTENSIONS, (GLuint)i);
            ext_add(e);
        }
    } else if (get_str) {
        const char *all = (const char *)get_str(GL_EXTENSIONS);
        if (all) {
            char *copy = strdup(all);
            if (copy) {
                char *save = NULL;
                for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
                    ext_add(tok);
                }
                free(copy);
            }
        }
    }

    /* What the driver itself can decode; the upload path transcodes the
     * rest.  Resolved before synthetic entries join the list. */
    g_ext_caps.s3tc = ext_list_contains("GL_EXT_texture_compression_s3tc") ||
                      ext_list_contains("GL_ARB_texture_compression_s3tc");
    g_ext_caps.rgtc = ext_list_contains("GL_EXT_texture_compression_rgtc") ||
                      ext_list_contains("GL_ARB_texture_compression_rgtc");

    /* Only advertise synthetic desktop extensions we can actually back. */
    if (fsgl_config_fake_s3tc()) {
        /* Without S3TC the game uploads its colour atlases as RGBA8
         * ("not-compressed"); advertised, it compresses them to DXT and the
         * upload path transcodes to ETC2 (4-8 bpp instead of 32). */
        ext_add("GL_EXT_texture_compression_s3tc");
        ext_add("GL_ARB_texture_compression_rgtc");
        fsgl_log_once("fake-s3tc", "extension: advertising S3TC/RGTC backed by the ETC2 transcoder");
    }
    ext_add_synthetic("GL_ARB_copy_image", "glCopyImageSubData", NULL);
    ext_add_synthetic("GL_ARB_clear_texture", "glClearTexImage", "glClearTexSubImage");
    ext_add_synthetic("GL_KHR_debug", "glDebugMessageCallback", "glDebugMessageCallbackKHR");
    if (fsgl_have("glQueryCounterEXT") && fsgl_have("glGetQueryObjecti64vEXT")) {
        ext_add("GL_ARB_timer_query");
        fsgl_log_once("GL_ARB_timer_query",
                      "extension: advertising synthetic GL_ARB_timer_query (backed by "
                      "glQueryCounterEXT + glGetQueryObjecti64vEXT)");
    }
    ext_add_synthetic("GL_ARB_texture_buffer_object", "glTexBuffer", NULL);

    /* Memory-info extensions are answered by the glGetIntegerv wrapper from
     * /proc/meminfo (unified-memory device): NVX total/current-available and
     * the ATI free-memory triplets.  Dedicated VRAM is always 0. */
    ext_add("GL_NVX_gpu_memory_info");
    fsgl_log_once("GL_NVX_gpu_memory_info",
                  "extension: advertising synthetic GL_NVX_gpu_memory_info "
                  "(backed by /proc/meminfo)");
    ext_add("GL_ATI_meminfo");
    fsgl_log_once("GL_ATI_meminfo",
                  "extension: advertising synthetic GL_ATI_meminfo (backed by /proc/meminfo)");

    size_t total = 1;
    for (GLuint i = 0; i < g_ext_count; i++) {
        total += strlen(g_exts[i]) + 1;
    }
    g_ext_joined = (char *)malloc(total);
    if (g_ext_joined) {
        g_ext_joined[0] = '\0';
        for (GLuint i = 0; i < g_ext_count; i++) {
            if (i) {
                strcat(g_ext_joined, " ");
            }
            strcat(g_ext_joined, g_exts[i]);
        }
    }
    g_ext_ready = true;
    fsgl_log(ANDROID_LOG_INFO, "extensions: %u entries (real+synthetic), %zu bytes",
             g_ext_count, g_ext_joined ? strlen(g_ext_joined) : (size_t)0);
}

void fsgl_ext_ensure(void)
{
    ext_build();
}

GLuint fsgl_ext_count(void)
{
    fsgl_ext_ensure();
    return g_ext_count;
}

const char *fsgl_ext_at(GLuint index)
{
    fsgl_ext_ensure();
    return index < g_ext_count ? g_exts[index] : NULL;
}

const char *fsgl_ext_string(void)
{
    fsgl_ext_ensure();
    return g_ext_joined ? g_ext_joined : "";
}

bool fsgl_has_ext(const char *name)
{
    fsgl_ext_ensure();
    return ext_list_contains(name);
}

const FsglExtCaps *fsgl_ext_caps(void)
{
    fsgl_ext_ensure();
    return &g_ext_caps;
}

/* ------------------------------------------------------------------ */
/* Texture state                                                       */
/* ------------------------------------------------------------------ */

static FsglTexRec *g_tex_recs;
static GLuint g_tex_cap;
static GLuint g_tex_bound[8];

/* Texture/buffer names are small in practice; cap the direct-index table so a
 * bogus name cannot overflow the doubling loop. */
#define FSGL_MAX_OBJECT_ID (1u << 22)

static bool tex_ensure(GLuint id)
{
    if (id >= FSGL_MAX_OBJECT_ID) {
        return false;
    }
    if (id < g_tex_cap) {
        return true;
    }
    GLuint cap = g_tex_cap ? g_tex_cap : 256;
    while (cap <= id) {
        cap *= 2;
    }
    FsglTexRec *next = (FsglTexRec *)realloc(g_tex_recs, (size_t)cap * sizeof(*next));
    if (!next) {
        return false;
    }
    memset(next + g_tex_cap, 0, (size_t)(cap - g_tex_cap) * sizeof(*next));
    g_tex_recs = next;
    g_tex_cap = cap;
    return true;
}

FsglTexRec *fsgl_tex_get(GLuint id, bool create)
{
    if (id == 0) {
        return NULL;
    }
    if (id < g_tex_cap && g_tex_recs[id].id == id) {
        return &g_tex_recs[id];
    }
    if (!create) {
        return NULL;
    }
    if (!tex_ensure(id)) {
        return NULL;
    }
    FsglTexRec *r = &g_tex_recs[id];
    memset(r, 0, sizeof(*r));
    r->id = id;
    return r;
}

void fsgl_tex_remove(GLuint id)
{
    if (id && id < g_tex_cap && g_tex_recs[id].id == id) {
        memset(&g_tex_recs[id], 0, sizeof(g_tex_recs[id]));
    }
}

static int tex_target_slot(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_2D: return 0;
    case GL_TEXTURE_3D: return 1;
    case GL_TEXTURE_2D_ARRAY: return 2;
    case GL_TEXTURE_CUBE_MAP: return 3;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: return 3;
    case GL_TEXTURE_BUFFER: return 4;
    case GL_TEXTURE_2D_MULTISAMPLE: return 5;
    default: return 6;
    }
}

GLuint fsgl_tex_bound(GLenum target)
{
    return g_tex_bound[tex_target_slot(target)];
}

void fsgl_tex_bind(GLenum target, GLuint id)
{
    g_tex_bound[tex_target_slot(target)] = id;
}

/* ------------------------------------------------------------------ */
/* Texture name redirection                                            */
/*                                                                     */
/* The game pre-creates texture objects with an uncompressed placeholder */
/* level (glTexImage2D GL_RGBA) and later redefines level 0 as compressed. */
/* Some drivers (Adreno 512 observed; Mali mostly tolerates it) reject    */
/* glCompressedTexImage2D over the uncompressed definition, silently      */
/* leaving the RGBA level in place: every subsequent streamed tile update */
/* then fails with INVALID_OPERATION and the atlas pops in and out.       */
/* Workaround: upload the compressed level to a FRESH texture object and  */
/* redirect every bind of the game's name to the replacement.             */
/* ------------------------------------------------------------------ */

#define TEX_REDIRECTS 64

typedef struct {
    GLenum target;
    GLuint orig;
    GLuint alt;
} TexRedirect;

static TexRedirect g_tex_redirects[TEX_REDIRECTS];
static int g_tex_redirect_count;

void fsgl_tex_redirect_add(GLenum target, GLuint orig, GLuint alt)
{
    if (g_tex_redirect_count < TEX_REDIRECTS) {
        TexRedirect *r = &g_tex_redirects[g_tex_redirect_count++];
        r->target = target;
        r->orig = orig;
        r->alt = alt;
    }
}

GLuint fsgl_tex_redirect_lookup(GLenum target, GLuint orig)
{
    for (int i = 0; i < g_tex_redirect_count; i++) {
        if ((target == 0 || g_tex_redirects[i].target == target) &&
            g_tex_redirects[i].orig == orig) {
            return g_tex_redirects[i].alt;
        }
    }
    return 0;
}

void fsgl_tex_redirect_remove(GLuint orig)
{
    for (int i = 0; i < g_tex_redirect_count; i++) {
        if (g_tex_redirects[i].orig == orig) {
            g_tex_redirects[i] = g_tex_redirects[--g_tex_redirect_count];
            return;
        }
    }
}

FsglTexLevel *fsgl_tex_level(FsglTexRec *rec, GLint level)
{
    if (!rec || level < 0 || level >= FSGL_MAX_TEX_LEVELS) {
        return NULL;
    }
    return &rec->levels[level];
}

/* ------------------------------------------------------------------ */
/* Buffer state                                                        */
/* ------------------------------------------------------------------ */

#define BUF_TARGETS 12

static GLuint g_buf_bound[BUF_TARGETS];

/* Hit on every bind/map/draw; a switch compiles to a compare tree instead of
 * a 12-entry linear scan. */
static int buf_target_slot(GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER: return 0;
    case GL_ELEMENT_ARRAY_BUFFER: return 1;
    case GL_PIXEL_PACK_BUFFER: return 2;
    case GL_PIXEL_UNPACK_BUFFER: return 3;
    case GL_UNIFORM_BUFFER: return 4;
    case GL_COPY_READ_BUFFER: return 5;
    case GL_COPY_WRITE_BUFFER: return 6;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return 7;
    case GL_SHADER_STORAGE_BUFFER: return 8;
    case GL_DRAW_INDIRECT_BUFFER: return 9;
    case GL_DISPATCH_INDIRECT_BUFFER: return 10;
    case GL_ATOMIC_COUNTER_BUFFER: return 11;
    default: return -1;
    }
}

GLuint fsgl_buf_bound(GLenum target)
{
    int i = buf_target_slot(target);
    return i >= 0 ? g_buf_bound[i] : 0;
}

void fsgl_buf_bind(GLenum target, GLuint id)
{
    int i = buf_target_slot(target);
    if (i >= 0) {
        g_buf_bound[i] = id;
    }
}

typedef struct {
    GLsizeiptr size;
} BufRec;

static BufRec **g_buf_recs;
static GLuint g_buf_cap;

static bool buf_ensure(GLuint id)
{
    if (id >= FSGL_MAX_OBJECT_ID) {
        return false;
    }
    if (id < g_buf_cap) {
        return true;
    }
    GLuint cap = g_buf_cap ? g_buf_cap : 256;
    while (cap <= id) {
        cap *= 2;
    }
    BufRec **next = (BufRec **)realloc(g_buf_recs, (size_t)cap * sizeof(*next));
    if (!next) {
        return false;
    }
    memset(next + g_buf_cap, 0, (size_t)(cap - g_buf_cap) * sizeof(*next));
    g_buf_recs = next;
    g_buf_cap = cap;
    return true;
}

static BufRec *buf_get(GLuint id, bool create)
{
    if (!id) {
        return NULL;
    }
    if (id < g_buf_cap && g_buf_recs[id]) {
        return g_buf_recs[id];
    }
    if (!create) {
        return NULL;
    }
    if (!buf_ensure(id)) {
        return NULL;
    }
    BufRec *r = (BufRec *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    g_buf_recs[id] = r;
    return r;
}

void fsgl_buf_set_size(GLenum target, GLsizeiptr size)
{
    GLuint id = fsgl_buf_bound(target);
    BufRec *r = buf_get(id, true);
    if (r) {
        r->size = size;
    }
}

GLsizeiptr fsgl_buf_get_size(GLuint id)
{
    BufRec *r = buf_get(id, false);
    return r ? r->size : 0;
}

void fsgl_buf_remove(GLuint id)
{
    if (id && id < g_buf_cap && g_buf_recs[id]) {
        free(g_buf_recs[id]);
        g_buf_recs[id] = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Program state                                                       */
/* ------------------------------------------------------------------ */

static GLuint g_current_program;
static bool g_bi_used;
static bool g_bi_dirty;

void fsgl_use_program(GLuint program)
{
    g_current_program = program;
}

GLuint fsgl_current_program(void)
{
    return g_current_program;
}

void fsgl_baseinstance_mark_used(void)
{
    g_bi_used = true;
}

bool fsgl_baseinstance_used(void)
{
    return g_bi_used;
}

bool fsgl_baseinstance_dirty(void)
{
    return g_bi_dirty;
}

void fsgl_baseinstance_set_dirty(bool dirty)
{
    g_bi_dirty = dirty;
}

GLint fsgl_baseinstance_location(GLuint program)
{
    /* The uniform only matters once a nonzero baseinstance has been used.
     * Querying it on every program switch used to cost a driver call even
     * though Factorio's baseinstance is almost always zero.  Do not cache the
     * result: GL may reuse program names, and a stale location would make
     * glUniform1i raise INVALID_OPERATION. */
    if (!g_bi_used || !program) {
        return -1;
    }
    PFN_glGetUniformLocation loc =
        (PFN_glGetUniformLocation)fsgl_real_id(FSGL_R_glGetUniformLocation);
    if (!loc) {
        return -1;
    }
    GLint location = loc(program, "fsgl_BaseInstance");
    if (location < 0) {
        fsgl_log_once("baseinstance-uniform-absent",
                      "base-instance: program %u has no active fsgl_BaseInstance uniform",
                      program);
    }
    return location;
}
