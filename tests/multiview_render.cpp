// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Does a draw through the layer actually reach BOTH array layers?
//
// Gates 0-2 established that the passes are masked, the framebuffers carry
// 2-layer array views and 570 of X4's pipelines were built against masked
// passes -- and layer 1 is still black in game. Everything required is
// present, so the next question is whether draw replication works at all on
// this driver through this layer, and that does not need X4 to answer.
//
// This does what X4 does, in miniature: creates a colour target the layer will
// double, a render pass the layer will mask, a pipeline against that pass, and
// draws one full-screen triangle. Then it reads both layers back and compares.
// No stereo -- the shader is view-independent, so with a working multiview the
// two layers must come out identical and non-empty.
//
// Prints KEY=VALUE lines for the runner.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL=%s:%d\n", #x, r_); return 1; } } while (0)

static std::vector<uint32_t> load_spv(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> v((size_t)n / 4);
    if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

int main(int argc, char **argv) {
    const char *vs_path = argc > 1 ? argv[1] : "tests/fullscreen.vert.spv";
    const char *fs_path = argc > 2 ? argv[2] : "tests/solid.frag.spv";

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X4";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> phys(n);
    CHECK(vkEnumeratePhysicalDevices(inst, &n, phys.data()));
    if (!n) { printf("FAIL=no_device:0\n"); return 1; }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qn, qf.data());
    uint32_t gfx = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }
    if (gfx == UINT32_MAX) { printf("FAIL=no_graphics_queue:0\n"); return 1; }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = gfx;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    VkDevice dev;
    CHECK(vkCreateDevice(phys[0], &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    const VkFormat FMT = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Deliberately larger than the 64x64 patch the layer's probe used to copy.
    // While the image was exactly 64x64 the two were indistinguishable, and a
    // probe that silently sampled a corner passed this suite while reporting
    // "identical" for two blank regions in the game.
    const uint32_t W = 128, H = 128;
    const VkDeviceSize LAYER_BYTES = (VkDeviceSize)W * H * 8;

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys[0], &mp);
    auto pick = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    };

    // The colour target. Asked for with one layer; the layer doubles it.
    VkImageCreateInfo imgci{};
    imgci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgci.imageType = VK_IMAGE_TYPE_2D;
    imgci.format = FMT;
    imgci.extent = {W, H, 1};
    imgci.mipLevels = 1;
    imgci.arrayLayers = 1;
    imgci.samples = VK_SAMPLE_COUNT_1_BIT;
    imgci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    CHECK(vkCreateImage(dev, &imgci, nullptr, &img));

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    const uint32_t layers = (uint32_t)(req.size / LAYER_BYTES);
    printf("LAYERS_IMPLIED=%u\n", layers);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = pick(req.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CHECK(vkBindImageMemory(dev, img, mem, 0));

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = FMT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView view;
    CHECK(vkCreateImageView(dev, &vci, nullptr, &view));

    // Cleared to black, then drawn over -- so "layer holds the draw colour"
    // and "layer was never touched" are different results, not the same one.
    VkAttachmentDescription att{};
    att.format = FMT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));

    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = W;
    fbci.height = H;
    fbci.layers = 1;
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &fb));

    auto vs = load_spv(vs_path), fs = load_spv(fs_path);
    if (vs.empty() || fs.empty()) { printf("FAIL=shaders_missing:0\n"); return 1; }
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = vs.size() * 4;
    smci.pCode = vs.data();
    VkShaderModule vsm, fsm;
    CHECK(vkCreateShaderModule(dev, &smci, nullptr, &vsm));
    smci.codeSize = fs.size() * 4;
    smci.pCode = fs.data();
    CHECK(vkCreateShaderModule(dev, &smci, nullptr, &fsm));

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vsm;
    st[0].pName = "main";
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fsm;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, (float)W, (float)H, 0, 1};
    VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = st;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.layout = pl;
    gp.renderPass = rp;   // the layer masked this; the pipeline inherits it
    gp.subpass = 0;
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr,
                                    &pipe));

    // Readback buffer: both layers, back to back.
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = LAYER_BYTES * 2; // sized for both; only `layers` are copied
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements breq{};
    vkGetBufferMemoryRequirements(dev, buf, &breq);
    VkMemoryAllocateInfo bmai{};
    bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bmai.allocationSize = breq.size;
    bmai.memoryTypeIndex = pick(breq.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &bmai, nullptr, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = gfx;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));

    VkClearValue clear{};
    clear.color.float32[0] = 0.0f;
    clear.color.float32[1] = 0.0f;
    clear.color.float32[2] = 0.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea = sc;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Both layers out. If the image was never doubled this copy is invalid
    // and validation says so; if it was doubled but never rendered, layer 1
    // comes back as the clear colour or as nothing at all.
    // Only read layers that exist. Reading layer 1 of a single-layer image is
    // out of range: without validation the driver quietly hands back layer 0
    // again, and the control then "passes" by reporting the drawn layer twice.
    for (uint32_t layer = 0; layer < layers && layer < 2; layer++) {
        VkBufferImageCopy c{};
        c.bufferOffset = LAYER_BYTES * layer;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.mipLevel = 0;
        c.imageSubresource.baseArrayLayer = layer;
        c.imageSubresource.layerCount = 1;
        c.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buf, 1, &c);
    }
    CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    void *ptr = nullptr;
    CHECK(vkMapMemory(dev, bmem, 0, LAYER_BYTES * 2, 0, &ptr));
    const uint16_t *l0 = (const uint16_t *)ptr;
    const uint16_t *l1 = (const uint16_t *)((const char *)ptr + LAYER_BYTES);
    // half-float 1.0 is 0x3C00; the shader writes (1,0,0,1) so red is non-zero
    // in any sane encoding. Just ask whether anything was written.
    auto nonzero = [](const uint16_t *p, size_t words) {
        for (size_t i = 0; i < words; i += 4)
            if (p[i] != 0) return true;
        return false;
    };
    const size_t words = (size_t)LAYER_BYTES / 2;
    printf("LAYER0_DRAWN=%d\n", nonzero(l0, words) ? 1 : 0);
    if (layers >= 2) {
        printf("LAYER1_DRAWN=%d\n", nonzero(l1, words) ? 1 : 0);
        printf("LAYERS_IDENTICAL=%d\n",
               memcmp(l0, l1, (size_t)LAYER_BYTES) == 0 ? 1 : 0);
    } else {
        printf("LAYER1_DRAWN=absent\n");
        printf("LAYERS_IDENTICAL=absent\n");
    }
    vkUnmapMemory(dev, bmem);

    // ---- stage 2: read the target back the way X4 does -------------------
    //
    // Everything above measures the *write* path, and eight live runs have now
    // agreed that it works. What none of them could settle is whether the
    // layer's gate-2 redirect actually delivers layer 1 to a shader, because
    // the only instrument for that was the redirect itself.
    //
    // So sample the doubled image through an ordinary combined image sampler,
    // which is the descriptor the redirect rewrites, and render the result
    // into a separate LDR target. The LDR format keeps this second pass
    // unmasked -- exactly X4's shape, where the per-eye chain is consumed by
    // passes that are not themselves per-eye. Whatever comes out is a direct
    // readout of which layer the descriptor ended up naming.
    const VkFormat OFMT = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize OBYTES = (VkDeviceSize)W * H * 4;
    VkImageCreateInfo oci = imgci;
    oci.format = OFMT;
    oci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage oimg;
    CHECK(vkCreateImage(dev, &oci, nullptr, &oimg));
    VkMemoryRequirements oreq{};
    vkGetImageMemoryRequirements(dev, oimg, &oreq);
    VkMemoryAllocateInfo omai{};
    omai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    omai.allocationSize = oreq.size;
    omai.memoryTypeIndex =
        pick(oreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory omem;
    CHECK(vkAllocateMemory(dev, &omai, nullptr, &omem));
    CHECK(vkBindImageMemory(dev, oimg, omem, 0));

    VkImageViewCreateInfo ovci{};
    ovci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ovci.image = oimg;
    ovci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ovci.format = OFMT;
    ovci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView oview;
    CHECK(vkCreateImageView(dev, &ovci, nullptr, &oview));

    VkAttachmentDescription oatt{};
    oatt.format = OFMT;
    oatt.samples = VK_SAMPLE_COUNT_1_BIT;
    oatt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    oatt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    oatt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    oatt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    oatt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    oatt.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference oref{};
    oref.attachment = 0;
    oref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription osub{};
    osub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    osub.colorAttachmentCount = 1;
    osub.pColorAttachments = &oref;
    VkRenderPassCreateInfo orpci{};
    orpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    orpci.attachmentCount = 1;
    orpci.pAttachments = &oatt;
    orpci.subpassCount = 1;
    orpci.pSubpasses = &osub;
    VkRenderPass orp;
    CHECK(vkCreateRenderPass(dev, &orpci, nullptr, &orp));

    VkFramebufferCreateInfo ofbci{};
    ofbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ofbci.renderPass = orp;
    ofbci.attachmentCount = 1;
    ofbci.pAttachments = &oview;
    ofbci.width = W;
    ofbci.height = H;
    ofbci.layers = 1;
    VkFramebuffer ofb;
    CHECK(vkCreateFramebuffer(dev, &ofbci, nullptr, &ofb));

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    VkSampler samp;
    CHECK(vkCreateSampler(dev, &sci, nullptr, &samp));

    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding = 0;
    dslb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb.descriptorCount = 1;
    dslb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &dslb;
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    VkDescriptorPoolSize dps{};
    dps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool));
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));

    // The write the redirect intercepts. It names the target's own view --
    // layer 0 as far as this program is concerned -- and the layer is free to
    // substitute a view onto layer 1 underneath us. Issued after the first
    // framebuffer exists, because that is when the layer learns the image is
    // rendered by a masked pass and becomes willing to redirect it.
    VkDescriptorImageInfo dii{};
    dii.sampler = samp;
    dii.imageView = view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = dset;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);

    std::vector<uint32_t> fs2_code = load_spv(argc > 3 ? argv[3]
                                                       : "tests/sample.frag.spv");
    if (fs2_code.empty()) { printf("FAIL=no_sample_frag:0\n"); return 1; }
    VkShaderModuleCreateInfo smci2{};
    smci2.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci2.codeSize = fs2_code.size() * 4;
    smci2.pCode = fs2_code.data();
    VkShaderModule fsm2;
    CHECK(vkCreateShaderModule(dev, &smci2, nullptr, &fsm2));

    VkPipelineLayoutCreateInfo plci2{};
    plci2.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci2.setLayoutCount = 1;
    plci2.pSetLayouts = &dsl;
    VkPipelineLayout pl2;
    CHECK(vkCreatePipelineLayout(dev, &plci2, nullptr, &pl2));

    VkPipelineShaderStageCreateInfo st2[2]{};
    st2[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st2[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st2[0].module = vsm;
    st2[0].pName = "main";
    st2[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st2[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st2[1].module = fsm2;
    st2[1].pName = "main";
    VkGraphicsPipelineCreateInfo gpci2 = gp;
    gpci2.pStages = st2;
    gpci2.layout = pl2;
    gpci2.renderPass = orp;
    VkPipeline pipe2;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci2, nullptr,
                                    &pipe2));

    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    // Both layers to SHADER_READ_ONLY, so this says nothing about whether a
    // narrow barrier would have been enough. That is a separate question and
    // this test must not accidentally answer it.
    VkImageMemoryBarrier imb{};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = img;
    imb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &imb);

    VkRenderPassBeginInfo orpbi = rpbi;
    orpbi.renderPass = orp;
    orpbi.framebuffer = ofb;
    vkCmdBeginRenderPass(cmd, &orpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2, 0, 1,
                            &dset, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy oc{};
    oc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    oc.imageSubresource.layerCount = 1;
    oc.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, oimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf,
                           1, &oc);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    CHECK(vkMapMemory(dev, bmem, 0, OBYTES, 0, &ptr));
    const uint8_t *o = (const uint8_t *)ptr;
    bool sampled = false;
    for (VkDeviceSize i = 0; i < OBYTES; i += 4)
        if (o[i] != 0) { sampled = true; break; }
    printf("SAMPLED_NONZERO=%d\n", sampled ? 1 : 0);
    vkUnmapMemory(dev, bmem);

    vkDestroyPipeline(dev, pipe2, nullptr);
    vkDestroyPipelineLayout(dev, pl2, nullptr);
    vkDestroyShaderModule(dev, fsm2, nullptr);
    vkDestroyDescriptorPool(dev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    vkDestroySampler(dev, samp, nullptr);
    vkDestroyFramebuffer(dev, ofb, nullptr);
    vkDestroyRenderPass(dev, orp, nullptr);
    vkDestroyImageView(dev, oview, nullptr);
    vkDestroyImage(dev, oimg, nullptr);
    vkFreeMemory(dev, omem, nullptr);

    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyPipelineLayout(dev, pl, nullptr);
    vkDestroyShaderModule(dev, vsm, nullptr);
    vkDestroyShaderModule(dev, fsm, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, bmem, nullptr);
    vkDestroyFramebuffer(dev, fb, nullptr);
    vkDestroyRenderPass(dev, rp, nullptr);
    vkDestroyImageView(dev, view, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
