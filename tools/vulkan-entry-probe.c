/* Resolve public NVIDIA entrypoints and report their ELF-relative addresses.
 * Does not dereference private driver objects or call private entrypoints.
 * Build: cc -Wall -Wextra -Werror tools/vulkan-entry-probe.c -ldl -o /tmp/vulkan-entry-probe
 * Run on the host. Selects NVIDIA by PCI vendor ID and needs external fd extensions.
 * Output addresses are ELF virtual addresses (not file offsets). No driver version
 * is pinned here: verify the installed library identity before using an address.
 * Initialization failures terminate this short-lived process with a named stage.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

static int fail(const char *stage, int status) {
    fprintf(stderr, "vulkan-entry-probe: %s failed (stage %d)\n", stage, status);
    return status;
}

int main(void) {
    void *lib = dlopen("libEGL_nvidia.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!get)
        return fail("resolve ICD get-instance-proc-address", 2);
    PFN_vkCreateInstance create_instance = (void *)get(NULL, "vkCreateInstance");
    if (!create_instance)
        return fail("resolve create-instance", 4);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    if (create_instance(&ici, NULL, &instance) != VK_SUCCESS)
        return fail("create Vulkan instance", 5);
    PFN_vkEnumeratePhysicalDevices enumerate_devices = (void *)get(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties = (void *)get(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families =
        (void *)get(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create_device = (void *)get(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr device_get = (void *)get(instance, "vkGetDeviceProcAddr");
    PFN_vkDestroyInstance destroy_instance = (void *)get(instance, "vkDestroyInstance");
    if (!enumerate_devices || !properties || !families || !create_device || !device_get || !destroy_instance)
        return fail("resolve instance dispatch", 6);
    uint32_t count = 0;
    if (enumerate_devices(instance, &count, NULL) != VK_SUCCESS || !count)
        return fail("count physical devices", 7);
    VkPhysicalDevice *physical = calloc(count, sizeof(*physical));
    if (!physical || enumerate_devices(instance, &count, physical) != VK_SUCCESS)
        return fail("enumerate physical devices", 8);
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        properties(physical[i], &p);
        if (p.vendorID == 0x10de) {
            selected = physical[i];
            break;
        }
    }
    free(physical);
    if (!selected)
        return fail("find NVIDIA physical device", 9);
    families(selected, &count, NULL);
    VkQueueFamilyProperties *q = calloc(count, sizeof(*q));
    if (!q)
        return fail("allocate queue-family array", 10);
    families(selected, &count, q);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i)
        if (q[i].queueCount && (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            family = i;
            break;
        }
    free(q);
    if (family == UINT32_MAX)
        return fail("find compute queue family", 11);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    const char *extensions[] = {"VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd",
                                "VK_KHR_external_fence_fd"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi,
                              .enabledExtensionCount = 3,
                              .ppEnabledExtensionNames = extensions};
    VkDevice device;
    if (create_device(selected, &dci, NULL, &device) != VK_SUCCESS)
        return fail("create Vulkan device with external fd extensions", 12);
    const char *names[] = {"vkCreateInstance",
                           "vkCreateDevice",
                           "vkGetDeviceProcAddr",
                           "vkAllocateMemory",
                           "vkGetMemoryFdPropertiesKHR",
                           "vkImportSemaphoreFdKHR",
                           "vkGetFenceFdKHR",
                           "vkCreateFence",
                           "vkGetFenceStatus",
                           "vkWaitForFences",
                           "vkResetFences",
                           "vkCreateSemaphore",
                           "vkWaitSemaphores",
                           "vkSignalSemaphore",
                           "vkGetSemaphoreCounterValue",
                           "vkGetSemaphoreFdKHR",
                           "vkCreateImage",
                           "vkQueueSubmit"};
    unsigned unresolved = 0;
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) {
        void *fn = (void *)device_get(device, names[i]);
        if (!fn)
            fn = (void *)get(instance, names[i]);
        Dl_info info = {0};
        if (!fn || !dladdr(fn, &info)) {
            printf("%s\tunresolved\n", names[i]);
            ++unresolved;
            continue;
        }
        printf("%s\t%s\t0x%lx\n", names[i], info.dli_fname, (unsigned long)((uintptr_t)fn - (uintptr_t)info.dli_fbase));
    }
    PFN_vkEnumerateInstanceVersion enumerate = (void *)get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (!enumerate)
        return fail("resolve instance-version query", 3);
    uint32_t version = 0;
    VkResult result = enumerate(&version);
    printf("instance_version\t%d\t%u.%u.%u\n", result, VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
           VK_API_VERSION_PATCH(version));
    PFN_vkDestroyDevice destroy_device = (void *)device_get(device, "vkDestroyDevice");
    if (!destroy_device)
        return fail("resolve destroy-device", 13);
    destroy_device(device, NULL);
    destroy_instance(instance, NULL);
    dlclose(lib);
    if (unresolved)
        return fail("resolve all requested entrypoints", 14);
    return result != VK_SUCCESS;
}
