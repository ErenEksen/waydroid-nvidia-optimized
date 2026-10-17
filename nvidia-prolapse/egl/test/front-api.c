/* front-api: the EGL API surface the front + platform give Android apps (host, real GPU).
 *
 *   front-api --front out/host/libEGL_nvnative.so --platform-lib out/host/libnvnative-egl-android.so
 *
 * 1. OpenGL ES only (front.c "client APIs"): for the Android platform display (our platform) and an NVIDIA device
 *    display, both through the front: EGL_CLIENT_APIS == "OpenGL_ES"; eglBindAPI(EGL_OPENGL_API) fails with
 *    EGL_BAD_PARAMETER and leaves EGL_OPENGL_ES_API bound; no config reports EGL_OPENGL_BIT in EGL_RENDERABLE_TYPE or
 *    EGL_CONFORMANT, every config still reports an ES bit; eglChooseConfig for EGL_OPENGL_BIT (renderable or
 *    conformant) matches nothing, for ES2/ES3 matches every config; an invalid attribute still fails with the
 *    driver's EGL_BAD_ATTRIBUTE; eglGetProcAddress("eglBindAPI") is the front's.
 * 2. The platform base's pass-through to the driver for surfaces the platform does not own (egl-x11 v1.0.6-5: c83c2e8,
 *    ec2d477; dEQP-EGL negative_api.swap_interval, negative_partial_update.not_postable_surface), Android platform
 *    display: eglSwapInterval with no current context -> EGL_BAD_CONTEXT (EGL 1.5 §3.10.3); with a pbuffer current ->
 *    EGL_TRUE; eglSetDamageRegionKHR on a pbuffer -> EGL_BAD_MATCH (EGL_KHR_partial_update). Also printed, not
 *    asserted (the driver's answer, not tested by dEQP): a context of another display current, a surfaceless context.
 * Exit status = number of failed checks. */

#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_ANDROID_KHR
#define EGL_PLATFORM_ANDROID_KHR 0x3141
#endif

static int fails;
#define CHECK(c, ...)                                                                                              \
    do {                                                                                                           \
        if (c) {                                                                                                   \
            printf("  ok   ");                                                                                     \
        } else {                                                                                                   \
            printf("  FAIL ");                                                                                     \
            fails++;                                                                                               \
        }                                                                                                          \
        printf(__VA_ARGS__);                                                                                       \
        printf("\n");                                                                                              \
    } while (0)
#define DIE(...) do { printf("FATAL: " __VA_ARGS__); printf("\n"); exit(100); } while (0)

static void *front;
static __eglMustCastToProperFunctionPointerType (*gpa)(const char *);
static void *sym(const char *n) {
    void *p = dlsym(front, n);
    if (!p) p = (void *)gpa(n);
    if (!p) DIE("no %s", n);
    return p;
}
#define FN(X)                                                                                                      \
    X(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay) X(PFNEGLINITIALIZEPROC, eglInitialize)                  \
    X(PFNEGLTERMINATEPROC, eglTerminate) X(PFNEGLQUERYDEVICESEXTPROC, eglQueryDevicesEXT)                          \
    X(PFNEGLQUERYSTRINGPROC, eglQueryString) X(PFNEGLBINDAPIPROC, eglBindAPI) X(PFNEGLQUERYAPIPROC, eglQueryAPI)    \
    X(PFNEGLGETCONFIGSPROC, eglGetConfigs) X(PFNEGLGETCONFIGATTRIBPROC, eglGetConfigAttrib)                        \
    X(PFNEGLCHOOSECONFIGPROC, eglChooseConfig) X(PFNEGLGETERRORPROC, eglGetError)                                  \
    X(PFNEGLCREATECONTEXTPROC, eglCreateContext) X(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext)                    \
    X(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface) X(PFNEGLDESTROYSURFACEPROC, eglDestroySurface)      \
    X(PFNEGLMAKECURRENTPROC, eglMakeCurrent) X(PFNEGLSWAPINTERVALPROC, eglSwapInterval)
#define DECL(t, n) static t n;
FN(DECL)
#define RES(t, n) n = (t)sym(#n);

