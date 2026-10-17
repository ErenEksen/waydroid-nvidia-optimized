/* libEGL_nvnative.so: the Android vendor EGL library over NVIDIA's glvnd vendor library, with no libglvnd.
 *
 * Android's loader (frameworks/native opengl/libs/EGL/Loader.cpp:600-668, android13-release) loads
 * lib{EGL,GLESv1_CM,GLESv2}_<ro.hardware.egl>.so through the sphal namespace, dlsym()s every name of
 * egl_entries.in from this library and asks its eglGetProcAddress for the rest, GL included.
 * This library is that EGL library and nothing more:
 *   - it hosts NVIDIA's vendor ABI itself: dlopen("libEGL_nvidia.so.0") by soname from its own namespace,
 *     __egl_Main(0.2) with our __EGLapiExports (libglvnd v1.7.0 include/glvnd/libeglabi.h), as the G7a probe
 *     nvidia-prolapse/probe/eglgles.c proved (NVIDIA calls none of the export callbacks on this path);
 *   - every exported EGL name forwards to the function the vendor's getProcAddress returns; GL names are
 *     answered by the same getProcAddress (NVIDIA returns directly callable GL functions, NV-EGL-CONTRACT §1.4);
 *   - eglGetDisplay(d) means eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, d): the Android window system is
 *     NVIDIA's external platform libnvnative-egl-android.so, which the driver finds through the shims' default
 *     environment (nvglibc-default-env). If no platform claims Android, the display call fails loudly.
 * The Android-specific window and image logic lives in the platform library, not here. */

#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glvnd/libeglabi.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#include "front_entries.h"

#define NVN_EXPORT __attribute__((visibility("default")))
static const char ICD_SONAME[] = "libEGL_nvidia.so.0";

