/* Independent layout oracle: compile against the matching public NVIDIA RM headers. */
#include <stddef.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <nvos.h>
#include <nv-ioctl.h>
#include <nv-unix-nvos-params-wrappers.h>
#include <class/cl0080.h>
#include <class/cl0070.h>
#include <alloc/alloc_channel.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl0080/ctrl0080gpu.h>
#include <ctrl/ctrl2080/ctrl2080gr.h>
#define SIZE(T) printf(#T "\t%zu\n", sizeof(T))
#define OFFSET(T, F) printf(#T "." #F "\t%zu\n", offsetof(T, F))
int main(void) {
    SIZE(NV2080_CTRL_GR_GET_INFO_PARAMS);
    OFFSET(NV2080_CTRL_GR_GET_INFO_PARAMS, grRouteInfo);
    SIZE(NV_VASPACE_ALLOCATION_PARAMETERS);
    SIZE(NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS);
    SIZE(NVOS46_PARAMETERS);
    SIZE(NVOS47_PARAMETERS);
    SIZE(NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS);
    SIZE(NV_CTXSHARE_ALLOCATION_PARAMETERS);
    SIZE(NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS);
    OFFSET(NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS, userdOffset);
    OFFSET(NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS, engineType);
    OFFSET(NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS, tpcConfigID);
    SIZE(NVOS21_PARAMETERS);
    OFFSET(NVOS21_PARAMETERS, status);
    SIZE(NVOS54_PARAMETERS);
    SIZE(NVOS00_PARAMETERS);
    SIZE(nv_ioctl_rm_api_version_t);
    SIZE(nv_ioctl_card_info_t);
    OFFSET(nv_ioctl_card_info_t, minor_number);
    SIZE(NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS);
    SIZE(NV0080_ALLOC_PARAMETERS);
    OFFSET(NV0080_ALLOC_PARAMETERS, vaMode);
    SIZE(NV0080_CTRL_GPU_GET_CLASSLIST_PARAMS);
    SIZE(NV_MEMORY_ALLOCATION_PARAMS);
    OFFSET(NV_MEMORY_ALLOCATION_PARAMS, size);
    OFFSET(NV_MEMORY_ALLOCATION_PARAMS, numaNode);
    SIZE(NVOS33_PARAMETERS);
    SIZE(NVOS34_PARAMETERS);
    OFFSET(NVOS34_PARAMETERS, pLinearAddress);
    OFFSET(NVOS34_PARAMETERS, status);
    OFFSET(NVOS34_PARAMETERS, flags);
    SIZE(nv_ioctl_nvos33_parameters_with_fd);
    printf("ioctl_alloc\t%lu\n", (unsigned long)_IOC(_IOC_READ|_IOC_WRITE,'F',0x2b,sizeof(NVOS21_PARAMETERS)));
    printf("ioctl_card_info\t%lu\n", (unsigned long)_IOC(_IOC_READ|_IOC_WRITE,'F',200,64*sizeof(nv_ioctl_card_info_t)));
    return 0;
}
