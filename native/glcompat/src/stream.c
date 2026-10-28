/* Buffer streaming: diagnosis and safe/fast reuse for UNSYNCHRONIZED maps.
 *
 * Why this file exists
 * -------------------
 * Factorio streams per-frame vertex and index data through a per-buffer ring
 * with
 *
 *     p = glMapBufferRange(target, off, len, WRITE|UNSYNCHRONIZED)
 *     ...write p...
 *     glFlushMappedBufferRange(...)        (sometimes)
 *     glDraw*()                            <-- while the range is still mapped
 *     glUnmapBuffer(target)
 *
 * GLES says a buffer must not be dereferenced by any command while a range of
 * it is mapped (only MAP_PERSISTENT storage is exempt), and
 * MAP_UNSYNCHRONIZED means the driver may not wait for, or even preserve,
 * in-flight GPU reads of that range.  Desktop drivers tolerate both; Adreno
 * does not.  Adreno hands out a direct pointer into the buffer object (it is
 * UMA), its tiler consumes vertex data several frames late, and its
 * glFlushMappedBufferRange runs a relocating "stream buffer object" path.
 * The result is the reported flicker: whole batches of vertices alternate
 * between the new and the previous contents.
 *
 * The counter-intuitive part, and the reason this is not just "drop the
 * UNSYNCHRONIZED bit": dropping it makes the vendor driver synchronise on
 * every single map, which serialises the pipeline (measured 60 -> 18.5 fps).
 *
 * This module therefore does the GLES work the driver would otherwise do,
 * but only where it is actually needed:
 *   - it tracks which buffers a draw reads (VAO attribs + element buffer),
 *   - it knows whether a draw touches a currently mapped range,
 *   - it fences each unmap so a later reuse of the same ring region can be
 *     checked without waiting,
 *   - it can finish a map (real unmap) at the first draw that consumes the
 *     written data, which removes the illegal "draw while mapped" window
 *     entirely for the pattern above.
 *
 * Everything is env-gated so the behaviour can be bisected on device:
 *   FACTORIO_GL_TRACE=1   periodic report + event ring (default: FSGL_DIAG)
 *   FACTORIO_GL_FENCE=1   create/poll unmap fences (diagnostic)
 *   FACTORIO_GL_EARLY_UNMAP=1  finish fully-flushed maps before a draw
 *
 * glcompat.ini accepts the same knobs plus "safe" (strip UNSYNCHRONIZED),
 * "invalidate_range" (add MAP_INVALIDATE_RANGE to WRITE|UNSYNCHRONIZED maps,
 * default 1) and "verbose".  invalidate_range=0 is the A/B switch for drivers
 * that implement INVALIDATE_RANGE with a per-map staging copy.
 *
 * The module never issues GL calls unless explicitly enabled, so an
 * instrumentation-only build is a passthrough.
 */
#include "glcompat.h"

#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "glcompat-stream"

#ifndef GL_MAP_INVALIDATE_RANGE_BIT
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#ifndef GL_MAP_FLUSH_EXPLICIT_BIT
#define GL_MAP_FLUSH_EXPLICIT_BIT 0x0040
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_TIMEOUT_IGNORED
#define GL_TIMEOUT_IGNORED 0xffffffffffffffffull
#endif

#define MAX_OPEN 32
#define STREAM_MAX_ID (1u << 22)
/* Zoomed-out Factorio can submit thousands of stream maps before the GPU
 * retires them. Retiring an old slot by deleting its sync would reopen the
 * overwrite race, so keep enough retirement records for a long burst. */
#define FENCE_RING 8192
#define MAX_ATTR 16
#define TRACE_RING 1024
#define MAX_ATTRIBS_REPORTED 8

/* Production defaults; glcompat.ini can override these at runtime.  The
 * trace costs a ring store per bind/attrib/map and a periodic report, so it
 * follows the diagnostics build switch. */
#define STREAM_DEFAULT_TRACE FSGL_DIAG
#define STREAM_DEFAULT_FENCE 0
#define STREAM_DEFAULT_EARLY_UNMAP 1
#define STREAM_DEFAULT_INVALIDATE_RANGE 1

typedef struct {
    bool used;
    GLenum target;
    GLuint buf;
    GLintptr off;
    GLsizeiptr len;
    GLbitfield flags;
    void *ptr;
    uint64_t seq;
    unsigned flushes;
    unsigned draws;             /* draws issued while this range was mapped */
    unsigned draws_before_flush;
    bool flushed_whole;
    bool driver_mapped;         /* real map currently open */
    bool write;
} OpenMap;

typedef struct {
    GLsizeiptr size;
    uint64_t maps;
    uint64_t bytes;
    uint64_t wraps;
    uint64_t draws;
    GLintptr prev_off;
    GLintptr max_end;
    bool seen;
} BufStat;

typedef struct {
    bool used;
    GLuint vao;
    GLuint ebo;
    struct {
        GLuint buf;
        GLintptr off;
        GLsizei stride;
        bool enabled;
    } attr[MAX_ATTR];
} VaoRec;

