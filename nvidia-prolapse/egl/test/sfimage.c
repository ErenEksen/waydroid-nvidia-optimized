/* sfimage: SurfaceFlinger RenderEngine's EGL sequence, through Android's own libEGL loader (so through whatever
 * ro.hardware.egl selects), without SurfaceFlinger. G7d diagnosis tool.
 *
 * Mirrors frameworks/native android13-release libs/renderengine/skia/SkiaGLRenderEngine.cpp:164-245 (display,
 * no-config ES3 context with EGL_CONTEXT_PRIORITY_LEVEL_IMG, surfaceless make-current) and Skia's
 * GrAHardwareBufferUtils.cpp:226-266 (AHB -> eglGetNativeClientBufferANDROID -> eglCreateImageKHR(EGL_NO_CONTEXT,
 * EGL_NATIVE_BUFFER_ANDROID, {EGL_IMAGE_PRESERVED_KHR, TRUE}) -> glEGLImageTargetTexture2DOES(GL_TEXTURE_2D)), then
 * renders into the texture through an FBO and reads it back.
 *
 *   sfimage [--size WxH] [--usage HEX] [--config] [--no-priority] [--protected-ctx] [--thread] [--cpu-check]
 *
 * --usage: raw AHardwareBuffer usage bits (default: SurfaceFlinger's client target, GPU_FRAMEBUFFER |
 *          COMPOSER_OVERLAY | 0x1000 (GRALLOC_USAGE_HW_FB)).
 * --cpu-check (needs CPU_READ|CPU_WRITE in --usage, e.g. b33 = SurfaceFlinger's screenshot buffer): the gralloc lock
 *          contract on a CPU-mapped render target. GL renders (clear + scissored clear); AHardwareBuffer_lock with the
 *          EGL native fence of that work as the acquire fence; every pixel checked through the CPU view; a CPU-written
 *          rectangle; unlock; every pixel checked again through glReadPixels (GL sees the CPU write) and through a
 *          second lock. Then the lock access flags through the real mapper (minigbm patch 0002): a READ lock held
 *          while GL renders a rectangle (glFinish, so the GL write lands between the lock and the unlock) and the
 *          CPU scribbles over its view must leave the GL rectangle and no scribble; a nested lock (READ, then
 *          WRITE while the READ is held) must write its CPU rectangle back; a WRITE-only lock of a rectangle must
 *          keep every other pixel. Exact values.
 * Exit status = number of failed checks. */
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int W = 2560, H = 1412;
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
/* --cpu-check scene: green everywhere, red rect R (GL), CPU pattern in rect C (written after R) */
static uint32_t cpu_pat(int x, int y) { return 0xff000000u | (uint32_t)((x * 7 + y * 13) & 0xffffff); }
static uint32_t want_px(int x, int y, int stage) {
    if (stage >= 2 && x >= W / 2 && x < W && y >= H / 3 && y < H / 3 + H / 2) return cpu_pat(x, y);
    if (x >= W / 4 && x < W / 2 + W / 8 && y >= H / 4 && y < H / 2) return 0xff0000ffu; /* red, RGBA bytes */
    return 0xff00ff00u;                                                                 /* green */
}
static long count_bad(const uint32_t *p, size_t pitch_px, int stage, int *fx, int *fy, uint32_t *got) {
    long bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = p[(size_t)y * pitch_px + (size_t)x];
            if (v != want_px(x, y, stage) && !bad++) { *fx = x; *fy = y; *got = v; }
        }
    return bad;
}
static void cpu_lock_check(EGLDisplay dpy, AHardwareBuffer *ahb, GLuint fbo);
static void access_flags_check(AHardwareBuffer *ahb, GLuint fbo, uint32_t stride);

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG 0x3101
#endif

