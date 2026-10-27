/* eglgles: headless EGL + GLES probe for NVIDIA's own EGL/GLES driver, with NO libglvnd.
 *
 * NVIDIA's libEGL_nvidia.so.0 is a glvnd *vendor* library: its only EGL entry is
 * __egl_Main(version, exports, vendor, imports) (libglvnd include/glvnd/libeglabi.h, ABI 0.2).
 * This probe is its own minimal vendor host instead of libEGL.so.1 + libGLdispatch.so.0:
 *   - it implements __EGLapiExports itself (per-thread current API/display/context/surface,
 *     last error / last vendor, display and device -> vendor maps) with glvnd's semantics
 *     (build/libglvnd at v1.7.0: src/EGL/libegl.c, libeglcurrent.c), counts every callback the
 *     vendor makes and aborts loudly on a callback it has no honest implementation for
 *     (fetchDispatchEntry: we never assign dispatch indices);
 *   - every EGL and GL function is resolved through the vendor's imports.getProcAddress, as
 *     glvnd (LookupVendorEntrypoints, GLdispatch tables) and Android's EGL loader do;
 *   - the display comes from imports.getPlatformDisplay with EGL_PLATFORM_DEVICE_EXT on a device
 *     from the vendor's eglQueryDevicesEXT, or EGL_PLATFORM_SURFACELESS_MESA (--platform).
 * Tests (each verified exactly, exit 0 only if all pass):
 *   1. GLES 3.2 context on a 64x64 pbuffer: clear + a triangle whose fragment shader writes a
 *      uniform and gl_FragCoord-derived channels; glReadPixels checked pixel by pixel.
 *   2. the same draw into an FBO with the context current without any surface
 *      (EGL_KHR_surfaceless_context), if the display advertises it (else reported, not run).
 *   3. a GLES 3.1 compute shader writing an SSBO (4096 uints), read back by glMapBufferRange.
 *   4. EGL_KHR_fence_sync client wait and EGL_ANDROID_native_fence_sync fd (if advertised).
 * --out FILE writes GL/EGL identity and every extension string. Bootstraps:
 *   --icd PATH (default libEGL_nvidia.so.0): dlopen;   --sphal NAME (Android only):
 *   android_load_sphal_library(NAME), the sphal linker namespace, like nvprobe --sphal.
 * VKCOMPUTE_HOLD=1 / EGLGLES_HOLD=N: sleep N s (default 30) before teardown, for nvidia-smi. */
#define _GNU_SOURCE
#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <glvnd/libeglabi.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

#define DIE(...) do { fprintf(stderr, "eglgles: FATAL: " __VA_ARGS__); fputc('\n', stderr); exit(2); } while (0)

/* ---------------- the vendor host (our libEGL replacement) ---------------- */
struct __EGLvendorInfoRec {
    __EGLapiImports imports;
    void *dso;
    const char *name;
};
static struct __EGLvendorInfoRec g_vendor;

/* callback census */
enum { CB_threadInit, CB_getCurrentApi, CB_getCurrentVendor, CB_getCurrentContext, CB_getCurrentDisplay,
       CB_getCurrentSurface, CB_fetchDispatchEntry, CB_setEGLError, CB_setLastVendor,
       CB_getVendorFromDisplay, CB_getVendorFromDevice, CB_setVendorForDevice, CB_COUNT };
static const char *cb_name[CB_COUNT] = { "threadInit", "getCurrentApi", "getCurrentVendor", "getCurrentContext",
    "getCurrentDisplay", "getCurrentSurface", "fetchDispatchEntry", "setEGLError", "setLastVendor",
    "getVendorFromDisplay", "getVendorFromDevice", "setVendorForDevice" };
