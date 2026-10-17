/* Public Vulkan fence-domain regression. No RM calls, shaders or GPU spin.
 * See native-vulkan-fence-design.md for scope, failure handling and commands. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <errno.h>
#include <linux/sync_file.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "native-vulkan-runtime.h"

#define FUNCTIONS(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) \
 X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
 X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateCommandPool) \
 X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) \
 X(EndCommandBuffer) X(CmdFillBuffer) X(CmdPipelineBarrier) X(CreateFence) \
 X(DestroyFence) X(WaitForFences) X(GetFenceStatus) X(CreateSemaphore) \
 X(DestroySemaphore) X(SignalSemaphore) X(QueueSubmit)
struct device {
    VkDevice handle;
#define DECLARE(n) PFN_vk##n n;
    FUNCTIONS(DECLARE)
#undef DECLARE
    PFN_vkGetFenceFdKHR GetFenceFdKHR;
};
struct gate {
    struct device *device;
    VkSemaphore semaphore;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint64_t deadline, value;
    bool release;
    atomic_int result;
    atomic_uint_fast64_t signal_started_at, signaled_at;
};
static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static void *signal_gate(void *argument) {
    struct gate *g = argument;
    pthread_mutex_lock(&g->mutex);
    while (!g->release && now_ns() < g->deadline) {
        struct timespec ts = {.tv_sec = (time_t)(g->deadline / 1000000000),
                              .tv_nsec = (long)(g->deadline % 1000000000)};
        int status = pthread_cond_timedwait(&g->condition, &g->mutex, &ts);
        if (status != 0 && status != ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&g->mutex);
    VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                  .semaphore = g->semaphore, .value = g->value ? g->value : 1};
    atomic_store(&g->signal_started_at, now_ns());
    VkResult result = g->device->SignalSemaphore(g->device->handle, &info);
    atomic_store(&g->result, result);
    atomic_store(&g->signaled_at, now_ns());
    return NULL;
}
static void release_gate(struct gate *g) {
    pthread_mutex_lock(&g->mutex);
    g->release = true;
    pthread_cond_signal(&g->condition);
    pthread_mutex_unlock(&g->mutex);
}
static bool stock_mapping_present(void) {
    FILE *file = fopen("/proc/self/maps", "r");
    if (!file) return true;
    bool found = false;
    char line[4096];
    while (fgets(line, sizeof(line), file))
        if (strstr(line, "libnvidia-") || strstr(line, "libEGL_nvidia") ||
            strstr(line, "libnvglibc")) found = true;
    if (ferror(file)) found = true;
    fclose(file);
    return found;
}
static int sync_status(int fd, int *status) {
    struct sync_file_info info = {0};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info)) return -1;
    *status = info.status;
    return 0;
}
static bool number(const char *text, uint64_t *value) {
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || !*text || *end || *text == '-' || parsed > 60000) return false;
    *value = parsed;
    return true;
}
#define REQUIRE(test, message) do { if (!(test)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); goto cleanup; \
} } while (0)
#define CHECK(call) do { VkResult checked = (call); if (checked != VK_SUCCESS) { \
    fprintf(stderr, "FAIL: %s returned %d (line %d)\n", #call, checked, __LINE__); \
    goto cleanup; } } while (0)

struct opaque_device {
    struct device d;
    PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
    PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
    PFN_vkWaitSemaphores WaitSemaphores;
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue;
    PFN_vkResetFences ResetFences;
    VkQueue queue;
    VkSemaphore timeline, binary;
    VkFence fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkCommandPool pool;
    VkCommandBuffer commands[2];
    uint32_t *mapped;
    bool quiescent, submitted;
};

/* Independent logical devices have separate driver object tables/workers.
 * Only opaque semaphore descriptors connect them; buffers are not shared. */
