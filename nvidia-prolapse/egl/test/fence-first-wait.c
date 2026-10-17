/* fence-first-wait: NVIDIA EGL_ANDROID_native_fence_sync defect D1 (FENCE-CONTRACT.md §3.3, §3.5), and the proof that
 * our front (libEGL_nvnative.so) enforces invariant I (§6a) for every sequence.
 *
 * Contract (EGL_ANDROID_native_fence_sync.txt:135-140, = EGL_KHR_fence_sync.txt:236-242): a native fence sync
 * created with no fd is signalled by "completion of the fence command corresponding to the sync object and all
 * preceding commands in the associated client API context's command stream", and "will not be signaled until all
 * effects from these commands on the client API's internal and framebuffer state are fully realized".
 *
 * Two ways to reach the driver, chosen at run time (nothing is linked):
 *   default        stock libglvnd: dlopen("libEGL.so.1") + dlopen("libGLESv2.so.2"), every name by dlsym/eglGetProcAddress
 *                  there. Nothing of ours is involved; this is the driver bug reproducer (`make driver-check`).
 *   --front LIB    our front, the way Android's loader uses it: dlopen(LIB), dlsym(name), else LIB's eglGetProcAddress.
 *                  With --platform-lib PLAT the display is the Android platform display (EGL_PLATFORM_ANDROID_KHR,
 *                  EGL_DEFAULT_DISPLAY) served by PLAT, as an Android app/SurfaceFlinger gets it; otherwise the NVIDIA
 *                  device display (EGL_PLATFORM_DEVICE_EXT). `make front-fence-check` runs every mode this way.
 *
 * Base sequence, per round (each round uses a NEW THREAD unless the mode says otherwise):
 *   new thread T, context B: make current, build a slow full-screen draw; report ready
 *   main thread, context A:  clear (with --slow-a/--a-iters also a slow draw) + native fence Fa (create, glFlush, dup)
 *   thread T:                import Fa as EGL_SYNC_NATIVE_FENCE_ANDROID + eglWaitSync   <- T's first fence op
 *                            the slow draw into a renderbuffer (~4.7 ms on an RTX 5090 with --iters 200000)
 *                            native fence Fr (create, glFlush, dup); wait for Fr; then glFinish
 * A correct Fr signals no earlier than the draw it covers. The verdict uses GPU timestamps (GL_EXT_disjoint_timer_query,
 * mapped to CLOCK_MONOTONIC through GL_TIMESTAMP_EXT): a round is early when the draw ended more than 0.1 ms after
 * poll() saw Fr signalled. The CPU-only criterion of the first version (glFinish after Fr still waits > 2 ms; an early
 * Fr leaves the whole draw to glFinish, 4.6-6.3 ms) remains with --no-timestamps; it is only valid on an otherwise
 * idle GPU. On 615.71.09 through stock glvnd Fr signals together with Fa. --mode selects variants of what T
 * does before or around the sequence (see usage()). In every mode the imported sync is destroyed right after
 * eglWaitSync, as Android's RenderEngine does.
 *
 *   cc -O1 -o fence-first-wait fence-first-wait.c -ldl -lpthread
 *   ./fence-first-wait [--front LIB [--platform-lib PLAT]] [--rounds N] [--iters N] [--slow-a] [--a-iters N] [--mode M]
 * Exit status: number of rounds whose fence (or, for "migrate", whose read) saw the work incomplete (100: setup). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define DIE(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); fprintf(stderr, "\n"); exit(100); } while (0)

/* every EGL/GL entry point, resolved at run time from stock glvnd or from our front */
#define EGLFN(X) \
    X(PFNEGLGETPROCADDRESSPROC, eglGetProcAddress) X(PFNEGLINITIALIZEPROC, eglInitialize) \
    X(PFNEGLTERMINATEPROC, eglTerminate) X(PFNEGLQUERYSTRINGPROC, eglQueryString) X(PFNEGLBINDAPIPROC, eglBindAPI) \
    X(PFNEGLCREATECONTEXTPROC, eglCreateContext) X(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext) \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLRELEASETHREADPROC, eglReleaseThread) \
    X(PFNEGLGETERRORPROC, eglGetError) X(PFNEGLCHOOSECONFIGPROC, eglChooseConfig) \
    X(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface) X(PFNEGLDESTROYSURFACEPROC, eglDestroySurface) \
    X(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay)
