/* libGLESv1_CM_nvnative.so / libGLESv2_nvnative.so: intentionally empty.
 *
 * Android's loader resolves every GL name with dlsym(this library) first and then with the EGL library's
 * eglGetProcAddress (Loader.cpp:316-376, init_api); NVIDIA's getProcAddress returns directly callable GL
 * functions (NV-EGL-CONTRACT §1.4), so libEGL_nvnative.so answers them all. These libraries exist because the
 * loader opens all three lib*_<suffix>.so files (Loader.cpp:611-623). They must never export
 * ANGLEGetDisplayPlatform (that would mark the driver as ANGLE, Loader.cpp:555-564); `make check` enforces that
 * they export nothing. */
typedef int nvn_gles_empty_translation_unit;
