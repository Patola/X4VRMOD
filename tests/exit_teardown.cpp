// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// gamescope's teardown, reproduced: Vulkan objects destroyed by a global's
// destructor, after main() has returned.
//
// Take 323: gamescope aborted at exit inside x4vr_DestroyImageView. Its own
// exit-time destructors destroy its image views; our layer was dlopen'ed
// later than gamescope's globals were built, so the layer's static tables were
// destroyed FIRST, g_devices.at() found nothing, threw, and the exception
// leaving a Vulkan entry point was std::terminate.
//
// `Holder` below is built before main(), so its destructor runs after the
// layer's would. It destroys an image view, an image, its memory, the device
// and the instance -- the calls gamescope makes -- and the process must then
// exit 0. Under the old layer it aborts (run-no-exit-destructors.sh records the
// control). X4VR_TEST_APP_NAME picks the name the layer sees ("X4" makes it
// the target; anything else leaves it inert, as in gamescope).
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct Holder {
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    ~Holder() {
        if (!dev)
            return;
        printf("test: destroying from a global's destructor, after main\n");
        fflush(stdout);
        vkDestroyImageView(dev, view, nullptr);
        vkDestroyImage(dev, img, nullptr);
        vkFreeMemory(dev, mem, nullptr);
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        printf("test: teardown returned\n");
        fflush(stdout);
    }
} g_holder;

} // namespace

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

int main() {
    const char *name = getenv("X4VR_TEST_APP_NAME");
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = name && *name ? name : "gamescope";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &g_holder.inst));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(g_holder.inst, &n, nullptr));
    std::vector<VkPhysicalDevice> phys(n);
    CHECK(vkEnumeratePhysicalDevices(g_holder.inst, &n, phys.data()));
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
    CHECK(vkCreateDevice(phys[0], &dci, nullptr, &g_holder.dev));

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {64, 64, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(g_holder.dev, &ii, nullptr, &g_holder.img));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_holder.dev, g_holder.img, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys[0], &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (req.memoryTypeBits & (1u << i)) {
            type = i;
            break;
        }
    if (type == UINT32_MAX) { printf("FAIL no memory type\n"); return 1; }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    CHECK(vkAllocateMemory(g_holder.dev, &ai, nullptr, &g_holder.mem));
    CHECK(vkBindImageMemory(g_holder.dev, g_holder.img, g_holder.mem, 0));

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = g_holder.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(vkCreateImageView(g_holder.dev, &vi, nullptr, &g_holder.view));

    printf("test: objects created (app \"%s\"); returning from main\n",
           app.pApplicationName);
    fflush(stdout);
    return 0;
}