static void flog(int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void flog(int error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_write(error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "nvnative-egl", buf);
#endif
    if (error || getenv("NVNATIVE_EGL_DEBUG") != NULL) {
        fprintf(stderr, "libEGL_nvnative: %s\n", buf);
    }
}

static _Noreturn void die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static _Noreturn void die(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    flog(1, "FATAL: %s", buf);
    abort();
}

/* ---------------- the vendor ---------------- */

struct __EGLvendorInfoRec {
    const char *name;
};
static struct __EGLvendorInfoRec g_vendor = {"nvidia"};
static __EGLapiImports g_im;
static int g_ok;
static char g_fail[768];
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* glvnd's per-thread error for errors the front itself raises or the vendor reports through
 * exports->setEGLError (libegl.c:822 semantics: the last call decides what eglGetError returns). */
static __thread EGLint t_err;
static __thread int t_err_set;

#define DECL(ret, name, params, args) static ret(*v_##name) params;
NVN_FRONT_FORWARDED(DECL)
#undef DECL
static EGLint (*v_eglGetError)(void);
static const char *(*v_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*v_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*v_eglBindAPI)(EGLenum);
static EGLBoolean (*v_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*v_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);

/* __EGLapiExports. Measured: NVIDIA calls none of them on the getProcAddress path (eglgles, G7a); each one is
 * still answered with glvnd's meaning for a single vendor whose current state is the vendor's own. */
static void ex_threadInit(void) {}
static EGLenum ex_getCurrentApi(void) { return v_eglQueryAPI ? v_eglQueryAPI() : EGL_OPENGL_ES_API; }
static __EGLvendorInfo *ex_getCurrentVendor(void) {
    return v_eglGetCurrentContext && v_eglGetCurrentContext() != EGL_NO_CONTEXT ? &g_vendor : NULL;
}
static EGLContext ex_getCurrentContext(void) { return v_eglGetCurrentContext ? v_eglGetCurrentContext() : EGL_NO_CONTEXT; }
static EGLDisplay ex_getCurrentDisplay(void) { return v_eglGetCurrentDisplay ? v_eglGetCurrentDisplay() : EGL_NO_DISPLAY; }
static EGLSurface ex_getCurrentSurface(EGLint rd) { return v_eglGetCurrentSurface ? v_eglGetCurrentSurface(rd) : EGL_NO_SURFACE; }
static __eglMustCastToProperFunctionPointerType ex_fetchDispatchEntry(__EGLvendorInfo *v, int index) {
    (void)v;
    /* Only the vendor's dispatch stubs (imports.getDispatchAddress) call this, for an index set through
     * imports.setDispatchIndex; this front hands out neither. */
    die("vendor called exports->fetchDispatchEntry(%d), but no dispatch index was ever assigned", index);
}
static void ex_setEGLError(EGLint e) {
    t_err = e;
    t_err_set = 1;
}
static EGLBoolean ex_setLastVendor(__EGLvendorInfo *v) {
    (void)v;
    t_err_set = 0;
    return EGL_TRUE;
}
static __EGLvendorInfo *ex_getVendorFromDisplay(EGLDisplay d) { return d != EGL_NO_DISPLAY ? &g_vendor : NULL; }
static __EGLvendorInfo *ex_getVendorFromDevice(EGLDeviceEXT d) { return d != EGL_NO_DEVICE_EXT ? &g_vendor : NULL; }
static EGLBoolean ex_setVendorForDevice(EGLDeviceEXT d, __EGLvendorInfo *v) {
    (void)d;
    (void)v;
    return EGL_TRUE;
}
static const __EGLapiExports g_exports = {
    .threadInit = ex_threadInit, .getCurrentApi = ex_getCurrentApi, .getCurrentVendor = ex_getCurrentVendor,
    .getCurrentContext = ex_getCurrentContext, .getCurrentDisplay = ex_getCurrentDisplay,
    .getCurrentSurface = ex_getCurrentSurface, .fetchDispatchEntry = ex_fetchDispatchEntry,
    .setEGLError = ex_setEGLError, .setLastVendor = ex_setLastVendor,
    .getVendorFromDisplay = ex_getVendorFromDisplay, .getVendorFromDevice = ex_getVendorFromDevice,
    .setVendorForDevice = ex_setVendorForDevice,
};

static void front_init(void) {
    void *h = dlopen(ICD_SONAME, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        const char *e = dlerror();
        snprintf(g_fail, sizeof(g_fail), "dlopen(\"%s\") failed: %s. Remedy: NVIDIA's EGL library and its glibc shims "
                 "must be in this namespace's search path (sphal: /vendor_extra/lib64 or /vendor_extra/lib)",
                 ICD_SONAME, e ? e : "(no dlerror)");
        flog(1, "%s", g_fail);
        return;
    }
    __PFNEGLMAINPROC egl_main = (__PFNEGLMAINPROC)dlsym(h, __EGL_MAIN_PROTO_NAME);
    if (egl_main == NULL) {
        snprintf(g_fail, sizeof(g_fail), "%s exports no %s: not a glvnd EGL vendor library", ICD_SONAME,
                 __EGL_MAIN_PROTO_NAME);
        flog(1, "%s", g_fail);
        return;
    }
    if (!egl_main(EGL_VENDOR_ABI_VERSION, &g_exports, &g_vendor, &g_im)) {
        snprintf(g_fail, sizeof(g_fail), "%s refused __egl_Main(ABI %d.%d)", ICD_SONAME, EGL_VENDOR_ABI_MAJOR_VERSION,
                 EGL_VENDOR_ABI_MINOR_VERSION);
        flog(1, "%s", g_fail);
        return;
    }
    if (g_im.getPlatformDisplay == NULL || g_im.getSupportsAPI == NULL || g_im.getProcAddress == NULL) {
        snprintf(g_fail, sizeof(g_fail), "%s left getPlatformDisplay/getSupportsAPI/getProcAddress NULL", ICD_SONAME);
        flog(1, "%s", g_fail);
        return;
    }
    if (!g_im.getSupportsAPI(EGL_OPENGL_ES_API)) {
        snprintf(g_fail, sizeof(g_fail), "%s does not support EGL_OPENGL_ES_API", ICD_SONAME);
        flog(1, "%s", g_fail);
        return;
    }
#define RESOLVE(ret, name, params, args)                                                                       \
    if ((*(void **)&v_##name = (void *)g_im.getProcAddress(#name)) == NULL) {                                  \
        snprintf(g_fail, sizeof(g_fail), "%s's getProcAddress(\"%s\") is NULL: driver/front mismatch", ICD_SONAME, \
                 #name);                                                                                       \
        flog(1, "%s", g_fail);                                                                                 \
        return;                                                                                                \
    }
    NVN_FRONT_FORWARDED(RESOLVE)
    RESOLVE(EGLint, eglGetError, (void), ())
    RESOLVE(const char *, eglQueryString, (EGLDisplay a0, EGLint a1), (a0, a1))
    RESOLVE(EGLBoolean, eglMakeCurrent, (EGLDisplay a0, EGLSurface a1, EGLSurface a2, EGLContext a3), (a0, a1, a2, a3))
    RESOLVE(EGLBoolean, eglBindAPI, (EGLenum a0), (a0))
    RESOLVE(EGLBoolean, eglChooseConfig, (EGLDisplay a0, const EGLint *a1, EGLConfig *a2, EGLint a3, EGLint *a4), ())
    RESOLVE(EGLBoolean, eglGetConfigAttrib, (EGLDisplay a0, EGLConfig a1, EGLint a2, EGLint *a3), (a0, a1, a2, a3))
#undef RESOLVE
    __atomic_store_n(&g_ok, 1, __ATOMIC_RELEASE);
    flog(0, "vendor %s loaded through __egl_Main %d.%d", ICD_SONAME, EGL_VENDOR_ABI_MAJOR_VERSION,
         EGL_VENDOR_ABI_MINOR_VERSION);
}

static int ready(void) {
    if (__builtin_expect(__atomic_load_n(&g_ok, __ATOMIC_ACQUIRE), 1)) return 1;
    pthread_once(&g_once, front_init);
    return __atomic_load_n(&g_ok, __ATOMIC_ACQUIRE);
}

static void set_error(EGLint e) {
    t_err = e;
    t_err_set = 1;
}

/* ---------------- forwarded entry points ---------------- */

#define FORWARD(ret, name, params, args)                                                                \
    NVN_EXPORT ret name params {                                                                        \
        if (__builtin_expect(!ready(), 0))                                                              \
            die("%s called, but the NVIDIA EGL vendor is not loaded: %s", #name, g_fail);              \
        t_err_set = 0;                                                                                  \
        return v_##name args;                                                                           \
    }
NVN_FRONT_FORWARDED(FORWARD)
#undef FORWARD

/* ---------------- front entry points ---------------- */

/* ---------------- eglMakeCurrent: the thread-timeline start (FENCE-CONTRACT.md §6a) ----------------
 *
 * NVIDIA 615.71.09 keeps one semaphore timeline {V, started} per thread (FENCE-CONTRACT §3.5). It is created inside
 * eglMakeCurrent, when a non-NULL context is bound on a thread that has none: the thread's first bind, and the first
 * bind after eglReleaseThread (measured, ioctl log: one SEMSURF CTX_CREATE per thread, inside that make-current; no
 * new one for surfaceless/no-config or config+pbuffer binds, for a second EGLDisplay of the device, or after
 * eglTerminate + eglInitialize of every display). A GPU wait (eglWaitSync on an imported native fence) advances V
 * without setting `started`; the thread's first fence creation then takes the wait's value and signals with the
 * imported fence (defect D1). Invariant I: the first fence creation on every timeline happens while V == 0.
 *
 * Construction: after every successful eglMakeCurrent with a non-NULL context, before returning to the caller,
 * create and destroy one EGL_SYNC_FENCE on the bound display. When the bind created the timeline, nothing has waited
 * on it yet (the wait slot is per timeline and the timeline is new), so V == 0 and this creation starts it
 * (measured equivalent: fence-first-wait khr-warm-nodup / warm-nodup, 0 early). When the timeline already existed,
 * the creation is an ordinary one (cached fence, or one FENCE_CREATE after pending work) and changes nothing. Doing
 * it on every bind, not once per thread, needs no knowledge of which calls destroy a timeline: whatever destroys
 * one, the next timeline can only be created by a bind, and every bind ends here. No other EGL entry point creates
 * one (measured, above), and the platform library (libnvnative-egl-android.so) creates no threads and binds no
 * contexts: its eglWaitSync (window.c) runs inside eglSwapBuffers on the thread that made the surface current
 * through this function.
 *
 * Re-entrancy: the driver's own internal binds (its unbind fence, the platform's surface switch through
 * EGLExtDriver's eglMakeCurrent) never come back through this export; t_bind_depth makes a nested call through it
 * a plain forward anyway, and the outermost call starts the timeline before anything returns to the app.
 * Failure is loud: logged with the driver's error, the previous binding restored (or NONE), EGL_FALSE returned. */
static __thread int t_bind_depth;

static EGLBoolean start_thread_timeline(EGLDisplay dpy, EGLContext ctx, EGLDisplay prev_dpy, EGLSurface prev_draw,
                                        EGLSurface prev_read, EGLContext prev_ctx) {
    EGLSync s = v_eglCreateSync(dpy, EGL_SYNC_FENCE, NULL);
    EGLint e = EGL_SUCCESS;
    const char *what = NULL;
    if (s == EGL_NO_SYNC) {
        e = v_eglGetError();
        what = "eglCreateSync(EGL_SYNC_FENCE)";
    } else if (!v_eglDestroySync(dpy, s)) {
        e = v_eglGetError();
        what = "eglDestroySync";
    }
    if (what == NULL) return EGL_TRUE;
    flog(1, "eglMakeCurrent(%p, ctx %p): %s failed (driver error 0x%04x) while starting this thread's fence timeline; "
         "without it the thread's first native fence after an eglWaitSync would signal before the work it covers "
         "(NVIDIA defect D1, FENCE-CONTRACT.md §6a). The bind is undone and eglMakeCurrent fails", dpy, ctx, what, e);
    if (prev_ctx == EGL_NO_CONTEXT || !v_eglMakeCurrent(prev_dpy, prev_draw, prev_read, prev_ctx)) {
        v_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    set_error(e == EGL_SUCCESS ? EGL_BAD_ALLOC : e);
    return EGL_FALSE;
}

/* ---------------- opt-in GL error log (diagnosis only, default off) ----------------
 * debug.nvnative.gldebug=1 (Android property, read once per process) or NVNATIVE_GL_DEBUG=1 (host): after every
 * successful bind, enable KHR_debug output on the bound context and log the driver's GL_DEBUG_TYPE_ERROR messages to
 * logcat ("nvnative-gl"), so a GL error inside a system client (SurfaceFlinger, HWUI) arrives with NVIDIA's own
 * reason. It changes app-visible GL state (GL_DEBUG_OUTPUT, the debug callback), hence opt-in. */
typedef void (*nvn_gldebugproc)(unsigned, unsigned, unsigned, unsigned, int, const char *, const void *);
static int g_gldebug = -1;
static void (*gl_Enable)(unsigned);
static void (*gl_DebugMessageCallback)(nvn_gldebugproc, const void *);

static void gldebug_cb(unsigned source, unsigned type, unsigned id, unsigned severity, int length, const char *msg,
                       const void *user) {
    (void)source; (void)severity; (void)user;
    if (type != 0x824C /* GL_DEBUG_TYPE_ERROR */) return;
    flog(1, "GL error (id 0x%x): %.*s", id, length > 0 ? length : 900, msg);
}

static void gldebug_on_bind(void) {
    if (g_gldebug < 0) {
        int on = 0;
        const char *e = getenv("NVNATIVE_GL_DEBUG");
        if (e != NULL && strcmp(e, "1") == 0) on = 1;
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX] = "";
        if (__system_property_get("debug.nvnative.gldebug", v) > 0 && strcmp(v, "1") == 0) on = 1;
#endif
        if (on) {
            *(void **)&gl_Enable = (void *)g_im.getProcAddress("glEnable");
            *(void **)&gl_DebugMessageCallback = (void *)g_im.getProcAddress("glDebugMessageCallback");
            on = gl_Enable != NULL && gl_DebugMessageCallback != NULL;
            flog(0, "GL error log %s (debug.nvnative.gldebug)", on ? "on" : "unavailable");
        }
        g_gldebug = on;
    }
    if (g_gldebug == 1) {
        gl_Enable(0x92E0 /* GL_DEBUG_OUTPUT */);
        gl_Enable(0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */);
        gl_DebugMessageCallback(gldebug_cb, NULL);
    }
}

NVN_EXPORT EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (__builtin_expect(!ready(), 0)) die("eglMakeCurrent called, but the NVIDIA EGL vendor is not loaded: %s", g_fail);
    t_err_set = 0;
    if (ctx == EGL_NO_CONTEXT || t_bind_depth > 0) return v_eglMakeCurrent(dpy, draw, read, ctx);
    EGLDisplay prev_dpy = v_eglGetCurrentDisplay();
    EGLSurface prev_draw = v_eglGetCurrentSurface(EGL_DRAW), prev_read = v_eglGetCurrentSurface(EGL_READ);
    EGLContext prev_ctx = v_eglGetCurrentContext();
    t_bind_depth++;
    EGLBoolean ok = v_eglMakeCurrent(dpy, draw, read, ctx);
    if (ok) ok = start_thread_timeline(dpy, ctx, prev_dpy, prev_draw, prev_read, prev_ctx);
    if (ok && g_gldebug != 0) gldebug_on_bind();
    t_bind_depth--;
    return ok;
}

NVN_EXPORT EGLint eglGetError(void) {
    if (t_err_set) {
        t_err_set = 0;
        return t_err;
    }
    return ready() ? v_eglGetError() : EGL_NOT_INITIALIZED;
}

static int has_word(const char *list, const char *w) {
    size_t n = strlen(w);
    for (const char *p = list; list != NULL && (p = strstr(p, w)) != NULL; p += n) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
    }
    return 0;
}

NVN_EXPORT EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display, const EGLAttrib *attribs) {
    if (!ready()) {
        flog(1, "eglGetPlatformDisplay: %s", g_fail);
        set_error(EGL_NOT_INITIALIZED);
        return EGL_NO_DISPLAY;
    }
    t_err_set = 0;
    EGLDisplay d = g_im.getPlatformDisplay(platform, native_display, attribs);
    if (d == EGL_NO_DISPLAY && platform == EGL_PLATFORM_ANDROID_KHR) {
        const char *pe = g_im.getVendorString ? g_im.getVendorString(__EGL_VENDOR_STRING_PLATFORM_EXTENSIONS) : NULL;
        if (!has_word(pe, "EGL_KHR_platform_android")) {
            flog(1, "eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR): no EGL external platform claims Android (driver "
                 "platform extensions: \"%s\"). Remedy: libnvnative-egl-android.so must be next to the shims and "
                 "nvglibc-default-env there must set __EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES to its JSON",
                 pe ? pe : "(null)");
            set_error(EGL_BAD_PARAMETER);
        } else {
            EGLint e = v_eglGetError();
            flog(1, "eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, %p) failed (EGL error 0x%04x; the platform "
                 "library logged why)", native_display, e);
            set_error(e == EGL_SUCCESS ? EGL_BAD_PARAMETER : e);
        }
    }
    return d;
}

NVN_EXPORT EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
    /* Android: eglGetDisplay(d) == eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, d) (egl_platform_entries.cpp:265) */
    return eglGetPlatformDisplay(EGL_PLATFORM_ANDROID_KHR, (void *)display, NULL);
}

