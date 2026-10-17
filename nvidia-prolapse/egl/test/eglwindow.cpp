/* eglwindow: G7c in the guest. A native executable that gets a real SurfaceFlinger layer, does what Android's
 * EGL loader does to its ANativeWindow, loads OUR front (libEGL_nvnative.so) through the sphal namespace like
 * Loader.cpp, and renders animated, verifiable frames through NVIDIA's own GLES. Also exercises
 * EGL_ANDROID_image_native_buffer on AHardwareBuffers.
 *
 * Run from a [system]-section directory (/data/local/tests/system) as root:
 *   eglwindow [--front PATH] [--default-display] [--seconds S] [--size WxH] [--pos X,Y] [--interval 0|1] [--no-ahb]
 * --default-display selects eglGetDisplay(EGL_DEFAULT_DISPLAY), as used by
 * Android's EGL loader and ANGLE. The default retains EGL_PLATFORM_ANDROID_KHR.
 * --angle-window leaves native-window connection/format/dataspace ownership to
 * Android's Vulkan runtime, matching AOSP egl_platform_entries.cpp's useAngle branch.
 * PATH defaults to /vendor/lib64/egl/libEGL_nvnative.so (/vendor/lib/egl/... for the 32-bit build), where the
 * container stages it next to ANGLE (waydroid/nvidia_native.py).
 *
 * The picture (top-left origin, as the compositor shows it):
 *   rows [0, H/4)          yellow band (255,255,0): proves the orientation;
 *   rows [H/4, H), x < W/2 the frame colour, cycling every frame through 6 saturated colours;
 *   rows [H/4, H), x >= W/2 a static 32-pixel black/white checkerboard whose top-left cell is white.
 * A torn or garbage frame shows up in a screencap as a mixed colour region or a broken pattern.
 * Exit status = number of failed checks. */
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nvn_android.h>
#include "sf_client.h" /* after nvn_android.h: needs the complete struct ANativeWindow */

using namespace android;

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
#define DIE(...) do { printf("FATAL: " __VA_ARGS__); printf("\n"); exit(100); } while (0)

static void *front;
static __eglMustCastToProperFunctionPointerType (*p_eglGetProcAddress)(const char *);
static void *sym(const char *n) {
    void *p = dlsym(front, n); /* Loader.cpp: dlsym, then the driver's eglGetProcAddress */
    if (!p) p = (void *)p_eglGetProcAddress(n);
    if (!p) DIE("front resolves neither dlsym nor eglGetProcAddress(\"%s\")", n);
    return p;
}
#define EGLFN(X) \
    X(PFNEGLGETDISPLAYPROC, eglGetDisplay) \
    X(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay) X(PFNEGLINITIALIZEPROC, eglInitialize) \
    X(PFNEGLTERMINATEPROC, eglTerminate) X(PFNEGLQUERYSTRINGPROC, eglQueryString) \
    X(PFNEGLCHOOSECONFIGPROC, eglChooseConfig) X(PFNEGLGETCONFIGATTRIBPROC, eglGetConfigAttrib) \
    X(PFNEGLCREATEWINDOWSURFACEPROC, eglCreateWindowSurface) X(PFNEGLDESTROYSURFACEPROC, eglDestroySurface) \
    X(PFNEGLCREATECONTEXTPROC, eglCreateContext) X(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext) \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLSWAPBUFFERSPROC, eglSwapBuffers) \
    X(PFNEGLSWAPINTERVALPROC, eglSwapInterval) X(PFNEGLQUERYSURFACEPROC, eglQuerySurface) \
    X(PFNEGLGETERRORPROC, eglGetError) X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) \
    X(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) X(PFNEGLRELEASETHREADPROC, eglReleaseThread)
#define GLFN(X) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARPROC, glClear) X(PFNGLENABLEPROC, glEnable) \
    X(PFNGLDISABLEPROC, glDisable) X(PFNGLSCISSORPROC, glScissor) X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLGETERRORPROC, glGetError) X(PFNGLGETSTRINGPROC, glGetString) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLFINISHPROC, glFinish)
#define DECL(t, n) static t n;
EGLFN(DECL)
GLFN(DECL)
#define RES(t, n) n = (t)sym(#n);

