# Independent Vulkan graphics gate

`tools/native-vulkan-offscreen.c` uses public Vulkan 1.0 APIs and directly loads
an explicitly named ICD. It selects an NVIDIA vendor device, compiles supplied
vertex/fragment SPIR-V into a graphics pipeline, renders a fullscreen triangle
to a 37×19 optimal-tiled RGBA8 image, and copies the image to host-visible memory.

Three push-constant phases change a pixel checker pattern. Every RGBA byte in
all 2,109 rendered pixels must match, and 128 guard bytes around each readback
must remain unchanged. The probe also verifies file-descriptor cleanup and
rejects proprietary NVIDIA userspace or the glibc shim in its process maps.

```sh
mkdir -p build/native-vulkan-offscreen
cc -std=c11 -O2 -Wall -Wextra -Werror tools/native-vulkan-offscreen.c \
  -ldl -o build/native-vulkan-offscreen/probe-host
glslangValidator -V tools/native-vulkan-shaders/offscreen.vert \
  -o build/native-vulkan-offscreen/vertex.spv
glslangValidator -V tools/native-vulkan-shaders/offscreen.frag \
  -o build/native-vulkan-offscreen/fragment.spv
spirv-val build/native-vulkan-offscreen/vertex.spv
spirv-val build/native-vulkan-offscreen/fragment.spv
build/native-vulkan-offscreen/probe-host /absolute/path/to/ICD.so \
  build/native-vulkan-offscreen/vertex.spv build/native-vulkan-offscreen/fragment.spv
```

For Android, compile the same C source with the NDK's
`x86_64-linux-android33-clang`, then run against the Android ICD under bionic.
No Vulkan loader library is required by the probe itself.
After Vulkan teardown, the Android probe explicitly releases liblog's
process-global logging socket through `__android_log_close` before comparing
descriptor counts. This platform resource is independent of Vulkan objects.

An explicit `--stock-control` final argument permits proprietary userspace for
the reference run. It does not change the selected ICD or silently fall back.
The installed NVIDIA driver passed all three phases and guards with fd 4→4.
The source-native driver's results must be recorded separately from this control.

Passing this test establishes offscreen color rendering and image readback for
this path. It does not establish Android presentation or Vulkan conformance.