static int opaque_probe(VkInstance instance, VkPhysicalDevice physical,
                        PFN_vkGetInstanceProcAddr gipa, uint64_t delay_ms,
                        uint64_t wait_ms, bool allow_stock, bool *safe) {
    struct opaque_device devices[2] = {{.quiescent = true}, {.quiescent = true}};
    struct opaque_device *producer = &devices[0], *consumer = &devices[1];
    int outcome = 1, fd = -1;
    VkSemaphore host_gate = VK_NULL_HANDLE;
    bool mutex_initialized = false, condition_initialized = false, thread_started = false;
    pthread_t thread;
    struct gate gate = {.device = &producer->d};
    atomic_init(&gate.result, VK_NOT_READY);
    atomic_init(&gate.signal_started_at, 0);
    atomic_init(&gate.signaled_at, 0);
#define INSTANCE(n) PFN_vk##n n = (PFN_vk##n)gipa(instance, "vk" #n); REQUIRE(n, "vk" #n)
    INSTANCE(CreateDevice);
    INSTANCE(GetDeviceProcAddr);
    INSTANCE(GetPhysicalDeviceFeatures2);
    INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(GetPhysicalDeviceMemoryProperties);
    INSTANCE(EnumerateDeviceExtensionProperties);
    INSTANCE(GetPhysicalDeviceExternalSemaphoreProperties);
#undef INSTANCE
    VkPhysicalDeviceTimelineSemaphoreFeatures features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = &features};
    GetPhysicalDeviceFeatures2(physical, &features2);
    REQUIRE(features.timelineSemaphore, "opaque mode requires timeline semaphores");
    VkExtensionProperties extensions[512];
    uint32_t extension_count = 512;
    CHECK(EnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
    const char *extension = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
    bool present = false;
    for (uint32_t i = 0; i < extension_count; i++)
        if (!strcmp(extensions[i].extensionName, extension)) present = true;
    if (!present) {
        puts("SKIP: opaque mode requires VK_KHR_external_semaphore_fd");
        outcome = 77; goto cleanup;
    }
    for (unsigned timeline = 0; timeline < 2; timeline++) {
        VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY};
        VkPhysicalDeviceExternalSemaphoreInfo query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
            .pNext = &type, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        VkExternalSemaphoreProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        GetPhysicalDeviceExternalSemaphoreProperties(physical, &query, &props);
        VkExternalSemaphoreFeatureFlags required = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                                                    VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        if ((props.externalSemaphoreFeatures & required) != required) {
            printf("SKIP: OPAQUE_FD %s import/export unsupported\n", timeline ? "timeline" : "binary");
            outcome = 77; goto cleanup;
        }
    }
    VkQueueFamilyProperties families[64];
    uint32_t family_count = 64, family = UINT32_MAX;
    GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    for (uint32_t i = 0; i < family_count; i++)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    REQUIRE(family != UINT32_MAX, "opaque graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    VkPhysicalDeviceMemoryProperties memory_properties;
    GetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    for (unsigned n = 0; n < 2; n++) {
        struct opaque_device *o = &devices[n];
        struct device *d = &o->d;
        CHECK(CreateDevice(physical, &device_info, NULL, &d->handle));
#define LOAD(n) d->n = (PFN_vk##n)GetDeviceProcAddr(d->handle, "vk" #n); REQUIRE(d->n, "vk" #n);
        FUNCTIONS(LOAD)
#undef LOAD
#define LOAD(n) o->n = (PFN_vk##n)GetDeviceProcAddr(d->handle, "vk" #n); REQUIRE(o->n, "vk" #n);
        LOAD(GetSemaphoreFdKHR) LOAD(ImportSemaphoreFdKHR) LOAD(WaitSemaphores)
        LOAD(GetSemaphoreCounterValue) LOAD(ResetFences)
#undef LOAD
        d->GetDeviceQueue(d->handle, family, 0, &o->queue);
        VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(d->CreateBuffer(d->handle, &buffer_info, NULL, &o->buffer));
        VkMemoryRequirements requirements;
        d->GetBufferMemoryRequirements(d->handle, o->buffer, &requirements);
        uint32_t memory_type = UINT32_MAX;
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
            VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory_properties.memoryTypes[i].propertyFlags & required) == required) { memory_type = i; break; }
        }
        REQUIRE(memory_type != UINT32_MAX, "opaque coherent marker memory");
        VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size, .memoryTypeIndex = memory_type};
        CHECK(d->AllocateMemory(d->handle, &allocate, NULL, &o->memory));
        CHECK(d->BindBufferMemory(d->handle, o->buffer, o->memory, 0));
        CHECK(d->MapMemory(d->handle, o->memory, 0, requirements.size, 0, (void **)&o->mapped));
        for (unsigned i = 0; i < 1024; i++) o->mapped[i] = 0xfeed1234;
        VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = family};
        CHECK(d->CreateCommandPool(d->handle, &pool_info, NULL, &o->pool));
        VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = o->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        CHECK(d->AllocateCommandBuffers(d->handle, &command_info, o->commands));
        for (unsigned cycle = 0; cycle < 2; cycle++) {
            VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            CHECK(d->BeginCommandBuffer(o->commands[cycle], &begin));
            d->CmdFillBuffer(o->commands[cycle], o->buffer, cycle * 4, 4, 0xcafe0000u + n * 16 + cycle);
            VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            d->CmdPipelineBarrier(o->commands[cycle], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &barrier, 0, NULL, 0, NULL);
            CHECK(d->EndCommandBuffer(o->commands[cycle]));
        }
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK(d->CreateFence(d->handle, &fence_info, NULL, &o->fence));
        for (unsigned timeline = 0; timeline < 2; timeline++) {
            VkExportSemaphoreCreateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
            VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .pNext = n == 0 ? &export_info : NULL,
                .semaphoreType = timeline ? VK_SEMAPHORE_TYPE_TIMELINE : VK_SEMAPHORE_TYPE_BINARY};
            VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
            CHECK(d->CreateSemaphore(d->handle, &info, NULL, timeline ? &o->timeline : &o->binary));
        }
    }
    REQUIRE(allow_stock || !stock_mapping_present(), "opaque mode unexpected proprietary driver/shim mapping");
    REQUIRE(producer->d.handle != consumer->d.handle, "independent logical devices");
    for (unsigned timeline = 0; timeline < 2; timeline++) {
        VkSemaphoreGetFdInfoKHR get = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .semaphore = timeline ? producer->timeline : producer->binary,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
        CHECK(producer->GetSemaphoreFdKHR(producer->d.handle, &get, &fd));
        REQUIRE(fd >= 0, "opaque export descriptor");
        VkImportSemaphoreFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = timeline ? consumer->timeline : consumer->binary,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, .fd = fd};
        CHECK(consumer->ImportSemaphoreFdKHR(consumer->d.handle, &import));
        fd = -1; /* Successful opaque import consumes the descriptor. */
    }
    VkSemaphoreTypeCreateInfo host_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo host_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &host_type};
    CHECK(producer->d.CreateSemaphore(producer->d.handle, &host_info, NULL, &host_gate));
    gate.semaphore = host_gate;
    REQUIRE(pthread_mutex_init(&gate.mutex, NULL) == 0, "opaque gate mutex");
    mutex_initialized = true;
    pthread_condattr_t attr;
    REQUIRE(pthread_condattr_init(&attr) == 0, "opaque gate attributes");
    int attr_status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int condition_status = attr_status ? attr_status : pthread_cond_init(&gate.condition, &attr);
    pthread_condattr_destroy(&attr);
    REQUIRE(condition_status == 0, "opaque monotonic gate condition");
    condition_initialized = true;
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        if (cycle) {
            for (unsigned n = 0; n < 2; n++) {
                CHECK(devices[n].ResetFences(devices[n].d.handle, 1, &devices[n].fence));
                REQUIRE(devices[n].d.GetFenceStatus(devices[n].d.handle, devices[n].fence) == VK_NOT_READY,
                        "ordinary fence reset between opaque binary cycles");
                devices[n].submitted = false;
            }
        }
        gate.release = false;
        gate.value = cycle + 1;
        atomic_store(&gate.result, VK_NOT_READY);
        atomic_store(&gate.signal_started_at, 0);
        atomic_store(&gate.signaled_at, 0);
        uint64_t start = now_ns(), value = cycle + 1;
        gate.deadline = start + delay_ms * UINT64_C(1000000);
        REQUIRE(pthread_create(&thread, NULL, signal_gate, &gate) == 0, "opaque host signal thread");
        thread_started = true;
        VkSemaphore producer_signals[] = {producer->timeline, producer->binary};
        uint64_t values[] = {value, 0};
        VkTimelineSemaphoreSubmitInfo producer_values = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value,
            .signalSemaphoreValueCount = 2, .pSignalSemaphoreValues = values};
        VkPipelineStageFlags stages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
        VkSubmitInfo produce = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &producer_values,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &host_gate, .pWaitDstStageMask = stages,
            .commandBufferCount = 1, .pCommandBuffers = &producer->commands[cycle],
            .signalSemaphoreCount = 2, .pSignalSemaphores = producer_signals};
        producer->quiescent = false;
        CHECK(producer->d.QueueSubmit(producer->queue, 1, &produce, producer->fence));
        producer->submitted = true;
        VkSemaphore consumer_waits[] = {consumer->timeline, consumer->binary};
        /* On reuse, wait only on binary: a stale completed binary generation
         * must not be hidden by the independently correct timeline wait. */
        unsigned wait_offset = cycle ? 1 : 0;
        VkTimelineSemaphoreSubmitInfo consumer_values = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 2 - wait_offset, .pWaitSemaphoreValues = values + wait_offset};
        VkSubmitInfo consume = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &consumer_values,
            .waitSemaphoreCount = 2 - wait_offset, .pWaitSemaphores = consumer_waits + wait_offset,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1, .pCommandBuffers = &consumer->commands[cycle]};
        consumer->quiescent = false;
        CHECK(consumer->d.QueueSubmit(consumer->queue, 1, &consume, consumer->fence));
        consumer->submitted = true;
        VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1, .pSemaphores = &consumer->timeline, .pValues = &value};
        VkResult early_cpu = consumer->WaitSemaphores(consumer->d.handle, &wait, UINT64_C(1000000));
        VkResult early_gpu = consumer->d.WaitForFences(consumer->d.handle, 1, &consumer->fence,
                                                     VK_TRUE, UINT64_C(1000000));
        printf("opaque cycle=%u before_host_signal imported_cpu_wait=%d gpu_fence_wait=%d elapsed_ms=%.3f\n",
               cycle + 1, early_cpu, early_gpu, (now_ns() - start) / 1e6);
        REQUIRE(!atomic_load(&gate.signal_started_at), "opaque gate expired before pending observation");
        REQUIRE(early_cpu == VK_TIMEOUT && early_gpu == VK_TIMEOUT, "opaque imported waits remain pending");
        while (!atomic_load(&gate.signal_started_at)) {
            uint64_t counter = UINT64_MAX;
            CHECK(consumer->GetSemaphoreCounterValue(consumer->d.handle, consumer->timeline, &counter));
            if (!atomic_load(&gate.signal_started_at)) {
                REQUIRE(counter == cycle, "imported timeline must not advance before producer signal");
                for (unsigned n = 0; n < 2; n++) {
                    VkResult status = devices[n].d.GetFenceStatus(devices[n].d.handle, devices[n].fence);
                    REQUIRE(atomic_load(&gate.signal_started_at) || status == VK_NOT_READY,
                            "opaque producer/consumer fence must remain pending");
                }
            }
            VkResult pending = consumer->WaitSemaphores(consumer->d.handle, &wait, UINT64_C(10000000));
            REQUIRE(pending == VK_TIMEOUT || pending == VK_SUCCESS, "opaque imported timeline wait during gate");
            REQUIRE(pending != VK_SUCCESS || atomic_load(&gate.signal_started_at),
                    "opaque imported timeline completed before producer dependency");
        }
        REQUIRE(pthread_join(thread, NULL) == 0, "join opaque host signal thread");
        thread_started = false;
        REQUIRE(atomic_load(&gate.result) == VK_SUCCESS, "opaque host gate signal");
        CHECK(consumer->WaitSemaphores(consumer->d.handle, &wait, wait_ms * UINT64_C(1000000)));
        for (unsigned n = 0; n < 2; n++) {
            struct opaque_device *o = &devices[n];
            CHECK(o->d.WaitForFences(o->d.handle, 1, &o->fence, VK_TRUE, wait_ms * UINT64_C(1000000)));
            o->quiescent = true;
            uint64_t counter;
            CHECK(o->GetSemaphoreCounterValue(o->d.handle, o->timeline, &counter));
            REQUIRE(counter == value, "exporter/importer timeline values match");
            for (unsigned i = 0; i < 1024; i++) {
                uint32_t expected = i <= cycle ? 0xcafe0000u + n * 16 + i : 0xfeed1234;
                REQUIRE(o->mapped[i] == expected, "opaque device marker/guards after actual completion");
            }
        }
        printf("opaque cycle=%u completed elapsed_ms=%.3f imported_value=%llu binary_consumed=1 markers=PASS\n",
               cycle + 1, (now_ns() - start) / 1e6, (unsigned long long)value);
    }
    puts("PASS: two-device OPAQUE_FD timeline CPU/GPU waits, binary consume/resignal, fence reset and second submission");
    outcome = 0;