static atomic_ulong cb_count[CB_COUNT];
static int trace_cb;
#define NOTE(cb) do { if (atomic_fetch_add(&cb_count[CB_##cb], 1) == 0 || trace_cb) \
    fprintf(stderr, "eglgles: vendor called exports->%s\n", #cb); } while (0)

/* glvnd's __EGLThreadAPIState + __EGLdispatchThreadState, one per thread */
struct thread_state {
    EGLenum api;                 /* eglBindAPI value; EGL_OPENGL_ES_API by default (glvnd __eglQueryAPI) */
    EGLint last_error;
    __EGLvendorInfo *last_vendor;
    __EGLvendorInfo *cur_vendor; /* non-NULL while a context is current (or being made current) */
    EGLDisplay cur_dpy;
    EGLSurface cur_draw, cur_read;
    EGLContext cur_ctx;
};
static __thread struct thread_state ts = { .api = EGL_OPENGL_ES_API, .last_error = EGL_SUCCESS };

/* display -> vendor and device -> vendor maps (glvnd __eglAddDisplay / __eglAddDevice) */
#define MAXMAP 64
static struct { void *h; __EGLvendorInfo *v; } dpy_map[MAXMAP], dev_map[MAXMAP];
static int n_dpy, n_dev;
static void map_add(void *map_, int *n, void *h, __EGLvendorInfo *v) {
    struct { void *h; __EGLvendorInfo *v; } *map = map_;
    for (int i = 0; i < *n; i++) if (map[i].h == h) { map[i].v = v; return; }
    if (*n == MAXMAP) DIE("handle map full");
    map[*n].h = h; map[*n].v = v; (*n)++;
}
static __EGLvendorInfo *map_get(void *map_, int n, void *h) {
    struct { void *h; __EGLvendorInfo *v; } *map = map_;
    for (int i = 0; i < n; i++) if (map[i].h == h) return map[i].v;
    return NULL;
}

static void ex_threadInit(void) { NOTE(threadInit); /* glvnd: per-thread init; our state is __thread */ }
static EGLenum ex_getCurrentApi(void) { NOTE(getCurrentApi); return ts.api; }
static __EGLvendorInfo *ex_getCurrentVendor(void) { NOTE(getCurrentVendor); return ts.cur_vendor; }
static EGLContext ex_getCurrentContext(void) { NOTE(getCurrentContext); return ts.cur_vendor ? ts.cur_ctx : EGL_NO_CONTEXT; }
static EGLDisplay ex_getCurrentDisplay(void) { NOTE(getCurrentDisplay); return ts.cur_vendor ? ts.cur_dpy : EGL_NO_DISPLAY; }
static EGLSurface ex_getCurrentSurface(EGLint rd) {
    NOTE(getCurrentSurface);
    if (!ts.cur_vendor) return EGL_NO_SURFACE;
    return rd == EGL_DRAW ? ts.cur_draw : rd == EGL_READ ? ts.cur_read : EGL_NO_SURFACE;
}
static __eglMustCastToProperFunctionPointerType ex_fetchDispatchEntry(__EGLvendorInfo *v, int index) {
    NOTE(fetchDispatchEntry);
    (void)v;
    /* Only reachable from the vendor's own dispatch stubs (imports.getDispatchAddress), which this host never
     * hands out, and only for an index set by imports.setDispatchIndex, which it never calls. */
    fprintf(stderr, "eglgles: UNEXPECTED exports->fetchDispatchEntry(index %d): no dispatch index was assigned\n", index);
    abort();
}
static void ex_setEGLError(EGLint e) { NOTE(setEGLError); ts.last_error = e; ts.last_vendor = NULL; }
static EGLBoolean ex_setLastVendor(__EGLvendorInfo *v) { NOTE(setLastVendor); ts.last_error = EGL_SUCCESS; ts.last_vendor = v; return EGL_TRUE; }
static __EGLvendorInfo *ex_getVendorFromDisplay(EGLDisplay d) { NOTE(getVendorFromDisplay); return map_get(dpy_map, n_dpy, d); }
static __EGLvendorInfo *ex_getVendorFromDevice(EGLDeviceEXT d) { NOTE(getVendorFromDevice); return map_get(dev_map, n_dev, d); }
static EGLBoolean ex_setVendorForDevice(EGLDeviceEXT d, __EGLvendorInfo *v) { NOTE(setVendorForDevice); map_add(dev_map, &n_dev, d, v); return EGL_TRUE; }

static const __EGLapiExports g_exports = {
    .threadInit = ex_threadInit, .getCurrentApi = ex_getCurrentApi, .getCurrentVendor = ex_getCurrentVendor,
    .getCurrentContext = ex_getCurrentContext, .getCurrentDisplay = ex_getCurrentDisplay,
    .getCurrentSurface = ex_getCurrentSurface, .fetchDispatchEntry = ex_fetchDispatchEntry,
    .setEGLError = ex_setEGLError, .setLastVendor = ex_setLastVendor,
    .getVendorFromDisplay = ex_getVendorFromDisplay, .getVendorFromDevice = ex_getVendorFromDevice,
    .setVendorForDevice = ex_setVendorForDevice,
};

static void *vproc(const char *name) {
    void *p = g_vendor.imports.getProcAddress(name);
    if (!p) DIE("vendor getProcAddress(\"%s\") = NULL", name);
    return p;
}
static void *vproc_opt(const char *name) { return g_vendor.imports.getProcAddress(name); }

/* EGL entry points (vendor implementations, called the way glvnd's libEGL calls them) */
static PFNEGLINITIALIZEPROC v_eglInitialize;
static PFNEGLTERMINATEPROC v_eglTerminate;
static PFNEGLQUERYSTRINGPROC v_eglQueryString;
static PFNEGLCHOOSECONFIGPROC v_eglChooseConfig;
static PFNEGLGETCONFIGATTRIBPROC v_eglGetConfigAttrib;
static PFNEGLCREATEPBUFFERSURFACEPROC v_eglCreatePbufferSurface;
static PFNEGLCREATECONTEXTPROC v_eglCreateContext;
static PFNEGLDESTROYCONTEXTPROC v_eglDestroyContext;
static PFNEGLDESTROYSURFACEPROC v_eglDestroySurface;
static PFNEGLMAKECURRENTPROC v_eglMakeCurrent;
static PFNEGLGETERRORPROC v_eglGetError;
static PFNEGLBINDAPIPROC v_eglBindAPI;          /* optional in glvnd */
static PFNEGLRELEASETHREADPROC v_eglReleaseThread;
static PFNEGLQUERYDEVICESEXTPROC v_eglQueryDevicesEXT;
static PFNEGLQUERYDEVICESTRINGEXTPROC v_eglQueryDeviceStringEXT;

/* glvnd eglGetError (libegl.c:822) */
static EGLint h_eglGetError(void) {
    EGLint r = ts.last_vendor ? v_eglGetError() : ts.last_error;
    ts.last_vendor = NULL; ts.last_error = EGL_SUCCESS;
    return r;
}
#define VCALL(expr) (ts.last_error = EGL_SUCCESS, ts.last_vendor = &g_vendor, (expr)) /* glvnd __eglSetLastVendor before dispatch */

/* glvnd eglBindAPI (libegl.c:462): host-side state, then every vendor's optional bindAPI */
static EGLBoolean h_eglBindAPI(EGLenum api) {
    if (api != EGL_OPENGL_API && api != EGL_OPENGL_ES_API) { ts.last_error = EGL_BAD_PARAMETER; ts.last_vendor = NULL; return EGL_FALSE; }
    if (api == EGL_OPENGL_ES_API && !g_vendor.imports.getSupportsAPI(api)) { ts.last_error = EGL_BAD_PARAMETER; ts.last_vendor = NULL; return EGL_FALSE; }
    if (api == ts.api) return EGL_TRUE;
    ts.api = api;
    if (v_eglBindAPI) v_eglBindAPI(api);
    return EGL_TRUE;
}

/* glvnd eglMakeCurrent for a single vendor (libegl.c:541-760) */
static EGLBoolean h_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    if (!map_get(dpy_map, n_dpy, dpy)) { ts.last_error = EGL_BAD_DISPLAY; ts.last_vendor = NULL; return EGL_FALSE; }
    if (ctx == EGL_NO_CONTEXT && (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE)) { ts.last_error = EGL_BAD_MATCH; ts.last_vendor = NULL; return EGL_FALSE; }
    if (!ts.cur_vendor) {
        if (ctx == EGL_NO_CONTEXT) return EGL_TRUE;
        ts.cur_vendor = &g_vendor; /* InternalMakeCurrentDispatch: apiState->currentVendor set before the vendor call */
        EGLBoolean r = VCALL(v_eglMakeCurrent(dpy, draw, read, ctx));
        if (r) { ts.cur_dpy = dpy; ts.cur_draw = draw; ts.cur_read = read; ts.cur_ctx = ctx; }
        else ts.cur_vendor = NULL;
        return r;
    }
    if (dpy == ts.cur_dpy && ctx == ts.cur_ctx && draw == ts.cur_draw && read == ts.cur_read) return EGL_TRUE;
    if (ctx == EGL_NO_CONTEXT) { /* InternalLoseCurrent */
        EGLBoolean r = VCALL(v_eglMakeCurrent(ts.cur_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        if (!r) return EGL_FALSE;
        ts.cur_vendor = NULL; ts.cur_dpy = EGL_NO_DISPLAY; ts.cur_draw = ts.cur_read = EGL_NO_SURFACE; ts.cur_ctx = EGL_NO_CONTEXT;
        return EGL_TRUE;
    }
    EGLBoolean r = VCALL(v_eglMakeCurrent(dpy, draw, read, ctx)); /* same vendor: InternalMakeCurrentVendor */
    if (r) { ts.cur_dpy = dpy; ts.cur_draw = draw; ts.cur_read = read; ts.cur_ctx = ctx; }
    return r;
}

/* ---------------- GL entry points, all through imports.getProcAddress ---------------- */
#define GLFUNCS(X) \
    X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLGETSTRINGIPROC, glGetStringi) X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLGETERRORPROC, glGetError) X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLCLEARCOLORPROC, glClearColor) \
    X(PFNGLCLEARPROC, glClear) X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM4FPROC, glUniform4f) X(PFNGLUNIFORM1UIPROC, glUniform1ui) X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLFINISHPROC, glFinish) X(PFNGLFLUSHPROC, glFlush) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLPIXELSTOREIPROC, glPixelStorei) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers) X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)
