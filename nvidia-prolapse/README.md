# nvidia-prolapse: NVIDIA GPU drivers inside Android

The driver parts that don't depend on Waydroid. They run in Android (bionic) processes and talk to the host's
`/dev/nvidia*` directly. Waydroid integration lives in `../waydroid/` and `../vendor-image/`.

Default mode (`nvidia-prolapse`):

- `driver-source/native/`: the Rust RM backend, NVK/NAK patches, the `nvrm` GLES driver and the NVDEC video
  backend. Start with `driver-source/native/README.md`.
- `media/`: the Codec2 decoder service on that video backend.
- `gralloc/`: the minigbm `gbm_mesa` backend that allocates NVIDIA-displayable buffers (both native modes).
  `gralloc/README.md`.

`glibc-shim` mode, which loads the host's unmodified NVIDIA libraries (glibc builds) into bionic processes:

- `shim/`: the glibc-to-bionic adapter libraries. `shim/DESIGN.md` explains how they work.
- `hal/`: `vulkan.nvidia.so`, the Android Vulkan HAL in front of NVIDIA's ICD, with its own
  `VK_ANDROID_native_buffer` and ASTC emulation. `hal/AHB-CONTRACT.md`.
- `egl/`: NVIDIA's own EGL/GLES as an Android EGL driver, with an external platform for Android windows.
  `egl/README.md`.
- `libdrm/`: upstream libdrm built for bionic; NVIDIA's ICD needs it for dma-buf export.
- `probe/`: small compute and GLES probes for checking a setup.

Build and test, per component:

```sh
make -C shim && make -C shim ARCH=x86        # 64- and 32-bit
make -C hal && make -C hal test              # host + Android builds, real-GPU pixel tests
make -C gralloc && make -C gralloc android32
make -C libdrm && make -C libdrm ARCH=x86
```

In `glibc-shim` mode NVIDIA's libraries are loaded unmodified from the host and must match the kernel module's
version exactly. In every mode a failure is loud and names a remedy; nothing falls back silently.
