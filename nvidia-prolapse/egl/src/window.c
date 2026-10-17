/* libnvnative-egl-android.so: ANativeWindow surfaces.
 *
 * Buffers are the window's own gralloc buffers (dequeueBuffer), imported once per buffer as driver color
 * buffers (eglPlatformImportColorBufferNVX) and attached as the NVX surface's GL_BACK. Per swap
 * (egl-wayland2 eplWlSwapBuffers + Mesa droid_swap_buffers):
 *   1. the driver's internal eglSwapBuffers on the NVX surface (pre-present work, NVX >= 0.2);
 *   2. acquire fence: eglCreateSync(EGL_SYNC_NATIVE_FENCE_ANDROID) + glFlush + eglDupNativeFenceFDANDROID;
 *   3. queueBuffer(back, fence) - the window owns the fd from here on;
 *   4. buffer ages (Mesa: every presented slot ages by one, the queued one becomes 1);
 *   5. dequeueBuffer(next, release fence), import if new;
 *   6. the release fence is imported as a native fence sync and waited on the GPU (eglWaitSync) in the
 *      rendering context, which is current: the surface is current by definition of eglSwapBuffers;
 *   7. only then eglPlatformSetColorBuffersNVX(GL_BACK, next). NVIDIA's own platforms wait first and attach
 *      second: egl-wayland2 v1.0.2 src/wayland/wayland-surface.c:1556-1570 (the swapchain's GetFreeBuffer waits,
 *      then PlatformSetColorBuffersNVX), egl-x11 v1.0.6 src/x11/x11-window.c:2255-2275 (same order). With this
 *      order no command that SetColorBuffers may emit for the new back buffer can precede the wait
 *      (FENCE-CONTRACT.md §4.1, §7).
 * The wait is correct only because this thread's fence timeline was started when the context was made current
 * (the front's eglMakeCurrent, FENCE-CONTRACT.md §6a): this library calls the driver's eglWaitSync directly and
 * never binds a context or creates a thread of its own.
 * The first buffer is dequeued in eglCreateWindowSurface, where no context of the surface's need be current;
 * its release fence (normally -1 for a fresh buffer) is therefore waited on the CPU.
 * Usage: GRALLOC_USAGE_HW_RENDER, buffer count left to the window (Mesa droid_create_surface). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "nvn.h"

#ifndef GL_BACK
#define GL_BACK 0x0405
#endif

/* EGL_SURFACE_Y_INVERTED_NVX: Android buffers have their first row at the top of the window. Measured with
 * test/host-window (row 0 of the queued buffer, read through an independent dma-buf import, holds the window's
 * top) and on SurfaceFlinger (screencap): see README.md "Measured". */
#ifndef NVN_Y_INVERTED /* the Makefile's measurement variant (host-ymeasure) builds the other value */
#define NVN_Y_INVERTED EGL_TRUE
#endif

static int cpu_wait(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    for (;;) {
        int r = poll(&p, 1, -1);
        if (r > 0) return 0;
        if (r < 0 && errno == EINTR) continue;
        return -1;
    }
}

static void slot_release(EplPlatformData *plat, EGLDisplay idpy, struct nvn_slot *s) {
    if (s->cb != NULL && !plat->destroyed) {
        plat->priv->nvx.FreeColorBuffer(idpy, s->cb);
    }
    if (s->anwb != NULL) {
        s->anwb->common.decRef(&s->anwb->common);
    }
    memset(s, 0, sizeof(*s));
}

