/* QMD CWD_MEMBAR_TYPE of the built NVK: calls its exported nak_fill_qmd for a minimal compute shader on
 * Blackwell A (cdc0) and Blackwell B (cec0) and checks QMDV05_00 CWD_MEMBAR_TYPE (MW(625:624),
 * clcdc0qmd.h): L1_MEMBAR (3) on cec0, as NVIDIA's driver launches every cec0 QMD, L1_NONE (0) on cdc0.
 * Usage: nak-qmd-membar LIBVULKAN_NOUVEAU.so. Build/run: tests/README.md "nak-qmd-membar".
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "nv_device_info.h"
#include "nak.h"
int main(int argc, char **argv) {
   void *h = dlopen(argv[1], RTLD_NOW);
   if (!h) { printf("%s\n", dlerror()); return 2; }
   void (*fill)(const struct nv_device_info *, const struct nak_shader_info *, const struct nak_qmd_info *, void *, size_t) = dlsym(h, "nak_fill_qmd");
   uint32_t (*size)(const struct nv_device_info *) = dlsym(h, "nak_qmd_size_B");
   int bad = 0;
   for (int k = 0; k < 2; k++) {
      struct nv_device_info dev; memset(&dev, 0, sizeof(dev));
      dev.cls_compute = k ? 0xcec0 : 0xcdc0; dev.cls_eng3d = k ? 0xce97 : 0xcd97;
      dev.sm = k ? 120 : 100; dev.max_smem_per_wg_kB = 48; dev.sm_smem_size_count = 2; dev.sm_smem_sizes_kB[0] = 32; dev.sm_smem_sizes_kB[1] = 100;
      struct nak_shader_info info; memset(&info, 0, sizeof(info));
      info.stage = MESA_SHADER_COMPUTE; info.max_warps_per_sm = 48; info.num_gprs = 16;
      info.cs.local_size[0] = 64; info.cs.local_size[1] = 1; info.cs.local_size[2] = 1;
      struct nak_qmd_info qi; memset(&qi, 0, sizeof(qi));
      qi.addr = 0x100000; qi.global_size[0] = 4; qi.global_size[1] = 1; qi.global_size[2] = 1;
      uint32_t q[96] = {0};
      uint32_t sz = size(&dev);
      fill(&dev, &info, &qi, q, sz);
      unsigned m = (q[19] >> 16) & 3;
      printf("cls_compute 0x%x qmd %u B: CWD_MEMBAR_TYPE = %u (dword19 = 0x%08x)\n", dev.cls_compute, sz, m, q[19]);
      if (m != (k ? 3u : 0u)) bad = 1;
   }
   printf("%s\n", bad ? "FAIL" : "PASS");
   return bad;
}