static EGLDisplay front_eglGetPlatformDisplayEXT(EGLenum platform, void *native_display, const EGLint *attribs) {
    EGLAttrib a[64];
    int n = 0;
    for (; attribs != NULL && attribs[n] != EGL_NONE; n += 2) {
        if (n + 3 > (int)(sizeof(a) / sizeof(a[0]))) {
            set_error(EGL_BAD_ATTRIBUTE);
            return EGL_NO_DISPLAY;
        }
        a[n] = attribs[n];
        a[n + 1] = attribs[n + 1];
    }
    a[n] = EGL_NONE;
    return eglGetPlatformDisplay(platform, native_display, attribs ? a : NULL);
}

/* ---------------- client APIs: OpenGL ES only ----------------
 *
 * Android has no desktop OpenGL: its EGL loader reports EGL_CLIENT_APIS "OpenGL_ES" (frameworks/native
 * opengl/libs/EGL/egl_display.cpp:46), fills only GLES dispatch tables, and queries glGetString(GL_EXTENSIONS) at
 * every first make-current (egl_object.cpp:319), which is GL_INVALID_ENUM in a desktop core profile. NVIDIA's EGL
 * offers EGL_OPENGL_API and sets EGL_OPENGL_BIT on all 69 configs (measured, 615.71.09), so without this an app (or
 * dEQP-EGL create_context_ext.gl_*) gets a desktop context Android cannot drive. As Mesa does on Android
 * (src/egl/main/eglcurrent.h _eglIsApiValid: "OpenGL is not a valid/supported API on Android"; egl_dri2.c keeps
 * EGL_OPENGL_BIT out of ClientAPIs, hence out of every config's RenderableType/Conformant): eglBindAPI refuses
 * EGL_OPENGL_API with EGL_BAD_PARAMETER, configs report no EGL_OPENGL_BIT, a config request for EGL_OPENGL_BIT
 * matches nothing, and EGL_CLIENT_APIS is "OpenGL_ES". No config is GL-only (measured), so no config disappears. */