/* The cached driver color buffer for `b`, importing it when the window hands us a buffer we have not seen. */
static struct nvn_slot *slot_for(EplPlatformData *plat, EplDisplay *pdpy, EplImplSurface *ps, ANativeWindowBuffer *b,
                                 const char *who) {
    EGLDisplay idpy = pdpy->internal_display;
    for (int i = 0; i < NVN_MAX_SLOTS; i++) {
        struct nvn_slot *s = &ps->slots[i];
        if (s->anwb == b && s->handle == b->handle && s->width == b->width && s->height == b->height) {
            s->used = ++ps->stamp;
            return s;
        }
    }
    struct nvn_dmabuf d;
    char why[512], fcc[5];
    if (nvn_gralloc_decode(b, &d, why, sizeof(why)) != 0) {
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: %s", who, why);
        return NULL;
    }
    /* A buffer of a new size: the window reallocated its buffers (resize); drop the old ones. */
    for (int i = 0; i < NVN_MAX_SLOTS; i++) {
        struct nvn_slot *s = &ps->slots[i];
        if (s->anwb != NULL && s != ps->back && (s->width != d.width || s->height != d.height)) {
            slot_release(plat, idpy, s);
        }
    }
    struct nvn_slot *victim = NULL;
    for (int i = 0; i < NVN_MAX_SLOTS; i++) {
        struct nvn_slot *s = &ps->slots[i];
        if (s->anwb == NULL) {
            victim = s;
            break;
        }
        if (s != ps->back && (victim == NULL || s->used < victim->used)) {
            victim = s;
        }
    }
    if (victim->anwb != NULL) {
        slot_release(plat, idpy, victim);
    }
    EGLPlatformColorBufferNVX cb = plat->priv->nvx.ImportColorBuffer(idpy, d.fd, d.width, d.height, (int)d.fourcc,
                                                                     (int)d.stride, (int)d.offset, d.modifier);
    if (cb == NULL) {
        EGLint e = plat->egl.GetError();
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: eglPlatformImportColorBufferNVX refused the window buffer (fourcc %s, "
                  "%dx%d, stride %u, offset %u, modifier 0x%016llx; driver error 0x%04x)", who,
                  nvn_fourcc_str(d.fourcc, fcc), d.width, d.height, d.stride, d.offset, (unsigned long long)d.modifier, e);
        return NULL;
    }
    b->common.incRef(&b->common);
    victim->anwb = b;
    victim->handle = b->handle;
    victim->cb = cb;
    victim->width = d.width;
    victim->height = d.height;
    victim->fourcc = d.fourcc;
    victim->modifier = d.modifier;
    victim->age = 0;
    victim->used = ++ps->stamp;
    nvn_log(NVN_LOG_DEBUG, "%s: imported buffer %p (fourcc %s, %dx%d, stride %u, modifier 0x%016llx)", who, (void *)b,
            nvn_fourcc_str(d.fourcc, fcc), d.width, d.height, d.stride, (unsigned long long)d.modifier);
    return victim;
}

/* An acquire fence for everything the current context has submitted: -1 on failure (error already set). */
static int make_fence(EplPlatformData *plat, EGLDisplay idpy, const char *who) {
    EGLSync s = plat->priv->egl.CreateSync(idpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (s == EGL_NO_SYNC) {
        nvn_error(plat, EGL_BAD_ALLOC, "%s: eglCreateSync(EGL_SYNC_NATIVE_FENCE_ANDROID) failed (driver error 0x%04x)",
                  who, plat->egl.GetError());
        return -1;
    }
    plat->priv->egl.Flush();
    int fd = plat->priv->egl.DupNativeFenceFD(idpy, s);
    plat->priv->egl.DestroySync(idpy, s);
    if (fd < 0) {
        nvn_error(plat, EGL_BAD_ALLOC, "%s: eglDupNativeFenceFDANDROID returned no fd (driver error 0x%04x)", who,
                  plat->egl.GetError());
    }
    return fd;
}

/* Makes the current context wait (on the GPU) for a release fence. Takes ownership of fd. On failure the error is
 * set and logged and EGL_FALSE returned; the caller still holds its own duplicate of the fence for the buffer. */
static EGLBoolean gpu_wait(EplPlatformData *plat, EGLDisplay idpy, int fd, const char *who) {
#ifdef NVN_TEST_SKIP_SIGNALLED_WAIT /* stress variant only (README "Tests"): never masks an early release */
    {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 0) == 1) {
            close(fd);
            return EGL_TRUE;
        }
    }
#endif
    EGLAttrib a[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
    EGLSync s = plat->priv->egl.CreateSync(idpy, EGL_SYNC_NATIVE_FENCE_ANDROID, a);
    if (s == EGL_NO_SYNC) {
        EGLint e = plat->egl.GetError();
        close(fd); /* not adopted by the driver */
        nvn_error(plat, EGL_BAD_ALLOC, "%s: importing the dequeue fence as EGL_SYNC_NATIVE_FENCE_ANDROID failed (driver "
                  "error 0x%04x)", who, e);
        return EGL_FALSE;
    }
    EGLBoolean ok = plat->priv->egl.WaitSync(idpy, s, 0);
    EGLint e = ok ? EGL_SUCCESS : plat->egl.GetError();
    plat->priv->egl.DestroySync(idpy, s);
    if (!ok) {
        nvn_error(plat, EGL_BAD_ALLOC, "%s: eglWaitSync on the dequeue fence failed (driver error 0x%04x)", who, e);
    }
    return ok;
}

