/* vkcaps: headless Vulkan capability + interop probe for the Android HAL design.
 * Measures (does not infer) what the NVIDIA ICD offers for the two contracts the
 * Vulkan HAL depends on: gralloc dma-buf import/export with DRM format modifiers,
 * and Android sync_fd semaphores/fences (VK_ANDROID_native_buffer semantics).
 *
 * GPU is selected by vendorID 0x10de, never by node path. No window system.
 * Bootstraps (same mechanism as vkcompute.c):
 *   default     : Khronos loader (linked with -lvulkan) -> ICD manifest -> libGLX_nvidia.so.0
 *   --icd PATH  : dlopen PATH, vk_icdNegotiateLoaderICDInterfaceVersion + vk_icdGetInstanceProcAddr
 * Exit 0 only if every step that the driver advertises as supported actually worked.
 * Expected negatives (unsupported format/modifier/usage combinations) are printed, not fatal.
 * Every unexpected failure prints "UNEXPECTED:" and makes the exit status 1. Bootstrap
 * failures exit 2. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/sync_file.h>
#include <linux/dma-buf.h>
#include <vulkan/vulkan.h>

typedef PFN_vkVoidFunction (*PFN_vk_icdGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t *);
static PFN_vk_icdGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static int fails;

#define UNEXPECTED(...) do { printf("    UNEXPECTED: " __VA_ARGS__); fails++; } while (0)

#define GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define INST_FNS(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties2) X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties) X(vkGetPhysicalDeviceExternalFenceProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEV_FNS(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements2) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkGetMemoryFdKHR) X(vkGetMemoryFdPropertiesKHR) X(vkGetImageDrmFormatModifierPropertiesEXT) \
    X(vkGetImageSubresourceLayout) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkGetSemaphoreFdKHR) \
    X(vkImportSemaphoreFdKHR) X(vkCreateFence) X(vkDestroyFence) X(vkGetFenceFdKHR) X(vkImportFenceFdKHR) \
    X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory)
#define DECL(n) static PFN_##n p_##n;
GLOBAL_FNS(DECL) INST_FNS(DECL) DEV_FNS(DECL)
#define LOAD_G(n) if (!(p_##n = (PFN_##n)gipa(NULL, #n))) { fprintf(stderr, "gipa(NULL): %s missing\n", #n); exit(2); }
#define LOAD_I(n) if (!(p_##n = (PFN_##n)gipa(inst, #n))) { fprintf(stderr, "gipa(inst): %s missing\n", #n); exit(2); }
#define LOAD_D(n) if (!(p_##n = (PFN_##n)gdpa(dev, #n))) { fprintf(stderr, "vkGetDeviceProcAddr: %s missing\n", #n); exit(2); }

static const char *res(VkResult r) {
    switch (r) {
#define R(x) case x: return #x;
    R(VK_SUCCESS) R(VK_NOT_READY) R(VK_TIMEOUT) R(VK_EVENT_SET) R(VK_EVENT_RESET) R(VK_INCOMPLETE)
    R(VK_ERROR_OUT_OF_HOST_MEMORY) R(VK_ERROR_OUT_OF_DEVICE_MEMORY) R(VK_ERROR_INITIALIZATION_FAILED)
    R(VK_ERROR_DEVICE_LOST) R(VK_ERROR_MEMORY_MAP_FAILED) R(VK_ERROR_LAYER_NOT_PRESENT)
    R(VK_ERROR_EXTENSION_NOT_PRESENT) R(VK_ERROR_FEATURE_NOT_PRESENT) R(VK_ERROR_INCOMPATIBLE_DRIVER)
    R(VK_ERROR_TOO_MANY_OBJECTS) R(VK_ERROR_FORMAT_NOT_SUPPORTED) R(VK_ERROR_FRAGMENTED_POOL)
    R(VK_ERROR_UNKNOWN) R(VK_ERROR_OUT_OF_POOL_MEMORY) R(VK_ERROR_INVALID_EXTERNAL_HANDLE)
    R(VK_ERROR_FRAGMENTATION) R(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
    R(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT) R(VK_ERROR_NOT_PERMITTED_KHR)
#undef R
    default: { static char b[32]; snprintf(b, sizeof b, "VkResult(%d)", r); return b; }
    }
}

static void print_flags(uint64_t v, const char *const *names, const uint64_t *bits, size_t n) {
    printf("[");
    int first = 1; uint64_t rest = v;
    for (size_t i = 0; i < n; i++) if (v & bits[i]) { printf("%s%s", first ? "" : "|", names[i]); first = 0; rest &= ~bits[i]; }
    if (rest) printf("%s0x%" PRIx64, first ? "" : "|", rest);
    else if (first) printf("none");
    printf("]");
}
#define FLAGS(v, ...) do { static const uint64_t b_[] = { __VA_ARGS__ }; static const char *const n_[] = { FLAGNAMES }; \
    print_flags((v), n_, b_, sizeof b_ / sizeof b_[0]); } while (0)

static void p_sem_types(VkExternalSemaphoreHandleTypeFlags v) {
#define FLAGNAMES "OPAQUE_FD", "OPAQUE_WIN32", "OPAQUE_WIN32_KMT", "D3D12_FENCE", "SYNC_FD"
    FLAGS(v, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT,
          VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
#undef FLAGNAMES
}
static void p_fence_types(VkExternalFenceHandleTypeFlags v) {
#define FLAGNAMES "OPAQUE_FD", "OPAQUE_WIN32", "OPAQUE_WIN32_KMT", "SYNC_FD"
    FLAGS(v, VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
          VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
#undef FLAGNAMES
}
static void p_mem_types(VkExternalMemoryHandleTypeFlags v) {
#define FLAGNAMES "OPAQUE_FD", "OPAQUE_WIN32", "OPAQUE_WIN32_KMT", "D3D11_TEXTURE", "D3D11_TEXTURE_KMT", "D3D12_HEAP", \
    "D3D12_RESOURCE", "DMA_BUF", "HOST_ALLOCATION", "HOST_MAPPED_FOREIGN_MEMORY", "ANDROID_HARDWARE_BUFFER"
    FLAGS(v, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT,
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT,
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
#undef FLAGNAMES
}
static void p_mem_feat(VkExternalMemoryFeatureFlags v) {
#define FLAGNAMES "DEDICATED_ONLY", "EXPORTABLE", "IMPORTABLE"
    FLAGS(v, VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT, VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
#undef FLAGNAMES
}
static void p_two_feat(uint32_t v) { /* semaphore and fence feature bits have the same values */
#define FLAGNAMES "EXPORTABLE", "IMPORTABLE"
    FLAGS(v, VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT, VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
#undef FLAGNAMES
}
static void p_fmt_feat2(VkFormatFeatureFlags2 v) {
#define FLAGNAMES "SAMPLED", "STORAGE", "COLOR_ATT", "COLOR_ATT_BLEND", "DEPTH_STENCIL_ATT", "BLIT_SRC", "BLIT_DST", \
    "FILTER_LINEAR", "TRANSFER_SRC", "TRANSFER_DST", "MIDPOINT_CHROMA", "COSITED_CHROMA", "YCBCR_LINEAR_FILTER", \
    "DISJOINT", "STORAGE_READ_NO_FMT", "STORAGE_WRITE_NO_FMT", "SAMPLED_DEPTH_CMP", "FILTER_MINMAX", "HOST_IMAGE_TRANSFER", \
    "LINEAR_COLOR_ATT_NV", "COPY_IMAGE_INDIRECT_DST"
    FLAGS(v, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT,
          VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT,
          VK_FORMAT_FEATURE_2_BLIT_SRC_BIT, VK_FORMAT_FEATURE_2_BLIT_DST_BIT, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
          VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT, VK_FORMAT_FEATURE_2_MIDPOINT_CHROMA_SAMPLES_BIT,
          VK_FORMAT_FEATURE_2_COSITED_CHROMA_SAMPLES_BIT, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT,
          VK_FORMAT_FEATURE_2_DISJOINT_BIT, VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT,
          VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT, VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT,
          VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_MINMAX_BIT, VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT,
          VK_FORMAT_FEATURE_2_LINEAR_COLOR_ATTACHMENT_BIT_NV, VK_FORMAT_FEATURE_2_COPY_IMAGE_INDIRECT_DST_BIT_KHR);
#undef FLAGNAMES
}
static void p_memprop(VkMemoryPropertyFlags v) {
#define FLAGNAMES "DEVICE_LOCAL", "HOST_VISIBLE", "HOST_COHERENT", "HOST_CACHED", "LAZY", "PROTECTED"
    FLAGS(v, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, VK_MEMORY_PROPERTY_PROTECTED_BIT);
#undef FLAGNAMES
}