#define DECL(t, n) static t p_##n;
GLFUNCS(DECL)
#define RESOLVE(t, n) p_##n = (t)vproc(#n);

static FILE *rep;
static void out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    if (rep) { va_start(ap, fmt); vfprintf(rep, fmt, ap); va_end(ap); }
}
static int has_ext(const char *list, const char *ext) {
    if (!list) return 0;
    size_t n = strlen(ext);
    for (const char *p = list; (p = strstr(p, ext)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
    return 0;
}
static void glcheck(const char *where) {
    GLenum e = p_glGetError();
    if (e != GL_NO_ERROR) DIE("GL error 0x%04x at %s", e, where);
}

static GLuint shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048] = ""; p_glGetShaderInfoLog(s, sizeof log, NULL, log); DIE("shader compile: %s", log); }
    return s;
}
static GLuint program(GLuint a, GLuint b) {
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, a); if (b) p_glAttachShader(p, b);
    p_glLinkProgram(p);
    GLint ok = 0; p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048] = ""; p_glGetProgramInfoLog(p, sizeof log, NULL, log); DIE("program link: %s", log); }
    return p;
}

#define W 64
#define H 64
static const char *vs_src =
    "#version 320 es\n"
    "layout(location = 0) in vec2 pos;\n"
    "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
    "#version 320 es\n"
    "precision highp float;\n"
    "uniform vec4 u_color;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = vec4(u_color.r, floor(gl_FragCoord.x) / 255.0, floor(gl_FragCoord.y) * 2.0 / 255.0, u_color.a); }\n";