EGLSurface nvnCreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf, EGLConfig config,
                                  void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform,
                                  const struct glvnd_list *existing_surfaces) {
    static const char WHO[] = "eglCreateWindowSurface";
    (void)create_platform;
    EplImplDisplay *dp = pdpy->priv;
    struct ANativeWindow *win = native_surface;
    EGLDisplay idpy = pdpy->internal_display;
    EGLSurface internal = EGL_NO_SURFACE;
    EGLAttrib *drv = NULL;
    EplImplSurface *ps = NULL;
    ANativeWindowBuffer *b = NULL;
    int fence = -1;

    if (win == NULL || win->common.magic != (int)NVN_ANDROID_NATIVE_WINDOW_MAGIC) {
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: %p is not an ANativeWindow (magic 0x%08x)", WHO, (void *)win,
                  win ? (unsigned)win->common.magic : 0u);
        return EGL_NO_SURFACE;
    }
    const EplSurface *other;
    glvnd_list_for_each_entry(other, existing_surfaces, entry) {
        if (other->type == EPL_SURFACE_TYPE_WINDOW && other->priv != NULL && other->priv->window == win) {
            nvn_error(plat, EGL_BAD_ALLOC, "%s: ANativeWindow %p already has an EGLSurface", WHO, (void *)win);
            return EGL_NO_SURFACE;
        }
    }
    const EplConfig *ci = eplConfigListFind(dp->configs, config);
    if (ci == NULL) {
        nvn_error(plat, EGL_BAD_CONFIG, "%s: invalid EGLConfig %p", WHO, config);
        return EGL_NO_SURFACE;
    }
    if (!(ci->surfaceMask & EGL_WINDOW_BIT)) {
        nvn_error(plat, EGL_BAD_MATCH, "%s: EGLConfig %p is not a window config", WHO, config);
        return EGL_NO_SURFACE;
    }

    /* Driver attributes: the application's, minus the ones this platform owns, plus the orientation. */
    EGLint n = eplCountAttribs(attribs), m = 0;
    drv = malloc(((size_t)n + 3) * sizeof(EGLAttrib));
    ps = calloc(1, sizeof(*ps));
    if (drv == NULL || ps == NULL) {
        nvn_error(plat, EGL_BAD_ALLOC, "out of memory");
        goto fail;
    }
    for (EGLint i = 0; attribs != NULL && attribs[i] != EGL_NONE; i += 2) {
        if (attribs[i] == EGL_SURFACE_Y_INVERTED_NVX) {
            nvn_error(plat, EGL_BAD_ATTRIBUTE, "%s: invalid attribute 0x%04x", WHO, (unsigned)attribs[i]);
            goto fail;
        } else if (attribs[i] == EGL_RENDER_BUFFER) {
            if (attribs[i + 1] == EGL_SINGLE_BUFFER) {
                /* A hint for windows (EGL 1.5 §3.5.1); EGL_KHR_mutable_render_buffer is not advertised. */
                nvn_log(NVN_LOG_WARN, "%s: EGL_SINGLE_BUFFER requested; front-buffer rendering is not implemented, the "
                        "surface is back-buffered", WHO);
            } else if (attribs[i + 1] != EGL_BACK_BUFFER) {
                nvn_error(plat, EGL_BAD_ATTRIBUTE, "%s: invalid EGL_RENDER_BUFFER 0x%04x", WHO, (unsigned)attribs[i + 1]);
                goto fail;
            }
        } else {
            drv[m++] = attribs[i];
            drv[m++] = attribs[i + 1];
        }
    }
    drv[m++] = EGL_SURFACE_Y_INVERTED_NVX;
    drv[m++] = NVN_Y_INVERTED;
    drv[m] = EGL_NONE;

    ps->window = win;
    win->common.incRef(&win->common);
    psurf->priv = ps;

    int r = win->perform(win, NVN_NATIVE_WINDOW_SET_USAGE64, (uint64_t)GRALLOC_USAGE_HW_RENDER);
    if (r != 0) {
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: ANativeWindow set usage HW_RENDER failed: %s (%d)", WHO,
                  strerror(-r), r);
        goto fail;
    }
    r = win->dequeueBuffer(win, &b, &fence);
    if (r != 0 || b == NULL) {
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: ANativeWindow dequeueBuffer failed: %s (%d)", WHO, strerror(-r), r);
        b = NULL;
        goto fail;
    }
    if (fence >= 0) {
        cpu_wait(fence);
        close(fence);
        fence = -1;
    }
    struct nvn_slot *s = slot_for(plat, pdpy, ps, b, WHO);
    if (s == NULL) {
        goto fail;
    }
    ps->back = s;
    EGLAttrib pattr[] = {GL_BACK, (EGLAttrib)s->cb, EGL_NONE};
    internal = plat->priv->nvx.CreateSurface(idpy, config, pattr, drv);
    if (internal == EGL_NO_SURFACE) {
        char fcc[5], cfcc[5];
        EGLint e = plat->egl.GetError();
        nvn_error(plat, e == EGL_SUCCESS ? EGL_BAD_ALLOC : e, "%s: eglPlatformCreateSurfaceNVX refused config %p (fourcc "
                  "%s) with buffer fourcc %s %dx%d modifier 0x%016llx (driver error 0x%04x)", WHO, config,
                  nvn_fourcc_str(ci->fourcc, cfcc), nvn_fourcc_str(s->fourcc, fcc), s->width, s->height,
                  (unsigned long long)s->modifier, e);
        goto fail;
    }
    free(drv);
    {
        char fcc[5];
        int minund = -1;
        win->query(win, NVN_NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &minund);
        nvn_log(NVN_LOG_INFO, "window surface on ANativeWindow %p: %dx%d fourcc %s modifier 0x%016llx, min undequeued %d",
                (void *)win, s->width, s->height, nvn_fourcc_str(s->fourcc, fcc), (unsigned long long)s->modifier, minund);
    }
    return internal;

