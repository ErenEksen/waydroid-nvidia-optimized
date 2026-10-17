/* sf_client.h: the few libgui/libbinder/libutils entry points eglwindow needs to get a real SurfaceFlinger layer.
 *
 * ABI-FRAGILE BY DESIGN (a test tool, not shipped): these are hand-written declarations of C++ classes of
 * Android 13 (frameworks/native android13-release 2fe7eddf, libs/gui/include/gui/SurfaceComposerClient.h,
 * SurfaceControl.h, LayerMetadata.h; libs/binder ProcessState.h; system/core libutils RefBase.h, String8.h).
 * The real headers cannot be used with the NDK: they pull in AIDL-generated headers and the platform's libc++
 * (std::__1) types, while the NDK's libc++ is std::__ndk1. So only non-virtual member functions whose mangled
 * names contain no std:: type are called, objects are created by their exported constructors into
 * over-sized storage, and nothing is ever destroyed (sp<> here never drops a reference; the process exits).
 * Every mangled name used here was checked against the guest's /system/lib{,64} with llvm-nm -D (README.md).
 * Must be rebuilt (and re-checked) for another Android release. */
#pragma once
#include <stdint.h>

struct AHardwareBuffer;
struct ANativeWindowBuffer;

namespace android {

typedef int32_t status_t;

class RefBase {
  public:
    void incStrong(const void *id) const;
    void decStrong(const void *id) const;

  protected:
    virtual ~RefBase(); /* RefBase is dynamic: it is the primary base of everything below (Itanium C++ ABI) */

  private:
    void *mRefs;
};

class String8 {
  public:
    explicit String8(const char *s);
    ~String8();

  private:
    const char *mString;
};

/* sp<T> is one pointer; the user-provided destructor makes it non-trivial for calls (returned/passed indirectly,
 * as the real one). It never drops the reference. */
template <typename T> class sp {
  public:
    sp() : m_ptr(nullptr) {}
    explicit sp(T *p) : m_ptr(p) {}
    ~sp() {}
    T *get() const { return m_ptr; }

  private:
    T *m_ptr;
};

class IBinder;
/* ui/ANativeObjectBase.h: template <NATIVE_TYPE, TYPE, REF> class ANativeObjectBase : public NATIVE_TYPE, public REF.
 * For Surface the first *dynamic* base, RefBase, becomes the primary base at offset 0, so the ANativeWindow
 * subobject sits after it: the upcast below must be done by the compiler, not by a reinterpret_cast. */
template <typename NATIVE_TYPE, typename TYPE, typename REF> class ANativeObjectBase : public NATIVE_TYPE, public REF {};
class Surface : public ANativeObjectBase<ANativeWindow, Surface, RefBase> {};

class SurfaceControl {
  public:
    sp<Surface> getSurface();
};

struct LayerMetadata {
    LayerMetadata(); /* exported; the real object is a Parcelable vptr + std::unordered_map */
    ~LayerMetadata() {}
    alignas(16) unsigned char storage[256];
};

namespace ui {
struct LayerStack {
    uint32_t id;
};
} // namespace ui

class SurfaceComposerClient {
  public:
    enum { eFXSurfaceBufferState = 0x00040000 }; /* ISurfaceComposerClient.h:50 */
    SurfaceComposerClient();
    status_t initCheck() const;
    sp<SurfaceControl> createSurface(const String8 &name, uint32_t w, uint32_t h, int32_t format, uint32_t flags,
                                     const sp<IBinder> &parentHandle, LayerMetadata metadata, uint32_t *outTransformHint);
    class Transaction {
      public:
        Transaction();
        Transaction &setLayer(const sp<SurfaceControl> &sc, int32_t z);
        Transaction &setPosition(const sp<SurfaceControl> &sc, float x, float y);
        Transaction &setLayerStack(const sp<SurfaceControl> &sc, ui::LayerStack stack);
        Transaction &show(const sp<SurfaceControl> &sc);
        status_t apply(bool synchronous, bool oneWay);
        alignas(16) unsigned char storage[16384];
    };
    alignas(16) unsigned char storage[1024];
};

class ProcessState {
  public:
    static sp<ProcessState> self();
    void startThreadPool();
};

ANativeWindowBuffer *AHardwareBuffer_to_ANativeWindowBuffer(AHardwareBuffer *buffer); /* libnativewindow, platform */

} // namespace android
