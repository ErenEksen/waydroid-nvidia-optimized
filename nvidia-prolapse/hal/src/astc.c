/* ASTC LDR emulation (risk R8): desktop NVIDIA has no ASTC, and its ICD even accepts vkCreateImage for an
 * ASTC format and then reports memory requirements of size 0 (measured on 615.71.09). Android titles create
 * ASTC images whether or not textureCompressionASTC_LDR is advertised (Unity: VMA fails, the game traps).
 *
 * Design: Mesa's common emulation (src/vulkan/runtime/vk_texcompress_astc.c, used by anv/radv) and the
 * baseline's vn_astc_emu (patches/mesa/0003, Venus), re-hosted in a pass-through HAL:
 *  - the app's ASTC image IS an ICD image of the matching RGBA8 format (UNORM or SRGB, as Mesa's
 *    vk_texcompress_astc_emulation_format) with STORAGE usage and MUTABLE_FORMAT|EXTENDED_USAGE added. The
 *    app's handle is that image, so sampling, descriptors, blits from it and barriers need no translation;
 *    views get the RGBA format and a VkImageViewUsageCreateInfo with the app's own usage;
 *  - beside it the HAL keeps the compressed data: one R32G32B32A32_UINT "block image" per mip level (one
 *    texel per 128-bit block, every layer), bound into the app's memory behind the RGBA image; the memory
 *    requirements the app sees cover all of them (so the RGBA size plus about a quarter for 4x4 blocks);
 *  - uploads (vkCmdCopyBufferToImage[2], vkCmdCopyImage[2] into it) copy the blocks into the block image and
 *    then run Mesa's decoder (astc_decoder.glsl, unorm8 mode) in the app's command buffer from the block image
 *    into an R8G8B8A8_UINT storage view of the RGBA image, between barriers; the app's compute state is then
 *    restored (cmdstate.c);
 *  - readbacks (vkCmdCopyImageToBuffer[2], vkCmdCopyImage[2] from it) copy the blocks, so compressed data
 *    round-trips bit-exactly; ASTC->ASTC copies copy blocks and decoded texels;
 *  - image barriers naming the app's image are mirrored onto its block images (layouts, queue ownership).
 * Everything outside that is refused loudly, never handed to the ICD: LINEAR tiling, 1D/3D, sparse, aliasing,
 * protected, disjoint, multisampled, external memory, block-texel views, storage/attachment/host-transfer
 * usage, HDR/3D ASTC, device-address and indirect copies, uploads on queues without compute. */
#include "nvhal.h"
#include "nvhal_astc.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASTC_FIRST VK_FORMAT_ASTC_4x4_UNORM_BLOCK
#define ASTC_LAST VK_FORMAT_ASTC_12x12_SRGB_BLOCK
#define MAX_MIPS 16
#define R8_TAG "(ASTC gap, R8)"

/* VkFormat order: 4x4, 5x4, 5x5, 6x5, 6x6, 8x5, 8x6, 8x8, 10x5, 10x6, 10x8, 10x10, 12x10, 12x12, UNORM then
 * SRGB for each; the same order as vk_texcompress_astc.c get_partition_table_index() */
static const uint8_t block_dims[NVHAL_ASTC_NUM_PARTITION_TABLES][2] = {
    {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

bool nvhal_astc_format(VkFormat f) { return f >= ASTC_FIRST && f <= ASTC_LAST; }
static unsigned fmt_table(VkFormat f) { return (unsigned)(f - ASTC_FIRST) / 2; }
static bool fmt_srgb(VkFormat f) { return ((unsigned)(f - ASTC_FIRST) & 1) != 0; }
static VkFormat fmt_rgba(VkFormat f) { return fmt_srgb(f) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; }
static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint32_t mip_dim(uint32_t d, uint32_t m) { return (d >> m) ? (d >> m) : 1; }

/* What textureCompressionASTC_LDR makes mandatory for every ASTC LDR format (spec "Required Format Support"),
 * plus the transfer bits; the RGBA8 format's own features bound it. */
#define ASTC_FEATURES (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
                       VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT)
#define ASTC_FEATURES_REQUIRED (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
#define ASTC_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define ASTC_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT | \
                    VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
/* dEQP-VK.api.info.image_format_properties requires SPARSE_BINDING (+ SPARSE_ALIASED) for a sampled compressed
 * 2D optimal format on a device with sparseBinding (sparseResidencyAliased); isRequiredImageParameterCombination()
 * exempts only SPARSE_RESIDENCY. Opaque binds are split over the parts (nvhal_astc_bind_sparse). */
#define SPARSE_FLAGS (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)
#define RGBA_EXTRA_FLAGS (VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
#define BLOCK_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
#define PUSH_SIZE 20u /* astc_decoder.glsl: ivec2 texel_blk_start, ivec2 texel_end, bool is_3Dimage */

/* ------------------------------------------------------------------ physical device */

static void probe(struct nvhal_instance *inst, struct nvhal_astc_pd *p) {
    VkPhysicalDevice pd = p->pd;
    VkPhysicalDeviceFeatures f;
    inst->GetPhysicalDeviceFeatures(pd, &f);
    VkPhysicalDeviceProperties props;
    inst->GetPhysicalDeviceProperties(pd, &props);
    p->api_version = props.apiVersion;
    uint32_t nq = 0;
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    bool compute = false;
    p->nq = nq;
    for (uint32_t i = 0; i < nq; i++) {
        p->qflags[i] = qf[i].queueFlags;
        compute |= (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
    }
    if (f.textureCompressionASTC_LDR) {
        p->state = NVHAL_ASTC_NATIVE;
        return;
    }
    p->state = NVHAL_ASTC_UNAVAILABLE;
#define NEED(cond, ...) do { if (!(cond)) { snprintf(p->why, sizeof p->why, __VA_ARGS__); return; } } while (0)
    VkFormatProperties unorm, srgb, u8, u128;
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &unorm);
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_SRGB, &srgb);
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UINT, &u8);
    inst->GetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32G32B32A32_UINT, &u128);
    NEED(u8.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
         "R8G8B8A8_UINT has no optimal-tiling STORAGE_IMAGE feature (0x%x); the decoder writes through it",
         u8.optimalTilingFeatures);
    const VkFormatFeatureFlags bf = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    NEED((u128.optimalTilingFeatures & bf) == bf,
         "R32G32B32A32_UINT optimal-tiling features 0x%x lack SAMPLED_IMAGE|TRANSFER_SRC|TRANSFER_DST (block images)",
         u128.optimalTilingFeatures);
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_LUTS; i++) {
        VkFormatProperties lf;
        inst->GetPhysicalDeviceFormatProperties(pd, nvhal_astc_luts[i].format, &lf);
        NEED(lf.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
             "VkFormat %d has no UNIFORM_TEXEL_BUFFER feature (decoder LUT %u)", nvhal_astc_luts[i].format, i);
    }
    NEED(f.shaderStorageImageWriteWithoutFormat,
         "shaderStorageImageWriteWithoutFormat is unsupported (the decoder's output image has no format qualifier)");
    const VkPhysicalDeviceLimits *l = &props.limits;
    NEED(l->maxComputeWorkGroupInvocations >= 12 * 12 * 4 && l->maxComputeWorkGroupSize[0] >= 12 &&
             l->maxComputeWorkGroupSize[1] >= 12 && l->maxComputeWorkGroupSize[2] >= 4,
         "compute workgroup limits (%u invocations, %ux%ux%u) below the 12x12x4 the decoder needs",
         l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1],
         l->maxComputeWorkGroupSize[2]);
    NEED(l->maxTexelBufferElements >= 12 * 32 * 12 * 32, "maxTexelBufferElements %u below the 12x12 partition table",
         l->maxTexelBufferElements);
    NEED(compute, "no queue family has VK_QUEUE_COMPUTE_BIT");
    p->feat[0] = unorm.optimalTilingFeatures & ASTC_FEATURES;
    p->feat[1] = srgb.optimalTilingFeatures & ASTC_FEATURES;
    NEED((p->feat[0] & ASTC_FEATURES_REQUIRED) == ASTC_FEATURES_REQUIRED &&
             (p->feat[1] & ASTC_FEATURES_REQUIRED) == ASTC_FEATURES_REQUIRED,
         "R8G8B8A8_UNORM/SRGB optimal features 0x%x/0x%x lack SAMPLED_IMAGE|BLIT_SRC|SAMPLED_IMAGE_FILTER_LINEAR, which "
         "textureCompressionASTC_LDR makes mandatory", unorm.optimalTilingFeatures, srgb.optimalTilingFeatures);
#undef NEED
    p->usage = ASTC_USAGE;
    p->flags = ASTC_FLAGS | (f.sparseBinding ? VK_IMAGE_CREATE_SPARSE_BINDING_BIT : 0) |
               (f.sparseBinding && f.sparseResidencyAliased ? VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0);
    p->state = NVHAL_ASTC_EMULATE;
    p->why[0] = 0;
}

const struct nvhal_astc_pd *nvhal_astc_pd_get(struct nvhal_instance *inst, VkPhysicalDevice pd) {
    static const struct nvhal_astc_pd oom = {.state = NVHAL_ASTC_UNAVAILABLE, .why = "out of host memory"};
    pthread_mutex_lock(&nvhal_lock);
    for (struct nvhal_astc_pd *p = inst->astc_pd; p; p = p->next)
        if (p->pd == pd) {
            pthread_mutex_unlock(&nvhal_lock);
            return p;
        }
    pthread_mutex_unlock(&nvhal_lock);
    struct nvhal_astc_pd *n = calloc(1, sizeof *n);
    if (!n) {
        NVHAL_ERR("ASTC emulation: out of host memory probing physical device %p", (void *)pd);
        return &oom;
    }
    n->pd = pd;
    probe(inst, n); /* ICD calls: outside the lock */
    pthread_mutex_lock(&nvhal_lock);
    for (struct nvhal_astc_pd *p = inst->astc_pd; p; p = p->next)
        if (p->pd == pd) {
            pthread_mutex_unlock(&nvhal_lock);
            free(n);
            return p;
        }
    n->next = inst->astc_pd;
    inst->astc_pd = n;
    pthread_mutex_unlock(&nvhal_lock);
    static atomic_bool reported;
    if (n->state == NVHAL_ASTC_UNAVAILABLE && !atomic_exchange(&reported, true))
        NVHAL_ERR("ASTC LDR emulation is unavailable on physical device %p: %s. textureCompressionASTC_LDR stays 0 and "
                  "every ASTC image is refused " R8_TAG " (reported once per process)", (void *)pd, n->why);
    else if (n->state == NVHAL_ASTC_EMULATE)
        NVHAL_TRACE("ASTC LDR emulation active on physical device %p (ASTC features UNORM 0x%x SRGB 0x%x)", (void *)pd,
                    n->feat[0], n->feat[1]);
    return n;
}

