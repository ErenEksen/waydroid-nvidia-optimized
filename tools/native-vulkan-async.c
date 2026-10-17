/* Independent asynchronous Vulkan SYNC_FD regression; no private driver ABI.
 * Build: cc -std=c11 -Wall -Wextra -Werror tools/native-vulkan-async.c
 *           -ldl -pthread -o PROBE
 * Run: PROBE --driver /absolute/ICD.so --gate-fd N --release-fd M
 *      [--allow-stock-driver]
 * See native-vulkan-async.md for the producer protocol.
 *
 * The launcher supplies a genuinely pending external Linux sync_file and a pipe
 * write end. One byte on the pipe asks it to signal that fence. Both inherited
 * descriptors are borrowed. The launcher MUST also release its fence after two
 * seconds, even if this process crashes. The probe's own 750 ms watchdog
 * releases it if a driver blocks submit/export. No GPU spin loop or long
 * workload is used.
 *
 * Unlike a host VkEvent gate (invalid while a command buffer waits on it), an
 * imported external semaphore is a legal already-submitted producer dependency.
 * https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphoreGetFdInfoKHR.html
 * https://docs.vulkan.org/refpages/latest/refpages/source/vkSetEvent.html
 * The GPU marker is read only after completion; unsignaled sync_files prove
 * that export returned before completion without racing host reads against GPU
 * writes.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "native-vulkan-runtime.h"

#define DEVICE_FUNCTIONS(X)                                                    \
  X(DestroyDevice)                                                             \
  X(GetDeviceQueue)                                                            \
  X(CreateBuffer)                                                              \
  X(DestroyBuffer)                                                             \
  X(GetBufferMemoryRequirements)                                               \
  X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory)             \
      X(UnmapMemory) X(CreateCommandPool) X(DestroyCommandPool)                \
          X(AllocateCommandBuffers) X(FreeCommandBuffers)                      \
              X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdFillBuffer)       \
                  X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence)         \
                      X(WaitForFences) X(ResetFences) X(GetFenceStatus)        \
                          X(GetFenceFdKHR) X(CreateSemaphore)                  \
                              X(DestroySemaphore) X(GetSemaphoreFdKHR)         \
                                  X(ImportSemaphoreFdKHR) X(QueueSubmit)       \
                                      X(DeviceWaitIdle)
struct Device {
  VkDevice handle;
  VkQueue queue;
  VkCommandPool pool;

#define DECLARE(name) PFN_vk##name name;
  DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
};
struct Buffer {
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkDeviceSize allocation_size;
  uint32_t *mapped;
};
static VkPhysicalDeviceMemoryProperties memory_properties;
static uint32_t queue_family;
static void release_gate(void);
static void require(int good, const char *operation) {
  if (!good) {
    fprintf(stderr, "FAIL: %s (errno %d: %s)\n", operation, errno,
            strerror(errno));
    release_gate();
    exit(1);
  }
}
static void result(VkResult value, const char *operation) {
  if (value != VK_SUCCESS) {
    fprintf(stderr, "FAIL: %s returned %d\n", operation, value);
    release_gate();
    exit(1);
  }
}
#define CHECK(call) result((call), #call)
static int fd_count(void) {
  DIR *d = opendir("/proc/self/fd");
  require(d != NULL, "count fd");
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)))
    if (e->d_name[0] != '.')
      n++;
  closedir(d);
  return n;
}
struct FdEntry {
  int fd;
  char *target;
  struct FdEntry *next;
};
static struct FdEntry *fd_snapshot(void) {
  struct FdEntry *head = NULL;
  DIR *directory = opendir("/proc/self/fd");
  require(directory != NULL, "fd diagnostic snapshot");
  struct dirent *entry;
  while ((entry = readdir(directory))) {
    if (entry->d_name[0] == '.')
      continue;
    int fd = atoi(entry->d_name);
    if (fd == dirfd(directory))
      continue;
    char path[512], target[4096];
    snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    if (len < 0)
      continue;
    target[len] = 0;
    struct FdEntry *item = malloc(sizeof(*item));
    require(item != NULL, "fd diagnostic allocation");
    item->fd = fd;
    item->target = strdup(target);
    require(item->target != NULL, "fd diagnostic target");
    item->next = head;
    head = item;
  }
  closedir(directory);
  return head;
}
static void fd_snapshot_finish(struct FdEntry *head, const char *label,
                               int print) {
  while (head) {
    struct FdEntry *next = head->next;
    if (print)
      fprintf(stderr, "fd %s: %d -> %s\n", label, head->fd, head->target);
    free(head->target);
    free(head);
    head = next;
  }
}
static void init_device(struct Device *d, PFN_vkCreateDevice create,
                        PFN_vkGetDeviceProcAddr resolve,
                        VkPhysicalDevice physical) {
  float priority = 1;
  VkDeviceQueueCreateInfo q = {.sType =
                                   VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                               .queueFamilyIndex = queue_family,
                               .queueCount = 1,
                               .pQueuePriorities = &priority};
  const char *extensions[] = {"VK_KHR_external_fence_fd",
                              "VK_KHR_external_semaphore_fd"};
  VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &q,
                             .enabledExtensionCount = 2,
                             .ppEnabledExtensionNames = extensions};
  CHECK(create(physical, &info, NULL, &d->handle));
#define LOAD(name)                                                             \
  d->name = (PFN_vk##name)resolve(d->handle, "vk" #name);                      \
  require(d->name != NULL, "vk" #name);
  DEVICE_FUNCTIONS(LOAD)
#undef LOAD
  d->GetDeviceQueue(d->handle, queue_family, 0, &d->queue);
  require(d->queue != VK_NULL_HANDLE, "compute queue");
  VkCommandPoolCreateInfo pool = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = queue_family};
  CHECK(d->CreateCommandPool(d->handle, &pool, NULL, &d->pool));
}
static struct Buffer create_buffer(struct Device *d) {
  struct Buffer b = {0};
  VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 16384,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
  CHECK(d->CreateBuffer(d->handle, &info, NULL, &b.buffer));
  VkMemoryRequirements req;
  d->GetBufferMemoryRequirements(d->handle, b.buffer, &req);
  uint32_t type = UINT32_MAX;
  const VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
    if ((req.memoryTypeBits & (1u << i)) &&
        (memory_properties.memoryTypes[i].propertyFlags & needed) == needed) {
      type = i;
      break;
    }
  require(type != UINT32_MAX, "host coherent memory type");
  VkMemoryAllocateInfo allocate = {.sType =
                                       VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = type};
  CHECK(d->AllocateMemory(d->handle, &allocate, NULL, &b.memory));
  b.allocation_size = req.size;
  CHECK(d->BindBufferMemory(d->handle, b.buffer, b.memory, 0));
  CHECK(d->MapMemory(d->handle, b.memory, 0, req.size, 0, (void **)&b.mapped));
  for (size_t i = 0; i < req.size / 4; i++)
    b.mapped[i] = 0xa55aa55a;
  return b;
}
static void destroy_buffer(struct Device *d, struct Buffer *b) {
  if (b->mapped)
    d->UnmapMemory(d->handle, b->memory);
  d->DestroyBuffer(d->handle, b->buffer, NULL);
  d->FreeMemory(d->handle, b->memory, NULL);
}
static VkFence fence(struct Device *d, int external) {
  VkExportFenceCreateInfo exp = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
  VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                            .pNext = external ? &exp : NULL};
  VkFence f = VK_NULL_HANDLE;
  CHECK(d->CreateFence(d->handle, &info, NULL, &f));
  return f;
}
static VkSemaphore semaphore_handles(struct Device *d, VkExternalSemaphoreHandleTypeFlags handles) {
  VkExportSemaphoreCreateInfo exp = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = handles};
  VkSemaphoreCreateInfo info = {.sType =
                                    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                .pNext = handles ? &exp : NULL};
  VkSemaphore s = VK_NULL_HANDLE;
  CHECK(d->CreateSemaphore(d->handle, &info, NULL, &s));
  return s;
}
static VkSemaphore semaphore(struct Device *d, int external) {
  return semaphore_handles(d, external ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT : 0);
}
static void maps_check(int allow_stock) {
  FILE *f = fopen("/proc/self/maps", "r");
  require(f != NULL, "runtime maps");
  char line[4096];
  while (fgets(line, sizeof(line), f))
    if (!allow_stock &&
        (strstr(line, "libnvidia-") || strstr(line, "libEGL_nvidia") ||
         strstr(line, "libnvglibc"))) {
      fprintf(stderr, "FAIL: unexpected proprietary runtime mapping: %s", line);
      release_gate();
      exit(1);
    }
  require(!ferror(f), "read maps");
  fclose(f);
}

static int release_fd = -1;
static atomic_int released;
static pthread_mutex_t watchdog_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t watchdog_condition;
static int watchdog_finished;
static void release_gate(void) {
  if (release_fd < 0 || atomic_exchange(&released, 1))
    return;
  char byte = 'R';
  ssize_t written;
  do {
    written = write(release_fd, &byte, 1);
  } while (written < 0 && errno == EINTR);
  if (written != 1)
    fprintf(stderr, "FAIL: release pipe write: %s\n", strerror(errno));
}
static void *watchdog(void *unused) {
  (void)unused;
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_nsec += 750000000;
  if (deadline.tv_nsec >= 1000000000) {
    deadline.tv_nsec -= 1000000000;
    deadline.tv_sec++;
  }
  pthread_mutex_lock(&watchdog_mutex);
  while (!watchdog_finished) {
    int ret =
        pthread_cond_timedwait(&watchdog_condition, &watchdog_mutex, &deadline);
    if (ret) {
      fprintf(stderr, "WATCHDOG: releasing external gate after 750 ms; "
                      "pending-export gate failed\n");
      release_gate();
      break;
    }
  }
  pthread_mutex_unlock(&watchdog_mutex);
  return NULL;
}
static int fd_ready(int fd, int timeout_ms) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  int ret;
  do {
    ret = poll(&p, 1, timeout_ms);
  } while (ret < 0 && errno == EINTR);
  require(ret >= 0 && !(p.revents & (POLLERR | POLLNVAL)), "sync_file poll");
  return ret == 1 && (p.revents & POLLIN);
}
static int export_semaphore_handle(struct Device *d, VkSemaphore s,
                                   VkExternalSemaphoreHandleTypeFlagBits type) {
  VkSemaphoreGetFdInfoKHR get = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .semaphore = s,
      .handleType = type};
  int fd = -2;
  CHECK(d->GetSemaphoreFdKHR(d->handle, &get, &fd));
  return fd;
}
static int export_semaphore(struct Device *d, VkSemaphore s) {
  return export_semaphore_handle(d, s, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
}
static int export_fence(struct Device *d, VkFence f) {
  VkFenceGetFdInfoKHR get = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
                             .fence = f,
                             .handleType =
                                 VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
  int fd = -2;
  CHECK(d->GetFenceFdKHR(d->handle, &get, &fd));
  return fd;
}
static void import_semaphore(struct Device *d, VkSemaphore s, int borrowed) {
  int copied = fcntl(borrowed, F_DUPFD_CLOEXEC, 0);
  require(copied >= 0, "duplicate borrowed sync_file");
  VkImportSemaphoreFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
      .semaphore = s,
      .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .fd = copied};
  CHECK(d->ImportSemaphoreFdKHR(d->handle, &info));
}
static VkCommandBuffer marker_command(struct Device *d, struct Buffer *b,
                                      uint32_t word, uint32_t value) {
  VkCommandBuffer command;
  VkCommandBufferAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = d->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  CHECK(d->AllocateCommandBuffers(d->handle, &alloc, &command));
  VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  CHECK(d->BeginCommandBuffer(command, &begin));
  d->CmdFillBuffer(command, b->buffer, word * 4, 4, value);
  VkBufferMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = b->buffer,
      .offset = word * 4,
      .size = 4};
  d->CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0,
                        NULL);
  CHECK(d->EndCommandBuffer(command));
  return command;
}
static void submit(struct Device *d, VkCommandBuffer *cmd, VkSemaphore *wait,
                   VkSemaphore *signal, VkFence f) {
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = cmd ? 1 : 0,
                       .pCommandBuffers = cmd,
                       .waitSemaphoreCount = wait ? 1 : 0,
                       .pWaitSemaphores = wait,
                       .pWaitDstStageMask = wait ? &stage : NULL,
                       .signalSemaphoreCount = signal ? 1 : 0,
                       .pSignalSemaphores = signal};
  CHECK(d->QueueSubmit(d->queue, 1, &info, f));
}
static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1000000.0;
}
static int pending_gate(struct Device *d, struct Buffer *b, int gate_fd,
                        int error_gate, struct Device *other) {
  const VkExternalSemaphoreHandleTypeFlags conversion_types =
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT | VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkSemaphore gate = semaphore(d, 0), rendered = semaphore(d, 0),
              exported = semaphore_handles(d, other ? conversion_types : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
  VkSemaphore imported[2] = {semaphore(d, 0), semaphore(d, 0)};
  VkFence reusable = fence(d, 1), complete = fence(d, 0);
  VkCommandBuffer commands[4];
  for (unsigned i = 0; i < 4; i++)
    commands[i] = marker_command(d, b, i * 2, 0x71c00000u + i);
  struct Buffer converted_markers = {0};
  VkSemaphore conversion_source[2] = {0}, conversion_import[2] = {0}, conversion_wait[2] = {0};
  VkFence conversion_complete = VK_NULL_HANDLE;
  VkCommandBuffer conversion_commands[2];
  int conversion_fds[2] = {-1, -1};
  if (other) {
    converted_markers = create_buffer(other);
    conversion_complete = fence(other, 0);
    for (unsigned i = 0; i < 2; i++) {
      conversion_source[i] = semaphore_handles(d, conversion_types);
      conversion_import[i] = semaphore_handles(other, conversion_types);
      conversion_wait[i] = semaphore(other, 0);
      conversion_commands[i] = marker_command(other, &converted_markers, i * 2, 0xc0110000u + i);
    }
  }
  import_semaphore(d, gate, gate_fd);
  require(!fd_ready(gate_fd, 0),
          "external gate must still be genuinely pending before submit");
  pthread_condattr_t attr;
  require(!pthread_condattr_init(&attr), "watchdog condition attributes");
  require(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC),
          "watchdog monotonic clock");
  require(!pthread_cond_init(&watchdog_condition, &attr), "watchdog condition");
  pthread_condattr_destroy(&attr);
  pthread_t thread;
  require(!pthread_create(&thread, NULL, watchdog, NULL), "watchdog thread");
  int sem_fds[2], fence_fds[2], pending = 1;
  double started = now_ms();
  for (unsigned i = 0; i < 2; i++) {
    submit(d, &commands[i], i == 0 ? &gate : NULL, &rendered, VK_NULL_HANDLE);
    /* Matches Android release: wait for rendered binary semaphore, then export
     * the completion. Export resets both payloads, permitting immediate reuse.
     */
    submit(d, NULL, &rendered, &exported, reusable);
    sem_fds[i] = export_semaphore(d, exported);
    fence_fds[i] = export_fence(d, reusable);
    pending &= sem_fds[i] >= 0 && fence_fds[i] >= 0;
    if (sem_fds[i] >= 0)
      pending &= !fd_ready(sem_fds[i], 0);
    if (fence_fds[i] >= 0)
      pending &= !fd_ready(fence_fds[i], 0);
    pending &= d->GetFenceStatus(d->handle, reusable) == VK_NOT_READY;
    printf("queued export %u: semaphore fd=%d fence fd=%d pending=%d after "
           "%.3f ms\n",
           i + 1, sem_fds[i], fence_fds[i], pending, now_ms() - started);
  }
  if (other) {
    for (unsigned i = 0; i < 2; i++) {
      require(sem_fds[i] >= 0, "conversion starts from a real pending sync_file");
      /* SYNC_FD copy imports are always temporary (VUID07307). Both export
       * types were enabled on the source payload, not only the import target. */
      import_semaphore(d, conversion_source[i], sem_fds[i]);
      int opaque_fd = export_semaphore_handle(d, conversion_source[i],
                         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
      require(opaque_fd >= 0, "converted OPAQUE_FD descriptor");
      VkImportSemaphoreFdInfoKHR import = {
          .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
          .semaphore = conversion_import[i],
          .flags = i ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0,
          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
          .fd = opaque_fd};
      CHECK(other->ImportSemaphoreFdKHR(other->handle, &import));
      /* No queue waits on this imported opaque semaphore. Copy export is its
       * one consumption; only the resulting independent SYNC_FD is GPU-waited. */
      conversion_fds[i] = export_semaphore(other, conversion_import[i]);
      require(conversion_fds[i] >= 0, "round-trip conversion returns a real pending sync_file");
      pending &= !fd_ready(conversion_fds[i], 0) && !atomic_load(&released);
      struct sync_file_info info = {0};
      require(ioctl(conversion_fds[i], SYNC_IOC_FILE_INFO, &info) == 0 && info.status == 0,
              "round-trip sync_file initially pending");
      import_semaphore(other, conversion_wait[i], conversion_fds[i]);
      submit(other, &conversion_commands[i], &conversion_wait[i], NULL,
             i == 1 ? conversion_complete : VK_NULL_HANDLE);
      printf("converted payload %u: SYNC_FD -> temporary -> OPAQUE_FD -> %s other-device import -> SYNC_FD=%d pending=%d after %.3f ms\n",
             i + 1, i ? "temporary" : "permanent", conversion_fds[i], pending, now_ms() - started);
    }
    require(other->WaitForFences(other->handle, 1, &conversion_complete, VK_TRUE,
                                UINT64_C(1000000)) == VK_TIMEOUT,
            "converted GPU dependencies remain pending before producer release");
  }
  /* Keep both old exported payloads alive while their Vulkan objects already
   * carry new payloads. Import duplicates and consume the old signals on GPU.
   */
  for (unsigned i = 0; i < 2; i++) {
    if (sem_fds[i] >= 0)
      import_semaphore(d, imported[i], sem_fds[i]);
    else {
      VkImportSemaphoreFdInfoKHR info = {
          .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
          .semaphore = imported[i],
          .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
          .fd = -1};
      CHECK(d->ImportSemaphoreFdKHR(d->handle, &info));
    }
    submit(d, &commands[i + 2], &imported[i], NULL,
           i == 1 ? complete : VK_NULL_HANDLE);
  }
  pending &= !atomic_load(&released) && !fd_ready(gate_fd, 0);
  release_gate();
  pthread_mutex_lock(&watchdog_mutex);
  watchdog_finished = 1;
  pthread_cond_signal(&watchdog_condition);
  pthread_mutex_unlock(&watchdog_mutex);
  require(!pthread_join(thread, NULL), "watchdog join");
  pthread_cond_destroy(&watchdog_condition);
  VkResult completion =
      d->WaitForFences(d->handle, 1, &complete, VK_TRUE, UINT64_C(5000000000));
  if (error_gate) {
    printf("errored gate: WaitForFences returned %d\n", completion);
    require(completion == VK_ERROR_DEVICE_LOST,
            "incoming fence error must become VK_ERROR_DEVICE_LOST");
    struct sync_file_info info = {0};
    require(ioctl(gate_fd, SYNC_IOC_FILE_INFO, &info) == 0 && info.status < 0,
            "external fixture actually delivered a fence error");
    printf("incoming sync_file status=%d\n", info.status);
    require(d->DeviceWaitIdle(d->handle) == VK_ERROR_DEVICE_LOST,
            "device loss remains visible at DeviceWaitIdle");
  } else {
    CHECK(completion);
    for (unsigned i = 0; i < 2; i++) {
      if (sem_fds[i] >= 0)
        require(fd_ready(sem_fds[i], 5000), "old semaphore payload completed");
      if (fence_fds[i] >= 0)
        require(fd_ready(fence_fds[i], 5000), "old fence payload completed");
    }
    require(d->GetFenceStatus(d->handle, reusable) == VK_NOT_READY,
            "exported fence stays reset after both old payloads complete");
    for (unsigned i = 0; i < 4; i++)
      require(b->mapped[i * 2] == 0x71c00000u + i,
              "pending-gate GPU-written marker");
    CHECK(d->DeviceWaitIdle(d->handle));
    if (other) {
      CHECK(other->WaitForFences(other->handle, 1, &conversion_complete, VK_TRUE,
                                UINT64_C(5000000000)));
      for (unsigned i = 0; i < 2; i++) {
        require(fd_ready(conversion_fds[i], 5000), "converted sync_file completed");
        struct sync_file_info info = {0};
        require(ioctl(conversion_fds[i], SYNC_IOC_FILE_INFO, &info) == 0 && info.status > 0,
                "converted sync_file completed successfully");
      }
      for (unsigned i = 0; i < converted_markers.allocation_size / 4; i++) {
        uint32_t expected = i == 0 || i == 2 ? 0xc0110000u + i / 2 : 0xa55aa55a;
        require(converted_markers.mapped[i] == expected, "converted GPU markers and guards");
      }
      /* Reuse the original permanent payload restored after a temporary
       * opaque import was consumed by copy export. No stale imported signal
       * may remain attached. A fresh signal followed by a wait must work. */
      CHECK(other->ResetFences(other->handle, 1, &conversion_complete));
      submit(other, NULL, NULL, &conversion_import[1], VK_NULL_HANDLE);
      submit(other, NULL, &conversion_import[1], NULL, conversion_complete);
      CHECK(other->WaitForFences(other->handle, 1, &conversion_complete, VK_TRUE,
                                UINT64_C(5000000000)));
      other->DestroyFence(other->handle, conversion_complete, NULL);
      other->FreeCommandBuffers(other->handle, other->pool, 2, conversion_commands);
      for (unsigned i = 0; i < 2; i++) {
        d->DestroySemaphore(d->handle, conversion_source[i], NULL);
        other->DestroySemaphore(other->handle, conversion_import[i], NULL);
        other->DestroySemaphore(other->handle, conversion_wait[i], NULL);
        require(fd_ready(conversion_fds[i], 0), "conversion FD survives Vulkan payload destruction");
        close(conversion_fds[i]);
      }
      destroy_buffer(other, &converted_markers);
      puts("PASS: two-device SYNC_FD/OPAQUE_FD round trips, temporary/permanent opaque imports, restored-payload reuse");
    }
  }
  d->DestroyFence(d->handle, reusable, NULL);
  d->DestroyFence(d->handle, complete, NULL);
  d->DestroySemaphore(d->handle, gate, NULL);
  d->DestroySemaphore(d->handle, rendered, NULL);
  d->DestroySemaphore(d->handle, exported, NULL);
  for (unsigned i = 0; i < 2; i++) {
    d->DestroySemaphore(d->handle, imported[i], NULL);
    /* Exported sync_files must also survive destruction of their source handle.
     */
    if (sem_fds[i] >= 0) {
      if (!error_gate)
        require(fd_ready(sem_fds[i], 0), "fd survives semaphore destruction");
      close(sem_fds[i]);
    }
    if (fence_fds[i] >= 0) {
      if (!error_gate)
        require(fd_ready(fence_fds[i], 0), "fd survives fence destruction");
      close(fence_fds[i]);
    }
  }
  d->FreeCommandBuffers(d->handle, d->pool, 4, commands);
  printf("%s: pending release exports, %s\n", pending ? "PASS" : "FAIL",
         error_gate ? "incoming fence error propagated and resources retired"
                    : "binary/fence payload reuse, imported old payloads, four "
                      "GPU markers");
  return pending;
}
static void queued_ring(struct Device *d, struct Buffer *b) {
  enum { COUNT = 1030, WINDOW = 32 };
  VkFence done = fence(d, 0);
  VkCommandBuffer commands[WINDOW];
  for (unsigned base = 0; base < COUNT; base += WINDOW) {
    unsigned end = base + WINDOW < COUNT ? base + WINDOW : COUNT;
    for (unsigned i = base; i < end; i++) {
      commands[i - base] = marker_command(d, b, 16 + 2 * i, 0xcafe0000u + i);
      submit(d, &commands[i - base], NULL, NULL,
             i + 1 == end ? done : VK_NULL_HANDLE);
    }
    CHECK(d->WaitForFences(d->handle, 1, &done, VK_TRUE, UINT64_C(5000000000)));
    CHECK(d->ResetFences(d->handle, 1, &done));
    d->FreeCommandBuffers(d->handle, d->pool, end - base, commands);
  }
  CHECK(d->DeviceWaitIdle(d->handle));
  for (unsigned i = 0; i < b->allocation_size / 4; i++) {
    uint32_t expected = 0xa55aa55a;
    if (i < 8 && !(i & 1))
      expected = 0x71c00000u + i / 2;
    if (i >= 16 && i < 16 + 2 * COUNT && !(i & 1))
      expected = 0xcafe0000u + (i - 16) / 2;
    require(b->mapped[i] == expected,
            "1030 queued GPU markers and allocation guards");
  }
  d->DestroyFence(d->handle, done, NULL);
  puts("PASS: 1030 marker submissions in windows of 32, command-buffer/fence "
       "retirement, all allocation guards");
}

