/* Always-on per-wrapper profiler for libGLCompat.
 *
 * Hot path: one pointer to a static counter block, two clock_gettime calls,
 * five integer adds.  No logging, no locks, no allocation.
 *
 * Dump format (tag "glcompat-perf", one line per wrapper active in the
 * interval, sorted by descending total time):
 *
 *   glcompat-perf: glBindTexture calls=18422 total=812.3ms max=1.9ms
 *   glcompat-perf: glTexImage2D calls=8 total=120.4ms max=80.1ms bytes_in=38174720 bytes_out=0
 *
 * bytes_in/bytes_out are present only for wrappers that move pixel/buffer
 * bytes (has_bytes).  Counters cover the interval since the previous dump;
 * they are reset by the dump.  A final dump runs from atexit().
 *
 * Factorio drives GL from one thread, so the counters are plain (non-atomic)
 * read-modify-write.  The dump thread only reads and zeroes them; a call in
 * flight during a dump may be counted in either interval, never lost twice.
 */
#include "glcompat.h"

#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define PROF_TAG "glcompat-perf"
#define PROF_INTERVAL_SEC 10

FsglProf fsgl_prof_table[FSGL_P_COUNT] = {
    [FSGL_P_glGetString] = { "glGetString", 0, 0, 0, 0, 0, false },
    [FSGL_P_glGetStringi] = { "glGetStringi", 0, 0, 0, 0, 0, false },
    [FSGL_P_glGetIntegerv] = { "glGetIntegerv", 0, 0, 0, 0, 0, false },
    [FSGL_P_glGetError] = { "glGetError", 0, 0, 0, 0, 0, false },

    [FSGL_P_glShaderSource] = { "glShaderSource", 0, 0, 0, 0, 0, false },
    [FSGL_P_glCompileShader] = { "glCompileShader", 0, 0, 0, 0, 0, false },
    [FSGL_P_glDeleteShader] = { "glDeleteShader", 0, 0, 0, 0, 0, false },

    [FSGL_P_glBindTexture] = { "glBindTexture", 0, 0, 0, 0, 0, false },
    [FSGL_P_glDeleteTextures] = { "glDeleteTextures", 0, 0, 0, 0, 0, false },
    [FSGL_P_glBindBuffer] = { "glBindBuffer", 0, 0, 0, 0, 0, false },
    [FSGL_P_glBufferData] = { "glBufferData", 0, 0, 0, 0, 0, true },
    [FSGL_P_glDeleteBuffers] = { "glDeleteBuffers", 0, 0, 0, 0, 0, false },
    [FSGL_P_glMapBuffer] = { "glMapBuffer", 0, 0, 0, 0, 0, false },
    [FSGL_P_glMapBufferRange] = { "glMapBufferRange", 0, 0, 0, 0, 0, true },
    [FSGL_P_glUnmapBuffer] = { "glUnmapBuffer", 0, 0, 0, 0, 0, false },
    [FSGL_P_glFlushMappedBufferRange] = { "glFlushMappedBufferRange", 0, 0, 0, 0, 0, false },
    [FSGL_P_glBindVertexArray] = { "glBindVertexArray", 0, 0, 0, 0, 0, false },
    [FSGL_P_glVertexAttribPointer] = { "glVertexAttribPointer", 0, 0, 0, 0, 0, false },

    [FSGL_P_glDrawArrays] = { "glDrawArrays", 0, 0, 0, 0, 0, false },
    [FSGL_P_glDrawElements] = { "glDrawElements", 0, 0, 0, 0, 0, false },
    [FSGL_P_glQueryCounter] = { "glQueryCounter", 0, 0, 0, 0, 0, false },
    [FSGL_P_glGetQueryObjecti64v] = { "glGetQueryObjecti64v", 0, 0, 0, 0, 0, false },
    [FSGL_P_glDrawArraysInstancedBaseInstance] = { "glDrawArraysInstancedBaseInstance", 0, 0, 0, 0, 0, false },
    [FSGL_P_glUseProgram] = { "glUseProgram", 0, 0, 0, 0, 0, false },
    [FSGL_P_glDeleteProgram] = { "glDeleteProgram", 0, 0, 0, 0, 0, false },

    [FSGL_P_glDebugMessageCallback] = { "glDebugMessageCallback", 0, 0, 0, 0, 0, false },
    [FSGL_P_glDebugMessageControl] = { "glDebugMessageControl", 0, 0, 0, 0, 0, false },
    [FSGL_P_glColorMaski] = { "glColorMaski", 0, 0, 0, 0, 0, false },
    [FSGL_P_glCopyImageSubData] = { "glCopyImageSubData", 0, 0, 0, 0, 0, false },
    [FSGL_P_glTexBuffer] = { "glTexBuffer", 0, 0, 0, 0, 0, false },

    [FSGL_P_glTexImage2D] = { "glTexImage2D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glTexSubImage2D] = { "glTexSubImage2D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glTexImage3D] = { "glTexImage3D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glTexSubImage3D] = { "glTexSubImage3D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glCompressedTexImage2D] = { "glCompressedTexImage2D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glCompressedTexSubImage2D] = { "glCompressedTexSubImage2D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glCompressedTexSubImage3D] = { "glCompressedTexSubImage3D", 0, 0, 0, 0, 0, true },
    [FSGL_P_glGetTexImage] = { "glGetTexImage", 0, 0, 0, 0, 0, true },
    [FSGL_P_glGetCompressedTexImage] = { "glGetCompressedTexImage", 0, 0, 0, 0, 0, true },
    [FSGL_P_glGetTexLevelParameteriv] = { "glGetTexLevelParameteriv", 0, 0, 0, 0, 0, false },

    [FSGL_P_glTexParameteri] = { "glTexParameteri", 0, 0, 0, 0, 0, false },
    [FSGL_P_glTexParameterf] = { "glTexParameterf", 0, 0, 0, 0, 0, false },
    [FSGL_P_glTexParameterfv] = { "glTexParameterfv", 0, 0, 0, 0, 0, false },
    [FSGL_P_glTexParameteriv] = { "glTexParameteriv", 0, 0, 0, 0, 0, false },
    [FSGL_P_glSamplerParameteri] = { "glSamplerParameteri", 0, 0, 0, 0, 0, false },
    [FSGL_P_glSamplerParameterf] = { "glSamplerParameterf", 0, 0, 0, 0, 0, false },
    [FSGL_P_glSamplerParameterfv] = { "glSamplerParameterfv", 0, 0, 0, 0, 0, false },
    [FSGL_P_glSamplerParameteriv] = { "glSamplerParameteriv", 0, 0, 0, 0, 0, false },

    [FSGL_P_glClientWaitSync] = { "glClientWaitSync", 0, 0, 0, 0, 0, false },
    [FSGL_P_glWaitSync] = { "glWaitSync", 0, 0, 0, 0, 0, false },
    [FSGL_P_glFinish] = { "glFinish", 0, 0, 0, 0, 0, false },
    [FSGL_P_glFlush] = { "glFlush", 0, 0, 0, 0, 0, false },
    [FSGL_P_glReadPixels] = { "glReadPixels", 0, 0, 0, 0, 0, true },
    [FSGL_P_glBufferSubData] = { "glBufferSubData", 0, 0, 0, 0, 0, true },
};