void nvhal_astc_instance_teardown(struct nvhal_instance *inst) {
    for (struct nvhal_astc_pd *p = inst->astc_pd, *nx; p; p = nx) {
        nx = p->next;
        free(p);
    }
    inst->astc_pd = NULL;
}

/* the emulating physical device's state, or NULL; *instp gets the instance (NULL: unknown physical device) */
static const struct nvhal_astc_pd *pd_emulating(VkPhysicalDevice pd, struct nvhal_instance **instp, const char *who) {
    struct nvhal_instance *inst = nvhal_physdev_instance(pd);
    *instp = inst;
    if (!inst) {
        NVHAL_ERR("%s(%p): physical device was not enumerated through this HAL", who, (void *)pd);
        return NULL;
    }
    const struct nvhal_astc_pd *p = nvhal_astc_pd_get(inst, pd);
    return p->state == NVHAL_ASTC_EMULATE ? p : NULL;
}

void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = pd_emulating(pd, &inst, "vkGetPhysicalDeviceFeatures");
    if (!inst) {
        memset(f, 0, sizeof *f);
        return;
    }
    inst->GetPhysicalDeviceFeatures(pd, f);
    if (p) f->textureCompressionASTC_LDR = VK_TRUE;
}

static void features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f, bool khr) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = pd_emulating(pd, &inst, "vkGetPhysicalDeviceFeatures2");
    if (!inst) return;
    (khr ? inst->GPDF2_khr : inst->GPDF2_core)(pd, f);
    if (p) f->features.textureCompressionASTC_LDR = VK_TRUE;
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    features2(pd, f, false);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    features2(pd, f, true);
}

void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties(VkPhysicalDevice pd, VkFormat format, VkFormatProperties *fp) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = NULL;
    if (nvhal_astc_format(format)) {
        p = pd_emulating(pd, &inst, "vkGetPhysicalDeviceFormatProperties");
    } else if (!(inst = nvhal_physdev_instance(pd))) {
        NVHAL_ERR("vkGetPhysicalDeviceFormatProperties(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
    }
    if (!inst) {
        memset(fp, 0, sizeof *fp);
        return;
    }
    if (!p) {
        inst->GetPhysicalDeviceFormatProperties(pd, format, fp);
        return;
    }
    *fp = (VkFormatProperties){0, p->feat[fmt_srgb(format)], 0};
}

static void format_props2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *fp, bool khr) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = NULL;
    if (nvhal_astc_format(format)) {
        p = pd_emulating(pd, &inst, "vkGetPhysicalDeviceFormatProperties2");
    } else if (!(inst = nvhal_physdev_instance(pd))) {
        NVHAL_ERR("vkGetPhysicalDeviceFormatProperties2(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
    }
    if (!inst) return;
    (khr ? inst->GPDFP2_khr : inst->GPDFP2_core)(pd, format, fp); /* chained structs: the ICD's answer (none) */
    if (!p) return;
    VkFormatFeatureFlags feat = p->feat[fmt_srgb(format)];
    fp->formatProperties = (VkFormatProperties){0, feat, 0};
    for (VkBaseOutStructure *s = fp->pNext; s; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3: {
            VkFormatProperties3 *f3 = (VkFormatProperties3 *)s;
            f3->linearTilingFeatures = 0;
            f3->optimalTilingFeatures = feat; /* the legacy bits have the same values in VkFormatFeatureFlags2 */
            f3->bufferFeatures = 0;
            break;
        }
        case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT:
            ((VkDrmFormatModifierPropertiesListEXT *)s)->drmFormatModifierCount = 0;
            break;
        case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT:
            ((VkDrmFormatModifierPropertiesList2EXT *)s)->drmFormatModifierCount = 0;
            break;
        default:
            break;
        }
    }
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    format_props2(pd, f, p, false);
}
void VKAPI_CALL nvhal_GetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    format_props2(pd, f, p, true);
}

/* what the emulation implements; *why names the first parameter outside it */
static VkResult ifp_check(const struct nvhal_astc_pd *p, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
                          VkImageCreateFlags flags, const char **why) {
    *why = type != VK_IMAGE_TYPE_2D ? "only 2D ASTC images are emulated"
         : tiling != VK_IMAGE_TILING_OPTIMAL ? "only OPTIMAL tiling is emulated (the texels live decoded in an RGBA8 "
                                               "image; a LINEAR/modifier ASTC memory layout cannot be provided)"
         : (usage & ~p->usage) ? "usage beyond SAMPLED|TRANSFER_SRC|TRANSFER_DST"
         : (flags & ~p->flags) ? "create flags beyond MUTABLE_FORMAT|CUBE_COMPATIBLE|EXTENDED_USAGE|SPARSE_BINDING|"
                                 "SPARSE_ALIASED (sparse residency, alias, protected, disjoint and block-texel-view "
                                 "images are not emulated)"
         : (flags & VK_IMAGE_CREATE_SPARSE_ALIASED_BIT) && !(flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
             ? "SPARSE_ALIASED without SPARSE_BINDING"
         : NULL;
    return *why ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_SUCCESS;
}

VkResult VKAPI_CALL nvhal_GetPhysicalDeviceImageFormatProperties(VkPhysicalDevice pd, VkFormat format, VkImageType type,
                                                                VkImageTiling tiling, VkImageUsageFlags usage,
                                                                VkImageCreateFlags flags, VkImageFormatProperties *out) {
    struct nvhal_instance *inst;
    const struct nvhal_astc_pd *p = NULL;
    if (nvhal_astc_format(format)) {
        p = pd_emulating(pd, &inst, "vkGetPhysicalDeviceImageFormatProperties");
    } else if (!(inst = nvhal_physdev_instance(pd))) {
        NVHAL_ERR("vkGetPhysicalDeviceImageFormatProperties(%p): physical device was not enumerated through this HAL",
                  (void *)pd);
    }
    if (!inst) return VK_ERROR_INITIALIZATION_FAILED;
    if (!p) return inst->GetPhysicalDeviceImageFormatProperties(pd, format, type, tiling, usage, flags, out);
    const char *why;
    VkResult r = ifp_check(p, type, tiling, usage, flags, &why);
    if (r == VK_SUCCESS)
        r = inst->GetPhysicalDeviceImageFormatProperties(pd, fmt_rgba(format), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                         usage | VK_IMAGE_USAGE_STORAGE_BIT, flags | RGBA_EXTRA_FLAGS, out);
    if (r == VK_SUCCESS) {
        out->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    } else {
        memset(out, 0, sizeof *out);
        NVHAL_TRACE("vkGetPhysicalDeviceImageFormatProperties(ASTC VkFormat %d, type %d, tiling %d, usage 0x%x, flags "
                    "0x%x) -> %s%s%s", format, type, tiling, usage, flags, NVHAL_VKR(r), why ? ": " : "", why ? why : "");
    }
    return r;
}

bool nvhal_astc_gpdifp2(struct nvhal_instance *inst, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                        VkImageFormatProperties2 *p, PFN_vkGetPhysicalDeviceImageFormatProperties2 f, VkResult *r) {
    if (!nvhal_astc_format(info->format)) return false;
    const struct nvhal_astc_pd *ap = nvhal_astc_pd_get(inst, pd);
    if (ap->state != NVHAL_ASTC_EMULATE) return false;
    const char *why;
    char buf[96];
    *r = ifp_check(ap, info->type, info->tiling, info->usage, info->flags, &why);
    for (const VkBaseInStructure *s = info->pNext; s && *r == VK_SUCCESS; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
            if (((const VkPhysicalDeviceExternalImageFormatInfo *)s)->handleType) {
                why = "external memory is not emulated for ASTC images";
                *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *fl = (const void *)s;
            for (uint32_t i = 0; i < fl->viewFormatCount; i++)
                if (!nvhal_astc_format(fl->pViewFormats[i]) || fmt_table(fl->pViewFormats[i]) != fmt_table(info->format)) {
                    why = "a view format outside the image's ASTC block size (block-texel views are not emulated)";
                    *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
                }
            break;
        }
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
            break;
        default:
            snprintf(buf, sizeof buf, "input structure sType %d is not answered for emulated ASTC", (int)s->sType);
            why = buf;
            *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            break;
        }
    }
    if (*r == VK_SUCCESS) {
        /* what vkCreateImage will ask the ICD for; the app's output chain is answered for that image */
        VkPhysicalDeviceImageFormatInfo2 q = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL,
                                              fmt_rgba(info->format), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                              info->usage | VK_IMAGE_USAGE_STORAGE_BIT, info->flags | RGBA_EXTRA_FLAGS};
        *r = f(pd, &q, p);
        why = NULL;
    }
    if (*r == VK_SUCCESS) {
        p->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
                memset(&((VkExternalImageFormatProperties *)s)->externalMemoryProperties, 0,
                       sizeof(VkExternalMemoryProperties));
    } else {
        memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties);
        NVHAL_TRACE("vkGetPhysicalDeviceImageFormatProperties2(ASTC VkFormat %d, type %d, tiling %d, usage 0x%x, flags "
                    "0x%x) -> %s%s%s", info->format, info->type, info->tiling, info->usage, info->flags, NVHAL_VKR(*r),
                    why ? ": " : "", why ? why : "");
    }
    return true;
}

