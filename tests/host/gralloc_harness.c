/* Host unit test for src/minigbm-vtest/vtest_wrapper.c (the guest gralloc backend) against a scripted vtest
 * server on a unix socket that speaks the host renderer's VCMD_RESOURCE_ALLOC_GPU protocol (patches/virglrenderer
 * 0004, vtest_resource_alloc_gpu). Run by tests/host/test_gralloc.py. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "gbm_mesa_wrapper.h"

struct gbm_ops *get_gbm_ops(void);

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define ABGR8888 FOURCC('A', 'B', '2', '4')
#define NV12 FOURCC('N', 'V', '1', '2')
#define YUYV FOURCC('Y', 'U', 'Y', 'V')

static char sock_path[108];
int
__system_property_get(const char *name, char *value)
{
   if (strcmp(name, "mesa.vtest.socket.name"))
      return 0;
   strcpy(value, sock_path);
   return (int)strlen(value);
}
int
__android_log_print(int prio, const char *tag, const char *fmt, ...)
{
   (void)prio; (void)tag; (void)fmt;
   return 0;
}

/* ---- scripted server ---- */
struct request { uint32_t width, height, format, flags; };
static struct request seen[32];
static int nseen, connections;
/* per request: status to answer; -1 = close the connection without answering */
static int script[32];
static int listen_fd;

static int
read_all(int fd, void *buf, size_t n)
{
   char *p = buf;
   while (n) {
      ssize_t r = read(fd, p, n);
      if (r <= 0)
         return -1;
      p += r;
      n -= (size_t)r;
   }
   return 0;
}

static void
send_fd(int sock, int fd)
{
   char byte = 0;
   struct iovec iov = { &byte, 1 };
   char ctrl[CMSG_SPACE(sizeof(int))];
   struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctrl, .msg_controllen = sizeof(ctrl) };
   struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
   c->cmsg_level = SOL_SOCKET;
   c->cmsg_type = SCM_RIGHTS;
   c->cmsg_len = CMSG_LEN(sizeof(int));
   memcpy(CMSG_DATA(c), &fd, sizeof(int));
   if (sendmsg(sock, &msg, MSG_NOSIGNAL) != 1)
      abort();
}