cleanup:
    if (thread_started) {
        release_gate(&gate);
        pthread_join(thread, NULL);
        fprintf(stderr, "opaque cleanup: gate released/joined signal=%d\n", atomic_load(&gate.result));
    }
    if (condition_initialized) pthread_cond_destroy(&gate.condition);
    if (mutex_initialized) pthread_mutex_destroy(&gate.mutex);
    if (fd >= 0) close(fd);
    for (unsigned n = 0; n < 2; n++) {
        struct opaque_device *o = &devices[n];
        if (!o->quiescent && o->submitted && atomic_load(&gate.result) == VK_SUCCESS) {
            VkResult drain = o->d.WaitForFences(o->d.handle, 1, &o->fence, VK_TRUE,
                                               wait_ms * UINT64_C(1000000));
            fprintf(stderr, "opaque cleanup: device=%u finite completion wait=%d\n", n, drain);
            o->quiescent = drain == VK_SUCCESS;
        }
    }
    *safe = devices[0].quiescent && devices[1].quiescent;
    if (!*safe) {
        fputs("FAIL: opaque quiescence unproven; skipping Vulkan destruction/dlclose, process exit owns cleanup\n", stderr);
        return 1;
    }
    if (host_gate) producer->d.DestroySemaphore(producer->d.handle, host_gate, NULL);
    for (unsigned n = 0; n < 2; n++) {
        struct opaque_device *o = &devices[n]; struct device *d = &o->d;
        if (!d->handle || !d->DestroyDevice) continue;
        if (o->timeline) d->DestroySemaphore(d->handle, o->timeline, NULL);
        if (o->binary) d->DestroySemaphore(d->handle, o->binary, NULL);
        if (o->fence) d->DestroyFence(d->handle, o->fence, NULL);
        if (o->pool) d->DestroyCommandPool(d->handle, o->pool, NULL);
        if (o->mapped) d->UnmapMemory(d->handle, o->memory);
        if (o->buffer) d->DestroyBuffer(d->handle, o->buffer, NULL);
        if (o->memory) d->FreeMemory(d->handle, o->memory, NULL);
        d->DestroyDevice(d->handle, NULL);
    }
    return outcome;
}