/* ------------------------------------------------------------------ device */

struct nvhal_astc_dev {
    pthread_mutex_t lock;
    int init; /* 0 not tried, 1 ready, -1 failed (logged) */
    VkDeviceSize texel_align;
    VkBuffer lut_buf;
    VkDeviceMemory lut_mem;
    VkBufferView lut_view[NVHAL_ASTC_NUM_LUTS], part_view[NVHAL_ASTC_NUM_PARTITION_TABLES];
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkShaderModule sm;
    VkPipeline pipe[NVHAL_ASTC_NUM_PARTITION_TABLES];
    bool pipe_failed[NVHAL_ASTC_NUM_PARTITION_TABLES];
    VkDescriptorPool *pools;
    uint32_t npools;
};

VkResult nvhal_astc_device_ci(struct nvhal_instance *inst, VkPhysicalDevice pd, uint32_t api, const VkDeviceCreateInfo *ci,
                              VkDeviceCreateInfo *out, struct nvhal_astc_ci_storage *st, bool *emulate, bool *maint2) {
    *out = *ci;
    *emulate = *maint2 = false;
    const struct nvhal_astc_pd *p = nvhal_astc_pd_get(inst, pd);
    if (p->state != NVHAL_ASTC_EMULATE) return VK_SUCCESS;
    *emulate = true;
    *maint2 = api < VK_API_VERSION_1_1; /* VkImageViewUsageCreateInfo + EXTENDED_USAGE on a 1.0 device */
    const VkPhysicalDeviceFeatures2 *f2 = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s && !f2; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) f2 = (const void *)s;
    if (ci->pEnabledFeatures || !f2) {
        if (ci->pEnabledFeatures) st->feats = *ci->pEnabledFeatures;
        else memset(&st->feats, 0, sizeof st->feats);
        if (st->feats.textureCompressionASTC_LDR)
            NVHAL_TRACE("vkCreateDevice: textureCompressionASTC_LDR is emulated by the HAL; not passed to the ICD");
        st->feats.textureCompressionASTC_LDR = VK_FALSE;
        st->feats.shaderStorageImageWriteWithoutFormat = VK_TRUE; /* the decoder's output image */
        out->pEnabledFeatures = &st->feats;
        return VK_SUCCESS;
    }
    st->f2 = *f2;
    st->f2.features.textureCompressionASTC_LDR = VK_FALSE;
    st->f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    if (!nvhal_chain_replace(ci->pNext, f2, &st->f2, (const void **)&st->f2.pNext, st->chain, sizeof st->chain,
                             "vkCreateDevice(ASTC emulation features)", &out->pNext))
        return VK_ERROR_INITIALIZATION_FAILED;
    return VK_SUCCESS;
}

VkResult nvhal_astc_device_init(struct nvhal_device *d) {
    const char *missing = NULL;
#define REQ(type, field, name) if (!d->cs.field && !missing) missing = name;
    NVHAL_ASTC_FNS(REQ)
#undef REQ
    if (!missing) {
        missing = !d->cs.BeginCommandBuffer ? "vkBeginCommandBuffer"
                : !d->cs.CreateCommandPool ? "vkCreateCommandPool"
                : !d->cs.CmdBindPipeline ? "vkCmdBindPipeline"
                : !d->cs.CmdBindDescriptorSets ? "vkCmdBindDescriptorSets"
                : !d->cs.CmdPushConstants ? "vkCmdPushConstants"
                : !d->xl.CmdPipelineBarrier ? "vkCmdPipelineBarrier"
                : !d->xl.CmdCopyBufferToImage ? "vkCmdCopyBufferToImage"
                : !d->xl.CmdCopyImage ? "vkCmdCopyImage"
                : !d->xl.CmdCopyImageToBuffer ? "vkCmdCopyImageToBuffer"
                : !d->xl.GetImageMemoryRequirements ? "vkGetImageMemoryRequirements" : NULL;
    }
    if (missing) {
        NVHAL_ERR("vkCreateDevice: the ICD returned NULL for %s, which the ASTC emulation needs -> "
                  "VK_ERROR_INITIALIZATION_FAILED", missing);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct nvhal_astc_dev *a = calloc(1, sizeof *a);
    if (!a) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init(&a->lock, NULL);
    VkPhysicalDeviceProperties props;
    d->inst->GetPhysicalDeviceProperties(d->physical_device, &props);
    a->texel_align = props.limits.minTexelBufferOffsetAlignment ? props.limits.minTexelBufferOffsetAlignment : 1;
    d->astc = a;
    return VK_SUCCESS;
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

static int32_t mem_type(const struct nvhal_device *d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->mem_props.memoryTypes[i].propertyFlags & want) == want) return (int32_t)i;
    return -1;
}

/* vk_texcompress_astc_init() + the shader module: the LUT buffer and its views, the set and pipeline layouts.
 * Caller holds a->lock. */
static VkResult decoder_init(struct nvhal_device *d) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->init) return a->init > 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    a->init = -1;
    VkDevice dev = d->handle;
    VkDeviceSize off[NVHAL_ASTC_NUM_LUTS + NVHAL_ASTC_NUM_PARTITION_TABLES], size = 0;
    const struct nvhal_astc_lut *lut[NVHAL_ASTC_NUM_LUTS + NVHAL_ASTC_NUM_PARTITION_TABLES];
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_LUTS; i++) lut[i] = &nvhal_astc_luts[i];
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_PARTITION_TABLES; i++) lut[NVHAL_ASTC_NUM_LUTS + i] = &nvhal_astc_partition_tables[i];
    const unsigned nl = NVHAL_ASTC_NUM_LUTS + NVHAL_ASTC_NUM_PARTITION_TABLES;
    for (unsigned i = 0; i < nl; i++) {
        size = align_up(size, a->texel_align);
        off[i] = size;
        size += lut[i]->size;
    }
    const char *step = "vkCreateBuffer(LUTs)";
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkResult r = d->xl.CreateBuffer(dev, &bci, NULL, &a->lut_buf);
    VkMemoryRequirements mr = {0};
    int32_t type = -1;
    if (r == VK_SUCCESS) {
        d->cs.GetBufferMemoryRequirements(dev, a->lut_buf, &mr);
        type = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        step = "a HOST_VISIBLE|HOST_COHERENT memory type for the LUT buffer";
        if (type < 0) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (r == VK_SUCCESS) {
        step = "vkAllocateMemory(LUTs)";
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)type};
        r = d->AllocateMemory(dev, &mai, NULL, &a->lut_mem);
    }
    if (r == VK_SUCCESS) {
        step = "vkBindBufferMemory(LUTs)";
        r = d->cs.BindBufferMemory(dev, a->lut_buf, a->lut_mem, 0);
    }
    void *map = NULL;
    if (r == VK_SUCCESS) {
        step = "vkMapMemory(LUTs)";
        r = d->cs.MapMemory(dev, a->lut_mem, 0, VK_WHOLE_SIZE, 0, &map);
    }
    if (r == VK_SUCCESS) {
        for (unsigned i = 0; i < nl; i++) memcpy((unsigned char *)map + off[i], lut[i]->data, lut[i]->size);
        d->cs.UnmapMemory(dev, a->lut_mem);
        step = "vkCreateBufferView(LUTs)";
        for (unsigned i = 0; i < nl && r == VK_SUCCESS; i++) {
            VkBufferViewCreateInfo v = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, a->lut_buf, lut[i]->format,
                                        off[i], lut[i]->size};
            r = d->cs.CreateBufferView(dev, &v, NULL,
                                       i < NVHAL_ASTC_NUM_LUTS ? &a->lut_view[i] : &a->part_view[i - NVHAL_ASTC_NUM_LUTS]);
        }
    }
    if (r == VK_SUCCESS) {
        /* vk_texcompress_astc.c create_layout(), without the push-descriptor flag: the HAL allocates sets */
        VkDescriptorSetLayoutBinding b[8];
        for (uint32_t i = 0; i < 8; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                     : i == 1 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                              : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                                  1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 8, b};
        step = "vkCreateDescriptorSetLayout";
        r = d->cs.CreateDescriptorSetLayout(dev, &dci, NULL, &a->dsl);
    }
    if (r == VK_SUCCESS) {
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE};
        VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &a->dsl, 1, &pcr};
        step = "vkCreatePipelineLayout";
        r = d->cs.CreatePipelineLayout(dev, &pci, NULL, &a->pl);
    }
    if (r == VK_SUCCESS) {
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nvhal_astc_spv_size,
                                        nvhal_astc_spv};
        step = "vkCreateShaderModule(astc_decoder.glsl)";
        r = d->cs.CreateShaderModule(dev, &smi, NULL, &a->sm);
    }
    if (r != VK_SUCCESS) {
        NVHAL_ERR("ASTC emulation: decoder setup failed at %s -> %s; ASTC uploads on device %p stay undecoded " R8_TAG,
                  step, NVHAL_VKR(r), (void *)dev);
        return r;
    }
    NVHAL_TRACE("ASTC emulation: decoder ready on device %p (LUT buffer %llu B, texel alignment %llu)", (void *)dev,
                (unsigned long long)size, (unsigned long long)a->texel_align);
    a->init = 1;
    return VK_SUCCESS;
}

/* vk_texcompress_astc.c create_astc_decode_pipeline(): specialization = block size, DECODE_8BIT = true.
 * Caller holds a->lock. */