static GLuint shader(GLenum t, const char *s) {
    GLuint x = glCreateShader(t);
    glShaderSource(x, 1, &s, NULL);
    glCompileShader(x);
    GLint ok = 0;
    glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024] = ""; glGetShaderInfoLog(x, sizeof(l), NULL, l); DIE("shader: %s", l); }
    return x;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) DIE("link failed");
    return p;
}
static const char *VS = "#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));"
                        "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
/* checkerboard in window coordinates with a top-left origin: cell (0,0) at the window's top-left is white */
static const char *FS_CHECK = "#version 300 es\nprecision highp float;\nuniform float h;\nout vec4 o;\nvoid main(){"
                              "ivec2 c=ivec2(gl_FragCoord.x, h-gl_FragCoord.y)/32;o=((c.x+c.y)&1)==0?vec4(1):vec4(0,0,0,1);}\n";

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef void *(*load_sphal_fn)(const char *, int);

static void ahb_tests(EGLDisplay dpy, EGLContext ctx) {
    /* (a) CPU-written AHB (gralloc: LINEAR) sampled as GL_TEXTURE_EXTERNAL_OES */
    const int iw = 64, ih = 32;
    AHardwareBuffer_Desc d = {};
    d.width = iw;
    d.height = ih;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer *ahb = NULL;
    int r = AHardwareBuffer_allocate(&d, &ahb);
    CHECK(r == 0, "AHardwareBuffer_allocate(CPU_WRITE|GPU_SAMPLED) = %d", r);
    if (r != 0) return;
    AHardwareBuffer_describe(ahb, &d);
    void *addr = NULL;
    r = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &addr);
    CHECK(r == 0 && addr, "AHardwareBuffer_lock = %d", r);
    if (r == 0) {
        for (int y = 0; y < ih; y++)
            for (int x = 0; x < iw; x++) {
                uint8_t *p = (uint8_t *)addr + ((size_t)y * d.stride + x) * 4;
                p[0] = (uint8_t)(x * 4); p[1] = (uint8_t)(y * 8); p[2] = (uint8_t)(x ^ y); p[3] = 255;
            }
        AHardwareBuffer_unlock(ahb, NULL);
    }
    ANativeWindowBuffer *anwb = AHardwareBuffer_to_ANativeWindowBuffer(ahb);
    EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)anwb, ia);
    CHECK(img != EGL_NO_IMAGE_KHR, "EGL_NATIVE_BUFFER_ANDROID on a CPU-written AHB: 0x%x", eglGetError());
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    if (img) {
        GLuint tex, fbo, rb;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
        CHECK(glGetError() == GL_NO_ERROR, "glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES)");
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, iw, ih);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        GLuint prog = program(VS, "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;\n"
                                  "uniform samplerExternalOES t;\nout vec4 o;\nvoid main(){o=texelFetch(t, ivec2(gl_FragCoord.xy), 0);}\n");
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "t"), 0);
        glViewport(0, 0, iw, ih);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        static uint8_t px[64 * 32 * 4];
        glReadPixels(0, 0, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int bad = 0;
        for (int y = 0; y < ih; y++)
            for (int x = 0; x < iw; x++) {
                const uint8_t *p = px + 4 * (y * iw + x);
                if (p[0] != (uint8_t)(x * 4) || p[1] != (uint8_t)(y * 8) || p[2] != (uint8_t)(x ^ y) || p[3] != 255) bad++;
            }
        printf("AHB CPU_WRITE|GPU_SAMPLED %dx%d (stride %u px) -> EGLImage -> GL_TEXTURE_EXTERNAL_OES: %d of %d texels wrong; "
               "(5,7) = %u,%u,%u,%u\n", iw, ih, d.stride, bad, iw * ih, px[4 * (7 * iw + 5)], px[4 * (7 * iw + 5) + 1],
               px[4 * (7 * iw + 5) + 2], px[4 * (7 * iw + 5) + 3]);
        CHECK(bad == 0, "CPU-written AHB contents through EGL_ANDROID_image_native_buffer");
        /* the same LINEAR image as GL_TEXTURE_2D: NVIDIA imports LINEAR dma-bufs external-only (contract §3) */
        GLuint t2;
        glGenTextures(1, &t2);
        glBindTexture(GL_TEXTURE_2D, t2);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        GLenum e2 = glGetError();
        printf("same LINEAR image as GL_TEXTURE_2D: GL error 0x%04x (driver: LINEAR is external-only)\n", e2);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        eglDestroyImageKHR(dpy, img);
    }
    AHardwareBuffer_release(ahb);

    /* (b) GPU AHB (gralloc: block-linear) as a GL_TEXTURE_2D render target, then sampled back as GL_TEXTURE_2D */
    d = {};
    d.width = iw;
    d.height = ih;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    r = AHardwareBuffer_allocate(&d, &ahb);
    CHECK(r == 0, "AHardwareBuffer_allocate(GPU_COLOR_OUTPUT|GPU_SAMPLED) = %d", r);
    if (r != 0) return;
    anwb = AHardwareBuffer_to_ANativeWindowBuffer(ahb);
    img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)anwb, ia);
    CHECK(img != EGL_NO_IMAGE_KHR, "EGL_NATIVE_BUFFER_ANDROID on a GPU AHB: 0x%x", eglGetError());
    EGLImageKHR bad = eglCreateImageKHR(dpy, ctx, EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer)anwb, ia);
    CHECK(bad == EGL_NO_IMAGE_KHR && eglGetError() == EGL_BAD_CONTEXT, "a context with EGL_NATIVE_BUFFER_ANDROID must fail");
    if (img) {
        GLuint tex, fbo;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        CHECK(st == GL_FRAMEBUFFER_COMPLETE, "GPU AHB FBO status 0x%x", st);
        glViewport(0, 0, iw, ih);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.25f, 0.5f, 0.75f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 8, 8);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        uint8_t a[4], b[4];
        glReadPixels(20, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, a);
        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
        printf("AHB GPU_COLOR_OUTPUT|GPU_SAMPLED -> EGLImage -> GL_TEXTURE_2D render target: (20,20) = %u,%u,%u,%u, "
               "(2,2) = %u,%u,%u,%u\n", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        CHECK(a[0] == 64 && (a[1] == 127 || a[1] == 128) && a[2] == 191 && b[0] == 255 && b[1] == 0, "GPU AHB readback");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        eglDestroyImageKHR(dpy, img);
    }
    AHardwareBuffer_release(ahb);
}