typedef struct {
    GLsync sync;
    GLuint buf;
    GLintptr off;
    GLsizeiptr len;
    uint64_t buf_maps;          /* BufStat.maps when the fence was created */
    bool used;
} FenceRec;

typedef struct {
    char kind;
    GLenum target;
    GLuint buf;
    GLintptr off;
    GLsizeiptr len;
    GLbitfield flags;
    long long arg;
} TraceRec;

static struct {
    bool inited;
    int trace;
    int fence;
    int early_unmap;
    int early_unmap_set;
    int safe;          /* strip UNSYNCHRONIZED: correctness control group */
    int inval_range;   /* add INVALIDATE_RANGE to WRITE|UNSYNCHRONIZED maps */
    int verbose;
    int fake_s3tc;     /* advertise S3TC/RGTC backed by the ETC2 transcoder */
    int invalidate_ds; /* drop depth/stencil when leaving a framebuffer */

    OpenMap maps[MAX_OPEN];
    int nmaps;
    int maxopen;
    uint64_t seq;

    BufStat **stats;
    GLuint stats_cap;

    VaoRec **vaos;
    GLuint vaos_cap;
    GLuint cur_vao;

    FenceRec fences[FENCE_RING];
    int fence_head;

    TraceRec trace_ring[TRACE_RING];
    int trace_head;

    /* interval counters (reset by fsgl_stream_report) */
    uint64_t c_maps, c_write, c_read, c_unsync, c_explicit, c_inval_range,
             c_inval_buf, c_dbl, c_bytes, c_open_sum, c_open_samples;
    GLsizeiptr c_len_min, c_len_max;
    uint64_t c_unmaps, c_unmap_false, c_flushes, c_flush_noexpl,
             c_flush_after_draw, c_flush_partial;
    uint64_t c_draws, c_draws_mapped, c_draws_read, c_draws_read_range,
             c_draws_before_flush;
    uint64_t c_fence_created, c_fence_poll, c_fence_busy, c_fence_busy_age;
    uint64_t c_early_unmap, c_early_skipped, c_draws_with_attribs;
} S;

#define STREAM_CONFIG_PATH \
    "/storage/emulated/0/Android/data/org.factorio.android/files/glcompat.ini"

static void config_apply(const char *key, const char *val)
{
    int v = atoi(val);
    if (!strcmp(key, "trace")) {
        S.trace = v;
    } else if (!strcmp(key, "fence")) {
        S.fence = v;
    } else if (!strcmp(key, "early_unmap")) {
        S.early_unmap = v;
        S.early_unmap_set = 1;
    } else if (!strcmp(key, "safe")) {
        S.safe = v;
    } else if (!strcmp(key, "invalidate_range")) {
        S.inval_range = v;
    } else if (!strcmp(key, "verbose")) {
        S.verbose = v;
    } else if (!strcmp(key, "fake_s3tc")) {
        S.fake_s3tc = v;
    } else if (!strcmp(key, "invalidate_ds")) {
        S.invalidate_ds = v;
    }
}

/* Optional on-device knobs, so a build can be re-configured without a
 * reinstall: /sdcard/Android/data/org.factorio.android/files/glcompat.ini
 * with "key=value" lines. */
static void stream_read_config(void)
{
    const char *path = getenv("FACTORIO_GL_CONFIG");
    if (!path || !*path) {
        path = STREAM_CONFIG_PATH;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char *k = line;
        while (*k == ' ' || *k == '\t') {
            k++;
        }
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) {
            v[--n] = 0;
        }
        size_t kn = strlen(k);
        while (kn && (k[kn - 1] == ' ' || k[kn - 1] == '\t')) {
            k[--kn] = 0;
        }
        config_apply(k, v);
    }
    fclose(f);
}

static void stream_init(void)
{
    if (S.inited) {
        return;
    }
    S.inited = true;
    const char *t = getenv("FACTORIO_GL_TRACE");
    S.trace = t && *t ? (*t != '0') : STREAM_DEFAULT_TRACE;
    const char *f = getenv("FACTORIO_GL_FENCE");
    /* Fence waiting is available as a conservative fallback, but the game
     * maps very large ring tails. Treating every such map as overlapping
     * serializes the stream. The default fast path is early-unmap; users can
     * select fence=1 when a device's queue is deeper than the game's ring. */
    S.fence = f && *f ? (*f != '0') : STREAM_DEFAULT_FENCE;
    const char *e = getenv("FACTORIO_GL_EARLY_UNMAP");
    S.early_unmap = e && *e && *e != '0';
    S.early_unmap_set = e && *e;
    if (!S.early_unmap_set) {
        /* This is a GLES correctness fix, not a tuning mode.  The game
         * draws from mapped write buffers; Adreno's deferred tiler cannot
         * safely consume that lifetime. */
        S.early_unmap = STREAM_DEFAULT_EARLY_UNMAP;
    }
    S.inval_range = STREAM_DEFAULT_INVALIDATE_RANGE;
    stream_read_config();
    fsgl_log(ANDROID_LOG_INFO,
             "stream: trace=%d fence=%d early_unmap=%d safe=%d invalidate_range=%d verbose=%d "
             "fake_s3tc=%d invalidate_ds=%d",
             S.trace, S.fence, S.early_unmap, S.safe, S.inval_range, S.verbose, S.fake_s3tc,
             S.invalidate_ds);
    if (S.trace) {
        /* The periodic report runs on the profiler's dump thread. */
        fsgl_prof_start();
    }
}