#define GLFN(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLFLUSHPROC, glFlush) X(PFNGLFINISHPROC, glFinish) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARPROC, glClear) X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLGETINTEGER64VPROC, glGetInteger64v) X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGENQUERIESEXTPROC, glGenQueriesEXT) X(PFNGLDELETEQUERIESEXTPROC, glDeleteQueriesEXT) \
    X(PFNGLQUERYCOUNTEREXTPROC, glQueryCounterEXT) X(PFNGLGETQUERYOBJECTUI64VEXTPROC, glGetQueryObjectui64vEXT)
#define DECL(t, n) static t n;
EGLFN(DECL)
GLFN(DECL)
#undef DECL
static PFNEGLQUERYDEVICESEXTPROC queryDevices;
static PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayEXT;
static PFNEGLCREATESYNCKHRPROC createSync;
static PFNEGLDESTROYSYNCKHRPROC destroySync;
static PFNEGLWAITSYNCKHRPROC waitSync;
static PFNEGLCLIENTWAITSYNCKHRPROC clientWaitSync;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence;

static EGLDisplay dpy, dpy2; /* dpy2: a second display of the same device (second-display, terminate-* modes) */
static EGLContext ctxA;
static int iters = 200000, a_iters = -1, fa, slow_a, use_front;
static const char *mode = "first-wait";
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static const struct { const char *name, *what; } MODES[] = {
    {"first-wait", "T: wait(Fa); draw; Fr                                    (the defect)"},
    {"warm", "T: native fence (create+flush+dup); wait(Fa); draw; Fr"},
    {"warm-nodup", "T: eglCreateSync(NATIVE)+eglDestroySync, no flush/dup; wait(Fa); draw; Fr"},
    {"warm-after-wait", "T: wait(Fa); eglCreateSync(NATIVE)+eglDestroySync; draw; Fr"},
    {"same-thread", "main thread (already used by A): wait(Fa); draw; Fr in a new context B"},
    {"switch-ctx", "T: makecurrent W, then makecurrent B (switch); wait(Fa); draw; Fr"},
    {"switch-after-wait", "T: W: wait(Fa); glFlush; makecurrent B (switch); draw; Fr"},
    {"rebind", "T: makecurrent B; makecurrent NONE; makecurrent B; wait(Fa); draw; Fr"},
    {"release-thread", "T: warm native fence; eglReleaseThread; makecurrent B; wait(Fa); draw; Fr"},
    {"main-warm-ctx", "B warmed (native fence) in main; T: makecurrent B; wait(Fa); draw; Fr"},
    {"two-waits", "T: wait(dup Fa); wait(Fa); draw; Fr"},
    {"client-wait-first", "T: eglClientWaitSync(import Fa, timeout 0) [--a-iters 4000000: times out]; wait(Fa); draw; Fr"},
    {"khr-warm", "T: EGL_SYNC_FENCE_KHR create+flush+client-wait+destroy; wait(Fa); draw; Fr"},
    {"khr-fence", "T: wait(Fa); draw; Fr = EGL_SYNC_FENCE_KHR (non-native), status polled at creation"},
    {"khr-fence-warm", "T: warm native fence; wait(Fa); draw; Fr = EGL_SYNC_FENCE_KHR"},
    {"migrate", "T1: B: wait(Fa); draw; makecurrent NONE.  T2: makecurrent B; glReadPixels (no user fence)"},
    {"migrate-warm", "as migrate, but T1 creates one native fence before its wait"},
    /* which entry points create a timeline state (FENCE-CONTRACT §3.5, task D1 measurements) */
    {"khr-warm-nodup", "T: eglCreateSync(EGL_SYNC_FENCE_KHR)+eglDestroySync only (no flush/wait); wait(Fa); draw; Fr"},
    {"pbuffer-first", "T: context WITH a config + pbuffer surface: wait(Fa); draw; Fr"},
    {"warm-then-pbuffer", "T: no-config surfaceless W: warm native fence; makecurrent config+pbuffer B; wait(Fa); draw; Fr"},
    {"second-display", "T: W on display 1: warm native fence; makecurrent B on display 2; wait(Fa); draw; Fr"},
    {"terminate-reinit", "T on display 2: W warm; NONE; eglTerminate+eglInitialize(display 2); new B; wait(Fa); draw; Fr"},
    {"reinit-noterm", "T on display 2: W warm; NONE; (no terminate) new B; wait(Fa); draw; Fr   (control)"},
};
static int is(const char *m) { return !strcmp(mode, m); }

