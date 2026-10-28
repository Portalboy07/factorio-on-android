/* GL error attribution and hygiene for libGLCompat.
 *
 * Two mechanisms:
 *
 * 1. A global ring of the last 8 intercepted GL calls (name + 4 integer
 *    arguments, no formatting), with an atomic write index.  Factorio issues
 *    all GL calls from the single render thread (the context is only current
 *    there), so the ring is deliberately not TLS: emulated TLS costs a
 *    pthread_getspecific per access.  Concurrent writers are race-tolerant
 *    (mixed entry at worst, no allocation, no crash).  When the game calls
 *    glGetError we log that ring once per distinct caller return address, at
 *    WARN, tag "glcompat-err".
 *
 * 2. After each forwarded driver call, fsgl_err_check() consumes the error
 *    (so it can be logged with the call's full parameters), queues it in a
 *    global virtual error queue, and glGetError returns it later.  The
 *    caller therefore still observes exactly the error its own call produced,
 *    even though the driver's sticky error flag was read by us.  Logging is
 *    rate limited to once per unique (name,error) pair per 2 s.
 *
 * Layer-internal calls (BGRA conversion state, compressed decode, FBO
 * readback, buffer-map fallback, query fallback) are bracketed by
 * fsgl_err_begin_extra()/fsgl_err_end_extra(): errors that existed before the
 * wrapper are preserved for the caller, errors produced by our own extra
 * calls are drained and logged at DEBUG, never returned.
 */
#include "glcompat.h"

#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ERR_TAG "glcompat-err"
#define RING_LEN 8
#define ERRQ_LEN 32