static VkPipeline decoder_pipeline(struct nvhal_device *d, unsigned t) {
    struct nvhal_astc_dev *a = d->astc;
    if (a->pipe[t] || a->pipe_failed[t]) return a->pipe[t];
    uint32_t spec[3] = {block_dims[t][0], block_dims[t][1], 1};
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo si = {3, me, sizeof spec, spec};
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, a->sm, "main", &si},
        a->pl, VK_NULL_HANDLE, -1};
    VkResult r = d->cs.CreateComputePipelines(d->handle, VK_NULL_HANDLE, 1, &ci, NULL, &a->pipe[t]);
    if (r != VK_SUCCESS) {
        a->pipe[t] = VK_NULL_HANDLE;
        a->pipe_failed[t] = true;
        NVHAL_ERR("ASTC emulation: vkCreateComputePipelines(decoder %ux%u) -> %s; uploads of that block size stay "
                  "undecoded " R8_TAG, block_dims[t][0], block_dims[t][1], NVHAL_VKR(r));
    }
    return a->pipe[t];
}

/* one descriptor set from the HAL's pools. Caller holds a->lock. */
static VkResult set_alloc(struct nvhal_device *d, VkDescriptorSet *set, VkDescriptorPool *pool) {
    struct nvhal_astc_dev *a = d->astc;
    for (uint32_t i = a->npools; i-- > 0;) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, a->pools[i], 1, &a->dsl};
        VkResult r = d->cs.AllocateDescriptorSets(d->handle, &ai, set);
        if (r == VK_SUCCESS) {
            *pool = a->pools[i];
            return r;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return r;
    }
    enum { SETS = 64 };
    VkDescriptorPoolSize sz[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS},
                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 6 * SETS}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                      VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, SETS, 3, sz};
    VkDescriptorPool *np = realloc(a->pools, (a->npools + 1) * sizeof *np);
    if (!np) return VK_ERROR_OUT_OF_HOST_MEMORY;
    a->pools = np;
    VkResult r = d->cs.CreateDescriptorPool(d->handle, &pci, NULL, &a->pools[a->npools]);
    if (r != VK_SUCCESS) return r;
    *pool = a->pools[a->npools++];
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, *pool, 1, &a->dsl};
    return d->cs.AllocateDescriptorSets(d->handle, &ai, set);
}

/* ------------------------------------------------------------------ images */

/* the decoder's inputs for one (mip level, layer range) of an image, created once, freed with the image */
struct astc_slot {
    uint32_t mip, base, count;
    VkImageView src, dst;
    VkDescriptorSet set;
    VkDescriptorPool pool;
};

struct nvhal_astc_image {
    struct nvhal_device *dev;
    VkImage image;          /* the app's handle = the ICD's RGBA8 image */
    VkFormat format;        /* the app's ASTC format */
    uint32_t bw, bh, table;
    VkExtent3D extent;
    uint32_t mips, layers;
    VkImageUsageFlags usage; /* the app's */
    VkImage block[MAX_MIPS];
    VkDeviceSize off[1 + MAX_MIPS]; /* binding offsets: [0] RGBA image, [1 + m] block image of mip m */
    VkDeviceSize size[1 + MAX_MIPS]; /* each part's own memory size */
    bool sparse;                    /* SPARSE_BINDING: bound with vkQueueBindSparse */
    VkMemoryRequirements req;       /* what the app is told */
    struct astc_slot *slots;        /* d->astc->lock */
    uint32_t nslots, cap;
};

static struct nvhal_hash images = NVHAL_HASH_INIT; /* RGBA VkImage -> struct nvhal_astc_image */
static atomic_size_t n_images;

bool nvhal_no_astc(void) { return atomic_load_explicit(&n_images, memory_order_acquire) == 0; }

static struct nvhal_astc_image *lookup(VkImage image) {
    if (image == VK_NULL_HANDLE || nvhal_no_astc()) return NULL;
    return nvhal_hash_get(&images, NVHAL_ND_U64(image));
}

bool nvhal_astc_is_image(VkImage image) { return lookup(image) != NULL; }

struct astc_cis {
    VkImageCreateInfo rgba;
    VkImageFormatListCreateInfo list;
    VkFormat list_fmts[3];
    VkImageCreateInfo block[MAX_MIPS];
    uint32_t mips;
};

/* the ICD create infos behind an app ASTC create info; refusals are logged and VK_ERROR_FORMAT_NOT_SUPPORTED */
static VkResult build_cis(const struct nvhal_astc_pd *p, const VkImageCreateInfo *ci, struct astc_cis *c,
                          const char *who) {
    const char *why;
    char buf[160];
    VkResult r = ifp_check(p, ci->imageType, ci->tiling, ci->usage, ci->flags, &why);
    if (r == VK_SUCCESS) {
        why = ci->samples != VK_SAMPLE_COUNT_1_BIT ? "multisampled"
            : ci->mipLevels == 0 || ci->mipLevels > MAX_MIPS ? "mip level count out of range"
            : ci->extent.depth != 1 ? "depth != 1"
            : NULL;
    }
    bool list = false, unorm = false, srgb = false;
    for (const VkBaseInStructure *s = ci->pNext; s && !why; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const VkImageFormatListCreateInfo *fl = (const void *)s;
            for (uint32_t i = 0; i < fl->viewFormatCount && !why; i++) {
                VkFormat v = fl->pViewFormats[i];
                if (!nvhal_astc_format(v) || fmt_table(v) != fmt_table(ci->format)) {
                    snprintf(buf, sizeof buf, "view format %d in VkImageFormatListCreateInfo (block-texel views are not "
                             "emulated)", v);
                    why = buf;
                }
                unorm |= !fmt_srgb(v);
                srgb |= fmt_srgb(v);
            }
            list |= fl->viewFormatCount > 0;
            break;
        }
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            if (((const VkExternalMemoryImageCreateInfo *)s)->handleTypes) why = "external memory handle types";
            break;
        case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID:
            if (((const VkExternalFormatANDROID *)s)->externalFormat) why = "an Android external format";
            break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO:
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV:
            break; /* nothing for a color image / a dedication hint the emulation cannot keep */
        default:
            snprintf(buf, sizeof buf, "pNext structure sType %d", (int)s->sType);
            why = buf;
            break;
        }
    }
    if (why) {
        NVHAL_ERR("%s: VkFormat %d (ASTC %ux%u %s) %ux%u, %u mip(s), %u layer(s), type %d, tiling %d, samples %d, usage "
                  "0x%x, flags 0x%x: not emulated: %s " R8_TAG " -> VK_ERROR_FORMAT_NOT_SUPPORTED", who,
                  ci->format, block_dims[fmt_table(ci->format)][0], block_dims[fmt_table(ci->format)][1],
                  fmt_srgb(ci->format) ? "SRGB" : "UNORM", ci->extent.width, ci->extent.height, ci->mipLevels,
                  ci->arrayLayers, ci->imageType, ci->tiling, ci->samples, ci->usage, ci->flags, why);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const unsigned t = fmt_table(ci->format);
    c->rgba = *ci;
    c->rgba.pNext = NULL;
    c->rgba.format = fmt_rgba(ci->format);
    c->rgba.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    c->rgba.flags |= RGBA_EXTRA_FLAGS;
    if (list) { /* the app's view formats, as RGBA, plus the decoder's storage view */
        uint32_t n = 0;
        if (unorm) c->list_fmts[n++] = VK_FORMAT_R8G8B8A8_UNORM;
        if (srgb) c->list_fmts[n++] = VK_FORMAT_R8G8B8A8_SRGB;
        c->list_fmts[n++] = VK_FORMAT_R8G8B8A8_UINT;
        c->list = (VkImageFormatListCreateInfo){VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, n, c->list_fmts};
        c->rgba.pNext = &c->list;
    }
    c->mips = ci->mipLevels;
    for (uint32_t m = 0; m < c->mips; m++)
        c->block[m] = (VkImageCreateInfo){
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, ci->flags & SPARSE_FLAGS, VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_UINT,
            {div_up(mip_dim(ci->extent.width, m), block_dims[t][0]), div_up(mip_dim(ci->extent.height, m), block_dims[t][1]),
             1},
            1, ci->arrayLayers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, BLOCK_USAGE, ci->sharingMode,
            ci->queueFamilyIndexCount, ci->pQueueFamilyIndices, ci->initialLayout};
    return VK_SUCCESS;
}

/* lay the parts out one after another in one allocation; false when no memory type fits all of them. sparse:
 * every part starts on the largest alignment (the app's sparse block size), so a bind range the app aligns to it
 * maps onto whole sparse blocks of each part */
static bool combine(const VkMemoryRequirements *parts, uint32_t n, bool sparse, VkDeviceSize *off,
                    VkMemoryRequirements *out) {
    VkDeviceSize end = 0, align = 1;
    uint32_t bits = ~0u;
    for (uint32_t i = 0; i < n; i++)
        if (parts[i].alignment > align) align = parts[i].alignment;
    for (uint32_t i = 0; i < n; i++) {
        VkDeviceSize a = sparse ? align : parts[i].alignment ? parts[i].alignment : 1;
        end = align_up(end, a);
        off[i] = end;
        end += parts[i].size;
        if (a > align) align = a;
        bits &= parts[i].memoryTypeBits;
    }
    *out = (VkMemoryRequirements){sparse ? align_up(end, align) : end, align, bits};
    return bits != 0;
}

static bool icd_lacks_astc(const struct nvhal_device *d) {
    return !d->astc_pd || d->astc_pd->state != NVHAL_ASTC_NATIVE;
}

static void destroy_parts(struct nvhal_device *d, struct nvhal_astc_image *ai, const VkAllocationCallbacks *alloc,
                          bool app_image) {
    if (d->astc) pthread_mutex_lock(&d->astc->lock);
    for (uint32_t i = 0; i < ai->nslots; i++) {
        struct astc_slot *s = &ai->slots[i];
        if (s->set) d->cs.FreeDescriptorSets(d->handle, s->pool, 1, &s->set);
        if (s->src) d->cs.DestroyImageView(d->handle, s->src, NULL);
        if (s->dst) d->cs.DestroyImageView(d->handle, s->dst, NULL);
    }
    if (d->astc) pthread_mutex_unlock(&d->astc->lock);
    for (uint32_t m = 0; m < MAX_MIPS; m++)
        if (ai->block[m]) d->DestroyImage(d->handle, ai->block[m], NULL);
    if (app_image && ai->image) d->DestroyImage(d->handle, ai->image, alloc);
    free(ai->slots);
    free(ai);
}

