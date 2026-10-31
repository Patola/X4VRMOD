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

// X4's per-object block, the one that carries the live shadow cascade.
static constexpr uint32_t kObjectSetIdx = 3;

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
    // VK_KHR_surface, so the layer's "which WSIs could this process use" line
    // has something to report. It is loader-implemented and needs no display,
    // so this stays a headless test -- but if a system somehow lacks it, fall
    // back to no extensions rather than failing every case in the file.
    const char *surf_ext = "VK_KHR_surface";
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = &surf_ext;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        ici.enabledExtensionCount = 0;
        ici.ppEnabledExtensionNames = nullptr;
        CHECK(vkCreateInstance(&ici, nullptr, &inst));
    }

    // What vkGetInstanceProcAddr answers for the surface constructors.
    //
    // The layer hooks these to record which WSI built a surface, and an app is
    // entitled to choose its backend by asking for one and reading the null.
    // So the answer must be byte-for-byte what it would be without us: the
    // run-multiview-render.sh case compares these lines with the layer on and
    // off. The bogus name is the control -- if the loader answered everything
    // the comparison would pass while proving nothing.
    {
        static const char *const kNames[] = {
            "vkCreateWaylandSurfaceKHR",
            "vkCreateXcbSurfaceKHR",
            "vkCreateXlibSurfaceKHR",
            "vkDestroySurfaceKHR",
            "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
            "vkGetPhysicalDeviceSurfaceSupportKHR",
            "vkNoSuchFunctionX4VR",
        };
        for (const char *name : kNames)
            printf("GIPA_%s=%d\n", name,
                   vkGetInstanceProcAddr(inst, name) ? 1 : 0);
    }

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
    // descriptorBindingPartiallyBound, for the bindless table at set 0
    // binding 0. That table declares eight slots and writes four, which is
    // X4's shape in miniature -- X4's real table is 53306 textures and cannot
    // possibly have every element written. Without this feature the harness
    // was making a promise it does not keep: an ordinary array binding says
    // every descriptor in it is valid, and validation duly reported
    // VUID-vkCmdDraw-None-08114 on element 4 in every run where nothing
    // mirrored into the top half.
    //
    // The error was real as a statement about the binding's declared contract
    // and false as a statement about behaviour -- `src` is a plain sampler2D,
    // so it reaches element 0 and nothing else. Enabling the feature makes the
    // declaration match both the shader and X4.
    // The INDIVIDUAL promoted struct, not VkPhysicalDeviceVulkan12Features.
    // The layer under test appends VkPhysicalDeviceMultiviewFeatures and
    // VkPhysicalDeviceTimelineSemaphoreFeatures to this very chain, and the
    // aggregate 1.2 struct may not coexist with either -- both carry the
    // timeline-semaphore feature, so the spec forbids the pair outright
    // (VUID-VkDeviceCreateInfo-pNext-02830). Using the aggregate here traded
    // one validation error for another. Matching the layer's own convention is
    // the only choice that is clean whether or not the layer is loaded.
    VkPhysicalDeviceDescriptorIndexingFeatures fdi{};
    fdi.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    fdi.descriptorBindingPartiallyBound = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &fdi;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    VkDevice dev;
    CHECK(vkCreateDevice(phys[0], &dci, nullptr, &dev));

    // X4VR_TEST_EARLY_SHADER: compile a throwaway module BEFORE
    // vkGetDeviceQueue, which is X4's real order and the one that broke takes
    // 166b and 167b.
    //
    // This harness naturally calls vkGetDeviceQueue first, so the layer's VR
    // session thread is already running by the time any shader arrives — and
    // that is precisely the case the off-axis latch does NOT have to cope
    // with. Without this, tests/run-offaxis-bringup.sh passed with the fix
    // deleted: it was measuring a code path X4 never takes.
    //
    // Off by default, so every other case in this file keeps the ordering it
    // has always had.
    // The value may name a module of its own. The pipeline below still uses
    // vs_path, so an arbitrary X4 module can be pushed through
    // vkCreateShaderModule -- which is where classification and every patch
    // happen -- without having to pair it with a compatible fragment stage.
    // "1" (or empty) keeps the original behaviour of reusing vs_path.
    if (const char *es = getenv("X4VR_TEST_EARLY_SHADER"); es && *es) {
        auto early = load_spv((es[0] == '1' && es[1] == '\0') ? vs_path : es);
        if (!early.empty()) {
            VkShaderModuleCreateInfo smci{};
            smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smci.codeSize = early.size() * 4;
            smci.pCode = early.data();
            VkShaderModule early_mod = VK_NULL_HANDLE;
            if (vkCreateShaderModule(dev, &smci, nullptr, &early_mod) ==
                VK_SUCCESS)
                vkDestroyShaderModule(dev, early_mod, nullptr);
        }
    }

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

    // X4VR_TEST_XRP: a render pass COMPATIBLE with `rp` but a different object,
    // so a pipeline created against `rp` can be bound inside it. That is X4's
    // actual shape and the reason take 192 answered the wrong question: a
    // pipeline records the pass it was CREATED against, and Vulkan lets it be
    // used with any compatible one. X4 creates against rp #26/#28/#30 and draws
    // through #27/#29/#31, so a creation-keyed inventory names three passes
    // that never execute and misses the three that do.
    //
    // Identical create-info, hence compatible, hence the framebuffer built for
    // `rp` is legal here too.
    VkRenderPass rp_alias = VK_NULL_HANDLE;
    const char *xrp_env = getenv("X4VR_TEST_XRP");
    const bool want_xrp = xrp_env && *xrp_env && *xrp_env != '0';
    if (want_xrp)
        CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp_alias));

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
    // Stage 2 re-records this buffer for the second submission, which needs
    // the pool to permit individual resets. Without the flag the suite still
    // passed while emitting a validation error -- and a suite that is not
    // validation-clean cannot be used to clear a patch of validation errors.
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
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

    // **X4VR_TEST_FRAMES — read once, used by BOTH recordings.**
    //
    // The first version wrapped only the output pass at the bottom of this
    // file, and that was not enough: this command buffer holds rp #0 and the
    // other early passes, so those ended exactly once no matter what N said.
    // The named-pass round-robin could then advance past a dead target and
    // find its live one already over. Every pass in the frame has to recur,
    // or "N frames" is only true of the tail of the frame.
    const char *frames_env = getenv("X4VR_TEST_FRAMES");
    long frames = frames_env && *frames_env ? atol(frames_env) : 1;
    if (frames < 1) frames = 1;
    if (frames > 512) frames = 512;

    // Idempotent by construction: the pass clears and redraws, and the copy
    // writes the same buffer offsets, so frame N leaves exactly what frame 0
    // did. That is what makes re-running safe rather than merely tolerated.
    // Declared OUTSIDE the loop: both outlive it. `orpbi` is derived from
    // `rpbi` further down, and `si` is reused by the later submits.
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
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    for (long frame = 0; frame < frames; frame++) {
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // The same pipeline, bound inside the ALIAS pass. A creation-keyed record
    // attributes this draw to `rp`; a bind-time record attributes it to
    // `rp_alias`. The two disagree by construction, which is the point.
    if (rp_alias != VK_NULL_HANDLE) {
        VkRenderPassBeginInfo abi = rpbi;
        abi.renderPass = rp_alias;
        vkCmdBeginRenderPass(cmd, &abi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

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
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    } // X4VR_TEST_FRAMES

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
    //
    // X4VR_TEST_OUT_SRGB switches this target to B8G8R8A8_SRGB, which is how
    // the layer tells X4's tonemap (#103, written by rp #40/#52) apart from
    // the UNORM blit chain that follows it. Same pass, same shader, one format
    // changed -- so a case that flips only this knob isolates the masking
    // predicate from everything else.
    const char *srgb_env = getenv("X4VR_TEST_OUT_SRGB");
    const bool out_srgb = srgb_env && *srgb_env && *srgb_env != '0';
    const VkFormat OFMT =
        out_srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    // Layer 1 of the output is a *rendered* layer only when this second pass is
    // masked, and copying from a layer whose layout was never transitioned is a
    // validation error rather than a wrong number. So the readback below stops
    // at layer 0 unless the pass replicates.
    //
    // The condition restates the layer's own rule -- an SRGB single-attachment
    // pass, with the knob on -- and that duplication is deliberate. If the two
    // ever disagree this copy starts touching an untransitioned layer and
    // validation names it, which beats silently reading undefined memory and
    // reporting it as a result.
    const char *mask_env = getenv("X4VR_MASK_TONEMAP");
    const bool out_masked =
        out_srgb && mask_env && *mask_env && *mask_env != '0';
    const VkDeviceSize OBYTES = (VkDeviceSize)W * H * 4;
    VkImageCreateInfo oci = imgci;
    oci.format = OFMT;
    oci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage oimg;
    CHECK(vkCreateImage(dev, &oci, nullptr, &oimg));
    VkMemoryRequirements oreq{};
    vkGetImageMemoryRequirements(dev, oimg, &oreq);
    // The output is a colour attachment, so the layer doubles it too. When the
    // second pass is masked it therefore renders into both layers -- which is
    // what makes "did the two eyes stay different through the sample?" a
    // question this test can ask at all.
    const uint32_t olayers =
        out_masked ? (uint32_t)(oreq.size / OBYTES) : 1;
    printf("OUT_LAYERS=%u\n", olayers);
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

    // ---- classification probes (rp #2..#5) ------------------------------
    //
    // The layer decides shear and masking from the create-info alone, in
    // classify_unsheared()/classify_per_eye(), so a render pass that is never
    // executed still exercises the whole predicate. That is worth doing here
    // because every pass this harness *renders* is a single colour attachment
    // with no depth, and since take 71 that one shape is unconditionally
    // per-eye. Four mask cases were written against the pre-71 model and had
    // been failing ever since, asserting an outcome no input this file could
    // construct would produce.
    //
    // These four cover the whole matrix instead. No framebuffer and no draws:
    // the assertion is the inventory line, which is all a pure predicate can
    // be asked for.
    {
        const VkFormat DFMT = VK_FORMAT_D32_SFLOAT;
        const VkFormat LDR = VK_FORMAT_R8G8B8A8_UNORM;
        const VkFormat HDR = VK_FORMAT_R16G16B16A16_SFLOAT;
        struct Probe {
            const char *name;
            bool colour;
            VkFormat cfmt;
            bool depth;
        } probes[] = {
            // colour, no depth -> the take-71 rule: always masked, no knob.
            {"fullscreen", true, LDR, false},
            // colour + depth, all-LDR -> the UI/HUD shape. Unsheared, and NOT
            // masked unless a knob asks, which is the carve-out the four stale
            // cases were trying to reach and never could without depth.
            {"ldr-depth", true, LDR, true},
            // colour + depth, HDR -> world geometry. Sheared and per-eye.
            {"world", true, HDR, true},
            // depth only -> a shadow pass. Must stay MONO and *unmasked*: this
            // is the guard that keeps X4's five cascaded shadow maps shared
            // between the eyes. Both eyes must sample the same light-space map,
            // and take 83's fix depends on it -- if this ever starts reporting
            // +MASKED, the shadow maps have gone per-eye and the per-eye
            // shading defect is back.
            {"shadow", false, LDR, true},
            // Appended, never inserted: the serials are positional and the
            // shadow case above is the one that must not be renumbered by
            // accident. HDR with no depth -- a fullscreen post pass, and the
            // case that separates task #30's canvas from everything sharing
            // its shape. X4 has 29 passes like this against one UI pass, and
            // the only thing telling them apart is the LDR test. If
            // subpass_is_canvas() ever loses it, every deferred lighting pass
            // becomes a canvas, and under X4VR_SHEAR_LIGHTS the light volumes
            // are World modules, so they would really take the shift.
            {"hdr-nodepth", true, HDR, false},
        };
        for (const Probe &p : probes) {
            VkAttachmentDescription att[2]{};
            VkAttachmentReference cref{}, dref{};
            uint32_t n = 0;
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            if (p.colour) {
                att[n].format = p.cfmt;
                att[n].samples = VK_SAMPLE_COUNT_1_BIT;
                att[n].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                att[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                cref.attachment = n;
                cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                sub.colorAttachmentCount = 1;
                sub.pColorAttachments = &cref;
                n++;
            }
            if (p.depth) {
                att[n].format = DFMT;
                att[n].samples = VK_SAMPLE_COUNT_1_BIT;
                att[n].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                att[n].finalLayout =
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                dref.attachment = n;
                dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                sub.pDepthStencilAttachment = &dref;
                n++;
            }
            VkRenderPassCreateInfo rpci2{};
            rpci2.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            rpci2.attachmentCount = n;
            rpci2.pAttachments = att;
            rpci2.subpassCount = 1;
            rpci2.pSubpasses = &sub;
            VkRenderPass probe_rp;
            CHECK(vkCreateRenderPass(dev, &rpci2, nullptr, &probe_rp));
            // The serial the layer assigned is positional, so name it here
            // rather than making the test script count render passes.
            printf("PROBE_PASS=%s\n", p.name);
            vkDestroyRenderPass(dev, probe_rp, nullptr);
        }
    }

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

    // X4VR_TEST_ARRAY_SAMPLER switches the descriptor from a plain 2D view of
    // layer 0 to a 2D_ARRAY view spanning both layers. It goes together with a
    // fragment shader put through patch_fragment_view_layer -- the shader
    // declares sampler2DArray, so the view bound to it has to be an array view
    // or the two disagree about what they are looking at, which is a
    // validation error and undefined behaviour rather than a wrong picture.
    //
    // The pair is the whole point of the case: the shader patch alone changes
    // nothing, because a 2D view has no layer 1 to reach.
    const char *arr_env = getenv("X4VR_TEST_ARRAY_SAMPLER");
    const bool array_sampler = arr_env && *arr_env && *arr_env != '0';
    VkImageView aview = VK_NULL_HANDLE;
    if (array_sampler) {
        VkImageViewCreateInfo avci = vci;
        avci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        // However many layers there turned out to be. Asking for 2 when the
        // layer is off would just fail here and hide the real result behind a
        // setup error.
        avci.subresourceRange.layerCount = layers;
        CHECK(vkCreateImageView(dev, &avci, nullptr, &aview));
    }

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    VkSampler samp;
    CHECK(vkCreateSampler(dev, &sci, nullptr, &samp));

    // A four-element descriptor array, not a single descriptor, for two reasons
    // that both bit in take twenty-one. It makes the layout carry a descriptor
    // array, so the survey has to attribute writes to a *layout* rather than to a
    // bare binding number; and it makes one vkUpdateDescriptorSets carry four
    // descriptors, exercising the dstArrayElement + j walk that X4 uses for
    // every write and that a single-descriptor test never touched. The shader
    // declares a plain sampler2D and so reads element 0; the rest exist to give
    // the reported extent something to be wrong about.
    // Declared eight, written four, so the top half is room for the mirror's
    // twin region: with X4VR_MIRROR_OFFSET=4 the twins land at 4..7 and element
    // 4 is the twin of element 0. That is what sample_twin.frag reads. It also
    // mirrors X4's real shape -- a table far larger than the prefix in use.
    const uint32_t kSlots = 8;
    const uint32_t kWritten = 4;
    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding = 0;
    dslb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb.descriptorCount = kSlots;
    dslb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // PARTIALLY_BOUND: elements kWritten..kSlots-1 are legitimately unwritten
    // unless the mirror fills them, and no shader here reaches them. This is
    // the declaration X4 must also make for its 53306-entry table, so the
    // harness is closer to X4's shape with it than without.
    const VkDescriptorBindingFlags dbf =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo dbfci{};
    dbfci.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    dbfci.bindingCount = 1;
    dbfci.pBindingFlags = &dbf;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.pNext = &dbfci;
    dslci.bindingCount = 1;
    dslci.pBindings = &dslb;
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl));

    // X4VR_TEST_VIEW_ARENA -- task #53's view arena, off by default.
    //
    // The layer's per-camera M_view report had no way to run headless: nothing
    // in this harness ever created a range-1792 uniform buffer at set 1, so
    // patch_view_before_submit() returned at its first line and the whole
    // report was unexercised code. Everything below is behind the flag and the
    // default path is byte-identical to before, because this file pins about a
    // hundred assertions that have nothing to do with #53.
    const bool view_arena = getenv("X4VR_TEST_VIEW_ARENA") != nullptr;
    // X4VR_TEST_OBJ_BLOCK -- task #56's OBJECT block, at set 3.
    //
    // The cascade the shaders actually sample is M_shadowCSM0, member 3 of the
    // WORLD/object block at set 3 binding 0 (189 of the dumped modules read
    // it). The camera block's M_shadowCSM0Clip at member 9 is declared by 370
    // and read by ZERO, and the layer used to report it -- which is why takes
    // 197 and 201, identical runs, disagreed.
    //
    // Requires the arena, because the shadow SCALARS are genuinely camera-block
    // members and are still read from there.
    const bool obj_block =
        view_arena && getenv("X4VR_TEST_OBJ_BLOCK") != nullptr;
    VkDescriptorSetLayout vdsl = VK_NULL_HANDLE;
    VkBuffer objbuf = VK_NULL_HANDLE;
    VkDeviceMemory objmem = VK_NULL_HANDLE;
    VkDescriptorSet objset = VK_NULL_HANDLE;
    if (view_arena) {
        VkDescriptorSetLayoutBinding vb{};
        vb.binding = 0;
        vb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        vb.descriptorCount = 1;
        vb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                        VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        vci.bindingCount = 1;
        vci.pBindings = &vb;
        CHECK(vkCreateDescriptorSetLayout(dev, &vci, nullptr, &vdsl));
    }

    VkDescriptorPoolSize dps[2]{};
    dps[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dps[0].descriptorCount = kSlots;
    dps[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    dps[1].descriptorCount = obj_block ? 3 : 2;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = obj_block ? 4 : (view_arena ? 3 : 1);
    dpci.poolSizeCount = view_arena ? 2 : 1;
    dpci.pPoolSizes = dps;
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
    VkDescriptorImageInfo dii[kWritten];
    for (uint32_t i = 0; i < kWritten; i++) {
        dii[i] = {};
        dii[i].sampler = samp;
        dii[i].imageView = array_sampler ? aview : view;
        dii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = dset;
    wds.dstBinding = 0;
    wds.descriptorCount = kWritten;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = dii;
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

    // Two blocks in one buffer, at set 1, range 1792 -- the shape the layer
    // keys on (x4vr_layer.cpp: `if (bi.range != kViewBlockRange) continue`).
    // Block 0 gets an identity M_view, block 1 gets take 178's, so the run
    // exercises BOTH report paths and a pass/fail cannot come from the
    // instrument only ever seeing one kind of camera.
    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    VkDescriptorSet vset[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceSize vstride = 1792;
    if (view_arena) {
        VkPhysicalDeviceProperties pp{};
        vkGetPhysicalDeviceProperties(phys[0], &pp);
        const VkDeviceSize align = pp.limits.minUniformBufferOffsetAlignment;
        if (align > 1)
            vstride = (1792 + align - 1) / align * align;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        // One extra stride under X4VR_TEST_VIEW_REWRITE, so the rewrite can
        // name a region NO other descriptor set does. Pointing it at an
        // existing block makes both walks pick the same winner and leaves the
        // "winner DIFFERS" branch unexercised. Zeros, so every other run is
        // byte-identical.
        bci.size = vstride + 1792 +
                   (getenv("X4VR_TEST_VIEW_REWRITE") ? vstride : 0);
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CHECK(vkCreateBuffer(dev, &bci, nullptr, &vbuf));
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, vbuf, &mr);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys[0], &mp);
        uint32_t mt = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags want =
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & want) == want) {
                mt = i;
                break;
            }
        }
        if (mt == UINT32_MAX) { printf("FAIL=no_host_visible:0\n"); return 1; }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = mt;
        CHECK(vkAllocateMemory(dev, &mai, nullptr, &vmem));
        CHECK(vkBindBufferMemory(dev, vbuf, vmem, 0));
        void *vp = nullptr;
        CHECK(vkMapMemory(dev, vmem, 0, VK_WHOLE_SIZE, 0, &vp));
        // The layer reads the mapped pointer directly, so the contents have to
        // be a plausible X4 block: a reversed-Z projection (so the storage
        // order is detectable and read_proj_terms succeeds) plus the M_view
        // under test. Anything less and the report refuses before it decides.
        float *arena = (float *)vp;
        memset(arena, 0, (size_t)(vstride + 1792));
        for (int b = 0; b < 2; b++) {
            float *blk = arena + (b * vstride) / 4;
            // M_view at float 0
            for (int i = 0; i < 4; i++)
                blk[i * 4 + i] = 1.0f;
            if (b == 1) {
                // take 178's M_view, as logged. Same sixteen floats
                // tests/view_math.cpp pins the decoder against.
                const float v[16] = {0.967f, -0.0f,  0.256f, 0.0f,
                                     -0.0f,  1.0f,   0.0f,   0.0f,
                                     -0.256f, -0.0f, 0.967f, 0.0f,
                                     8.415f, -0.0f,  2.225f, 1.0f};
                memcpy(blk, v, sizeof(v));
            }
            // M_projection / M_projectionUJ: x4_projection(0.889, -1.778, 0.1)
            for (uint32_t off : {16u, 48u}) {
                blk[off + 0] = 0.889f;
                blk[off + 5] = -1.778f;
                blk[off + 11] = 1.0f;
                blk[off + 14] = 0.1f;
            }
            // Task #56's shadow terms, in block 0 ONLY. Members 9/10 are read
            // by zero of the 409 dumped modules, so X4 may well leave them at
            // whatever the buffer held -- and a report that only ever saw
            // zeros would be a branch nobody had run. Block 0 exercises the
            // populated path, block 1 stays zeroed and exercises the caveat.
            // The numbers are chosen to make the arithmetic checkable:
            //   |row0| = 0.002 -> cascade spans 500 m
            //   2048 texels * factor 0.5 = 1024 across that span
            //   -> 1 / (0.002 * 2048 * 0.5) = 0.48828 m per texel
            if (b == 0) {
                // **POISON, since take 201.** These are members 9 and 10 --
                // M_shadowCSM0Clip / M_shadowCSM1Clip -- which 370 dumped
                // modules declare and ZERO sample. The layer must NOT read
                // them. They used to hold 0.002 here, so a report that read
                // the wrong member produced exactly the right answer and the
                // case passed: a green test on a dead member.
                //
                // 0.009 spans 111 m, nothing else in this fixture produces
                // that, so a regression names itself in the failure text.
                blk[144 + 0] = 0.009f;
                blk[144 + 5] = 0.009f;
                blk[144 + 10] = 0.009f;
                blk[144 + 15] = 1.0f;
                blk[160 + 0] = 0.009f;
                blk[160 + 5] = 0.009f;
                blk[160 + 10] = 0.009f;
                blk[160 + 15] = 1.0f;
                blk[232] = 30.0f;        // V_csmthresholds
                blk[233] = 120.0f;
                blk[234] = 500.0f;
                blk[235] = 2000.0f;
                blk[359] = 1.0f;         // F_csm_blendstrength
                blk[360] = 2048.0f;      // F_shadowmapsize
                blk[361] = 8000.0f;      // F_shadowmaxdistance
                for (int i = 0; i < 5; i++)
                    blk[363 + i] = 0.5f; // F_texturefactorCSM0..4
            }
        }
        VkDescriptorSetLayout vl[2] = {vdsl, vdsl};
        VkDescriptorSetAllocateInfo vai{};
        vai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        vai.descriptorPool = dpool;
        vai.descriptorSetCount = 2;
        vai.pSetLayouts = vl;
        CHECK(vkAllocateDescriptorSets(dev, &vai, vset));
        VkDescriptorBufferInfo vbi[2]{};
        VkWriteDescriptorSet vw[2]{};
        for (int b = 0; b < 2; b++) {
            vbi[b].buffer = vbuf;
            vbi[b].offset = b * vstride;
            vbi[b].range = 1792;          // the layer's whole recognition test
            vw[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            vw[b].dstSet = vset[b];
            vw[b].dstBinding = 0;
            vw[b].descriptorCount = 1;
            vw[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            vw[b].pBufferInfo = &vbi[b];
        }
        vkUpdateDescriptorSets(dev, 2, vw, 0, nullptr);

        // X4VR_TEST_VIEW_REWRITE -- task #67, and the only configuration in
        // which the two camera walks can disagree.
        //
        // Written once each, the accumulating and superseding records are
        // identical, so a "LIVESETS ... AGREES" line would prove nothing about
        // the instrument. Re-point vset[0]'s binding 0 at the second block and
        // they must diverge: the accumulating record keeps both regions and
        // credits both on every draw, the superseding one keeps the live one.
        //
        // It re-points at the THIRD region, which nothing else names, so the
        // stale entry at offset 0 keeps vset[0]'s whole draw count to itself.
        // That makes the two walks disagree about the WINNER and not merely
        // the slot count: the accumulating one still picks offset 0, the
        // superseding one picks 2*vstride. Both branches of the comparison
        // then get exercised by the pair of runs.
        //
        // This case asserts slot BOOKKEEPING, not block contents; the third
        // region is zeros and nothing reads it for geometry.
        if (getenv("X4VR_TEST_VIEW_REWRITE")) {
            VkDescriptorBufferInfo rbi = vbi[0];
            rbi.offset = 2 * vstride;
            VkWriteDescriptorSet rw = vw[0];
            rw.pBufferInfo = &rbi;
            vkUpdateDescriptorSets(dev, 1, &rw, 0, nullptr);
        }

        // ---- task #56: the OBJECT block at set 3 -----------------------
        //
        // A DIFFERENT buffer at a DIFFERENT range. 576 rather than 1792 is
        // the point: the layer's camera tracking filters on range == 1792, so
        // a block of any other size is invisible to it, and the object path
        // has to record whatever range the descriptor declares. If this were
        // 1792 the case would pass through the camera path and prove nothing.
        if (obj_block) {
            VkBufferCreateInfo objbci{};
            objbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            objbci.size = 1024;
            objbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            objbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            CHECK(vkCreateBuffer(dev, &objbci, nullptr, &objbuf));
            VkMemoryRequirements objmr{};
            vkGetBufferMemoryRequirements(dev, objbuf, &objmr);
            VkPhysicalDeviceMemoryProperties objmp{};
            vkGetPhysicalDeviceMemoryProperties(phys[0], &objmp);
            uint32_t objmt = UINT32_MAX;
            for (uint32_t i = 0; i < objmp.memoryTypeCount; i++) {
                const VkMemoryPropertyFlags w =
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if ((objmr.memoryTypeBits & (1u << i)) &&
                    (objmp.memoryTypes[i].propertyFlags & w) == w) { objmt = i; break; }
            }
            if (objmt == UINT32_MAX) { printf("FAIL=no_host_visible_obj:0\n"); return 1; }
            VkMemoryAllocateInfo objmai{};
            objmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            objmai.allocationSize = objmr.size;
            objmai.memoryTypeIndex = objmt;
            CHECK(vkAllocateMemory(dev, &objmai, nullptr, &objmem));
            CHECK(vkBindBufferMemory(dev, objbuf, objmem, 0));
            void *objp = nullptr;
            CHECK(vkMapMemory(dev, objmem, 0, VK_WHOLE_SIZE, 0, &objp));
            float *objb = (float *)objp;
            memset(objb, 0, 1024);
            // The LIVE cascade, at member 3 = float 48. The same numbers the
            // camera block used to carry, so the arithmetic the old case
            // asserted is reproduced here -- from the member X4 actually
            // samples. |row0| = 0.002 spans 500 m; 2048 texels at factor 0.5
            // is 1024 across it; 0.48828 m per texel.
            objb[48 + 0] = 0.002f;
            objb[48 + 5] = 0.002f;
            objb[48 + 10] = 0.002f;
            objb[48 + 15] = 1.0f;
            objb[64 + 0] = 0.0005f;   // member 4, a wider cascade
            objb[64 + 5] = 0.0005f;
            objb[64 + 10] = 0.0005f;
            objb[64 + 15] = 1.0f;
            VkDescriptorSetAllocateInfo objai{};
            objai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            objai.descriptorPool = dpool;
            objai.descriptorSetCount = 1;
            objai.pSetLayouts = &vdsl;
            CHECK(vkAllocateDescriptorSets(dev, &objai, &objset));
            VkDescriptorBufferInfo objbi{};
            objbi.buffer = objbuf;
            objbi.offset = 0;
            objbi.range = 576;
            VkWriteDescriptorSet objw{};
            objw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            objw.dstSet = objset;
            objw.dstBinding = 0;
            objw.descriptorCount = 1;
            objw.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            objw.pBufferInfo = &objbi;
            vkUpdateDescriptorSets(dev, 1, &objw, 0, nullptr);

            // X4VR_TEST_OBJ_REWRITE -- take 202's defect, in miniature.
            //
            // X4 re-points set 3 binding 0 at a fresh sub-allocation for every
            // draw: take 202 logged 13748 binds and the layer had accumulated
            // 167 slots, credited ALL of them on every draw, and read the dead
            // ones -- which is where `csm0-scale=inf` and "2 distinct cascade
            // scales" came from. vkUpdateDescriptorSets REPLACES a binding, so
            // after this second write the only correct answer is the second
            // region: |row0| 0.008, 125 m, and ONE distinct scale.
            //
            // The old append-and-dedupe code cannot produce that. It reports
            // two scales and picks between them by draw count, so this case
            // fails closed against a revert.
            if (getenv("X4VR_TEST_OBJ_REWRITE")) {
                const uint32_t base = 256 / 4;   // second region, byte 256
                objb[base + 48 + 0] = 0.008f;
                objb[base + 48 + 5] = 0.008f;
                objb[base + 48 + 10] = 0.008f;
                objb[base + 48 + 15] = 1.0f;
                objb[base + 64 + 0] = 0.002f;
                objb[base + 64 + 5] = 0.002f;
                objb[base + 64 + 10] = 0.002f;
                objb[base + 64 + 15] = 1.0f;
                VkDescriptorBufferInfo objbi2 = objbi;
                objbi2.offset = 256;
                VkWriteDescriptorSet objw2 = objw;
                objw2.pBufferInfo = &objbi2;
                vkUpdateDescriptorSets(dev, 1, &objw2, 0, nullptr);
            }
        }
    }

    // X4VR_TEST_PAD_SETS reproduces the SHAPE of X4's real pipeline layout,
    // which is the thing this harness did not have when it certified #59 and
    // #60 as working. Every layout here declared one or two sets, so the pose
    // channel's index always fit, and take 186b then met X4's main layout --
    // 1109 of its 1135 pipelines -- which declares SIX. The channel collided,
    // the frame was undefined, and every case below had passed.
    //
    // A test that only runs in its simplest configuration certifies the
    // configuration, not the code. This knob exists so the harness can be run
    // at X4's width, and the cases at the bottom of run-multiview-render.sh
    // drive it at 6 with the real index and at 6 against a forced-low one.
    const char *pad_env = getenv("X4VR_TEST_PAD_SETS");
    const uint32_t pad = pad_env ? (uint32_t)atoi(pad_env) : 0;
    VkDescriptorSetLayout pad_dsl = VK_NULL_HANDLE;
    VkDescriptorSetLayout sl2[16] = {dsl, vdsl};
    const uint32_t base2 = view_arena ? 2u : 1u;
    uint32_t count2 = base2;
    // Task #56: reach set 3 with a REAL uniform layout there. PAD_SETS fills
    // the gap with EMPTY layouts, which are fine for width but cannot take a
    // descriptor -- so the object block needs set 2 padded and set 3 declared.
    uint32_t want_sets = pad;
    if (obj_block && want_sets < kObjectSetIdx + 1)
        want_sets = kObjectSetIdx + 1;
    const uint32_t pad_target = want_sets;
    if (pad_target > base2) {
        VkDescriptorSetLayoutCreateInfo eci{};
        eci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        CHECK(vkCreateDescriptorSetLayout(dev, &eci, nullptr, &pad_dsl));
        count2 = pad_target < 16 ? pad_target : 16;
        for (uint32_t i = base2; i < count2; i++)
            sl2[i] = pad_dsl;
        if (obj_block && kObjectSetIdx < count2)
            sl2[kObjectSetIdx] = vdsl;
    }
    VkPipelineLayoutCreateInfo plci2{};
    plci2.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci2.setLayoutCount = count2;
    plci2.pSetLayouts = sl2;
    VkPipelineLayout pl2;
    CHECK(vkCreatePipelineLayout(dev, &plci2, nullptr, &pl2));

    VkPipelineShaderStageCreateInfo st2[2]{};
    // Task #60: with the arena on, the OUTPUT pass gets a vertex shader that
    // declares the camera block, so #33's conjugation has something to attach
    // to. The first pass keeps the plain fullscreen shader -- it binds no set
    // 1, so a shader reading one there would be reading an unbound descriptor.
    VkShaderModule vsm2 = vsm;
    if (view_arena) {
        std::vector<uint32_t> vc2 = load_spv("tests/fullscreen_camera.vert.spv");
        if (vc2.empty()) { printf("FAIL=no_fullscreen_camera_vert\n"); return 1; }
        VkShaderModuleCreateInfo s2{};
        s2.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        s2.codeSize = vc2.size() * 4;
        s2.pCode = vc2.data();
        CHECK(vkCreateShaderModule(dev, &s2, nullptr, &vsm2));
    }
    st2[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st2[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st2[0].module = vsm2;
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

    // A compute pipeline, for the layer's compute bookkeeping. X4 performs at
    // least part of the frame in dispatches, and until now the layer hooked no
    // compute entry point at all -- so a stage done that way left no line in
    // any log. The shader has no resources: what is under test is
    // CreateComputePipelines -> module serial -> CmdBindPipeline -> CmdDispatch
    // -> the counter, and bindings would only add setup the assertion does not
    // rest on.
    VkPipeline cpipe = VK_NULL_HANDLE;
    VkPipelineLayout cpl = VK_NULL_HANDLE;
    VkShaderModule csm = VK_NULL_HANDLE;
    const char *comp_env = getenv("X4VR_TEST_COMPUTE");
    const bool want_compute = comp_env && *comp_env && *comp_env != '0';
    // X4VR_TEST_COMPUTE=fsr: tests/fsr_shape.comp, the shape of X4's FSR 1
    // pass, with real resources -- a uniform buffer, the doubled target
    // sampled, a 64x64 rgba16f storage image written -- so X4VR_COMPLOG's
    // descriptor tracking and dispatch report run on a GPU (take 379).
    const bool want_fsr = want_compute && !strcmp(comp_env, "fsr");
    VkDescriptorSetLayout fdsl = VK_NULL_HANDLE;
    VkDescriptorPool fpool = VK_NULL_HANDLE;
    VkDescriptorSet fset = VK_NULL_HANDLE;
    VkImage fimg = VK_NULL_HANDLE;
    VkImageView fview = VK_NULL_HANDLE;
    VkDeviceMemory fmem = VK_NULL_HANDLE, fbmem = VK_NULL_HANDLE;
    VkBuffer fbuf = VK_NULL_HANDLE;
    if (want_compute) {
        std::vector<uint32_t> cs_code =
            load_spv(want_fsr ? "tests/fsr_shape.comp.spv" : "tests/noop.comp.spv");
        if (cs_code.empty()) { printf("FAIL=no_noop_comp:0\n"); return 1; }
        VkShaderModuleCreateInfo csci{};
        csci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        csci.codeSize = cs_code.size() * 4;
        csci.pCode = cs_code.data();
        CHECK(vkCreateShaderModule(dev, &csci, nullptr, &csm));
        VkPipelineLayoutCreateInfo cplci{};
        cplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        if (want_fsr) {
            VkDescriptorSetLayoutBinding fb[3]{};
            const VkDescriptorType ft[3] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
            for (uint32_t i = 0; i < 3; i++) {
                fb[i].binding = i;
                fb[i].descriptorType = ft[i];
                fb[i].descriptorCount = 1;
                fb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo fdci{};
            fdci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            fdci.bindingCount = 3;
            fdci.pBindings = fb;
            CHECK(vkCreateDescriptorSetLayout(dev, &fdci, nullptr, &fdsl));
            cplci.setLayoutCount = 1;
            cplci.pSetLayouts = &fdsl;

            VkImageCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            fci.imageType = VK_IMAGE_TYPE_2D;
            fci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            fci.extent = {64, 64, 1};
            fci.mipLevels = 1;
            fci.arrayLayers = 1;
            fci.samples = VK_SAMPLE_COUNT_1_BIT;
            fci.tiling = VK_IMAGE_TILING_OPTIMAL;
            fci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            CHECK(vkCreateImage(dev, &fci, nullptr, &fimg));
            VkMemoryRequirements freq{};
            vkGetImageMemoryRequirements(dev, fimg, &freq);
            VkMemoryAllocateInfo fmai{};
            fmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            fmai.allocationSize = freq.size;
            fmai.memoryTypeIndex =
                pick(freq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CHECK(vkAllocateMemory(dev, &fmai, nullptr, &fmem));
            CHECK(vkBindImageMemory(dev, fimg, fmem, 0));
            VkImageViewCreateInfo fvci{};
            fvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            fvci.image = fimg;
            fvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            fvci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            fvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            CHECK(vkCreateImageView(dev, &fvci, nullptr, &fview));

            VkBufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            fbci.size = 64;
            fbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            CHECK(vkCreateBuffer(dev, &fbci, nullptr, &fbuf));
            VkMemoryRequirements fbreq{};
            vkGetBufferMemoryRequirements(dev, fbuf, &fbreq);
            VkMemoryAllocateInfo fbmai{};
            fbmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            fbmai.allocationSize = fbreq.size;
            fbmai.memoryTypeIndex =
                pick(fbreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CHECK(vkAllocateMemory(dev, &fbmai, nullptr, &fbmem));
            CHECK(vkBindBufferMemory(dev, fbuf, fbmem, 0));

            VkDescriptorPoolSize fps[3] = {{ft[0], 1}, {ft[1], 1}, {ft[2], 1}};
            VkDescriptorPoolCreateInfo fpci{};
            fpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            fpci.maxSets = 1;
            fpci.poolSizeCount = 3;
            fpci.pPoolSizes = fps;
            CHECK(vkCreateDescriptorPool(dev, &fpci, nullptr, &fpool));
            VkDescriptorSetAllocateInfo fsai{};
            fsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            fsai.descriptorPool = fpool;
            fsai.descriptorSetCount = 1;
            fsai.pSetLayouts = &fdsl;
            CHECK(vkAllocateDescriptorSets(dev, &fsai, &fset));
            VkDescriptorBufferInfo fbi{fbuf, 0, 64};
            VkDescriptorImageInfo fin{samp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo fout{VK_NULL_HANDLE, fview, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet fw[3]{};
            for (uint32_t i = 0; i < 3; i++) {
                fw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                fw[i].dstSet = fset;
                fw[i].dstBinding = i;
                fw[i].descriptorCount = 1;
                fw[i].descriptorType = ft[i];
            }
            fw[0].pBufferInfo = &fbi;
            fw[1].pImageInfo = &fin;
            fw[2].pImageInfo = &fout;
            vkUpdateDescriptorSets(dev, 3, fw, 0, nullptr);
        }
        CHECK(vkCreatePipelineLayout(dev, &cplci, nullptr, &cpl));
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = csm;
        cpci.stage.pName = "main";
        cpci.layout = cpl;
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                       &cpipe));
    }

    // **X4VR_TEST_FRAMES: RE-RECORD and re-submit N times.**
    //
    // Re-submitting a pre-recorded buffer is not enough, and finding that out
    // is the whole reason this exists. The layer's pass-end hook is
    // vkCmdEndRenderPass -- a RECORDING command. Submitting the same buffer
    // eight times runs the GPU work eight times and calls the layer's hook
    // ONCE, so every frame-counting path in the layer still saw a single
    // frame. The loop therefore encloses the recording, not just the submit.
    //
    // This is what makes the named-pass round-robin testable at all: a target
    // abandoned on one frame gets a real pass-end on the next to be captured
    // at. Default 1, so every existing case records and submits exactly as
    // before.
    for (long frame = 0; frame < frames; frame++) {
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

    if (want_compute) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
        if (want_fsr) {
            VkImageMemoryBarrier fbar{};
            fbar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            fbar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            fbar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            fbar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            fbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fbar.image = fimg;
            fbar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &fbar);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cpl, 0, 1,
                                    &fset, 0, nullptr);
            vkCmdDispatch(cmd, 4, 4, 1);
        } else {
            vkCmdDispatch(cmd, 1, 1, 1);
        }
    }

    VkRenderPassBeginInfo orpbi = rpbi;
    orpbi.renderPass = orp;
    orpbi.framebuffer = ofb;
    vkCmdBeginRenderPass(cmd, &orpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe2);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2, 0, 1,
                            &dset, 0, nullptr);
    // Before the FIRST draw, not just before the credit loop: the output pass's
    // vertex shader reads set 1 now, and a draw with it unbound is undefined
    // behaviour rather than a wrong picture.
    if (view_arena)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2, 1, 1,
                                &vset[0], 0, nullptr);
    // Task #56: bound once, before any draw, and left bound. The object block
    // is credited on the same draws as the camera slots, so binding it later
    // would credit it on fewer and make the two counts incomparable.
    if (obj_block)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2,
                                kObjectSetIdx, 1, &objset, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (view_arena) {
        // patch_view_before_submit() refuses unless the winning block carries
        // 50+ draws -- "has X4 populated this yet", in draw counts. So block 0
        // gets 60 and block 1 gets 10: one clears the bar and one does not,
        // which also checks that the per-camera loop reports the loser. The
        // whole point of #53 is that reading only the winner is what hid a
        // non-identity camera for five weeks, so a harness where both blocks
        // are winners would test nothing.
        for (int i = 0; i < 60; i++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2,
                                    1, 1, &vset[0], 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        for (int i = 0; i < 10; i++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2,
                                    1, 1, &vset[1], 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }
    vkCmdEndRenderPass(cmd);

    for (uint32_t layer = 0; layer < olayers && layer < 2; layer++) {
        VkBufferImageCopy oc{};
        oc.bufferOffset = OBYTES * layer;
        oc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        oc.imageSubresource.baseArrayLayer = layer;
        oc.imageSubresource.layerCount = 1;
        oc.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, oimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buf, 1, &oc);
    }
    CHECK(vkEndCommandBuffer(cmd));
    // **X4VR_TEST_FRAMES: submit the recorded frame N times.**
    //
    // Every cadence-driven path in the layer -- the named-pass round-robin, its
    // give-up, the probe sweep -- counts frames, and this harness rendered
    // exactly one. So none of them could ever be observed here: the give-up
    // that take 179 lost a run to was written, shipped, and never once seen to
    // fire, because a single frame can show STARVATION but never RECOVERY.
    //
    // Re-submitting the same command buffer is legal and is what a real frame
    // loop looks like from the layer's side: the same passes end again, with
    // the same serials, so a target abandoned on one frame can be captured on
    // the next. Default 1, so every existing case is byte-identical.
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    if (frame + 1 < frames)
        CHECK(vkQueueWaitIdle(queue));
    } // X4VR_TEST_FRAMES
    CHECK(vkQueueWaitIdle(queue));

    // An image-to-image blit, for the layer's transfer-edge inventory. Placed
    // last, in its own submission, after every readback: a merge performed by
    // a blit leaves no entry in the writer list, so the edge has to be recorded
    // somewhere -- but proving that must not perturb what this test asserts.
    VkImage bimg = VK_NULL_HANDLE;
    VkDeviceMemory bimem = VK_NULL_HANDLE;
    const char *blit_env = getenv("X4VR_TEST_BLIT");
    if (blit_env && *blit_env && *blit_env != '0') {
        VkImageCreateInfo bic = oci;
        bic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(vkCreateImage(dev, &bic, nullptr, &bimg));
        VkMemoryRequirements br;
        vkGetImageMemoryRequirements(dev, bimg, &br);
        VkMemoryAllocateInfo bai{};
        bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bai.allocationSize = br.size;
        bai.memoryTypeIndex =
            pick(br.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(dev, &bai, nullptr, &bimem));
        CHECK(vkBindImageMemory(dev, bimg, bimem, 0));

        CHECK(vkResetCommandBuffer(cmd, 0));
        CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkImageMemoryBarrier bb[2]{};
        for (int i = 0; i < 2; i++) {
            bb[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bb[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                      VK_REMAINING_ARRAY_LAYERS};
        }
        bb[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bb[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bb[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bb[0].image = img;
        bb[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bb[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bb[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb[1].image = bimg;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, bb);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {(int32_t)W, (int32_t)H, 1};
        bl.dstOffsets[1] = {(int32_t)W, (int32_t)H, 1};
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bimg,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       VK_FILTER_NEAREST);
        CHECK(vkEndCommandBuffer(cmd));
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(queue));
    }

    CHECK(vkMapMemory(dev, bmem, 0, OBYTES * 2, 0, &ptr));
    const uint8_t *o = (const uint8_t *)ptr;
    // Colour bytes only, all three of them.
    //
    // This used to read byte 0 of each texel, which is red in R8G8B8A8 and
    // *blue* in the B8G8R8A8 the SRGB cases use -- so a target holding nothing
    // but the shader's red was reported empty. Alpha stays out of it because
    // the clear sets it to 1, and including it would make every untouched
    // layer look drawn.
    auto drawn = [&](const uint8_t *p) {
        for (VkDeviceSize i = 0; i < OBYTES; i += 4)
            if (p[i] || p[i + 1] || p[i + 2])
                return true;
        return false;
    };
    printf("SAMPLED_NONZERO=%d\n", drawn(o) ? 1 : 0);
    // The gate for the fragment patch. Both layers of the output hold the
    // result of a sample, and what the second one holds says which layer that
    // sample reached -- which is the one thing multiview does not decide for
    // us, because it view-indexes subpass inputs and never samplers.
    if (olayers >= 2) {
        printf("OUT1_NONZERO=%d\n", drawn(o + OBYTES) ? 1 : 0);
        printf("OUT_DIFFER=%d\n",
               memcmp(o, o + OBYTES, (size_t)OBYTES) != 0 ? 1 : 0);
    } else {
        printf("OUT1_NONZERO=absent\n");
        printf("OUT_DIFFER=absent\n");
    }
    vkUnmapMemory(dev, bmem);

    vkDestroyPipeline(dev, pipe2, nullptr);
    vkDestroyPipelineLayout(dev, pl2, nullptr);
    vkDestroyShaderModule(dev, fsm2, nullptr);
    vkDestroyDescriptorPool(dev, dpool, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    // The view arena, released here so a validation sweep of this harness
    // stays exactly as clean as it was. It carries four pre-existing VUIDs
    // and leaving these three objects alive added a fifth -- and a suite that
    // is not validation-clean cannot be used to clear a patch of validation
    // errors, which is what the note at the top of this file is about.
    if (vsm2 != vsm)
        vkDestroyShaderModule(dev, vsm2, nullptr);
    if (vdsl != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, vdsl, nullptr);
    if (pad_dsl != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(dev, pad_dsl, nullptr);
    if (vbuf != VK_NULL_HANDLE)
        vkDestroyBuffer(dev, vbuf, nullptr);
    if (vmem != VK_NULL_HANDLE) {
        vkUnmapMemory(dev, vmem);
        vkFreeMemory(dev, vmem, nullptr);
    }
    vkDestroySampler(dev, samp, nullptr);
    if (aview != VK_NULL_HANDLE)
        vkDestroyImageView(dev, aview, nullptr);
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
    if (rp_alias != VK_NULL_HANDLE)
        vkDestroyRenderPass(dev, rp_alias, nullptr);
    vkDestroyRenderPass(dev, rp, nullptr);
    vkDestroyImageView(dev, view, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    // A doubled image that NO framebuffer ever names. The layer doubles any
    // 2D single-layer colour target, so this one is doubled and then never
    // attached -- which is the case take 182 had 45 of and could not report,
    // because the writer map only gained an entry at framebuffer creation.
    // Without this the "NO RENDER PASS WROTE IT" line has no way to fire here
    // and would ship unexercised, which is how three earlier reports in this
    // project turned out to be untestable. No memory is bound: the layer records at
    // vkCreateImage, and Vulkan allows create/destroy unbound. Created LAST
    // on purpose -- doing it early consumed an image serial and shifted the
    // ones the transfer-inventory case asserts on.
    {
        VkImageCreateInfo oci{};
        oci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        oci.imageType = VK_IMAGE_TYPE_2D;
        oci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        oci.extent = {64, 64, 1};
        oci.mipLevels = 1;
        oci.arrayLayers = 1;
        oci.samples = VK_SAMPLE_COUNT_1_BIT;
        oci.tiling = VK_IMAGE_TILING_OPTIMAL;
        oci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_STORAGE_BIT;
        oci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        oci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage orphan;
        CHECK(vkCreateImage(dev, &oci, nullptr, &orphan));
        printf("ORPHAN_DOUBLED=1\n");
        vkDestroyImage(dev, orphan, nullptr);
    }

    if (want_fsr) {
        vkDestroyDescriptorPool(dev, fpool, nullptr);
        vkDestroyDescriptorSetLayout(dev, fdsl, nullptr);
        vkDestroyImageView(dev, fview, nullptr);
        vkDestroyImage(dev, fimg, nullptr);
        vkFreeMemory(dev, fmem, nullptr);
        vkDestroyBuffer(dev, fbuf, nullptr);
        vkFreeMemory(dev, fbmem, nullptr);
    }
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
