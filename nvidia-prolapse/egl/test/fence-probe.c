/* fence-probe: does an EGL_ANDROID_native_fence_sync fd from NVIDIA's EGL signal only after the GPU work
 * submitted before it has completed?
 *
 * Host, glibc, NVIDIA's EGL through our front (device display, no window system). For each round in a fresh
 * context: a deliberately slow full-screen draw (into a renderbuffer FBO, or into an EGLImage of a dma-buf from
 * our gralloc backend with --dmabuf), then eglCreateSync(EGL_SYNC_NATIVE_FENCE_ANDROID) + glFlush +
 * eglDupNativeFenceFDANDROID, then:
 *   t_sig  = time until poll(fd) reports the fence signalled,
 *   pixels = what the draw target holds right then, read by a SECOND context (its own glReadPixels, i.e. a GPU
 *            consumer that trusts the fence), compared with the colour the slow draw writes.
 * A correct fence gives "all pixels written" every time. Exit status = rounds with an early fence. */
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
#include <time.h>
#include <unistd.h>

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
    X(PFNEGLDUPNATIVEFENCEFDANDROIDPROC, eglDupNativeFenceFDANDROID) X(PFNEGLGETERRORPROC, eglGetError) \
    X(PFNEGLBINDAPIPROC, eglBindAPI) X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) X(PFNEGLWAITSYNCPROC, eglWaitSync) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLFINISHPROC, glFinish) X(PFNGLFLUSHPROC, glFlush) X(PFNGLCLEARPROC, glClear) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1FPROC, glUniform1f)
