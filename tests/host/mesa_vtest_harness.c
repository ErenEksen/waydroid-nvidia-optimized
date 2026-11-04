/* Host unit test for code patches/mesa/0003 adds to src/virtio/vulkan/vn_renderer_vtest.c.
 * tests/host/test_mesa_vtest.py writes snippet.c (the patch's own text) and builds this file against it. The
 * kernel and the vtest socket are fakes that record what the code asks of them. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <libdrm/drm.h> /* kernel >= 6.16 UAPI: struct drm_syncobj_handle has .point */
#include <vulkan/vulkan.h>

/* ---- what vn_renderer_vtest.c gets from Mesa ---- */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define p_atomic_read(v) __atomic_load_n((v), __ATOMIC_ACQUIRE)
#define p_atomic_set(v, i) __atomic_store_n((v), (i), __ATOMIC_RELEASE)
#define VTEST_HDR_SIZE 2
#define VTEST_CMD_LEN 0
#define VTEST_CMD_ID 1
#define VCMD_SEMAPHORE_IMPORT_SYNC_FD 44
typedef pthread_mutex_t mtx_t;
#define mtx_lock pthread_mutex_lock
#define mtx_unlock pthread_mutex_unlock

struct vn_renderer { int unused; };
struct vtest {
   struct vn_renderer base;
   mtx_t sock_mutex;
   struct {
      int node_fd;
      uint32_t handle;
      uint64_t next_point;
      uint32_t ring_idx;
      bool no_point_export;
   } timeline;
};

/* ---- fake kernel ---- */
enum kernel { KERNEL_616, KERNEL_OLD, KERNEL_POINT_EMFILE, KERNEL_WAIT_FAILS };
static enum kernel kernel;
static unsigned long calls[16];
static unsigned ncalls;
static uint64_t seen_point;
static unsigned seen_size;

static int
fake_ioctl(int fd, unsigned long request, ...)
{
   (void)fd;
   va_list ap;
   va_start(ap, request);
   void *arg = va_arg(ap, void *);
   va_end(ap);
   assert(ncalls < 16);
   calls[ncalls++] = request;
   switch (_IOC_NR(request)) {
   case 0xCA: /* TIMELINE_WAIT */
      if (kernel == KERNEL_WAIT_FAILS) {
         errno = ETIME;
         return -1;
      }
      return 0;
   case 0xBF: /* CREATE */
      ((struct drm_syncobj_create *)arg)->handle = 77;
      return 0;
   case 0xC0: /* DESTROY */
   case 0xCC: /* TRANSFER */
      return 0;
   case 0xC1: { /* HANDLE_TO_FD */
      struct drm_syncobj_handle *h = arg;
      seen_size = _IOC_SIZE(request);
      if (h->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE) {
         if (kernel == KERNEL_OLD) {
            errno = EINVAL;
            return -1;
         }
         if (kernel == KERNEL_POINT_EMFILE) {
            errno = EMFILE;
            return -1;
         }
         seen_point = h->point;
         h->fd = 41;
         return 0;
      }
      h->fd = 42;
      return 0;
   }
   default:
      fprintf(stderr, "unexpected ioctl nr 0x%x\n", (unsigned)_IOC_NR(request));
      abort();
   }
}

/* ---- fake vtest socket ---- */
static unsigned char wire[256];
static size_t wire_len;
static unsigned writes, sent_fds;
static uint32_t reply_status;
static void
vtest_write(struct vtest *vtest, const void *buf, size_t size)
{
   (void)vtest;
   assert(wire_len + size <= sizeof(wire));
   memcpy(wire + wire_len, buf, size);
   wire_len += size;
   writes++;
}
static int
vtest_send_fd(struct vtest *vtest, int fd)
{
   (void)vtest;
   assert(fd == 9);
   sent_fds++;
   return 0;
}
static void
vtest_read(struct vtest *vtest, void *buf, size_t size)
{
   (void)vtest;

   if (size == 2 * sizeof(uint32_t)) {
      const uint32_t hdr[2] = { 1, VCMD_SEMAPHORE_IMPORT_SYNC_FD };
      memcpy(buf, hdr, size);
   } else {
      assert(size == sizeof(uint32_t));
      memcpy(buf, &reply_status, size);
   }

}

#define ioctl fake_ioctl
#include "snippet.c"
#undef ioctl

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
reset(enum kernel k)
{
   kernel = k;
   ncalls = 0;
   seen_point = 0;
   seen_size = 0;
}

static unsigned
nr(unsigned i)
{
   return _IOC_NR(calls[i]);
}