static int native_fence(void) {
    EGLSyncKHR s = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (s == EGL_NO_SYNC_KHR) DIE("eglCreateSyncKHR(NATIVE_FENCE) 0x%x", eglGetError());
    glFlush();
    int fd = dupFence(dpy, s);
    destroySync(dpy, s);
    if (fd < 0) DIE("eglDupNativeFenceFDANDROID 0x%x", eglGetError());
    return fd;
}
static int native_fence_on(EGLDisplay d) {
    EGLSyncKHR s = createSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (s == EGL_NO_SYNC_KHR) DIE("eglCreateSyncKHR(NATIVE_FENCE) 0x%x", eglGetError());
    glFlush();
    int fd = dupFence(d, s);
    destroySync(d, s);
    if (fd < 0) DIE("eglDupNativeFenceFDANDROID 0x%x", eglGetError());
    return fd;
}
static void native_sync_nodup(void) { /* create + destroy only: no flush, no fd */
    EGLSyncKHR s = createSync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (s == EGL_NO_SYNC_KHR) DIE("eglCreateSyncKHR(NATIVE_FENCE) 0x%x", eglGetError());
    destroySync(dpy, s);
}
/* the display T's context lives on (all waits/fences of T use it) */
static __thread EGLDisplay tdpy;
static EGLSyncKHR import(int fd) {
    EGLint a[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSyncKHR s = createSync(tdpy ? tdpy : dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    if (s == EGL_NO_SYNC_KHR) DIE("import 0x%x", eglGetError());
    return s;
}
static void gpu_wait(int fd) {
    EGLDisplay d = tdpy ? tdpy : dpy;
    EGLSyncKHR s = import(fd);
    if (!waitSync(d, s, 0)) DIE("eglWaitSyncKHR 0x%x", eglGetError());
    destroySync(d, s);
}
static GLuint compile(GLenum t, const char *src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) DIE("shader");
    return s;
}
/* per-context draw objects: a slow magenta full-screen draw into a 256x128 renderbuffer */
struct target { GLuint prog, fb, vao; };
static void setup_n(struct target *t, int n) {
    char fs[512];
    snprintf(fs, sizeof(fs), "#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){float a=gl_FragCoord.x;"
             "for(int i=0;i<%d;i++){a=fract(sin(a*12.9898+float(i))*43758.5453);}o=vec4(1.0,0.0,1.0,1.0)+vec4(a*1e-9);}\n",
             n);
    t->prog = glCreateProgram();
    glAttachShader(t->prog, compile(GL_VERTEX_SHADER, "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),"
                                                      "float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n"));
    glAttachShader(t->prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(t->prog);
    GLuint rb;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 256, 128);
    glGenFramebuffers(1, &t->fb);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glGenVertexArrays(1, &t->vao);
    glBindVertexArray(t->vao);
    glUseProgram(t->prog);
    glViewport(0, 0, 256, 128);
}
static void setup(struct target *t) { setup_n(t, iters); }
static void draw(const struct target *t) {
    glBindFramebuffer(GL_FRAMEBUFFER, t->fb);
    glBindVertexArray(t->vao);
    glUseProgram(t->prog);
    glViewport(0, 0, 256, 128);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
static EGLContext new_ctx_on(EGLDisplay d) {
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (!c) DIE("eglCreateContext 0x%x", eglGetError());
    return c;
}
static EGLContext new_ctx(void) { return new_ctx_on(dpy); }
static void make_current_on(EGLDisplay d, EGLContext c) {
    if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) DIE("eglMakeCurrent 0x%x", eglGetError());
}
static void make_current(EGLContext c) { make_current_on(dpy, c); }

struct result { int signalled_at_creation; double t_sig, t_finish, end_after_sig; int bad_px; const char *note; };
static int use_ts = 1; /* GL_EXT_disjoint_timer_query: when did the covered draw really end (--no-timestamps: off) */

/* GPU time of the end of everything submitted before the query, mapped to CLOCK_MONOTONIC ms with GL_TIMESTAMP_EXT */
static double ts_end_ms(GLuint q) {
    GLuint64 g_end = 0;
    glGetQueryObjectui64vEXT(q, GL_QUERY_RESULT_EXT, &g_end);
    GLint64 g_now = 0;
    double c0 = now_ms();
    glGetInteger64v(GL_TIMESTAMP_EXT, &g_now);
    double c1 = now_ms();
    return (c0 + c1) / 2 - (double)(g_now - (GLint64)g_end) / 1e6;
}

/* Fr as a native fence (default) or as an EGL_SYNC_FENCE_KHR (khr-*). Records whether Fr was signalled at
 * creation, when it signalled (t_sig) and when glFinish returned (t_finish), both from Fr's creation. A correct
 * fence signals no earlier than the work it covers, so glFinish right after it returns at once. */
static void check_fence(struct result *r) {
    EGLDisplay d = tdpy ? tdpy : dpy;
    GLuint tq = 0;
    if (use_ts) { /* written when the draw (everything before it) has completed */
        glGenQueriesEXT(1, &tq);
        glQueryCounterEXT(tq, GL_TIMESTAMP_EXT);
    }
    double t0 = now_ms(), sig_abs;
    if (is("khr-fence") || is("khr-fence-warm")) {
        EGLSyncKHR s = createSync(d, EGL_SYNC_FENCE_KHR, NULL);
        if (s == EGL_NO_SYNC_KHR) DIE("eglCreateSyncKHR(FENCE) 0x%x", eglGetError());
        glFlush();
        r->signalled_at_creation = clientWaitSync(d, s, 0, 0) == EGL_CONDITION_SATISFIED_KHR;
        if (clientWaitSync(d, s, 0, 5000000000ull) != EGL_CONDITION_SATISFIED_KHR) DIE("Fr (KHR) did not signal in 5 s");
        sig_abs = now_ms();
        destroySync(d, s);
    } else {
        int fr = native_fence_on(d);
        struct pollfd q = {.fd = fr, .events = POLLIN};
        r->signalled_at_creation = poll(&q, 1, 0) == 1;
        if (poll(&q, 1, 5000) != 1) DIE("Fr did not signal in 5 s");
        sig_abs = now_ms();
        close(fr);
    }
    r->t_sig = sig_abs - t0;
    glFinish();
    r->t_finish = now_ms() - t0;
    if (use_ts) {
        r->end_after_sig = ts_end_ms(tq) - sig_abs;
        glDeleteQueriesEXT(1, &tq);
    }
}

/* Hand-off: the worker threads finish everything that does not involve Fa (make-current, setup, warm-ups) and
 * report ready; only then does the main thread submit A's work and create Fa, so Fa is still pending when the
 * workers use it. (The first eglMakeCurrent in a new thread blocks until previously submitted GPU work has
 * completed, measured 890 ms with --a-iters 40000000; without this hand-off Fa would always be signalled.) */
static pthread_mutex_t hs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hs_cv = PTHREAD_COND_INITIALIZER;
static int hs_ready, hs_fa_posted, hs_inline;
static void await_fa(void) {
    if (hs_inline) return; /* same-thread: Fa exists already */
    pthread_mutex_lock(&hs_mu);
    hs_ready++;
    pthread_cond_broadcast(&hs_cv);
    while (!hs_fa_posted) pthread_cond_wait(&hs_cv, &hs_mu);
    pthread_mutex_unlock(&hs_mu);
}

static EGLContext ctx_warm_main; /* main-warm-ctx */
static EGLContext mig_ctx;       /* migrate: the context that moves from T1 to T2 */
static struct target mig_t;
static pthread_mutex_t mig_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mig_cv = PTHREAD_COND_INITIALIZER;
static int mig_stage;
static void mig_set(int s) { pthread_mutex_lock(&mig_mu); mig_stage = s; pthread_cond_broadcast(&mig_cv); pthread_mutex_unlock(&mig_mu); }
static void mig_wait(int s) { pthread_mutex_lock(&mig_mu); while (mig_stage < s) pthread_cond_wait(&mig_cv, &mig_mu); pthread_mutex_unlock(&mig_mu); }

static void *migrate_t1(void *arg) {
    (void)arg;
    mig_ctx = new_ctx();
    make_current(mig_ctx);
    setup(&mig_t);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (is("migrate-warm")) close(native_fence());
    await_fa();
    gpu_wait(fa);
    draw(&mig_t);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); /* unbind: the context may now move */
    mig_set(1);
    mig_wait(2); /* stay alive (thread state kept) until T2 has read */
    eglReleaseThread();
    return NULL;
}
static void *migrate_t2(void *arg) {
    struct result *r = arg;
    EGLContext w = new_ctx();
    make_current(w); /* T2's first make-current (and its idle wait) happens before Fa exists */
    await_fa();
    mig_wait(1);
    double t0 = now_ms();
    make_current(mig_ctx);
    static unsigned char px[256 * 128 * 4];
    glBindFramebuffer(GL_FRAMEBUFFER, mig_t.fb);
    glReadPixels(0, 0, 256, 128, GL_RGBA, GL_UNSIGNED_BYTE, px);
    r->t_finish = now_ms() - t0;
    for (int i = 0; i < 256 * 128; i++) r->bad_px += !(px[4 * i] == 255 && px[4 * i + 1] == 0 && px[4 * i + 2] == 255);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, w);
    eglReleaseThread();
    mig_set(2);
    return NULL;
}

