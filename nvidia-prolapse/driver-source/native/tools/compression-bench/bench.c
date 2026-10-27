#define _GNU_SOURCE
/* SPDX-License-Identifier: MIT
 * Bandwidth-bound A/B microbenchmark for colour compression (independent code).
 * One 3840x2160 RGBA8 OPTIMAL colour attachment (device-local), per frame:
 *   clear pass, DRAWS blended full-screen draws (read+write every pixel),
 *   one full-image vkCmdCopyImage into a second render-target-capable image.
 * GPU time from timestamps; result pixels verified against the exact blend.
 * Usage: bench ICD.so fullscreen.vert.spv blend.frag.spv [frames]
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSTANCE_API(X) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_API(X) X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(GetImageMemoryRequirements) \
 X(CreateBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindImageMemory) X(BindBufferMemory) X(MapMemory) \
 X(CreateImageView) X(CreateRenderPass) X(CreateFramebuffer) X(CreateShaderModule) X(CreatePipelineLayout) \
 X(CreateGraphicsPipelines) X(CreateCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) \
 X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdPushConstants) X(CmdDraw) \
 X(CmdCopyImage) X(CmdCopyImageToBuffer) X(CmdPipelineBarrier) X(CreateFence) X(ResetFences) X(WaitForFences) \
 X(QueueSubmit) X(DeviceWaitIdle) X(CreateQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults) X(CmdResetQueryPool)
#define DECLARE(name) static PFN_vk##name p##name;
INSTANCE_API(DECLARE)
DEVICE_API(DECLARE)
#undef DECLARE
static void fail(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void result(VkResult s, const char *op, int line) { if (s) { fprintf(stderr, "FAIL: line %d %s = %d\n", line, op, s); exit(1); } }
#define CHECK(call) result((call), #call, __LINE__)
static VkDevice device;
static VkPhysicalDeviceMemoryProperties mp;
static VkDeviceMemory allocate(VkMemoryRequirements r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((r.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
            !(mp.memoryTypes[i].propertyFlags & avoid)) {
            VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = i};
            VkDeviceMemory m; CHECK(pAllocateMemory(device, &info, NULL, &m)); return m;
        }
    fail("memory type"); return VK_NULL_HANDLE;
}
static VkShaderModule shader(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) fail("open shader");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(n); if (fread(code, 1, n, f) != (size_t)n) fail("read shader"); fclose(f);
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code};
    VkShaderModule m; CHECK(pCreateShaderModule(device, &info, NULL, &m)); free(code); return m;
}
static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s ICD.so vert.spv frag.spv [frames]\n", argv[0]); return 2; }
    int frames = argc > 4 ? atoi(argv[4]) : 60;
    enum { W = 3840, H = 2160, DRAWS = 16 };
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); if (!lib) fail(dlerror());
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) fail("no gipa");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; CHECK(((PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance"))(&ii, NULL, &inst));
#define LI(name) p##name = (PFN_vk##name)gipa(inst, "vk" #name); if (!p##name) fail("vk" #name);
    INSTANCE_API(LI)
    uint32_t count = 8; VkPhysicalDevice pds[8]; CHECK(pEnumeratePhysicalDevices(inst, &count, pds));
    VkPhysicalDevice pd = VK_NULL_HANDLE; VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < count; i++) { pGetPhysicalDeviceProperties(pds[i], &props); if (props.vendorID == 0x10de) { pd = pds[i]; break; } }
    if (!pd) fail("no NVIDIA device");
    printf("GPU: %s driver %08x\n", props.deviceName, props.driverVersion);
    pGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(pCreateDevice(pd, &di, NULL, &device));
#define LD(name) p##name = (PFN_vk##name)pGetDeviceProcAddr(device, "vk" #name); if (!p##name) fail("vk" #name);
    DEVICE_API(LD)
    VkQueue queue; pGetDeviceQueue(device, 0, 0, &queue);
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img, dst; CHECK(pCreateImage(device, &ici, NULL, &img));
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(pCreateImage(device, &ici, NULL, &dst));
    VkMemoryRequirements r; pGetImageMemoryRequirements(device, img, &r);
    CHECK(pBindImageMemory(device, img, allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT), 0));
    pGetImageMemoryRequirements(device, dst, &r);
    CHECK(pBindImageMemory(device, dst, allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT), 0));
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf; CHECK(pCreateBuffer(device, &bci, NULL, &buf));
    pGetBufferMemoryRequirements(device, buf, &r);
    VkDeviceMemory bm = allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    CHECK(pBindBufferMemory(device, buf, bm, 0));
    uint8_t *mapped; CHECK(pMapMemory(device, bm, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = ici.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; CHECK(pCreateImageView(device, &vi, NULL, &view));
    VkAttachmentDescription att = {.format = ici.format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref};
    VkSubpassDependency dep[2] = {{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0}};
    VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep};
    VkRenderPass pass; CHECK(pCreateRenderPass(device, &rpi, NULL, &pass));
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass, .attachmentCount = 1,
        .pAttachments = &view, .width = W, .height = H, .layers = 1};
    VkFramebuffer fb; CHECK(pCreateFramebuffer(device, &fbi, NULL, &fb));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout layout; CHECK(pCreatePipelineLayout(device, &pli, NULL, &layout));
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(argv[2]), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(argv[3]), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = layout, .renderPass = pass};
    VkPipeline pipe; CHECK(pCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    VkCommandPool pool; CHECK(pCreateCommandPool(device, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1};
    VkCommandBuffer cmd; CHECK(pAllocateCommandBuffers(device, &cai, &cmd));
    VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 3};
    VkQueryPool qp; CHECK(pCreateQueryPool(device, &qpi, NULL, &qp));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; CHECK(pCreateFence(device, &fci, NULL, &fence));
    double *render_ms = calloc(frames, sizeof(double)), *copy_ms = calloc(frames, sizeof(double));
    const float alpha = 0.5f, base[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    for (int frame = 0; frame < frames + 1; frame++) {
        int verify = frame == frames;
        CHECK(pResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(pBeginCommandBuffer(cmd, &bi));
        pCmdResetQueryPool(cmd, qp, 0, 3);
        VkImageMemoryBarrier tb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = dst,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        pCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
        pCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        VkClearValue clear = {.color = {{base[0], base[1], base[2], base[3]}}};
        VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass, .framebuffer = fb,
            .renderArea = {{0, 0}, {W, H}}, .clearValueCount = 1, .pClearValues = &clear};
        pCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE); pCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        for (int d = 0; d < DRAWS; d++) {
            float c[4] = {(float)(d & 1), (float)((d >> 1) & 1), (float)((d >> 2) & 1), alpha};
            pCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, c); pCmdDraw(cmd, 3, 1, 0, 0);
        }
        pCmdEndRenderPass(cmd);
        pCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        VkImageCopy ic = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {W, H, 1}};
        pCmdCopyImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        pCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2);
        if (verify) {
            tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; tb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            pCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
            VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
            pCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bc);
            VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            pCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        }
        CHECK(pEndCommandBuffer(cmd)); CHECK(pResetFences(device, 1, &fence));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        CHECK(pQueueSubmit(queue, 1, &si, fence)); CHECK(pWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull));
        uint64_t ts[3]; CHECK(pGetQueryPoolResults(device, qp, 0, 3, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        if (!verify) {
            render_ms[frame] = (ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
            copy_ms[frame] = (ts[2] - ts[1]) * props.limits.timestampPeriod / 1e6;
        }
    }
    /* Expected value: start from the clear colour, blend each draw with alpha 0.5. */
    float e[4] = {base[0], base[1], base[2], base[3]};
    for (int d = 0; d < DRAWS; d++) {
        float c[3] = {(float)(d & 1), (float)((d >> 1) & 1), (float)((d >> 2) & 1)};
        for (int k = 0; k < 3; k++) { e[k] = c[k] * alpha + e[k] * (1 - alpha); e[k] = (float)(int)(e[k] * 255.0f + 0.5f) / 255.0f; }
        e[3] = alpha;
    }
    unsigned bad = 0;
    for (size_t p = 0; p < (size_t)W * H; p++)
        for (int k = 0; k < 4; k++) { int v = mapped[p * 4 + k], x = (int)(e[k] * 255.0f + 0.5f); if (v - x > 2 || x - v > 2) { if (!bad++) fprintf(stderr, "pixel %zu ch%d got %d expected ~%d\n", p, k, v, x); } }
    qsort(render_ms, frames, sizeof(double), cmp); qsort(copy_ms, frames, sizeof(double), cmp);
    printf("render (clear + %d blended full-screen draws, %dx%d RGBA8): median %.3f ms, min %.3f, max %.3f\n", DRAWS, W, H, render_ms[frames / 2], render_ms[0], render_ms[frames - 1]);
    printf("copy (full-image vkCmdCopyImage): median %.3f ms, min %.3f, max %.3f\n", copy_ms[frames / 2], copy_ms[0], copy_ms[frames - 1]);
    printf("%s: %u mismatching bytes of %u\n", bad ? "FAIL" : "PASS", bad, W * H * 4);
    CHECK(pDeviceWaitIdle(device));
    return bad ? 1 : 0;
}