bool nvhal_astc_CreateImage(struct nvhal_device *d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc,
                            VkImage *pImage, VkResult *r) {
    static const char who[] = "vkCreateImage(ASTC)";
    if (!nvhal_astc_format(ci->format) || !icd_lacks_astc(d)) return false;
    if (!d->astc) {
        NVHAL_ERR("%s: VkFormat %d is ASTC, which the ICD does not support, and this device has no ASTC emulation (%s) "
                  R8_TAG " -> VK_ERROR_FORMAT_NOT_SUPPORTED", who, ci->format,
                  d->astc_pd && d->astc_pd->why[0] ? d->astc_pd->why : "not set up");
        *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return true;
    }
    struct astc_cis c;
    *r = build_cis(d->astc_pd, ci, &c, who);
    if (*r != VK_SUCCESS) return true;
    struct nvhal_astc_image *ai = calloc(1, sizeof *ai);
    if (!ai) {
        *r = VK_ERROR_OUT_OF_HOST_MEMORY;
        return true;
    }
    const unsigned t = fmt_table(ci->format);
    ai->dev = d;
    ai->format = ci->format;
    ai->bw = block_dims[t][0];
    ai->bh = block_dims[t][1];
    ai->table = t;
    ai->extent = ci->extent;
    ai->mips = c.mips;
    ai->layers = ci->arrayLayers;
    ai->usage = ci->usage;
    VkDevice dev = d->handle;
    const char *step = "vkCreateImage(RGBA8)";
    *r = d->CreateImage(dev, &c.rgba, alloc, &ai->image);
    for (uint32_t m = 0; m < c.mips && *r == VK_SUCCESS; m++) {
        step = "vkCreateImage(block image)";
        *r = d->CreateImage(dev, &c.block[m], NULL, &ai->block[m]);
    }
    VkMemoryRequirements parts[1 + MAX_MIPS];
    if (*r == VK_SUCCESS) {
        d->xl.GetImageMemoryRequirements(dev, ai->image, &parts[0]);
        for (uint32_t m = 0; m < c.mips; m++) d->xl.GetImageMemoryRequirements(dev, ai->block[m], &parts[1 + m]);
        /* a part the ICD wants dedicated cannot share the app's allocation (asked where the core query is valid) */
        PFN_vkGetImageMemoryRequirements2 req2 = d->xl.GetImageMemoryRequirements2;
        if (req2 && d->api_version >= VK_API_VERSION_1_1) {
            for (uint32_t i = 0; i <= c.mips && *r == VK_SUCCESS; i++) {
                VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, NULL, 0, 0};
                VkMemoryRequirements2 m2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr, {0}};
                VkImageMemoryRequirementsInfo2 q = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL,
                                                    i ? ai->block[i - 1] : ai->image};
                req2(dev, &q, &m2);
                if (dr.requiresDedicatedAllocation) {
                    step = "memory requirements (the ICD requires a dedicated allocation for a part; the parts share one)";
                    *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
                }
            }
        }
        ai->sparse = (ci->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0;
        for (uint32_t i = 0; i <= c.mips; i++) ai->size[i] = parts[i].size;
        if (*r == VK_SUCCESS && !combine(parts, 1 + c.mips, ai->sparse, ai->off, &ai->req)) {
            step = "memory requirements (no memory type fits the RGBA8 image and the block images)";
            *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
    }
    if (*r == VK_SUCCESS && !nvhal_hash_put(&images, NVHAL_ND_U64(ai->image), ai)) {
        step = "recording the image";
        *r = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (*r != VK_SUCCESS) {
        NVHAL_ERR("%s: VkFormat %d %ux%u: %s -> %s " R8_TAG, who, ci->format, ci->extent.width, ci->extent.height, step,
                  NVHAL_VKR(*r));
        destroy_parts(d, ai, alloc, true);
        return true;
    }
    atomic_fetch_add_explicit(&n_images, 1, memory_order_release);
    NVHAL_TRACE("%s: VkImage %p = RGBA8 %s %ux%u, %u mip(s), %u layer(s) + %u block image(s); memory %llu B (RGBA8 %llu B), "
                "alignment %llu, types 0x%x", who, NVHAL_LOGH(ai->image), fmt_srgb(ci->format) ? "SRGB" : "UNORM",
                ci->extent.width, ci->extent.height, ai->mips, ai->layers, ai->mips, (unsigned long long)ai->req.size,
                (unsigned long long)parts[0].size, (unsigned long long)ai->req.alignment, ai->req.memoryTypeBits);
    *pImage = ai->image;
    return true;
}

bool nvhal_astc_DestroyImage(struct nvhal_device *d, VkImage image, const VkAllocationCallbacks *alloc) {
    if (image == VK_NULL_HANDLE || nvhal_no_astc()) return false;
    struct nvhal_astc_image *ai = nvhal_hash_del(&images, NVHAL_ND_U64(image));
    if (!ai) return false;
    atomic_fetch_sub_explicit(&n_images, 1, memory_order_release);
    destroy_parts(d, ai, alloc, true);
    return true;
}

static void fill_reqs(const VkMemoryRequirements *req, VkMemoryRequirements2 *out) {
    out->memoryRequirements = *req;
    for (VkBaseOutStructure *s = out->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            ((VkMemoryDedicatedRequirements *)s)->prefersDedicatedAllocation = VK_FALSE;
            ((VkMemoryDedicatedRequirements *)s)->requiresDedicatedAllocation = VK_FALSE;
        }
}

bool nvhal_astc_image_requirements(VkImage image, VkMemoryRequirements2 *out) {
    struct nvhal_astc_image *ai = lookup(image);
    if (!ai) return false;
    fill_reqs(&ai->req, out);
    return true;
}

bool nvhal_astc_ci_refused(struct nvhal_device *d, const VkImageCreateInfo *ci, const char *who) {
    if (!ci || !nvhal_astc_format(ci->format) || !icd_lacks_astc(d)) return false;
    NVHAL_TRACE("%s: ASTC create info (VkFormat %d) answered by the HAL, not the ICD", who, ci->format);
    return true;
}

bool nvhal_astc_ci_requirements(struct nvhal_device *d, const VkImageCreateInfo *ci, VkMemoryRequirements2 *out, bool khr) {
    static const char who[] = "vkGetDeviceImageMemoryRequirements(ASTC)";
    if (!ci || !nvhal_astc_format(ci->format) || !icd_lacks_astc(d)) return false;
    memset(&out->memoryRequirements, 0, sizeof out->memoryRequirements);
    if (!d->astc) {
        NVHAL_ERR("%s: VkFormat %d is ASTC, which the ICD does not support, and this device has no ASTC emulation "
                  R8_TAG "; zero requirements returned", who, ci->format);
        return true;
    }
    struct astc_cis c;
    if (build_cis(d->astc_pd, ci, &c, who) != VK_SUCCESS) return true;
    PFN_vkGetDeviceImageMemoryRequirements f =
        khr ? d->xl.GetDeviceImageMemoryRequirementsKHR : d->xl.GetDeviceImageMemoryRequirements;
    VkMemoryRequirements parts[1 + MAX_MIPS];
    VkDeviceSize off[1 + MAX_MIPS];
    for (uint32_t i = 0; i <= c.mips; i++) {
        VkDeviceImageMemoryRequirements q = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, NULL,
                                             i ? &c.block[i - 1] : &c.rgba, 0};
        VkMemoryRequirements2 m2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, NULL, {0}};
        f(d->handle, &q, &m2);
        parts[i] = m2.memoryRequirements;
    }
    VkMemoryRequirements req;
    if (!combine(parts, 1 + c.mips, (ci->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) != 0, off, &req)) {
        NVHAL_ERR("%s: VkFormat %d: no memory type fits the RGBA8 image and the block images " R8_TAG, who, ci->format);
        return true;
    }
    fill_reqs(&req, out);
    return true;
}

VkResult nvhal_astc_bind1(struct nvhal_device *d, VkImage image, VkDeviceMemory mem, VkDeviceSize offset, bool *handled) {
    struct nvhal_astc_image *ai = lookup(image);
    *handled = ai != NULL;
    if (!ai) return VK_SUCCESS;
    VkResult r = d->xl.BindImageMemory(d->handle, image, mem, offset + ai->off[0]);
    for (uint32_t m = 0; m < ai->mips && r == VK_SUCCESS; m++)
        r = d->xl.BindImageMemory(d->handle, ai->block[m], mem, offset + ai->off[1 + m]);
    if (r != VK_SUCCESS)
        NVHAL_ERR("vkBindImageMemory: ASTC image %p (+%u block images) at memory %p offset %llu -> %s", NVHAL_LOGH(image),
                  ai->mips, NVHAL_LOGH(mem), (unsigned long long)offset, NVHAL_VKR(r));
    return r;
}

VkResult nvhal_astc_expand_binds(uint32_t n, const VkBindImageMemoryInfo *in, VkBindImageMemoryInfo **out, uint32_t *nout) {
    *out = (VkBindImageMemoryInfo *)in;
    *nout = n;
    if (nvhal_no_astc()) return VK_SUCCESS;
    uint32_t extra = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct nvhal_astc_image *ai = lookup(in[i].image);
        if (ai) extra += ai->mips;
    }
    if (!extra) return VK_SUCCESS;
    VkBindImageMemoryInfo *o = malloc((n + extra) * sizeof *o);
    if (!o) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        o[k++] = in[i];
        struct nvhal_astc_image *ai = lookup(in[i].image);
        for (uint32_t m = 0; ai && m < ai->mips; m++)
            o[k++] = (VkBindImageMemoryInfo){VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, NULL, ai->block[m], in[i].memory,
                                             in[i].memoryOffset + ai->off[1 + m]};
    }
    *out = o;
    *nout = k;
    return VK_SUCCESS;
}

