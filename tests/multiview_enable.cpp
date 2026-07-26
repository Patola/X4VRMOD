// Does the layer actually enable multiview on a device the app created
// without it? Declares Vulkan 1.2 and calls itself "X4" so the layer treats
// it as the target, then proves the feature is live by creating a render pass
// with a two-view mask -- which validation rejects outright if multiview is
// disabled. Passing a feature struct and having the feature enabled are not
// the same claim; only this second one matters.
//
// X4VR_MV_TEST_CHAIN=1 makes the app supply its own
// VkPhysicalDeviceVulkan11Features (multiview=false), exercising the
// flip-in-place path instead of the prepend path.
//
// Run it through run-multiview-enable.sh, which drives all four cases and
// checks the two that must fail as well as the two that must not.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X4";              // match the layer's target test
    app.apiVersion = VK_API_VERSION_1_2;      // exactly what X4 declares

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> phys(n);
    CHECK(vkEnumeratePhysicalDevices(inst, &n, phys.data()));
    if (!n) { printf("FAIL no physical devices\n"); return 1; }

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

    // Path B: app already has a 1.1 feature struct, multiview off.
    VkPhysicalDeviceVulkan11Features v11{};
    if (getenv("X4VR_MV_TEST_CHAIN")) {
        v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        v11.multiview = VK_FALSE;
        dci.pNext = &v11;
        printf("test: supplying Vulkan11Features with multiview=false\n");
    } else {
        printf("test: no feature struct in the chain\n");
    }

    VkDevice dev;
    CHECK(vkCreateDevice(phys[0], &dci, nullptr, &dev));
    if (dci.pNext == &v11)
        printf("test: app's struct now reads multiview=%u\n", v11.multiview);

    // The real proof: a render pass with two views. Invalid unless the
    // multiview feature is enabled on this device.
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
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

    const uint32_t view_mask = 0x3;     // views 0 and 1 -- left eye, right eye
    const uint32_t correlation = 0x3;
    VkRenderPassMultiviewCreateInfo mv{};
    mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
    mv.subpassCount = 1;
    mv.pViewMasks = &view_mask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &correlation;

    VkRenderPassCreateInfo rpci{};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.pNext = &mv;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;

    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, nullptr, &rp));
    printf("test: OK — two-view render pass created\n");

    vkDestroyRenderPass(dev, rp, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
