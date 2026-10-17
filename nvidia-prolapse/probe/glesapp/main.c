/* glesapp: the smallest real GLES app, as a NativeActivity (no Java code).
 *
 * Renders a rotating triangle over a slowly cycling background into the activity's window with
 * EGL + GLES2, so SurfaceFlinger composites real frames from it, and logs (tag "glesapp") the
 * GL_VENDOR / GL_RENDERER / GL_VERSION strings once and the frame rate every 2 s. Built per ABI;
 * the x86-only build is how a 32-bit GLES process is exercised (P6: ANGLE x86 -> the 32-bit Vulkan
 * HAL -> NVIDIA's i386 ICD). Fails loudly: every EGL/GL failure is logged with its code and the
 * render thread stops (the window then stays black, which a screencap shows).
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TAG "glesapp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct app {
    pthread_mutex_t lock;
    ANativeWindow *window; /* set/cleared by the activity callbacks */
    pthread_t thread;
    int running;
    atomic_int stop;
    AInputQueue *input; /* owned by NativeActivity, used only on its main looper */
    unsigned input_events;
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        return 0;
    }
    return s;
}

static void *render(void *arg) {
    struct app *a = arg;
    pthread_mutex_lock(&a->lock);
    ANativeWindow *win = a->window;
    if (win) ANativeWindow_acquire(win);
    pthread_mutex_unlock(&a->lock);
    if (!win) return NULL;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        goto out_win;
    }
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        LOGE("eglChooseConfig failed: 0x%x (%d configs)", eglGetError(), n);
        goto out_dpy;
    }
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, win, NULL);
    if (surf == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        goto out_dpy;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("eglCreateContext/eglMakeCurrent failed: 0x%x", eglGetError());
        goto out_surf;
    }
    LOGI("EGL %d.%d; GL_VENDOR \"%s\"; GL_RENDERER \"%s\"; GL_VERSION \"%s\"; pointer size %zu",
         major, minor, (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
         (const char *)glGetString(GL_VERSION), sizeof(void *));

    GLuint vs = shader(GL_VERTEX_SHADER,
                       "attribute vec2 p; attribute vec3 c; uniform float t; varying vec3 v;\n"
                       "void main() { float s = sin(t), k = cos(t);\n"
                       "  gl_Position = vec4(k * p.x - s * p.y, s * p.x + k * p.y, 0.0, 1.0); v = c; }\n");
    GLuint fs = shader(GL_FRAGMENT_SHADER,
                       "precision mediump float; varying vec3 v; void main() { gl_FragColor = vec4(v, 1.0); }\n");
    if (!vs || !fs) goto out_ctx;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "p");
    glBindAttribLocation(prog, 1, "c");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        LOGE("program link failed");
        goto out_ctx;
    }
    static const GLfloat pos[] = {0.0f, 0.8f, -0.7f, -0.5f, 0.7f, -0.5f};
    static const GLfloat col[] = {1.0f, 0.1f, 0.1f, 0.1f, 1.0f, 0.1f, 0.1f, 0.2f, 1.0f};
    GLint ut = glGetUniformLocation(prog, "t");

    double t0 = now_s(), tl = t0;
    unsigned frames = 0, total = 0;
    while (!atomic_load(&a->stop)) {
        double t = now_s() - t0;
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.15f + 0.1f * (float)sin(t), 0.15f, 0.25f + 0.1f * (float)cos(t), 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1f(ut, (float)t);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, col);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOGE("GL error 0x%x at frame %u", err, total);
            break;
        }
        if (!eglSwapBuffers(dpy, surf)) {
            LOGE("eglSwapBuffers failed: 0x%x at frame %u", eglGetError(), total);
            break;
        }
        frames++;
        total++;
        double tn = now_s();
        if (tn - tl >= 2.0) {
            LOGI("%u frames in %.2f s (%.1f fps), %d x %d, %u total", frames, tn - tl, frames / (tn - tl), w, h, total);
            frames = 0;
            tl = tn;
        }
    }
    LOGI("render thread stops after %u frames", total);