int main(int argc, char **argv) {
#if defined(__LP64__)
    const char *frontp = "/vendor/lib64/egl/libEGL_nvnative.so";
#else
    const char *frontp = "/vendor/lib/egl/libEGL_nvnative.so";
#endif
    double seconds = 10;
    int W = 640, H = 400, X = 64, Y = 64, interval = 1, do_ahb = 1;
    bool default_display = false;
    bool angle_window = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--front") && i + 1 < argc) frontp = argv[++i];
        else if (!strcmp(argv[i], "--default-display")) default_display = true;
        else if (!strcmp(argv[i], "--angle-window")) angle_window = true;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--pos") && i + 1 < argc) sscanf(argv[++i], "%d,%d", &X, &Y);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-ahb")) do_ahb = 0;
        else DIE("usage: eglwindow [--front PATH] [--default-display] [--angle-window] [--seconds S] [--size WxH] [--pos X,Y] [--interval 0|1] [--no-ahb]");
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("eglwindow: pid %d, %s, front %s\n", (int)getpid(), sizeof(void *) == 8 ? "64-bit" : "32-bit", frontp);

    /* a SurfaceFlinger layer (BLAST: the Surface's consumer is a BLASTBufferQueue in this process, whose buffer
     * releases arrive over binder, hence the thread pool) */
    ProcessState::self().get()->startThreadPool();
    SurfaceComposerClient *client = new SurfaceComposerClient();
    reinterpret_cast<RefBase *>(client)->incStrong(client); /* onFirstRef() connects to SurfaceFlinger */
    CHECK(client->initCheck() == 0, "SurfaceComposerClient::initCheck = %d", client->initCheck());
    sp<SurfaceControl> sc = client->createSurface(String8("nvnative-eglwindow"), (uint32_t)W, (uint32_t)H,
                                                  NVN_HAL_PIXEL_FORMAT_RGBA_8888,
                                                  SurfaceComposerClient::eFXSurfaceBufferState, sp<IBinder>(),
                                                  LayerMetadata(), nullptr);
    if (!sc.get()) DIE("createSurface failed");
    SurfaceComposerClient::Transaction *t = new SurfaceComposerClient::Transaction();
    t->setLayerStack(sc, ui::LayerStack{0}).setLayer(sc, 0x7ffffff0).setPosition(sc, (float)X, (float)Y).show(sc);
    CHECK(t->apply(false, false) == 0, "Transaction::apply");
    Surface *surface = sc.get()->getSurface().get();
    if (!surface) DIE("SurfaceControl::getSurface returned NULL");
    struct ANativeWindow *win = static_cast<struct ANativeWindow *>(surface);
    if (win->common.magic != (int)NVN_ANDROID_NATIVE_WINDOW_MAGIC || win->common.version != (int)sizeof(struct ANativeWindow))
        DIE("Surface %p: the ANativeWindow at %p has magic 0x%x version %d (sf_client.h no longer matches libgui)", (void *)surface,
            (void *)win, (unsigned)win->common.magic, win->common.version);
    printf("layer \"nvnative-eglwindow\" %dx%d at %d,%d, z 0x7ffffff0, ANativeWindow %p\n", W, H, X, Y, (void *)win);

    /* load OUR front the way Loader.cpp loads a vendor EGL driver: android_load_sphal_library */
    void *vndk = dlopen("libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
    if (!vndk) DIE("dlopen libvndksupport.so: %s", dlerror());
    load_sphal_fn load = (load_sphal_fn)dlsym(vndk, "android_load_sphal_library");
    if (!load) DIE("no android_load_sphal_library");
    front = load(frontp, RTLD_NOW | RTLD_LOCAL);
    if (!front) DIE("android_load_sphal_library(%s): %s", frontp, dlerror());
    p_eglGetProcAddress = (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(front, "eglGetProcAddress");
    if (!p_eglGetProcAddress) DIE("front exports no eglGetProcAddress");
    EGLFN(RES)
    GLFN(RES)

    printf("display selection: %s\n", default_display ? "eglGetDisplay(EGL_DEFAULT_DISPLAY)" : "EGL_PLATFORM_ANDROID_KHR");
    EGLDisplay dpy = default_display ? eglGetDisplay(EGL_DEFAULT_DISPLAY) :
        eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj = 0, min = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) DIE("display/initialize 0x%x", eglGetError());
    const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
    printf("EGL %d.%d %s; Android extensions:", maj, min, eglQueryString(dpy, EGL_VENDOR));
    for (const char *n = "EGL_ANDROID_image_native_buffer\0EGL_ANDROID_native_fence_sync\0EGL_ANDROID_recordable\0"
                         "EGL_ANDROID_framebuffer_target\0EGL_KHR_wait_sync\0EGL_EXT_buffer_age\0EGL_KHR_partial_update\0"
                         "EGL_KHR_no_config_context\0EGL_KHR_surfaceless_context\0EGL_IMG_context_priority\0";
         *n; n += strlen(n) + 1) {
        bool has = strstr(ext, n) != NULL;
        printf(" %s=%s", n, has ? "yes" : "NO");
        CHECK(has, "display extensions lack %s", n);
    }
    printf("\n");

    /* the loader's ANativeWindow steps (egl_platform_entries.cpp:657-743) */
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
                         EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RECORDABLE_ANDROID, EGL_TRUE, EGL_NONE};
    EGLConfig cfg;
    EGLint nc = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &nc) || nc < 1) DIE("eglChooseConfig 0x%x", eglGetError());
    EGLint vid = 0, ct = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &vid);
    CHECK(eglGetConfigAttrib(dpy, cfg, EGL_COLOR_COMPONENT_TYPE_EXT, &ct), "EGL_COLOR_COMPONENT_TYPE_EXT");
    int valid = 0;
    win->query(win, NVN_NATIVE_WINDOW_IS_VALID, &valid);
    CHECK(valid == 1, "NATIVE_WINDOW_IS_VALID = %d", valid);
    if (!angle_window) {
        CHECK(win->perform(win, NVN_NATIVE_WINDOW_API_CONNECT, NVN_NATIVE_WINDOW_API_EGL) == 0, "api_connect(EGL)");
        CHECK(win->perform(win, NVN_NATIVE_WINDOW_SET_BUFFERS_FORMAT, NVN_HAL_PIXEL_FORMAT_RGBA_8888) == 0, "set format");
        CHECK(win->perform(win, NVN_NATIVE_WINDOW_SET_BUFFERS_DATASPACE, 0) == 0, "set dataspace");
    } else {
        printf("native-window lifecycle: Android Vulkan runtime (ANGLE)\n");
    }
    win->setSwapInterval(win, 1);
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, win, NULL);
    if (surf == EGL_NO_SURFACE) DIE("eglCreateWindowSurface 0x%x", eglGetError());
    const EGLint cxa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cxa);
    if (!ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) DIE("context/makecurrent 0x%x", eglGetError());
    printf("config %p visual %d component 0x%x; GL_RENDERER \"%s\", GL_VERSION \"%s\"\n", cfg, vid, ct,
           glGetString(GL_RENDERER), glGetString(GL_VERSION));

    if (do_ahb) ahb_tests(dpy, ctx);
    eglMakeCurrent(dpy, surf, surf, ctx);
    CHECK(eglSwapInterval(dpy, interval), "eglSwapInterval(%d) 0x%x", interval, eglGetError());

    GLuint cprog = program(VS, FS_CHECK);
    GLint uh = glGetUniformLocation(cprog, "h");
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    static const float PAL[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}};
    double t0 = now(), tlast = t0;
    long frames = 0, frames_last = 0;
    int ages[8] = {0};
    while (now() - t0 < seconds) {
        EGLint sw = 0, sh = 0, age = -1;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
        eglQuerySurface(dpy, surf, EGL_BUFFER_AGE_EXT, &age);
        ages[age >= 0 && age < 7 ? age : 7]++;
        const float *c = PAL[frames % 6];
        glViewport(0, 0, sw, sh);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(c[0], c[1], c[2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(sw / 2, 0, sw - sw / 2, sh - sh / 4); /* right half below the band: checkerboard */
        glUseProgram(cprog);
        glUniform1f(uh, (float)sh);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glScissor(0, sh - sh / 4, sw, sh / 4); /* GL coordinates: the top quarter */
        glClearColor(1, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        if (frames == 0) {
            uint8_t p[4];
            glReadPixels(sw / 4, sh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
            printf("first frame %dx%d: GL readback of the colour region %u,%u,%u,%u\n", sw, sh, p[0], p[1], p[2], p[3]);
        }
        CHECK(eglSwapBuffers(dpy, surf), "frame %ld: eglSwapBuffers 0x%x", frames, eglGetError());
        frames++;
        double tn = now();
        if (tn - tlast >= 1.0) {
            printf("t=%.0fs frames %ld (%.1f fps)\n", tn - t0, frames, (frames - frames_last) / (tn - tlast));
            tlast = tn;
            frames_last = frames;
        }
        if (fails > 20) break;
    }
    double dt = now() - t0;
    printf("rendered %ld frames in %.1f s = %.1f fps (swap interval %d); buffer ages:", frames, dt, frames / dt, interval);
    for (int i = 0; i < 8; i++) if (ages[i]) printf(" %s%d x%d", i == 7 ? ">=" : "", i, ages[i]);
    printf("\n");
    CHECK(glGetError() == GL_NO_ERROR, "GL error at the end");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    CHECK(eglDestroySurface(dpy, surf), "eglDestroySurface 0x%x", eglGetError());
    eglDestroyContext(dpy, ctx);
    if (!angle_window) win->perform(win, NVN_NATIVE_WINDOW_API_DISCONNECT, NVN_NATIVE_WINDOW_API_EGL);
    eglTerminate(dpy);
    eglReleaseThread();
    printf("eglwindow: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    fflush(stdout);
    _exit(fails); /* no global destructors: the sf_client.h objects are deliberately never destroyed */
}