#define FRONT_CLIENT_APIS "OpenGL_ES"

NVN_EXPORT EGLBoolean eglBindAPI(EGLenum api) {
    if (__builtin_expect(!ready(), 0)) die("eglBindAPI called, but the NVIDIA EGL vendor is not loaded: %s", g_fail);
    if (api == EGL_OPENGL_API) {
        set_error(EGL_BAD_PARAMETER);
        return EGL_FALSE;
    }
    t_err_set = 0;
    return v_eglBindAPI(api);
}

NVN_EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
    if (__builtin_expect(!ready(), 0)) die("eglGetConfigAttrib called, but the NVIDIA EGL vendor is not loaded: %s", g_fail);
    t_err_set = 0;
    EGLBoolean ok = v_eglGetConfigAttrib(dpy, config, attribute, value);
    if (ok && value != NULL && (attribute == EGL_RENDERABLE_TYPE || attribute == EGL_CONFORMANT)) {
        *value &= ~EGL_OPENGL_BIT;
    }
    return ok;
}

NVN_EXPORT EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size,
                                      EGLint *num) {
    if (__builtin_expect(!ready(), 0)) die("eglChooseConfig called, but the NVIDIA EGL vendor is not loaded: %s", g_fail);
    t_err_set = 0;
    /* the driver validates the list (errors stay the driver's); a request for desktop GL then matches nothing */
    EGLBoolean ok = v_eglChooseConfig(dpy, attribs, configs, size, num);
    if (!ok || attribs == NULL || num == NULL) return ok;
    for (const EGLint *a = attribs; *a != EGL_NONE; a += 2) {
        if ((a[0] == EGL_RENDERABLE_TYPE || a[0] == EGL_CONFORMANT) && a[1] != EGL_DONT_CARE &&
            (a[1] & EGL_OPENGL_BIT)) {
            *num = 0;
            break;
        }
    }
    return ok;
}

