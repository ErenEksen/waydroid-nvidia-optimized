# driver-source

Only `native/` is published here: the Rust RM backend (`libnv_bionic_native.so`, built
directly against bionic), the NVK/NAK Mesa patches and the RM adapter (`native/mesa/`),
our own gallium GLES driver `nvrm` (`native/mesa/gallium-nvrm/`), the NAK compiler bridge
(`native/compiler/`), the NVDEC video backend (`native/src/video/`), build scripts and tests.

It talks to the host's NVIDIA kernel module through its public RM/UVM/NVKMS interfaces
(the open kernel module headers and Mesa's NVIDIA class headers).

`.gitignore` keeps everything in this directory except `README.md` and `native/` out of git.

Start with [native/README.md](native/README.md) and [native/mesa/README.md](native/mesa/README.md).