static void *
server(void *arg)
{
   (void)arg;
   for (;;) {
      int c = accept(listen_fd, NULL, NULL);
      if (c < 0)
         return NULL;
      connections++;
      uint32_t hdr[2];
      char name[64];
      if (read_all(c, hdr, sizeof(hdr)) || hdr[1] != 8 || hdr[0] > sizeof(name) || read_all(c, name, hdr[0]) ||
          memcmp(name, "gralloc", 8)) {
         fprintf(stderr, "server: bad CREATE_RENDERER\n");
         abort();
      }
      for (;;) {
         uint32_t req[6];
         if (read_all(c, req, sizeof(req)))
            break;
         if (req[0] != 4 || req[1] != 41) {
            fprintf(stderr, "server: bad ALLOC header %u %u\n", req[0], req[1]);
            abort();
         }
         const int n = nseen++;
         seen[n] = (struct request){ req[2], req[3], req[4], req[5] };
         if (script[n] < 0) {
            close(c);
            c = -1;
            break;
         }
         uint32_t resp[9] = { 7, 41, (uint32_t)script[n], 4096, 4096, 0x00606010u, 0x03000000u, 65536, 0 };
         if (write(c, resp, sizeof(resp)) != sizeof(resp))
            abort();
         if (!script[n]) {
            int fd = memfd_create("dmabuf", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, 65536))
               abort();
            send_fd(c, fd);
            close(fd);
         }
      }
      if (c >= 0)
         close(c);
   }
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int
main(int argc, char **argv)
{
   if (argc != 2 || strlen(argv[1]) >= sizeof(sock_path))
      return 2;
   strcpy(sock_path, argv[1]);
   listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   strcpy(addr.sun_path, sock_path);
   if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(listen_fd, 8))
      return 2;
   pthread_t t;
   pthread_create(&t, NULL, server, NULL);

   struct gbm_ops *ops = get_gbm_ops();
   CHECK(ops->get_gbm_format(NV12) == NV12, "NV12 must be a supported format (issue #16)");
   CHECK(ops->get_gbm_format(ABGR8888) == ABGR8888, "ABGR8888 supported");
   CHECK(ops->get_gbm_format(YUYV) == 0, "YUYV goes to minigbm's blob path");

   struct gbm_device *dev = ops->dev_create(-1);
   script[0] = 0; /* GPU image */
   struct alloc_args a = { .gbm = dev, .width = 1920, .height = 1080, .drm_format = ABGR8888 };
   int r = ops->alloc(&a);
   CHECK(r == 0 && a.out_fd >= 0, "GPU alloc: %d", r);
   CHECK(seen[0].flags == 0 && seen[0].width == 1920 && seen[0].height == 1080 && seen[0].format == ABGR8888,
         "GPU alloc request flags %u", seen[0].flags);
   CHECK(a.out_stride == 4096 && a.out_map_stride == 4096 && a.out_modifier == 0x0300000000606010ull,
         "GPU alloc outputs: stride %u modifier %llx", a.out_stride, (unsigned long long)a.out_modifier);

   /* a video decoder's NV12 buffer carries neither SW flag and must still be linear/mappable */
   script[1] = 0;
   struct alloc_args v = { .gbm = dev, .width = 1280, .height = 720, .drm_format = NV12 };
   r = ops->alloc(&v);
   CHECK(r == 0 && seen[1].flags == 1, "NV12 alloc: ret %d flags %u (MAPPABLE expected)", r, seen[1].flags);

   script[2] = 0;
   struct alloc_args s = { .gbm = dev, .width = 64, .height = 64, .drm_format = ABGR8888, .use_scanout = true,
                           .needs_map_stride = true };
   r = ops->alloc(&s);
   CHECK(r == 0 && seen[2].flags == 3, "scanout+map: flags %u", seen[2].flags);

   /* host refusal (e.g. the allocation rate limit's ENOSPC) is returned, the connection is kept */
   script[3] = ENOSPC;
   struct alloc_args e = { .gbm = dev, .width = 64, .height = 64, .drm_format = ABGR8888 };
   r = ops->alloc(&e);
   CHECK(r == -ENOSPC, "host status: %d", r);
   CHECK(connections == 1, "refusal must not reconnect (%d connections)", connections);

   /* a dropped connection is reconnected once and the request retried */
   script[4] = -1;
   script[5] = 0;
   struct alloc_args d = { .gbm = dev, .width = 32, .height = 32, .drm_format = ABGR8888 };
   r = ops->alloc(&d);
   CHECK(r == 0 && connections == 2 && nseen == 6, "reconnect: ret %d, %d connections, %d requests", r,
         connections, nseen);

   /* import + map: the mapping is created once and kept until free */
   struct gbm_bo *bo = ops->import(dev, v.out_fd, 1280, 720, 4096, 0, NV12);
   CHECK(bo != NULL, "import");
   void *addr1 = NULL, *addr2 = NULL, *md = NULL;
   ops->map(bo, 1280, 720, &addr1, &md);
   ops->unmap(bo, md);
   ops->map(bo, 1280, 720, &addr2, &md);
   CHECK(addr1 && addr1 == addr2, "map is cached");
   if (addr1)
      memset(addr1, 0xab, 65536);
   ops->free(bo);
   close(a.out_fd);
   close(v.out_fd);
   close(s.out_fd);
   close(d.out_fd);
   ops->dev_destroy(dev);

   printf(failures ? "gralloc harness: %d failure(s)\n" : "gralloc harness: all checks passed\n", failures);
   return failures != 0;
}
