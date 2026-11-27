// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The march, through the layer, on a GPU (after take 486). This plays X4's part: a pipeline
// layout like X4's (samplers and images at set 0, the camera at set 1, the dynamic block at
// set 4), X4's fog walk (tests/sample_volume_walk.comp, the stand-in the layer recognises as
// X4's), dispatched at X4's size, and X4's fog blend (tests/sample_fog_apply.frag) drawn
// after it -- all in one submission, the first the layer sees. With X4VR_FOG_PROBE=march
// the layer must swap in its march, dispatch it at its own size between barriers, patch the
// blend to read the march's volume, and put its one-time setup in front of the submission.
//
// What it prints, for tests/run-march-smoke.sh: FINITE (no NaN or Inf in the blended
// picture), MEAN and SPREAD of its green, and OURS -- whether the picture is not what X4's
// own grid stand-in (a constant 0.5) would give: 0.3 * 0.5 + 0.5 = 0.65. With the validation layer below ours, any
// misuse of Vulkan by the layer shows on stderr.
//
// SMOKE_FRAMES=3 (after take 492): two more frames, each presented through a headless
// swapchain so the layer sees X4's frame boundaries -- the blend drawn with no walk
// dispatched, as in a sector without fog. Frame 3 is printed too (F3_ lines): the layer
// must have cleared its volume once it saw a frame with no march, or the blend shows the
// last march's picture, fixed to the screen (take 492, The Void).
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CK(x)                                                                              \
    do {                                                                                   \
        VkResult r_ = (x);                                                                 \
        if (r_ != VK_SUCCESS) {                                                            \
            printf("FAIL %s -> %d (line %d)\n", #x, r_, __LINE__);                         \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memp;
static uint32_t mtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}
static std::vector<uint32_t> load(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f)
        return {};
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> v((size_t)n / 4);
    if (fread(v.data(), 4, v.size(), f) != v.size())
        v.clear();
    fclose(f);
    return v;
}
static VkShaderModule module(const char *p) {
    const std::vector<uint32_t> c = load(p);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = c.size() * 4;
    ci.pCode = c.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (c.empty() || vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}
static bool buffer(VkDeviceSize n, VkBufferUsageFlags u, VkBuffer *b, void **map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n;
    bi.usage = u;
    if (vkCreateBuffer(dev, &bi, nullptr, b) != VK_SUCCESS)
        return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory m;
    return vkAllocateMemory(dev, &ai, nullptr, &m) == VK_SUCCESS &&
           vkBindBufferMemory(dev, *b, m, 0) == VK_SUCCESS &&
           vkMapMemory(dev, m, 0, n, 0, map) == VK_SUCCESS;
}
static bool image(VkImageType t, VkImageViewType vt, VkFormat f, VkExtent3D e,
                  VkImageUsageFlags u, VkImage *im, VkImageView *v) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = t;
    ci.format = f;
    ci.extent = e;
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = u;
    if (vkCreateImage(dev, &ci, nullptr, im) != VK_SUCCESS)
        return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *im, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, nullptr, &m) != VK_SUCCESS ||
        vkBindImageMemory(dev, *im, m, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = *im;
    vi.viewType = vt;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(dev, &vi, nullptr, v) == VK_SUCCESS;
}
static float h2f(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0    ? std::ldexp((float)m, -24)
                    : e == 31 ? (m ? NAN : INFINITY)
                              : std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: %s walk.comp.spv fullscreen.vert.spv fog_apply.frag.spv\n", argv[0]);
        return 2;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "X4";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const bool frames3 = getenv("SMOKE_FRAMES") && atoi(getenv("SMOKE_FRAMES")) >= 3;
    const char *iexts[2] = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_EXT_headless_surface"};
    if (frames3) {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = iexts;
    }
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    if (!n) {
        printf("FAIL no device\n");
        return 1;
    }
    VkPhysicalDevice phys = pds[0];
    VkPhysicalDeviceProperties props{};
    for (auto p : pds) {
        vkGetPhysicalDeviceProperties(p, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = p;
            break;
        }
    }
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("DEVICE=%s\n", props.deviceName);
    vkGetPhysicalDeviceMemoryProperties(phys, &memp);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    uint32_t fam = 0;
    while (fam < qn && !(qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        fam++;
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = fam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    const char *dexts[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (frames3) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dexts;
    }
    CK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, fam, 0, &q);

    // X4's sets: 0 samplers (4), images (5) and storage images (6, the walk's output
    // table), both tables as large as X4's, so the stereo patch offsets the blend's
    // lookups by the eye as in X4 (take 487); 1 the camera; 4 the dynamic block.
    VkDescriptorSetLayoutBinding b0[3] = {
        {4, VK_DESCRIPTOR_TYPE_SAMPLER, 18, VK_SHADER_STAGE_ALL, nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 53306, VK_SHADER_STAGE_ALL, nullptr},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 53306, VK_SHADER_STAGE_ALL, nullptr}};
    VkDescriptorSetLayoutBinding b1 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                       VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorSetLayout dsl0, dsl1, dsl4, dsle;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3;
    lci.pBindings = b0;
    CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsl0));
    lci.bindingCount = 1;
    lci.pBindings = &b1;
    CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsl1));
    CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsl4));
    lci.bindingCount = 0;
    CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsle));
    const VkDescriptorSetLayout sets[5] = {dsl0, dsl1, dsle, dsle, dsl4};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 5;
    pli.pSetLayouts = sets;
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));

    // The camera block, as X4's: reversed-Z (z_ndc = 0.1 / z), a 55 deg half field, the
    // camera where the offline model looked from.
    VkBuffer cam, dyn, rb;
    void *pc, *pd, *prb;
    if (!buffer(2048, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &cam, &pc) ||
        !buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &dyn, &pd) ||
        !buffer(64 * 64 * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &prb)) {
        printf("FAIL buffers\n");
        return 1;
    }
    float *c = (float *)pc;
    memset(c, 0, 2048);
    const float T = 1.4281480f;
    float *mi = c + 128 / 4;
    mi[0] = mi[5] = 0.1f * T;
    mi[14] = 0.1f;
    mi[11] = 1.f;
    float *vi = c + 512 / 4;
    vi[0] = vi[5] = vi[10] = vi[15] = 1.f;
    vi[12] = -18799.f;
    vi[13] = -5441.f;
    vi[14] = 49200.f;
    const float lc[4] = {1.55f, 2.45f, 2.59f, 1.f};
    memcpy(c + 784 / 4, lc, 16);
    const float vs[4] = {176.f, 176.f, 128.f, 0.f};
    memcpy(c + 960 / 4, vs, 16);
    c[1420 / 4] = 91000.f;
    c[1528 / 4] = 1.667e-5f;
    c[1532 / 4] = 0.003108f;
    uint32_t *dw = (uint32_t *)pd;
    memset(dw, 0, 256);
    dw[1] = 1; // U_pass

    // X4's grid, stood in by a constant 0.5 volume at every image slot.
    VkImage grid, colour, walkout;
    VkImageView grid_v, colour_v, walkout_v;
    if (!image(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, {4, 4, 4},
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &grid, &grid_v) ||
        !image(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, {64, 64, 1},
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &colour,
               &colour_v) ||
        // X4's own walk writes slot 0 of the storage table when the march is off: a
        // volume the size of X4's grid there.
        !image(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT,
               {176, 176, 128}, VK_IMAGE_USAGE_STORAGE_BIT, &walkout, &walkout_v)) {
        printf("FAIL images\n");
        return 1;
    }
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler smp;
    CK(vkCreateSampler(dev, &sci, nullptr, &smp));
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 18},
                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 53306},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 53306}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 3;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = ps;
    VkDescriptorPool dp;
    CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
    const VkDescriptorSetLayout al[3] = {dsl0, dsl1, dsl4};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dp;
    dai.descriptorSetCount = 3;
    dai.pSetLayouts = al;
    VkDescriptorSet ds[3];
    CK(vkAllocateDescriptorSets(dev, &dai, ds));
    std::vector<VkDescriptorImageInfo> smps(18, VkDescriptorImageInfo{smp, VK_NULL_HANDLE,
                                                                      VK_IMAGE_LAYOUT_UNDEFINED});
    std::vector<VkDescriptorImageInfo> ims(64, VkDescriptorImageInfo{
                                                   VK_NULL_HANDLE, grid_v,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkDescriptorBufferInfo bc{cam, 0, 2048}, bd{dyn, 0, 256};
    VkDescriptorImageInfo wo{VK_NULL_HANDLE, walkout_v, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[5]{};
    for (auto &x : w) {
        x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        x.descriptorCount = 1;
    }
    w[0].dstSet = ds[0], w[0].dstBinding = 4, w[0].descriptorCount = 18;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, w[0].pImageInfo = smps.data();
    w[1].dstSet = ds[0], w[1].dstBinding = 5, w[1].descriptorCount = 64;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[1].pImageInfo = ims.data();
    w[2].dstSet = ds[1], w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    w[2].pBufferInfo = &bc;
    w[3].dstSet = ds[2], w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    w[3].pBufferInfo = &bd;
    w[4].dstSet = ds[0], w[4].dstBinding = 6, w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    w[4].pImageInfo = &wo;
    vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);

    // X4's walk (the layer swaps in its march) and X4's blend (the layer patches it).
    const VkShaderModule walk = module(argv[1]), vert = module(argv[2]), frag = module(argv[3]);
    if (!walk || !vert || !frag) {
        printf("FAIL modules\n");
        return 1;
    }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, walk, "main", nullptr};
    cpi.layout = pl;
    VkPipeline cpipe;
    CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &cpipe));
    // X4's preparation, stood in by volume_point when a fourth module is given (after take
    // 495): dispatched before the walk, as X4 does.
    VkPipeline ppipe = VK_NULL_HANDLE;
    if (argc > 4) {
        const VkShaderModule pm = module(argv[4]);
        if (!pm) {
            printf("FAIL modules\n");
            return 1;
        }
        cpi.stage.module = pm;
        CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &ppipe));
    }
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    VkRenderPass rp;
    CK(vkCreateRenderPass(dev, &rpi, nullptr, &rp));
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = rp;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &colour_v;
    fbi.width = fbi.height = 64;
    fbi.layers = 1;
    VkFramebuffer fb;
    CK(vkCreateFramebuffer(dev, &fbi, nullptr, &fb));
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, 64, 64, 0, 1};
    VkRect2D sc{{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    vps.pViewports = &vp;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = st;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.layout = pl;
    gpi.renderPass = rp;
    VkPipeline gpipe;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &gpipe));

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = fam;
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cmd, &bi));
    // X4's grid stand-in: filled with 0.5, ready to sample.
    VkImageMemoryBarrier gb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    gb.srcQueueFamilyIndex = gb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    gb.image = grid;
    gb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    gb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    gb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    VkImageMemoryBarrier wb = gb;
    wb.image = walkout;
    wb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    wb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &wb);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &gb);
    VkClearColorValue half{};
    const float gv = getenv("SMOKE_GRID") ? (float)atof(getenv("SMOKE_GRID")) : 0.5f;
    half.float32[0] = half.float32[1] = half.float32[2] = half.float32[3] = gv;
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, grid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &half, 1, &all);
    gb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    gb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    gb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    gb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &gb);
    // X4's walk, at X4's size. Its sets before its pipeline, as in X4's frames, where sets
    // 0-4 are long bound by then: binding set 0 for the first time AFTER the layer has
    // bound its own set at the pipeline would, by Vulkan's compatibility rule, disturb it.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, ds, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 4, 1, &ds[2], 0, nullptr);
    if (ppipe) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ppipe);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier pmb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        pmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        pmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &pmb, 0, nullptr, 0,
                             nullptr);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
    vkCmdDispatch(cmd, 22, 22, 1);
    // X4's blend.
    VkClearValue cv{};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = sc;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, ds, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 4, 1, &ds[2], 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy rg{};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {64, 64, 1};
    vkCmdCopyImageToBuffer(cmd, colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &rg);
    CK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));

    // The blended picture's green: X4's grid alone would give 0.3 * 0.5 + 0.5 = 0.65, no
    // fog at all 0.3 (the stand-in's colour) with no spread.
    auto stats = [&](const char *pre) {
        const uint16_t *px = (const uint16_t *)prb;
        bool finite = true;
        double sum = 0, sum2 = 0;
        for (int i = 0; i < 64 * 64; i++) {
            const float g = h2f(px[i * 4 + 1]);
            if (!std::isfinite(g))
                finite = false;
            sum += g;
            sum2 += (double)g * g;
        }
        const double mean = sum / 4096.0,
                     spread = std::sqrt(std::fmax(0.0, sum2 / 4096.0 - mean * mean));
        printf("%sFINITE=%d\n%sMEAN=%.4f\n%sSPREAD=%.4f\n%sOURS=%d\n", pre, finite ? 1 : 0, pre,
               mean, pre, spread, pre, (std::fabs(mean - 0.65) > 0.02 || spread > 0.005) ? 1 : 0);
    };
    stats("");
    if (frames3) {
        // A headless swapchain, its images ready to present.
        auto csurf = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(
            inst, "vkCreateHeadlessSurfaceEXT");
        if (!csurf) {
            printf("FAIL no VK_EXT_headless_surface\n");
            return 1;
        }
        VkHeadlessSurfaceCreateInfoEXT hsi{};
        hsi.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        VkSurfaceKHR surf;
        CK(csurf(inst, &hsi, nullptr, &surf));
        uint32_t nf = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> sf(nf);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, sf.data());
        VkSurfaceCapabilitiesKHR caps;
        CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps));
        VkSwapchainCreateInfoKHR sci2{};
        sci2.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci2.surface = surf;
        sci2.minImageCount = std::max(2u, caps.minImageCount);
        sci2.imageFormat = sf[0].format;
        sci2.imageColorSpace = sf[0].colorSpace;
        sci2.imageExtent = {64, 64};
        sci2.imageArrayLayers = 1;
        sci2.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci2.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        sci2.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci2.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci2.clipped = VK_TRUE;
        VkSwapchainKHR swc;
        CK(vkCreateSwapchainKHR(dev, &sci2, nullptr, &swc));
        uint32_t ni = 0;
        vkGetSwapchainImagesKHR(dev, swc, &ni, nullptr);
        std::vector<VkImage> sims(ni);
        vkGetSwapchainImagesKHR(dev, swc, &ni, sims.data());
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence af;
        CK(vkCreateFence(dev, &fci, nullptr, &af));
        // Acquire, bring that image to the present layout (nothing is drawn in it), present.
        auto present = [&]() -> int {
            uint32_t idx = 0;
            CK(vkAcquireNextImageKHR(dev, swc, UINT64_MAX, VK_NULL_HANDLE, af, &idx));
            CK(vkWaitForFences(dev, 1, &af, VK_TRUE, UINT64_MAX));
            CK(vkResetFences(dev, 1, &af));
            VkCommandBuffer pc;
            CK(vkAllocateCommandBuffers(dev, &cai, &pc));
            CK(vkBeginCommandBuffer(pc, &bi));
            VkImageMemoryBarrier pb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            pb.image = sims[idx];
            pb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            pb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pb.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(pc, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &pb);
            CK(vkEndCommandBuffer(pc));
            VkSubmitInfo psi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            psi.commandBufferCount = 1;
            psi.pCommandBuffers = &pc;
            CK(vkQueueSubmit(q, 1, &psi, VK_NULL_HANDLE));
            CK(vkQueueWaitIdle(q));
            VkPresentInfoKHR ppi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            ppi.swapchainCount = 1;
            ppi.pSwapchains = &swc;
            ppi.pImageIndices = &idx;
            CK(vkQueuePresentKHR(q, &ppi));
            return 0;
        };
        // Frames 2 and 3: X4's blend, and no walk -- a sector with no fog.
        VkCommandBuffer c2;
        CK(vkAllocateCommandBuffers(dev, &cai, &c2));
        CK(vkBeginCommandBuffer(c2, &bi));
        vkCmdBeginRenderPass(c2, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindDescriptorSets(c2, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, ds, 0, nullptr);
        vkCmdBindDescriptorSets(c2, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 4, 1, &ds[2], 0, nullptr);
        vkCmdBindPipeline(c2, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
        vkCmdDraw(c2, 3, 1, 0, 0);
        vkCmdEndRenderPass(c2);
        vkCmdCopyImageToBuffer(c2, colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &rg);
        CK(vkEndCommandBuffer(c2));
        if (present())
            return 1;
        si.pCommandBuffers = &c2;
        for (int f = 2; f <= 3; f++) {
            CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
            CK(vkQueueWaitIdle(q));
            if (f == 2 && present())
                return 1;
        }
        stats("F3_");
    }
    vkDeviceWaitIdle(dev);
    return 0;
}
