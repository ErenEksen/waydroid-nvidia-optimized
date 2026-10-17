/* fence-race: the producer/consumer fence hand-off of one window buffer, with NO platform code involved.
 *
 * One thread, two contexts on a device display, one dma-buf (gralloc host build) imported by each context
 * (EGL_LINUX_DMA_BUF_EXT). One round = what host-window's first buffer reuse does:
 *   P: clear T to A; acquire fence Fa = native fence (create, glFlush, dup)
 *   C: eglWaitSync(Fa); read T (glReadPixels); slow full-screen "poison" draw into T; release fence Fr
 *   P: eglWaitSync(Fr) [or, with --skip-signalled, nothing when poll(Fr) already reports signalled]; clear T to B;
 *      glFinish; read T
 * T must then be B everywhere. Poison in T means the consumer's draw landed after the producer's clear although
 * the producer waited for Fr: Fr signalled before the draw it covers had completed. Run it in fresh processes
 * (the effect seen in host-window is first-use only).
 *
 * Bisection of README "D1" (FENCE-CONTRACT.md §3.3); the consumer also reports whether its release fence Fr was
 * already signalled when created, and how much of its draw was in memory (dma-buf mmap) when Fr signalled:
 *   --consumer-thread      C runs in its own thread (its first native-fence operation is the eglWaitSync): Fr was
 *                          early on 615.71.09, every run, until the front started each thread's fence timeline in
 *                          eglMakeCurrent (FENCE-CONTRACT.md §6a); since then 0 (make front-fence-check)
 *   --c-nowait             C waits for Fa on the CPU instead of eglWaitSync
 *   --c-warmfence          C's thread creates one native fence before its wait
 *   --c-warmfence-other    ... with another context current in that thread
 *   --c-warm-main          C's context creates a native fence in the main thread first (per-context state test)
 *   --c-warmdraw           C submits GPU work (no fence) before its wait
 *   --c-presignal          Fa has signalled before C waits
 *   --iters N              poison loop length (default 4000; 200000 = ~4 ms on an RTX 5090)
 * It also reports whether the dma-buf's implicit fence (DMA_BUF_IOCTL_EXPORT_SYNC_FILE) knows about C's flushed draw.
 * Exit status: 1 when T is not B, 2 when Fr signalled before C's draw was in memory. */
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "gbm_mesa_wrapper.h"
#include <drm_fourcc.h>

#define DIE(...) do { printf("FATAL: " __VA_ARGS__); printf("\n"); exit(100); } while (0)
static void *front;
static __eglMustCastToProperFunctionPointerType (*gpa)(const char *);
static void *sym(const char *n) {
    void *p = dlsym(front, n);
    if (!p) p = (void *)gpa(n);
    if (!p) DIE("no %s", n);
    return p;
}
#define FN(X) \
    X(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay) X(PFNEGLINITIALIZEPROC, eglInitialize) \
    X(PFNEGLQUERYDEVICESEXTPROC, eglQueryDevicesEXT) X(PFNEGLCREATECONTEXTPROC, eglCreateContext) \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLCREATESYNCPROC, eglCreateSync) X(PFNEGLDESTROYSYNCPROC, eglDestroySync) \
    X(PFNEGLDUPNATIVEFENCEFDANDROIDPROC, eglDupNativeFenceFDANDROID) X(PFNEGLBINDAPIPROC, eglBindAPI) \
    X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) X(PFNEGLWAITSYNCPROC, eglWaitSync) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLFINISHPROC, glFinish) X(PFNGLFLUSHPROC, glFlush) X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)
#define DECL(t, n) static t n;
FN(DECL)
#define RES(t, n) n = (t)sym(#n);

