/* Wrappers for entry points whose desktop behaviour differs from GLES 3.2.
 *
 * rev-2 instrumentation:
 *   - every wrapper pushes name + up to 4 integer args to the per-thread call
 *     ring (no formatting),
 *   - times the forwarded driver call with two clock reads and updates its
 *     profiler slot (no logging),
 *   - after the caller's forwarded call, fsgl_err_check() consumes and logs
 *     the error with full parameters, then queues it for the caller's own
 *     glGetError.
 *
 * Wrappers that perform layer-internal GL calls bracket them with
 * fsgl_err_begin_extra()/fsgl_err_end_extra(); see wrap_tex.c for the upload
 * and readback paths.
 */
#include "glcompat.h"

#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX 0x9047
#endif
#ifndef GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX
#define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
#endif
#ifndef GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX
#define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
#endif
#ifndef GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX
#define GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX 0x904A
#endif
#ifndef GL_VBO_FREE_MEMORY_ATI
#define GL_VBO_FREE_MEMORY_ATI 0x87FB
#endif
#ifndef GL_TEXTURE_FREE_MEMORY_ATI
#define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
#endif
#ifndef GL_RENDERBUFFER_FREE_MEMORY_ATI
#define GL_RENDERBUFFER_FREE_MEMORY_ATI 0x87FD
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
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_TEXTURE_MIN_LOD
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif

/* ------------------------------------------------------------------ */
/* Strings and integers                                                */
/* ------------------------------------------------------------------ */

static const GLubyte *GL_APIENTRY wrap_glGetString(GLenum name)
{
    FSGL_PROF_BEGIN(glGetString);
    fsgl_ring_push("glGetString", (uintptr_t)name, 0, 0, 0);

    PFN_glGetString real = (PFN_glGetString)fsgl_real_id(FSGL_R_glGetString);
    if (!real) {
        FSGL_PROF_END(0, 0);
        return NULL;
    }
    switch (name) {
    case GL_VERSION: {
        static const char s[] = "3.3.0 GLCompat (OpenGL ES 3.2)";
        fsgl_log_once("string-version", "string: GL_VERSION -> \"%s\"", s);
        FSGL_PROF_END(0, 0);
        return (const GLubyte *)s;
    }
    case GL_SHADING_LANGUAGE_VERSION: {
        static const char s[] = "3.30 GLCompat (GLSL ES 3.20)";
        fsgl_log_once("string-glsl", "string: GL_SHADING_LANGUAGE_VERSION -> \"%s\"", s);
        FSGL_PROF_END(0, 0);
        return (const GLubyte *)s;
    }
    case GL_EXTENSIONS: {
        fsgl_ext_ensure();
        FSGL_PROF_END(0, 0);
        return (const GLubyte *)fsgl_ext_string();
    }
    case GL_RENDERER: {
        const GLubyte *r = real(name);
        fsgl_log_once("string-renderer", "string: GL_RENDERER -> \"%s\"",
                      r ? (const char *)r : "unknown");
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glGetString", "name=0x%x", (unsigned)name);
        return r;
    }
    default: {
        const GLubyte *r = real(name);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glGetString", "name=0x%x", (unsigned)name);
        return r;
    }
    }
}

static const GLubyte *GL_APIENTRY wrap_glGetStringi(GLenum name, GLuint index)
{
    FSGL_PROF_BEGIN(glGetStringi);
    fsgl_ring_push("glGetStringi", (uintptr_t)name, (uintptr_t)index, 0, 0);

    if (name == GL_EXTENSIONS) {
        const char *e = fsgl_ext_at(index);
        FSGL_PROF_END(0, 0);
        return e ? (const GLubyte *)e : NULL;
    }
    PFN_glGetStringi real = (PFN_glGetStringi)fsgl_real_id(FSGL_R_glGetStringi);
    if (!real) {
        FSGL_PROF_END(0, 0);
        return NULL;
    }
    const GLubyte *r = real(name, index);
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glGetStringi", "name=0x%x index=%u", (unsigned)name, (unsigned)index);
    return r;
}

/* MemAvailable from /proc/meminfo in kB, cached for 1 s (the value moves
 * slowly and some callers query it every frame).  Unified-memory devices
 * have no separate VRAM, so "available memory" is the honest answer. */
static GLint fsgl_mem_available_kb(void)
{
    static GLint cached_kb;
    static int64_t cached_ns;
    int64_t now = fsgl_now_ns();
    if (cached_kb > 0 && now - cached_ns < 1000000000) {
        return cached_kb;
    }
    GLint avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemAvailable:", 13) == 0) {
                avail = (GLint)strtoll(line + 13, NULL, 10);
                break;
            }
        }
        fclose(f);
    }
    if (avail > 0) {
        cached_kb = avail;
        cached_ns = now;
    }
    return cached_kb;
}

static void GL_APIENTRY wrap_glGetIntegerv(GLenum pname, GLint *params)
{
    FSGL_PROF_BEGIN(glGetIntegerv);
    fsgl_ring_push("glGetIntegerv", (uintptr_t)pname, (uintptr_t)params, 0, 0);

    if (!params) {
        FSGL_PROF_END(0, 0);
        return;
    }
    switch (pname) {
    case GL_MAJOR_VERSION:
        *params = 3;
        fsgl_log_once("pname-major", "pname: GL_MAJOR_VERSION -> 3");
        FSGL_PROF_END(0, 0);
        return;
    case GL_MINOR_VERSION:
        *params = 3;
        fsgl_log_once("pname-minor", "pname: GL_MINOR_VERSION -> 3");
        FSGL_PROF_END(0, 0);
        return;
    case GL_NUM_EXTENSIONS:
        *params = (GLint)fsgl_ext_count();
        fsgl_log_once("pname-num-ext", "pname: GL_NUM_EXTENSIONS -> %d", *params);
        FSGL_PROF_END(0, 0);
        return;
    case GL_CONTEXT_PROFILE_MASK:
        *params = GL_CONTEXT_CORE_PROFILE_BIT;
        fsgl_log_once("pname-profile", "pname: GL_CONTEXT_PROFILE_MASK -> CORE");
        FSGL_PROF_END(0, 0);
        return;
    case GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX:
        /* Unified memory: there is no dedicated VRAM; 0 is the documented
         * "none" answer and pushes the game to its RAM-based fallback. */
        *params = 0;
        fsgl_log_once("pname-nvx-mem",
                      "pname: NVX dedicated VRAM -> 0 (unified memory)");
        FSGL_PROF_END(0, 0);
        return;
    case GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX:
    case GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX:
        *params = fsgl_mem_available_kb();
        fsgl_log_once("pname-nvx-avail",
                      "pname: NVX available memory 0x%x -> %d kB", pname, *params);
        FSGL_PROF_END(0, 0);
        return;
    case GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX:
        *params = 0;
        FSGL_PROF_END(0, 0);
        return;
    case GL_VBO_FREE_MEMORY_ATI:
    case GL_TEXTURE_FREE_MEMORY_ATI:
    case GL_RENDERBUFFER_FREE_MEMORY_ATI: {
        /* Triplets: [0] total free kB, [1] largest free block, [2] free in
         * the (nonexistent) auxiliary pool. */
        GLint free_kb = fsgl_mem_available_kb();
        params[0] = free_kb;
        params[1] = free_kb;
        params[2] = 0;
        fsgl_log_once("pname-ati-mem",
                      "pname: ATI free memory 0x%x -> %d kB", pname, free_kb);
        FSGL_PROF_END(0, 0);
        return;
    }
    default: {
        PFN_glGetIntegerv real = (PFN_glGetIntegerv)fsgl_real_id(FSGL_R_glGetIntegerv);
        if (real) {
            real(pname, params);
            FSGL_PROF_END(0, 0);
            fsgl_err_check("glGetIntegerv", "pname=0x%x", (unsigned)pname);
        } else {
            FSGL_PROF_END(0, 0);
        }
        return;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Shader translation and diagnostics                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    GLuint id;
    GLenum stage;
    char *source;
} ShaderRec;

static ShaderRec *g_shaders;
static size_t g_shader_count;
static size_t g_shader_cap;

static ShaderRec *shader_find(GLuint id)
{
    for (size_t i = 0; i < g_shader_count; i++) {
        if (g_shaders[i].id == id) {
            return &g_shaders[i];
        }
    }
    return NULL;
}

static void shader_store(GLuint id, GLenum stage, char *source)
{
    ShaderRec *r = shader_find(id);
    if (!r) {
        if (g_shader_count == g_shader_cap) {
            size_t cap = g_shader_cap ? g_shader_cap * 2 : 64;
            ShaderRec *next = (ShaderRec *)realloc(g_shaders, cap * sizeof(*next));
            if (!next) {
                free(source);
                return;
            }
            g_shaders = next;
            g_shader_cap = cap;
        }
        r = &g_shaders[g_shader_count++];
        r->id = id;
        r->source = NULL;
    }
    r->stage = stage;
    free(r->source);
    r->source = source;
}

const char *fsgl_shader_source(GLuint shader, GLenum *stage)
{
    ShaderRec *r = shader_find(shader);
    if (stage) {
        *stage = r ? r->stage : 0;
    }
    return r ? r->source : NULL;
}

static void shader_remove(GLuint id)
{
    ShaderRec *r = shader_find(id);
    if (r) {
        free(r->source);
        /* Compact the array; shader counts are small. */
        size_t idx = (size_t)(r - g_shaders);
        if (idx + 1 < g_shader_count) {
            memmove(&g_shaders[idx], &g_shaders[idx + 1],
                    (g_shader_count - idx - 1) * sizeof(g_shaders[0]));
        }
        g_shader_count--;
    }
}

static void GL_APIENTRY wrap_glShaderSource(GLuint shader, GLsizei count,
                                         const GLchar *const *string, const GLint *length)
{
    FSGL_PROF_BEGIN(glShaderSource);
    fsgl_ring_push("glShaderSource", (uintptr_t)shader, (uintptr_t)count,
                   (uintptr_t)string, (uintptr_t)length);

    PFN_glShaderSource real = (PFN_glShaderSource)fsgl_real_id(FSGL_R_glShaderSource);
    if (!real || !string) {
        FSGL_PROF_END(0, 0);
        return;
    }
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i]) {
            continue;
        }
        total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    }
    if (total == 0 || total > (16u << 20)) {
        real(shader, count, string, length);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glShaderSource", "shader=%u count=%d", (unsigned)shader, (int)count);
        return;
    }
    char *joined = (char *)malloc(total + 1);
    if (!joined) {
        real(shader, count, string, length);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glShaderSource", "shader=%u count=%d", (unsigned)shader, (int)count);
        return;
    }
    size_t used = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i]) {
            continue;
        }
        size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(joined + used, string[i], n);
        used += n;
    }
    joined[used] = '\0';

    GLenum stage = 0;
    PFN_glGetShaderiv getiv = (PFN_glGetShaderiv)fsgl_real_id(FSGL_R_glGetShaderiv);
    if (getiv) {
        GLint v = 0;
        fsgl_err_begin_extra("glShaderSource");
        getiv(shader, GL_SHADER_TYPE, &v);
        fsgl_err_end_extra("glShaderSource");
        stage = (GLenum)v;
    }
    bool changed = false;
    char *translated = fsgl_translate_shader(joined, stage, &changed);
    if (!translated) {
        translated = joined;
        joined = NULL;
    } else {
        free(joined);
    }
    shader_store(shader, stage, translated);
    if (changed) {
        fsgl_log(ANDROID_LOG_INFO, "shader %u: translated desktop GLSL to GLSL ES 3.20 (%zu bytes)%s",
                 shader, strlen(translated),
                 stage == GL_FRAGMENT_SHADER ? " [fragment]" : (stage == GL_VERTEX_SHADER ? " [vertex]" : ""));
    }
    const GLchar *one = translated;
    real(shader, 1, &one, NULL);
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glShaderSource", "shader=%u count=%d", (unsigned)shader, (int)count);
}

