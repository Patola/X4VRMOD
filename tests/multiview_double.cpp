// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Gate 0 of docs/phase4b-test-plan.md: does the layer turn a single-layer
// frame into a two-layer one, and is the result valid Vulkan?
//
// Mimics X4's call shape -- declare Vulkan 1.2, call itself "X4", create a
// 1-layer colour image, a view, a render pass with colour attachments, and a
// framebuffer. With the layer on, all of that must come back doubled, masked
// and valid; with X4VR_MV=0 it must come back exactly as asked for.
//
// Prints one KEY=VALUE line per observable so the runner can assert on them
// without parsing prose. Validation errors are the runner's business.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL=%s:%d\n", #x, r_); return 1; } } while (0)

int main() {
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
    if (!n) { printf("FAIL=no_physical_devices:0\n"); return 1; }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = 0;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    VkDevice dev;
    CHECK(vkCreateDevice(phys[0], &dci, nullptr, &dev));

    const VkFormat FMT = VK_FORMAT_R16G16B16A16_SFLOAT;
    const uint32_t W = 256, H = 256;

    // A colour attachment, asked for with exactly one layer.
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
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    imgci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    CHECK(vkCreateImage(dev, &imgci, nullptr, &img));

    // The layer must have grown it behind our back, so the memory the driver
    // demands is the doubled size. That is the observable, since the create
    // info we hold is our own copy and still says 1.
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys[0], &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    if (type == UINT32_MAX) { printf("FAIL=no_device_local:0\n"); return 1; }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CHECK(vkBindImageMemory(dev, img, mem, 0));

    // One layer's worth, for the ratio below.
    const VkDeviceSize one_layer = (VkDeviceSize)W * H * 8;
    printf("MEM_BYTES=%llu\n", (unsigned long long)req.size);
    printf("LAYERS_IMPLIED=%llu\n",
           (unsigned long long)(req.size / one_layer));

    // The path X4 takes when it samples one of these as a texture: a plain 2D
    // view with VK_REMAINING_ARRAY_LAYERS. On a doubled image that would span
    // 2 layers and be invalid, so the layer has to pin it to 1.
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = FMT;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    VkImageView sampled;
    CHECK(vkCreateImageView(dev, &vci, nullptr, &sampled));
    printf("REMAINING_2D_VIEW=ok\n");

    // The attachment view, single-layer as X4 would make it.
    vci.subresourceRange.layerCount = 1;
    VkImageView att_view;
    CHECK(vkCreateImageView(dev, &vci, nullptr, &att_view));

    VkAttachmentDescription att{};
    att.format = FMT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;

    // No multiview struct from us: an HDR colour pass is what the layer is
    // supposed to recognise and mask on its own.
    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));

    // The gate: a framebuffer over a single-layer view. If the pass was
    // masked, the layer must have swapped in an array view; if it did not,
    // validation rejects this call.
    VkFramebufferCreateInfo fbci{};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = rp;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &att_view;
    fbci.width = W;
    fbci.height = H;
    fbci.layers = 1;
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbci, nullptr, &fb));
    printf("FRAMEBUFFER=ok\n");

    vkDestroyFramebuffer(dev, fb, nullptr);
    vkDestroyRenderPass(dev, rp, nullptr);
    vkDestroyImageView(dev, att_view, nullptr);
    vkDestroyImageView(dev, sampled, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
