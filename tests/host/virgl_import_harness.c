/* Host unit test for vkr_context_import_semaphore_sync_fd() as patches/virglrenderer/0004 adds it to
 * src/venus/vkr_context.c (the import retry merged from upstream v0.2.0 with this fork's lock and device checks).
 * tests/host/test_virgl_import.py writes snippet.c from the patch text. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
typedef pthread_mutex_t mtx_t;
#define mtx_lock pthread_mutex_lock
#define mtx_unlock pthread_mutex_unlock
#define vkr_log(...) do { printf("vkr: " __VA_ARGS__); printf("\n"); } while (0)

struct vkr_object {
   VkObjectType type;
   union { VkDevice device; VkSemaphore semaphore; } handle;
};
struct vkr_device {
   struct vkr_object base;
   struct { PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR; } proc_table;
};
struct vkr_semaphore {
   struct vkr_object base;
   struct vkr_device *device;
};
struct vkr_context {
   mtx_t external_sync_mutex;
   atomic_bool sync_failed;
   bool perf_stats;
   uint64_t perf_imports, perf_import_errors, perf_import_ns;
};

static int64_t
vkr_timeline_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- fake object table: the semaphore appears after `appear_after` lookups ---- */
static struct vkr_context ctx;
static struct vkr_device dev, other_dev;
static struct vkr_semaphore sem;
static int lookups, appear_after, locked_lookups;
static VkResult import_result;
static int imported_fd = -1;
static bool lock_was_free_while_sleeping;

static void *
vkr_context_get_object(struct vkr_context *c, uint64_t id)
{
   /* every lookup must run under the external-sync lock */
   if (pthread_mutex_trylock(&c->external_sync_mutex) == 0)
      pthread_mutex_unlock(&c->external_sync_mutex);
   else
      locked_lookups++;
   if (id == 1)
      return &dev;
   if (id == 2)
      return ++lookups > appear_after ? &sem : NULL;
   if (id == 3)
      return &dev; /* a device id passed as the semaphore: wrong type */
   return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fake_import(VkDevice d, const VkImportSemaphoreFdInfoKHR *info)
{
   (void)d;
   imported_fd = info->fd;
   return import_result;
}

static void *
prober(void *arg)
{
   (void)arg;
   /* while the importer waits for the ring, other imports must be able to take the lock */
   for (int i = 0; i < 2000 && !lock_was_free_while_sleeping; i++) {
      if (pthread_mutex_trylock(&ctx.external_sync_mutex) == 0) {
         if (lookups > 0)
            lock_was_free_while_sleeping = true;
         pthread_mutex_unlock(&ctx.external_sync_mutex);
      }
      usleep(50);
   }
   return NULL;
}

#include "snippet.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool
fd_open(int fd)
{
   return fcntl(fd, F_GETFD) >= 0;
}

static double
run(uint64_t dev_id, uint64_t sem_id, int *fd_out, int *ret_out)
{
   int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
   *fd_out = fd;
   imported_fd = -1;
   const int64_t t0 = vkr_timeline_now_ns();
   *ret_out = vkr_context_import_semaphore_sync_fd(&ctx, dev_id, sem_id, fd);
   return (vkr_timeline_now_ns() - t0) / 1e6;
}

int
main(void)
{
   pthread_mutex_init(&ctx.external_sync_mutex, NULL);
   atomic_init(&ctx.sync_failed, false);
   dev.base.type = VK_OBJECT_TYPE_DEVICE;
   dev.proc_table.ImportSemaphoreFdKHR = fake_import;
   other_dev.base.type = VK_OBJECT_TYPE_DEVICE;
   sem.base.type = VK_OBJECT_TYPE_SEMAPHORE;
   sem.device = &dev;
   int fd, ret;

   /* present at once: imported, the driver owns the fd, no waiting */
   lookups = 0; appear_after = 0; import_result = VK_SUCCESS;
   double ms = run(1, 2, &fd, &ret);
   CHECK(ret == 0 && imported_fd == fd && ms < 5, "immediate import: ret %d, %.2f ms", ret, ms);
   close(fd);

   /* created by a ring command not yet dispatched: found after a few retries, lock free while sleeping */
   lookups = 0; appear_after = 4; lock_was_free_while_sleeping = false;
   pthread_t t;
   pthread_create(&t, NULL, prober, NULL);
   ms = run(1, 2, &fd, &ret);
   pthread_join(t, NULL);
   CHECK(ret == 0 && imported_fd == fd, "late object: ret %d", ret);
   CHECK(lookups == 5, "late object: %d lookups", lookups);
   CHECK(ms >= 1.4 && ms < 50, "late object waited %.2f ms (100+200+400+800 us expected)", ms);
   CHECK(lock_was_free_while_sleeping, "external_sync_mutex held while sleeping");
   close(fd);

   /* never appears: gives up after the bounded backoff and closes the fd */
   lookups = 0; appear_after = 1000;
   ms = run(1, 2, &fd, &ret);
   CHECK(ret == -EINVAL && lookups == 10, "missing object: ret %d after %d lookups", ret, lookups);
   CHECK(ms >= 25 && ms < 200, "missing object gave up after %.2f ms (~25.5 expected)", ms);
   CHECK(!fd_open(fd), "fd must be closed on failure");

   /* wrong type or wrong device never resolves by waiting: immediate failure */
   lookups = 0;
   ms = run(1, 3, &fd, &ret);
   CHECK(ret == -EINVAL && ms < 5 && !fd_open(fd), "wrong type: ret %d after %.2f ms", ret, ms);
   sem.device = &other_dev;
   lookups = 0; appear_after = 0;
   ms = run(1, 2, &fd, &ret);
   CHECK(ret == -EINVAL && ms < 5 && !fd_open(fd), "semaphore of another device: ret %d after %.2f ms", ret, ms);
   sem.device = &dev;

   /* driver refusal closes the fd; device loss also marks the context failed */
   lookups = 0; import_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   run(1, 2, &fd, &ret);
   CHECK(ret == -EINVAL && !fd_open(fd) && !atomic_load(&ctx.sync_failed), "driver refusal: ret %d", ret);
   import_result = VK_ERROR_DEVICE_LOST;
   run(1, 2, &fd, &ret);
   CHECK(ret == -EINVAL && atomic_load(&ctx.sync_failed), "device lost marks the context failed");

   CHECK(locked_lookups > 0 && pthread_mutex_trylock(&ctx.external_sync_mutex) == 0, "lock state");
   printf(failures ? "virgl import harness: %d failure(s)\n" : "virgl import harness: all checks passed\n", failures);
   return failures != 0;
}