bool nvhal_astc_CreateImageView(struct nvhal_device *d, VkDevice device, const VkImageViewCreateInfo *ci,
                                const VkAllocationCallbacks *alloc, VkImageView *view, VkResult *r) {
    struct nvhal_astc_image *ai = lookup(ci->image);
    if (!ai) return false;
    if (!nvhal_astc_format(ci->format) || fmt_table(ci->format) != ai->table) {
        NVHAL_ERR("vkCreateImageView: view format %d of ASTC image %p (VkFormat %d): only the image's own ASTC block size "
                  "is emulated (block-texel views are not) " R8_TAG " -> VK_ERROR_FORMAT_NOT_SUPPORTED", ci->format,
                  NVHAL_LOGH(ci->image), ai->format);
        *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return true;
    }
    VkImageViewCreateInfo t = *ci;
    t.format = fmt_rgba(ci->format);
    bool has_usage = false;
    const void *decode_mode = NULL;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO) has_usage = true;
        if (s->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT) decode_mode = s; /* RGBA8 has no decode mode */
    }
    /* the image has STORAGE usage for the decoder; the app's view must not inherit it (SRGB has no storage) */
    VkImageViewUsageCreateInfo u = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, ai->usage};
    unsigned char storage[1024];
    if ((decode_mode || !has_usage) &&
        !nvhal_chain_replace(ci->pNext, decode_mode, has_usage ? NULL : &u, (const void **)&u.pNext, storage,
                             sizeof storage, "vkCreateImageView(ASTC)", &t.pNext)) {
        *r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        return true;
    }
    *r = d->xl.CreateImageView(device, &t, alloc, view);
    return true;
}

int nvhal_astc_alloc_info(const VkMemoryAllocateInfo *ai, VkMemoryAllocateInfo *out, void *storage, size_t storage_size) {
    if (nvhal_no_astc()) return 0;
    const VkBaseInStructure *ded = NULL;
    VkImage image = VK_NULL_HANDLE;
    for (const VkBaseInStructure *s = ai->pNext; s && !ded; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            image = ((const VkMemoryDedicatedAllocateInfo *)s)->image;
        else if (s->sType == VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV)
            image = ((const VkDedicatedAllocationMemoryAllocateInfoNV *)s)->image;
        else
            continue;
        if (lookup(image)) ded = s;
    }
    if (!ded) return 0;
    *out = *ai;
    if (!nvhal_chain_replace(ai->pNext, ded, NULL, NULL, storage, storage_size, "vkAllocateMemory(ASTC)", &out->pNext))
        return -1;
    NVHAL_TRACE("vkAllocateMemory: dedicated allocation for emulated ASTC image %p made non-dedicated (its memory also "
                "backs the block images)", NVHAL_LOGH(image));
    return 1;
}

/* ------------------------------------------------------------------ sparse binding */

VkResult nvhal_astc_bind_sparse(uint32_t n, const VkBindSparseInfo *in, VkBindSparseInfo **out, void ***owned,
                                uint32_t *nowned) {
    *out = (VkBindSparseInfo *)in;
    *owned = NULL;
    *nowned = 0;
    if (nvhal_no_astc()) return VK_SUCCESS;
    bool any = false;
    for (uint32_t i = 0; i < n && !any; i++) {
        for (uint32_t k = 0; k < in[i].imageOpaqueBindCount && !any; k++) any = lookup(in[i].pImageOpaqueBinds[k].image);
        for (uint32_t k = 0; k < in[i].imageBindCount; k++)
            if (lookup(in[i].pImageBinds[k].image)) {
                NVHAL_ERR("vkQueueBindSparse: VkSparseImageMemoryBindInfo for ASTC image %p: emulated ASTC images have no "
                          "sparse residency " R8_TAG " -> VK_ERROR_FORMAT_NOT_SUPPORTED", NVHAL_LOGH(in[i].pImageBinds[k].image));
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
    }
    if (!any) return VK_SUCCESS;
    VkBindSparseInfo *o = malloc(n * sizeof *o);
    void **own = calloc(2 * (size_t)n + 1, sizeof *own);
    if (!o || !own) {
        free(o);
        free(own);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(o, in, n * sizeof *o);
    own[(*nowned)++] = o;
    for (uint32_t i = 0; i < n; i++) {
        /* count: each emulated image becomes one bind info per part, each with up to the app's bind count */
        uint32_t ninfo = 0, nbind = 0;
        for (uint32_t k = 0; k < in[i].imageOpaqueBindCount; k++) {
            const VkSparseImageOpaqueMemoryBindInfo *b = &in[i].pImageOpaqueBinds[k];
            struct nvhal_astc_image *ai = lookup(b->image);
            ninfo += ai ? 1 + ai->mips : 1;
            nbind += ai ? (1 + ai->mips) * b->bindCount : 0;
        }
        if (ninfo == in[i].imageOpaqueBindCount && !nbind) continue;
        VkSparseImageOpaqueMemoryBindInfo *infos = malloc(ninfo * sizeof *infos + nbind * sizeof(VkSparseMemoryBind));
        if (!infos) {
            for (uint32_t j = 0; j < *nowned; j++) free(own[j]);
            free(own);
            *nowned = 0;
            *out = (VkBindSparseInfo *)in;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        own[(*nowned)++] = infos;
        VkSparseMemoryBind *binds = (VkSparseMemoryBind *)(infos + ninfo);
        uint32_t ni = 0;
        for (uint32_t k = 0; k < in[i].imageOpaqueBindCount; k++) {
            const VkSparseImageOpaqueMemoryBindInfo *b = &in[i].pImageOpaqueBinds[k];
            struct nvhal_astc_image *ai = lookup(b->image);
            if (!ai) {
                infos[ni++] = *b;
                continue;
            }
            for (uint32_t part = 0; part <= ai->mips; part++) {
                const VkDeviceSize ps = ai->off[part], pe = ps + ai->size[part];
                VkSparseImageOpaqueMemoryBindInfo *t = &infos[ni++];
                *t = (VkSparseImageOpaqueMemoryBindInfo){part ? ai->block[part - 1] : ai->image, 0, binds};
                for (uint32_t j = 0; j < b->bindCount; j++) {
                    const VkSparseMemoryBind *m = &b->pBinds[j];
                    VkDeviceSize s0 = m->resourceOffset > ps ? m->resourceOffset : ps;
                    VkDeviceSize e0 = m->resourceOffset + m->size < pe ? m->resourceOffset + m->size : pe;
                    if (s0 >= e0) continue; /* outside this part (or in the padding between parts) */
                    binds[t->bindCount++] = (VkSparseMemoryBind){
                        s0 - ps, e0 - s0, m->memory,
                        m->memory ? m->memoryOffset + (s0 - m->resourceOffset) : 0, m->flags};
                }
                binds += t->bindCount;
            }
        }
        o[i].imageOpaqueBindCount = ni;
        o[i].pImageOpaqueBinds = infos;
    }
    *out = o;
    *owned = own;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ commands */

bool nvhal_astc_refuse(VkImage image, const char *what) {
    struct nvhal_astc_image *ai = lookup(image);
    if (!ai) return false;
    NVHAL_ERR("%s names ASTC image %p (VkFormat %d), which the ASTC emulation does not implement for this command; the "
              "command is dropped " R8_TAG, what, NVHAL_LOGH(image), ai->format);
    return true;
}

static uint32_t range_mips(const struct nvhal_astc_image *ai, const VkImageSubresourceRange *s, uint32_t *first) {
    uint32_t base = s->baseMipLevel;
    if (base >= ai->mips) return 0;
    uint32_t n = s->levelCount == VK_REMAINING_MIP_LEVELS ? ai->mips - base : s->levelCount;
    if (n > ai->mips - base) n = ai->mips - base;
    *first = base;
    return n;
}

#define ASTC_BARRIERS(fn, T)                                                                                       \
    const T *fn(const T *in, uint32_t n, uint32_t *nout, T **owned) {                                             \
        *owned = NULL;                                                                                            \
        *nout = n;                                                                                                \
        if (!n || !in || nvhal_no_astc()) return in;                                                              \
        uint32_t extra = 0, first = 0;                                                                            \
        for (uint32_t i = 0; i < n; i++) {                                                                        \
            struct nvhal_astc_image *ai = lookup(in[i].image);                                                    \
            if (ai) extra += range_mips(ai, &in[i].subresourceRange, &first);                                     \
        }                                                                                                         \
        if (!extra) return in;                                                                                    \
        T *o = malloc((n + extra) * sizeof *o);                                                                   \
        if (!o) {                                                                                                 \
            NVHAL_ERR("image barrier: out of host memory adding the ASTC block images' barriers; their layouts "   \
                      "now differ from the app's image " R8_TAG);                                                 \
            return in;                                                                                            \
        }                                                                                                         \
        memcpy(o, in, n * sizeof *o);                                                                             \
        uint32_t k = n;                                                                                           \
        for (uint32_t i = 0; i < n; i++) {                                                                        \
            struct nvhal_astc_image *ai = lookup(in[i].image);                                                    \
            uint32_t c = ai ? range_mips(ai, &in[i].subresourceRange, &first) : 0;                                \
            for (uint32_t m = first; m < first + c; m++) {                                                        \
                o[k] = in[i];                                                                                     \
                o[k].image = ai->block[m];                                                                        \
                o[k].subresourceRange.baseMipLevel = 0;                                                           \
                o[k].subresourceRange.levelCount = 1;                                                             \
                k++;                                                                                              \
            }                                                                                                     \
        }                                                                                                         \
        *owned = o;                                                                                               \
        *nout = k;                                                                                                \
        return o;                                                                                                 \
    }
ASTC_BARRIERS(nvhal_astc_barriers, VkImageMemoryBarrier)
ASTC_BARRIERS(nvhal_astc_barriers2, VkImageMemoryBarrier2)

static uint32_t layers_of(const struct nvhal_astc_image *ai, const VkImageSubresourceLayers *s) {
    if (s->layerCount != VK_REMAINING_ARRAY_LAYERS) return s->layerCount;
    return s->baseArrayLayer < ai->layers ? ai->layers - s->baseArrayLayer : 0;
}

/* a buffer<->image region of the ASTC image as the same region of its block image (texels -> blocks) */
static VkBufferImageCopy region_blocks(const struct nvhal_astc_image *ai, const VkBufferImageCopy *r) {
    VkBufferImageCopy b = *r;
    b.bufferRowLength = div_up(r->bufferRowLength, ai->bw);
    b.bufferImageHeight = div_up(r->bufferImageHeight, ai->bh);
    b.imageSubresource.mipLevel = 0;
    b.imageOffset = (VkOffset3D){r->imageOffset.x / (int32_t)ai->bw, r->imageOffset.y / (int32_t)ai->bh, 0};
    b.imageExtent = (VkExtent3D){div_up(r->imageExtent.width, ai->bw), div_up(r->imageExtent.height, ai->bh), 1};
    return b;
}

struct decode_job {
    struct nvhal_astc_image *ai;
    uint32_t mip, base, count;
    int32_t x, y; /* texels, block-aligned */
    uint32_t w, h;
    VkImageLayout layout; /* the app's layout of the image (and so of the block image) */
};

/* the cached views + descriptor set for (mip, layers). Caller holds a->lock. */
static VkResult slot_get(struct nvhal_device *d, struct nvhal_astc_image *ai, uint32_t mip, uint32_t base, uint32_t count,
                         struct astc_slot *out) {
    for (uint32_t i = 0; i < ai->nslots; i++)
        if (ai->slots[i].mip == mip && ai->slots[i].base == base && ai->slots[i].count == count) {
            *out = ai->slots[i];
            return VK_SUCCESS;
        }
    if (ai->nslots == ai->cap) {
        uint32_t cap = ai->cap ? ai->cap * 2 : 4;
        struct astc_slot *s = realloc(ai->slots, cap * sizeof *s);
        if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
        ai->slots = s;
        ai->cap = cap;
    }
    struct nvhal_astc_dev *a = d->astc;
    struct astc_slot s = {mip, base, count, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageViewCreateInfo sv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, ai->block[mip],
                                VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R32G32B32A32_UINT, id,
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, base, count}};
    VkImageViewUsageCreateInfo su = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageViewCreateInfo dv = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &su, 0, ai->image, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                VK_FORMAT_R8G8B8A8_UINT, id, {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, base, count}};
    VkResult r = d->xl.CreateImageView(d->handle, &sv, NULL, &s.src);
    if (r == VK_SUCCESS) r = d->xl.CreateImageView(d->handle, &dv, NULL, &s.dst);
    if (r == VK_SUCCESS) r = set_alloc(d, &s.set, &s.pool);
    if (r != VK_SUCCESS) {
        if (s.src) d->cs.DestroyImageView(d->handle, s.src, NULL);
        if (s.dst) d->cs.DestroyImageView(d->handle, s.dst, NULL);
        return r;
    }
    /* vk_texcompress_astc_fill_write_descriptor_sets() */
    VkDescriptorImageInfo di = {VK_NULL_HANDLE, s.dst, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo si = {VK_NULL_HANDLE, s.src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[8];
    w[0] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 0, 0, 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &di, NULL, NULL};
    w[1] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 1, 0, 1,
                                  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &si, NULL, NULL};
    for (uint32_t i = 0; i < NVHAL_ASTC_NUM_LUTS; i++)
        w[2 + i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 2 + i, 0, 1,
                                          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &a->lut_view[i]};
    w[7] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s.set, 7, 0, 1,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &a->part_view[ai->table]};
    d->cs.UpdateDescriptorSets(d->handle, 8, w, 0, NULL);
    ai->slots[ai->nslots++] = s;
    *out = s;
    return VK_SUCCESS;
}

static void record_decode(struct nvhal_device *d, VkCommandBuffer cb, const struct decode_job *j, VkPipeline pipe,
                          const struct astc_slot *s) {
    struct nvhal_astc_image *ai = j->ai;
    /* the copy into the block image (TRANSFER) -> the decoder (COMPUTE); the app's own barrier before its copy
     * covers TRANSFER, which chains into this one for the RGBA image's earlier accesses */
    VkImageMemoryBarrier pre[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, j->layout,
         VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->block[j->mip],
         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, j->base, j->count}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
         j->layout, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->image,
         {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}},
    };
    d->xl.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
                             2, pre);
    d->cs.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    d->cs.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d->astc->pl, 0, 1, &s->set, 0, NULL);
    /* radv_meta_astc_decode.c / vn_astc_emu: block coordinates of the start, texel coordinates of the end, 2D */
    const uint32_t pc[5] = {(uint32_t)j->x / ai->bw, (uint32_t)j->y / ai->bh, (uint32_t)j->x + j->w,
                            (uint32_t)j->y + j->h, 0};
    d->cs.CmdPushConstants(cb, d->astc->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, PUSH_SIZE, pc);
    /* each workgroup decodes 2x2 blocks (local size = block size x 4) */
    d->cs.CmdDispatch(cb, div_up(div_up(j->w, ai->bw), 2), div_up(div_up(j->h, ai->bh), 2), j->count);
    /* back to the app's layout; dst stage TRANSFER so that the app's next barrier, whose first scope is the
     * transfer it recorded, chains with the decoder's writes */
    VkImageMemoryBarrier post[2] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_IMAGE_LAYOUT_GENERAL, j->layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->block[j->mip],
         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, j->base, j->count}},
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, j->layout,
         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, ai->image, {VK_IMAGE_ASPECT_COLOR_BIT, j->mip, 1, j->base, j->count}},
    };
    d->xl.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                             2, post);
}