/* Modifier naming follows include/uapi/drm/drm_fourcc.h (DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D). */
static void p_modifier(uint64_t m) {
    printf("0x%016" PRIx64, m);
    if (m == 0) { printf(" DRM_FORMAT_MOD_LINEAR"); return; }
    if (m == 0x00ffffffffffffffull) { printf(" DRM_FORMAT_MOD_INVALID"); return; }
    if ((m >> 56) == 0x03 && (m & 0x10)) {
        unsigned h = m & 0xf, k = (m >> 12) & 0xff, g = (m >> 20) & 3, c = (m >> 23) & 7;
        unsigned s = ((m >> 22) & 1) | ((m >> 25) & 6);
        printf(" NVIDIA_BLOCK_LINEAR_2D(c=%u,s=%u,g=%u,k=0x%02x,h=%u)", c, s, g, k, h);
    } else printf(" vendor=0x%02x", (unsigned)(m >> 56));
}

/* ---------------------------------------------------------------- state */
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool cpool;
static VkCommandBuffer cmd;
static VkPhysicalDeviceMemoryProperties memprops;
static int have_dev_ext_fmt2;

struct fmt { VkFormat f; const char *name; int depth; };
static const struct fmt fmts[] = {
    { VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 0 },
    { VK_FORMAT_R8G8B8A8_SRGB, "R8G8B8A8_SRGB", 0 },
    { VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM", 0 },
    { VK_FORMAT_B8G8R8A8_SRGB, "B8G8R8A8_SRGB", 0 },
    { VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM_PACK16", 0 },
    { VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT", 0 },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32", 0 },
    { VK_FORMAT_A2R10G10B10_UNORM_PACK32, "A2R10G10B10_UNORM_PACK32", 0 },
    { VK_FORMAT_R8_UNORM, "R8_UNORM", 0 },
    { VK_FORMAT_R8G8_UNORM, "R8G8_UNORM", 0 },
    { VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT", 1 },
    { VK_FORMAT_D32_SFLOAT, "D32_SFLOAT", 1 },
    { VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, "G8_B8R8_2PLANE_420_UNORM", 0 },
    { VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, "G8_B8_R8_3PLANE_420_UNORM", 0 },
};
#define NFMT (sizeof fmts / sizeof fmts[0])

/* modifier list of R8G8B8A8_UNORM, kept for the round trip */
static VkDrmFormatModifierProperties2EXT rgba_mods[64];
static uint32_t rgba_nmods;

/* ---------------------------------------------------------------- bootstrap */
static void bootstrap_icd(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); exit(2); }
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion neg =
        (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    gipa = (PFN_vk_icdGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!neg || !gipa) { fprintf(stderr, "%s: vk_icd* entry points missing\n", path); exit(2); }
    uint32_t ver = 5; VkResult r = neg(&ver);
    if (r != VK_SUCCESS) { fprintf(stderr, "ICD interface negotiation failed: %s\n", res(r)); exit(2); }
    Dl_info di;
    printf("bootstrap: --icd %s (resolved %s), loader<->ICD interface version %u\n", path,
           dladdr((void *)gipa, &di) && di.dli_fname ? di.dli_fname : "?", ver);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- 1+2: semaphores and fences */
static int sem_sync_exp, sem_sync_imp, fence_sync_exp, fence_sync_imp;

static void query_semaphores(void) {
    printf("\n== 1. vkGetPhysicalDeviceExternalSemaphoreProperties ==\n");
    static const struct { VkExternalSemaphoreHandleTypeFlagBits t; const char *n; } hts[] = {
        { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" }, { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" } };
    static const struct { VkSemaphoreType t; const char *n; } sts[] = { { VK_SEMAPHORE_TYPE_BINARY, "binary" }, { VK_SEMAPHORE_TYPE_TIMELINE, "timeline" } };
    for (unsigned s = 0; s < 2; s++) for (unsigned h = 0; h < 2; h++) {
        VkSemaphoreTypeCreateInfo sti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = sts[s].t };
        VkPhysicalDeviceExternalSemaphoreInfo info = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .pNext = &sti, .handleType = hts[h].t };
        VkExternalSemaphoreProperties p = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
        p_vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &info, &p);
        printf("  %-8s %-9s features=", sts[s].n, hts[h].n); p_two_feat(p.externalSemaphoreFeatures);
        printf(" compatible="); p_sem_types(p.compatibleHandleTypes);
        printf(" exportFromImported="); p_sem_types(p.exportFromImportedHandleTypes); printf("\n");
        if (s == 0 && hts[h].t == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
            sem_sync_exp = !!(p.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
            sem_sync_imp = !!(p.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
        }
    }
}

static void query_fences(void) {
    printf("\n== 2. vkGetPhysicalDeviceExternalFenceProperties ==\n");
    static const struct { VkExternalFenceHandleTypeFlagBits t; const char *n; } hts[] = {
        { VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" }, { VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" } };
    for (unsigned h = 0; h < 2; h++) {
        VkPhysicalDeviceExternalFenceInfo info = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, .handleType = hts[h].t };
        VkExternalFenceProperties p = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES };
        p_vkGetPhysicalDeviceExternalFenceProperties(pd, &info, &p);
        printf("  %-9s features=", hts[h].n); p_two_feat(p.externalFenceFeatures);
        printf(" compatible="); p_fence_types(p.compatibleHandleTypes);
        printf(" exportFromImported="); p_fence_types(p.exportFromImportedHandleTypes); printf("\n");
        if (hts[h].t == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT) {
            fence_sync_exp = !!(p.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT);
            fence_sync_imp = !!(p.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT);
        }
    }
}

/* ---------------------------------------------------------------- 3: formats and modifiers */
/* Returns the VkResult; prints one line. Only SUCCESS and FORMAT_NOT_SUPPORTED are expected. */
static VkResult image_fmt_query(VkFormat f, VkImageTiling tiling, VkImageUsageFlags usage,
                                VkExternalMemoryHandleTypeFlagBits ht, int with_mod, uint64_t mod, const char *label) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = mod, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkPhysicalDeviceExternalImageFormatInfo ei = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = with_mod ? &mi : NULL, .handleType = ht };
    VkPhysicalDeviceImageFormatInfo2 fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &ei,
        .format = f, .type = VK_IMAGE_TYPE_2D, .tiling = tiling, .usage = usage };
    VkExternalImageFormatProperties ep = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 ip = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ep };
    VkResult r = p_vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &ip);
    printf("      %s -> %s", label, res(r));
    if (r == VK_SUCCESS) {
        const VkImageFormatProperties *p = &ip.imageFormatProperties;
        printf(" maxExtent=%ux%ux%u mips=%u layers=%u samples=0x%x ext.features=", p->maxExtent.width, p->maxExtent.height,
               p->maxExtent.depth, p->maxMipLevels, p->maxArrayLayers, p->sampleCounts);
        p_mem_feat(ep.externalMemoryProperties.externalMemoryFeatures);
        printf(" compatible="); p_mem_types(ep.externalMemoryProperties.compatibleHandleTypes);
        printf(" exportFromImported="); p_mem_types(ep.externalMemoryProperties.exportFromImportedHandleTypes);
    }
    printf("\n");
    if (r != VK_SUCCESS && r != VK_ERROR_FORMAT_NOT_SUPPORTED) UNEXPECTED("image format query returned %s\n", res(r));
    return r;
}