static void log_shader_failure(GLuint shader, const char *source)
{
    PFN_glGetShaderiv getiv = (PFN_glGetShaderiv)fsgl_real_id(FSGL_R_glGetShaderiv);
    PFN_glGetShaderInfoLog getlog =
        (PFN_glGetShaderInfoLog)fsgl_real_id(FSGL_R_glGetShaderInfoLog);
    GLint ok = 1;
    if (getiv) {
        getiv(shader, GL_COMPILE_STATUS, &ok);
    }
    if (ok) {
        return;
    }
    if (getlog) {
        char logbuf[4096];
        GLsizei written = 0;
        getlog(shader, sizeof(logbuf), &written, logbuf);
        if (written > 0) {
            logbuf[sizeof(logbuf) - 1] = '\0';
            fsgl_log(ANDROID_LOG_ERROR, "shader %u: compile failed: %s", shader, logbuf);
        }
    }
    if (!source) {
        fsgl_log(ANDROID_LOG_ERROR, "shader %u: compile failed (no translated source stored)", shader);
        return;
    }
    int line_no = 0;
    const char *p = source;
    while (*p && line_no < 40) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char buf[220];
        size_t copy = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
        memcpy(buf, p, copy);
        buf[copy] = '\0';
        fsgl_log(ANDROID_LOG_ERROR, "shader %u src[%02d]: %s", shader, line_no + 1, buf);
        line_no++;
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void GL_APIENTRY wrap_glCompileShader(GLuint shader)
{
    FSGL_PROF_BEGIN(glCompileShader);
    fsgl_ring_push("glCompileShader", (uintptr_t)shader, 0, 0, 0);

    PFN_glCompileShader real = (PFN_glCompileShader)fsgl_real_id(FSGL_R_glCompileShader);
    if (!real) {
        FSGL_PROF_END(0, 0);
        return;
    }
    real(shader);
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glCompileShader", "shader=%u", (unsigned)shader);

    /* Shader diagnostics are layer-internal GL calls: keep their errors away
     * from the caller's glGetError. */
    fsgl_err_begin_extra("glCompileShader");
    ShaderRec *rec = shader_find(shader);
    log_shader_failure(shader, rec ? rec->source : NULL);
    fsgl_err_end_extra("glCompileShader");
}

static void GL_APIENTRY wrap_glDeleteShader(GLuint shader)
{
    FSGL_PROF_BEGIN(glDeleteShader);
    fsgl_ring_push("glDeleteShader", (uintptr_t)shader, 0, 0, 0);

    shader_remove(shader);
    PFN_glDeleteShader real = (PFN_glDeleteShader)fsgl_real_id(FSGL_R_glDeleteShader);
    if (real) {
        real(shader);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDeleteShader", "shader=%u", (unsigned)shader);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Objects and buffers                                                 */
/* ------------------------------------------------------------------ */

static void GL_APIENTRY wrap_glBindTexture(GLenum target, GLuint texture)
{
    FSGL_PROF_BEGIN(glBindTexture);
    fsgl_ring_push("glBindTexture", (uintptr_t)target, (uintptr_t)texture, 0, 0);

    fsgl_tex_bind(target, texture);
    PFN_glBindTexture real = (PFN_glBindTexture)fsgl_real_id(FSGL_R_glBindTexture);
    if (real) {
        /* Serve redirected (driver-rejected and re-created) textures under
         * the game's name. */
        GLuint alt = fsgl_tex_redirect_lookup(target, texture);
        real(target, alt ? alt : texture);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glBindTexture", "target=0x%x texture=%u",
                       (unsigned)target, (unsigned)texture);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glDeleteTextures(GLsizei n, const GLuint *textures)
{
    FSGL_PROF_BEGIN(glDeleteTextures);
    fsgl_ring_push("glDeleteTextures", (uintptr_t)n, (uintptr_t)textures, 0, 0);

    /* Delete any redirected replacement objects alongside the game's
     * names (the driver may not know the game's name at all). */
    GLuint filtered[64];
    GLsizei fn = 0;
    if (textures) {
        for (GLsizei i = 0; i < n; i++) {
            GLuint alt = fsgl_tex_redirect_lookup(0, textures[i]);
            if (alt) {
                fsgl_tex_redirect_remove(textures[i]);
                if (fn < 64) {
                    filtered[fn++] = alt;
                }
            } else if (fn < 64) {
                filtered[fn++] = textures[i];
            }
            fsgl_tex_remove(textures[i]);
        }
    }
    PFN_glDeleteTextures real = (PFN_glDeleteTextures)fsgl_real_id(FSGL_R_glDeleteTextures);
    if (real) {
        real(fn, filtered);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDeleteTextures", "n=%d", (int)n);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glBindBuffer(GLenum target, GLuint buffer)
{
    FSGL_PROF_BEGIN(glBindBuffer);
    fsgl_ring_push("glBindBuffer", (uintptr_t)target, (uintptr_t)buffer, 0, 0);

    fsgl_buf_bind(target, buffer);
    fsgl_stream_element_binding(target, buffer);
    fsgl_stream_trace('B', buffer, (long long)target, 0, 0);
    PFN_glBindBuffer real = (PFN_glBindBuffer)fsgl_real_id(FSGL_R_glBindBuffer);
    if (real) {
        real(target, buffer);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glBindBuffer", "target=0x%x buffer=%u",
                       (unsigned)target, (unsigned)buffer);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    FSGL_PROF_BEGIN(glBufferData);
    fsgl_ring_push("glBufferData", (uintptr_t)target, (uintptr_t)size,
                   (uintptr_t)usage, 0);

    PFN_glBufferData real = (PFN_glBufferData)fsgl_real_id(FSGL_R_glBufferData);
    if (real) {
        real(target, size, data, usage);
        fsgl_buf_set_size(target, size);
        fsgl_stream_note_buffer_data(target, fsgl_buf_bound(target), size);
        FSGL_PROF_END(size > 0 ? (uint64_t)size : 0, 0);
        fsgl_err_check("glBufferData", "target=0x%x size=%lld usage=0x%x",
                       (unsigned)target, (long long)size, (unsigned)usage);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    FSGL_PROF_BEGIN(glDeleteBuffers);
    fsgl_ring_push("glDeleteBuffers", (uintptr_t)n, (uintptr_t)buffers, 0, 0);

    if (buffers) {
        for (GLsizei i = 0; i < n; i++) {
            fsgl_buf_remove(buffers[i]);
            fsgl_stream_drop_buffer(buffers[i]);
        }
    }
    PFN_glDeleteBuffers real = (PFN_glDeleteBuffers)fsgl_real_id(FSGL_R_glDeleteBuffers);
    if (real) {
        real(n, buffers);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDeleteBuffers", "n=%d", (int)n);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void *GL_APIENTRY wrap_glMapBuffer(GLenum target, GLenum access)
{
    FSGL_PROF_BEGIN(glMapBuffer);
    fsgl_ring_push("glMapBuffer", (uintptr_t)target, (uintptr_t)access, 0, 0);

    /* The size fallback below is a layer-internal GL call. */
    fsgl_err_begin_extra("glMapBuffer");

    PFN_glMapBufferRange map = (PFN_glMapBufferRange)fsgl_real_id(FSGL_R_glMapBufferRange);
    if (!map) {
        fsgl_err_end_extra("glMapBuffer");
        fsgl_log_once("mapbuffer-norange", "mapbuffer: glMapBufferRange missing; returning NULL");
        FSGL_PROF_END(0, 0);
        return NULL;
    }
    GLuint id = fsgl_buf_bound(target);
    GLsizeiptr size = fsgl_buf_get_size(id);
    if (size <= 0) {
        PFN_glGetBufferParameteriv getp =
            (PFN_glGetBufferParameteriv)fsgl_real_id(FSGL_R_glGetBufferParameteriv);
        GLint v = 0;
        if (getp) {
            getp(target, GL_BUFFER_SIZE, &v);
            size = (GLsizeiptr)v;
        }
    }
    fsgl_err_end_extra("glMapBuffer");

    if (size <= 0) {
        fsgl_log_once("mapbuffer-zerosize", "mapbuffer: target 0x%04x buffer %u has unknown size; returning NULL",
                      target, id);
        FSGL_PROF_END(0, 0);
        return NULL;
    }
    GLbitfield flags;
    switch (access) {
    case GL_READ_ONLY: flags = GL_MAP_READ_BIT; break;
    case GL_WRITE_ONLY: flags = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT; break;
    case GL_READ_WRITE: flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
    default: flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
    }
    fsgl_log_once("mapbuffer-path",
                  "mapbuffer: target 0x%04x buffer %u size %lld access 0x%04x -> glMapBufferRange(flags 0x%x)",
                  target, id, (long long)size, access, flags);
    int slot = fsgl_stream_begin_map(target, id, 0, size, flags);
    void *p = map(target, 0, size, flags);
    fsgl_stream_mapped(slot, p);
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glMapBuffer", "target=0x%x access=0x%x size=%lld",
                   (unsigned)target, (unsigned)access, (long long)size);
    return p;
}

/* ------------------------------------------------------------------ */
/* Buffer range mapping (the streaming path)                           */
/* ------------------------------------------------------------------ */

static void *GL_APIENTRY wrap_glMapBufferRange(GLenum target, GLintptr offset,
                                               GLsizeiptr length, GLbitfield access)
{
    FSGL_PROF_BEGIN(glMapBufferRange);
    fsgl_ring_push("glMapBufferRange", (uintptr_t)target, (uintptr_t)offset,
                   (uintptr_t)length, (uintptr_t)access);

    PFN_glMapBufferRange real =
        (PFN_glMapBufferRange)fsgl_real_id(FSGL_R_glMapBufferRange);
    if (!real) {
        FSGL_PROF_END(0, 0);
        return NULL;
    }
    GLuint buf = fsgl_buf_bound(target);
    if (!buf) {
        /* Some titles map without an explicit bind after the buffer was
         * created; the driver would fail, so do not invent one. */
        fsgl_log_once("maprange-nobind",
                      "maprange: no buffer bound to target 0x%04x", (unsigned)target);
    }
    /* "safe" and "invalidate_range" knobs; see stream.c. */
    access = fsgl_stream_adjust_access(access);
    int slot = fsgl_stream_begin_map(target, buf, offset, length, access);
    void *p = real(target, offset, length, access);
    fsgl_stream_mapped(slot, p);
    FSGL_PROF_END(length > 0 ? (uint64_t)length : 0, 0);
    fsgl_err_check("glMapBufferRange",
                   "target=0x%x offset=%lld length=%lld access=0x%x",
                   (unsigned)target, (long long)offset, (long long)length,
                   (unsigned)access);
    return p;
}

static GLboolean GL_APIENTRY wrap_glUnmapBuffer(GLenum target)
{
    FSGL_PROF_BEGIN(glUnmapBuffer);
    fsgl_ring_push("glUnmapBuffer", (uintptr_t)target, 0, 0, 0);

    PFN_glUnmapBuffer real =
        (PFN_glUnmapBuffer)fsgl_real_id(FSGL_R_glUnmapBuffer);
    if (!real) {
        FSGL_PROF_END(0, 0);
        return GL_TRUE;
    }
    GLuint buf = fsgl_buf_bound(target);
    int slot = fsgl_stream_find_map(target, buf);
    if (slot < 0) {
        slot = fsgl_stream_find_map_recent(target);
    }
    if (slot >= 0 && !fsgl_stream_map_driver_mapped(slot)) {
        /* The range was finished at the draw that consumed it; the driver
         * has nothing left to do. */
        fsgl_stream_end_map(slot, GL_TRUE);
        FSGL_PROF_END(0, 0);
        return GL_TRUE;
    }
    GLboolean ok = real(target);
    fsgl_stream_end_map(slot, ok);
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glUnmapBuffer", "target=0x%x", (unsigned)target);
    return ok;
}

static void GL_APIENTRY wrap_glFlushMappedBufferRange(GLenum target, GLintptr offset,
                                                      GLsizeiptr length)
{
    FSGL_PROF_BEGIN(glFlushMappedBufferRange);
    fsgl_ring_push("glFlushMappedBufferRange", (uintptr_t)target, (uintptr_t)offset,
                   (uintptr_t)length, 0);

    GLuint buf = fsgl_buf_bound(target);
    if (fsgl_stream_find_map(target, buf) < 0) {
        int recent = fsgl_stream_find_map_recent(target);
        if (recent >= 0) {
            buf = fsgl_stream_map_buffer(recent);
        }
    }
    fsgl_stream_flush(target, buf, offset, length);
    int slot = fsgl_stream_find_map(target, buf);
    PFN_glFlushMappedBufferRange real =
        (PFN_glFlushMappedBufferRange)fsgl_real_id(FSGL_R_glFlushMappedBufferRange);
    if (real && (slot < 0 || fsgl_stream_map_driver_mapped(slot))) {
        real(target, offset, length);
    }
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glFlushMappedBufferRange",
                   "target=0x%x offset=%lld length=%lld",
                   (unsigned)target, (long long)offset, (long long)length);
}

/* ------------------------------------------------------------------ */
/* Vertex array state                                                  */
/* ------------------------------------------------------------------ */

static void GL_APIENTRY wrap_glBindVertexArray(GLuint vao)
{
    FSGL_PROF_BEGIN(glBindVertexArray);
    fsgl_ring_push("glBindVertexArray", (uintptr_t)vao, 0, 0, 0);

    fsgl_stream_bind_vao(vao);
    fsgl_stream_trace('V', vao, 0, 0, 0);
    PFN_glBindVertexArray real =
        (PFN_glBindVertexArray)fsgl_real_id(FSGL_R_glBindVertexArray);
    if (real) {
        real(vao);
    }
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glBindVertexArray", "vao=%u", (unsigned)vao);
}

static void GL_APIENTRY wrap_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                                   GLboolean normalized, GLsizei stride,
                                                   const void *pointer)
{
    FSGL_PROF_BEGIN(glVertexAttribPointer);
    fsgl_ring_push("glVertexAttribPointer", (uintptr_t)index, (uintptr_t)size,
                   (uintptr_t)type, (uintptr_t)pointer);

    fsgl_stream_attrib_pointer(index, size, type, stride, pointer);
    fsgl_stream_trace('A', fsgl_buf_bound(GL_ARRAY_BUFFER),
                      (long long)(uintptr_t)pointer, (long long)index, (long long)stride);
    PFN_glVertexAttribPointer real =
        (PFN_glVertexAttribPointer)fsgl_real_id(FSGL_R_glVertexAttribPointer);
    if (real) {
        real(index, size, type, normalized, stride, pointer);
    }
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glVertexAttribPointer",
                   "index=%u size=%d type=0x%x stride=%d",
                   (unsigned)index, (int)size, (unsigned)type, (int)stride);
}

static void GL_APIENTRY wrap_glEnableVertexAttribArray(GLuint index)
{
    fsgl_stream_attrib_enable(index, true);
    PFN_glEnableVertexAttribArray real =
        (PFN_glEnableVertexAttribArray)fsgl_real_id(FSGL_R_glEnableVertexAttribArray);
    if (real) {
        real(index);
    }
    fsgl_err_check("glEnableVertexAttribArray", "index=%u", (unsigned)index);
}

static void GL_APIENTRY wrap_glDisableVertexAttribArray(GLuint index)
{
    fsgl_stream_attrib_enable(index, false);
    PFN_glDisableVertexAttribArray real =
        (PFN_glDisableVertexAttribArray)fsgl_real_id(FSGL_R_glDisableVertexAttribArray);
    if (real) {
        real(index);
    }
    fsgl_err_check("glDisableVertexAttribArray", "index=%u", (unsigned)index);
}

/* ------------------------------------------------------------------ */
/* Timer queries                                                       */
/* ------------------------------------------------------------------ */

static void GL_APIENTRY wrap_glQueryCounter(GLuint id, GLenum target)
{
    FSGL_PROF_BEGIN(glQueryCounter);
    fsgl_ring_push("glQueryCounter", (uintptr_t)id, (uintptr_t)target, 0, 0);

    PFN_glQueryCounterEXT ext = (PFN_glQueryCounterEXT)fsgl_real_id(FSGL_R_glQueryCounterEXT);
    if (ext && target == GL_TIMESTAMP) {
        fsgl_log_once("timer-path", "timer: glQueryCounter -> glQueryCounterEXT (EXT_disjoint_timer_query)");
        ext(id, GL_TIMESTAMP);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glQueryCounter", "id=%u target=0x%x", (unsigned)id, (unsigned)target);
        return;
    }
    fsgl_log_once("timer-absent", "timer: glQueryCounter has no EXT back end; dropping timestamp query %u", id);
    FSGL_PROF_END(0, 0);
}

/* Non-blocking availability probe, only on the EXT_disjoint_timer_query path
 * (without it no timestamp was ever issued and the emulation below never
 * blocks).  An invalid id raises the same error the real result query would,
 * so no error bracketing is needed. */
static bool query_result_ready(GLuint id)
{
    PFN_glGetQueryObjectivEXT iv =
        (PFN_glGetQueryObjectivEXT)fsgl_real_id(FSGL_R_glGetQueryObjectivEXT);
    if (!iv || !fsgl_real_id(FSGL_R_glQueryCounterEXT)) {
        return true;
    }
    GLint v = 1;
    iv(id, GL_QUERY_RESULT_AVAILABLE, &v);
    return v != 0;
}

static void GL_APIENTRY wrap_glGetQueryObjecti64v(GLuint id, GLenum pname, GLint64 *params)
{
    FSGL_PROF_BEGIN(glGetQueryObjecti64v);
    fsgl_ring_push("glGetQueryObjecti64v", (uintptr_t)id, (uintptr_t)pname,
                   (uintptr_t)params, 0);

    if (!params) {
        FSGL_PROF_END(0, 0);
        return;
    }
    if (pname == GL_QUERY_RESULT && !query_result_ready(id)) {
        /* GL_QUERY_RESULT blocks until the GPU has executed the query.  On a
         * tiler that means flushing the open render pass and waiting for the
         * whole frame: CPU and GPU stop overlapping.  The value only feeds
         * timing statistics, so answer "no time" instead of draining. */
        fsgl_log_once("timer-not-ready",
                      "timer: GL_QUERY_RESULT for query %u not available yet; "
                      "returning 0 instead of stalling the pipeline", id);
        *params = 0;
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glGetQueryObjecti64vEXT i64 =
        (PFN_glGetQueryObjecti64vEXT)fsgl_real_id(FSGL_R_glGetQueryObjecti64vEXT);
    if (i64) {
        fsgl_log_once("timer-get-path", "timer: glGetQueryObjecti64v -> glGetQueryObjecti64vEXT");
        i64(id, pname, params);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glGetQueryObjecti64v", "id=%u pname=0x%x",
                       (unsigned)id, (unsigned)pname);
        return;
    }

    /* Emulated result: all driver calls here are layer-internal. */
    fsgl_err_begin_extra("glGetQueryObjecti64v");
    PFN_glGetQueryObjectui64vEXT u64 =
        (PFN_glGetQueryObjectui64vEXT)fsgl_real_id(FSGL_R_glGetQueryObjectui64vEXT);
    if (u64) {
        GLuint64 v = 0;
        u64(id, pname, &v);
        *params = (GLint64)v;
        fsgl_err_end_extra("glGetQueryObjecti64v");
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glGetQueryObjectivEXT iv =
        (PFN_glGetQueryObjectivEXT)fsgl_real_id(FSGL_R_glGetQueryObjectivEXT);
    if (iv && pname != GL_QUERY_RESULT) {
        GLint v = 0;
        iv(id, pname, &v);
        *params = v;
        fsgl_err_end_extra("glGetQueryObjecti64v");
        FSGL_PROF_END(0, 0);
        return;
    }
    fsgl_err_end_extra("glGetQueryObjecti64v");
    fsgl_log_once("timer-result-absent",
                  "timer: no EXT query-result entry point; returning available=1, result=0 for query %u", id);
    *params = (pname == GL_QUERY_RESULT_AVAILABLE) ? 1 : 0;
    FSGL_PROF_END(0, 0);
}

/* ------------------------------------------------------------------ */
/* Instanced base-instance draws                                       */
/* ------------------------------------------------------------------ */

static void GL_APIENTRY wrap_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    FSGL_PROF_BEGIN(glDrawArrays);
    fsgl_ring_push("glDrawArrays", (uintptr_t)mode, (uintptr_t)first,
                   (uintptr_t)count, 0);

    if (fsgl_baseinstance_dirty()) {
        GLint loc;
        PFN_glUniform1i uni = (PFN_glUniform1i)fsgl_real_id(FSGL_R_glUniform1i);
        fsgl_err_begin_extra("glDrawArrays");
        loc = fsgl_baseinstance_location(fsgl_current_program());
        if (loc >= 0 && uni) {
            uni(loc, 0);
        }
        fsgl_err_end_extra("glDrawArrays");
        fsgl_baseinstance_set_dirty(false);
    }
#if FSGL_DIAG
    fsgl_ft_draw(false, false);
#endif
    fsgl_stream_note_draw("glDrawArrays", 0, count, (const void *)(uintptr_t)first);
    PFN_glDrawArrays real = (PFN_glDrawArrays)fsgl_real_id(FSGL_R_glDrawArrays);
    if (real) {
        real(mode, first, count);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDrawArrays", "mode=0x%x first=%d count=%d",
                       (unsigned)mode, (int)first, (int)count);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    FSGL_PROF_BEGIN(glDrawElements);
    fsgl_ring_push("glDrawElements", (uintptr_t)mode, (uintptr_t)count,
                   (uintptr_t)type, (uintptr_t)indices);

    if (fsgl_baseinstance_dirty()) {
        GLint loc;
        PFN_glUniform1i uni = (PFN_glUniform1i)fsgl_real_id(FSGL_R_glUniform1i);
        fsgl_err_begin_extra("glDrawElements");
        loc = fsgl_baseinstance_location(fsgl_current_program());
        if (loc >= 0 && uni) {
            uni(loc, 0);
        }
        fsgl_err_end_extra("glDrawElements");
        fsgl_baseinstance_set_dirty(false);
    }
#if FSGL_DIAG
    fsgl_ft_draw(false, false);
#endif
    fsgl_stream_note_draw("glDrawElements",
                          fsgl_buf_bound(GL_ELEMENT_ARRAY_BUFFER), count, indices);
    PFN_glDrawElements real = (PFN_glDrawElements)fsgl_real_id(FSGL_R_glDrawElements);
    if (real) {
        real(mode, count, type, indices);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDrawElements", "mode=0x%x count=%d type=0x%x",
                       (unsigned)mode, (int)count, (unsigned)type);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glDrawArraysInstancedBaseInstance(GLenum mode, GLint first,
                                                            GLsizei count, GLsizei instancecount,
                                                            GLuint baseinstance)
{
    FSGL_PROF_BEGIN(glDrawArraysInstancedBaseInstance);
    fsgl_ring_push("glDrawArraysInstancedBaseInstance", (uintptr_t)mode,
                   (uintptr_t)first, (uintptr_t)count, (uintptr_t)baseinstance);

#if FSGL_DIAG
    fsgl_ft_draw(true, false);
#endif
    fsgl_stream_note_draw("glDrawArraysInstancedBaseInstance", 0, count,
                          (const void *)(uintptr_t)first);
    PFN_glDrawArraysInstanced draw =
        (PFN_glDrawArraysInstanced)fsgl_real_id(FSGL_R_glDrawArraysInstanced);
    if (!draw) {
        fsgl_log_once("baseinstance-nodraw", "base-instance: glDrawArraysInstanced missing");
        FSGL_PROF_END(0, 0);
        return;
    }
    if (baseinstance != 0) {
        fsgl_baseinstance_mark_used();
        GLint loc;
        PFN_glUniform1i uni = (PFN_glUniform1i)fsgl_real_id(FSGL_R_glUniform1i);
        fsgl_err_begin_extra("glDrawArraysInstancedBaseInstance");
        loc = fsgl_baseinstance_location(fsgl_current_program());
        if (loc >= 0 && uni) {
            uni(loc, (GLint)baseinstance);
            fsgl_baseinstance_set_dirty(true);
        }
        fsgl_err_end_extra("glDrawArraysInstancedBaseInstance");
        fsgl_log_once("baseinstance-nonzero",
                      "base-instance: nonzero baseinstance=%u mode=0x%04x first=%d count=%d instances=%d "
                      "(shader gl_InstanceID offset applied; instanced attribute fetch offset is not)",
                      baseinstance, mode, first, count, instancecount);
    } else if (fsgl_baseinstance_dirty()) {
        GLint loc;
        PFN_glUniform1i uni = (PFN_glUniform1i)fsgl_real_id(FSGL_R_glUniform1i);
        fsgl_err_begin_extra("glDrawArraysInstancedBaseInstance");
        loc = fsgl_baseinstance_location(fsgl_current_program());
        if (loc >= 0 && uni) {
            uni(loc, 0);
        }
        fsgl_err_end_extra("glDrawArraysInstancedBaseInstance");
        fsgl_baseinstance_set_dirty(false);
    }
    draw(mode, first, count, instancecount);
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glDrawArraysInstancedBaseInstance",
                   "mode=0x%x first=%d count=%d instancecount=%d baseinstance=%u",
                   (unsigned)mode, (int)first, (int)count, (int)instancecount,
                   (unsigned)baseinstance);
}

/* ------------------------------------------------------------------ */
/* Program tracking (for fsgl_BaseInstance)                            */
/* ------------------------------------------------------------------ */

static void GL_APIENTRY wrap_glUseProgram(GLuint program)
{
    FSGL_PROF_BEGIN(glUseProgram);
    fsgl_ring_push("glUseProgram", (uintptr_t)program, 0, 0, 0);

    fsgl_use_program(program);
    PFN_glUseProgram real = (PFN_glUseProgram)fsgl_real_id(FSGL_R_glUseProgram);
    if (real) {
        real(program);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glUseProgram", "program=%u", (unsigned)program);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glDeleteProgram(GLuint program)
{
    FSGL_PROF_BEGIN(glDeleteProgram);
    fsgl_ring_push("glDeleteProgram", (uintptr_t)program, 0, 0, 0);

    if (fsgl_current_program() == program) {
        fsgl_use_program(0);
    }
    PFN_glDeleteProgram real = (PFN_glDeleteProgram)fsgl_real_id(FSGL_R_glDeleteProgram);
    if (real) {
        real(program);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDeleteProgram", "program=%u", (unsigned)program);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Debug callbacks and ES-3.2-only entry points                        */
/* ------------------------------------------------------------------ */

static void GL_APIENTRY wrap_glDebugMessageCallback(GLDEBUGPROC callback, const void *userParam)
{
    FSGL_PROF_BEGIN(glDebugMessageCallback);
    fsgl_ring_push("glDebugMessageCallback", (uintptr_t)callback, (uintptr_t)userParam, 0, 0);

    PFN_glDebugMessageCallback core =
        (PFN_glDebugMessageCallback)fsgl_real_id(FSGL_R_glDebugMessageCallback);
    if (core) {
        fsgl_log_once("debug-path", "debug: glDebugMessageCallback (ES 3.2 core)");
        core(callback, userParam);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDebugMessageCallback", "");
        return;
    }
    PFN_glDebugMessageCallbackKHR khr =
        (PFN_glDebugMessageCallbackKHR)fsgl_real_id(FSGL_R_glDebugMessageCallbackKHR);
    if (khr) {
        fsgl_log_once("debug-path-khr", "debug: glDebugMessageCallback -> glDebugMessageCallbackKHR");
        khr(callback, userParam);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDebugMessageCallback", "");
        return;
    }
    fsgl_log_once("debug-absent", "debug: glDebugMessageCallback unsupported; callback ignored");
    FSGL_PROF_END(0, 0);
}

static void GL_APIENTRY wrap_glDebugMessageControl(GLenum source, GLenum type, GLenum severity,
                                                GLsizei count, const GLuint *ids, GLboolean enabled)
{
    FSGL_PROF_BEGIN(glDebugMessageControl);
    fsgl_ring_push("glDebugMessageControl", (uintptr_t)source, (uintptr_t)type,
                   (uintptr_t)severity, (uintptr_t)count);

    PFN_glDebugMessageControl core =
        (PFN_glDebugMessageControl)fsgl_real_id(FSGL_R_glDebugMessageControl);
    if (core) {
        core(source, type, severity, count, ids, enabled);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDebugMessageControl", "source=0x%x type=0x%x severity=0x%x count=%d",
                       (unsigned)source, (unsigned)type, (unsigned)severity, (int)count);
        return;
    }
    PFN_glDebugMessageControlKHR khr =
        (PFN_glDebugMessageControlKHR)fsgl_real_id(FSGL_R_glDebugMessageControlKHR);
    if (khr) {
        khr(source, type, severity, count, ids, enabled);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glDebugMessageControl", "source=0x%x type=0x%x severity=0x%x count=%d",
                       (unsigned)source, (unsigned)type, (unsigned)severity, (int)count);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    FSGL_PROF_BEGIN(glColorMaski);
    fsgl_ring_push("glColorMaski", (uintptr_t)index, (uintptr_t)r,
                   (uintptr_t)g, (uintptr_t)b);

    PFN_glColorMaski real = (PFN_glColorMaski)fsgl_real_id(FSGL_R_glColorMaski);
    if (real) {
        real(index, r, g, b, a);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glColorMaski", "index=%u r=%d g=%d b=%d a=%d",
                       (unsigned)index, (int)r, (int)g, (int)b, (int)a);
    } else {
        fsgl_log_once("colormaski-absent", "colorMaski: unsupported; draw buffer %u mask ignored", index);
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glCopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel,
                                             GLint srcX, GLint srcY, GLint srcZ, GLuint dstName,
                                             GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY,
                                             GLint dstZ, GLsizei srcWidth, GLsizei srcHeight,
                                             GLsizei srcDepth)
{
    FSGL_PROF_BEGIN(glCopyImageSubData);
    fsgl_ring_push("glCopyImageSubData", (uintptr_t)srcName, (uintptr_t)srcTarget,
                   (uintptr_t)srcLevel, (uintptr_t)srcX);

    PFN_glCopyImageSubData real =
        (PFN_glCopyImageSubData)fsgl_real_id(FSGL_R_glCopyImageSubData);
    if (real) {
        real(srcName, srcTarget, srcLevel, srcX, srcY, srcZ, dstName, dstTarget, dstLevel,
             dstX, dstY, dstZ, srcWidth, srcHeight, srcDepth);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glCopyImageSubData",
                       "src=%u srcTarget=0x%x srcLevel=%d srcX=%d srcY=%d srcZ=%d "
                       "dst=%u dstTarget=0x%x dstLevel=%d dstX=%d dstY=%d dstZ=%d "
                       "w=%d h=%d d=%d",
                       (unsigned)srcName, (unsigned)srcTarget, (int)srcLevel,
                       (int)srcX, (int)srcY, (int)srcZ, (unsigned)dstName,
                       (unsigned)dstTarget, (int)dstLevel, (int)dstX, (int)dstY,
                       (int)dstZ, (int)srcWidth, (int)srcHeight, (int)srcDepth);
    } else {
        fsgl_log_once("copyimage-absent", "copyImageSubData: unsupported; src=%u dst=%u skipped",
                      srcName, dstName);
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer)
{
    FSGL_PROF_BEGIN(glTexBuffer);
    fsgl_ring_push("glTexBuffer", (uintptr_t)target, (uintptr_t)internalformat,
                   (uintptr_t)buffer, 0);

    PFN_glTexBuffer real = (PFN_glTexBuffer)fsgl_real_id(FSGL_R_glTexBuffer);
    if (real) {
        real(target, internalformat, buffer);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glTexBuffer", "target=0x%x internalformat=0x%x buffer=%u",
                       (unsigned)target, (unsigned)internalformat, (unsigned)buffer);
    } else {
        fsgl_log_once("texbuffer-absent", "texBuffer: unsupported; target 0x%04x skipped", target);
        FSGL_PROF_END(0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Sampler / texture parameters                                        */
/* ------------------------------------------------------------------ */

/* GLES 3.2 has no per-texture LOD bias, no sampler LOD bias, no min/max LOD
 * and no border colour.  The desktop game sets them; forwarding them makes
 * the driver raise INVALID_ENUM (14x per run at createSamplerState:518).
 * Drop them with a one-time log instead of faking a value. */
static bool drop_tex_pname(GLenum pname, const char *fn)
{
    switch (pname) {
    case GL_TEXTURE_MIN_LOD:
        fsgl_log_once("drop-0x813a",
                      "%s: dropping GL_TEXTURE_MIN_LOD (0x813a): not valid in GLES 3.2; value not faked",
                      fn);
        return true;
    case GL_TEXTURE_MAX_LOD:
        fsgl_log_once("drop-0x813b",
                      "%s: dropping GL_TEXTURE_MAX_LOD (0x813b): not valid in GLES 3.2; value not faked",
                      fn);
        return true;
    case GL_TEXTURE_LOD_BIAS:
        fsgl_log_once("drop-0x8501",
                      "%s: dropping GL_TEXTURE_LOD_BIAS (0x8501): not valid in GLES 3.2; value not faked",
                      fn);
        return true;
    case GL_TEXTURE_BORDER_COLOR:
        fsgl_log_once("drop-0x1004",
                      "%s: dropping GL_TEXTURE_BORDER_COLOR (0x1004): not valid in GLES 3.2; value not faked",
                      fn);
        return true;
    default:
        return false;
    }
}

static void GL_APIENTRY wrap_glSamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
    FSGL_PROF_BEGIN(glSamplerParameteri);
    fsgl_ring_push("glSamplerParameteri", (uintptr_t)sampler, (uintptr_t)pname,
                   (uintptr_t)param, 0);

    if (drop_tex_pname(pname, "glSamplerParameteri")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glSamplerParameteri real =
        (PFN_glSamplerParameteri)fsgl_real_id(FSGL_R_glSamplerParameteri);
    if (real) {
        real(sampler, pname, param);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glSamplerParameteri", "sampler=%u pname=0x%x param=%d",
                       (unsigned)sampler, (unsigned)pname, (int)param);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param)
{
    FSGL_PROF_BEGIN(glSamplerParameterf);
    fsgl_ring_push("glSamplerParameterf", (uintptr_t)sampler, (uintptr_t)pname, 0, 0);

    if (drop_tex_pname(pname, "glSamplerParameterf")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glSamplerParameterf real =
        (PFN_glSamplerParameterf)fsgl_real_id(FSGL_R_glSamplerParameterf);
    if (real) {
        real(sampler, pname, param);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glSamplerParameterf", "sampler=%u pname=0x%x param=%g",
                       (unsigned)sampler, (unsigned)pname, (double)param);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glSamplerParameterfv(GLuint sampler, GLenum pname, const GLfloat *params)
{
    FSGL_PROF_BEGIN(glSamplerParameterfv);
    fsgl_ring_push("glSamplerParameterfv", (uintptr_t)sampler, (uintptr_t)pname,
                   (uintptr_t)params, 0);

    if (drop_tex_pname(pname, "glSamplerParameterfv")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glSamplerParameterfv real =
        (PFN_glSamplerParameterfv)fsgl_real_id(FSGL_R_glSamplerParameterfv);
    if (real) {
        real(sampler, pname, params);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glSamplerParameterfv", "sampler=%u pname=0x%x value=%g",
                       (unsigned)sampler, (unsigned)pname, params ? (double)params[0] : 0.0);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glSamplerParameteriv(GLuint sampler, GLenum pname, const GLint *params)
{
    FSGL_PROF_BEGIN(glSamplerParameteriv);
    fsgl_ring_push("glSamplerParameteriv", (uintptr_t)sampler, (uintptr_t)pname,
                   (uintptr_t)params, 0);

    if (drop_tex_pname(pname, "glSamplerParameteriv")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glSamplerParameteriv real =
        (PFN_glSamplerParameteriv)fsgl_real_id(FSGL_R_glSamplerParameteriv);
    if (real) {
        real(sampler, pname, params);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glSamplerParameteriv", "sampler=%u pname=0x%x value=%d",
                       (unsigned)sampler, (unsigned)pname, params ? (int)params[0] : 0);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    FSGL_PROF_BEGIN(glTexParameteri);
    fsgl_ring_push("glTexParameteri", (uintptr_t)target, (uintptr_t)pname,
                   (uintptr_t)param, 0);

    if (drop_tex_pname(pname, "glTexParameteri")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glTexParameteri real = (PFN_glTexParameteri)fsgl_real_id(FSGL_R_glTexParameteri);
    if (real) {
        real(target, pname, param);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glTexParameteri", "target=0x%x pname=0x%x param=%d",
                       (unsigned)target, (unsigned)pname, (int)param);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    FSGL_PROF_BEGIN(glTexParameterf);
    fsgl_ring_push("glTexParameterf", (uintptr_t)target, (uintptr_t)pname, 0, 0);

    if (drop_tex_pname(pname, "glTexParameterf")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glTexParameterf real = (PFN_glTexParameterf)fsgl_real_id(FSGL_R_glTexParameterf);
    if (real) {
        real(target, pname, param);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glTexParameterf", "target=0x%x pname=0x%x param=%g",
                       (unsigned)target, (unsigned)pname, (double)param);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    FSGL_PROF_BEGIN(glTexParameterfv);
    fsgl_ring_push("glTexParameterfv", (uintptr_t)target, (uintptr_t)pname,
                   (uintptr_t)params, 0);

    if (drop_tex_pname(pname, "glTexParameterfv")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glTexParameterfv real = (PFN_glTexParameterfv)fsgl_real_id(FSGL_R_glTexParameterfv);
    if (real) {
        real(target, pname, params);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glTexParameterfv", "target=0x%x pname=0x%x value=%g",
                       (unsigned)target, (unsigned)pname, params ? (double)params[0] : 0.0);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

static void GL_APIENTRY wrap_glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    FSGL_PROF_BEGIN(glTexParameteriv);
    fsgl_ring_push("glTexParameteriv", (uintptr_t)target, (uintptr_t)pname,
                   (uintptr_t)params, 0);

    if (drop_tex_pname(pname, "glTexParameteriv")) {
        FSGL_PROF_END(0, 0);
        return;
    }
    PFN_glTexParameteriv real = (PFN_glTexParameteriv)fsgl_real_id(FSGL_R_glTexParameteriv);
    if (real) {
        real(target, pname, params);
        FSGL_PROF_END(0, 0);
        fsgl_err_check("glTexParameteriv", "target=0x%x pname=0x%x value=%d",
                       (unsigned)target, (unsigned)pname, params ? (int)params[0] : 0);
    } else {
        FSGL_PROF_END(0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Stall probes (FSGL_DIAG only)                                       */
/* ------------------------------------------------------------------ */

#if FSGL_DIAG
/* Timing-only pass-throughs for the calls where a CPU<->GPU sync usually
 * hides.  Before these existed the profile could not see such a stall at
 * all: the game called the driver directly. */
static GLenum GL_APIENTRY wrap_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    FSGL_PROF_BEGIN(glClientWaitSync);
    fsgl_ring_push("glClientWaitSync", (uintptr_t)sync, (uintptr_t)flags,
                   (uintptr_t)timeout, 0);
    PFN_glClientWaitSync real = (PFN_glClientWaitSync)fsgl_real_id(FSGL_R_glClientWaitSync);
    GLenum r = real ? real(sync, flags, timeout) : GL_WAIT_FAILED;
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glClientWaitSync", "flags=0x%x timeout=%llu",
                   (unsigned)flags, (unsigned long long)timeout);
    return r;
}

static void GL_APIENTRY wrap_glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    FSGL_PROF_BEGIN(glWaitSync);
    fsgl_ring_push("glWaitSync", (uintptr_t)sync, (uintptr_t)flags, 0, 0);
    PFN_glWaitSync real = (PFN_glWaitSync)fsgl_real_id(FSGL_R_glWaitSync);
    if (real) {
        real(sync, flags, timeout);
    }
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glWaitSync", "flags=0x%x", (unsigned)flags);
}

static void GL_APIENTRY wrap_glFinish(void)
{
    FSGL_PROF_BEGIN(glFinish);
    fsgl_ring_push("glFinish", 0, 0, 0, 0);
    PFN_glFinish real = (PFN_glFinish)fsgl_real_id(FSGL_R_glFinish);
    if (real) {
        real();
    }
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glFinish", "");
}

static void GL_APIENTRY wrap_glFlush(void)
{
    FSGL_PROF_BEGIN(glFlush);
    fsgl_ring_push("glFlush", 0, 0, 0, 0);
    PFN_glFlush real = (PFN_glFlush)fsgl_real_id(FSGL_R_glFlush);
    if (real) {
        real();
    }
    FSGL_PROF_END(0, 0);
    fsgl_err_check("glFlush", "");
}

static void GL_APIENTRY wrap_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                          GLenum format, GLenum type, void *pixels)
{
    FSGL_PROF_BEGIN(glReadPixels);
    fsgl_ring_push("glReadPixels", (uintptr_t)width, (uintptr_t)height,
                   (uintptr_t)format, (uintptr_t)type);
    PFN_glReadPixels real = (PFN_glReadPixels)fsgl_real_id(FSGL_R_glReadPixels);
    if (real) {
        real(x, y, width, height, format, type, pixels);
    }
    FSGL_PROF_END(0, width > 0 && height > 0 ? (uint64_t)width * (uint64_t)height * 4u : 0);
    fsgl_err_check("glReadPixels", "x=%d y=%d w=%d h=%d fmt=0x%x type=0x%x",
                   (int)x, (int)y, (int)width, (int)height, (unsigned)format, (unsigned)type);
}

static void GL_APIENTRY wrap_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                             const void *data)
{
    FSGL_PROF_BEGIN(glBufferSubData);
    fsgl_ring_push("glBufferSubData", (uintptr_t)target, (uintptr_t)offset,
                   (uintptr_t)size, 0);
    PFN_glBufferSubData real = (PFN_glBufferSubData)fsgl_real_id(FSGL_R_glBufferSubData);
    if (real) {
        real(target, offset, size, data);
    }
    FSGL_PROF_END(size > 0 ? (uint64_t)size : 0, 0);
    fsgl_err_check("glBufferSubData", "target=0x%x offset=%lld size=%lld",
                   (unsigned)target, (long long)offset, (long long)size);
}
#endif

/* ------------------------------------------------------------------ */
/* glGetError                                                          */
/* ------------------------------------------------------------------ */

static GLenum GL_APIENTRY wrap_glGetError(void)
{
    FSGL_PROF_BEGIN(glGetError);
    GLenum err = fsgl_err_wrap_get_error((uintptr_t)__builtin_return_address(0));
    FSGL_PROF_END(0, 0);
    return err;
}

/* Texture upload/readback wrappers live in wrap_tex.c. */
void GL_APIENTRY fsgl_wrap_glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void GL_APIENTRY fsgl_wrap_glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
void GL_APIENTRY fsgl_wrap_glTexImage3D(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void GL_APIENTRY fsgl_wrap_glTexSubImage3D(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
void GL_APIENTRY fsgl_wrap_glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
void GL_APIENTRY fsgl_wrap_glCompressedTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
void GL_APIENTRY fsgl_wrap_glCompressedTexSubImage3D(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *);
void GL_APIENTRY fsgl_wrap_glGetTexImage(GLenum, GLint, GLenum, GLenum, void *);
void GL_APIENTRY fsgl_wrap_glGetCompressedTexImage(GLenum, GLint, void *);
void GL_APIENTRY fsgl_wrap_glGetTexLevelParameteriv(GLenum, GLint, GLenum, GLint *);
void GL_APIENTRY fsgl_wrap_glGenerateMipmap(GLenum);

#if FSGL_DIAG
/* frametrace.c */
void GL_APIENTRY fsgl_ft_glBindFramebuffer(GLenum, GLuint);
void GL_APIENTRY fsgl_ft_glViewport(GLint, GLint, GLsizei, GLsizei);
void GL_APIENTRY fsgl_ft_glClear(GLbitfield);
void GL_APIENTRY fsgl_ft_glClearBufferfv(GLenum, GLint, const GLfloat *);
void GL_APIENTRY fsgl_ft_glClearBufferfi(GLenum, GLint, GLfloat, GLint);
void GL_APIENTRY fsgl_ft_glInvalidateFramebuffer(GLenum, GLsizei, const GLenum *);
void GL_APIENTRY fsgl_ft_glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                           GLbitfield, GLenum);
void GL_APIENTRY fsgl_ft_glDrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei);
void GL_APIENTRY fsgl_ft_glDrawElementsInstanced(GLenum, GLsizei, GLenum, const void *, GLsizei);
void GL_APIENTRY fsgl_ft_glDrawElementsBaseVertex(GLenum, GLsizei, GLenum, const void *, GLint);
void GL_APIENTRY fsgl_ft_glDrawRangeElements(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
void GL_APIENTRY fsgl_ft_glAttachShader(GLuint, GLuint);
#endif

/* ------------------------------------------------------------------ */
/* Lookup table                                                        */
/* ------------------------------------------------------------------ */

/* How a lookup is served.
 *   E_WRAP  always the wrapper: it translates or keeps state the layer needs.
 *   E_DIAG  the wrapper only does bookkeeping/timing.  Release builds return
 *           NULL so SDL resolves the driver's own entry point (zero
 *           overhead); when the driver lacks it the wrapper is still
 *           returned, because it degrades to a logged no-op instead of a
 *           NULL call.  The stall probes exist only in FSGL_DIAG builds. */
enum { E_WRAP, E_DIAG };

typedef struct {
    const char *name;
    FSGLProc fn;
    int kind;
} Entry;

static const Entry k_entries[] = {
    { "glGetString", (FSGLProc)wrap_glGetString, E_WRAP },
    { "glGetStringi", (FSGLProc)wrap_glGetStringi, E_WRAP },
    { "glGetIntegerv", (FSGLProc)wrap_glGetIntegerv, E_WRAP },
    { "glGetError", (FSGLProc)wrap_glGetError, E_WRAP },
    { "glShaderSource", (FSGLProc)wrap_glShaderSource, E_WRAP },
    { "glCompileShader", (FSGLProc)wrap_glCompileShader, E_WRAP },
    { "glDeleteShader", (FSGLProc)wrap_glDeleteShader, E_WRAP },
    { "glBindTexture", (FSGLProc)wrap_glBindTexture, E_WRAP },
    { "glDeleteTextures", (FSGLProc)wrap_glDeleteTextures, E_WRAP },
    { "glBindBuffer", (FSGLProc)wrap_glBindBuffer, E_WRAP },
    { "glBufferData", (FSGLProc)wrap_glBufferData, E_WRAP },
    { "glDeleteBuffers", (FSGLProc)wrap_glDeleteBuffers, E_WRAP },
    { "glMapBuffer", (FSGLProc)wrap_glMapBuffer, E_WRAP },
    { "glMapBufferRange", (FSGLProc)wrap_glMapBufferRange, E_WRAP },
    { "glUnmapBuffer", (FSGLProc)wrap_glUnmapBuffer, E_WRAP },
    { "glFlushMappedBufferRange", (FSGLProc)wrap_glFlushMappedBufferRange, E_WRAP },
    /* Keeps the per-VAO element-buffer binding the map/unmap path relies on. */
    { "glBindVertexArray", (FSGLProc)wrap_glBindVertexArray, E_WRAP },
    /* Attribute tracking only feeds the stream report: early unmap finishes
     * every open write map at a draw and never consults it. */
    { "glVertexAttribPointer", (FSGLProc)wrap_glVertexAttribPointer, E_DIAG },
    { "glEnableVertexAttribArray", (FSGLProc)wrap_glEnableVertexAttribArray, E_DIAG },
    { "glDisableVertexAttribArray", (FSGLProc)wrap_glDisableVertexAttribArray, E_DIAG },
    { "glDrawArrays", (FSGLProc)wrap_glDrawArrays, E_WRAP },
    { "glDrawElements", (FSGLProc)wrap_glDrawElements, E_WRAP },
    { "glQueryCounter", (FSGLProc)wrap_glQueryCounter, E_WRAP },
    { "glGetQueryObjecti64v", (FSGLProc)wrap_glGetQueryObjecti64v, E_WRAP },
    { "glDrawArraysInstancedBaseInstance", (FSGLProc)wrap_glDrawArraysInstancedBaseInstance, E_WRAP },
    { "glUseProgram", (FSGLProc)wrap_glUseProgram, E_WRAP },
    { "glDeleteProgram", (FSGLProc)wrap_glDeleteProgram, E_WRAP },
    { "glDebugMessageCallback", (FSGLProc)wrap_glDebugMessageCallback, E_WRAP },
    { "glDebugMessageControl", (FSGLProc)wrap_glDebugMessageControl, E_WRAP },
    /* Plain forwards when the driver has them. */
    { "glColorMaski", (FSGLProc)wrap_glColorMaski, E_DIAG },
    { "glCopyImageSubData", (FSGLProc)wrap_glCopyImageSubData, E_DIAG },
    { "glTexBuffer", (FSGLProc)wrap_glTexBuffer, E_DIAG },
#if FSGL_DIAG
    { "glClientWaitSync", (FSGLProc)wrap_glClientWaitSync, E_DIAG },
    { "glWaitSync", (FSGLProc)wrap_glWaitSync, E_DIAG },
    { "glFinish", (FSGLProc)wrap_glFinish, E_DIAG },
    { "glFlush", (FSGLProc)wrap_glFlush, E_DIAG },
    { "glReadPixels", (FSGLProc)wrap_glReadPixels, E_DIAG },
    { "glBufferSubData", (FSGLProc)wrap_glBufferSubData, E_DIAG },
    { "glBindFramebuffer", (FSGLProc)fsgl_ft_glBindFramebuffer, E_DIAG },
    { "glViewport", (FSGLProc)fsgl_ft_glViewport, E_DIAG },
    { "glClear", (FSGLProc)fsgl_ft_glClear, E_DIAG },
    { "glClearBufferfv", (FSGLProc)fsgl_ft_glClearBufferfv, E_DIAG },
    { "glClearBufferfi", (FSGLProc)fsgl_ft_glClearBufferfi, E_DIAG },
    { "glInvalidateFramebuffer", (FSGLProc)fsgl_ft_glInvalidateFramebuffer, E_DIAG },
    { "glBlitFramebuffer", (FSGLProc)fsgl_ft_glBlitFramebuffer, E_DIAG },
    { "glDrawArraysInstanced", (FSGLProc)fsgl_ft_glDrawArraysInstanced, E_DIAG },
    { "glDrawElementsInstanced", (FSGLProc)fsgl_ft_glDrawElementsInstanced, E_DIAG },
    { "glDrawElementsBaseVertex", (FSGLProc)fsgl_ft_glDrawElementsBaseVertex, E_DIAG },
    { "glDrawRangeElements", (FSGLProc)fsgl_ft_glDrawRangeElements, E_DIAG },
    { "glAttachShader", (FSGLProc)fsgl_ft_glAttachShader, E_DIAG },
#endif

    { "glSamplerParameteri", (FSGLProc)wrap_glSamplerParameteri, E_WRAP },
    { "glSamplerParameterf", (FSGLProc)wrap_glSamplerParameterf, E_WRAP },
    { "glSamplerParameterfv", (FSGLProc)wrap_glSamplerParameterfv, E_WRAP },
    { "glSamplerParameteriv", (FSGLProc)wrap_glSamplerParameteriv, E_WRAP },
    { "glTexParameteri", (FSGLProc)wrap_glTexParameteri, E_WRAP },
    { "glTexParameterf", (FSGLProc)wrap_glTexParameterf, E_WRAP },
    { "glTexParameterfv", (FSGLProc)wrap_glTexParameterfv, E_WRAP },
    { "glTexParameteriv", (FSGLProc)wrap_glTexParameteriv, E_WRAP },

    { "glTexImage2D", (FSGLProc)fsgl_wrap_glTexImage2D, E_WRAP },
    { "glTexSubImage2D", (FSGLProc)fsgl_wrap_glTexSubImage2D, E_WRAP },
    { "glTexImage3D", (FSGLProc)fsgl_wrap_glTexImage3D, E_WRAP },
    { "glTexSubImage3D", (FSGLProc)fsgl_wrap_glTexSubImage3D, E_WRAP },
    { "glCompressedTexImage2D", (FSGLProc)fsgl_wrap_glCompressedTexImage2D, E_WRAP },
    { "glCompressedTexSubImage2D", (FSGLProc)fsgl_wrap_glCompressedTexSubImage2D, E_WRAP },
    { "glCompressedTexSubImage3D", (FSGLProc)fsgl_wrap_glCompressedTexSubImage3D, E_WRAP },
    { "glGetTexImage", (FSGLProc)fsgl_wrap_glGetTexImage, E_WRAP },
    { "glGetCompressedTexImage", (FSGLProc)fsgl_wrap_glGetCompressedTexImage, E_WRAP },
    { "glGetTexLevelParameteriv", (FSGLProc)fsgl_wrap_glGetTexLevelParameteriv, E_WRAP },
    { "glGenerateMipmap", (FSGLProc)fsgl_wrap_glGenerateMipmap, E_WRAP },
};

__attribute__((visibility("default")))
void *fsgl_lookup(const char *name)
{
    if (!name) {
        return NULL;
    }
    /* Panfrost (Mesa) mode: FACTORIO_MESA_EGL=1 means the game runs on real
     * desktop GL via Mesa's EGL - every entry point comes from eglGetProc-
     * Address and this layer must stay out of the way. */
    static int mesa_mode = -1;
    if (mesa_mode < 0) {
        mesa_mode = getenv("FACTORIO_MESA_EGL") ? 1 : 0;
    }
    if (mesa_mode) {
        fsgl_log_once("lookup-mesa", "lookup: Mesa panfrost mode active; fsgl_lookup passthrough");
        return NULL;
    }
    for (size_t i = 0; i < sizeof(k_entries) / sizeof(k_entries[0]); i++) {
        if (strcmp(name, k_entries[i].name) == 0) {
#if !FSGL_DIAG
            /* NULL hands the name back to SDL's EGL resolution, exactly as
             * for any entry point this layer does not wrap. */
            if (k_entries[i].kind == E_DIAG && fsgl_have(name)) {
                return NULL;
            }
#endif
            fsgl_log_once("lookup-first", "lookup: fsgl_lookup('%s') -> wrapper", name);
            return (void *)k_entries[i].fn;
        }
    }
    return NULL;
}