int
main(void)
{
   /* the local UAPI copy is the kernel's: same layout, same ioctl number */
   CHECK(sizeof(struct vtest_syncobj_handle_point) == sizeof(struct drm_syncobj_handle), "struct size");
   CHECK(offsetof(struct vtest_syncobj_handle_point, point) == offsetof(struct drm_syncobj_handle, point), "point offset");
   CHECK(offsetof(struct vtest_syncobj_handle_point, fd) == offsetof(struct drm_syncobj_handle, fd), "fd offset");
   CHECK(VTEST_IOCTL_SYNCOBJ_HANDLE_TO_FD_POINT == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, "ioctl number");
   CHECK(VTEST_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE == DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE, "TIMELINE flag");
   CHECK(VTEST_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE == DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE, "EXPORT flag");

   struct vtest vt = { .timeline = { .node_fd = 5, .handle = 3 } };
   pthread_mutex_init(&vt.sock_mutex, NULL);

   /* kernel >= 6.16: wait for availability, export the point itself; nothing created or destroyed */
   for (int round = 0; round < 2; round++) {
      reset(KERNEL_616);
      int fd = vtest_timeline_export_sync_file(&vt, 1234 + round);
      CHECK(fd == 41, "6.16 export fd %d", fd);
      CHECK(ncalls == 2 && nr(0) == 0xCA && nr(1) == 0xC1, "6.16 sequence: %u ioctls", ncalls);
      CHECK(seen_point == (uint64_t)(1234 + round) && seen_size == 24, "6.16 point %llu size %u",
            (unsigned long long)seen_point, seen_size);
      CHECK(!vt.timeline.no_point_export, "6.16 keeps the point path");
   }

   /* a transient failure of the point export (EMFILE) uses the old sequence once, then tries the point again */
   reset(KERNEL_POINT_EMFILE);
   int fd = vtest_timeline_export_sync_file(&vt, 9);
   CHECK(fd == 42, "EMFILE fallback fd %d", fd);
   CHECK(ncalls == 6 && nr(2) == 0xBF && nr(3) == 0xCC && nr(4) == 0xC1 && nr(5) == 0xC0, "EMFILE sequence");
   CHECK(!vt.timeline.no_point_export, "EMFILE does not disable the point path");

   /* older kernel: EINVAL once, then always create/transfer/export/destroy */
   reset(KERNEL_OLD);
   fd = vtest_timeline_export_sync_file(&vt, 10);
   CHECK(fd == 42, "old kernel fd %d", fd);
   CHECK(ncalls == 6 && nr(0) == 0xCA && nr(1) == 0xC1 && nr(2) == 0xBF && nr(3) == 0xCC && nr(4) == 0xC1 &&
         nr(5) == 0xC0, "old kernel first sequence (%u ioctls)", ncalls);
   CHECK(vt.timeline.no_point_export, "EINVAL is remembered");
   reset(KERNEL_OLD);
   fd = vtest_timeline_export_sync_file(&vt, 11);
   CHECK(fd == 42 && ncalls == 5 && nr(1) == 0xBF, "old kernel later sequence (%u ioctls)", ncalls);

   /* a failed wait is a failed export, with nothing else attempted */
   vt.timeline.no_point_export = false;
   reset(KERNEL_WAIT_FAILS);
   fd = vtest_timeline_export_sync_file(&vt, 12);
   CHECK(fd == -ENOTSUP && ncalls == 1, "wait failure: fd %d, %u ioctls", fd, ncalls);

   /* semaphore import: header and command leave in ONE write, byte-identical to the two-write protocol */
   wire_len = writes = sent_fds = 0;
   reply_status = 0;
   VkResult r = vtest_semaphore_import_sync_fd(&vt.base, 0x1122334455667788ull, 0x99aabbccddeeff00ull, 9);
   const uint32_t expect[6] = { 4, VCMD_SEMAPHORE_IMPORT_SYNC_FD, 0x55667788u, 0x11223344u, 0xddeeff00u, 0x99aabbccu };
   CHECK(r == VK_SUCCESS, "import result %d", r);
   CHECK(writes == 1 && wire_len == sizeof(expect) && !memcmp(wire, expect, sizeof(expect)),
         "import wire: %u writes, %zu bytes", writes, wire_len);
   CHECK(sent_fds == 1, "import sends the fd once");
   reply_status = 1;
   wire_len = writes = sent_fds = 0;
   r = vtest_semaphore_import_sync_fd(&vt.base, 1, 2, 9);
   CHECK(r == VK_ERROR_INVALID_EXTERNAL_HANDLE, "import failure maps to INVALID_EXTERNAL_HANDLE (%d)", r);
   CHECK(pthread_mutex_trylock(&vt.sock_mutex) == 0, "sock_mutex released");

   printf(failures ? "mesa vtest harness: %d failure(s)\n" : "mesa vtest harness: all checks passed\n", failures);
   return failures != 0;
}