static void query_formats(void) {
    printf("\n== 3. DRM format modifiers + image format properties (type 2D, sharing EXCLUSIVE) ==\n");
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags depth_usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (unsigned i = 0; i < NFMT; i++) {
        VkImageUsageFlags usage = fmts[i].depth ? depth_usage : color_usage;
        const char *uname = fmts[i].depth ? "DEPTH_STENCIL_ATT|SAMPLED" : "COLOR_ATT|SAMPLED|XFER_SRC|XFER_DST";
        VkDrmFormatModifierPropertiesList2EXT ml = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT };
        VkFormatProperties3 f3 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, .pNext = &ml };
        VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &f3 };
        p_vkGetPhysicalDeviceFormatProperties2(pd, fmts[i].f, &fp);
        VkDrmFormatModifierProperties2EXT *mods = calloc(ml.drmFormatModifierCount ? ml.drmFormatModifierCount : 1, sizeof *mods);
        ml.pDrmFormatModifierProperties = mods;
        p_vkGetPhysicalDeviceFormatProperties2(pd, fmts[i].f, &fp);
        printf("\n  %s (VkFormat %d): %u modifiers; optimalTiling=", fmts[i].name, fmts[i].f, ml.drmFormatModifierCount);
        p_fmt_feat2(f3.optimalTilingFeatures); printf(" linearTiling="); p_fmt_feat2(f3.linearTilingFeatures); printf("\n");
        int has_linear = 0;
        for (uint32_t m = 0; m < ml.drmFormatModifierCount; m++) {
            uint64_t mod = mods[m].drmFormatModifier;
            if (mod == 0) has_linear = 1;
            printf("    mod[%u] ", m); p_modifier(mod);
            printf(" planes=%u features=", mods[m].drmFormatModifierPlaneCount);
            p_fmt_feat2(mods[m].drmFormatModifierTilingFeatures); printf("\n");
            char lbl[160];
            snprintf(lbl, sizeof lbl, "DRM_MOD tiling, DMA_BUF, usage=%s", uname);
            VkResult r = image_fmt_query(fmts[i].f, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, usage,
                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, 1, mod, lbl);
            if (r != VK_SUCCESS)
                image_fmt_query(fmts[i].f, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, 1, mod, "  retry usage=SAMPLED only");
        }
        printf("    LINEAR (0x0) in modifier list: %s\n", has_linear ? "YES" : "NO");
        char lbl[160];
        snprintf(lbl, sizeof lbl, "OPTIMAL tiling, DMA_BUF,   usage=%s", uname);
        image_fmt_query(fmts[i].f, VK_IMAGE_TILING_OPTIMAL, usage, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, 0, 0, lbl);
        snprintf(lbl, sizeof lbl, "OPTIMAL tiling, OPAQUE_FD, usage=%s", uname);
        image_fmt_query(fmts[i].f, VK_IMAGE_TILING_OPTIMAL, usage, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, 0, 0, lbl);
        snprintf(lbl, sizeof lbl, "LINEAR tiling,  DMA_BUF,   usage=%s", uname);
        image_fmt_query(fmts[i].f, VK_IMAGE_TILING_LINEAR, usage, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, 0, 0, lbl);
        if (fmts[i].f == VK_FORMAT_R8G8B8A8_UNORM) {
            rgba_nmods = ml.drmFormatModifierCount < 64 ? ml.drmFormatModifierCount : 64;
            memcpy(rgba_mods, mods, rgba_nmods * sizeof *mods);
        }
        free(mods);
    }
}

/* ---------------------------------------------------------------- fd inspection helpers */
static void describe_fd(const char *what, int fd) {
    char p[64], l[256] = "";
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, l, sizeof l - 1); if (n > 0) l[n] = 0;
    printf("    %s fd=%d -> %s\n", what, fd, n > 0 ? l : strerror(errno));
}

static void describe_sync_fd(const char *what, int fd) {
    describe_fd(what, fd);
    struct sync_file_info info; memset(&info, 0, sizeof info);
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0) { printf("      SYNC_IOC_FILE_INFO: errno=%d (%s) -> not a sync_file\n", errno, strerror(errno)); return; }
    printf("      sync_file name='%s' status=%d num_fences=%u\n", info.name, info.status, info.num_fences);
    if (info.num_fences) {
        struct sync_fence_info *fi = calloc(info.num_fences, sizeof *fi);
        uint32_t nf = info.num_fences;
        memset(&info, 0, sizeof info); info.num_fences = nf; info.sync_fence_info = (uint64_t)(uintptr_t)fi;
        if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) == 0)
            for (uint32_t i = 0; i < info.num_fences; i++)
                printf("      fence[%u] driver='%s' timeline='%s' status=%d\n", i, fi[i].driver_name, fi[i].obj_name, fi[i].status);
        else printf("      SYNC_IOC_FILE_INFO(fences): errno=%d (%s)\n", errno, strerror(errno));
        free(fi);
    }
    struct pollfd pf = { .fd = fd, .events = POLLIN };
    int pr = poll(&pf, 1, 0);
    printf("      poll(0ms)=%d revents=0x%x (%s)\n", pr, pf.revents, pr > 0 ? "signaled" : pr == 0 ? "not yet signaled" : strerror(errno));
}

/* ---------------------------------------------------------------- 4: dma-buf round trip */
static uint32_t pick_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int submit_and_wait(const char *what) {
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f; VkResult r = p_vkCreateFence(dev, &fci, NULL, &f);
    if (r) { UNEXPECTED("%s: vkCreateFence %s\n", what, res(r)); return 0; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    r = p_vkQueueSubmit(queue, 1, &si, f);
    printf("    %s: vkQueueSubmit -> %s", what, res(r));
    if (r == VK_SUCCESS) { r = p_vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull); printf(", vkWaitForFences -> %s", res(r)); }
    printf("\n");
    p_vkDestroyFence(dev, f, NULL);
    if (r) { UNEXPECTED("%s failed: %s\n", what, res(r)); return 0; }
    return 1;
}

static const VkImageSubresourceRange color_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

/* clear `img` (fresh, UNDEFINED) to rgba, leave it in GENERAL with writes available */
static int clear_image(VkImage img, const float rgba[4]) {
    p_vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    p_vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = color_range };
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue cc; memcpy(cc.float32, rgba, sizeof cc.float32);
    p_vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &color_range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    p_vkEndCommandBuffer(cmd);
    return submit_and_wait("clear exported image");
}

/* copy `img` (in layout `old`) to a host-visible buffer and check every texel equals px */
static int check_image(VkImage img, VkImageLayout old, const uint8_t px[4], uint32_t w, uint32_t h) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf; VkResult r = p_vkCreateBuffer(dev, &bci, NULL, &buf);
    if (r) { UNEXPECTED("vkCreateBuffer %s\n", res(r)); return 0; }
    VkMemoryRequirements mr; p_vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t mt = pick_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    VkDeviceMemory bm; r = p_vkAllocateMemory(dev, &mai, NULL, &bm);
    if (r) { UNEXPECTED("readback vkAllocateMemory %s\n", res(r)); p_vkDestroyBuffer(dev, buf, NULL); return 0; }
    p_vkBindBufferMemory(dev, buf, bm, 0);
    p_vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    p_vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = old, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = color_range };
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
    p_vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    p_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    p_vkEndCommandBuffer(cmd);
    int ok = submit_and_wait("copy imported image to buffer");
    if (ok) {
        uint8_t *p; r = p_vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&p);
        if (r) { UNEXPECTED("vkMapMemory %s\n", res(r)); ok = 0; }
        else {
            uint32_t bad = 0, first_bad = 0;
            for (uint32_t i = 0; i < w * h; i++) if (memcmp(p + 4 * i, px, 4)) { if (!bad) first_bad = i; bad++; }
            printf("    pixel check of imported image: %u/%u texels == {%u,%u,%u,%u}", w * h - bad, w * h, px[0], px[1], px[2], px[3]);
            if (bad) printf("; first mismatch texel %u = {%u,%u,%u,%u}", first_bad, p[4 * first_bad], p[4 * first_bad + 1], p[4 * first_bad + 2], p[4 * first_bad + 3]);
            printf("\n");
            if (bad) { UNEXPECTED("imported image does not show the exported image's contents\n"); ok = 0; }
            p_vkUnmapMemory(dev, bm);
        }
    }
    p_vkDestroyBuffer(dev, buf, NULL); p_vkFreeMemory(dev, bm, NULL);
    return ok;
}