/* Client extensions = the vendor's + the platform extensions of the loaded external platforms (what glvnd's
 * libEGL reports, libegl.c). */
static pthread_mutex_t g_cext_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_cext;

NVN_EXPORT const char *eglQueryString(EGLDisplay dpy, EGLint name) {
    if (!ready()) {
        set_error(EGL_NOT_INITIALIZED);
        return NULL;
    }
    t_err_set = 0;
    if (name == EGL_CLIENT_APIS) {
        /* the driver's answer for errors (bad display, not initialised); OpenGL ES only otherwise (see above) */
        return v_eglQueryString(dpy, name) != NULL ? FRONT_CLIENT_APIS : NULL;
    }
    if (dpy != EGL_NO_DISPLAY || name != EGL_EXTENSIONS) {
        return v_eglQueryString(dpy, name);
    }
    pthread_mutex_lock(&g_cext_lock);
    if (g_cext == NULL) {
        const char *c = v_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        const char *p = g_im.getVendorString ? g_im.getVendorString(__EGL_VENDOR_STRING_PLATFORM_EXTENSIONS) : NULL;
        size_t lc = c ? strlen(c) : 0, lp = p ? strlen(p) : 0;
        char *s = malloc(lc + lp + 2);
        if (s != NULL) {
            memcpy(s, c ? c : "", lc);
            s[lc] = '\0';
            if (lp) {
                if (lc) s[lc++] = ' ';
                memcpy(s + lc, p, lp + 1);
            }
            g_cext = s;
        }
    }
    const char *r = g_cext;
    pthread_mutex_unlock(&g_cext_lock);
    if (r == NULL) set_error(EGL_BAD_ALLOC);
    return r;
}