int main(int argc, char **argv) {
    const char *driver = NULL;
    uint64_t delay_ms = 100, wait_ms = 2000;
    bool allow_stock = false, export_fd = false, expect_timeout = false, opaque = false;
    bool single_wait = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--driver") && i + 1 < argc) driver = argv[++i];
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) {
            if (!number(argv[++i], &delay_ms) || delay_ms < 10) return 2;
        } else if (!strcmp(argv[i], "--wait-ms") && i + 1 < argc) {
            if (!number(argv[++i], &wait_ms) || wait_ms == 0) return 2;
        } else if (!strcmp(argv[i], "--allow-stock-driver")) allow_stock = true;
        else if (!strcmp(argv[i], "--sync-fd")) export_fd = true;
        else if (!strcmp(argv[i], "--opaque")) opaque = true;
        else if (!strcmp(argv[i], "--single-wait")) single_wait = true;
        else if (!strcmp(argv[i], "--expect-export-timeout")) expect_timeout = true;
        else {
            fprintf(stderr, "usage: %s --driver /absolute/ICD.so [--allow-stock-driver] "
                    "[--delay-ms 100] [--wait-ms 2000] [--sync-fd] "
                    "[--expect-export-timeout] [--opaque] [--single-wait]\n", argv[0]);
            return 2;
        }
    }
    if ((opaque && export_fd) || !driver || driver[0] != '/' || (expect_timeout && (!export_fd || delay_ms < 5500))) {
        fputs("FAIL: absolute --driver required; timeout expectation needs --sync-fd and delay >=5500ms; --opaque excludes --sync-fd\n", stderr);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    nv_probe_android_runtime_init();
    int before_fds = nv_probe_runtime_fd_count();
    int outcome = 1, fd = -1;
    void *library = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance DestroyInstance = NULL;
    struct device d = {0};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    VkFence first = VK_NULL_HANDLE, witness = VK_NULL_HANDLE, second = VK_NULL_HANDLE;
    VkFence drain_fence = VK_NULL_HANDLE;
    uint32_t *mapped = NULL;
    bool thread_started = false, gate_initialized = false, submitted = false;
    bool quiescent = true, witness_submitted = false;
    pthread_t thread;
    struct gate gate = {.device = &d};
    atomic_init(&gate.result, VK_NOT_READY);
    atomic_init(&gate.signal_started_at, 0);
    atomic_init(&gate.signaled_at, 0);
    library = dlopen(driver, RTLD_NOW | RTLD_LOCAL);
    if (!library) fprintf(stderr, "dlopen: %s\n", dlerror());
    REQUIRE(library, "load explicit ICD");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
    if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    REQUIRE(gipa, "instance resolver");
    PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    REQUIRE(CreateInstance, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "RM fence domain regression", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                         .pApplicationInfo = &app};
    CHECK(CreateInstance(&instance_info, NULL, &instance));
    DestroyInstance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    REQUIRE(DestroyInstance, "vkDestroyInstance");
#define INSTANCE(n) PFN_vk##n n = (PFN_vk##n)gipa(instance, "vk" #n); REQUIRE(n, "vk" #n)
    INSTANCE(EnumeratePhysicalDevices);
    INSTANCE(GetPhysicalDeviceProperties);
    INSTANCE(GetPhysicalDeviceFeatures2);
    INSTANCE(GetPhysicalDeviceMemoryProperties);
    INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(EnumerateDeviceExtensionProperties);
    INSTANCE(GetPhysicalDeviceExternalFenceProperties);
    INSTANCE(CreateDevice);
    INSTANCE(GetDeviceProcAddr);
#undef INSTANCE
    VkPhysicalDevice devices[32], physical = VK_NULL_HANDLE;
    uint32_t count = 32;
    CHECK(EnumeratePhysicalDevices(instance, &count, devices));
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        GetPhysicalDeviceProperties(devices[i], &props);
        if (props.vendorID != 0x10de) continue;
        physical = devices[i];
        printf("device=%s vendor=%04x device=%04x api=%u.%u.%u driver=%u\n",
            props.deviceName, props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
        REQUIRE(props.apiVersion >= VK_API_VERSION_1_2, "Vulkan1.2 for core timeline operations");
        break;
    }
    REQUIRE(physical, "NVIDIA vendor device");
    if (opaque) {
        bool safe = false;
        outcome = opaque_probe(instance, physical, gipa, delay_ms, wait_ms, allow_stock, &safe);
        if (!safe) return 1; /* The helper deliberately left live GPU state intact. */
        goto cleanup;
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                         .pNext = &timeline_features};
    GetPhysicalDeviceFeatures2(physical, &features);
    REQUIRE(timeline_features.timelineSemaphore, "timelineSemaphore feature");
    const char *extension = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME;
    if (export_fd) {
        VkExtensionProperties extensions[512];
        uint32_t extension_count = 512;
        CHECK(EnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions));
        bool present = false;
        for (uint32_t i = 0; i < extension_count; i++)
            if (!strcmp(extensions[i].extensionName, extension)) present = true;
        VkPhysicalDeviceExternalFenceInfo query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
            .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        VkExternalFenceProperties properties = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
        GetPhysicalDeviceExternalFenceProperties(physical, &query, &properties);
        if (!present || !(properties.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT)) {
            puts("SKIP: selected device does not support exportable SYNC_FD fences");
            outcome = 77;
            goto cleanup;
        }
    }
    VkQueueFamilyProperties families[64];
    uint32_t family_count = 64, family = UINT32_MAX;
    GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    for (uint32_t i = 0; i < family_count; i++)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    REQUIRE(family != UINT32_MAX, "graphics queue with transfer support");
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &timeline_features, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = export_fd ? 1 : 0, .ppEnabledExtensionNames = export_fd ? &extension : NULL};
    CHECK(CreateDevice(physical, &device_info, NULL, &d.handle));