fail:
    if (ps != NULL) {
        if (b != NULL) {
            win->cancelBuffer(win, b, fence);
            fence = -1;
        }
        for (int i = 0; i < NVN_MAX_SLOTS; i++) {
            if (ps->slots[i].anwb != NULL) slot_release(plat, idpy, &ps->slots[i]);
        }
        if (ps->window != NULL) {
            ps->window->common.decRef(&ps->window->common);
        }
        free(ps);
    }
    if (fence >= 0) close(fence);
    psurf->priv = NULL;
    free(drv);
    return EGL_NO_SURFACE;
}

void nvnDestroySurface(EplDisplay *pdpy, EplSurface *psurf, const struct glvnd_list *existing_surfaces) {
    (void)existing_surfaces;
    EplImplSurface *ps = psurf->priv;
    if (ps == NULL) {
        return;
    }
    EplPlatformData *plat = pdpy->platform;
    EGLDisplay idpy = pdpy->internal_display;
    int fence = -1;
    if (!plat->destroyed) {
        /* Rendering since the last swap may still be in flight: hand the buffer back with a fence for it when the
         * surface is current here (the only case in which this thread can have submitted to it). */
        if (ps->back != NULL && !ps->lost && plat->egl.GetCurrentSurface(EGL_DRAW) == psurf->external_surface) {
            fence = make_fence(plat, idpy, "eglDestroySurface");
        }
        if (psurf->internal_surface != EGL_NO_SURFACE) {
            plat->egl.DestroySurface(idpy, psurf->internal_surface);
        }
    }
    if (ps->back != NULL && !ps->lost) {
        int r = ps->window->cancelBuffer(ps->window, ps->back->anwb, fence);
        if (r != 0) {
            nvn_log(NVN_LOG_WARN, "eglDestroySurface: ANativeWindow cancelBuffer failed: %s (%d)", strerror(-r), r);
        }
    } else if (fence >= 0) {
        close(fence);
    }
    for (int i = 0; i < NVN_MAX_SLOTS; i++) {
        if (ps->slots[i].anwb != NULL) slot_release(plat, idpy, &ps->slots[i]);
    }
    ps->window->common.decRef(&ps->window->common);
    free(ps);
    psurf->priv = NULL;
}

