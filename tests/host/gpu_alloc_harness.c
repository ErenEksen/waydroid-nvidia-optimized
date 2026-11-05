/* Host unit test for src/virglrenderer-vtest/vtest_gpu_alloc.c (the host renderer's gralloc allocator), built
 * from the repository file itself. Vulkan is tests/host/mock_vulkan.c loaded as libvulkan.so.1; /dev/udmabuf is
 * faked through the linker's --wrap=open,--wrap=ioctl. Run by tests/host/test_gpu_alloc.py. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/udmabuf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vtest_gpu_alloc.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define ABGR8888 FOURCC('A', 'B', '2', '4')
#define XRGB8888 FOURCC('X', 'R', '2', '4')
#define RGB565 FOURCC('R', 'G', '1', '6')
#define NV12 FOURCC('N', 'V', '1', '2')
#define YUYV FOURCC('Y', 'U', 'Y', 'V')

/* ---- fake /dev/udmabuf ---- */
enum { UDMA_OK, UDMA_MISSING, UDMA_DENIED };
static int udma_mode = UDMA_OK, udma_fd = -1, udma_creates, udma_bad;
int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long req, ...);

int
__wrap_open(const char *path, int flags, ...)
{
   if (strcmp(path, "/dev/udmabuf"))
      return __real_open(path, flags, 0);
   if (udma_mode == UDMA_MISSING) {
      errno = ENOENT;
      return -1;
   }
   if (udma_mode == UDMA_DENIED) {
      errno = EACCES;
      return -1;
   }
   udma_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
   return udma_fd;
}