#define LOAD(n) d.n = (PFN_vk##n)GetDeviceProcAddr(d.handle, "vk" #n); REQUIRE(d.n, "vk" #n);
    FUNCTIONS(LOAD)
#undef LOAD
    if (export_fd) {
        d.GetFenceFdKHR = (PFN_vkGetFenceFdKHR)GetDeviceProcAddr(d.handle, "vkGetFenceFdKHR");
        REQUIRE(d.GetFenceFdKHR, "vkGetFenceFdKHR");
    }
    REQUIRE(allow_stock || !stock_mapping_present(), "unexpected proprietary driver/shim mapping (use explicit --allow-stock-driver)");
    VkQueue queue;
    d.GetDeviceQueue(d.handle, family, 0, &queue);
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4096, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    CHECK(d.CreateBuffer(d.handle, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    d.GetBufferMemoryRequirements(d.handle, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory_properties;
    GetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    uint32_t memory_type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
        VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & required) == required) { memory_type = i; break; }
    }
    REQUIRE(memory_type != UINT32_MAX, "host-visible coherent marker memory");
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = memory_type};
    CHECK(d.AllocateMemory(d.handle, &allocate, NULL, &memory));
    CHECK(d.BindBufferMemory(d.handle, buffer, memory, 0));
    CHECK(d.MapMemory(d.handle, memory, 0, requirements.size, 0, (void **)&mapped));
    for (unsigned i = 0; i < 1024; i++) mapped[i] = 0xfeed1234;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family};
    CHECK(d.CreateCommandPool(d.handle, &pool_info, NULL, &pool));
    VkCommandBuffer commands[2];
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    CHECK(d.AllocateCommandBuffers(d.handle, &command_info, commands));
    for (unsigned i = 0; i < 2; i++) {
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(d.BeginCommandBuffer(commands[i], &begin));
        d.CmdFillBuffer(commands[i], buffer, i * 4, 4, 0xcafe0000u + i);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        d.CmdPipelineBarrier(commands[i], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        CHECK(d.EndCommandBuffer(commands[i]));
    }
    VkSemaphoreTypeCreateInfo semaphore_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &semaphore_type};
    CHECK(d.CreateSemaphore(d.handle, &semaphore_info, NULL, &timeline));
    gate.semaphore = timeline;
    VkExportFenceCreateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = export_fd ? &export_info : NULL};
    CHECK(d.CreateFence(d.handle, &fence_info, NULL, &first));
    fence_info.pNext = NULL;
    CHECK(d.CreateFence(d.handle, &fence_info, NULL, &witness));
    CHECK(d.CreateFence(d.handle, &fence_info, NULL, &second));
    REQUIRE(pthread_mutex_init(&gate.mutex, NULL) == 0, "gate mutex");
    pthread_condattr_t attr;
    REQUIRE(pthread_condattr_init(&attr) == 0, "gate condition attributes");
    REQUIRE(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0, "monotonic gate clock");
    REQUIRE(pthread_cond_init(&gate.condition, &attr) == 0, "gate condition");
    pthread_condattr_destroy(&attr);
    gate_initialized = true;
    uint64_t start = now_ns();
    gate.deadline = start + delay_ms * UINT64_C(1000000);
    REQUIRE(pthread_create(&thread, NULL, signal_gate, &gate) == 0, "gate signal thread");
    thread_started = true;
    uint64_t value = 1;
    VkTimelineSemaphoreSubmitInfo timeline_submit = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline_submit,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &timeline, .pWaitDstStageMask = &stage,
        .commandBufferCount = 1, .pCommandBuffers = &commands[0]};
    submitted = true;
    quiescent = false;
    CHECK(d.QueueSubmit(queue, 1, &submit, first));
    VkSubmitInfo empty = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    CHECK(d.QueueSubmit(queue, 1, &empty, witness));
    witness_submitted = true;
    drain_fence = witness;
    printf("submitted delay_ms=%llu wait_ms=%llu submit_wall_ms=%.3f\n",
        (unsigned long long)delay_ms, (unsigned long long)wait_ms, (now_ns() - start) / 1e6);
    VkResult early_wait = d.WaitForFences(d.handle, 1, &first, VK_TRUE, UINT64_C(1000000));
    VkResult early_status = d.GetFenceStatus(d.handle, first);
    printf("before_host_signal finite_wait=%d fence_status=%d\n", early_wait, early_status);
    REQUIRE(atomic_load(&gate.signal_started_at) == 0, "gate expired before pending observation");
    REQUIRE(early_wait == VK_TIMEOUT && early_status == VK_NOT_READY, "blocked ordinary fence timeout/status");
    bool timed_out_export = false;
    if (export_fd) {
        VkFenceGetFdInfoKHR get = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
            .fence = first, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        uint64_t export_start = now_ns();
        CHECK(d.GetFenceFdKHR(d.handle, &get, &fd));
        printf("export wall_ms=%.3f fd=%d gate_signaled=%d\n", (now_ns() - export_start) / 1e6,
            fd, atomic_load(&gate.signal_started_at) != 0);
        REQUIRE(fd >= 0 && atomic_load(&gate.signal_started_at) == 0, "SYNC_FD export returned a genuinely pending payload");
        int status;
        REQUIRE(sync_status(fd, &status) == 0 && status == 0, "exported sync_file initially pending");
        REQUIRE(d.GetFenceStatus(d.handle, first) == VK_NOT_READY, "SYNC_FD export resets source fence");
        while (!atomic_load(&gate.signal_started_at)) {
            struct pollfd poll_fd = {.fd = fd, .events = POLLIN};
            int ready = poll(&poll_fd, 1, 10);
            if (ready < 0 && errno == EINTR) continue;
            REQUIRE(ready >= 0, "poll exported sync_file");
            if (!ready) continue;
            REQUIRE(!(poll_fd.revents & POLLNVAL), "valid exported descriptor");
            REQUIRE(sync_status(fd, &status) == 0, "read exported fence result");
            bool gate_done = atomic_load(&gate.signal_started_at) != 0;
            printf("sync_file ready elapsed_ms=%.3f status=%d gate_signaled=%d ordinary_witness=%d\n",
                (now_ns() - start) / 1e6, status, gate_done, d.GetFenceStatus(d.handle, witness));
            if (!gate_done) {
                REQUIRE(status < 0, "exported fence cannot succeed before host dependency");
                timed_out_export = status == -ETIME || status == -ETIMEDOUT;
                REQUIRE(d.GetFenceStatus(d.handle, witness) == VK_NOT_READY,
                        "exported error must not falsely complete/lose ordinary witness");
            }
            break;
        }
    }
    /* Keep observing ordinary completion through the whole held interval.
     * A started signal call may legitimately unblock GPU work before that host
     * call returns, so distinguish its start from its recorded return time. */
    while (!atomic_load(&gate.signal_started_at)) {
        /* A long API wait lets syscall tracing distinguish the driver's own
         * timeout slices from this probe's usual 10 ms caller deadline. */
        uint64_t interval_ns = single_wait ? wait_ms * UINT64_C(1000000) : UINT64_C(10000000);
        VkResult pending = d.WaitForFences(d.handle, 1, &witness, VK_TRUE, interval_ns);
        if (pending != VK_TIMEOUT && !atomic_load(&gate.signal_started_at)) {
            fprintf(stderr, "FAIL: ordinary witness returned %d before host signal at %.3fms\n",
                    pending, (now_ns() - start) / 1e6);
            goto cleanup;
        }
        REQUIRE(pending == VK_TIMEOUT || pending == VK_SUCCESS, "ordinary witness during host release");
    }
    REQUIRE(pthread_join(thread, NULL) == 0, "join gate signal thread");
    thread_started = false;
    printf("host_signal result=%d elapsed_ms=%.3f\n", atomic_load(&gate.result),
        (atomic_load(&gate.signaled_at) - start) / 1e6);
    REQUIRE(atomic_load(&gate.result) == VK_SUCCESS, "host timeline signal");
    VkResult completion = d.WaitForFences(d.handle, 1, &witness, VK_TRUE, wait_ms * UINT64_C(1000000));
    printf("actual_completion ordinary_witness=%d elapsed_ms=%.3f\n", completion, (now_ns() - start) / 1e6);
    REQUIRE(completion == VK_SUCCESS, "ordinary completion after released dependency");
    quiescent = true;
    REQUIRE(mapped[0] == 0xcafe0000 && mapped[1] == 0xfeed1234, "first completed marker and untouched second marker");
    if (!export_fd) REQUIRE(d.GetFenceStatus(d.handle, first) == VK_SUCCESS, "ordinary first fence completion");
    if (export_fd) {
        int status;
        REQUIRE(sync_status(fd, &status) == 0, "final exported sync_file status");
        printf("export_final status=%d timed_out_before_completion=%d\n", status, timed_out_export);
        REQUIRE(status != 0, "exported event finished");
        REQUIRE(!expect_timeout || timed_out_export, "explicit exported-timeout expectation");
    }
    VkSubmitInfo next = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &commands[1]};
    quiescent = false;
    witness_submitted = false;
    CHECK(d.QueueSubmit(queue, 1, &next, second));
    drain_fence = second; /* Last submitted completion used by finite failure cleanup. */
    witness_submitted = true;
    CHECK(d.WaitForFences(d.handle, 1, &second, VK_TRUE, wait_ms * UINT64_C(1000000)));
    quiescent = true;
    REQUIRE(mapped[0] == 0xcafe0000 && mapped[1] == 0xcafe0001, "device works on second submission");
    for (unsigned i = 2; i < 1024; i++) REQUIRE(mapped[i] == 0xfeed1234, "marker buffer guards");
    puts("PASS: pending ordinary fence, host timeline release, actual marker completion, second submission and guards");
    outcome = 0;