int main(int argc, char **argv) {
  const char *driver = NULL;
  int allow_stock = 0, gate_fd = -1, error_gate = 0;
  int convert = getenv("NV_FENCE_CONVERT") && strcmp(getenv("NV_FENCE_CONVERT"), "0");
  signal(SIGPIPE, SIG_IGN);
  atexit(release_gate);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--driver") && i + 1 < argc)
      driver = argv[++i];
    else if (!strcmp(argv[i], "--gate-fd") && i + 1 < argc)
      gate_fd = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--release-fd") && i + 1 < argc)
      release_fd = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--convert"))
      convert = 1;
    else if (!strcmp(argv[i], "--error-gate"))
      error_gate = 1;
    else if (!strcmp(argv[i], "--allow-stock-driver"))
      allow_stock = 1;
    else
      require(0, "usage: PROBE --driver /absolute/ICD.so --gate-fd N "
                 "--release-fd M [--allow-stock-driver] [--error-gate] [--convert]");
  }
  require(!convert || !error_gate, "--convert cannot combine with --error-gate");
  require(driver && driver[0] == '/', "absolute driver path");
  require(gate_fd >= 0 && release_fd >= 0 && gate_fd != release_fd,
          "gate sync_file and release pipe descriptors");
  setvbuf(stdout, NULL, _IONBF, 0);
  nv_probe_android_runtime_init();
  int before = fd_count();
  struct FdEntry *before_fds = fd_snapshot();
  void *library = dlopen(driver, RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 1;
  }
  PFN_vkGetInstanceProcAddr gipa =
      (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
  if (!gipa)
    gipa =
        (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
  require(gipa != NULL, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance create =
      (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  require(create != NULL, "vkCreateInstance");
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName =
                               "Independent pending sync_file probe",
                           .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo = &app};
  VkInstance instance = VK_NULL_HANDLE;
  CHECK(create(&ci, NULL, &instance));
#define INSTANCE(name)                                                         \
  PFN_vk##name name = (PFN_vk##name)gipa(instance, "vk" #name);                \
  require(name != NULL, "vk" #name)
  INSTANCE(DestroyInstance);
  INSTANCE(EnumeratePhysicalDevices);
  INSTANCE(GetPhysicalDeviceProperties);
  INSTANCE(GetPhysicalDeviceMemoryProperties);
  INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
  INSTANCE(GetPhysicalDeviceExternalFenceProperties);
  INSTANCE(GetPhysicalDeviceExternalSemaphoreProperties);
  INSTANCE(CreateDevice);
  INSTANCE(GetDeviceProcAddr);
#undef INSTANCE
  uint32_t n = 0;
  CHECK(EnumeratePhysicalDevices(instance, &n, NULL));
  require(n && n <= 64, "physical device count");
  VkPhysicalDevice devices[64], physical = VK_NULL_HANDLE;
  CHECK(EnumeratePhysicalDevices(instance, &n, devices));
  VkPhysicalDeviceProperties props;
  for (uint32_t i = 0; i < n; i++) {
    GetPhysicalDeviceProperties(devices[i], &props);
    if (props.vendorID == 0x10de) {
      physical = devices[i];
      break;
    }
  }
  require(physical != VK_NULL_HANDLE, "NVIDIA vendor device");
  printf("device: %.256s vendor=%04x device=%04x\n", props.deviceName,
         props.vendorID, props.deviceID);
  GetPhysicalDeviceMemoryProperties(physical, &memory_properties);
  require(memory_properties.memoryTypeCount <= 32, "memory type count");
  n = 0;
  GetPhysicalDeviceQueueFamilyProperties(physical, &n, NULL);
  require(n && n <= 64, "queue families");
  VkQueueFamilyProperties queues[64];
  GetPhysicalDeviceQueueFamilyProperties(physical, &n, queues);
  queue_family = UINT32_MAX;
  for (uint32_t i = 0; i < n; i++)
    if (queues[i].queueCount && (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
      queue_family = i;
      break;
    }
  require(queue_family != UINT32_MAX, "compute queue family");
  VkPhysicalDeviceExternalFenceInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
      .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
  VkExternalFenceProperties fence_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
  GetPhysicalDeviceExternalFenceProperties(physical, &fence_info, &fence_props);
  require((fence_props.externalFenceFeatures &
           (VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT |
            VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) ==
              (VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT |
               VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT),
          "fence sync-fd import/export support");
  VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
  VkExternalSemaphoreProperties semaphore_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  GetPhysicalDeviceExternalSemaphoreProperties(physical, &semaphore_info,
                                               &semaphore_props);
  require((semaphore_props.externalSemaphoreFeatures &
           (VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) ==
              (VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
               VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT),
          "semaphore sync-fd import/export support");
  if (convert) {
    const VkExternalSemaphoreHandleTypeFlags both = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT |
                                                   VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceExternalSemaphoreInfo opaque_info = semaphore_info;
    opaque_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties opaque_props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    GetPhysicalDeviceExternalSemaphoreProperties(physical, &opaque_info, &opaque_props);
    printf("conversion properties: SYNC_FD features=%x compatible=%x imported=%x; OPAQUE_FD features=%x compatible=%x imported=%x\n",
           semaphore_props.externalSemaphoreFeatures, semaphore_props.compatibleHandleTypes,
           semaphore_props.exportFromImportedHandleTypes, opaque_props.externalSemaphoreFeatures,
           opaque_props.compatibleHandleTypes, opaque_props.exportFromImportedHandleTypes);
    const VkExternalSemaphoreFeatureFlags features = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
                                                      VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
    if ((opaque_props.externalSemaphoreFeatures & features) != features ||
        (opaque_props.compatibleHandleTypes & both) != both ||
        (semaphore_props.compatibleHandleTypes & both) != both ||
        !(opaque_props.exportFromImportedHandleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) ||
        !(semaphore_props.exportFromImportedHandleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)) {
      puts("SKIP: selected ICD does not advertise both cross-handle conversions with compatible creation types");
      release_gate();
      DestroyInstance(instance, NULL);
      require(dlclose(library) == 0, "dlclose skipped conversion ICD");
      fd_snapshot_finish(before_fds, "before", 0);
      nv_probe_android_logger_close();
      return 77;
    }
  }
  struct Device other = {0};
  if (convert) init_device(&other, CreateDevice, GetDeviceProcAddr, physical);
  struct Device device = {0};
  init_device(&device, CreateDevice, GetDeviceProcAddr, physical);
  struct Buffer markers = create_buffer(&device);
  int pending = pending_gate(&device, &markers, gate_fd, error_gate, convert ? &other : NULL);
  if (pending && !error_gate)
    queued_ring(&device, &markers);
  maps_check(allow_stock);
  destroy_buffer(&device, &markers);
  device.DestroyCommandPool(device.handle, device.pool, NULL);
  device.DestroyDevice(device.handle, NULL);
  if (other.handle) {
    other.DestroyCommandPool(other.handle, other.pool, NULL);
    other.DestroyDevice(other.handle, NULL);
  }
  DestroyInstance(instance, NULL);
  nv_probe_android_logger_close();
  for (unsigned cycle = 0; cycle < 2; cycle++) {
    VkInstance repeated = VK_NULL_HANDLE;
    CHECK(create(&ci, NULL, &repeated));
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)gipa(repeated,
                                             "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(
            repeated, "vkGetPhysicalDeviceProperties");
    PFN_vkCreateDevice create_device =
        (PFN_vkCreateDevice)gipa(repeated, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr get_proc =
        (PFN_vkGetDeviceProcAddr)gipa(repeated, "vkGetDeviceProcAddr");
    PFN_vkDestroyInstance destroy_instance =
        (PFN_vkDestroyInstance)gipa(repeated, "vkDestroyInstance");
    require(enumerate && properties && create_device && get_proc &&
                destroy_instance,
            "repeat lifecycle entrypoints");
    uint32_t count = 64;
    VkPhysicalDevice found[64], selected = VK_NULL_HANDLE;
    CHECK(enumerate(repeated, &count, found));
    for (unsigned i = 0; i < count; i++) {
      VkPhysicalDeviceProperties prop;
      properties(found[i], &prop);
      if (prop.vendorID == 0x10de) {
        selected = found[i];
        break;
      }
    }
    require(selected != VK_NULL_HANDLE, "repeat NVIDIA device");
    struct Device extra = {0};
    init_device(&extra, create_device, get_proc, selected);
    extra.DestroyCommandPool(extra.handle, extra.pool, NULL);
    extra.DestroyDevice(extra.handle, NULL);
    destroy_instance(repeated, NULL);
    nv_probe_android_logger_close();
    int count_after = fd_count();
    printf("additional instance/device lifecycle %u: fds %d -> %d\n", cycle + 1,
           before, count_after);
    if (count_after != before) {
      fd_snapshot_finish(fd_snapshot(), "lifecycle", 1);
      require(0, "repeated Vulkan lifecycle leaks descriptors");
    }
  }
  require(dlclose(library) == 0, "dlclose");
  int after = fd_count();
  printf("file descriptors %d -> %d\n", before, after);
  fd_snapshot_finish(before_fds, "before", before != after);
  if (before != after)
    fd_snapshot_finish(fd_snapshot(), "after", 1);
  require(before == after, "fd leak");
  require(
      pending,
      "exports must return while external gate and GPU work remain pending");
  puts(error_gate ? "PASS: pending external error becomes device loss, clean "
                    "lifetime retirement"
                  : "PASS: genuinely pending SYNC_FD release, payload "
                    "transference, queued work and lifetime retirement");
  return 0;
}