static int use_config, no_prio, prot_ctx, in_thread, cpu_check;
static uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY | 0x1000;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *run(void *unused) {
    (void)unused;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    CHECK(eglInitialize(dpy, &maj, &min), "eglInitialize -> %d.%d", maj, min);
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("EGL_VENDOR %s\nEGL_EXTENSIONS %s\n", eglQueryString(dpy, EGL_VENDOR), ext ? ext : "(null)");
    EGLConfig cfg = EGL_NO_CONFIG_KHR;
    if (use_config) {
        EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                       EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint n = 0;
        CHECK(eglChooseConfig(dpy, ca, &cfg, 1, &n) && n == 1, "eglChooseConfig (n %d)", n);
    }
    EGLint cxa[16], i = 0;
    cxa[i++] = EGL_CONTEXT_CLIENT_VERSION; cxa[i++] = 3;
    if (!no_prio) { cxa[i++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG; cxa[i++] = EGL_CONTEXT_PRIORITY_HIGH_IMG; }
    if (prot_ctx) { cxa[i++] = EGL_PROTECTED_CONTENT_EXT; cxa[i++] = EGL_TRUE; }
    cxa[i++] = EGL_NONE;
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cxa);
    CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext(%s, ES3%s%s) err 0x%x", use_config ? "config" : "NO_CONFIG",
          no_prio ? "" : ", priority HIGH", prot_ctx ? ", protected" : "", eglGetError());
    if (ctx == EGL_NO_CONTEXT) return NULL;
    EGLint prio = 0;
    eglQueryContext(dpy, ctx, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &prio);
    printf("context priority 0x%x\n", prio);
    CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx), "eglMakeCurrent surfaceless err 0x%x",
          eglGetError());
    printf("GL_RENDERER %s\n", (const char *)glGetString(GL_RENDERER));

    AHardwareBuffer_Desc d = {0};
    d.width = (uint32_t)W; d.height = (uint32_t)H; d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = usage;
    AHardwareBuffer *ahb = NULL;
    int r = AHardwareBuffer_allocate(&d, &ahb);
    CHECK(r == 0, "AHardwareBuffer_allocate %dx%d usage 0x%llx = %d", W, H, (unsigned long long)usage, r);
    if (r) return NULL;
    EGLClientBuffer cb = eglGetNativeClientBufferANDROID(ahb);
    EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, ia);
    CHECK(img != EGL_NO_IMAGE_KHR, "eglCreateImageKHR(NATIVE_BUFFER, PRESERVED) err 0x%x", eglGetError());
    if (img == EGL_NO_IMAGE_KHR) return NULL;
    while (glGetError() != GL_NO_ERROR) {}
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum e = glGetError();
    CHECK(e == GL_NO_ERROR, "glBindTexture(GL_TEXTURE_2D) 0x%x", e);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    e = glGetError();
    CHECK(e == GL_NO_ERROR, "glEGLImageTargetTexture2DOES(GL_TEXTURE_2D) 0x%x", e);
    if (e == GL_NO_ERROR) {
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK(st == GL_FRAMEBUFFER_COMPLETE, "FBO status 0x%x", st);
        glClearColor(0.f, 1.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (cpu_check) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(W / 4, H / 4, W / 2 + W / 8 - W / 4, H / 2 - H / 4);
            glClearColor(1.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            cpu_lock_check(dpy, ahb, fbo);
        }
        unsigned char px[4] = {0};
        glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (!cpu_check)
            CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255, "render+readback = %u,%u,%u,%u", px[0],
                  px[1], px[2], px[3]);
    }
    /* the same image as GL_TEXTURE_EXTERNAL_OES, for comparison */
    GLuint t2;
    glGenTextures(1, &t2);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, t2);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    e = glGetError();
    printf("info: same image as GL_TEXTURE_EXTERNAL_OES: 0x%x\n", e);
    eglDestroyImageKHR(dpy, img);
    AHardwareBuffer_release(ahb);
    return NULL;
}