/* decode the blocks just copied into the block images; then give the app its compute state back */
static void decode_jobs(struct nvhal_device *d, VkCommandBuffer cb, const struct decode_job *jobs, uint32_t n,
                        const char *who) {
    if (!n) return;
    struct nvhal_cmd *e = nvhal_cmd_get(cb);
    uint32_t family = e ? e->family : UINT32_MAX;
    if (family == UINT32_MAX || family >= d->astc_pd->nq || !(d->astc_pd->qflags[family] & VK_QUEUE_COMPUTE_BIT)) {
        NVHAL_ERR("%s: ASTC image %p: the compressed blocks were copied, but command buffer %p belongs to queue family %d, "
                  "which has no VK_QUEUE_COMPUTE_BIT (flags 0x%x); the decode needs compute, the texels stay undecoded "
                  R8_TAG, who, NVHAL_LOGH(jobs[0].ai->image), (void *)cb, (int)family,
                  family < d->astc_pd->nq ? d->astc_pd->qflags[family] : 0);
        return;
    }
    if (nvhal_cs_conditional(cb))
        NVHAL_ERR("%s: ASTC image %p: the upload is recorded inside conditional rendering (command buffer %p); the "
                  "decode dispatch obeys the app's predicate while the copy does not, so a false predicate leaves the "
                  "texels undecoded " R8_TAG, who, NVHAL_LOGH(jobs[0].ai->image), (void *)cb);
    struct { VkPipeline pipe; struct astc_slot slot; } st[8], *res = n <= 8 ? st : malloc(n * sizeof *res);
    if (!res) {
        NVHAL_ERR("%s: out of host memory; ASTC decode dropped " R8_TAG, who);
        return;
    }
    struct nvhal_astc_dev *a = d->astc;
    pthread_mutex_lock(&a->lock);
    VkResult r = decoder_init(d);
    for (uint32_t i = 0; i < n; i++) {
        res[i].pipe = r == VK_SUCCESS ? decoder_pipeline(d, jobs[i].ai->table) : VK_NULL_HANDLE;
        VkResult rs = VK_ERROR_INITIALIZATION_FAILED;
        if (res[i].pipe) rs = slot_get(d, jobs[i].ai, jobs[i].mip, jobs[i].base, jobs[i].count, &res[i].slot);
        if (res[i].pipe && rs != VK_SUCCESS) {
            NVHAL_ERR("%s: ASTC image %p mip %u layers %u+%u: decoder views/descriptor set -> %s; this region stays "
                      "undecoded " R8_TAG, who, NVHAL_LOGH(jobs[i].ai->image), jobs[i].mip, jobs[i].base, jobs[i].count,
                      NVHAL_VKR(rs));
            res[i].pipe = VK_NULL_HANDLE;
        }
    }
    pthread_mutex_unlock(&a->lock);
    bool any = false;
    for (uint32_t i = 0; i < n; i++)
        if (res[i].pipe && jobs[i].w && jobs[i].h && jobs[i].count) {
            record_decode(d, cb, &jobs[i], res[i].pipe, &res[i].slot);
            any = true;
            NVHAL_TRACE("%s: ASTC image %p mip %u layers %u+%u texels (%d,%d) %ux%u decoded in command buffer %p", who,
                        NVHAL_LOGH(jobs[i].ai->image), jobs[i].mip, jobs[i].base, jobs[i].count, jobs[i].x, jobs[i].y,
                        jobs[i].w, jobs[i].h, (void *)cb);
        }
    if (res != st) free(res);
    if (any) nvhal_cs_restore(d, cb, who);
}

static bool mip_ok(const struct nvhal_astc_image *ai, uint32_t mip, const char *who) {
    if (mip < ai->mips) return true;
    NVHAL_ERR("%s: mip level %u of ASTC image %p, which has %u; region skipped", who, mip, NVHAL_LOGH(ai->image), ai->mips);
    return false;
}