/* CPU mmap of a dma-buf fd. Returns mapping or NULL; reports errno. */
static void *try_mmap(int fd, size_t *len_out) {
    off_t len = lseek(fd, 0, SEEK_END);
    int lerr = errno;
    lseek(fd, 0, SEEK_SET);
    printf("    dma-buf lseek(SEEK_END) size = %lld%s%s\n", (long long)len, len < 0 ? " errno " : "", len < 0 ? strerror(lerr) : "");
    size_t sz = len > 0 ? (size_t)len : 4096;
    errno = 0;
    void *m = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { int e = errno; printf("    mmap(PROT_READ, MAP_SHARED, len=%zu) -> MAP_FAILED errno=%d (%s)\n", sz, e, strerror(e)); return NULL; }
    printf("    mmap(PROT_READ, MAP_SHARED, len=%zu) -> OK %p\n", sz, m);
    *len_out = sz;
    return m;
}

struct exported { VkImage img; VkDeviceMemory mem; int fd; uint64_t mod; uint32_t planes; VkSubresourceLayout lay[4]; VkDeviceSize size; };

static uint32_t mod_planes(uint64_t mod) {
    for (uint32_t i = 0; i < rgba_nmods; i++) if (rgba_mods[i].drmFormatModifier == mod) return rgba_mods[i].drmFormatModifierPlaneCount;
    return 0;
}

static const VkImageUsageFlags rt_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

/* create + allocate + bind + export one R8G8B8A8_UNORM 256x256 image. 0 on failure (already reported).
 * mods == NULL: tiling OPTIMAL (no modifier, no layout), to measure dma-buf export from OPTIMAL. */
static int export_image(const uint64_t *mods, uint32_t nmods, VkMemoryPropertyFlags want_mem, struct exported *e) {
    const int opt = mods == NULL;
    memset(e, 0, sizeof *e); e->fd = -1;
    VkImageDrmFormatModifierListCreateInfoEXT ml = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = nmods, .pDrmFormatModifiers = mods };
    VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = opt ? NULL : &ml,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 256, 256, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = opt ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = rt_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkResult r = p_vkCreateImage(dev, &ici, NULL, &e->img);
    if (opt) printf("    vkCreateImage(tiling OPTIMAL, ext DMA_BUF) -> %s\n", res(r));
    else printf("    vkCreateImage(DRM_MOD list of %u, ext DMA_BUF) -> %s\n", nmods, res(r));
    if (r) { UNEXPECTED("export image creation failed\n"); return 0; }

    if (!opt) {
    VkImageDrmFormatModifierPropertiesEXT mp = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    r = p_vkGetImageDrmFormatModifierPropertiesEXT(dev, e->img, &mp);
    printf("    vkGetImageDrmFormatModifierPropertiesEXT -> %s chosen modifier ", res(r)); p_modifier(mp.drmFormatModifier); printf("\n");
    if (r) { UNEXPECTED("modifier query failed\n"); return 0; }
    e->mod = mp.drmFormatModifier;
    e->planes = mod_planes(e->mod);
    if (!e->planes) { UNEXPECTED("chosen modifier not in the format's reported modifier list\n"); return 0; }
    }

    VkMemoryDedicatedRequirements dr = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr };
    VkImageMemoryRequirementsInfo2 mri = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = e->img };
    p_vkGetImageMemoryRequirements2(dev, &mri, &mr);
    printf("    memreqs: size=%llu align=%llu memoryTypeBits=0x%x prefersDedicated=%u requiresDedicated=%u\n",
           (unsigned long long)mr.memoryRequirements.size, (unsigned long long)mr.memoryRequirements.alignment,
           mr.memoryRequirements.memoryTypeBits, dr.prefersDedicatedAllocation, dr.requiresDedicatedAllocation);
    uint32_t mt = pick_mem(mr.memoryRequirements.memoryTypeBits, want_mem);
    if (mt == UINT32_MAX) {
        printf("    no memory type with "); p_memprop(want_mem); printf(" in memoryTypeBits 0x%x (expected negative, skipping)\n", mr.memoryRequirements.memoryTypeBits);
        p_vkDestroyImage(dev, e->img, NULL); e->img = VK_NULL_HANDLE;
        return -1;
    }
    printf("    memory type %u heap %u flags=", mt, memprops.memoryTypes[mt].heapIndex); p_memprop(memprops.memoryTypes[mt].propertyFlags); printf("\n");
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = e->img };
    VkExportMemoryAllocateInfo exp = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exp,
        .allocationSize = mr.memoryRequirements.size, .memoryTypeIndex = mt };
    r = p_vkAllocateMemory(dev, &mai, NULL, &e->mem);
    printf("    vkAllocateMemory(export DMA_BUF, dedicated) -> %s\n", res(r));
    if (r) { UNEXPECTED("export allocation failed\n"); return 0; }
    e->size = mr.memoryRequirements.size;
    r = p_vkBindImageMemory(dev, e->img, e->mem, 0);
    printf("    vkBindImageMemory -> %s\n", res(r));
    if (r) { UNEXPECTED("bind failed\n"); return 0; }
    VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = e->mem,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    r = p_vkGetMemoryFdKHR(dev, &gfi, &e->fd);
    printf("    vkGetMemoryFdKHR(DMA_BUF) -> %s fd=%d\n", res(r), e->fd);
    if (r) { UNEXPECTED("dma-buf export failed\n"); return 0; }
    describe_fd("exported dma-buf", e->fd);
    for (uint32_t p = 0; p < e->planes && p < 4; p++) {
        VkImageSubresource sr = { .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << p };
        p_vkGetImageSubresourceLayout(dev, e->img, &sr, &e->lay[p]);
        printf("    plane %u layout (MEMORY_PLANE_%u): offset=%llu rowPitch=%llu size=%llu arrayPitch=%llu depthPitch=%llu\n", p, p,
               (unsigned long long)e->lay[p].offset, (unsigned long long)e->lay[p].rowPitch, (unsigned long long)e->lay[p].size,
               (unsigned long long)e->lay[p].arrayPitch, (unsigned long long)e->lay[p].depthPitch);
    }
    return 1;
}

static void destroy_exported(struct exported *e) {
    if (e->fd >= 0) close(e->fd);
    if (e->img) p_vkDestroyImage(dev, e->img, NULL);
    if (e->mem) p_vkFreeMemory(dev, e->mem, NULL);
    memset(e, 0, sizeof *e); e->fd = -1;
}