int
__wrap_ioctl(int fd, unsigned long req, ...)
{
   va_list ap;
   va_start(ap, req);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   if (fd != udma_fd || req != UDMABUF_CREATE)
      return __real_ioctl(fd, req, arg);
   const struct udmabuf_create *c = arg;
   struct stat st;
   const int seals = fcntl((int)c->memfd, F_GET_SEALS);
   /* what the kernel requires: a shrink-sealed memfd covering offset+size, page granular */
   if (seals < 0 || !(seals & F_SEAL_SHRINK) || fstat((int)c->memfd, &st) || (uint64_t)st.st_size < c->offset + c->size ||
       c->size % 4096 || c->offset % 4096 || !(c->flags & UDMABUF_FLAGS_CLOEXEC)) {
      udma_bad++;
      errno = EINVAL;
      return -1;
   }
   udma_creates++;
   return fcntl((int)c->memfd, F_DUPFD_CLOEXEC, 0);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int
open_fds(void)
{
   int n = 0;
   for (int fd = 0; fd < 1024; fd++)
      n += fcntl(fd, F_GETFD) >= 0;
   return n;
}

static void
cpu_case(uint32_t w, uint32_t h, uint32_t fmt, uint32_t want_stride, uint64_t want_size)
{
   uint32_t stride = 0;
   uint64_t size = 0;
   int fd = -1;
   const int ret = vtest_gpu_alloc_cpu(w, h, fmt, &stride, &size, &fd);
   CHECK(ret == 0, "cpu %ux%u: ret %d", w, h, ret);
   if (ret)
      return;
   struct stat st;
   CHECK(!fstat(fd, &st) && (uint64_t)st.st_size == size, "cpu %ux%u: fd size %lld vs %" PRIu64, w, h,
         (long long)st.st_size, size);
   CHECK(stride == want_stride && size == want_size, "cpu %ux%u fmt %08x: stride %u size %" PRIu64
         " (want %u %" PRIu64 ")", w, h, fmt, stride, size, want_stride, want_size);
   if (fmt == NV12) {
      /* minigbm's biplanar_yuv_420_layout: Y at 0, UV at stride*h, UV size stride*ceil(h/2) */
      CHECK(size >= (uint64_t)stride * h + (uint64_t)stride * ((h + 1) / 2), "NV12 %ux%u: UV plane out of bounds", w, h);
      CHECK(stride >= w, "NV12 stride below width");
   }
   close(fd);
}

int
main(void)
{
   /* ---- CPU (udmabuf) path ---- */
   const int fds_before = open_fds();
   cpu_case(1920, 1080, ABGR8888, 7680, 8294400);
   cpu_case(1080, 2400, RGB565, 2304, 5529600);
   cpu_case(1920, 1080, NV12, 2048, 3317760);
   cpu_case(1279, 721, NV12, 1280, 1388544);  /* odd sizes: chroma rows round up */
   cpu_case(1, 1, NV12, 256, 4096);
   CHECK(udma_bad == 0, "udmabuf contract violated %d times", udma_bad);
   uint32_t stride;
   uint64_t size;
   int fd;
   CHECK(vtest_gpu_alloc_cpu(0, 16, ABGR8888, &stride, &size, &fd) == -EINVAL, "zero width");
   CHECK(vtest_gpu_alloc_cpu(16, 0, NV12, &stride, &size, &fd) == -EINVAL, "zero height");
   CHECK(vtest_gpu_alloc_cpu(UINT32_MAX, 2, ABGR8888, &stride, &size, &fd) == -EINVAL, "stride overflow");
   CHECK(vtest_gpu_alloc_cpu(UINT32_MAX, 2, NV12, &stride, &size, &fd) == -EINVAL, "NV12 stride overflow");
   udma_mode = UDMA_MISSING;
   CHECK(vtest_gpu_alloc_cpu(64, 64, ABGR8888, &stride, &size, &fd) == -ENOENT, "no udmabuf -> -ENOENT");
   udma_mode = UDMA_DENIED;
   CHECK(vtest_gpu_alloc_cpu(64, 64, ABGR8888, &stride, &size, &fd) == -EACCES, "denied udmabuf -> -EACCES");
   udma_mode = UDMA_OK;
   if (udma_fd >= 0)
      close(udma_fd), udma_fd = -1;
   CHECK(open_fds() == fds_before, "CPU path leaks fds (%d -> %d)", fds_before, open_fds());

   /* ---- GPU (Vulkan, mock) path ---- */
   uint64_t modifier = 0;
   CHECK(vtest_gpu_alloc_gpu(64, 64, YUYV, &stride, &modifier, &size, &fd) == -EINVAL, "unknown format");
   int ret = vtest_gpu_alloc_gpu(1920, 1080, ABGR8888, &stride, &modifier, &size, &fd);
   CHECK(ret == 0, "gpu alloc ret %d", ret);
   void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
   CHECK(lib != NULL, "mock libvulkan not loaded by the allocator");
   if (!lib) {
      printf("gpu alloc harness: %d failure(s)\n", failures);
      return 1;
   }
   unsigned *queries = dlsym(lib, "mock_format_queries");
   unsigned *images = dlsym(lib, "mock_images_live");
   unsigned *memories = dlsym(lib, "mock_memories_live");
   unsigned *fail_image = dlsym(lib, "mock_create_image_fail");
   uint64_t *mods = dlsym(lib, "mock_last_modifiers");
   unsigned *nmods = dlsym(lib, "mock_last_modifier_count");
   uint32_t *memtype = dlsym(lib, "mock_last_memory_type");

   /* only single-plane, renderable+samplable, non-LINEAR modifiers are offered */
   CHECK(*nmods == 2 && mods[0] == 0x0300000000606010ull && mods[1] == 0x0300000000606014ull,
         "modifier candidates: %u", *nmods);
   CHECK(*memtype == 1, "device-local memory type expected, got %u", *memtype);
   CHECK(stride == 8192 && modifier == 0x0300000000606010ull && size == (1 << 20) && fd >= 0, "gpu outputs");
   CHECK(*images == 0 && *memories == 0, "transient Vulkan objects leaked (%u images, %u memories)", *images, *memories);
   close(fd);
   const unsigned first = *queries;
   CHECK(first == 2, "first allocation of a format queries its modifiers once (2 calls), got %u", first);

   /* (4) the modifier list is cached per format: no driver query for the same format again */
   for (int i = 0; i < 5; i++) {
      ret = vtest_gpu_alloc_gpu(640 + i, 480, ABGR8888, &stride, &modifier, &size, &fd);
      CHECK(ret == 0, "repeat alloc %d", i);
      if (!ret)
         close(fd);
   }
   CHECK(*queries == first, "cached format re-queried (%u calls)", *queries);
   CHECK(*nmods == 2, "cached candidates differ");
   /* XRGB8888 maps to another VkFormat: one more query, then cached too */
   ret = vtest_gpu_alloc_gpu(64, 64, XRGB8888, &stride, &modifier, &size, &fd);
   CHECK(ret == 0 && *queries == first + 2, "second format: %u calls", *queries);
   if (!ret)
      close(fd);
   ret = vtest_gpu_alloc_gpu(64, 64, XRGB8888, &stride, &modifier, &size, &fd);
   CHECK(ret == 0 && *queries == first + 2, "second format cached: %u calls", *queries);
   if (!ret)
      close(fd);

   /* a failing driver call returns an error, frees everything and releases the lock */
   *fail_image = 1;
   CHECK(vtest_gpu_alloc_gpu(64, 64, ABGR8888, &stride, &modifier, &size, &fd) == -EINVAL, "CreateImage failure");
   *fail_image = 0;
   ret = vtest_gpu_alloc_gpu(64, 64, ABGR8888, &stride, &modifier, &size, &fd);
   CHECK(ret == 0, "allocation after a failure (lock released?)");
   if (!ret)
      close(fd);
   CHECK(*images == 0 && *memories == 0, "leak after failure");

   printf(failures ? "gpu alloc harness: %d failure(s)\n" : "gpu alloc harness: all checks passed\n", failures);
   return failures != 0;
}