static void cpu_lock_check(EGLDisplay dpy, AHardwareBuffer *ahb, GLuint fbo) {
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(ahb, &d);
    printf("buffer: %ux%u stride %u px, usage 0x%llx\n", d.width, d.height, d.stride, (unsigned long long)d.usage);
    /* the acquire fence: an EGL native fence after the GL work, as SurfaceFlinger hands one out */
    EGLSyncKHR sync = eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    glFlush();
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    int fence = sync != EGL_NO_SYNC_KHR && dup_fence ? dup_fence(dpy, sync) : -1;
    CHECK(fence >= 0, "native fence for the GL work: fd %d", fence);
    if (sync != EGL_NO_SYNC_KHR) eglDestroySyncKHR(dpy, sync);
    void *addr = NULL;
    double t0 = now_ms();
    int r = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                 fence, NULL, &addr);
    double t1 = now_ms();
    CHECK(r == 0 && addr, "AHardwareBuffer_lock(read|write, acquire fence) = %d (%.2f ms)", r, t1 - t0);
    if (r || !addr) return;
    int fx = 0, fy = 0;
    uint32_t got = 0;
    long bad = count_bad(addr, d.stride, 1, &fx, &fy, &got);
    CHECK(!bad, "CPU view of the GL render: %ld/%d pixels wrong (first (%d,%d) 0x%08x want 0x%08x)", bad, W * H, fx,
          fy, got, want_px(fx, fy, 1));
    for (int y = H / 3; y < H / 3 + H / 2; y++)
        for (int x = W / 2; x < W; x++) ((uint32_t *)addr)[(size_t)y * d.stride + (size_t)x] = cpu_pat(x, y);
    int rel = -2;
    t0 = now_ms();
    r = AHardwareBuffer_unlock(ahb, &rel);
    t1 = now_ms();
    CHECK(r == 0 && rel == -1, "AHardwareBuffer_unlock = %d, release fence %d (-1: complete) (%.2f ms)", r, rel,
          t1 - t0);
    if (rel >= 0) close(rel);
    /* GL sees the CPU write */
    uint32_t *px = malloc((size_t)W * H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    bad = count_bad(px, (size_t)W, 2, &fx, &fy, &got);
    CHECK(!bad && glGetError() == GL_NO_ERROR, "glReadPixels after the CPU write: %ld pixels wrong (first (%d,%d) "
          "0x%08x want 0x%08x)", bad, fx, fy, got, want_px(fx, fy, 2));
    free(px);
    /* and a second lock sees it too */
    r = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &addr);
    if (r == 0 && addr) {
        bad = count_bad(addr, d.stride, 2, &fx, &fy, &got);
        AHardwareBuffer_unlock(ahb, NULL);
    }
    CHECK(r == 0 && !bad, "second lock (read): %ld pixels wrong", bad);
    access_flags_check(ahb, fbo, d.stride);
}