/* import e->fd (dup) into a second image with the explicit modifier + layout */
static int import_image(struct exported *e, VkImage *out_img, VkDeviceMemory *out_mem) {
    VkSubresourceLayout pl[4]; memset(pl, 0, sizeof pl);
    for (uint32_t p = 0; p < e->planes && p < 4; p++) { pl[p].offset = e->lay[p].offset; pl[p].rowPitch = e->lay[p].rowPitch; } /* size/array/depth must be 0 */
    VkImageDrmFormatModifierExplicitCreateInfoEXT xi = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = e->mod, .drmFormatModifierPlaneCount = e->planes, .pPlaneLayouts = pl };
    const int opt = e->planes == 0;
    VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = opt ? NULL : &xi,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 256, 256, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = opt ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, .usage = rt_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED };
    VkImage img; VkResult r = p_vkCreateImage(dev, &ici, NULL, &img);
    if (opt) printf("    import: vkCreateImage(tiling OPTIMAL, ext DMA_BUF, initialLayout PREINITIALIZED) -> %s\n", res(r));
    else { printf("    import: vkCreateImage(DRM_MOD explicit ");
           p_modifier(e->mod); printf(", %u plane(s), initialLayout PREINITIALIZED) -> %s\n", e->planes, res(r)); }
    if (r) { UNEXPECTED("import image creation failed\n"); return 0; }
    if (!opt) {
        VkImageDrmFormatModifierPropertiesEXT mp = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
        r = p_vkGetImageDrmFormatModifierPropertiesEXT(dev, img, &mp);
        printf("    import: vkGetImageDrmFormatModifierPropertiesEXT -> %s modifier ", res(r)); p_modifier(mp.drmFormatModifier); printf("\n");
        if (r || mp.drmFormatModifier != e->mod) UNEXPECTED("imported image does not report the explicit modifier\n");
    }

    int fd = dup(e->fd);
    if (fd < 0) { UNEXPECTED("dup: %s\n", strerror(errno)); p_vkDestroyImage(dev, img, NULL); return 0; }
    VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    r = p_vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
    printf("    import: vkGetMemoryFdPropertiesKHR(DMA_BUF, fd %d) -> %s memoryTypeBits=0x%x\n", fd, res(r), fp.memoryTypeBits);
    if (r) UNEXPECTED("vkGetMemoryFdPropertiesKHR failed\n");
    VkMemoryDedicatedRequirements dr = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr };
    VkImageMemoryRequirementsInfo2 mri = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img };
    p_vkGetImageMemoryRequirements2(dev, &mri, &mr);
    printf("    import: memreqs size=%llu memoryTypeBits=0x%x prefersDedicated=%u requiresDedicated=%u\n",
           (unsigned long long)mr.memoryRequirements.size, mr.memoryRequirements.memoryTypeBits,
           dr.prefersDedicatedAllocation, dr.requiresDedicatedAllocation);
    uint32_t bits = mr.memoryRequirements.memoryTypeBits & (r == VK_SUCCESS ? fp.memoryTypeBits : ~0u);
    uint32_t mt = pick_mem(bits, 0);
    if (mt == UINT32_MAX) { UNEXPECTED("no memory type in image bits & fd bits (0x%x)\n", bits); close(fd); p_vkDestroyImage(dev, img, NULL); return 0; }
    VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img };
    VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
        .allocationSize = mr.memoryRequirements.size, .memoryTypeIndex = mt };
    VkDeviceMemory mem; r = p_vkAllocateMemory(dev, &mai, NULL, &mem);
    printf("    import: vkAllocateMemory(VkImportMemoryFdInfoKHR DMA_BUF + dedicated, type %u) -> %s\n", mt, res(r));
    if (r) { UNEXPECTED("dma-buf import failed\n"); close(fd); p_vkDestroyImage(dev, img, NULL); return 0; }
    r = p_vkBindImageMemory(dev, img, mem, 0);
    printf("    import: vkBindImageMemory -> %s\n", res(r));
    if (r) { UNEXPECTED("bind of imported memory failed\n"); p_vkFreeMemory(dev, mem, NULL); p_vkDestroyImage(dev, img, NULL); return 0; }
    *out_img = img; *out_mem = mem;
    return 1;
}

static void dmabuf_roundtrip(void) {
    printf("\n== 4. dma-buf export/import round trip (R8G8B8A8_UNORM 256x256, usage COLOR_ATT|SAMPLED|XFER_SRC|XFER_DST) ==\n");
    if (!rgba_nmods) { UNEXPECTED("R8G8B8A8_UNORM reported no DRM format modifiers; round trip impossible\n"); return; }
    uint64_t mods[64];
    for (uint32_t i = 0; i < rgba_nmods; i++) mods[i] = rgba_mods[i].drmFormatModifier;
    const float rgba[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
    const uint8_t px[4] = { 51, 102, 153, 255 };

    printf("  4a. modifier list = all %u reported modifiers, DEVICE_LOCAL memory\n", rgba_nmods);
    struct exported e;
    if (export_image(mods, rgba_nmods, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &e) == 1) {
        size_t ml = 0; uint8_t *m = try_mmap(e.fd, &ml);
        VkImage bi; VkDeviceMemory bm;
        if (import_image(&e, &bi, &bm)) {
            if (clear_image(e.img, rgba)) {
                check_image(bi, VK_IMAGE_LAYOUT_PREINITIALIZED, px, 256, 256);
                if (m) { /* uniform clear: every 4-byte group must match whatever the tiling (modifier has c=0, no compression) */
                    struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
                    int sr = ioctl(e.fd, DMA_BUF_IOCTL_SYNC, &s); int se = errno;
                    printf("    DMA_BUF_IOCTL_SYNC(START|READ) -> %d%s%s\n", sr, sr ? " errno " : "", sr ? strerror(se) : "");
                    size_t good = 0, groups = ml / 4;
                    for (size_t i = 0; i < groups; i++) if (!memcmp(m + 4 * i, px, 4)) good++;
                    printf("    CPU view through block-linear dma-buf mmap: %zu/%zu 4-byte groups == {51,102,153,255} (first bytes {%u,%u,%u,%u})\n",
                           good, groups, m[0], m[1], m[2], m[3]);
                    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(e.fd, DMA_BUF_IOCTL_SYNC, &s);
                }
            }
            p_vkDestroyImage(dev, bi, NULL); p_vkFreeMemory(dev, bm, NULL);
        }
        if (m) munmap(m, ml);
    }
    destroy_exported(&e);

    printf("  4b. tiling OPTIMAL + VkExternalMemoryImageCreateInfo DMA_BUF, DEVICE_LOCAL memory (no modifier, same-driver import)\n");
    if (export_image(NULL, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &e) == 1) {
        VkImage bi; VkDeviceMemory bm;
        if (import_image(&e, &bi, &bm)) {
            if (clear_image(e.img, rgba)) check_image(bi, VK_IMAGE_LAYOUT_PREINITIALIZED, px, 256, 256);
            p_vkDestroyImage(dev, bi, NULL); p_vkFreeMemory(dev, bm, NULL);
        }
    }
    destroy_exported(&e);

    static const struct { VkMemoryPropertyFlags f; const char *n; } mts[] = {
        { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL" }, { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE" } };
    const uint64_t lin = 0;
    for (unsigned k = 0; k < 2; k++) {
        printf("  4%c. modifier list = { LINEAR } only, %s memory\n", 'c' + k, mts[k].n);
        if (!mod_planes(0)) { printf("    LINEAR not in R8G8B8A8_UNORM modifier list; skipped (expected negative)\n"); continue; }
        int rr = export_image(&lin, 1, mts[k].f, &e);
        if (rr == 1) {
            size_t ml = 0; uint8_t *m = try_mmap(e.fd, &ml);
            if (clear_image(e.img, rgba)) {
                if (m) {
                    struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
                    int sr = ioctl(e.fd, DMA_BUF_IOCTL_SYNC, &s); int se = errno;
                    printf("    DMA_BUF_IOCTL_SYNC(START|READ) -> %d%s%s\n", sr, sr ? " errno " : "", sr ? strerror(se) : "");
                    uint32_t bad = 0;
                    for (uint32_t y = 0; y < 256; y++) for (uint32_t x = 0; x < 256; x++) {
                        size_t off = e.lay[0].offset + y * e.lay[0].rowPitch + x * 4;
                        if (off + 4 > ml || memcmp(m + off, px, 4)) bad++;
                    }
                    printf("    CPU view through dma-buf mmap: %u/65536 texels == {51,102,153,255} (first texel {%u,%u,%u,%u})\n",
                           65536 - bad, m[e.lay[0].offset], m[e.lay[0].offset + 1], m[e.lay[0].offset + 2], m[e.lay[0].offset + 3]);
                    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(e.fd, DMA_BUF_IOCTL_SYNC, &s);
                    if (bad) UNEXPECTED("dma-buf mmap succeeded but does not show the GPU-written texels\n");
                }
                VkImage bi; VkDeviceMemory bm;
                if (import_image(&e, &bi, &bm)) {
                    check_image(bi, VK_IMAGE_LAYOUT_PREINITIALIZED, px, 256, 256);
                    p_vkDestroyImage(dev, bi, NULL); p_vkFreeMemory(dev, bm, NULL);
                }
            }
            if (m) munmap(m, ml);
        }
        destroy_exported(&e);
    }
}

/* ---------------------------------------------------------------- 5: sync_fd round trips */
/* A command buffer that keeps the GPU busy for tens of ms (repeated clears of a 4096^2 RGBA16F image),
 * so that a signal operation is still pending when its sync_fd is exported. */
static VkCommandBuffer busy_cmd;
static VkImage busy_img;
static VkDeviceMemory busy_mem;
static int make_busy(void) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = { 4096, 4096, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkResult r = p_vkCreateImage(dev, &ici, NULL, &busy_img);
    if (r) { UNEXPECTED("busy image: vkCreateImage %s\n", res(r)); return 0; }
    VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkImageMemoryRequirementsInfo2 mri = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = busy_img };
    p_vkGetImageMemoryRequirements2(dev, &mri, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.memoryRequirements.size,
        .memoryTypeIndex = pick_mem(mr.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if ((r = p_vkAllocateMemory(dev, &mai, NULL, &busy_mem))) { UNEXPECTED("busy image: vkAllocateMemory %s\n", res(r)); return 0; }
    p_vkBindImageMemory(dev, busy_img, busy_mem, 0);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if ((r = p_vkAllocateCommandBuffers(dev, &cai, &busy_cmd))) { UNEXPECTED("busy: vkAllocateCommandBuffers %s\n", res(r)); return 0; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    p_vkBeginCommandBuffer(busy_cmd, &bi);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = busy_img, .subresourceRange = color_range };
    p_vkCmdPipelineBarrier(busy_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    for (int i = 0; i < 400; i++) {
        VkClearColorValue cc = { .float32 = { (float)i / 400.0f, 0.5f, 0.25f, 1.0f } };
        p_vkCmdClearColorImage(busy_cmd, busy_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &color_range);
        p_vkCmdPipelineBarrier(busy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    p_vkEndCommandBuffer(busy_cmd);
    return 1;
}
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int fd_signaled(int fd) { struct pollfd pf = { .fd = fd, .events = POLLIN }; return poll(&pf, 1, 0) > 0; }
static VkSemaphore make_sem(int exportable) {
    VkExportSemaphoreCreateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = exportable ? &ex : NULL };
    VkSemaphore s; VkResult r = p_vkCreateSemaphore(dev, &ci, NULL, &s);
    if (r) { UNEXPECTED("vkCreateSemaphore(%s) %s\n", exportable ? "export SYNC_FD" : "plain", res(r)); return VK_NULL_HANDLE; }
    return s;
}
static VkFence make_fence(int exportable) {
    VkExportFenceCreateInfo ex = { .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT };
    VkFenceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = exportable ? &ex : NULL };
    VkFence f; VkResult r = p_vkCreateFence(dev, &ci, NULL, &f);
    if (r) { UNEXPECTED("vkCreateFence(%s) %s\n", exportable ? "export SYNC_FD" : "plain", res(r)); return VK_NULL_HANDLE; }
    return f;
}
static VkResult submit_sem(VkSemaphore wait, VkSemaphore signal, VkFence f) {
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait ? 1 : 0, .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &st, .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal };
    return p_vkQueueSubmit(queue, 1, &si, f);
}
static VkResult sem_get_fd(VkSemaphore s, int *fd) {
    VkSemaphoreGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = s, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    *fd = -2; return p_vkGetSemaphoreFdKHR(dev, &gi, fd);
}
static VkResult sem_import(VkSemaphore s, int fd) {
    VkImportSemaphoreFdInfoKHR ii = { .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = s,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fd };
    return p_vkImportSemaphoreFdKHR(dev, &ii);
}
static VkResult fence_get_fd(VkFence f, int *fd) {
    VkFenceGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = f, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT };
    *fd = -2; return p_vkGetFenceFdKHR(dev, &gi, fd);
}
static VkResult fence_import(VkFence f, int fd) {
    VkImportFenceFdInfoKHR ii = { .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, .fence = f,
        .flags = VK_FENCE_IMPORT_TEMPORARY_BIT, .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, .fd = fd };
    return p_vkImportFenceFdKHR(dev, &ii);
}
#define EXPECT_OK(r, ...) do { if ((r) != VK_SUCCESS) UNEXPECTED(__VA_ARGS__); } while (0)

/* import fd into a plain semaphore and prove a queue wait on it completes */
static void sem_import_and_wait(const char *what, int fd) {
    VkSemaphore s = make_sem(0); VkFence f = make_fence(0);
    if (!s || !f) return;
    VkResult r = sem_import(s, fd);
    printf("    %s: vkImportSemaphoreFdKHR(TEMPORARY, SYNC_FD, fd=%d) -> %s\n", what, fd, res(r));
    if (r == VK_SUCCESS) {
        VkResult r2 = submit_sem(s, VK_NULL_HANDLE, f);
        VkResult r3 = r2 ? r2 : p_vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull);
        printf("    %s: vkQueueSubmit(wait on imported semaphore) -> %s, vkWaitForFences -> %s\n", what, res(r2), res(r3));
        EXPECT_OK(r3, "%s: wait on imported semaphore did not complete\n", what);
    } else {
        UNEXPECTED("%s: semaphore SYNC_FD import rejected (%s)\n", what, res(r));
        if (fd >= 0) close(fd);
    }
    p_vkQueueWaitIdle(queue);
    p_vkDestroySemaphore(dev, s, NULL); p_vkDestroyFence(dev, f, NULL);
}

