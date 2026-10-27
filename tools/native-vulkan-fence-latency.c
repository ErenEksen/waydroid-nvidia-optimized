/* Public Vulkan + Linux vgem latency probe. See native-vulkan-fence-latency.md.
 * No RM/private driver ABI, proprietary code, GPU spin loop, or module loading.
 * The marker is read only after bounded fence completion and a HOST barrier.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <drm.h>
#include <drm_mode.h>
#include <vgem_drm.h>
#include <vulkan/vulkan.h>
#include "native-vulkan-runtime.h"

#define FUNCTIONS(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) \
 X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
 X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateCommandPool) \
 X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
 X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdFillBuffer) X(CmdPipelineBarrier) \
 X(QueueSubmit) X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) \
 X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(ImportSemaphoreFdKHR)
struct Api {
#define DECLARE(name) PFN_vk##name name;
  FUNCTIONS(DECLARE)
#undef DECLARE
};
struct Producer {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int fd;
  uint32_t fence;
  bool active, stop, forced, failed;
  struct timespec deadline;
};
static uint64_t time_ns(clockid_t clock) {
  struct timespec t;
  if (clock_gettime(clock, &t)) { perror("clock_gettime"); abort(); }
  return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static void sleep_ms(unsigned milliseconds) {
  struct timespec t = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&t, &t) && errno == EINTR) {}
}
/* Caller holds mutex. The vgem producer is signaled even on Vulkan failure. */
static void release_locked(struct Producer *p, bool forced) {
  if (!p->active) return;
  struct drm_vgem_fence_signal signal = {.fence = p->fence};
  if (ioctl(p->fd, DRM_IOCTL_VGEM_FENCE_SIGNAL, &signal)) {
    perror("VGEM_FENCE_SIGNAL");
    p->failed = true;
  }
  p->active = false;
  p->forced |= forced;
  pthread_cond_broadcast(&p->condition);
}
static bool release(struct Producer *p, uint64_t *start) {
  pthread_mutex_lock(&p->mutex);
  if (start) *start = time_ns(CLOCK_MONOTONIC);
  release_locked(p, false);
  bool good = !p->forced && !p->failed;
  pthread_mutex_unlock(&p->mutex);
  return good;
}
static void *watchdog(void *arg) {
  struct Producer *p = arg;
  pthread_mutex_lock(&p->mutex);
  while (!p->stop) {
    if (!p->active) {
      pthread_cond_wait(&p->condition, &p->mutex);
      continue;
    }
    int r = pthread_cond_timedwait(&p->condition, &p->mutex, &p->deadline);
    if (r == ETIMEDOUT && p->active) release_locked(p, true);
    else if (r) { p->failed = true; release_locked(p, true); }
  }
  release_locked(p, false);
  pthread_mutex_unlock(&p->mutex);
  return NULL;
}
static int attach(struct Producer *p, uint32_t handle) {
  struct drm_vgem_fence_attach a = {.handle = handle, .flags = VGEM_FENCE_WRITE};
  pthread_mutex_lock(&p->mutex);
  int r = ioctl(p->fd, DRM_IOCTL_VGEM_FENCE_ATTACH, &a);
  if (!r) {
    uint64_t end = time_ns(CLOCK_MONOTONIC) + 750000000;
    p->fence = a.out_fence;
    p->active = true;
    p->deadline = (struct timespec){(time_t)(end / 1000000000), (long)(end % 1000000000)};
    pthread_cond_broadcast(&p->condition);
  }
  pthread_mutex_unlock(&p->mutex);
  return r;
}
static int open_vgem(void) {
  DIR *dir = opendir("/dev/dri");
  if (!dir) return -1;
  int selected = -1;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strncmp(entry->d_name, "card", 4)) continue;
    char *end;
    (void)strtoul(entry->d_name + 4, &end, 10);
    if (end == entry->d_name + 4 || *end) continue;
    int fd = openat(dirfd(dir), entry->d_name, O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    char name[64] = {0};
    struct drm_version version = {.name_len = sizeof(name) - 1, .name = name};
    if (!ioctl(fd, DRM_IOCTL_VERSION, &version) && !strcmp(name, "vgem")) {
      printf("producer: vgem /dev/dri/%s\n", entry->d_name);
      selected = fd;
      break;
    }
    close(fd);
  }
  closedir(dir);
  return selected;
}
static int compare(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
static void report(const char *label, double *values, unsigned count,
                   uint64_t wall, uint64_t cpu) {
  qsort(values, count, sizeof(*values), compare);
  double sum = 0;
  for (unsigned i = 0; i < count; i++) sum += values[i];
  printf("%s: n=%u mean_us=%.3f min_us=%.3f p50_us=%.3f p90_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f wall_ms=%.3f process_cpu_ms=%.3f\n",
         label, count, sum / count, values[0], values[count / 2], values[count * 90 / 100],
         values[count * 95 / 100], values[count * 99 / 100], values[count - 1], wall / 1e6, cpu / 1e6);
}
static bool number(const char *text, unsigned *value, unsigned maximum) {
  char *end; errno = 0;
  unsigned long n = strtoul(text, &end, 10);
  if (!*text || *end || errno || n > maximum) return false;
  *value = (unsigned)n;
  return true;
}
static void driver_maps(void) {
  FILE *maps = fopen("/proc/self/maps", "r");
  if (!maps) return;
  char line[4096], previous[4096] = "";
  while (fgets(line, sizeof(line), maps)) {
    char *path = strchr(line, '/');
    if (path && (strstr(path, "libvulkan_nouveau") || strstr(path, "libnv_bionic_native") ||
                 strstr(path, "libEGL_nvidia") || strstr(path, "libnvidia-eglcore")) && strcmp(path, previous)) {
      printf("mapped_driver: %s", path);
      snprintf(previous, sizeof(previous), "%s", path);
    }
  }
  fclose(maps);
}
static void usage(const char *exe) {
  printf("Usage: %s --driver /absolute/ICD.so [--iterations 1000] [--device-index 0]\n"
         "  [--idle-ms 20] [--measure-idle-ms 1000] [--allow-stock-driver]\n"
         "Requires an already loaded, accessible vgem device. No module is loaded.\n", exe);
}
#define REQUIRE(test, what) do { if (!(test)) { fprintf(stderr, "FAIL: %s (errno=%d)\n", what, errno); goto cleanup; } } while (0)
#define CHECK(call) do { VkResult result_ = (call); if (result_ != VK_SUCCESS) { fprintf(stderr, "FAIL: %s -> %d\n", #call, result_); goto cleanup; } } while (0)

int main(int argc, char **argv) {
  const char *driver = NULL;
  unsigned iterations = 1000, index = 0, idle_ms = 20, measure_idle_ms = 1000;
  bool allow_stock = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    if (!strcmp(argv[i], "--allow-stock-driver")) { allow_stock = true; continue; }
    if (i + 1 >= argc) { usage(argv[0]); return 2; }
    const char *key = argv[i++], *value = argv[i];
    if (!strcmp(key, "--driver")) driver = value;
    else if (!strcmp(key, "--iterations") && number(value, &iterations, 100000) && iterations) {}
    else if (!strcmp(key, "--device-index") && number(value, &index, 1024)) {}
    else if (!strcmp(key, "--idle-ms") && number(value, &idle_ms, 200)) {}
    else if (!strcmp(key, "--measure-idle-ms") && number(value, &measure_idle_ms, 60000)) {}
    else { usage(argv[0]); return 2; }
  }
  if (!driver || driver[0] != '/') { usage(argv[0]); return 2; }
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("driver: %s iterations=%u idle_ms=%u measure_idle_ms=%u device_index=%u\n",
         driver, iterations, idle_ms, measure_idle_ms, index);
  nv_probe_android_runtime_init();
  int before = nv_probe_runtime_fd_count(), outcome = 1;
  int vg = -1, dma = -1, imported = -1;
  uint32_t dumb_handle = 0;
  struct Producer producer = {.mutex = PTHREAD_MUTEX_INITIALIZER, .fd = -1};
  pthread_t thread;
  bool thread_started = false, condition_initialized = false, pending = false;
  void *library = NULL;
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSemaphore semaphore = VK_NULL_HANDLE;
  uint32_t *marker = NULL;
  struct Api api = {0};
  PFN_vkDestroyInstance destroy_instance = NULL;
  double *samples = calloc(iterations, sizeof(*samples));
  VkPhysicalDevice *devices = NULL;
  VkQueueFamilyProperties *families = NULL;
  REQUIRE(samples, "allocate samples");
  vg = open_vgem();
  REQUIRE(vg >= 0, "no accessible vgem card (load vgem separately; grant access)");
  producer.fd = vg;
  pthread_condattr_t attr;
  REQUIRE(!pthread_condattr_init(&attr), "condition attributes");
  int attr_result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  int condition_result = attr_result ? attr_result : pthread_cond_init(&producer.condition, &attr);
  pthread_condattr_destroy(&attr);
  REQUIRE(!condition_result, "monotonic watchdog condition");
  condition_initialized = true;
  REQUIRE(!pthread_create(&thread, NULL, watchdog, &producer), "start producer watchdog");
  thread_started = true;
  struct drm_mode_create_dumb dumb = {.height = 1, .width = 1024, .bpp = 32};
  REQUIRE(!ioctl(vg, DRM_IOCTL_MODE_CREATE_DUMB, &dumb), "create vgem dumb buffer");
  dumb_handle = dumb.handle;
  struct drm_prime_handle prime = {.handle = dumb_handle, .flags = DRM_CLOEXEC | DRM_RDWR, .fd = -1};
  REQUIRE(!ioctl(vg, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime), "export vgem dma-buf");
  dma = prime.fd;
  library = dlopen(driver, RTLD_NOW | RTLD_LOCAL);
  if (!library) fprintf(stderr, "dlopen: %s\n", dlerror());
  REQUIRE(library, "load requested Vulkan ICD");
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
  if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
  REQUIRE(gipa, "Vulkan ICD instance entry point");
  PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  REQUIRE(create_instance, "vkCreateInstance");
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  CHECK(create_instance(&ici, NULL, &instance));
  destroy_instance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
#define GI(name) PFN_vk##name name = (PFN_vk##name)gipa(instance, "vk" #name); REQUIRE(name, "vk" #name)
  GI(EnumeratePhysicalDevices); GI(GetPhysicalDeviceProperties2); GI(GetPhysicalDeviceQueueFamilyProperties);
  GI(GetPhysicalDeviceMemoryProperties); GI(GetPhysicalDeviceExternalSemaphoreProperties); GI(CreateDevice); GI(GetDeviceProcAddr);
  REQUIRE(destroy_instance, "vkDestroyInstance");
  uint32_t count = 0;
  CHECK(EnumeratePhysicalDevices(instance, &count, NULL));
  devices = calloc(count, sizeof(*devices)); REQUIRE(devices && count, "physical device list");
  CHECK(EnumeratePhysicalDevices(instance, &count, devices));
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  unsigned eligible = 0;
  for (uint32_t i = 0; i < count; i++) {
    VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
    GetPhysicalDeviceProperties2(devices[i], &properties);
    if (properties.properties.vendorID != 0x10de ||
        (dp.driverID != VK_DRIVER_ID_MESA_NVK && !(allow_stock && dp.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY))) continue;
    if (eligible++ == index) {
      physical = devices[i];
      printf("device: %s vendor=0x%x device=0x%x driver_id=%u driver=%s info=%s\n",
             properties.properties.deviceName, properties.properties.vendorID, properties.properties.deviceID,
             (unsigned)dp.driverID, dp.driverName, dp.driverInfo);
    }
  }
  REQUIRE(physical, "requested NVIDIA vendor/driver selection absent");
  VkPhysicalDeviceExternalSemaphoreInfo esi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
  VkExternalSemaphoreProperties esp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  GetPhysicalDeviceExternalSemaphoreProperties(physical, &esi, &esp);
  REQUIRE(esp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT, "SYNC_FD import unsupported");
  uint32_t family_count = 0;
  GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
  families = calloc(family_count, sizeof(*families)); REQUIRE(families && family_count, "queue family list");
  GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
  uint32_t family = 0;
  while (family < family_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
  REQUIRE(family < family_count, "graphics queue absent");
  float priority = 1;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
  const char *extensions[] = {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = extensions};
  CHECK(CreateDevice(physical, &dci, NULL, &device));
#define LOAD(name) api.name = (PFN_vk##name)GetDeviceProcAddr(device, "vk" #name); REQUIRE(api.name, "vk" #name);
  FUNCTIONS(LOAD)
#undef LOAD
  VkQueue queue;
  api.GetDeviceQueue(device, family, 0, &queue);
  VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64,
    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  CHECK(api.CreateBuffer(device, &bci, NULL, &buffer));
  VkMemoryRequirements requirements;
  api.GetBufferMemoryRequirements(device, buffer, &requirements);
  VkPhysicalDeviceMemoryProperties mp;
  GetPhysicalDeviceMemoryProperties(physical, &mp);
  uint32_t type = 0, wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  while (type < mp.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
         (mp.memoryTypes[type].propertyFlags & wanted) != wanted)) type++;
  REQUIRE(type < mp.memoryTypeCount, "coherent host-visible marker memory absent");
  VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size, .memoryTypeIndex = type};
  CHECK(api.AllocateMemory(device, &mai, NULL, &memory));
  CHECK(api.BindBufferMemory(device, buffer, memory, 0));
  CHECK(api.MapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&marker));
  VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family};
  CHECK(api.CreateCommandPool(device, &pci, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  VkCommandBuffer command;
  CHECK(api.AllocateCommandBuffers(device, &cai, &command));
  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  CHECK(api.CreateFence(device, &fci, NULL, &fence));
  VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  CHECK(api.CreateSemaphore(device, &sci, NULL, &semaphore));
  driver_maps();
  uint64_t idle_wall = time_ns(CLOCK_MONOTONIC), idle_cpu = time_ns(CLOCK_PROCESS_CPUTIME_ID);
  sleep_ms(measure_idle_ms);
  printf("idle: requested_ms=%u wall_ms=%.3f process_cpu_ms=%.3f\n", measure_idle_ms,
         (time_ns(CLOCK_MONOTONIC) - idle_wall) / 1e6, (time_ns(CLOCK_PROCESS_CPUTIME_ID) - idle_cpu) / 1e6);
  for (unsigned mode = 0; mode < 2; mode++) {
    uint64_t wall = time_ns(CLOCK_MONOTONIC), cpu = time_ns(CLOCK_PROCESS_CPUTIME_ID);
    for (unsigned i = 0; i < iterations; i++) {
      uint32_t value = 1 + i + mode * iterations;
      *marker = 0; /* Previous iteration's fence has completed; no GPU race. */
      CHECK(api.ResetCommandBuffer(command, 0));
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      CHECK(api.BeginCommandBuffer(command, &begin));
      api.CmdFillBuffer(command, buffer, 0, 4, value);
      VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      api.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
      CHECK(api.EndCommandBuffer(command));
      VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command};
      uint64_t start;
      if (mode == 0) {
        REQUIRE(!attach(&producer, dumb_handle), "attach vgem producer fence");
        struct dma_buf_export_sync_file export = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
        REQUIRE(!ioctl(dma, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export), "export vgem sync_file");
        imported = export.fd;
        struct pollfd pollfd = {.fd = imported, .events = POLLIN};
        REQUIRE(poll(&pollfd, 1, 0) == 0, "foreign sync_file must start pending");
        VkImportSemaphoreFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
          .semaphore = semaphore, .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = imported};
        CHECK(api.ImportSemaphoreFdKHR(device, &import));
        imported = -1; /* Successful import transfers ownership. */
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &semaphore; submit.pWaitDstStageMask = &stage;
        CHECK(api.QueueSubmit(queue, 1, &submit, fence));
        pending = true;
        sleep_ms(idle_ms);
        REQUIRE(api.GetFenceStatus(device, fence) == VK_NOT_READY, "dependent work completed before producer release");
        REQUIRE(release(&producer, &start), "producer watchdog fired or signal failed; discard measurement");
      } else {
        sleep_ms(idle_ms);
        start = time_ns(CLOCK_MONOTONIC);
        CHECK(api.QueueSubmit(queue, 1, &submit, fence));
        pending = true;
      }
      CHECK(api.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(2000000000)));
      uint64_t complete = time_ns(CLOCK_MONOTONIC);
      pending = false;
      REQUIRE(*marker == value, "completed GPU marker mismatch");
      samples[i] = (complete - start) / 1e3;
      CHECK(api.ResetFences(device, 1, &fence));
    }
    report(mode ? "control_submit_to_completion_observed_marker" : "foreign_signal_to_completion_observed_marker",
           samples, iterations, time_ns(CLOCK_MONOTONIC) - wall, time_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu);
  }
  outcome = 0;
