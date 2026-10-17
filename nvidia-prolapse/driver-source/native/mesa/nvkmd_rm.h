/* SPDX-License-Identifier: MIT */
#ifndef NVKMD_RM_H
#define NVKMD_RM_H
#include "../nvkmd.h"
VkResult
nvkmd_rm_try_create_pdev(struct _drmDevice* drm,
                         struct vk_object_base* log_obj,
                         enum nvk_debug debug,
                         struct nvkmd_pdev** out);
#endif
