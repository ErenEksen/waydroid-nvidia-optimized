/* Same-thread wait-before-signal under ring pressure (public Vulkan only).
 *
 * Thread T submits batch 0 to the queue waiting on timeline S >= 1 whose
 * signal does not exist yet, then N more batches to the same queue, then
 * host-signals S = 1. Vulkan allows this (wait-before-signal, timeline
 * semaphores). A driver that hands the unsatisfiable wait straight to a
 * fixed-size GPU ring and CPU-waits for ring space on the submitting thread
 * deadlocks: the space only frees after S is signalled, which only T can do
 * after its submit call returns. A watchdog turns that hang into FAIL.
 *
 * Optional --bench measures CPU time per ordinary submit (waits already
 * satisfied/pending) on a fresh device and after the wait-before-signal
 * phase, for fast-path cost comparison between driver builds.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define REQUIRE(c, what) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); exit(1); } } while (0)
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { fprintf(stderr, "FAIL: %s = %d (%s:%d)\n", #call, r_, __FILE__, __LINE__); exit(1); } } while (0)

static uint64_t now_ns(clockid_t clock) {
  struct timespec t;
  clock_gettime(clock, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static unsigned watchdog_seconds = 30;
static const char *volatile watchdog_phase = "start";
static void *watchdog(void *arg) {
  (void)arg;
  sleep(watchdog_seconds);
  fprintf(stderr, "FAIL: watchdog %us expired in phase '%s' (deadlock)\n", watchdog_seconds,
          (const char *)watchdog_phase);
  fflush(stderr);
  _exit(3);
}

#define FUNCTIONS(X) X(GetDeviceQueue) X(DestroyDevice) X(CreateBuffer) X(DestroyBuffer) \
  X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
  X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
  X(EndCommandBuffer) X(CmdFillBuffer) X(QueueSubmit) X(QueueWaitIdle) X(CreateSemaphore) \
  X(DestroySemaphore) X(SignalSemaphore) X(WaitSemaphores) X(GetSemaphoreCounterValue)
struct Api {
#define DECL(name) PFN_vk##name name;
  FUNCTIONS(DECL)
#undef DECL
};

struct Ctx {
  VkDevice device;
  VkQueue queue;
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkCommandPool pool;
  VkCommandBuffer command;
  struct Api api;
};

static VkSemaphore timeline(struct Ctx *c, uint64_t initial) {
  VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = initial};
  VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
  VkSemaphore s;
  CHECK(c->api.CreateSemaphore(c->device, &sci, NULL, &s));
  return s;
}

static void submit(struct Ctx *c, VkSemaphore wait, uint64_t wait_value,
                   VkSemaphore signal, uint64_t signal_value) {
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkTimelineSemaphoreSubmitInfo tsi = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
    .waitSemaphoreValueCount = wait ? 1 : 0, .pWaitSemaphoreValues = &wait_value,
    .signalSemaphoreValueCount = signal ? 1 : 0, .pSignalSemaphoreValues = &signal_value};
  VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi,
    .waitSemaphoreCount = wait ? 1 : 0, .pWaitSemaphores = &wait, .pWaitDstStageMask = &stage,
    .commandBufferCount = 1, .pCommandBuffers = &c->command,
    .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal};
  CHECK(c->api.QueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE));
}

static void wait_value(struct Ctx *c, VkSemaphore s, uint64_t value, uint64_t timeout_ns) {
  VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
    .semaphoreCount = 1, .pSemaphores = &s, .pValues = &value};
  CHECK(c->api.WaitSemaphores(c->device, &wi, timeout_ns));
}

/* Ordinary submits: each waits on the previous submit's signal (already
 * pending when submitted) and signals the next value. */
static void bench(struct Ctx *c, unsigned n, const char *label) {
  VkSemaphore s = timeline(c, 0);
  submit(c, VK_NULL_HANDLE, 0, s, 1);
  wait_value(c, s, 1, 5000000000ull);
  uint64_t wall = now_ns(CLOCK_MONOTONIC), cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
  for (unsigned i = 0; i < n; i++) {
    submit(c, s, 1 + i, s, 2 + i);
    if ((i & 255) == 255) wait_value(c, s, 2 + i, 5000000000ull); /* bound outstanding work */
  }
  uint64_t submit_cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
  wait_value(c, s, 1 + n, 5000000000ull);
  printf("bench %s: submits=%u thread_cpu_per_submit_us=%.3f wall_ms=%.3f\n", label, n,
         submit_cpu / 1e3 / n, (now_ns(CLOCK_MONOTONIC) - wall) / 1e6);
  c->api.DestroySemaphore(c->device, s, NULL);
}