/* clear (51,102,153,255); triangle (-1,-1),(1,-1),(-1,1) covers pixel (x,y) iff x+y <= 62 (centre strictly
 * inside); x+y == 63 lies on the edge and is not checked. Inside: (200, x, 2y, 255). */
static int draw_and_verify(const char *what) {
    GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = program(vs, fs);
    static const float tri[] = { -1, -1, 1, -1, -1, 1 };
    GLuint vao, vbo;
    p_glGenVertexArrays(1, &vao); p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0); p_glEnableVertexAttribArray(0);
    p_glViewport(0, 0, W, H);
    p_glClearColor(0.2f, 0.4f, 0.6f, 1.0f); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glUseProgram(prog);
    GLint loc = p_glGetUniformLocation(prog, "u_color");
    if (loc < 0) DIE("u_color not found");
    p_glUniform4f(loc, 200.0f / 255.0f, 0.0f, 0.0f, 1.0f);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    glcheck("draw");
    static unsigned char px[W * H * 4];
    memset(px, 0xee, sizeof px);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glcheck("readpixels");
    int wrong = 0, inside = 0, outside = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const unsigned char *p = px + 4 * (y * W + x);
        unsigned char e[4];
        if (x + y <= 62) { e[0] = 200; e[1] = (unsigned char)x; e[2] = (unsigned char)(2 * y); e[3] = 255; inside++; }
        else if (x + y >= 64) { e[0] = 51; e[1] = 102; e[2] = 153; e[3] = 255; outside++; }
        else continue;
        if (memcmp(p, e, 4)) {
            if (wrong++ < 5) fprintf(stderr, "eglgles: %s pixel (%d,%d) = %u,%u,%u,%u expected %u,%u,%u,%u\n", what, x, y,
                                     p[0], p[1], p[2], p[3], e[0], e[1], e[2], e[3]);
        }
    }
    const unsigned char *a = px + 4 * (10 * W + 20), *b = px + 4 * (60 * W + 50);
    out("%s: %d inside + %d outside pixels checked, %d wrong; (20,10)=%u,%u,%u,%u (50,60)=%u,%u,%u,%u\n", what,
        inside, outside, wrong, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
    p_glDeleteBuffers(1, &vbo); p_glDeleteVertexArrays(1, &vao);
    p_glDeleteProgram(prog); p_glDeleteShader(vs); p_glDeleteShader(fs);
    return wrong;
}

#define NCOMP 4096u
static const char *cs_src =
    "#version 310 es\n"
    "layout(local_size_x = 64) in;\n"
    "layout(std430, binding = 0) buffer B { uint d[]; };\n"
    "uniform uint u_k;\n"
    "void main() { uint i = gl_GlobalInvocationID.x; d[i] = i * i + u_k; }\n";