static EGLConfig pbuffer_config(void) {
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
                         EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) DIE("eglChooseConfig(PBUFFER, ES3, RGBA8) 0x%x", eglGetError());
    return cfg;
}

static void *thread_b(void *arg) {
    struct result *r = arg;
    struct target t;
    EGLContext b = EGL_NO_CONTEXT, w = EGL_NO_CONTEXT;
    EGLSurface pb = EGL_NO_SURFACE;
    EGLDisplay bd = dpy; /* the display B lives on */
    if (is("pbuffer-first") || is("warm-then-pbuffer")) {
        EGLConfig cfg = pbuffer_config();
        const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE}, pa[] = {EGL_WIDTH, 256, EGL_HEIGHT, 128, EGL_NONE};
        b = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
        pb = eglCreatePbufferSurface(dpy, cfg, pa);
        if (!b || !pb) DIE("config context / pbuffer 0x%x", eglGetError());
        if (is("warm-then-pbuffer")) {
            w = new_ctx();
            make_current(w);
            close(native_fence());
        }
        if (!eglMakeCurrent(dpy, pb, pb, b)) DIE("eglMakeCurrent(pbuffer) 0x%x", eglGetError());
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else if (is("second-display")) {
        w = new_ctx();
        make_current(w);
        close(native_fence());
        bd = tdpy = dpy2;
        b = new_ctx_on(dpy2);
        make_current_on(dpy2, b);
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else if (is("terminate-reinit") || is("reinit-noterm")) {
        bd = tdpy = dpy2;
        w = new_ctx_on(dpy2);
        make_current_on(dpy2, w);
        close(native_fence_on(dpy2));
        make_current_on(dpy2, EGL_NO_CONTEXT);
        if (is("terminate-reinit")) {
            eglDestroyContext(dpy2, w);
            w = EGL_NO_CONTEXT;
            if (!eglTerminate(dpy2)) DIE("eglTerminate(display 2) 0x%x", eglGetError());
            if (!eglInitialize(dpy2, NULL, NULL)) DIE("eglInitialize(display 2) after terminate 0x%x", eglGetError());
        }
        b = new_ctx_on(dpy2);
        make_current_on(dpy2, b);
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else if (is("switch-ctx")) {
        b = new_ctx();
        w = new_ctx();
        make_current(w);
        make_current(b);
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else if (is("switch-after-wait")) {
        b = new_ctx();
        w = new_ctx();
        make_current(w);
        await_fa();
        gpu_wait(fa);
        glFlush();
        make_current(b);
        setup(&t);
    } else if (is("rebind")) {
        b = new_ctx();
        make_current(b);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        make_current(b);
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else if (is("release-thread")) {
        b = new_ctx();
        make_current(b);
        close(native_fence());
        if (!eglReleaseThread()) DIE("eglReleaseThread 0x%x", eglGetError());
        make_current(b);
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else if (is("main-warm-ctx")) {
        b = ctx_warm_main;
        make_current(b);
        setup(&t);
        await_fa();
        gpu_wait(fa);
    } else {
        b = new_ctx();
        make_current(b);
        setup(&t);
        if (is("warm") || is("khr-fence-warm")) close(native_fence());
        if (is("warm-nodup")) native_sync_nodup();
        if (is("khr-warm") || is("khr-warm-nodup")) {
            EGLSyncKHR s = createSync(dpy, EGL_SYNC_FENCE_KHR, NULL);
            if (s == EGL_NO_SYNC_KHR) DIE("eglCreateSyncKHR(FENCE) 0x%x", eglGetError());
            if (is("khr-warm")) {
                glFlush();
                clientWaitSync(dpy, s, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER_KHR);
            }
            destroySync(dpy, s);
        }
        await_fa();
        if (is("client-wait-first")) {
            int d = fcntl(fa, F_DUPFD_CLOEXEC, 0);
            EGLSyncKHR s = import(d);
            EGLint cw = clientWaitSync(dpy, s, 0, 0);
            r->note = cw == EGL_TIMEOUT_EXPIRED_KHR ? "client wait TIMEOUT_EXPIRED" : "client wait satisfied";
            destroySync(dpy, s);
        }
        if (is("two-waits")) gpu_wait(fcntl(fa, F_DUPFD_CLOEXEC, 0));
        gpu_wait(fa); /* the thread's first native-fence operation is a wait on an imported fence */
        if (is("warm-after-wait")) native_sync_nodup();
    }
    draw(&t);
    check_fence(r);
    eglMakeCurrent(bd, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (b != ctx_warm_main) eglDestroyContext(bd, b);
    if (w != EGL_NO_CONTEXT) eglDestroyContext(is("second-display") ? dpy : bd, w);
    if (pb != EGL_NO_SURFACE) eglDestroySurface(dpy, pb);
    eglReleaseThread();
    return NULL;
}

static void usage(void) {
    fprintf(stderr, "usage: fence-first-wait [--front LIB [--platform-lib PLAT]] [--rounds N] [--iters N] [--slow-a] "
            "[--a-iters N] [--mode M]\n  --slow-a: Fa covers a slow draw; --a-iters N: that draw's loop length (implies "
            "--slow-a)\nmodes:\n");
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) fprintf(stderr, "  %-18s %s\n", MODES[i].name, MODES[i].what);
    exit(100);
}

/* ---- resolving the entry points ---- */
static void *h_egl, *h_gles, *h_front;
static void *resolve(const char *n) {
    void *p = NULL;
    if (use_front) {
        p = dlsym(h_front, n); /* Android's loader order: dlsym, then eglGetProcAddress */
        if (!p && eglGetProcAddress) p = (void *)eglGetProcAddress(n);
    } else {
        p = dlsym(n[0] == 'g' ? h_gles : h_egl, n);
        if (!p && eglGetProcAddress) p = (void *)eglGetProcAddress(n);
    }
    if (!p) DIE("cannot resolve %s through %s", n, use_front ? "the front" : "stock libglvnd");
    return p;
}

static char json_path[64];
static void rm_json(void) { if (json_path[0]) unlink(json_path); }

int main(int argc, char **argv) {
    int rounds = 10;
    const char *front = NULL, *platlib = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--front") && i + 1 < argc) front = argv[++i];
        else if (!strcmp(argv[i], "--platform-lib") && i + 1 < argc) platlib = argv[++i];
        else if (!strcmp(argv[i], "--warm")) mode = "warm";
        else if (!strcmp(argv[i], "--same-thread")) mode = "same-thread";
        else if (!strcmp(argv[i], "--slow-a")) slow_a = 1;
        else if (!strcmp(argv[i], "--a-iters") && i + 1 < argc) a_iters = atoi(argv[++i]), slow_a = 1;
        else if (!strcmp(argv[i], "--no-timestamps")) use_ts = 0;
        else usage();
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *what = NULL;
    for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) if (is(MODES[i].name)) what = MODES[i].what;
    if (!what) usage();
    if (platlib && !front) DIE("--platform-lib needs --front");
    use_front = front != NULL;
    if (platlib) { /* the driver finds the platform the way host-window points it there (guest: nvglibc-default-env) */
        char abs[4096];
        snprintf(json_path, sizeof json_path, "/tmp/ffw-XXXXXX.json");
        int jf = mkstemps(json_path, 5);
        if (jf < 0 || !realpath(platlib, abs)) DIE("json/realpath");
        dprintf(jf, "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"%s\"}}\n", abs);
        close(jf);
        atexit(rm_json);
        setenv("__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES", json_path, 1);
    }
    if (use_front) {
        if (!(h_front = dlopen(front, RTLD_NOW | RTLD_LOCAL))) DIE("dlopen %s: %s", front, dlerror());
        eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(h_front, "eglGetProcAddress");
    } else {
        if (!(h_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL))) DIE("dlopen libEGL.so.1: %s", dlerror());
        if (!(h_gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL))) DIE("dlopen libGLESv2.so.2: %s", dlerror());
        eglGetProcAddress = (PFNEGLGETPROCADDRESSPROC)dlsym(h_egl, "eglGetProcAddress");
    }
    if (!eglGetProcAddress) DIE("no eglGetProcAddress");
#define RES(t, n) n = (t)resolve(#n);
    EGLFN(RES)
    GLFN(RES)
#undef RES
#define GPA(v, t, n) if (!(v = (t)eglGetProcAddress(n))) DIE("%s", n)
    GPA(queryDevices, PFNEGLQUERYDEVICESEXTPROC, "eglQueryDevicesEXT");
    GPA(getPlatformDisplayEXT, PFNEGLGETPLATFORMDISPLAYEXTPROC, "eglGetPlatformDisplayEXT");
    GPA(createSync, PFNEGLCREATESYNCKHRPROC, "eglCreateSyncKHR");
    GPA(destroySync, PFNEGLDESTROYSYNCKHRPROC, "eglDestroySyncKHR");
    GPA(waitSync, PFNEGLWAITSYNCKHRPROC, "eglWaitSyncKHR");
    GPA(clientWaitSync, PFNEGLCLIENTWAITSYNCKHRPROC, "eglClientWaitSyncKHR");
    GPA(dupFence, PFNEGLDUPNATIVEFENCEFDANDROIDPROC, "eglDupNativeFenceFDANDROID");
    EGLDeviceEXT dev[8], nvdev = NULL;
    EGLDisplay devdpy = EGL_NO_DISPLAY;
    EGLint nd = 0;
    if (!queryDevices(8, dev, &nd) || nd < 1) DIE("no EGL device");
    for (EGLint i = 0; i < nd && !nvdev; i++) { /* the NVIDIA device: the one whose display has native fences */
        EGLDisplay d = getPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, dev[i], NULL);
        if (d && eglInitialize(d, NULL, NULL)) {
            const char *v = eglQueryString(d, EGL_VENDOR), *x = eglQueryString(d, EGL_EXTENSIONS);
            if (v && strstr(v, "NVIDIA") && x && strstr(x, "EGL_ANDROID_native_fence_sync")) nvdev = dev[i], devdpy = d;
        }
    }
    if (!nvdev) DIE("no NVIDIA EGL device display with EGL_ANDROID_native_fence_sync");
    /* display 1 = what the test runs on; display 2 = a second display of the same device */
    EGLint tr[] = {EGL_TRACK_REFERENCES_KHR, EGL_TRUE, EGL_NONE};
    if (platlib) {
        dpy = eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, NULL);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) DIE("Android platform display 0x%x", eglGetError());
        dpy2 = getPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, nvdev, tr);
    } else {
        dpy = devdpy;
        dpy2 = getPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, nvdev, tr);
    }
    if (dpy2 == EGL_NO_DISPLAY || dpy2 == dpy || !eglInitialize(dpy2, NULL, NULL))
        DIE("no second display of the device (EGL_TRACK_REFERENCES_KHR): %p 0x%x", dpy2, eglGetError());
    eglBindAPI(EGL_OPENGL_ES_API);
    ctxA = new_ctx();
    struct target ta;
    make_current(ctxA);
    setup_n(&ta, a_iters > 0 ? a_iters : iters);
    if (use_ts && !strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_disjoint_timer_query")) {
        printf("fence-first-wait: GL_EXT_disjoint_timer_query absent: CPU-timing criterion only\n");
        use_ts = 0;
    }
    printf("fence-first-wait: %s %s via %s, display %s, %d rounds, mode %s: %s%s\n", eglQueryString(dpy, EGL_VENDOR),
           eglQueryString(dpy, EGL_VERSION), use_front ? front : "stock libglvnd",
           platlib ? "EGL_PLATFORM_ANDROID_KHR" : "EGL_PLATFORM_DEVICE_EXT", rounds, mode, what,
           slow_a ? " [Fa covers a slow draw]" : "");
    int bad = 0, migrate = is("migrate") || is("migrate-warm");
    hs_inline = is("same-thread");
    for (int r = 0; r < rounds; r++) {
        make_current(ctxA);
        if (is("main-warm-ctx")) { /* B gets a native fence in the main thread first */
            ctx_warm_main = new_ctx();
            make_current(ctx_warm_main);
            close(native_fence());
            make_current(ctxA);
        }
        struct result res = {0};
        pthread_t t[2];
        int nt = hs_inline ? 0 : migrate ? 2 : 1;
        hs_ready = hs_fa_posted = mig_stage = 0;
        if (migrate) {
            pthread_create(&t[0], NULL, migrate_t1, NULL);
            pthread_create(&t[1], NULL, migrate_t2, &res);
        } else if (nt) {
            pthread_create(&t[0], NULL, thread_b, &res);
        }
        pthread_mutex_lock(&hs_mu);
        while (hs_ready < nt) pthread_cond_wait(&hs_cv, &hs_mu);
        pthread_mutex_unlock(&hs_mu);
        glBindFramebuffer(GL_FRAMEBUFFER, ta.fb);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (slow_a) draw(&ta);
        fa = native_fence();
        struct pollfd qa = {.fd = fa, .events = POLLIN};
        int fa_pending = poll(&qa, 1, 0) == 0;
        pthread_mutex_lock(&hs_mu);
        hs_fa_posted = 1;
        pthread_cond_broadcast(&hs_cv);
        pthread_mutex_unlock(&hs_mu);
        if (hs_inline) thread_b(&res);
        for (int i = 0; i < nt; i++) pthread_join(t[i], NULL);
        if (migrate) {
            eglDestroyContext(dpy, mig_ctx);
            int early = res.bad_px != 0;
            bad += early;
            printf("round %d: Fa %s when posted; T2 read %d of %d px not yet drawn by T1 (read took %.2f ms)%s\n", r,
                   fa_pending ? "pending" : "signalled", res.bad_px, 256 * 128, res.t_finish,
                   early ? "  <-- READ BEFORE THE WORK COMPLETED" : "");
            continue;
        }
        if (ctx_warm_main) { eglDestroyContext(dpy, ctx_warm_main); ctx_warm_main = EGL_NO_CONTEXT; }
        /* early, with GPU timestamps: the draw Fr covers ended more than 0.1 ms after poll() saw Fr signalled.
         * Without them (--no-timestamps): glFinish after Fr still waited > 2 ms, which only holds on an otherwise idle
         * GPU (with other GPU work time-sliced in, glFinish's own submission can wait ~2.6 ms: measured). */
        double gap = res.t_finish - res.t_sig;
        int early = use_ts ? res.end_after_sig > 0.1 : gap > 2.0;
        bad += early;
        char tsb[96] = "";
        if (use_ts)
            snprintf(tsb, sizeof tsb, "; GPU: the draw ended %.3f ms %s Fr was seen signalled",
                     res.end_after_sig > 0 ? res.end_after_sig : -res.end_after_sig, res.end_after_sig > 0 ? "AFTER" : "before");
        printf("round %d: Fa %s when posted; Fr %s at creation, signalled at %.2f ms, glFinish returned at %.2f ms "
               "(gap %.2f)%s%s%s%s\n", r, fa_pending ? "pending" : "signalled",
               res.signalled_at_creation ? "SIGNALLED" : "pending", res.t_sig, res.t_finish, gap, tsb, res.note ? "; " : "",
               res.note ? res.note : "", early ? "  <-- EARLY FENCE" : "");
    }
    printf("fence-first-wait: mode %s via %s: %d of %d rounds had a fence signalled (or a read done) before the work it "
           "covers\n", mode, use_front ? "front" : "stock libglvnd", bad, rounds);
    return bad;
}