int main(int argc, char **argv) {
  const char *driver = NULL;
  unsigned count = 1100, bench_n = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--driver")) driver = argv[i + 1];
    else if (!strcmp(argv[i], "--submits")) count = (unsigned)strtoul(argv[i + 1], NULL, 10);
    else if (!strcmp(argv[i], "--timeout")) watchdog_seconds = (unsigned)strtoul(argv[i + 1], NULL, 10);
    else if (!strcmp(argv[i], "--bench")) bench_n = (unsigned)strtoul(argv[i + 1], NULL, 10);
    else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  if (!driver || driver[0] != '/' || !count || !watchdog_seconds) {
    fprintf(stderr, "Usage: %s --driver /abs/ICD.so [--submits 1100] [--timeout 30] [--bench N]\n", argv[0]);
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  pthread_t dog;
  REQUIRE(!pthread_create(&dog, NULL, watchdog, NULL), "watchdog thread");
  void *library = dlopen(driver, RTLD_NOW | RTLD_LOCAL);
  if (!library) fprintf(stderr, "dlopen: %s\n", dlerror());
  REQUIRE(library, "load ICD");
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
  REQUIRE(gipa, "vk_icdGetInstanceProcAddr");
  PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  VkInstance instance;
  CHECK(create_instance(&ici, NULL, &instance));
#define GI(name) PFN_vk##name name = (PFN_vk##name)gipa(instance, "vk" #name); REQUIRE(name, "vk" #name)
  GI(EnumeratePhysicalDevices); GI(GetPhysicalDeviceProperties2); GI(GetPhysicalDeviceMemoryProperties);
  GI(CreateDevice); GI(GetDeviceProcAddr); GI(DestroyInstance);
  uint32_t n = 0;
  CHECK(EnumeratePhysicalDevices(instance, &n, NULL));
  VkPhysicalDevice devices[16];
  REQUIRE(n && n <= 16, "physical devices");
  CHECK(EnumeratePhysicalDevices(instance, &n, devices));
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  for (uint32_t i = 0; i < n && !physical; i++) {
    VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
    GetPhysicalDeviceProperties2(devices[i], &p);
    if (p.properties.vendorID == 0x10de && dp.driverID == VK_DRIVER_ID_MESA_NVK) {
      physical = devices[i];
      printf("device: %s driver=%s %s\n", p.properties.deviceName, dp.driverName, dp.driverInfo);
    }
  }
  REQUIRE(physical, "NVIDIA NVK device (vendor/driver ID)");
  float priority = 1;
  VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
  VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    .timelineSemaphore = VK_TRUE};
  VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12,
    .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
  struct Ctx c = {0};
  CHECK(CreateDevice(physical, &dci, NULL, &c.device));
#define LOAD(name) c.api.name = (PFN_vk##name)GetDeviceProcAddr(c.device, "vk" #name); REQUIRE(c.api.name, "vk" #name);
  FUNCTIONS(LOAD)
#undef LOAD
  c.api.GetDeviceQueue(c.device, 0, 0, &c.queue);
  VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  CHECK(c.api.CreateBuffer(c.device, &bci, NULL, &c.buffer));
  VkMemoryRequirements req;
  c.api.GetBufferMemoryRequirements(c.device, c.buffer, &req);
  VkPhysicalDeviceMemoryProperties mp;
  GetPhysicalDeviceMemoryProperties(physical, &mp);
  uint32_t type = 0;
  while (type < mp.memoryTypeCount && !(req.memoryTypeBits & (1u << type))) type++;
  REQUIRE(type < mp.memoryTypeCount, "memory type");
  VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = req.size, .memoryTypeIndex = type};
  CHECK(c.api.AllocateMemory(c.device, &mai, NULL, &c.memory));
  CHECK(c.api.BindBufferMemory(c.device, c.buffer, c.memory, 0));
  VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
  CHECK(c.api.CreateCommandPool(c.device, &pci, NULL, &c.pool));
  VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = c.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
  CHECK(c.api.AllocateCommandBuffers(c.device, &cai, &c.command));
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
  CHECK(c.api.BeginCommandBuffer(c.command, &begin));
  c.api.CmdFillBuffer(c.command, c.buffer, 0, 256, 0xc0ffee);
  CHECK(c.api.EndCommandBuffer(c.command));

  if (bench_n) {
    watchdog_phase = "bench fresh";
    bench(&c, bench_n, "fresh-device");
  }

  /* The reproducer: one thread, one queue, unsatisfied wait first. */
  watchdog_phase = "wait-before-signal submits";
  VkSemaphore s = timeline(&c, 0), done = timeline(&c, 0);
  uint64_t t0 = now_ns(CLOCK_MONOTONIC);
  submit(&c, s, 1, VK_NULL_HANDLE, 0);
  for (unsigned i = 1; i <= count; i++)
    submit(&c, VK_NULL_HANDLE, 0, done, i);
  uint64_t t1 = now_ns(CLOCK_MONOTONIC);
  uint64_t value = ~0ull;
  CHECK(c.api.GetSemaphoreCounterValue(c.device, done, &value));
  REQUIRE(value == 0, "work behind an unsignalled wait must not complete before the signal");
  watchdog_phase = "host signal";
  VkSemaphoreSignalInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = s, .value = 1};
  CHECK(c.api.SignalSemaphore(c.device, &sig));
  watchdog_phase = "completion";
  wait_value(&c, done, count, 10000000000ull);
  CHECK(c.api.QueueWaitIdle(c.queue));
  printf("wait-before-signal: submits=%u submit_ms=%.3f complete_ms=%.3f\n", count + 1,
         (t1 - t0) / 1e6, (now_ns(CLOCK_MONOTONIC) - t0) / 1e6);

  if (bench_n) {
    watchdog_phase = "bench after wait-before-signal";
    bench(&c, bench_n, "after-wbs");
  }
  watchdog_phase = "teardown";
  c.api.DestroySemaphore(c.device, s, NULL);
  c.api.DestroySemaphore(c.device, done, NULL);
  c.api.DestroyCommandPool(c.device, c.pool, NULL);
  c.api.DestroyBuffer(c.device, c.buffer, NULL);
  c.api.FreeMemory(c.device, c.memory, NULL);
  c.api.DestroyDevice(c.device, NULL);
  DestroyInstance(instance, NULL);
  printf("PASS\n");
  return 0;
}