static void sync_roundtrip(void) {
    printf("\n== 5. sync_fd round trips ==\n");
    VkResult r; int fd;
    printf("  5a. binary semaphore, VkExportSemaphoreCreateInfo SYNC_FD (advertised: export=%s import=%s)\n",
           sem_sync_exp ? "yes" : "no", sem_sync_imp ? "yes" : "no");
    if (!sem_sync_exp || !sem_sync_imp) printf("    not advertised; skipped (expected negative)\n");
    else {
        VkSemaphore a = make_sem(1);
        if (a) {
            r = submit_sem(VK_NULL_HANDLE, a, VK_NULL_HANDLE);
            printf("    empty vkQueueSubmit(signal) -> %s\n", res(r)); EXPECT_OK(r, "signal submit failed\n");
            r = sem_get_fd(a, &fd);
            printf("    vkGetSemaphoreFdKHR(SYNC_FD) right after submit -> %s fd=%d\n", res(r), fd);
            EXPECT_OK(r, "semaphore SYNC_FD export failed\n");
            if (r == VK_SUCCESS) {
                if (fd >= 0) describe_sync_fd("exported semaphore sync_fd", fd);
                sem_import_and_wait("import exported fd", fd);
            }
            p_vkQueueWaitIdle(queue);
            p_vkDestroySemaphore(dev, a, NULL);
        }
        printf("  5b. SYNC_FD export of a semaphore whose signal already completed (vkQueueWaitIdle before export)\n");
        VkSemaphore c = make_sem(1);
        if (c) {
            r = submit_sem(VK_NULL_HANDLE, c, VK_NULL_HANDLE);
            printf("    empty vkQueueSubmit(signal) -> %s\n", res(r)); EXPECT_OK(r, "signal submit failed\n");
            r = p_vkQueueWaitIdle(queue); printf("    vkQueueWaitIdle -> %s\n", res(r));
            r = sem_get_fd(c, &fd);
            printf("    vkGetSemaphoreFdKHR(SYNC_FD) after completion -> %s fd=%d%s\n", res(r), fd,
                   r == VK_SUCCESS && fd == -1 ? " (the -1 'already signaled' value)" : "");
            EXPECT_OK(r, "semaphore SYNC_FD export after completion failed\n");
            if (r == VK_SUCCESS && fd >= 0) { describe_sync_fd("exported (completed) semaphore sync_fd", fd); close(fd); }
            p_vkDestroySemaphore(dev, c, NULL);
        }
        printf("  5c. import fd -1 into a semaphore (Android loader 'already signaled' convention; spec-valid for SYNC_FD)\n");
        sem_import_and_wait("import -1", -1);
    }

    printf("  5d. fence, VkExportFenceCreateInfo SYNC_FD (advertised: export=%s import=%s)\n",
           fence_sync_exp ? "yes" : "no", fence_sync_imp ? "yes" : "no");
    if (!fence_sync_exp || !fence_sync_imp) { printf("    not advertised; skipped (expected negative)\n"); return; }
    VkFence fa = make_fence(1), fb = make_fence(0);
    if (fa && fb) {
        r = p_vkQueueSubmit(queue, 0, NULL, fa);
        printf("    vkQueueSubmit(0 submits, fence) -> %s\n", res(r)); EXPECT_OK(r, "fence submit failed\n");
        r = fence_get_fd(fa, &fd);
        printf("    vkGetFenceFdKHR(SYNC_FD) right after submit -> %s fd=%d\n", res(r), fd);
        EXPECT_OK(r, "fence SYNC_FD export failed\n");
        printf("    source fence vkGetFenceStatus after export -> %s (copy transference: export acts as a reset)\n", res(p_vkGetFenceStatus(dev, fa)));
        if (r == VK_SUCCESS) {
            if (fd >= 0) describe_sync_fd("exported fence sync_fd", fd);
            r = fence_import(fb, fd);
            printf("    vkImportFenceFdKHR(TEMPORARY, SYNC_FD, fd=%d) -> %s\n", fd, res(r));
            EXPECT_OK(r, "fence SYNC_FD import failed\n");
            if (r == VK_SUCCESS) {
                r = p_vkWaitForFences(dev, 1, &fb, VK_TRUE, 5000000000ull);
                printf("    vkWaitForFences(imported fence) -> %s\n", res(r)); EXPECT_OK(r, "wait on imported fence failed\n");
            } else if (fd >= 0) close(fd);
        }
    }
    p_vkQueueWaitIdle(queue);
    if (fa) p_vkDestroyFence(dev, fa, NULL);
    if (fb) p_vkDestroyFence(dev, fb, NULL);

    printf("  5e. SYNC_FD export of a fence whose signal already completed (vkWaitForFences before export)\n");
    VkFence fc = make_fence(1);
    if (fc) {
        r = p_vkQueueSubmit(queue, 0, NULL, fc); printf("    vkQueueSubmit(0 submits, fence) -> %s\n", res(r));
        r = p_vkWaitForFences(dev, 1, &fc, VK_TRUE, 5000000000ull); printf("    vkWaitForFences -> %s\n", res(r));
        r = fence_get_fd(fc, &fd);
        printf("    vkGetFenceFdKHR(SYNC_FD) after completion -> %s fd=%d%s\n", res(r), fd,
               r == VK_SUCCESS && fd == -1 ? " (the -1 'already signaled' value)" : "");
        EXPECT_OK(r, "fence SYNC_FD export after completion failed\n");
        if (r == VK_SUCCESS && fd >= 0) { describe_sync_fd("exported (completed) fence sync_fd", fd); close(fd); }
        p_vkDestroyFence(dev, fc, NULL);
    }
    printf("  5f. import fd -1 into a fence (spec-valid for SYNC_FD)\n");
    VkFence fd_ = make_fence(0);
    if (fd_) {
        r = fence_import(fd_, -1);
        printf("    vkImportFenceFdKHR(TEMPORARY, SYNC_FD, fd=-1) -> %s\n", res(r));
        EXPECT_OK(r, "fence SYNC_FD import of -1 rejected\n");
        if (r == VK_SUCCESS) {
            printf("    vkGetFenceStatus(imported -1) -> %s\n", res(p_vkGetFenceStatus(dev, fd_)));
            r = p_vkWaitForFences(dev, 1, &fd_, VK_TRUE, 1000000000ull);
            printf("    vkWaitForFences(imported -1) -> %s\n", res(r)); EXPECT_OK(r, "wait on fence imported from -1 failed\n");
        }
        p_vkDestroyFence(dev, fd_, NULL);
    }

    printf("  5g. semaphore SYNC_FD export while the signal is genuinely pending (busy GPU work, 400 clears of 4096^2 RGBA16F)\n");
    if (!make_busy()) return;
    {
        VkSemaphore a = make_sem(1), b = make_sem(0); VkFence f = make_fence(0);
        if (a && b && f) {
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy_cmd,
                .signalSemaphoreCount = 1, .pSignalSemaphores = &a };
            double t0 = now_ms();
            r = p_vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            printf("    vkQueueSubmit(busy work, signal) -> %s\n", res(r)); EXPECT_OK(r, "busy submit failed\n");
            r = sem_get_fd(a, &fd);
            printf("    vkGetSemaphoreFdKHR(SYNC_FD) while pending -> %s fd=%d (t=%.2f ms)\n", res(r), fd, now_ms() - t0);
            EXPECT_OK(r, "pending semaphore SYNC_FD export failed\n");
            if (r == VK_SUCCESS && fd >= 0) {
                describe_sync_fd("pending semaphore sync_fd", fd);
                int keep = dup(fd);
                r = sem_import(b, fd);
                printf("    vkImportSemaphoreFdKHR(TEMPORARY, SYNC_FD) -> %s\n", res(r)); EXPECT_OK(r, "pending semaphore import failed\n");
                if (r == VK_SUCCESS) {
                    r = submit_sem(b, VK_NULL_HANDLE, f);
                    printf("    vkQueueSubmit(wait on imported semaphore, fence) -> %s\n", res(r));
                    VkResult st = p_vkGetFenceStatus(dev, f); int sig = fd_signaled(keep);
                    printf("    immediately: waiter fence %s, sync_fd %s (t=%.2f ms)\n", res(st), sig ? "signaled" : "not signaled", now_ms() - t0);
                    if (st == VK_SUCCESS && !sig) UNEXPECTED("waiter completed before the imported sync_fd signaled\n");
                    r = p_vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull);
                    printf("    vkWaitForFences(waiter) -> %s (t=%.2f ms), sync_fd now %s\n", res(r), now_ms() - t0, fd_signaled(keep) ? "signaled" : "NOT signaled");
                    EXPECT_OK(r, "waiter did not complete\n");
                    if (r == VK_SUCCESS && !fd_signaled(keep)) UNEXPECTED("waiter done but sync_fd unsignaled\n");
                } else close(fd);
                if (keep >= 0) close(keep);
            }
        }
        p_vkQueueWaitIdle(queue);
        if (a) p_vkDestroySemaphore(dev, a, NULL);
        if (b) p_vkDestroySemaphore(dev, b, NULL);
        if (f) p_vkDestroyFence(dev, f, NULL);
    }

    printf("  5h. fence SYNC_FD export while the signal is genuinely pending (same busy work)\n");
    {
        VkFence a = make_fence(1), b = make_fence(0);
        if (a && b) {
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &busy_cmd };
            double t0 = now_ms();
            r = p_vkQueueSubmit(queue, 1, &si, a);
            printf("    vkQueueSubmit(busy work, fence) -> %s\n", res(r)); EXPECT_OK(r, "busy submit failed\n");
            r = fence_get_fd(a, &fd);
            printf("    vkGetFenceFdKHR(SYNC_FD) while pending -> %s fd=%d (t=%.2f ms)\n", res(r), fd, now_ms() - t0);
            EXPECT_OK(r, "pending fence SYNC_FD export failed\n");
            if (r == VK_SUCCESS && fd >= 0) {
                describe_sync_fd("pending fence sync_fd", fd);
                int keep = dup(fd);
                r = fence_import(b, fd);
                printf("    vkImportFenceFdKHR(TEMPORARY, SYNC_FD) -> %s\n", res(r)); EXPECT_OK(r, "pending fence import failed\n");
                if (r == VK_SUCCESS) {
                    VkResult st = p_vkGetFenceStatus(dev, b); int sig = fd_signaled(keep);
                    printf("    immediately: imported fence %s, sync_fd %s (t=%.2f ms)\n", res(st), sig ? "signaled" : "not signaled", now_ms() - t0);
                    if (st == VK_SUCCESS && !sig) UNEXPECTED("imported fence signaled before its sync_fd\n");
                    r = p_vkWaitForFences(dev, 1, &b, VK_TRUE, 5000000000ull);
                    printf("    vkWaitForFences(imported fence) -> %s (t=%.2f ms), sync_fd now %s\n", res(r), now_ms() - t0, fd_signaled(keep) ? "signaled" : "NOT signaled");
                    EXPECT_OK(r, "imported fence wait failed\n");
                } else close(fd);
                if (keep >= 0) close(keep);
            }
        }
        p_vkQueueWaitIdle(queue);
        if (a) p_vkDestroyFence(dev, a, NULL);
        if (b) p_vkDestroyFence(dev, b, NULL);
    }
    p_vkDestroyImage(dev, busy_img, NULL); p_vkFreeMemory(dev, busy_mem, NULL);
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv) {
    const char *icd = NULL;
    int all_exts = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
        else if (!strcmp(argv[i], "--all-exts")) all_exts = 1;
        else { fprintf(stderr, "usage: %s [--icd PATH] [--all-exts]\n", argv[0]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (icd) bootstrap_icd(icd);
    else { gipa = (PFN_vk_icdGetInstanceProcAddr)vkGetInstanceProcAddr; printf("bootstrap: Khronos loader (libvulkan, -lvulkan)\n"); }
    GLOBAL_FNS(LOAD_G)

    static const char *const want_iext[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME };
    uint32_t nie = 0; VkResult r = p_vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof *ie);
    if (!r) r = p_vkEnumerateInstanceExtensionProperties(NULL, &nie, ie);
    if (r) { fprintf(stderr, "vkEnumerateInstanceExtensionProperties: %s\n", res(r)); return 2; }
    printf("instance extensions: %u\n", nie);
    if (all_exts) for (uint32_t i = 0; i < nie; i++) printf("  iext: %s %u\n", ie[i].extensionName, ie[i].specVersion);
    for (unsigned i = 0; i < 4; i++) if (!has_ext(ie, nie, want_iext[i])) { fprintf(stderr, "instance extension %s missing\n", want_iext[i]); return 2; }

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkcaps", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai,
        .enabledExtensionCount = 4, .ppEnabledExtensionNames = want_iext };
    r = p_vkCreateInstance(&ici, NULL, &inst);
    printf("vkCreateInstance(apiVersion 1.3, 4 exts) -> %s\n", res(r));
    if (r) return 2;
    INST_FNS(LOAD_I)

    uint32_t n = 0; r = p_vkEnumeratePhysicalDevices(inst, &n, NULL);
    if (r || !n) { fprintf(stderr, "vkEnumeratePhysicalDevices: %s, %u devices\n", res(r), n); return 2; }
    VkPhysicalDevice *pds = calloc(n, sizeof *pds); p_vkEnumeratePhysicalDevices(inst, &n, pds);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; p_vkGetPhysicalDeviceProperties(pds[i], &p);
        printf("device %u: %s vendor=0x%04x device=0x%04x type=%d api=%u.%u.%u\n", i, p.deviceName, p.vendorID, p.deviceID,
               p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (!pd && p.vendorID == 0x10de) pd = pds[i];
    }
    if (!pd) { fprintf(stderr, "no device with vendorID 0x10de\n"); return 2; }

    VkPhysicalDeviceDrmPropertiesEXT drm = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, .pNext = &drm };
    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &hostp };
    VkPhysicalDeviceIDProperties idp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .pNext = &drv };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &idp };
    uint32_t nde = 0; p_vkEnumerateDeviceExtensionProperties(pd, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de);
    r = p_vkEnumerateDeviceExtensionProperties(pd, NULL, &nde, de);
    if (r) { fprintf(stderr, "vkEnumerateDeviceExtensionProperties: %s\n", res(r)); return 2; }
    int have_drm = has_ext(de, nde, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME), have_host = has_ext(de, nde, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    hostp.pNext = have_drm ? (void *)&drm : NULL;
    drv.pNext = have_host ? (void *)&hostp : (have_drm ? (void *)&drm : NULL);
    p_vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("selected: %s (vendorID 0x10de) driverName='%s' driverInfo='%s' conformance %u.%u.%u.%u\n",
           p2.properties.deviceName, drv.driverName, drv.driverInfo, drv.conformanceVersion.major, drv.conformanceVersion.minor,
           drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
    printf("  deviceUUID="); for (unsigned i = 0; i < VK_UUID_SIZE; i++) printf("%02x", idp.deviceUUID[i]);
    printf(" driverUUID="); for (unsigned i = 0; i < VK_UUID_SIZE; i++) printf("%02x", idp.driverUUID[i]); printf("\n");
    if (have_drm) printf("  VK_EXT_physical_device_drm: hasPrimary=%u primary=%lld:%lld hasRender=%u render=%lld:%lld\n",
                         drm.hasPrimary, (long long)drm.primaryMajor, (long long)drm.primaryMinor, drm.hasRender,
                         (long long)drm.renderMajor, (long long)drm.renderMinor);
    if (have_host) printf("  minImportedHostPointerAlignment=%llu\n", (unsigned long long)hostp.minImportedHostPointerAlignment);

    printf("device extensions: %u\n", nde);
    static const char *const interesting[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list", "VK_KHR_format_feature_flags2", "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence", "VK_KHR_external_fence_fd", "VK_KHR_timeline_semaphore",
        "VK_EXT_queue_family_foreign", "VK_EXT_external_memory_host", "VK_EXT_external_memory_acquire_unmodified",
        "VK_EXT_physical_device_drm", "VK_KHR_dedicated_allocation", "VK_KHR_bind_memory2", "VK_KHR_sampler_ycbcr_conversion",
        "VK_KHR_swapchain", "VK_ANDROID_native_buffer", "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_ANDROID_external_format_resolve", "VK_KHR_maintenance1" };
    for (unsigned i = 0; i < sizeof interesting / sizeof interesting[0]; i++)
        printf("  %-52s %s\n", interesting[i], has_ext(de, nde, interesting[i]) ? "yes" : "NO");
    if (all_exts) for (uint32_t i = 0; i < nde; i++) printf("  ext: %s %u\n", de[i].extensionName, de[i].specVersion);

    static const char *const req_dext[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd", "VK_EXT_queue_family_foreign" };
    const unsigned nreq = sizeof req_dext / sizeof req_dext[0];
    for (unsigned i = 0; i < nreq; i++) if (!has_ext(de, nde, req_dext[i])) { fprintf(stderr, "device extension %s missing\n", req_dext[i]); return 2; }
    have_dev_ext_fmt2 = VK_API_VERSION_MINOR(p2.properties.apiVersion) >= 3 || has_ext(de, nde, "VK_KHR_format_feature_flags2");
    if (!have_dev_ext_fmt2) { fprintf(stderr, "VkDrmFormatModifierPropertiesList2EXT needs Vulkan 1.3 or VK_KHR_format_feature_flags2\n"); return 2; }

    p_vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    printf("memory types:\n");
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
        printf("  type %u heap %u ", i, memprops.memoryTypes[i].heapIndex); p_memprop(memprops.memoryTypes[i].propertyFlags); printf("\n");
    }

    query_semaphores();
    query_fences();
    query_formats();

    uint32_t qn = 0; p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    VkQueueFamilyProperties *qf = calloc(qn, sizeof *qf); p_vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    if (qfam == UINT32_MAX) { fprintf(stderr, "no graphics queue family\n"); return 2; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = nreq, .ppEnabledExtensionNames = req_dext };
    r = p_vkCreateDevice(pd, &dci, NULL, &dev);
    printf("\nvkCreateDevice(queue family %u, %u exts) -> %s\n", qfam, nreq, res(r));
    if (r) return 2;
    gdpa = p_vkGetDeviceProcAddr;
    DEV_FNS(LOAD_D)
    p_vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    if ((r = p_vkCreateCommandPool(dev, &cpi, NULL, &cpool))) { fprintf(stderr, "vkCreateCommandPool %s\n", res(r)); return 2; }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if ((r = p_vkAllocateCommandBuffers(dev, &cai, &cmd))) { fprintf(stderr, "vkAllocateCommandBuffers %s\n", res(r)); return 2; }

    dmabuf_roundtrip();
    sync_roundtrip();

    p_vkDeviceWaitIdle(dev);
    p_vkDestroyCommandPool(dev, cpool, NULL);
    p_vkDestroyDevice(dev, NULL);
    p_vkDestroyInstance(inst, NULL);
    printf("\nvkcaps: %d unexpected failure(s) -> exit %d\n", fails, fails ? 1 : 0);
    return fails ? 1 : 0;
}