static EGLDisplay d;
static int fence(void) {
    EGLSync s = eglCreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    glFlush();
    int fd = eglDupNativeFenceFDANDROID(d, s);
    eglDestroySync(d, s);
    return fd;
}
static void gpu_wait(int fd) {
    EGLAttrib a[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSync s = eglCreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    eglWaitSync(d, s, 0);
    eglDestroySync(d, s);
}
static GLuint fbo_for(EGLImageKHR img);
static struct { int fa, fr, green, sig_at_create, poison_at_signal; double t_sig; } cs;
static EGLContext C;
static EGLint *g_ia;
static GLuint g_prog, g_vao;
static int consumer_thread, g_buf_fd, poison_iters = 4000, c_nowait, c_warmfence, c_warmdraw, c_presignal, c_warmfence_other, c_warm_main;
static EGLContext X;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void *consumer_run(void *arg) {
    (void)arg;
    const int W = 256, H = 128;
    if (c_warmfence_other) { /* a native fence in this thread, with another context current */
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, X);
        close(fence());
    }
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, C);
    if (c_warmdraw) { /* GPU work in this thread before the first wait (no fence) */
        GLuint rb, fb;
        glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
        glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        glViewport(0, 0, 16, 16); glClear(GL_COLOR_BUFFER_BIT); glFlush();
    }
    if (c_warmfence) close(fence()); /* one native fence in this thread before the first wait */
    if (c_presignal) { struct pollfd q = {.fd = cs.fa, .events = POLLIN}; poll(&q, 1, 5000); }
    if (c_nowait) { struct pollfd q = {.fd = cs.fa, .events = POLLIN}; poll(&q, 1, 5000); close(cs.fa); }
    else gpu_wait(cs.fa);
    GLuint cf = fbo_for(eglCreateImageKHR(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, g_ia));
    static unsigned char px[256 * 128 * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    cs.green = 0;
    for (int i = 0; i < W * H; i++) cs.green += px[4 * i] == 0 && px[4 * i + 1] == 255;
    glBindFramebuffer(GL_FRAMEBUFFER, cf);
    glViewport(0, 0, W, H);
    glUseProgram(g_prog);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFlush();
    { /* implicit sync: does the dma-buf's reservation object know about the outstanding GPU write? */
        struct dma_buf_export_sync_file e = {.flags = DMA_BUF_SYNC_RW, .fd = -1};
        int r = ioctl(g_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &e);
        struct pollfd q = {.fd = e.fd, .events = POLLIN};
        printf("fence-race: implicit fence (DMA_BUF_IOCTL_EXPORT_SYNC_FILE rc %d) with the poison draw flushed and "
               "outstanding: %s\n", r, r == 0 && poll(&q, 1, 0) == 1 ? "SIGNALLED" : "pending");
        if (e.fd >= 0) close(e.fd);
    }
    double t0 = now_ms();
    cs.fr = fence();
    struct pollfd p = {.fd = cs.fr, .events = POLLIN};
    cs.sig_at_create = poll(&p, 1, 0) == 1;
    poll(&p, 1, 5000);
    cs.t_sig = now_ms() - t0;
    { /* what memory holds at the moment the fence reports signalled (CPU view through the dma-buf) */
        size_t sz = (size_t)lseek(g_buf_fd, 0, SEEK_END), np = 0, n = 0;
        struct dma_buf_sync dsy = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        ioctl(g_buf_fd, DMA_BUF_IOCTL_SYNC, &dsy);
        const unsigned char *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, g_buf_fd, 0);
        for (size_t x = 0; m != MAP_FAILED && x + 4 <= sz; x += 4, n++) np += m[x] == 255 && m[x + 1] == 0 && m[x + 2] == 255;
        if (m != MAP_FAILED) munmap((void *)m, sz);
        dsy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(g_buf_fd, DMA_BUF_IOCTL_SYNC, &dsy);
        cs.poison_at_signal = (int)np;
        glFinish();
        double tf = now_ms() - t0;
        printf("fence-race: consumer: poison in memory when the release fence signalled: %zu of %zu px; draw finished "
               "(glFinish) %.2f ms after fence creation\n", np, n, tf);
    }
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return NULL;
}
static GLuint fbo_for(EGLImageKHR img) {
    GLuint t, f;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    return f;
}

