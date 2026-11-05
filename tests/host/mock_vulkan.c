/* A libvulkan.so.1 stand-in for tests/host/gpu_alloc_harness.c: one discrete "Mock NVIDIA" device with dma-buf
 * export and DRM format modifiers. It records what vtest_gpu_alloc.c asks for. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))

/* observable state, read by the harness through dlsym on this library */
EXPORT unsigned mock_format_queries, mock_images_live, mock_memories_live, mock_create_image_fail;
EXPORT uint64_t mock_last_modifiers[16];
EXPORT unsigned mock_last_modifier_count;
EXPORT uint32_t mock_last_memory_type;

#define MOD_LINEAR 0ull
#define MOD_TILED_A 0x0300000000606010ull /* single plane, colour + sampled: usable */
#define MOD_TILED_2PLANE 0x0300000000606015ull
#define MOD_TILED_NOSAMPLE 0x0300000000606011ull
#define MOD_TILED_B 0x0300000000606014ull /* usable */

static VKAPI_ATTR VkResult VKAPI_CALL
CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *a, VkInstance *out)
{
   (void)info; (void)a;
   *out = (VkInstance)(uintptr_t)0x1000;
   return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL
EnumeratePhysicalDevices(VkInstance i, uint32_t *count, VkPhysicalDevice *devs)
{
   (void)i;
   if (devs && *count >= 1)
      devs[0] = (VkPhysicalDevice)(uintptr_t)0x2000;
   *count = 1;
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
GetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties *p)
{
   (void)d;
   memset(p, 0, sizeof(*p));
   p->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
   strcpy(p->deviceName, "Mock NVIDIA");
}
static VKAPI_ATTR VkResult VKAPI_CALL
EnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char *layer, uint32_t *count, VkExtensionProperties *p)
{
   (void)d; (void)layer;
   static const char *names[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                                  "VK_EXT_image_drm_format_modifier" };
   if (p) {
      for (unsigned i = 0; i < 3 && i < *count; i++) {
         memset(&p[i], 0, sizeof(p[i]));
         strcpy(p[i].extensionName, names[i]);
      }
   }
   *count = 3;
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
GetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties *p)
{
   (void)d;
   memset(p, 0, sizeof(*p));
   p->memoryTypeCount = 3;
   p->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   p->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   p->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL
CreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a, VkDevice *out)
{
   (void)d; (void)info; (void)a;
   *out = (VkDevice)(uintptr_t)0x3000;
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
GetPhysicalDeviceFormatProperties2(VkPhysicalDevice d, VkFormat format, VkFormatProperties2 *props)
{
   (void)d; (void)format;
   mock_format_queries++;
   VkDrmFormatModifierPropertiesListEXT *list = props->pNext;
   const VkFormatFeatureFlags good = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
   const VkDrmFormatModifierPropertiesEXT all[] = {
      { MOD_LINEAR, 1, good },
      { MOD_TILED_A, 1, good },
      { MOD_TILED_2PLANE, 2, good },
      { MOD_TILED_NOSAMPLE, 1, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT },
      { MOD_TILED_B, 1, good },
   };
   const uint32_t n = sizeof(all) / sizeof(all[0]);
   if (!list->pDrmFormatModifierProperties) {
      list->drmFormatModifierCount = n;
      return;
   }
   uint32_t w = list->drmFormatModifierCount < n ? list->drmFormatModifierCount : n;
   memcpy(list->pDrmFormatModifierProperties, all, w * sizeof(all[0]));
   list->drmFormatModifierCount = w;
}
static VKAPI_ATTR VkResult VKAPI_CALL
CreateImage(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *a, VkImage *out)
{
   (void)dev; (void)a;
   if (mock_create_image_fail)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   const VkImageDrmFormatModifierListCreateInfoEXT *mods = info->pNext;
   if (!mods || mods->sType != VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT)
      abort();
   mock_last_modifier_count = mods->drmFormatModifierCount;
   memcpy(mock_last_modifiers, mods->pDrmFormatModifiers,
          (mods->drmFormatModifierCount < 16 ? mods->drmFormatModifierCount : 16) * sizeof(uint64_t));
   mock_images_live++;
   *out = (VkImage)(uintptr_t)(0x4000 + mock_images_live);
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
DestroyImage(VkDevice dev, VkImage img, const VkAllocationCallbacks *a)
{
   (void)dev; (void)a;
   if (img)
      mock_images_live--;
}
static VKAPI_ATTR void VKAPI_CALL
GetImageMemoryRequirements(VkDevice dev, VkImage img, VkMemoryRequirements *r)
{
   (void)dev; (void)img;
   r->size = 1 << 20;
   r->alignment = 4096;
   r->memoryTypeBits = 0x7;
}
static VKAPI_ATTR void VKAPI_CALL
GetImageSubresourceLayout(VkDevice dev, VkImage img, const VkImageSubresource *s, VkSubresourceLayout *l)
{
   (void)dev; (void)img; (void)s;
   memset(l, 0, sizeof(*l));
   l->rowPitch = 8192;
}
static VKAPI_ATTR VkResult VKAPI_CALL
AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
   (void)dev; (void)a;
   mock_last_memory_type = info->memoryTypeIndex;
   mock_memories_live++;
   *out = (VkDeviceMemory)(uintptr_t)(0x5000 + mock_memories_live);
   return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL
FreeMemory(VkDevice dev, VkDeviceMemory m, const VkAllocationCallbacks *a)
{
   (void)dev; (void)a;
   if (m)
      mock_memories_live--;
}
static VKAPI_ATTR VkResult VKAPI_CALL
BindImageMemory(VkDevice dev, VkImage img, VkDeviceMemory m, VkDeviceSize off)
{
   (void)dev; (void)img; (void)m; (void)off;
   return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL
GetMemoryFdKHR(VkDevice dev, const VkMemoryGetFdInfoKHR *info, int *fd)
{
   (void)dev; (void)info;
   *fd = memfd_create("mock-dmabuf", MFD_CLOEXEC);
   return *fd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
}
static VKAPI_ATTR VkResult VKAPI_CALL
GetImageDrmFormatModifierPropertiesEXT(VkDevice dev, VkImage img, VkImageDrmFormatModifierPropertiesEXT *p)
{
   (void)dev; (void)img;
   p->drmFormatModifier = mock_last_modifiers[0];
   return VK_SUCCESS;
}

static PFN_vkVoidFunction
lookup(const char *name)
{
#define F(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)n;
   F(CreateInstance) F(EnumeratePhysicalDevices) F(GetPhysicalDeviceProperties)
   F(EnumerateDeviceExtensionProperties) F(GetPhysicalDeviceMemoryProperties) F(CreateDevice)
   F(GetPhysicalDeviceFormatProperties2) F(CreateImage) F(DestroyImage) F(GetImageMemoryRequirements)
   F(GetImageSubresourceLayout) F(AllocateMemory) F(FreeMemory) F(BindImageMemory) F(GetMemoryFdKHR)
   F(GetImageDrmFormatModifierPropertiesEXT)
#undef F
   return NULL;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
GetDeviceProcAddr(VkDevice d, const char *name)
{
   (void)d;
   return lookup(name);
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance i, const char *name)
{
   (void)i;
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)GetDeviceProcAddr;
   return lookup(name);
}
