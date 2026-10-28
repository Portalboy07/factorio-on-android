/* Render-pass trace with GPU timestamps (FSGL_DIAG builds only).
 *
 * A tiler pays per pass: every switch of the draw framebuffer ends a render
 * pass, and a pass that starts without glClear/glInvalidateFramebuffer has
 * to load the previous contents from memory.  The per-call profiler cannot
 * see that, so every FT_INTERVAL frames one frame is traced:
 *
 *   frametrace: frame 1200 passes=7 draws=412 gpu=24.31ms
 *   frametrace:   #0 fbo=3 vp=2400x1080 clear=0x4000 inval=0 blit=0 draws=180 gpu=11.02ms
 *
 * A pass starts at each change of the bound draw framebuffer; a frame ends
 * when a non-zero framebuffer is bound after drawing to framebuffer 0.  GPU
 * time per pass comes from EXT_disjoint_timer_query timestamps written at
 * each pass boundary and read back (non-blocking) a few frames later.
 */
#include "glcompat.h"

#if FSGL_DIAG

#include <android/log.h>
#include <stdio.h>
#include <string.h>

#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

#define FT_INTERVAL 300
#define FT_MAX_PASSES 48

typedef struct {
    GLuint fbo;
    GLint vp[4];
    GLbitfield clear;
    unsigned inval;
    unsigned blit;
    unsigned draws;
    unsigned instanced;
    unsigned base_vertex;
    GLuint progs[4];
} Pass;

#define FT_MAX_PROGRAMS 1024
/* Fragment shader attached to each program; programs already described. */
static GLuint g_prog_fs[FT_MAX_PROGRAMS];
static bool g_prog_described[FT_MAX_PROGRAMS];

static struct {
    unsigned frame;
    GLuint draw_fbo;
    bool fb0_drawn;          /* framebuffer 0 received draws this frame */
    GLint vp[4];

    bool tracing;
    unsigned traced_frame;
    int npass;
    Pass pass[FT_MAX_PASSES];

    /* GPU timestamps: q[0..npass] = start of each pass + end of frame. */
    bool have_timer;
    bool timer_checked;
    GLuint q[FT_MAX_PASSES + 1];
    int nq;
    bool pending;            /* trace done, timestamps not read yet */
    Pass done[FT_MAX_PASSES];
    int ndone;
    unsigned done_frame;
} T;

typedef void (GL_APIENTRY *PFN_glGenQueries)(GLsizei, GLuint *);

static void ft_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ft_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fsgl_vlog(ANDROID_LOG_INFO, "glcompat-frame", fmt, ap);
    va_end(ap);
}

static void timestamp(void)
{
    if (!T.have_timer || T.nq > FT_MAX_PASSES) {
        return;
    }
    PFN_glQueryCounterEXT counter = (PFN_glQueryCounterEXT)fsgl_real_id(FSGL_R_glQueryCounterEXT);
    fsgl_err_begin_extra("frametrace");
    counter(T.q[T.nq++], GL_TIMESTAMP);
    fsgl_err_end_extra("frametrace");
}

static void timer_init(void)
{
    if (T.timer_checked) {
        return;
    }
    T.timer_checked = true;
    PFN_glGenQueries gen = (PFN_glGenQueries)fsgl_real("glGenQueriesEXT");
    if (!gen) {
        gen = (PFN_glGenQueries)fsgl_real("glGenQueries");
    }
    if (gen && fsgl_real_id(FSGL_R_glQueryCounterEXT) &&
        fsgl_real_id(FSGL_R_glGetQueryObjectui64vEXT) &&
        fsgl_real_id(FSGL_R_glGetQueryObjectivEXT)) {
        fsgl_err_begin_extra("frametrace");
        gen(FT_MAX_PASSES + 1, T.q);
        fsgl_err_end_extra("frametrace");
        T.have_timer = T.q[0] != 0;
    }
    ft_log("GPU timestamps %s", T.have_timer ? "available (EXT_disjoint_timer_query)"
                                              : "unavailable; pass counts only");
}