bool nvhal_astc_cmd_copy_buffer_to_image(struct nvhal_device *d, VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                         VkImageLayout layout, uint32_t n, const VkBufferImageCopy *r) {
    static const char who[] = "vkCmdCopyBufferToImage(ASTC)";
    struct nvhal_astc_image *ai = lookup(dst);
    if (!ai) return false;
    struct decode_job st[8] = {0}, *jobs = n <= 8 ? st : malloc(n * sizeof *jobs);
    if (!jobs) {
        NVHAL_ERR("%s: out of host memory; copy dropped " R8_TAG, who);
        return true;
    }
    uint32_t nj = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t mip = r[i].imageSubresource.mipLevel;
        if (!mip_ok(ai, mip, who)) continue;
        VkBufferImageCopy b = region_blocks(ai, &r[i]);
        d->xl.CmdCopyBufferToImage(cb, src, ai->block[mip], layout, 1, &b);
        jobs[nj++] = (struct decode_job){ai, mip, r[i].imageSubresource.baseArrayLayer, layers_of(ai, &r[i].imageSubresource),
                                         r[i].imageOffset.x, r[i].imageOffset.y, r[i].imageExtent.width,
                                         r[i].imageExtent.height, layout};
    }
    decode_jobs(d, cb, jobs, nj, who);
    if (jobs != st) free(jobs);
    return true;
}

bool nvhal_astc_cmd_copy_image_to_buffer(struct nvhal_device *d, VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                                         VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    struct nvhal_astc_image *ai = lookup(src);
    if (!ai) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t mip = r[i].imageSubresource.mipLevel;
        if (!mip_ok(ai, mip, "vkCmdCopyImageToBuffer(ASTC)")) continue;
        VkBufferImageCopy b = region_blocks(ai, &r[i]);
        d->xl.CmdCopyImageToBuffer(cb, ai->block[mip], layout, dst, 1, &b);
    }
    return true;
}

bool nvhal_astc_cmd_copy_image(struct nvhal_device *d, VkCommandBuffer cb, VkImage src, VkImageLayout srcL, VkImage dst,
                               VkImageLayout dstL, uint32_t n, const VkImageCopy *r) {
    static const char who[] = "vkCmdCopyImage(ASTC)";
    struct nvhal_astc_image *sa = lookup(src), *da = lookup(dst);
    if (!sa && !da) return false;
    if (sa && da) /* the decoded texels, RGBA8 to RGBA8, as the app's regions say */
        d->xl.CmdCopyImage(cb, src, srcL, dst, dstL, n, r);
    struct decode_job st[8] = {0}, *jobs = st;
    if (!sa && n > 8 && !(jobs = malloc(n * sizeof *jobs))) {
        NVHAL_ERR("%s: out of host memory; copy dropped " R8_TAG, who);
        return true;
    }
    uint32_t nj = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkImageCopy b = r[i];
        VkImage bs = src, bd = dst;
        if (sa) { /* source texels -> blocks; the extent is the source's (spec: compressed <-> uncompressed copies) */
            if (!mip_ok(sa, r[i].srcSubresource.mipLevel, who)) continue;
            bs = sa->block[r[i].srcSubresource.mipLevel];
            b.srcSubresource.mipLevel = 0;
            b.srcOffset = (VkOffset3D){r[i].srcOffset.x / (int32_t)sa->bw, r[i].srcOffset.y / (int32_t)sa->bh, 0};
            b.extent = (VkExtent3D){div_up(r[i].extent.width, sa->bw), div_up(r[i].extent.height, sa->bh), 1};
        }
        if (da) {
            if (!mip_ok(da, r[i].dstSubresource.mipLevel, who)) continue;
            bd = da->block[r[i].dstSubresource.mipLevel];
            b.dstSubresource.mipLevel = 0;
            b.dstOffset = (VkOffset3D){r[i].dstOffset.x / (int32_t)da->bw, r[i].dstOffset.y / (int32_t)da->bh, 0};
        }
        d->xl.CmdCopyImage(cb, bs, srcL, bd, dstL, 1, &b);
        if (da && !sa) { /* uncompressed source texels are blocks of the destination: decode what they cover */
            const uint32_t mip = r[i].dstSubresource.mipLevel;
            uint32_t mw = mip_dim(da->extent.width, mip), mh = mip_dim(da->extent.height, mip);
            uint32_t x = (uint32_t)r[i].dstOffset.x, y = (uint32_t)r[i].dstOffset.y;
            uint32_t w = r[i].extent.width * da->bw, h = r[i].extent.height * da->bh;
            if (x + w > mw) w = mw > x ? mw - x : 0;
            if (y + h > mh) h = mh > y ? mh - y : 0;
            jobs[nj++] = (struct decode_job){da, mip, r[i].dstSubresource.baseArrayLayer,
                                             layers_of(da, &r[i].dstSubresource), (int32_t)x, (int32_t)y, w, h, dstL};
        }
    }
    decode_jobs(d, cb, jobs, nj, who);
    if (jobs != st) free(jobs);
    return true;
}

/* the *2 commands: same work; a pNext the emulation does not know is refused */
static bool regions2_plain(const void *info_next, const void *regions, uint32_t n, size_t stride, const char *who) {
    bool ok = info_next == NULL;
    for (uint32_t i = 0; i < n && ok; i++) ok = ((const VkBaseInStructure *)((const char *)regions + i * stride))->pNext == NULL;
    if (!ok) NVHAL_ERR("%s: a pNext structure on the copy or a region is not implemented by the ASTC emulation; the "
                       "command is dropped " R8_TAG, who);
    return ok;
}

bool nvhal_astc_cmd_copy_buffer_to_image2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info) {
    if (!lookup(info->dstImage)) return false;
    if (!regions2_plain(info->pNext, info->pRegions, info->regionCount, sizeof *info->pRegions, "vkCmdCopyBufferToImage2"))
        return true;
    VkBufferImageCopy st[8] = {0}, *r = info->regionCount <= 8 ? st : malloc(info->regionCount * sizeof *r);
    if (!r) return true;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2 *s = &info->pRegions[i];
        r[i] = (VkBufferImageCopy){s->bufferOffset, s->bufferRowLength, s->bufferImageHeight, s->imageSubresource,
                                   s->imageOffset, s->imageExtent};
    }
    nvhal_astc_cmd_copy_buffer_to_image(d, cb, info->srcBuffer, info->dstImage, info->dstImageLayout, info->regionCount, r);
    if (r != st) free(r);
    return true;
}

bool nvhal_astc_cmd_copy_image_to_buffer2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *info) {
    if (!lookup(info->srcImage)) return false;
    if (!regions2_plain(info->pNext, info->pRegions, info->regionCount, sizeof *info->pRegions, "vkCmdCopyImageToBuffer2"))
        return true;
    VkBufferImageCopy st[8] = {0}, *r = info->regionCount <= 8 ? st : malloc(info->regionCount * sizeof *r);
    if (!r) return true;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkBufferImageCopy2 *s = &info->pRegions[i];
        r[i] = (VkBufferImageCopy){s->bufferOffset, s->bufferRowLength, s->bufferImageHeight, s->imageSubresource,
                                   s->imageOffset, s->imageExtent};
    }
    nvhal_astc_cmd_copy_image_to_buffer(d, cb, info->srcImage, info->srcImageLayout, info->dstBuffer, info->regionCount, r);
    if (r != st) free(r);
    return true;
}

bool nvhal_astc_cmd_copy_image2(struct nvhal_device *d, VkCommandBuffer cb, const VkCopyImageInfo2 *info) {
    if (!lookup(info->srcImage) && !lookup(info->dstImage)) return false;
    if (!regions2_plain(info->pNext, info->pRegions, info->regionCount, sizeof *info->pRegions, "vkCmdCopyImage2"))
        return true;
    VkImageCopy st[8] = {0}, *r = info->regionCount <= 8 ? st : malloc(info->regionCount * sizeof *r);
    if (!r) return true;
    for (uint32_t i = 0; i < info->regionCount; i++) {
        const VkImageCopy2 *s = &info->pRegions[i];
        r[i] = (VkImageCopy){s->srcSubresource, s->srcOffset, s->dstSubresource, s->dstOffset, s->extent};
    }
    nvhal_astc_cmd_copy_image(d, cb, info->srcImage, info->srcImageLayout, info->dstImage, info->dstImageLayout,
                              info->regionCount, r);
    if (r != st) free(r);
    return true;
}

/* ------------------------------------------------------------------ teardown */

static bool of_device(void *v, void *arg) { return ((struct nvhal_astc_image *)v)->dev == arg; }
static void leaked_image(void *v) {
    struct nvhal_astc_image *ai = v;
    atomic_fetch_sub_explicit(&n_images, 1, memory_order_release);
    NVHAL_WARN("vkDestroyDevice: ASTC image %p was not destroyed by the app; its HAL objects are freed now", NVHAL_LOGH(ai->image));
    destroy_parts(ai->dev, ai, NULL, false); /* the RGBA8 image is the app's (leaked) object */
}

void nvhal_astc_device_teardown(struct nvhal_device *d) {
    struct nvhal_astc_dev *a = d->astc;
    if (!a) return;
    nvhal_hash_sweep(&images, of_device, d, leaked_image);
    VkDevice dev = d->handle;
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_PARTITION_TABLES; i++)
        if (a->pipe[i]) d->cs.DestroyPipeline(dev, a->pipe[i], NULL);
    for (uint32_t i = 0; i < a->npools; i++) d->cs.DestroyDescriptorPool(dev, a->pools[i], NULL);
    if (a->pl) d->cs.DestroyPipelineLayout(dev, a->pl, NULL);
    if (a->dsl) d->cs.DestroyDescriptorSetLayout(dev, a->dsl, NULL);
    if (a->sm) d->cs.DestroyShaderModule(dev, a->sm, NULL);
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_LUTS; i++)
        if (a->lut_view[i]) d->cs.DestroyBufferView(dev, a->lut_view[i], NULL);
    for (unsigned i = 0; i < NVHAL_ASTC_NUM_PARTITION_TABLES; i++)
        if (a->part_view[i]) d->cs.DestroyBufferView(dev, a->part_view[i], NULL);
    if (a->lut_buf) d->cs.DestroyBuffer(dev, a->lut_buf, NULL);
    if (a->lut_mem) d->FreeMemory(dev, a->lut_mem, NULL);
    free(a->pools);
    pthread_mutex_destroy(&a->lock);
    free(a);
    d->astc = NULL;
}