void fsgl_err_log(int prio, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fsgl_vlog(prio, ERR_TAG, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Per-thread call ring                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uintptr_t a[4];
} RingEntry;

/* Factorio issues all GL calls from one render thread, and the layer's
 * texture/buffer/program state is already global for the same reason.  Keep
 * the ring and the virtual error queue global too: no TLS, no emutls, no
 * loader-version dependency, and one fewer indirection per intercepted call.
 * The index is atomic and the entry fields are plain aligned stores, so a
 * concurrent writer can at worst produce a mixed entry; it can never crash or
 * allocate.  The GL context is only current on the render thread. */
static RingEntry t_ring[RING_LEN];
static unsigned t_ring_pos;

#if FSGL_DIAG
void fsgl_ring_push(const char *name, uintptr_t a0, uintptr_t a1,
                    uintptr_t a2, uintptr_t a3)
{
    unsigned pos = __atomic_fetch_add(&t_ring_pos, 1, __ATOMIC_RELAXED);
    RingEntry *e = &t_ring[pos & (RING_LEN - 1)];
    e->a[0] = a0;
    e->a[1] = a1;
    e->a[2] = a2;
    e->a[3] = a3;
    __atomic_store_n(&e->name, name, __ATOMIC_RELEASE);
}
#endif

/* ------------------------------------------------------------------ */
/* Virtual error queue (single GL thread; see the ring comment above)  */
/* ------------------------------------------------------------------ */

static GLenum t_q[ERRQ_LEN];
static unsigned t_qh, t_qt;
static bool t_q_overflow_logged;

static void q_push(GLenum err)
{
    for (;;) {
        unsigned tail = __atomic_load_n(&t_qt, __ATOMIC_RELAXED);
        unsigned head = __atomic_load_n(&t_qh, __ATOMIC_ACQUIRE);
        if (tail - head >= ERRQ_LEN) {
            if (!__atomic_exchange_n(&t_q_overflow_logged, true, __ATOMIC_RELAXED)) {
                fsgl_err_log(ANDROID_LOG_WARN,
                             "error queue full (%d); further errors are left in the driver flag",
                             ERRQ_LEN);
            }
            return;
        }
        if (__atomic_compare_exchange_n(&t_qt, &tail, tail + 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            t_q[tail & (ERRQ_LEN - 1)] = err;
            return;
        }
    }
}

static bool q_pop(GLenum *out)
{
    for (;;) {
        unsigned head = __atomic_load_n(&t_qh, __ATOMIC_RELAXED);
        unsigned tail = __atomic_load_n(&t_qt, __ATOMIC_ACQUIRE);
        if (head == tail) {
            return false;
        }
        if (__atomic_compare_exchange_n(&t_qh, &head, head + 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            *out = t_q[head & (ERRQ_LEN - 1)];
            return true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Error names                                                         */
/* ------------------------------------------------------------------ */

static const char *err_name(GLenum err)
{
    switch (err) {
    case GL_INVALID_ENUM: return "INVALID_ENUM";
    case GL_INVALID_VALUE: return "INVALID_VALUE";
    case GL_INVALID_OPERATION: return "INVALID_OPERATION";
    case GL_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case GL_STACK_UNDERFLOW: return "STACK_UNDERFLOW";
    case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION";
#ifdef GL_CONTEXT_LOST
    case GL_CONTEXT_LOST: return "CONTEXT_LOST";
#endif
    default: return "GL_ERROR";
    }
}

static PFN_glGetError real_get_error(void)
{
    return (PFN_glGetError)fsgl_real_id(FSGL_R_glGetError);
}

/* ------------------------------------------------------------------ */
/* Per-call error logging, rate limited per (name,error) per 2 s       */
/* ------------------------------------------------------------------ */

#define ERR_RATE_SLOTS 64
#define ERR_RATE_NS 2000000000ull

typedef struct {
    const char *name;
    GLenum err;
    uint64_t last_ns;
} ErrRateSlot;

static ErrRateSlot g_err_rate[ERR_RATE_SLOTS];
static unsigned g_err_rate_next;

static bool err_rate_allow(const char *name, GLenum err, uint64_t now)
{
    for (int i = 0; i < ERR_RATE_SLOTS; i++) {
        ErrRateSlot *s = &g_err_rate[i];
        if (s->name == name && s->err == err) {
            if (now - s->last_ns < ERR_RATE_NS) {
                return false;
            }
            s->last_ns = now;
            return true;
        }
    }
    ErrRateSlot *s = &g_err_rate[g_err_rate_next++ % ERR_RATE_SLOTS];
    s->name = name;
    s->err = err;
    s->last_ns = now;
    return true;
}

#if FSGL_DIAG
void fsgl_err_check(const char *name, const char *params_fmt, ...)
{
    PFN_glGetError geterr = real_get_error();
    if (!geterr) {
        return;
    }
    GLenum err = geterr();
    if (err == GL_NO_ERROR) {
        return;
    }
    q_push(err);

    uint64_t now = fsgl_now_ns();
    if (!err_rate_allow(name, err, now)) {
        return;
    }
    char params[512];
    va_list ap;
    va_start(ap, params_fmt);
    vsnprintf(params, sizeof(params), params_fmt, ap);
    va_end(ap);
    fsgl_err_log(ANDROID_LOG_WARN, "%s (0x%x) after %s(%s)",
                 err_name(err), (unsigned)err, name, params);
}
#endif

/* ------------------------------------------------------------------ */
/* Layer-internal call bracketing                                      */
/* ------------------------------------------------------------------ */

static void drain_internal(const char *name, bool preserve)
{
    PFN_glGetError geterr = real_get_error();
    if (!geterr) {
        return;
    }
    for (int i = 0; i < 8; i++) {
        GLenum err = geterr();
        if (err == GL_NO_ERROR) {
            return;
        }
        /* Every line is a syscall into logd; an error that recurs each draw
         * must not turn into a log storm on the render thread. */
        bool log = err_rate_allow(name, err, fsgl_now_ns());
        if (preserve) {
            /* An error from an earlier caller call, not from us: keep it
             * visible to the caller's next glGetError. */
            q_push(err);
            if (log) {
                fsgl_err_log(ANDROID_LOG_DEBUG,
                             "pre-existing %s (0x%x) before %s; preserved for caller",
                             err_name(err), (unsigned)err, name);
            }
        } else if (log) {
            fsgl_err_log(ANDROID_LOG_DEBUG,
                         "internal %s (0x%x) from %s's layer-internal GL calls; drained",
                         err_name(err), (unsigned)err, name);
        }
    }
}

void fsgl_err_begin_extra(const char *name)
{
    drain_internal(name, true);
}

void fsgl_err_end_extra(const char *name)
{
    drain_internal(name, false);
}

/* ------------------------------------------------------------------ */
/* glGetError wrapper: ring attribution, once per caller site          */
/* ------------------------------------------------------------------ */

#define SITE_SLOTS 64
#define SITE_BUCKET_NS 1000000000ull
#define SITE_BUCKET_MAX 5

typedef struct {
    uintptr_t site;
    GLenum err;
} SiteSlot;

static SiteSlot g_sites[SITE_SLOTS];
static unsigned g_site_next;
static uint64_t g_site_window_ns;
static unsigned g_site_window_count;

static bool site_allow(uintptr_t site, GLenum err, uint64_t now)
{
    for (int i = 0; i < SITE_SLOTS; i++) {
        if (g_sites[i].site == site && g_sites[i].err == err) {
            return false; /* already logged once for this site+error */
        }
    }
    if (now - g_site_window_ns >= SITE_BUCKET_NS) {
        g_site_window_ns = now;
        g_site_window_count = 0;
    }
    if (g_site_window_count >= SITE_BUCKET_MAX) {
        return false; /* rate limited; allow a later window */
    }
    SiteSlot *s = &g_sites[g_site_next++ % SITE_SLOTS];
    s->site = site;
    s->err = err;
    g_site_window_count++;
    return true;
}

static void log_ring(GLenum err, uintptr_t site)
{
    char buf[1024];
    size_t used = 0;
    used += (size_t)snprintf(buf + used, sizeof(buf) - used,
                             "glGetError=%s (0x%x) at caller 0x%llx; last intercepted calls (newest first):",
                             err_name(err), (unsigned)err, (unsigned long long)site);
    unsigned pos = __atomic_load_n(&t_ring_pos, __ATOMIC_ACQUIRE);
    /* Release builds never fill the ring; the loop then ends at once. */
    for (int i = 0; i < RING_LEN && used < sizeof(buf) - 64; i++) {
        unsigned idx = (pos - 1 - (unsigned)i) & (RING_LEN - 1);
        const RingEntry *e = &t_ring[idx];
        const char *nm = __atomic_load_n(&e->name, __ATOMIC_ACQUIRE);
        if (!nm) {
            break;
        }
        used += (size_t)snprintf(buf + used, sizeof(buf) - used,
                                 " %s(0x%llx,0x%llx,0x%llx,0x%llx)",
                                 nm,
                                 (unsigned long long)e->a[0],
                                 (unsigned long long)e->a[1],
                                 (unsigned long long)e->a[2],
                                 (unsigned long long)e->a[3]);
    }
    fsgl_err_log(ANDROID_LOG_WARN, "%s", buf);
}

GLenum fsgl_err_wrap_get_error(uintptr_t caller)
{
    GLenum err;
    if (!q_pop(&err)) {
        PFN_glGetError geterr = real_get_error();
        err = geterr ? geterr() : GL_NO_ERROR;
    }
    if (err != GL_NO_ERROR) {
        uint64_t now = fsgl_now_ns();
        if (site_allow(caller, err, now)) {
            log_ring(err, caller);
        }
    }
    return err;
}