static void describe_program(GLuint prog);

/* Non-blocking: logs the pending trace once its last timestamp is ready. */
static void poll_pending(void)
{
    if (!T.pending) {
        return;
    }
    double ms[FT_MAX_PASSES];
    double total = -1.0;
    if (T.have_timer && T.nq == T.ndone + 1) {
        PFN_glGetQueryObjectivEXT iv =
            (PFN_glGetQueryObjectivEXT)fsgl_real_id(FSGL_R_glGetQueryObjectivEXT);
        PFN_glGetQueryObjectui64vEXT u64 =
            (PFN_glGetQueryObjectui64vEXT)fsgl_real_id(FSGL_R_glGetQueryObjectui64vEXT);
        PFN_glGetIntegerv gi = (PFN_glGetIntegerv)fsgl_real_id(FSGL_R_glGetIntegerv);
        GLint ready = 0;
        fsgl_err_begin_extra("frametrace");
        iv(T.q[T.nq - 1], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) {
            fsgl_err_end_extra("frametrace");
            return;
        }
        GLint disjoint = 0;
        gi(GL_GPU_DISJOINT_EXT, &disjoint);
        GLuint64 t[FT_MAX_PASSES + 1];
        for (int i = 0; i < T.nq; i++) {
            u64(T.q[i], GL_QUERY_RESULT, &t[i]);
        }
        fsgl_err_end_extra("frametrace");
        if (!disjoint) {
            for (int i = 0; i < T.ndone; i++) {
                ms[i] = (double)(t[i + 1] - t[i]) / 1e6;
            }
            total = (double)(t[T.nq - 1] - t[0]) / 1e6;
        }
    }
    unsigned draws = 0;
    for (int i = 0; i < T.ndone; i++) {
        draws += T.done[i].draws;
    }
    if (total >= 0) {
        ft_log("frame %u passes=%d draws=%u gpu=%.2fms", T.done_frame, T.ndone, draws, total);
    } else {
        ft_log("frame %u passes=%d draws=%u gpu=n/a", T.done_frame, T.ndone, draws);
    }
    for (int i = 0; i < T.ndone; i++) {
        const Pass *p = &T.done[i];
        char gpu[32] = "";
        if (total >= 0) {
            snprintf(gpu, sizeof(gpu), " gpu=%.2fms", ms[i]);
        }
        ft_log("  #%d fbo=%u vp=%dx%d clear=0x%x inval=%u blit=%u draws=%u inst=%u basevtx=%u%s",
               i, p->fbo, p->vp[2], p->vp[3], (unsigned)p->clear, p->inval, p->blit,
               p->draws, p->instanced, p->base_vertex, gpu);
        if (p->progs[0]) {
            ft_log("     progs=%u,%u,%u,%u", p->progs[0], p->progs[1], p->progs[2], p->progs[3]);
        }
    }
    for (int i = 0; i < T.ndone; i++) {
        for (int k = 0; k < 4; k++) {
            describe_program(T.done[i].progs[k]);
        }
    }
    T.pending = false;
}

/* One line per program: the sampler names of its fragment shader, which
 * identify the game's .frag file. */
static void describe_program(GLuint prog)
{
    if (!prog || prog >= FT_MAX_PROGRAMS || g_prog_described[prog]) {
        return;
    }
    g_prog_described[prog] = true;
    const char *src = fsgl_shader_source(g_prog_fs[prog], NULL);
    char names[256] = "";
    size_t used = 0;
    for (const char *p = src; p && (p = strstr(p, "uniform ")) != NULL; p += 8) {
        const char *t = p + 8;
        while (*t == ' ' || *t == 'h' || *t == 'm' || *t == 'l') {
            /* skip precision words: highp/mediump/lowp */
            if (!strncmp(t, "highp ", 6)) t += 6;
            else if (!strncmp(t, "mediump ", 8)) t += 8;
            else if (!strncmp(t, "lowp ", 5)) t += 5;
            else if (*t == ' ') t++;
            else break;
        }
        const char *s = strstr(t, "sampler");
        if (!s || s - t > 2) {
            continue;
        }
        const char *n = strchr(s, ' ');
        if (!n) {
            continue;
        }
        n++;
        size_t len = strcspn(n, " ;[");
        if (used + len + 2 < sizeof(names)) {
            used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%.*s",
                                     used ? "," : "", (int)len, n);
        }
    }
    ft_log("  prog %u: fs=%u samplers=[%s] src_len=%zu", prog, g_prog_fs[prog], names,
           src ? strlen(src) : (size_t)0);
}