EGLBoolean nvnSwapBuffers(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf, const EGLint *rects,
                          EGLint n_rects) {
    static const char WHO[] = "eglSwapBuffers";
    /* Damage: the Android loader already passed it to the window (native_window_set_surface_damage,
     * egl_platform_entries.cpp:1312-1376); the whole back buffer is queued. */
    (void)rects;
    (void)n_rects;
    EplImplSurface *ps = psurf->priv;
    EGLDisplay idpy = pdpy->internal_display;
    struct ANativeWindow *win = ps->window;
    if (ps->lost || ps->back == NULL) {
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: the surface has no window buffer (an earlier dequeueBuffer failed)",
                  WHO);
        return EGL_FALSE;
    }
    if (!plat->egl.SwapBuffers(idpy, psurf->internal_surface)) {
        nvn_error(plat, plat->egl.GetError(), "%s: the driver's internal swap (NVX pre-present work) failed", WHO);
        return EGL_FALSE;
    }
    int fd = make_fence(plat, idpy, WHO);
    if (fd < 0) {
        return EGL_FALSE;
    }
    int r = win->queueBuffer(win, ps->back->anwb, fd);
    if (r != 0) {
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: ANativeWindow queueBuffer failed: %s (%d)", WHO, strerror(-r), r);
        return EGL_FALSE;
    }
    for (int i = 0; i < NVN_MAX_SLOTS; i++) {
        if (ps->slots[i].anwb != NULL && ps->slots[i].age > 0) ps->slots[i].age++;
    }
    ps->back->age = 1;

    ANativeWindowBuffer *b = NULL;
    int fence = -1;
    r = win->dequeueBuffer(win, &b, &fence);
    if (r != 0 || b == NULL) {
        ps->lost = EGL_TRUE;
        ps->back = NULL;
        nvn_error(plat, EGL_BAD_NATIVE_WINDOW, "%s: ANativeWindow dequeueBuffer failed: %s (%d); the surface is lost", WHO,
                  strerror(-r), r);
        return EGL_FALSE;
    }
    struct nvn_slot *s = slot_for(plat, pdpy, ps, b, WHO);
    EGLBoolean attached = EGL_FALSE;
    if (s != NULL) {
        /* Wait first, attach second (header step 7; egl-wayland2 wayland-surface.c:1556-1570, egl-x11
         * x11-window.c:2255-2275). The GPU wait consumes `fence`; `fence` keeps a duplicate so that a failure from
         * here on can hand the buffer back with the consumer's release fence still attached. */
        int wait_fd = -1;
        EGLBoolean waited = EGL_TRUE;
        if (fence >= 0) {
            wait_fd = fcntl(fence, F_DUPFD_CLOEXEC, 0);
            if (wait_fd < 0) {
                nvn_error(plat, EGL_BAD_ALLOC, "%s: duplicating the dequeue fence failed: %s", WHO, strerror(errno));
                waited = EGL_FALSE;
            } else {
                waited = gpu_wait(plat, idpy, wait_fd, WHO);
            }
        }
        if (waited) {
            EGLAttrib buffers[] = {GL_BACK, (EGLAttrib)s->cb, EGL_NONE};
            attached = plat->priv->nvx.SetColorBuffers(idpy, psurf->internal_surface, buffers);
            if (!attached) {
                nvn_error(plat, EGL_BAD_ALLOC, "%s: eglPlatformSetColorBuffersNVX(GL_BACK, %dx%d) failed (driver error "
                          "0x%04x)", WHO, s->width, s->height, plat->egl.GetError());
            }
        }
    }
    if (!attached) {
        win->cancelBuffer(win, b, fence); /* the window owns the release fence again */
        ps->lost = EGL_TRUE;
        ps->back = NULL;
        return EGL_FALSE;
    }
    if (fence >= 0) {
        close(fence);
    }
    ps->back = s;
    return EGL_TRUE;
}

EGLBoolean nvnSwapInterval(EplDisplay *pdpy, EplSurface *psurf, EGLint interval) {
    struct ANativeWindow *win = psurf->priv->window;
    if (interval < win->minSwapInterval) interval = win->minSwapInterval;
    if (interval > win->maxSwapInterval) interval = win->maxSwapInterval;
    int r = win->setSwapInterval(win, interval);
    if (r != 0) {
        nvn_error(pdpy->platform, EGL_BAD_NATIVE_WINDOW, "eglSwapInterval(%d): ANativeWindow setSwapInterval failed: "
                  "%s (%d)", interval, strerror(-r), r);
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EplQueryResult nvnQuerySurface(EplDisplay *pdpy, EplSurface *psurf, EGLint attrib, EGLint *value) {
    (void)pdpy;
    EplImplSurface *ps = psurf->priv;
    switch (attrib) {
    case EGL_BUFFER_AGE_KHR:
        *value = ps->back != NULL ? ps->back->age : 0;
        return EPL_QUERY_RESULT_SUCCESS;
    case EGL_WIDTH:
    case EGL_HEIGHT:
        if (ps->back == NULL) return EPL_QUERY_RESULT_UNKNOWN;
        *value = attrib == EGL_WIDTH ? ps->back->width : ps->back->height;
        return EPL_QUERY_RESULT_SUCCESS;
    default:
        return EPL_QUERY_RESULT_UNKNOWN;
    }
}