static int compute_and_verify(void) {
    GLuint cs = shader(GL_COMPUTE_SHADER, cs_src), prog = program(cs, 0);
    GLuint buf; p_glGenBuffers(1, &buf); p_glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
    p_glBufferData(GL_SHADER_STORAGE_BUFFER, NCOMP * 4, NULL, GL_DYNAMIC_READ);
    p_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
    p_glUseProgram(prog);
    GLint loc = p_glGetUniformLocation(prog, "u_k");
    if (loc < 0) DIE("u_k not found");
    p_glUniform1ui(loc, 0x5a5a0000u);
    p_glDispatchCompute(NCOMP / 64, 1, 1);
    p_glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glcheck("dispatch");
    const uint32_t *d = p_glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, NCOMP * 4, GL_MAP_READ_BIT);
    if (!d) DIE("glMapBufferRange failed (0x%04x)", p_glGetError());
    unsigned wrong = 0;
    for (uint32_t i = 0; i < NCOMP; i++)
        if (d[i] != i * i + 0x5a5a0000u && wrong++ < 5) fprintf(stderr, "eglgles: compute d[%u] = 0x%x\n", i, d[i]);
    out("compute: %u elements, %u wrong; d[4095]=0x%08x\n", NCOMP, wrong, d[NCOMP - 1]);
    p_glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    p_glDeleteBuffers(1, &buf); p_glDeleteProgram(prog); p_glDeleteShader(cs);
    glcheck("compute teardown");
    return (int)wrong;
}

static int sync_tests(EGLDisplay dpy, const char *exts) {
    int fails = 0;
    if (has_ext(exts, "EGL_KHR_fence_sync")) {
        PFNEGLCREATESYNCKHRPROC cs = (PFNEGLCREATESYNCKHRPROC)vproc("eglCreateSyncKHR");
        PFNEGLCLIENTWAITSYNCKHRPROC cw = (PFNEGLCLIENTWAITSYNCKHRPROC)vproc("eglClientWaitSyncKHR");
        PFNEGLDESTROYSYNCKHRPROC ds = (PFNEGLDESTROYSYNCKHRPROC)vproc("eglDestroySyncKHR");
        p_glClear(GL_COLOR_BUFFER_BIT);
        EGLSyncKHR s = VCALL(cs(dpy, EGL_SYNC_FENCE_KHR, NULL));
        EGLint r = s ? VCALL(cw(dpy, s, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 1000000000ull)) : 0;
        out("EGL_KHR_fence_sync: sync %s, client wait -> 0x%04x (%s)\n", s ? "created" : "FAILED", r,
            r == EGL_CONDITION_SATISFIED_KHR ? "CONDITION_SATISFIED" : "unexpected");
        if (r != EGL_CONDITION_SATISFIED_KHR) fails++;
        if (s) VCALL(ds(dpy, s));
    } else out("EGL_KHR_fence_sync: not advertised (not run)\n");
    if (has_ext(exts, "EGL_ANDROID_native_fence_sync")) {
        PFNEGLCREATESYNCKHRPROC cs = (PFNEGLCREATESYNCKHRPROC)vproc("eglCreateSyncKHR");
        PFNEGLDESTROYSYNCKHRPROC ds = (PFNEGLDESTROYSYNCKHRPROC)vproc("eglDestroySyncKHR");
        PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)vproc("eglDupNativeFenceFDANDROID");
        p_glClear(GL_COLOR_BUFFER_BIT);
        EGLSyncKHR s = VCALL(cs(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL));
        p_glFlush();
        int fd = s ? VCALL(dup(dpy, s)) : -1;
        int pr = -1;
        if (fd >= 0) { struct pollfd pf = { .fd = fd, .events = POLLIN }; pr = poll(&pf, 1, 1000); close(fd); }
        out("EGL_ANDROID_native_fence_sync: sync %s, dup fd %d, poll -> %d (%s)\n", s ? "created" : "FAILED", fd, pr,
            pr == 1 ? "signalled" : "unexpected");
        if (pr != 1) fails++;
        if (s) VCALL(ds(dpy, s));
    } else out("EGL_ANDROID_native_fence_sync: not advertised (not run)\n");
    return fails;
}

static void highlight(const char *label, const char *list, const char *const *names) {
    out("%s highlights:", label);
    for (int i = 0; names[i]; i++) out(" %s=%s", names[i], has_ext(list, names[i]) ? "yes" : "no");
    out("\n");
}