#define DECL(t, n) static t n;
FN(DECL)
#define RES(t, n) n = (t)sym(#n);

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    const char *frontp = NULL, *gralloc = NULL;
    int rounds = 6, iters = 20000, wait_first = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--front") && i + 1 < argc) frontp = argv[++i];
        else if (!strcmp(argv[i], "--dmabuf") && i + 1 < argc) gralloc = argv[++i];
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wait-first")) wait_first = 1;
        else DIE("usage: fence-probe --front libEGL_nvnative.so [--dmabuf libgbm_mesa_wrapper.so] [--rounds N] [--iters N]");
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!frontp || !(front = dlopen(frontp, RTLD_NOW))) DIE("front");
    gpa = (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(front, "eglGetProcAddress");
    FN(RES)
    EGLDeviceEXT dev;
    EGLint nd = 0;
    eglQueryDevicesEXT(1, &dev, &nd);
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
    if (!eglInitialize(d, NULL, NULL)) DIE("init");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    const int W = 256, H = 128;
    int early_rounds = 0;
    struct gbm_ops *ops = NULL;
    struct gbm_device *gdev = NULL;
    if (gralloc) {
        void *g = dlopen(gralloc, RTLD_NOW);
        if (!g) DIE("gralloc");
        ops = ((struct gbm_ops * (*)(void))dlsym(g, "get_gbm_ops"))();
        glob_t gl;
        glob("/dev/dri/renderD*", 0, NULL, &gl);
        int fd = -1;
        for (size_t i = 0; i < gl.gl_pathc && fd < 0; i++) {
            char p[256], v[16] = "";
            snprintf(p, sizeof(p), "/sys/class/drm/%s/device/vendor", strrchr(gl.gl_pathv[i], '/') + 1);
            FILE *f = fopen(p, "r");
            if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
            if (!strncmp(v, "0x10de", 6)) fd = open(gl.gl_pathv[i], O_RDWR);
        }
        globfree(&gl);
        gdev = ops->dev_create(fd);
    }
    for (int r = 0; r < rounds; r++) {
        /* fresh producer and consumer contexts each round: the effect, if any, may be a first-use one */
        EGLContext prod = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
        EGLContext cons = eglCreateContext(d, EGL_NO_CONFIG_KHR, prod, ca); /* shares the renderbuffer/texture */
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, prod);
        GLuint fbo, tex = 0, rb = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        if (gralloc) {
            struct alloc_args a = {.gbm = gdev, .width = W, .height = H, .drm_format = DRM_FORMAT_ABGR8888};
            if (ops->alloc(&a)) DIE("alloc");
            EGLint ia[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
                           EGL_DMA_BUF_PLANE0_FD_EXT, a.out_fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                           EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)a.out_stride,
                           EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(a.out_modifier & 0xffffffff),
                           EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(a.out_modifier >> 32), EGL_NONE};
            EGLImageKHR img = eglCreateImageKHR(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
            if (!img) DIE("import");
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        } else {
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        }
        glViewport(0, 0, W, H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        static const char *vs = "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
                                "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
        static const char *fs = "#version 300 es\nprecision highp float;\nuniform float n;\nout vec4 o;\nvoid main(){float a=gl_FragCoord.x;"
                                "for(int i=0;i<int(n);i++){a=fract(sin(a*12.9898+float(i))*43758.5453);}o=vec4(1.0,0.0,1.0,1.0)+vec4(a*1e-9);}\n";
        GLuint s1 = glCreateShader(GL_VERTEX_SHADER), s2 = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
        glShaderSource(s1, 1, &vs, NULL); glCompileShader(s1);
        glShaderSource(s2, 1, &fs, NULL); glCompileShader(s2);
        glAttachShader(p, s1); glAttachShader(p, s2); glLinkProgram(p);
        glUseProgram(p);
        glUniform1f(glGetUniformLocation(p, "n"), (float)iters);
        GLuint vao;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        if (wait_first) { /* what a compositor does first: GPU-wait an imported native fence (here an already signalled one) */
            EGLSync s0 = eglCreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
            glFlush();
            int fd0 = eglDupNativeFenceFDANDROID(d, s0);
            eglDestroySync(d, s0);
            struct pollfd p0 = {.fd = fd0, .events = POLLIN};
            poll(&p0, 1, 5000);
            EGLAttrib a0[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd0, EGL_NONE};
            EGLSync w0 = eglCreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, a0);
            eglWaitSync(d, w0, 0);
            eglDestroySync(d, w0);
        }
        double t0 = now();
        glDrawArrays(GL_TRIANGLES, 0, 3);
        EGLSync s = eglCreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        glFlush();
        int fd = eglDupNativeFenceFDANDROID(d, s);
        eglDestroySync(d, s);
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        int immediate = poll(&pf, 1, 0);
        poll(&pf, 1, 5000);
        double tsig = now() - t0;
        /* consumer: a second context reads the target, ordered after the fence by eglWaitSync */
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, cons);
        GLuint cfbo;
        glGenFramebuffers(1, &cfbo);
        glBindFramebuffer(GL_FRAMEBUFFER, cfbo);
        if (tex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        else glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        EGLAttrib fa[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
        EGLSync ws = eglCreateSync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, fa);
        eglWaitSync(d, ws, 0);
        eglDestroySync(d, ws);
        static unsigned char px[256 * 128 * 4];
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int done = 0;
        for (int i = 0; i < W * H; i++) done += px[4 * i] == 255 && px[4 * i + 1] == 0 && px[4 * i + 2] == 255;
        /* then the truth: the producer finishes and reads */
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, prod);
        glFinish();
        double tfin = now() - t0;
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int done2 = 0;
        for (int i = 0; i < W * H; i++) done2 += px[4 * i] == 255 && px[4 * i + 1] == 0 && px[4 * i + 2] == 255;
        int early = done < W * H;
        early_rounds += early;
        printf("round %d (%s%s): fence signalled %s after %.2f ms; consumer saw %d/%d written; after glFinish (%.2f ms) %d/%d%s\n",
               r, gralloc ? "dma-buf EGLImage FBO" : "renderbuffer FBO", wait_first ? ", after eglWaitSync" : "", immediate == 1 ? "IMMEDIATELY" : "", tsig * 1e3, done,
               W * H, tfin * 1e3, done2, W * H, early ? "  <-- EARLY FENCE" : "");
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (gdev) ops->dev_destroy(gdev);
    printf("fence-probe: %d of %d rounds had an early fence\n", early_rounds, rounds);
    return early_rounds;
}