NVN_EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    if (name == NULL || !ready()) {
        return NULL;
    }
    static const struct {
        const char *name;
        __eglMustCastToProperFunctionPointerType fn;
    } own[] = {
        {"eglGetDisplay", (__eglMustCastToProperFunctionPointerType)eglGetDisplay},
        {"eglGetPlatformDisplay", (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplay},
        {"eglGetPlatformDisplayEXT", (__eglMustCastToProperFunctionPointerType)front_eglGetPlatformDisplayEXT},
        {"eglGetProcAddress", (__eglMustCastToProperFunctionPointerType)eglGetProcAddress},
        {"eglGetError", (__eglMustCastToProperFunctionPointerType)eglGetError},
        {"eglQueryString", (__eglMustCastToProperFunctionPointerType)eglQueryString},
        {"eglMakeCurrent", (__eglMustCastToProperFunctionPointerType)eglMakeCurrent}, /* never the driver's: §6a */
        {"eglBindAPI", (__eglMustCastToProperFunctionPointerType)eglBindAPI}, /* OpenGL ES only, above */
        {"eglChooseConfig", (__eglMustCastToProperFunctionPointerType)eglChooseConfig},
        {"eglGetConfigAttrib", (__eglMustCastToProperFunctionPointerType)eglGetConfigAttrib},
    };
    for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        if (strcmp(name, own[i].name) == 0) return own[i].fn;
    }
    __eglMustCastToProperFunctionPointerType f = (__eglMustCastToProperFunctionPointerType)g_im.getProcAddress(name);
    return f;
}