static void begin_pass(void)
{
    if (!T.tracing || T.npass >= FT_MAX_PASSES) {
        return;
    }
    Pass *p = &T.pass[T.npass++];
    memset(p, 0, sizeof(*p));
    p->fbo = T.draw_fbo;
    memcpy(p->vp, T.vp, sizeof(p->vp));
    timestamp();
}

static void end_frame(void)
{
    if (T.tracing) {
        timestamp();
        memcpy(T.done, T.pass, sizeof(Pass) * (size_t)T.npass);
        T.ndone = T.npass;
        T.done_frame = T.traced_frame;
        T.pending = true;
        T.tracing = false;
    }
    T.frame++;
    poll_pending();
    if (!T.pending && T.frame % FT_INTERVAL == 0) {
        timer_init();
        T.tracing = true;
        T.traced_frame = T.frame;
        T.npass = 0;
        T.nq = 0;
        begin_pass();
    }
}

static Pass *cur(void)
{
    return T.tracing && T.npass > 0 ? &T.pass[T.npass - 1] : NULL;
}

/* ------------------------------------------------------------------ */
/* Wrappers (timing-transparent pass-throughs)                         */
/* ------------------------------------------------------------------ */

typedef void (GL_APIENTRY *PFN_glClear)(GLbitfield);
typedef void (GL_APIENTRY *PFN_glClearBufferfv)(GLenum, GLint, const GLfloat *);
typedef void (GL_APIENTRY *PFN_glClearBufferfi)(GLenum, GLint, GLfloat, GLint);
typedef void (GL_APIENTRY *PFN_glInvalidateFramebuffer)(GLenum, GLsizei, const GLenum *);
typedef void (GL_APIENTRY *PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                                  GLint, GLint, GLbitfield, GLenum);
typedef void (GL_APIENTRY *PFN_glDrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
typedef void (GL_APIENTRY *PFN_glDrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLint);
typedef void (GL_APIENTRY *PFN_glDrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);

#define REAL(type, name) ((type)fsgl_real_id(FSGL_R_##name))

void GL_APIENTRY fsgl_ft_glBindFramebuffer(GLenum target, GLuint fbo)
{
    static int inval = -1;
    if (inval < 0) {
        inval = fsgl_config_invalidate_ds() ? 1 : 0;
    }
    if (inval && T.draw_fbo != 0 && fbo != T.draw_fbo &&
        (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)) {
        static const GLenum ds[] = { GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
        fsgl_err_begin_extra("invalidate-ds");
        REAL(PFN_glInvalidateFramebuffer, glInvalidateFramebuffer)(GL_DRAW_FRAMEBUFFER, 2, ds);
        fsgl_err_end_extra("invalidate-ds");
    }
    REAL(PFN_glBindFramebuffer, glBindFramebuffer)(target, fbo);
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER) {
        return;
    }
    if (fbo == T.draw_fbo) {
        return;
    }
    if (fbo != 0 && T.fb0_drawn) {
        T.fb0_drawn = false;
        T.draw_fbo = fbo;
        end_frame();
        return;
    }
    T.draw_fbo = fbo;
    begin_pass();
}

void GL_APIENTRY fsgl_ft_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    REAL(PFN_glViewport, glViewport)(x, y, w, h);
    T.vp[0] = x; T.vp[1] = y; T.vp[2] = w; T.vp[3] = h;
    Pass *p = cur();
    if (p && p->draws == 0) {
        memcpy(p->vp, T.vp, sizeof(p->vp));
    }
}

