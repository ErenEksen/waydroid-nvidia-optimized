/* Create -> immediately import multiple external semaphores -> submit -> export
 * a completion fence. Run through the host Venus ICD to cover the same socket
 * ordering as Android without needing a rooted/running Android container. */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x) do { VkResult r=(x); if(r!=VK_SUCCESS) {fprintf(stderr,"%s: %d at %d\n",#x,r,__LINE__);return 1;} } while(0)
static int fd_count(void) {
    DIR *d=opendir("/proc/self/fd");
    if(!d)return -1;
    int n=0;struct dirent *e;
    while((e=readdir(d))) if(e->d_name[0]!='.') n++;
    closedir(d);return n;
}
int main(int argc, char **argv) {
    const int debug=getenv("NVWD_SYNC_DEBUG")!=NULL;
    const unsigned rounds=argc>1?(unsigned)strtoul(argv[1],NULL,10):1000;
    if(!rounds || rounds>100000) return 2;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="nvwd-sync-regression",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance; CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=16; VkPhysicalDevice devices[16],pd=VK_NULL_HANDLE;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,devices));
    for(uint32_t i=0;i<count;i++) {
        VkPhysicalDeviceProperties prop;vkGetPhysicalDeviceProperties(devices[i],&prop);
        if(prop.vendorID==0x10de || prop.vendorID==0x1af4) { pd=devices[i];fprintf(stderr,"GPU: %s\n",prop.deviceName);break; }
    }
    if(!pd) {fprintf(stderr,"no NVIDIA/Venus device\n");return 77;}
    uint32_t n=16,qf=UINT32_MAX;VkQueueFamilyProperties qp[16];vkGetPhysicalDeviceQueueFamilyProperties(pd,&n,qp);
    for(uint32_t i=0;i<n;i++) if(qp[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) {qf=i;break;}
    if(qf==UINT32_MAX)return 77;
    unsigned nq=getenv("NVWD_SYNC_QUEUES")?(unsigned)strtoul(getenv("NVWD_SYNC_QUEUES"),NULL,10):1;
    if(nq<1||nq>2||qp[qf].queueCount<nq) return 77;
    float priority[2]={1,1};VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qf,.queueCount=nq,.pQueuePriorities=priority};
    const char *exts[]={VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci,.enabledExtensionCount=2,.ppEnabledExtensionNames=exts};
    if(debug)fprintf(stderr,"creating device\n");
    VkDevice dev;CHECK(vkCreateDevice(pd,&dci,NULL,&dev));VkQueue queues[2];
    for(unsigned i=0;i<nq;i++)vkGetDeviceQueue(dev,qf,i,&queues[i]);
    fprintf(stderr,"queues=%u\n",nq);
    PFN_vkGetSemaphoreFdKHR getSem=(void*)vkGetDeviceProcAddr(dev,"vkGetSemaphoreFdKHR");
    PFN_vkImportSemaphoreFdKHR importSem=(void*)vkGetDeviceProcAddr(dev,"vkImportSemaphoreFdKHR");
    PFN_vkGetFenceFdKHR getFence=(void*)vkGetDeviceProcAddr(dev,"vkGetFenceFdKHR");
    if(!getSem||!importSem||!getFence)return 77;
    VkExportSemaphoreCreateInfo esi={.sType=VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo sci={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&esi};
    VkExportFenceCreateInfo efi={.sType=VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,.handleTypes=VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.pNext=&efi};
    int initial_fds=-1;
    for(unsigned k=0;k<rounds;k++) {
        VkQueue queue=queues[k%nq];
        if(debug)fprintf(stderr,"round %u begin\n",k);
        VkSemaphore sources[2],targets[2];VkFence fence;
        for(unsigned i=0;i<2;i++)CHECK(vkCreateSemaphore(dev,&sci,NULL,&sources[i]));
        CHECK(vkCreateFence(dev,&fci,NULL,&fence));
        VkSubmitInfo signal={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.signalSemaphoreCount=2,.pSignalSemaphores=sources};
        CHECK(vkQueueSubmit(queue,1,&signal,VK_NULL_HANDLE));
        if(debug)fprintf(stderr,"signal submitted\n");
        for(unsigned i=0;i<2;i++) {
            int fd=-1;VkSemaphoreGetFdInfoKHR gi={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,.semaphore=sources[i],.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            CHECK(getSem(dev,&gi,&fd));
            if(debug)fprintf(stderr,"source %u exported fd %d\n",i,fd);
            CHECK(vkCreateSemaphore(dev,&sci,NULL,&targets[i]));
            VkImportSemaphoreFdInfoKHR ii={.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,.semaphore=targets[i],.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=fd};
            CHECK(importSem(dev,&ii)); // success transfers fd ownership
        }
        VkPipelineStageFlags stages[2]={VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        VkSubmitInfo wait={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=2,.pWaitSemaphores=targets,.pWaitDstStageMask=stages};
        if(debug)fprintf(stderr,"submitting imported waits\n");
        CHECK(vkQueueSubmit(queue,1,&wait,fence));
        int fd=-1;VkFenceGetFdInfoKHR gi={.sType=VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,.fence=fence,.handleType=VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        CHECK(getFence(dev,&gi,&fd));
        if(debug)fprintf(stderr,"completion exported fd %d\n",fd);
        if(fd>=0) {
            struct pollfd p={.fd=fd,.events=POLLIN};int ready;
            do {ready=poll(&p,1,5000);} while(ready<0&&errno==EINTR);
            close(fd);
            if(ready<=0||!(p.revents&POLLIN)||(p.revents&(POLLERR|POLLNVAL))) {fprintf(stderr,"fence timeout/error at round %u\n",k);return 1;}
        }
        for(unsigned i=0;i<2;i++){vkDestroySemaphore(dev,sources[i],NULL);vkDestroySemaphore(dev,targets[i],NULL);}
        vkDestroyFence(dev,fence,NULL);
        if(k==31)initial_fds=fd_count();
    }
    CHECK(vkDeviceWaitIdle(dev));
    int final_fds=fd_count();
    if(initial_fds>=0 && final_fds>initial_fds+2) {fprintf(stderr,"FD growth: %d -> %d\n",initial_fds,final_fds);return 1;}
    fprintf(stderr,"client fds: plateau=%d final=%d\n",initial_fds,final_fds);
    vkDestroyDevice(dev,NULL);vkDestroyInstance(instance,NULL);
    printf("PASS: %u create/import/submit/export cycles, two semaphores per batch\n",rounds);
    return 0;
}