static TraceRec *trace_push(char kind, GLenum target, GLuint buf, GLintptr off,
                            GLsizeiptr len, GLbitfield flags)
{
    TraceRec *r = &S.trace_ring[S.trace_head];
    S.trace_head = (S.trace_head + 1) % TRACE_RING;
    r->kind = kind;
    r->target = target;
    r->buf = buf;
    r->off = off;
    r->len = len;
    r->flags = flags;
    r->arg = 0;
    return r;
}

/* Trace an event that does not carry a map range (binds, attribs, draws).
 * Kept separate so the hot map path stays a single store. */
void fsgl_stream_trace(char kind, GLuint buf, long long off, long long len,
                       long long arg)
{
    if (!S.inited || S.trace < 1) {
        return;
    }
    TraceRec *r = &S.trace_ring[S.trace_head];
    S.trace_head = (S.trace_head + 1) % TRACE_RING;
    r->kind = kind;
    r->target = 0;
    r->buf = buf;
    r->off = (GLintptr)off;
    r->len = (GLsizeiptr)len;
    r->flags = 0;
    r->arg = arg;
}

/* ------------------------------------------------------------------ */
/* Small id-keyed tables                                               */
/* ------------------------------------------------------------------ */

static BufStat *stat_get(GLuint id, bool create)
{
    if (!id || id >= STREAM_MAX_ID) {
        return NULL;
    }
    if (id < S.stats_cap && S.stats[id]) {
        return S.stats[id];
    }
    if (!create) {
        return NULL;
    }
    if (id >= S.stats_cap) {
        GLuint cap = S.stats_cap ? S.stats_cap : 256;
        while (cap <= id) {
            cap *= 2;
        }
        BufStat **next = (BufStat **)realloc(S.stats, (size_t)cap * sizeof(*next));
        if (!next) {
            return NULL;
        }
        memset(next + S.stats_cap, 0, (size_t)(cap - S.stats_cap) * sizeof(*next));
        S.stats = next;
        S.stats_cap = cap;
    }
    BufStat *r = (BufStat *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    S.stats[id] = r;
    return r;
}

static VaoRec *vao_get(GLuint id)
{
    if (id >= STREAM_MAX_ID) {
        return NULL;
    }
    if (id < S.vaos_cap && S.vaos[id]) {
        return S.vaos[id];
    }
    if (id >= S.vaos_cap) {
        GLuint cap = S.vaos_cap ? S.vaos_cap : 32;
        while (cap <= id) {
            cap *= 2;
        }
        VaoRec **next = (VaoRec **)realloc(S.vaos, (size_t)cap * sizeof(*next));
        if (!next) {
            return NULL;
        }
        memset(next + S.vaos_cap, 0, (size_t)(cap - S.vaos_cap) * sizeof(*next));
        S.vaos = next;
        S.vaos_cap = cap;
    }
    VaoRec *r = (VaoRec *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->vao = id;
    S.vaos[id] = r;
    return r;
}

static OpenMap *map_find(GLenum target, GLuint buf)
{
    for (int i = 0; i < MAX_OPEN; i++) {
        OpenMap *m = &S.maps[i];
        if (m->used && m->target == target && m->buf == buf) {
            return m;
        }
    }
    return NULL;
}

static int map_slot(const OpenMap *m)
{
    return (int)(m - S.maps);
}

/* ------------------------------------------------------------------ */
/* Fences                                                              */
/* ------------------------------------------------------------------ */

static void fence_create(GLuint buf, GLintptr off, GLsizeiptr len)
{
    PFN_glFenceSync f = (PFN_glFenceSync)fsgl_real_id(FSGL_R_glFenceSync);
    if (!f) {
        return;
    }
    FenceRec *r = &S.fences[S.fence_head];
    S.fence_head = (S.fence_head + 1) % FENCE_RING;
    if (r->used) {
        PFN_glDeleteSync old_delete =
            (PFN_glDeleteSync)fsgl_real_id(FSGL_R_glDeleteSync);
        if (old_delete) {
            fsgl_err_begin_extra("stream-fence");
            old_delete(r->sync);
            fsgl_err_end_extra("stream-fence");
        }
        r->used = false;
    }
    fsgl_err_begin_extra("stream-fence");
    GLsync s = f(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    fsgl_err_end_extra("stream-fence");
    if (!s) {
        r->used = false;
        return;
    }
    r->sync = s;
    r->buf = buf;
    r->off = off;
    r->len = len;
    r->used = true;
    BufStat *st = stat_get(buf, false);
    r->buf_maps = st ? st->maps : 0;
    S.c_fence_created++;
}

/* Non-blocking: true when every fence covering this range is signalled. */
static bool fence_region_idle(GLuint buf, GLintptr off, GLsizeiptr len)
{
    PFN_glClientWaitSync w = (PFN_glClientWaitSync)fsgl_real_id(FSGL_R_glClientWaitSync);
    PFN_glDeleteSync d = (PFN_glDeleteSync)fsgl_real_id(FSGL_R_glDeleteSync);
    if (!w) {
        return true;
    }
    bool idle = true;
    BufStat *st = stat_get(buf, false);
    uint64_t now_maps = st ? st->maps : 0;
    for (int i = 0; i < FENCE_RING; i++) {
        FenceRec *r = &S.fences[i];
        if (!r->used || r->buf != buf) {
            continue;
        }
        if (r->off >= off + len || off >= r->off + r->len) {
            continue;
        }
        S.c_fence_poll++;
        fsgl_err_begin_extra("stream-fence");
        GLenum res = w(r->sync, 0, 0);
        fsgl_err_end_extra("stream-fence");
        if (res == GL_ALREADY_SIGNALED || res == GL_CONDITION_SATISFIED) {
            if (d) {
                fsgl_err_begin_extra("stream-fence");
                d(r->sync);
                fsgl_err_end_extra("stream-fence");
            }
            r->used = false;
        } else {
            idle = false;
            S.c_fence_busy++;
            uint64_t age = now_maps - r->buf_maps;
            if (age > S.c_fence_busy_age) {
                S.c_fence_busy_age = age;
            }
        }
    }
    return idle;
}

/* A timeout is not a correctness result: writing through an UNSYNC map while
 * a prior draw still reads the range recreates the original corruption.  We
 * therefore wait only for an actually overlapping fence.  Maps whose ranges
 * are not in flight remain completely non-blocking, preserving the fast path
 * that motivated UNSYNCHRONIZED in the first place. */
static void fence_wait_region(GLuint buf, GLintptr off, GLsizeiptr len)
{
    PFN_glClientWaitSync w =
        (PFN_glClientWaitSync)fsgl_real_id(FSGL_R_glClientWaitSync);
    PFN_glDeleteSync d = (PFN_glDeleteSync)fsgl_real_id(FSGL_R_glDeleteSync);
    if (!w) {
        return;
    }
    for (int i = 0; i < FENCE_RING; i++) {
        FenceRec *r = &S.fences[i];
        if (!r->used || r->buf != buf || r->off >= off + len ||
            off >= r->off + r->len) {
            continue;
        }
        fsgl_err_begin_extra("stream-fence-wait");
        GLenum res = w(r->sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                       (GLuint64)GL_TIMEOUT_IGNORED);
        fsgl_err_end_extra("stream-fence-wait");
        S.c_fence_busy++;
        if (res == GL_ALREADY_SIGNALED || res == GL_CONDITION_SATISFIED) {
            if (d) {
                fsgl_err_begin_extra("stream-fence-wait");
                d(r->sync);
                fsgl_err_end_extra("stream-fence-wait");
            }
            r->used = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Map / flush / unmap                                                 */
/* ------------------------------------------------------------------ */

bool fsgl_config_invalidate_ds(void)
{
    stream_init();
    return S.invalidate_ds != 0;
}

bool fsgl_config_fake_s3tc(void)
{
    stream_init();
    return S.fake_s3tc != 0;
}

GLbitfield fsgl_stream_adjust_access(GLbitfield access)
{
    stream_init();
    if (S.safe) {
        access &= ~(GLbitfield)GL_MAP_UNSYNCHRONIZED_BIT;
    }
    /* The game writes the mapped range without reading its previous bytes.
     * This is legal with WRITE and lets Adreno select a fresh backing range
     * instead of preserving old contents, without the full-buffer stall
     * caused by removing UNSYNCHRONIZED.  A driver that instead services
     * INVALIDATE_RANGE through a staging copy pays for the whole mapped
     * length on every map, hence the knob. */
    if (S.inval_range && (access & GL_MAP_WRITE_BIT) &&
        (access & GL_MAP_UNSYNCHRONIZED_BIT)) {
        access |= GL_MAP_INVALIDATE_RANGE_BIT;
    }
    return access;
}

int fsgl_stream_begin_map(GLenum target, GLuint buf, GLintptr offset,
                          GLsizeiptr length, GLbitfield access)
{
    stream_init();
    S.seq++;

    if (!S.trace && !S.fence && !S.early_unmap) {
        return -1;
    }

    S.c_maps++;
    S.c_bytes += (uint64_t)(length > 0 ? length : 0);
    if (length > 0) {
        if (S.c_len_min == 0 || length < S.c_len_min) {
            S.c_len_min = length;
        }
        if (length > S.c_len_max) {
            S.c_len_max = length;
        }
    }
    if (access & GL_MAP_WRITE_BIT) {
        S.c_write++;
    }
    if (access & GL_MAP_READ_BIT) {
        S.c_read++;
    }
    if (access & GL_MAP_UNSYNCHRONIZED_BIT) {
        S.c_unsync++;
    }
    if (access & GL_MAP_FLUSH_EXPLICIT_BIT) {
        S.c_explicit++;
    }
    if (access & GL_MAP_INVALIDATE_RANGE_BIT) {
        S.c_inval_range++;
    }
    if (access & GL_MAP_INVALIDATE_BUFFER_BIT) {
        S.c_inval_buf++;
    }

    BufStat *st = (S.trace || S.fence) ? stat_get(buf, true) : NULL;
    if (st) {
        st->maps++;
        st->bytes += (uint64_t)(length > 0 ? length : 0);
        if (st->prev_off > offset) {
            st->wraps++;
        }
        st->prev_off = offset;
        if (offset + length > st->max_end) {
            st->max_end = offset + length;
        }
    }

    if (map_find(target, buf)) {
        /* GL forbids two live maps on one buffer.  The vendor driver may
         * ignore it and hand out an aliasing pointer. */
        S.c_dbl++;
    }

    if (S.fence && (access & GL_MAP_WRITE_BIT)) {
        /* Poll first.  Only an overlapping unsignalled fence takes the
         * blocking path; never overwrite a live GPU range. */
        if (!fence_region_idle(buf, offset, length)) {
            fence_wait_region(buf, offset, length);
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!S.maps[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    OpenMap *m = &S.maps[slot];
    memset(m, 0, sizeof(*m));
    m->used = true;
    m->target = target;
    m->buf = buf;
    m->off = offset;
    m->len = length;
    m->flags = access;
    m->seq = S.seq;
    m->write = (access & GL_MAP_WRITE_BIT) != 0;
    m->driver_mapped = true;
    if (++S.nmaps > S.maxopen) {
        S.maxopen = S.nmaps;
    }
    S.c_open_sum += (uint64_t)S.nmaps;
    S.c_open_samples++;

    if (S.trace) {
        trace_push('M', target, buf, offset, length, access);
    }
    return slot;
}

void fsgl_stream_mapped(int slot, void *ptr)
{
    if (slot >= 0 && slot < MAX_OPEN) {
        S.maps[slot].ptr = ptr;
    }
}

int fsgl_stream_find_map(GLenum target, GLuint buf)
{
    if (!S.inited) {
        return -1;
    }
    OpenMap *m = map_find(target, buf);
    return m ? map_slot(m) : -1;
}

int fsgl_stream_find_map_recent(GLenum target)
{
    if (!S.inited) {
        return -1;
    }
    int best = -1;
    uint64_t seq = 0;
    for (int i = 0; i < MAX_OPEN; i++) {
        OpenMap *m = &S.maps[i];
        if (m->used && m->target == target && (best < 0 || m->seq > seq)) {
            best = i;
            seq = m->seq;
        }
    }
    return best;
}

bool fsgl_stream_map_driver_mapped(int slot)
{
    return slot >= 0 && slot < MAX_OPEN && S.maps[slot].used &&
           S.maps[slot].driver_mapped;
}

GLuint fsgl_stream_map_buffer(int slot)
{
    if (slot >= 0 && slot < MAX_OPEN && S.maps[slot].used) {
        return S.maps[slot].buf;
    }
    return 0;
}

void fsgl_stream_end_map(int slot, GLboolean ok)
{
    if (slot < 0 || slot >= MAX_OPEN || !S.maps[slot].used) {
        return;
    }
    OpenMap *m = &S.maps[slot];
    S.c_unmaps++;
    if (ok == GL_FALSE) {
        S.c_unmap_false++;
    }
    if (S.trace) {
        trace_push('U', m->target, m->buf, m->off, m->len, 0);
    }
    if (S.fence && m->write) {
        fence_create(m->buf, m->off, m->len);
    }
    m->used = false;
    if (S.nmaps > 0) {
        S.nmaps--;
    }
}

void fsgl_stream_flush(GLenum target, GLuint buf, GLintptr offset, GLsizeiptr length)
{
    if (!S.inited) {
        return;
    }
    S.c_flushes++;
    OpenMap *m = map_find(target, buf);
    if (!m) {
        return;
    }
    m->flushes++;
    if (!(m->flags & GL_MAP_FLUSH_EXPLICIT_BIT)) {
        /* glFlushMappedBufferRange is an error without FLUSH_EXPLICIT and
         * sends Adreno through its relocating stream-buffer path. */
        S.c_flush_noexpl++;
    }
    if (m->draws > 0) {
        S.c_flush_after_draw++;
    }
    if (offset <= 0 && (GLsizeiptr)(offset + length) >= m->len) {
        m->flushed_whole = true;
    } else {
        S.c_flush_partial++;
    }
    if (S.trace) {
        trace_push('F', target, buf, offset, length, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Buffer lifetime                                                     */
/* ------------------------------------------------------------------ */

void fsgl_stream_note_buffer_data(GLenum target, GLuint buf, GLsizeiptr size)
{
    if (!S.inited) {
        return;
    }
    BufStat *st = stat_get(buf, true);
    if (st) {
        st->size = size;
        st->seen = true;
    }
}

void fsgl_stream_drop_buffer(GLuint buf)
{
    if (!S.inited) {
        return;
    }
    for (int i = 0; i < MAX_OPEN; i++) {
        if (S.maps[i].used && S.maps[i].buf == buf) {
            S.maps[i].used = false;
            if (S.nmaps > 0) {
                S.nmaps--;
            }
        }
    }
    /* Fences only exist with fence=1; skip the 8192-entry scan otherwise. */
    for (int i = 0; S.fence && i < FENCE_RING; i++) {
        if (S.fences[i].used && S.fences[i].buf == buf) {
            S.fences[i].used = false;
        }
    }
    if (buf < S.stats_cap && S.stats[buf]) {
        free(S.stats[buf]);
        S.stats[buf] = NULL;
    }
    if (buf < S.vaos_cap && S.vaos[buf]) {
        /* not a VAO id; ignore */
    }
}

/* ------------------------------------------------------------------ */
/* Vertex-array tracking                                               */
/* ------------------------------------------------------------------ */

void fsgl_stream_bind_vao(GLuint vao)
{
    stream_init();
    S.cur_vao = vao;
    VaoRec *r = vao_get(vao);
    if (r) {
        fsgl_buf_bind(GL_ELEMENT_ARRAY_BUFFER, r->ebo);
    }
}

GLuint fsgl_stream_current_vao(void)
{
    return S.cur_vao;
}

void fsgl_stream_element_binding(GLenum target, GLuint buf)
{
    if (target != GL_ELEMENT_ARRAY_BUFFER) {
        return;
    }
    VaoRec *r = vao_get(S.cur_vao);
    if (r) {
        r->ebo = buf;
    }
}

void fsgl_stream_attrib_pointer(GLuint index, GLint size, GLenum type,
                                GLsizei stride, const void *ptr)
{
    if (index >= MAX_ATTR) {
        return;
    }
    VaoRec *r = vao_get(S.cur_vao);
    if (!r) {
        return;
    }
    r->attr[index].buf = fsgl_buf_bound(GL_ARRAY_BUFFER);
    r->attr[index].off = (GLintptr)(uintptr_t)ptr;
    r->attr[index].stride = stride > 0 ? stride : (GLsizei)size;
    r->attr[index].enabled = true;
    (void)type;
}

void fsgl_stream_attrib_enable(GLuint index, bool on)
{
    if (index >= MAX_ATTR) {
        return;
    }
    VaoRec *r = vao_get(S.cur_vao);
    if (r) {
        r->attr[index].enabled = on;
    }
}

/* ------------------------------------------------------------------ */
/* Draws                                                               */
/* ------------------------------------------------------------------ */

/* Finishing a map before the draw that consumes it.
 *
 * Adreno's vendor driver is not safe when a draw dereferences a buffer that
 * is still mapped: the tiler queues the vertex fetch, and unmap afterwards
 * relocates/copies the range, so the queued draw reads whichever copy the
 * driver picked.  That is the flicker.  Desktop GL tolerates the pattern and
 * Factorio is written for it: it maps a ring region, writes one batch,
 * restores the previous buffer binding, draws, and only then unmaps.
 *
 * With a write-only UNSYNCHRONIZED map the pointer is the buffer itself, so
 * the bytes are already in the buffer when the draw arrives: unmapping here
 * cannot lose them, costs one driver call, and makes the draw reference an
 * ordinary unmapped buffer.  The game's later glUnmapBuffer is completed
 * without touching the driver.
 *
 * Read maps are left alone. Element buffers are included when the current
 * VAO says that the draw reads them; index data is just as vulnerable as
 * vertex data on a deferred tiler. */
static void stream_finish_map(OpenMap *m)
{
    PFN_glUnmapBuffer u = (PFN_glUnmapBuffer)fsgl_real_id(FSGL_R_glUnmapBuffer);
    PFN_glBindBuffer b = (PFN_glBindBuffer)fsgl_real_id(FSGL_R_glBindBuffer);
    if (!u || !b) {
        return;
    }
    GLuint cur = fsgl_buf_bound(m->target);
    fsgl_err_begin_extra("stream-finish-map");
    if (cur != m->buf) {
        b(m->target, m->buf);
    }
    u(m->target);
    if (cur != m->buf) {
        b(m->target, cur);
    }
    fsgl_err_end_extra("stream-finish-map");
    m->driver_mapped = false;
    S.c_early_unmap++;
}

static void stream_finish_for_draw(GLuint ebo, const VaoRec *vao)
{
    if (!S.early_unmap || S.nmaps == 0) {
        return;
    }
    for (int i = 0; i < MAX_OPEN; i++) {
        OpenMap *m = &S.maps[i];
        if (!m->used || !m->driver_mapped || !m->write) {
            continue;
        }
        /* Factorio's desktop renderer keeps several maps open while it
         * prepares one batch. Its VAO state is not always observable here
         * (some attribute setup is delegated to the real GLES entry point),
         * so leaving any write map open across a draw is unsafe. All writes
         * preceding this draw are complete by the time the game submits it;
         * finish every open write map rather than guessing from incomplete
         * VAO bookkeeping. */
        bool consumed = true;
        if (!consumed && vao) {
            for (int a = 0; a < MAX_ATTR; a++) {
                if (vao->attr[a].enabled && vao->attr[a].buf == m->buf) {
                    consumed = true;
                    break;
                }
            }
        }
        if (consumed) {
            stream_finish_map(m);
        }
    }
}

static void map_note_draw(OpenMap *m, bool range_hit)
{
    m->draws++;
    if (m->flushes == 0) {
        m->draws_before_flush++;
    }
    BufStat *st = stat_get(m->buf, false);
    if (st) {
        st->draws++;
    }
    S.c_draws_read++;
    if (range_hit) {
        S.c_draws_read_range++;
    }
}

void fsgl_stream_note_draw(const char *kind, GLuint ebo, GLsizei count,
                           const void *indices)
{
    (void)kind;
    if (!S.inited) {
        return;
    }
    S.c_draws++;
    if (S.nmaps == 0) {
        return;
    }
    S.c_draws_mapped++;
    VaoRec *vao = vao_get(S.cur_vao);
    /* Finish only buffers this draw can dereference.  Other concurrently
     * open maps may still be receiving CPU writes for a later draw. */
    stream_finish_for_draw(ebo, vao);
    if (!S.trace) {
        /* Everything below only feeds the periodic report.  After the first
         * draw of a batch the maps stay "open" (finished, awaiting the
         * game's glUnmapBuffer), so this used to run a 16x32 slot scan on
         * every following draw. */
        return;
    }
    bool ebo_hit = false;
    for (int i = 0; i < MAX_OPEN; i++) {
        OpenMap *m = &S.maps[i];
        if (m->used && ebo && m->buf == ebo) {
            bool hit = (m->len <= 0);
            if (!hit) {
                /* The element range is not fully known here; treat any live
                 * mapping of the element buffer as overlapping. */
                hit = true;
            }
            map_note_draw(m, hit);
            ebo_hit = true;
        }
    }
    if (!vao) {
        return;
    }
    bool any_attrib = false;
    for (int a = 0; a < MAX_ATTR; a++) {
        if (!vao->attr[a].enabled || !vao->attr[a].buf) {
            continue;
        }
        any_attrib = true;
        for (int i = 0; i < MAX_OPEN; i++) {
            OpenMap *m = &S.maps[i];
            if (!m->used || m->buf != vao->attr[a].buf) {
                continue;
            }
            bool hit = false;
            if (count > 0 && m->len > 0) {
                GLintptr lo = vao->attr[a].off;
                GLintptr hi = lo + (GLintptr)(count - 1) * vao->attr[a].stride + 1;
                hit = !(hi <= m->off || lo >= m->off + m->len);
            }
            map_note_draw(m, hit && !ebo_hit);
            break;
        }
    }
    if (any_attrib) {
        S.c_draws_with_attribs++;
    }
    if (S.trace && (S.verbose || S.nmaps > 0)) {
        fsgl_stream_trace(ebo ? 'E' : 'D', ebo, (long long)(uintptr_t)indices,
                          count, vao ? (long long)vao->vao : 0);
    }
}

/* ------------------------------------------------------------------ */
/* Periodic report                                                     */
/* ------------------------------------------------------------------ */

void fsgl_stream_report(void)
{
    if (!S.inited || !S.trace) {
        return;
    }
    uint64_t avg_open = S.c_open_samples ? (S.c_open_sum / S.c_open_samples) : 0;
    fsgl_log(ANDROID_LOG_INFO,
             "stream: maps=%llu write=%llu read=%llu unsync=%llu expl=%llu invalr=%llu invalb=%llu dbl=%llu bytes=%llu len[min=%lld max=%lld] open_avg=%llu open_max=%d",
             (unsigned long long)S.c_maps, (unsigned long long)S.c_write,
             (unsigned long long)S.c_read, (unsigned long long)S.c_unsync,
             (unsigned long long)S.c_explicit, (unsigned long long)S.c_inval_range,
             (unsigned long long)S.c_inval_buf, (unsigned long long)S.c_dbl,
             (unsigned long long)S.c_bytes, (long long)S.c_len_min,
             (long long)S.c_len_max, (unsigned long long)avg_open, S.maxopen);
    fsgl_log(ANDROID_LOG_INFO,
             "stream: unmaps=%llu false=%llu flushes=%llu noexpl=%llu after_draw=%llu partial=%llu",
             (unsigned long long)S.c_unmaps, (unsigned long long)S.c_unmap_false,
             (unsigned long long)S.c_flushes, (unsigned long long)S.c_flush_noexpl,
             (unsigned long long)S.c_flush_after_draw,
             (unsigned long long)S.c_flush_partial);
    fsgl_log(ANDROID_LOG_INFO,
             "stream: draws=%llu mapped_any=%llu read_mapped=%llu read_mapped_range=%llu",
             (unsigned long long)S.c_draws, (unsigned long long)S.c_draws_mapped,
             (unsigned long long)S.c_draws_read, (unsigned long long)S.c_draws_read_range);
    fsgl_log(ANDROID_LOG_INFO,
             "stream: fences created=%llu poll=%llu busy=%llu busy_age_max=%llu",
             (unsigned long long)S.c_fence_created, (unsigned long long)S.c_fence_poll,
             (unsigned long long)S.c_fence_busy, (unsigned long long)S.c_fence_busy_age);
    fsgl_log(ANDROID_LOG_INFO,
             "stream: early_unmap enabled=%d done=%llu skipped=%llu draws_with_vao_attribs=%llu",
             S.early_unmap, (unsigned long long)S.c_early_unmap,
             (unsigned long long)S.c_early_skipped,
             (unsigned long long)S.c_draws_with_attribs);

    int shown = 0;
    for (GLuint id = 1; id < S.stats_cap && shown < MAX_ATTRIBS_REPORTED; id++) {
        BufStat *st = S.stats[id];
        if (!st || (!st->maps && !st->size)) {
            continue;
        }
        fsgl_log(ANDROID_LOG_INFO,
                 "stream: buf=%u size=%lld maps=%llu bytes=%llu wraps=%llu maxend=%lld draws=%llu",
                 (unsigned)id, (long long)st->size, (unsigned long long)st->maps,
                 (unsigned long long)st->bytes, (unsigned long long)st->wraps,
                 (long long)st->max_end, (unsigned long long)st->draws);
        st->maps = 0;
        st->bytes = 0;
        st->wraps = 0;
        st->draws = 0;
        st->max_end = 0;
        st->prev_off = 0;
        shown++;
    }

    if (S.trace) {
        /* Last events, oldest first.  Each record is "<kind><buf>:<off>+<len>"
         * (maps/unmaps), "B<buf>/<target>" (bind), "V<vao>", "A<buf>:<off>#<idx>"
         * or "D/E<ebo>:<indices>+<count>#<vao>". */
        int start = (S.trace_head - TRACE_RING + TRACE_RING) % TRACE_RING;
        int total = 0;
        for (int k = 0; k < TRACE_RING; k++) {
            if (S.trace_ring[(start + k) % TRACE_RING].kind) {
                total++;
            }
        }
        int skip = total > 240 ? total - 240 : 0;
        char line[2000];
        size_t n = 0;
        line[0] = 0;
        int emitted = 0;
        for (int k = 0; k < TRACE_RING; k++) {
            TraceRec *t = &S.trace_ring[(start + k) % TRACE_RING];
            if (!t->kind) {
                continue;
            }
            if (emitted++ < skip) {
                continue;
            }
            char one[96];
            int w;
            switch (t->kind) {
            case 'B':
                w = snprintf(one, sizeof(one), "B%u/0x%x ", (unsigned)t->buf,
                             (unsigned)t->off);
                break;
            case 'V':
                w = snprintf(one, sizeof(one), "V%u ", (unsigned)t->buf);
                break;
            case 'A':
                w = snprintf(one, sizeof(one), "A%u:%lld#%lld ", (unsigned)t->buf,
                             (long long)t->off, (long long)t->len);
                break;
            case 'D':
            case 'E':
                w = snprintf(one, sizeof(one), "%c%u:%lld+%lld#%lld ", t->kind,
                             (unsigned)t->buf, (long long)t->off, (long long)t->len,
                             t->arg);
                break;
            default:
                w = snprintf(one, sizeof(one), "%c%u:%lld+%lld ", t->kind,
                             (unsigned)t->buf, (long long)t->off, (long long)t->len);
                break;
            }
            if (w <= 0 || (size_t)w >= sizeof(one) || (size_t)w >= sizeof(line) - n) {
                fsgl_log(ANDROID_LOG_INFO, "stream: tail %s", line);
                n = 0;
                line[0] = 0;
                if (w <= 0 || (size_t)w >= sizeof(one)) {
                    continue;
                }
            }
            memcpy(line + n, one, (size_t)w);
            n += (size_t)w;
            line[n] = 0;
        }
        if (n) {
            fsgl_log(ANDROID_LOG_INFO, "stream: tail %s", line);
        }
        S.trace_head = S.trace_head; /* keep the ring as-is */
    }

    S.c_maps = S.c_write = S.c_read = S.c_unsync = S.c_explicit = 0;
    S.c_inval_range = S.c_inval_buf = S.c_dbl = S.c_bytes = 0;
    S.c_open_sum = S.c_open_samples = 0;
    S.c_len_min = S.c_len_max = 0;
    S.maxopen = S.nmaps;
    S.c_unmaps = S.c_unmap_false = S.c_flushes = 0;
    S.c_flush_noexpl = S.c_flush_after_draw = S.c_flush_partial = 0;
    S.c_draws = S.c_draws_mapped = S.c_draws_read = S.c_draws_read_range = 0;
    S.c_draws_before_flush = 0;
    S.c_fence_created = S.c_fence_poll = S.c_fence_busy = S.c_fence_busy_age = 0;
    S.c_early_unmap = S.c_early_skipped = 0;
    S.c_draws_with_attribs = 0;
}
