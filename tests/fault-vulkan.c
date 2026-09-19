/* Test-only Vulkan-loader shim. Never built into, or installed with, the renderer.
 * Loaded from an isolated test LD_LIBRARY_PATH, forwards to the real loader. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static pthread_once_t once=PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr real_gipa;
static PFN_vkGetDeviceProcAddr real_gdpa;
static PFN_vkCreateSemaphore real_create;
static PFN_vkImportSemaphoreFdKHR real_import;
static void init(void) {
    const char *path=getenv("NVWD_REAL_VULKAN");
    void *lib=path?dlopen(path,RTLD_NOW|RTLD_LOCAL):NULL;
    if(!lib) { fprintf(stderr,"FAULT SHIM: no real Vulkan loader\n"); abort(); }
    real_gipa=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
    real_gdpa=(PFN_vkGetDeviceProcAddr)dlsym(lib,"vkGetDeviceProcAddr");
    if(!real_gipa||!real_gdpa) abort();
    fprintf(stderr,"FAULT SHIM enabled: %s\n",getenv("NVWD_FAULT"));
}
static VkResult bad_export(VkDevice d,const VkFenceGetFdInfoKHR *i,int *fd) {
    (void)d;(void)i;(void)fd;fprintf(stderr,"FAULT SHIM: export error\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
static VkResult bad_import(VkDevice d,const VkImportSemaphoreFdInfoKHR *i) {
    if(i->fd < 0) return real_import(d,i);
    fprintf(stderr,"FAULT SHIM: import error (fd NOT consumed)\n");
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
static VkResult lost_submit(VkQueue q,uint32_t n,const VkSubmitInfo *i,VkFence f) {
    (void)q;(void)n;(void)i;(void)f;fprintf(stderr,"FAULT SHIM: device lost\n");
    return VK_ERROR_DEVICE_LOST;
}
static VkResult delayed_create(VkDevice d,const VkSemaphoreCreateInfo *i,const VkAllocationCallbacks *a,VkSemaphore *s) {
    struct timespec delay={.tv_nsec=3000000};nanosleep(&delay,NULL);
    return real_create(d,i,a,s);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice d,const char *name) {
    pthread_once(&once,init);
    const char *mode=getenv("NVWD_FAULT");
    if(mode) {
        if(!strcmp(mode,"export")&&!strcmp(name,"vkGetFenceFdKHR")) return (PFN_vkVoidFunction)bad_export;
        if(!strcmp(mode,"import")&&!strcmp(name,"vkImportSemaphoreFdKHR")) {
            real_import=(PFN_vkImportSemaphoreFdKHR)real_gdpa(d,name);
            return (PFN_vkVoidFunction)bad_import;
        }
        if(!strcmp(mode,"device-lost")&&!strcmp(name,"vkQueueSubmit")) return (PFN_vkVoidFunction)lost_submit;
        if(!strcmp(mode,"delay")&&!strcmp(name,"vkCreateSemaphore")) {
            real_create=(PFN_vkCreateSemaphore)real_gdpa(d,name);
            return (PFN_vkVoidFunction)delayed_create;
        }
    }
    return real_gdpa(d,name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,const char *name) {
    pthread_once(&once,init);
    if(!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if(!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    return real_gipa(i,name);
}