int main(int argc, char **argv) {
    const char *frontp = NULL, *gralloc = NULL;
    int skip_signalled = 0, wait_cpu = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--front") && i + 1 < argc) frontp = argv[++i];
        else if (!strcmp(argv[i], "--gralloc") && i + 1 < argc) gralloc = argv[++i];
        else if (!strcmp(argv[i], "--skip-signalled")) skip_signalled = 1;
        else if (!strcmp(argv[i], "--wait-cpu")) wait_cpu = 1;
        else if (!strcmp(argv[i], "--consumer-thread")) consumer_thread = 1;
        else if (!strcmp(argv[i], "--c-nowait")) c_nowait = 1;
        else if (!strcmp(argv[i], "--c-warmfence")) c_warmfence = 1;
        else if (!strcmp(argv[i], "--c-warmdraw")) c_warmdraw = 1;
        else if (!strcmp(argv[i], "--c-presignal")) c_presignal = 1;
        else if (!strcmp(argv[i], "--c-warmfence-other")) c_warmfence_other = 1;
        else if (!strcmp(argv[i], "--c-warm-main")) c_warm_main = 1;
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) poison_iters = atoi(argv[++i]); /* hand off only once poll() reports Fr signalled */
        else DIE("usage: fence-race --front libEGL_nvnative.so --gralloc libgbm_mesa_wrapper.so [--skip-signalled]");
    }
    if (!frontp || !gralloc || !(front = dlopen(frontp, RTLD_NOW))) DIE("front/gralloc");
    gpa = (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(front, "eglGetProcAddress");
    FN(RES)
    void *g = dlopen(gralloc, RTLD_NOW);
    if (!g) DIE("gralloc");
    struct gbm_ops *ops = ((struct gbm_ops * (*)(void))dlsym(g, "get_gbm_ops"))();
    glob_t gl;
    glob("/dev/dri/renderD*", 0, NULL, &gl);
    int rfd = -1;
    for (size_t i = 0; i < gl.gl_pathc && rfd < 0; i++) {
        char p[256], v[16] = "";
        snprintf(p, sizeof(p), "/sys/class/drm/%s/device/vendor", strrchr(gl.gl_pathv[i], '/') + 1);
        FILE *f = fopen(p, "r");
        if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
        if (!strncmp(v, "0x10de", 6)) rfd = open(gl.gl_pathv[i], O_RDWR);
    }
    globfree(&gl);
    struct gbm_device *gdev = ops->dev_create(rfd);
    const int W = 256, H = 128;
    struct alloc_args a = {.gbm = gdev, .width = W, .height = H, .drm_format = DRM_FORMAT_ABGR8888};
    if (ops->alloc(&a)) DIE("alloc");
    g_buf_fd = a.out_fd;
    EGLDeviceEXT dev;
    EGLint nd = 0;
    eglQueryDevicesEXT(1, &dev, &nd);
    d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
    if (!eglInitialize(d, NULL, NULL)) DIE("init");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext P = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    C = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    X = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    EGLint ia[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888, EGL_DMA_BUF_PLANE0_FD_EXT, a.out_fd,
                   EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)a.out_stride,
                   EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(a.out_modifier & 0xffffffff),
                   EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(a.out_modifier >> 32), EGL_NONE};
    /* consumer setup first, like a compositor that is already running */
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, C);
    static const char *vs = "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
                            "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
    static const char *fs0 = "#version 300 es\nprecision highp float;\nout vec4 o;\nvoid main(){float a=gl_FragCoord.x;"
                            "for(int i=0;i<%d;i++){a=fract(sin(a*12.9898+float(i))*43758.5453);}o=vec4(1.0,0.0,1.0,1.0)+vec4(a*1e-9);}\n";
    GLuint s1 = glCreateShader(GL_VERTEX_SHADER), s2 = glCreateShader(GL_FRAGMENT_SHADER), prog = glCreateProgram();
    glShaderSource(s1, 1, &vs, NULL); glCompileShader(s1);
    char fsb[512];
    snprintf(fsb, sizeof(fsb), fs0, poison_iters);
    const char *fs = fsb;
    glShaderSource(s2, 1, &fs, NULL); glCompileShader(s2);
    glAttachShader(prog, s1); glAttachShader(prog, s2); glLinkProgram(prog);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    g_prog = prog;
    g_vao = vao;
    g_ia = ia;
    if (c_warm_main) { /* the consumer's context creates a native fence in the MAIN thread first */
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, C);
        close(fence());
    }
    /* P: frame */
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, P);
    GLuint pf = fbo_for(eglCreateImageKHR(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia));
    glViewport(0, 0, W, H);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int fa = fence();
    /* C: acquire, read, poison, release (in this thread, or with --consumer-thread in its own thread) */
    cs.fa = fa;
    if (consumer_thread) {
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        pthread_t t;
        pthread_create(&t, NULL, consumer_run, NULL);
        pthread_join(t, NULL);
    } else {
        consumer_run(NULL);
    }
    int fr = cs.fr, green = cs.green;
    /* P: next frame into the same buffer after the release fence */
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, P);
    struct pollfd p = {.fd = fr, .events = POLLIN};
    if (wait_cpu) poll(&p, 1, 5000);
    int signalled = poll(&p, 1, 0) == 1;
    if (skip_signalled && signalled) close(fr);
    else gpu_wait(fr);
    static unsigned char px[256 * 128 * 4];
    glBindFramebuffer(GL_FRAMEBUFFER, pf);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int blue = 0, poison = 0;
    for (int i = 0; i < W * H; i++) {
        blue += px[4 * i] == 0 && px[4 * i + 1] == 0 && px[4 * i + 2] == 255;
        poison += px[4 * i] == 255 && px[4 * i + 1] == 0 && px[4 * i + 2] == 255;
    }
    usleep(50000);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int poison_late = 0;
    for (int i = 0; i < W * H; i++) poison_late += px[4 * i] == 255 && px[4 * i + 1] == 0 && px[4 * i + 2] == 255;
    printf("fence-race: release fence %s at creation, signalled %.2f ms after creation\n",
           cs.sig_at_create ? "SIGNALLED" : "pending", cs.t_sig);
    printf("fence-race: consumer read %d/%d green; release fence %s at hand-off; producer frame: %d blue, %d poison "
           "(50 ms later %d poison) -> %s\n", green, W * H, signalled ? "already signalled" : "pending", blue, poison,
           poison_late, blue == W * H && poison_late == 0 ? "OK" : "RACE");
    ops->dev_destroy(gdev);
    if (!(blue == W * H && poison_late == 0)) return 1;
    return cs.poison_at_signal < W * H ? 2 : 0;
}