/* expected image after stage 2, then edited per step; compared with glReadPixels */
static uint32_t *g_exp;
static void exp_rect(int x0, int y0, int x1, int y1, uint32_t (*f)(int, int)) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) g_exp[(size_t)y * W + x] = f(x, y);
}
static uint32_t blue(int x, int y) { (void)x; (void)y; return 0xffff0000u; } /* RGBA bytes */
static uint32_t pat2(int x, int y) { return 0xff000000u | (uint32_t)((x * 3 + y * 29 + 0x5a5a) & 0xffffff); }
static uint32_t pat3(int x, int y) { return 0xff000000u | (uint32_t)((x * 11 + y * 5 + 0x3c3c3c) & 0xffffff); }
static long gl_cmp(GLuint fbo, int *fx, int *fy, uint32_t *got, uint32_t *want) {
    uint32_t *px = malloc((size_t)W * H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    long bad = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
        if (px[i] != g_exp[i] && !bad++) { *fx = (int)(i % (size_t)W); *fy = (int)(i / (size_t)W); *got = px[i];
                                           *want = g_exp[i]; }
    free(px);
    return glGetError() == GL_NO_ERROR ? bad : -1;
}
static void access_flags_check(AHardwareBuffer *ahb, GLuint fbo, uint32_t stride) {
    g_exp = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) g_exp[(size_t)y * W + x] = want_px(x, y, 2);
    int fx = 0, fy = 0;
    uint32_t got = 0, want = 0;
    long bad;
    void *addr = NULL;
    /* 1. READ lock; GL renders rect G (blue) and finishes while it is held; the CPU scribbles over its view */
    const int gx0 = W / 8, gy0 = H / 8, gx1 = W / 8 + W / 3, gy1 = H / 8 + H / 3;
    int r = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &addr);
    CHECK(r == 0 && addr, "READ lock = %d", r);
    if (r == 0 && addr) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(gx0, gy0, gx1 - gx0, gy1 - gy0);
        glClearColor(0.f, 0.f, 1.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glFinish(); /* the GL write is complete before the unlock */
        memset(addr, 0x77, (size_t)stride * 4 * H);
        AHardwareBuffer_unlock(ahb, NULL);
        exp_rect(gx0, gy0, gx1, gy1, blue);
        bad = gl_cmp(fbo, &fx, &fy, &got, &want);
        CHECK(!bad, "READ lock does not write the buffer: GL write during the lock kept, CPU scribble not copied "
              "back: %ld pixels wrong (first (%d,%d) 0x%08x want 0x%08x)", bad, fx, fy, got, want);
    }
    /* 2. nested: READ held, then WRITE (the mapping widens), CPU writes rect N, two unlocks */
    const int nx0 = W / 2, ny0 = (2 * H) / 3, nx1 = W - W / 16, ny1 = H - H / 16;
    void *a2 = NULL;
    r = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &addr);
    int r2 = r ? -1 : AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &a2);
    CHECK(r == 0 && r2 == 0 && a2 == addr, "nested lock READ then WRITE = %d, %d (same address %d)", r, r2,
          a2 == addr);
    if (r == 0 && r2 == 0) {
        for (int y = ny0; y < ny1; y++)
            for (int x = nx0; x < nx1; x++) ((uint32_t *)a2)[(size_t)y * stride + (size_t)x] = pat2(x, y);
        AHardwareBuffer_unlock(ahb, NULL);
        AHardwareBuffer_unlock(ahb, NULL);
        exp_rect(nx0, ny0, nx1, ny1, pat2);
        bad = gl_cmp(fbo, &fx, &fy, &got, &want);
        CHECK(!bad, "nested READ+WRITE lock: CPU write copied back: %ld pixels wrong (first (%d,%d) 0x%08x want "
              "0x%08x)", bad, fx, fy, got, want);
    } else if (r == 0) {
        AHardwareBuffer_unlock(ahb, NULL);
    }
    /* 3. WRITE-only lock of rect X: every other pixel kept */
    const int wx0 = 0, wy0 = H / 2, wx1 = W / 3, wy1 = H;
    const ARect rect = {wx0, wy0, wx1, wy1};
    r = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, &rect, &addr);
    CHECK(r == 0 && addr, "WRITE-only lock of a rect = %d", r);
    if (r == 0 && addr) {
        for (int y = wy0; y < wy1; y++)
            for (int x = wx0; x < wx1; x++) ((uint32_t *)addr)[(size_t)y * stride + (size_t)x] = pat3(x, y);
        AHardwareBuffer_unlock(ahb, NULL);
        exp_rect(wx0, wy0, wx1, wy1, pat3);
        bad = gl_cmp(fbo, &fx, &fy, &got, &want);
        CHECK(!bad, "WRITE-only lock: rect written, the rest kept: %ld pixels wrong (first (%d,%d) 0x%08x want "
              "0x%08x)", bad, fx, fy, got, want);
    }
    free(g_exp);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--usage") && i + 1 < argc) usage = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--config")) use_config = 1;
        else if (!strcmp(argv[i], "--no-priority")) no_prio = 1;
        else if (!strcmp(argv[i], "--protected-ctx")) prot_ctx = 1;
        else if (!strcmp(argv[i], "--thread")) in_thread = 1;
        else if (!strcmp(argv[i], "--cpu-check")) cpu_check = 1;
        else { fprintf(stderr, "usage: sfimage [--size WxH] [--usage HEX] [--config] [--no-priority] "
                       "[--protected-ctx] [--thread] [--cpu-check]\n"); return 2; }
    }
    if (in_thread) {
        pthread_t t;
        pthread_create(&t, NULL, run, NULL);
        pthread_join(t, NULL);
    } else {
        run(NULL);
    }
    printf("sfimage: %d failed checks\n", fails);
    return fails;
}