cleanup:
  if (thread_started) {
    release(&producer, NULL);
    pthread_mutex_lock(&producer.mutex);
    producer.stop = true;
    pthread_cond_broadcast(&producer.condition);
    pthread_mutex_unlock(&producer.mutex);
    pthread_join(thread, NULL);
  }
  if (imported >= 0) close(imported);
  /* Never issue an infinite DeviceWaitIdle after a failure. Release the producer
   * first, then allow a bounded drain. Driver/syscall hangs still need an outer
   * process watchdog; userspace cannot guarantee a bound for kernel teardown. */
  if (pending && api.WaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(2000000000)) != VK_SUCCESS) {
    fputs("FAIL: released producer but GPU did not drain; skipping unsafe resource destruction\n", stderr);
    fflush(NULL);
    _Exit(1);
  }
  if (device) {
    if (semaphore) api.DestroySemaphore(device, semaphore, NULL);
    if (fence) api.DestroyFence(device, fence, NULL);
    if (pool) api.DestroyCommandPool(device, pool, NULL);
    if (marker) api.UnmapMemory(device, memory);
    if (buffer) api.DestroyBuffer(device, buffer, NULL);
    if (memory) api.FreeMemory(device, memory, NULL);
    if (api.DestroyDevice) api.DestroyDevice(device, NULL);
  }
  if (instance && destroy_instance) destroy_instance(instance, NULL);
  if (library) dlclose(library);
  if (dma >= 0) close(dma);
  if (dumb_handle) {
    struct drm_mode_destroy_dumb destroy = {.handle = dumb_handle};
    if (ioctl(vg, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy)) { perror("destroy vgem buffer"); outcome = 1; }
  }
  if (vg >= 0) close(vg);
  if (condition_initialized) pthread_cond_destroy(&producer.condition);
  pthread_mutex_destroy(&producer.mutex);
  free(families); free(devices); free(samples);
  nv_probe_android_logger_close();
  int after = nv_probe_runtime_fd_count();
  if (after != before) { fprintf(stderr, "FAIL: FD count %d -> %d\n", before, after); nv_probe_dump_fds(); outcome = 1; }
  printf("%s: fd%d->%d; completion-observed metric, not raw GPU store time\n", outcome ? "FAIL" : "PASS", before, after);
  return outcome;
}