#ifdef __ANDROID__
typedef void *(*PFN_android_load_sphal_library)(const char *name, int flag);
static void *sphal_dlopen(const char *name) {
    void *vndk = dlopen("libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
    if (!vndk) DIE("dlopen libvndksupport.so: %s", dlerror());
    PFN_android_load_sphal_library load = (PFN_android_load_sphal_library)dlsym(vndk, "android_load_sphal_library");
    if (!load) DIE("libvndksupport.so has no android_load_sphal_library");
    void *h = load(name, RTLD_NOW | RTLD_LOCAL);
    if (!h) DIE("android_load_sphal_library(%s): %s", name, dlerror());
    return h;
}
#endif

int main(int argc, char **argv) {
    const char *icd = "libEGL_nvidia.so.0", *sphal = NULL, *outfile = NULL, *platform = "device";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
        else if (!strcmp(argv[i], "--sphal") && i + 1 < argc) sphal = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outfile = argv[++i];
        else if (!strcmp(argv[i], "--platform") && i + 1 < argc) platform = argv[++i];
        else DIE("usage: eglgles [--icd PATH | --sphal NAME] [--platform device|surfaceless] [--out FILE]");
    }
    trace_cb = getenv("EGLGLES_TRACE_CALLBACKS") != NULL;
    if (outfile && !(rep = fopen(outfile, "w"))) DIE("cannot write %s", outfile);
    setvbuf(stdout, NULL, _IOLBF, 0);

    void *h;
    if (sphal) {
#ifdef __ANDROID__
        h = sphal_dlopen(sphal); icd = sphal;
#else
        DIE("--sphal is only meaningful inside Android (bionic)");
#endif
    } else if (!(h = dlopen(icd, RTLD_NOW | RTLD_LOCAL))) DIE("dlopen %s: %s", icd, dlerror());
    Dl_info di;
    __PFNEGLMAINPROC egl_main = (__PFNEGLMAINPROC)dlsym(h, __EGL_MAIN_PROTO_NAME);
    if (!egl_main) DIE("%s has no %s", icd, __EGL_MAIN_PROTO_NAME);
    if (dladdr((void *)egl_main, &di) && di.dli_fname) fprintf(stderr, "eglgles: %s resolved to %s\n", icd, di.dli_fname);

    /* glvnd LoadVendorFromPath (libeglvendor.c:520-580) */
    g_vendor.dso = h; g_vendor.name = "nvidia";
    if (!egl_main(EGL_VENDOR_ABI_VERSION, &g_exports, &g_vendor, &g_vendor.imports))
        DIE("__egl_Main(ABI %u.%u) refused", EGL_VENDOR_ABI_MAJOR_VERSION, EGL_VENDOR_ABI_MINOR_VERSION);
    __EGLapiImports *im = &g_vendor.imports;
    if (!im->getPlatformDisplay || !im->getSupportsAPI || !im->getProcAddress || !im->getDispatchAddress || !im->setDispatchIndex)
        DIE("vendor left a required import NULL");
    out("vendor imports: getVendorString=%s isPatchSupported=%s initiatePatch=%s releasePatch=%s patchThreadAttach=%s findNativeDisplayPlatform=%s\n",
        im->getVendorString ? "set" : "NULL", im->isPatchSupported ? "set" : "NULL", im->initiatePatch ? "set" : "NULL",
        im->releasePatch ? "set" : "NULL", im->patchThreadAttach ? "set" : "NULL", im->findNativeDisplayPlatform ? "set" : "NULL");
    out("supports GL=%d GLES=%d\n", im->getSupportsAPI(EGL_OPENGL_API), im->getSupportsAPI(EGL_OPENGL_ES_API));
    if (im->getVendorString) {
        const char *pe = im->getVendorString(__EGL_VENDOR_STRING_PLATFORM_EXTENSIONS);
        out("vendor platform extensions: %s\n", pe ? pe : "(null)");
    }

    v_eglInitialize = (PFNEGLINITIALIZEPROC)vproc("eglInitialize");
    v_eglTerminate = (PFNEGLTERMINATEPROC)vproc("eglTerminate");
    v_eglQueryString = (PFNEGLQUERYSTRINGPROC)vproc("eglQueryString");
    v_eglChooseConfig = (PFNEGLCHOOSECONFIGPROC)vproc("eglChooseConfig");
    v_eglGetConfigAttrib = (PFNEGLGETCONFIGATTRIBPROC)vproc("eglGetConfigAttrib");
    v_eglCreatePbufferSurface = (PFNEGLCREATEPBUFFERSURFACEPROC)vproc("eglCreatePbufferSurface");
    v_eglCreateContext = (PFNEGLCREATECONTEXTPROC)vproc("eglCreateContext");
    v_eglDestroyContext = (PFNEGLDESTROYCONTEXTPROC)vproc("eglDestroyContext");
    v_eglDestroySurface = (PFNEGLDESTROYSURFACEPROC)vproc("eglDestroySurface");
    v_eglMakeCurrent = (PFNEGLMAKECURRENTPROC)vproc("eglMakeCurrent");
    v_eglGetError = (PFNEGLGETERRORPROC)vproc("eglGetError");
    v_eglReleaseThread = (PFNEGLRELEASETHREADPROC)vproc("eglReleaseThread");
    v_eglBindAPI = (PFNEGLBINDAPIPROC)vproc_opt("eglBindAPI");
    out("vendor eglBindAPI: %s\n", v_eglBindAPI ? "provided" : "not provided (host-side state only)");
    GLFUNCS(RESOLVE)

    /* client extensions: glvnd builds eglQueryString(EGL_NO_DISPLAY) itself; ask the vendor directly */
    const char *cext = VCALL(v_eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS));
    out("EGL client extensions (vendor, EGL_NO_DISPLAY): %s\n", cext ? cext : "(null)");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (!strcmp(platform, "device")) {
        v_eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC)vproc("eglQueryDevicesEXT");
        v_eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC)vproc("eglQueryDeviceStringEXT");
        EGLDeviceEXT devs[16]; EGLint nd = 0;
        if (!VCALL(v_eglQueryDevicesEXT(16, devs, &nd)) || nd < 1) DIE("eglQueryDevicesEXT: %d devices, error 0x%x", nd, h_eglGetError());
        for (int i = 0; i < nd; i++) {
            map_add(dev_map, &n_dev, devs[i], &g_vendor); /* glvnd __eglAddDevice in its eglQueryDevicesEXT */
            const char *de = VCALL(v_eglQueryDeviceStringEXT(devs[i], EGL_EXTENSIONS));
            const char *node = has_ext(de, "EGL_EXT_device_drm") ? VCALL(v_eglQueryDeviceStringEXT(devs[i], EGL_DRM_DEVICE_FILE_EXT)) : NULL;
            const char *rnode = has_ext(de, "EGL_EXT_device_drm_render_node") ? VCALL(v_eglQueryDeviceStringEXT(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT)) : NULL;
            out("EGL device %d: extensions: %s; drm %s, render %s\n", i, de ? de : "(null)", node ? node : "-", rnode ? rnode : "-");
        }
        /* every device here comes from NVIDIA's own vendor library; GL_VENDOR is checked below */
        dpy = VCALL(im->getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[0], NULL));
    } else if (!strcmp(platform, "surfaceless")) {
        dpy = VCALL(im->getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL));
    } else DIE("unknown --platform %s", platform);
    if (dpy == EGL_NO_DISPLAY) DIE("getPlatformDisplay(%s) = EGL_NO_DISPLAY, error 0x%x", platform, h_eglGetError());
    map_add(dpy_map, &n_dpy, dpy, &g_vendor); /* glvnd __eglAddDisplay */
    EGLint maj = 0, min = 0;
    if (!VCALL(v_eglInitialize(dpy, &maj, &min))) DIE("eglInitialize: 0x%x", h_eglGetError());
    const char *dext = VCALL(v_eglQueryString(dpy, EGL_EXTENSIONS));
    const char *ever = VCALL(v_eglQueryString(dpy, EGL_VERSION));
    const char *evendor = VCALL(v_eglQueryString(dpy, EGL_VENDOR));
    const char *eapis = VCALL(v_eglQueryString(dpy, EGL_CLIENT_APIS));
    out("platform: %s\nEGL_VERSION: %d.%d \"%s\"\nEGL_VENDOR: %s\nEGL_CLIENT_APIS: %s\nEGL display extensions: %s\n", platform,
        maj, min, ever, evendor, eapis, dext);
    static const char *const egl_hl[] = { "EGL_ANDROID_native_fence_sync", "EGL_ANDROID_image_native_buffer",
        "EGL_ANDROID_get_native_client_buffer", "EGL_ANDROID_recordable", "EGL_ANDROID_presentation_time",
        "EGL_ANDROID_framebuffer_target", "EGL_ANDROID_blob_cache", "EGL_ANDROID_front_buffer_auto_refresh",
        "EGL_ANDROID_get_frame_timestamps", "EGL_KHR_fence_sync", "EGL_KHR_reusable_sync", "EGL_KHR_wait_sync",
        "EGL_KHR_image_base", "EGL_KHR_gl_texture_2D_image", "EGL_EXT_image_dma_buf_import",
        "EGL_EXT_image_dma_buf_import_modifiers", "EGL_MESA_image_dma_buf_export", "EGL_KHR_surfaceless_context",
        "EGL_KHR_no_config_context", "EGL_KHR_create_context", "EGL_KHR_create_context_no_error",
        "EGL_EXT_create_context_robustness", "EGL_IMG_context_priority", "EGL_KHR_partial_update",
        "EGL_EXT_buffer_age", "EGL_KHR_swap_buffers_with_damage", "EGL_EXT_pixel_format_float",
        "EGL_KHR_gl_colorspace", "EGL_EXT_gl_colorspace_display_p3", "EGL_EXT_protected_content",
        "EGL_NV_stream_consumer_eglimage", "EGL_KHR_stream", NULL };
    static const char *const cl_hl[] = { "EGL_EXT_platform_base", "EGL_EXT_platform_device", "EGL_EXT_device_base",
        "EGL_EXT_device_enumeration", "EGL_EXT_device_query", "EGL_MESA_platform_surfaceless", "EGL_KHR_platform_gbm",
        "EGL_KHR_platform_wayland", "EGL_KHR_platform_x11", "EGL_ANDROID_GLES_layers", "EGL_KHR_debug", NULL };
    highlight("EGL display", dext, egl_hl);
    highlight("EGL client", cext, cl_hl);

    if (!h_eglBindAPI(EGL_OPENGL_ES_API)) DIE("eglBindAPI(GLES): 0x%x", h_eglGetError());
    const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!VCALL(v_eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg)) || ncfg < 1) DIE("eglChooseConfig: %d configs, 0x%x", ncfg, h_eglGetError());
    EGLint cid = 0; VCALL(v_eglGetConfigAttrib(dpy, cfg, EGL_CONFIG_ID, &cid));
    const EGLint pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface pb = VCALL(v_eglCreatePbufferSurface(dpy, cfg, pb_attr));
    if (pb == EGL_NO_SURFACE) DIE("eglCreatePbufferSurface: 0x%x", h_eglGetError());
    const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE };
    EGLContext ctx = VCALL(v_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr));
    if (ctx == EGL_NO_CONTEXT) DIE("eglCreateContext(GLES 3.2): 0x%x", h_eglGetError());
    if (!h_eglMakeCurrent(dpy, pb, pb, ctx)) DIE("eglMakeCurrent(pbuffer): 0x%x", h_eglGetError());

    out("config id %d\nGL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s\nGL_SHADING_LANGUAGE_VERSION: %s\n", cid,
        p_glGetString(GL_VENDOR), p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION), p_glGetString(GL_SHADING_LANGUAGE_VERSION));
    if (!p_glGetString(GL_VENDOR) || !strstr((const char *)p_glGetString(GL_VENDOR), "NVIDIA")) DIE("GL_VENDOR is not NVIDIA");
    GLint next = 0; p_glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    size_t cap = 65536, len = 0; char *glext = calloc(1, cap);
    for (GLint i = 0; i < next; i++) {
        const char *e = (const char *)p_glGetStringi(GL_EXTENSIONS, (GLuint)i);
        size_t l = strlen(e);
        while (len + l + 2 > cap) glext = realloc(glext, cap *= 2);
        if (len) glext[len++] = ' ';
        memcpy(glext + len, e, l + 1); len += l;
    }
    out("GL extensions (%d): %s\n", next, glext);
    static const char *const gl_hl[] = { "GL_OES_EGL_image", "GL_OES_EGL_image_external", "GL_OES_EGL_image_external_essl3",
        "GL_EXT_EGL_image_storage", "GL_OES_EGL_sync", "GL_EXT_memory_object_fd", "GL_EXT_semaphore_fd",
        "GL_KHR_texture_compression_astc_ldr", "GL_OES_texture_compression_astc", "GL_EXT_texture_compression_s3tc",
        "GL_OES_compressed_ETC1_RGB8_texture", "GL_EXT_YUV_target", "GL_EXT_disjoint_timer_query",
        "GL_EXT_shader_framebuffer_fetch", "GL_ANDROID_extension_pack_es31a", "GL_EXT_buffer_storage",
        "GL_EXT_clip_cull_distance", "GL_OVR_multiview2", NULL };
    highlight("GL", glext, gl_hl);
    glcheck("strings");

    int fails = 0;
    fails += draw_and_verify("pbuffer draw") != 0;
    fails += compute_and_verify() != 0;
    fails += sync_tests(dpy, dext);

    if (has_ext(dext, "EGL_KHR_surfaceless_context")) {
        if (!h_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) DIE("eglMakeCurrent(surfaceless): 0x%x", h_eglGetError());
        GLuint fbo, rb;
        p_glGenFramebuffers(1, &fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        p_glGenRenderbuffers(1, &rb); p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) DIE("FBO incomplete 0x%04x", st);
        fails += draw_and_verify("surfaceless FBO draw") != 0;
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        p_glDeleteRenderbuffers(1, &rb); p_glDeleteFramebuffers(1, &fbo);
    } else out("surfaceless FBO draw: EGL_KHR_surfaceless_context not advertised (not run)\n");

    const char *hold = getenv("EGLGLES_HOLD");
    if (!hold && getenv("VKCOMPUTE_HOLD")) hold = "30";
    if (hold) { out("holding %s s, pid %d\n", hold, (int)getpid()); sleep((unsigned)atoi(hold)); }

    if (!h_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) DIE("release current: 0x%x", h_eglGetError());
    VCALL(v_eglDestroyContext(dpy, ctx)); VCALL(v_eglDestroySurface(dpy, pb));
    if (!VCALL(v_eglTerminate(dpy))) DIE("eglTerminate: 0x%x", h_eglGetError());
    VCALL(v_eglReleaseThread());

    out("export callbacks used by the vendor:");
    for (int i = 0; i < CB_COUNT; i++) if (cb_count[i]) out(" %s=%lu", cb_name[i], (unsigned long)cb_count[i]);
    out("\nexport callbacks never called:");
    for (int i = 0; i < CB_COUNT; i++) if (!cb_count[i]) out(" %s", cb_name[i]);
    out("\n%s: %s\n", platform, fails ? "FAIL" : "PASS");
    if (rep) fclose(rep);
    return fails ? 1 : 0;
}
