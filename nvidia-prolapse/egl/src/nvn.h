/* libnvnative-egl-android.so: internal header.
 *
 * An NVIDIA EGL external platform (eglexternalplatform 1.2 + the driver's "platform surface" NVX
 * interface 0.2) that implements EGL_PLATFORM_ANDROID_KHR: window surfaces on an ANativeWindow's
 * gralloc dma-bufs and EGL_ANDROID_image_native_buffer. The display/surface bookkeeping is NVIDIA's
 * platform base (egl-x11 v1.0.6 src/base, Apache-2.0, built from the pinned clone; see Makefile).
 * Driver-side contract: build/p7/NV-EGL-CONTRACT.md; Android-side contract: ANDROID-EGL-CONTRACT.md. */
#ifndef NVN_H
#define NVN_H

#include <stdint.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "platform-base.h"
#include "platform-impl.h"
#include "platform-utils.h"
#include "config-list.h"
#include "driver-platform-surface.h"

#include <nvn_android.h>

/* NVX platform-surface interface: major 0, minor >= 2 (internal eglSwapBuffers before present,
 * driver-platform-surface.h:58-67). The header says neither backward nor forward compatibility is
 * guaranteed, so anything else is refused at load. */
#define NVN_NVX_MIN_MINOR EGL_PLATFORM_SURFACE_INTERNAL_SWAP_SINCE
/* eglexternalplatform interface major the library is built for (eglexternalplatformversion.h). */
#define NVN_PLATFORM_MAJOR 1

#define NVN_MAX_SLOTS 8

enum nvn_prio { NVN_LOG_DEBUG, NVN_LOG_INFO, NVN_LOG_WARN, NVN_LOG_ERROR };
void nvn_log(enum nvn_prio prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int nvn_debug_on(void);
/* Logs at ERROR (logcat "nvnative-egl" / stderr) and sets the EGL error through the driver. */
void nvn_error(EplPlatformData *plat, EGLint error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

struct _EplImplPlatform {
    EGLint nvx_version;
    struct {
        pfn_eglPlatformImportColorBufferNVX ImportColorBuffer;
        pfn_eglPlatformFreeColorBufferNVX FreeColorBuffer;
        pfn_eglPlatformCreateSurfaceNVX CreateSurface;
        pfn_eglPlatformSetColorBuffersNVX SetColorBuffers;
        pfn_eglPlatformGetConfigAttribNVX GetConfigAttrib;
    } nvx;
    struct {
        PFNEGLCREATESYNCPROC CreateSync;
        PFNEGLDESTROYSYNCPROC DestroySync;
        PFNEGLWAITSYNCPROC WaitSync;
        PFNEGLDUPNATIVEFENCEFDANDROIDPROC DupNativeFenceFD;
        PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
        PFNEGLCREATEIMAGEPROC CreateImage;
        PFNEGLQUERYDMABUFFORMATSEXTPROC QueryDmaBufFormats;
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC QueryDmaBufModifiers;
        void (*Flush)(void);
    } egl;
};

/* Per-config Android attributes that the driver does not know (EplConfig has surfaceMask and
 * nativeVisualID/Type; these are the two EGL_ANDROID_* booleans). */
struct nvn_config_extra {
    EGLBoolean recordable;
    EGLBoolean framebuffer_target;
};

struct _EplImplDisplay {
    EGLDeviceEXT device;
    EplInternalDisplay *idpy;
    EplConfigList *configs;
    struct nvn_config_extra *extra; /* parallel to configs->configs */
    char *extension_string;
};

/* One gralloc buffer the window has handed us, imported as a driver color buffer. */
struct nvn_slot {
    ANativeWindowBuffer *anwb; /* holds a reference (common.incRef) while cached */
    const native_handle_t *handle;
    EGLPlatformColorBufferNVX cb;
    int width, height;
    uint32_t fourcc;
    uint64_t modifier;
    int age;       /* EGL_EXT_buffer_age: 0 = never presented */
    uint64_t used; /* LRU stamp */
};

struct _EplImplSurface {
    struct ANativeWindow *window; /* holds a reference (common.incRef) */
    struct nvn_slot slots[NVN_MAX_SLOTS];
    struct nvn_slot *back; /* dequeued from the window and attached as GL_BACK */
    uint64_t stamp;
    EGLBoolean lost; /* a dequeue failed after a queue: the surface has no back buffer */
};

/* platform.c */
EplImplDisplay *nvnDisplayPriv(EplDisplay *pdpy);

/* window.c */
EGLSurface nvnCreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf, EGLConfig config,
                                  void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform,
                                  const struct glvnd_list *existing_surfaces);
void nvnDestroySurface(EplDisplay *pdpy, EplSurface *psurf, const struct glvnd_list *existing_surfaces);
EGLBoolean nvnSwapBuffers(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf, const EGLint *rects,
                          EGLint n_rects);
EGLBoolean nvnSwapInterval(EplDisplay *pdpy, EplSurface *psurf, EGLint interval);
EplQueryResult nvnQuerySurface(EplDisplay *pdpy, EplSurface *psurf, EGLint attrib, EGLint *value);

/* gralloc.c: an ANativeWindowBuffer from minigbm (cros_gralloc_handle) as one dma-buf plane */
struct nvn_dmabuf {
    int fd;
    int width, height;
    uint32_t fourcc;
    uint32_t stride, offset;
    uint64_t modifier;
};
/* Returns 0, or -1 with a human-readable reason (naming the buffer's facts) in why. */
int nvn_gralloc_decode(const ANativeWindowBuffer *b, struct nvn_dmabuf *out, char *why, size_t whyn);
const char *nvn_fourcc_str(uint32_t fourcc, char buf[5]);
/* HAL pixel format for a DRM fourcc that minigbm produces (cros_gralloc_helpers.cc), or 0. */
int nvn_hal_format_for_fourcc(uint32_t fourcc);

#endif