static void prof_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void prof_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fsgl_vlog(ANDROID_LOG_INFO, PROF_TAG, fmt, ap);
    va_end(ap);
}

uint64_t fsgl_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void prof_dump(void)
{
    FsglProf snap[FSGL_P_COUNT];
    int order[FSGL_P_COUNT];
    int n = 0;

    for (int i = 0; i < FSGL_P_COUNT; i++) {
        FsglProf *p = &fsgl_prof_table[i];
        snap[i] = *p;
        if (p->calls) {
            order[n++] = i;
        }
        p->calls = 0;
        p->total_ns = 0;
        p->max_ns = 0;
        p->bytes_in = 0;
        p->bytes_out = 0;
    }
    if (n == 0) {
        return;
    }

    /* Insertion sort by descending total time; n is ~60. */
    for (int i = 1; i < n; i++) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && snap[order[j]].total_ns < snap[v].total_ns) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }

    for (int i = 0; i < n; i++) {
        const FsglProf *p = &snap[order[i]];
        if (p->has_bytes) {
            prof_log(
                                "%s calls=%llu total=%.1fms max=%.1fms bytes_in=%llu bytes_out=%llu",
                                p->name, (unsigned long long)p->calls,
                                (double)p->total_ns / 1e6, (double)p->max_ns / 1e6,
                                (unsigned long long)p->bytes_in,
                                (unsigned long long)p->bytes_out);
        } else {
            prof_log(
                                "%s calls=%llu total=%.1fms max=%.1fms",
                                p->name, (unsigned long long)p->calls,
                                (double)p->total_ns / 1e6, (double)p->max_ns / 1e6);
        }
    }
}

static void *prof_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts;
        ts.tv_sec = PROF_INTERVAL_SEC;
        ts.tv_nsec = 0;
        while (nanosleep(&ts, &ts) != 0) {
            /* EINTR: finish the remainder. */
        }
        prof_dump();
        fsgl_stream_report();
    }
    return NULL;
}

static pthread_once_t g_prof_once = PTHREAD_ONCE_INIT;

static void prof_init(void)
{
    pthread_t th;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, prof_thread, NULL) != 0) {
            prof_log(
                                "profiler: periodic dump thread unavailable; only the final dump will run");
        }
        pthread_attr_destroy(&attr);
    }
}

void fsgl_prof_start(void)
{
    pthread_once(&g_prof_once, prof_init);
}

/* Release builds start the dump thread only when the stream trace is
 * enabled from glcompat.ini (stream.c calls fsgl_prof_start()). */
#if FSGL_DIAG
__attribute__((constructor)) static void prof_ctor(void)
{
    fsgl_prof_start();
    atexit(prof_dump);
}
#endif