out_ctx:
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
out_surf:
    eglDestroySurface(dpy, surf);
out_dpy:
    eglTerminate(dpy);
out_win:
    ANativeWindow_release(win);
    return NULL;
}

static void stop_thread(struct app *a) {
    if (!a->running) return;
    atomic_store(&a->stop, 1);
    pthread_join(a->thread, NULL);
    a->running = 0;
}

static void on_window_created(ANativeActivity *act, ANativeWindow *win) {
    struct app *a = act->instance;
    stop_thread(a);
    pthread_mutex_lock(&a->lock);
    a->window = win;
    pthread_mutex_unlock(&a->lock);
    atomic_store(&a->stop, 0);
    if (pthread_create(&a->thread, NULL, render, a) == 0) a->running = 1;
    else LOGE("pthread_create failed");
}

static void on_window_destroyed(ANativeActivity *act, ANativeWindow *win) {
    (void)win;
    struct app *a = act->instance;
    stop_thread(a);
    pthread_mutex_lock(&a->lock);
    a->window = NULL;
    pthread_mutex_unlock(&a->lock);
}

/* NativeActivity transfers input delivery to this queue. Even a render-only
 * test must finish events, or InputDispatcher reports an ANR after five seconds.
 * Keep input on the activity looper so GPU submission cannot delay its drain. */
static int on_input(int fd, int events, void *data) {
    (void)fd;
    struct app *a = data;
    if (!(events & ALOOPER_EVENT_INPUT)) return 1;
    AInputEvent *event;
    while (AInputQueue_getEvent(a->input, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(a->input, event)) continue;
        int type = AInputEvent_getType(event);
        /* Let the framework handle unconsumed keys, including Back. */
        int handled = type == AINPUT_EVENT_TYPE_MOTION;
        AInputQueue_finishEvent(a->input, event, handled);
        a->input_events++;
        if (a->input_events <= 8 || a->input_events % 64 == 0)
            LOGI("input event %u finished (type %d, handled %d)", a->input_events, type, handled);
    }
    return 1;
}

static void on_input_created(ANativeActivity *act, AInputQueue *queue) {
    struct app *a = act->instance;
    if (a->input) AInputQueue_detachLooper(a->input);
    a->input = queue;
    ALooper *looper = ALooper_prepare(0);
    if (!looper) {
        LOGE("cannot attach NativeActivity input queue: no main looper");
        ANativeActivity_finish(act);
        return;
    }
    AInputQueue_attachLooper(queue, looper, 0, on_input, a);
    LOGI("input queue attached to activity looper");
}

static void on_input_destroyed(ANativeActivity *act, AInputQueue *queue) {
    struct app *a = act->instance;
    AInputQueue_detachLooper(queue);
    if (a->input == queue) a->input = NULL;
    LOGI("input queue detached after %u events", a->input_events);
}

static void on_destroy(ANativeActivity *act) {
    struct app *a = act->instance;
    stop_thread(a);
    if (a->input) AInputQueue_detachLooper(a->input);
    pthread_mutex_destroy(&a->lock);
    free(a);
    act->instance = NULL;
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *act, void *saved, size_t size) {
    (void)saved;
    (void)size;
    struct app *a = calloc(1, sizeof(*a));
    if (!a) {
        LOGE("out of memory");
        return;
    }
    pthread_mutex_init(&a->lock, NULL);
    act->instance = a;
    act->callbacks->onNativeWindowCreated = on_window_created;
    act->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    act->callbacks->onInputQueueCreated = on_input_created;
    act->callbacks->onInputQueueDestroyed = on_input_destroyed;
    act->callbacks->onDestroy = on_destroy;
    LOGI("onCreate (pointer size %zu)", sizeof(void *));
}