static void api_checks(EGLDisplay d, const char *what) {
    printf("[%s]\n", what);
    const char *apis = eglQueryString(d, EGL_CLIENT_APIS);
    CHECK(apis && !strcmp(apis, "OpenGL_ES"), "EGL_CLIENT_APIS \"%s\" == \"OpenGL_ES\"", apis ? apis : "(null)");
    CHECK(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI(EGL_OPENGL_ES_API)");
    EGLBoolean b = eglBindAPI(EGL_OPENGL_API);
    EGLint e = eglGetError();
    CHECK(!b && e == EGL_BAD_PARAMETER, "eglBindAPI(EGL_OPENGL_API) = %d, error 0x%x (want EGL_FALSE, EGL_BAD_PARAMETER)",
          b, e);
    CHECK(eglQueryAPI() == EGL_OPENGL_ES_API, "eglQueryAPI() still EGL_OPENGL_ES_API (0x%x)", eglQueryAPI());
    b = eglBindAPI(0xfdfdfdfd);
    e = eglGetError();
    CHECK(!b && e == EGL_BAD_PARAMETER, "eglBindAPI(0xfdfdfdfd): driver's EGL_BAD_PARAMETER (0x%x)", e);

    EGLint n = 0;
    eglGetConfigs(d, NULL, 0, &n);
    EGLConfig *c = calloc((size_t)n, sizeof(*c));
    eglGetConfigs(d, c, n, &n);
    int gl = 0, noes = 0, es23 = 0;
    for (EGLint i = 0; i < n; i++) {
        EGLint r = 0, cf = 0;
        eglGetConfigAttrib(d, c[i], EGL_RENDERABLE_TYPE, &r);
        eglGetConfigAttrib(d, c[i], EGL_CONFORMANT, &cf);
        if ((r | cf) & EGL_OPENGL_BIT) gl++;
        if (!(r & EGL_OPENGL_ES2_BIT) || !(cf & EGL_OPENGL_ES2_BIT)) noes++;
        if ((r & (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT)) == (EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT)) es23++;
    }
    CHECK(n > 0 && gl == 0, "%d configs, %d report EGL_OPENGL_BIT (renderable or conformant)", n, gl);
    CHECK(noes == 0, "every config renders and conforms to ES2 (%d do not)", noes);
    /* EGL_SURFACE_TYPE defaults to EGL_WINDOW_BIT: DONT_CARE so that every config is a candidate */
    const EGLint want_gl[][5] = {{EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_NONE},
                                 {EGL_CONFORMANT, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_DONT_CARE, EGL_NONE},
                                 {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE,
                                  EGL_DONT_CARE, EGL_NONE}};
    for (int i = 0; i < 3; i++) {
        EGLint m = -1;
        b = eglChooseConfig(d, want_gl[i], c, n, &m);
        CHECK(b && m == 0, "eglChooseConfig(0x%x = 0x%x) matches nothing (%d, %d)", want_gl[i][0], want_gl[i][1], b, m);
    }
    /* EGL_COLOR_COMPONENT_TYPE_EXT defaults to FIXED: DONT_CARE keeps the float configs */
    const EGLint es[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_DONT_CARE,
                         EGL_COLOR_COMPONENT_TYPE_EXT, EGL_DONT_CARE, EGL_NONE};
    EGLint m = 0;
    b = eglChooseConfig(d, es, NULL, 0, &m);
    CHECK(b && m == es23, "eglChooseConfig(ES2|ES3) matches the %d configs that render both (%d)", es23, m);
    const EGLint bad[] = {0x7fff, 1, EGL_NONE};
    b = eglChooseConfig(d, bad, c, n, &m);
    e = eglGetError();
    CHECK(!b && e == EGL_BAD_ATTRIBUTE, "eglChooseConfig(invalid attribute): driver's EGL_BAD_ATTRIBUTE (0x%x)", e);
    free(c);
}

int main(int argc, char **argv) {
    const char *frontp = NULL, *platlib = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--front") && i + 1 < argc) frontp = argv[++i];
        else if (!strcmp(argv[i], "--platform-lib") && i + 1 < argc) platlib = argv[++i];
        else DIE("usage: front-api --front libEGL_nvnative.so --platform-lib libnvnative-egl-android.so");
    }
    if (!frontp || !platlib) DIE("--front and --platform-lib are required");
    setvbuf(stdout, NULL, _IOLBF, 0);
    char json[] = "/tmp/front-api-XXXXXX.json";
    int jf = mkstemps(json, 5);
    char abs[4096];
    if (jf < 0 || !realpath(platlib, abs)) DIE("json/realpath");
    dprintf(jf, "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"%s\"}}\n", abs);
    close(jf);
    setenv("__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES", json, 1);
    if (!(front = dlopen(frontp, RTLD_NOW | RTLD_LOCAL))) DIE("dlopen %s: %s", frontp, dlerror());
    gpa = (__eglMustCastToProperFunctionPointerType(*)(const char *))dlsym(front, "eglGetProcAddress");
    if (!gpa) DIE("front exports no eglGetProcAddress");
    FN(RES)
    CHECK((void *)gpa("eglBindAPI") == dlsym(front, "eglBindAPI") &&
              (void *)gpa("eglChooseConfig") == dlsym(front, "eglChooseConfig") &&
              (void *)gpa("eglGetConfigAttrib") == dlsym(front, "eglGetConfigAttrib"),
          "eglGetProcAddress returns the front's eglBindAPI/eglChooseConfig/eglGetConfigAttrib");

    EGLDisplay ad = eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, EGL_DEFAULT_DISPLAY, NULL);
    if (ad == EGL_NO_DISPLAY || !eglInitialize(ad, NULL, NULL)) DIE("Android platform display: 0x%x", eglGetError());
    EGLDeviceEXT dev;
    EGLint nd = 0;
    eglQueryDevicesEXT(1, &dev, &nd);
    EGLDisplay dd = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
    if (dd == EGL_NO_DISPLAY || !eglInitialize(dd, NULL, NULL)) DIE("device display: 0x%x", eglGetError());
    api_checks(ad, "Android platform display (our platform)");
    api_checks(dd, "NVIDIA device display");

    printf("[eglSwapInterval / eglSetDamageRegionKHR errors, Android platform display]\n");
    eglMakeCurrent(ad, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    EGLBoolean b = eglSwapInterval(ad, 0);
    EGLint e = eglGetError();
    CHECK(!b && e == EGL_BAD_CONTEXT, "no current context: %d, 0x%x (want EGL_BAD_CONTEXT)", b, e);
    const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    const EGLint cx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    const EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLConfig cfg, dcfg;
    EGLint m = 0;
    eglChooseConfig(dd, ca, &dcfg, 1, &m);
    EGLContext dctx = eglCreateContext(dd, dcfg, EGL_NO_CONTEXT, cx);
    if (!eglMakeCurrent(dd, EGL_NO_SURFACE, EGL_NO_SURFACE, dctx)) DIE("device display make-current 0x%x", eglGetError());
    b = eglSwapInterval(ad, 0);
    e = eglGetError();
    printf("  info a context of another display current: %d, 0x%x (driver)\n", b, e);
    eglMakeCurrent(dd, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dd, dctx);
    eglChooseConfig(ad, ca, &cfg, 1, &m);
    EGLContext ctx = eglCreateContext(ad, cfg, EGL_NO_CONTEXT, cx);
    if (!eglMakeCurrent(ad, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) DIE("surfaceless make-current 0x%x", eglGetError());
    b = eglSwapInterval(ad, 0);
    e = eglGetError();
    printf("  info surfaceless context: %d, 0x%x (driver; EGL 1.5 says EGL_BAD_SURFACE)\n", b, e);
    EGLSurface pb = eglCreatePbufferSurface(ad, cfg, pa);
    if (pb == EGL_NO_SURFACE || !eglMakeCurrent(ad, pb, pb, ctx)) DIE("pbuffer make-current 0x%x", eglGetError());
    b = eglSwapInterval(ad, 0);
    e = eglGetError();
    CHECK(b && e == EGL_SUCCESS, "pbuffer current: %d, 0x%x (want EGL_TRUE)", b, e);
    PFNEGLSETDAMAGEREGIONKHRPROC sdr = (PFNEGLSETDAMAGEREGIONKHRPROC)sym("eglSetDamageRegionKHR");
    EGLint rect[4] = {0, 0, 1, 1};
    b = sdr(ad, pb, rect, 1);
    e = eglGetError();
    CHECK(!b && e == EGL_BAD_MATCH, "eglSetDamageRegionKHR(pbuffer): %d, 0x%x (want EGL_FALSE, EGL_BAD_MATCH)", b, e);
    eglMakeCurrent(ad, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(ad, pb);
    eglDestroyContext(ad, ctx);
    eglTerminate(ad);
    eglTerminate(dd);
    unlink(json);
    printf("front-api: %s (%d failure(s))\n", fails ? "FAILED" : "PASS", fails);
    return fails;
}