void GL_APIENTRY fsgl_ft_glClear(GLbitfield mask)
{
    REAL(PFN_glClear, glClear)(mask);
    Pass *p = cur();
    if (p) {
        p->clear |= mask;
    }
}

void GL_APIENTRY fsgl_ft_glClearBufferfv(GLenum buffer, GLint drawbuffer, const GLfloat *value)
{
    REAL(PFN_glClearBufferfv, glClearBufferfv)(buffer, drawbuffer, value);
    Pass *p = cur();
    if (p) {
        p->clear |= buffer == GL_COLOR ? GL_COLOR_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
    }
}

void GL_APIENTRY fsgl_ft_glClearBufferfi(GLenum buffer, GLint drawbuffer, GLfloat depth, GLint stencil)
{
    REAL(PFN_glClearBufferfi, glClearBufferfi)(buffer, drawbuffer, depth, stencil);
    Pass *p = cur();
    if (p) {
        p->clear |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    }
}

void GL_APIENTRY fsgl_ft_glInvalidateFramebuffer(GLenum target, GLsizei n, const GLenum *att)
{
    REAL(PFN_glInvalidateFramebuffer, glInvalidateFramebuffer)(target, n, att);
    Pass *p = cur();
    if (p) {
        p->inval++;
    }
}

void GL_APIENTRY fsgl_ft_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1,
                                           GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                           GLbitfield mask, GLenum filter)
{
    REAL(PFN_glBlitFramebuffer, glBlitFramebuffer)(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                                                  mask, filter);
    Pass *p = cur();
    if (p) {
        p->blit++;
    }
}

void fsgl_ft_draw(bool instanced, bool base_vertex)
{
    if (T.draw_fbo == 0) {
        T.fb0_drawn = true;
    }
    Pass *p = cur();
    if (p) {
        p->draws++;
        p->instanced += instanced;
        p->base_vertex += base_vertex;
        GLuint prog = fsgl_current_program();
        for (int k = 0; k < 4; k++) {
            if (p->progs[k] == prog) {
                break;
            }
            if (!p->progs[k]) {
                p->progs[k] = prog;
                break;
            }
        }
    }
}

void GL_APIENTRY fsgl_ft_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei n)
{
    fsgl_ft_draw(true, false);
    REAL(PFN_glDrawArraysInstanced, glDrawArraysInstanced)(mode, first, count, n);
}

void GL_APIENTRY fsgl_ft_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                                 const void *indices, GLsizei n)
{
    fsgl_ft_draw(true, false);
    REAL(PFN_glDrawElementsInstanced, glDrawElementsInstanced)(mode, count, type, indices, n);
}

void GL_APIENTRY fsgl_ft_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                                  const void *indices, GLint base)
{
    fsgl_ft_draw(false, true);
    REAL(PFN_glDrawElementsBaseVertex, glDrawElementsBaseVertex)(mode, count, type, indices, base);
}

void GL_APIENTRY fsgl_ft_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                                             GLenum type, const void *indices)
{
    fsgl_ft_draw(false, false);
    REAL(PFN_glDrawRangeElements, glDrawRangeElements)(mode, start, end, count, type, indices);
}

typedef void (GL_APIENTRY *PFN_glAttachShader)(GLuint, GLuint);

void GL_APIENTRY fsgl_ft_glAttachShader(GLuint program, GLuint shader)
{
    REAL(PFN_glAttachShader, glAttachShader)(program, shader);
    GLenum stage = 0;
    fsgl_shader_source(shader, &stage);
    if (program < FT_MAX_PROGRAMS && stage == GL_FRAGMENT_SHADER) {
        g_prog_fs[program] = shader;
    }
}

#endif /* FSGL_DIAG */