cleanup:
    if (thread_started) {
        release_gate(&gate);
        pthread_join(thread, NULL);
        thread_started = false;
        fprintf(stderr, "cleanup: host gate released/joined, signal result=%d\n", atomic_load(&gate.result));
    }
    if (gate_initialized) {
        pthread_cond_destroy(&gate.condition);
        pthread_mutex_destroy(&gate.mutex);
    }
    if (submitted && !quiescent && witness_submitted && atomic_load(&gate.result) == VK_SUCCESS) {
        VkResult drain = d.WaitForFences(d.handle, 1, &drain_fence, VK_TRUE, wait_ms * UINT64_C(1000000));
        fprintf(stderr, "cleanup: finite completion wait=%d\n", drain);
        quiescent = drain == VK_SUCCESS;
    }
    if (fd >= 0) close(fd);
    if (!quiescent) {
        fputs("FAIL: quiescence unproven; skipping Vulkan object destruction/dlclose, process exit owns cleanup\n", stderr);
        return 1;
    }
    if (d.handle && d.DestroyDevice) {
        if (witness) d.DestroyFence(d.handle, witness, NULL);
        if (second) d.DestroyFence(d.handle, second, NULL);
        if (first) d.DestroyFence(d.handle, first, NULL);
        if (timeline) d.DestroySemaphore(d.handle, timeline, NULL);
        if (pool) d.DestroyCommandPool(d.handle, pool, NULL);
        if (mapped) d.UnmapMemory(d.handle, memory);
        if (buffer) d.DestroyBuffer(d.handle, buffer, NULL);
        if (memory) d.FreeMemory(d.handle, memory, NULL);
        d.DestroyDevice(d.handle, NULL);
    }
    if (instance && DestroyInstance) DestroyInstance(instance, NULL);
    if (library) dlclose(library);
    nv_probe_android_logger_close();
    printf("runtime_fds before=%d after=%d\n", before_fds, nv_probe_runtime_fd_count());
    return outcome;
}
