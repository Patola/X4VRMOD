// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Does the layer learn which doubled images X4's pool won't take? (Takes 457e
// and 457f, 2026-11-24: on an 8 GB card the map's 3D view, doubled, is larger
// than X4's 128 MiB block; X4 re-creates it every frame and draws nothing.)
//
// The observable is the driver's memory requirement, as in multiview_double.cpp:
// the layer adds the second layer behind the application's back. Each case makes
// images the way X4 does and either drops them unused or makes a view first.
// Prints one NAME=1-or-2 line per case: how many layers the LAST image has.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
static const VkFormat FMT = VK_FORMAT_R16G16B16A16_SFLOAT;
static const VkImageUsageFlags MAP = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // 0x17, the map's

static VkImage make(uint32_t w, uint32_t h, VkImageUsageFlags u, VkDeviceSize *size) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = FMT;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = u;
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ci, nullptr, &img) != VK_SUCCESS) { printf("FAIL=create:0\n"); exit(1); }
    VkMemoryRequirements r{};
    vkGetImageMemoryRequirements(dev, img, &r);
    if (size) *size = r.size;
    return img;
}
// Bound and viewed, as X4 does with one its pool takes; then destroyed.
static void use(uint32_t w, uint32_t h, VkImageUsageFlags u) {
    VkImage img = make(w, h, u, nullptr);
    VkMemoryRequirements r{};
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = r.size;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (r.memoryTypeBits & (1u << i)) { ai.memoryTypeIndex = i; break; }
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &ai, nullptr, &m) != VK_SUCCESS ||
        vkBindImageMemory(dev, img, m, 0) != VK_SUCCESS) { printf("FAIL=bind:0\n"); exit(1); }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = FMT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vi, nullptr, &v) != VK_SUCCESS) { printf("FAIL=view:0\n"); exit(1); }
    vkDestroyImageView(dev, v, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, m, nullptr);
}
static void drop(uint32_t w, uint32_t h, VkImageUsageFlags u, int n) {
    for (int i = 0; i < n; i++)
        vkDestroyImage(dev, make(w, h, u, nullptr), nullptr);
}
// One layer's requirement, from an image the layer never doubles (two layers of
// its own asked for would be doubled by nobody): a plain sampled image.
static int layers(uint32_t w, uint32_t h, VkImageUsageFlags u) {
    VkDeviceSize got = 0, two = 0;
    VkImage a = make(w, h, u, &got);
    vkDestroyImage(dev, a, nullptr);
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = FMT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 2;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = u;
    if (vkCreateImage(dev, &ci, nullptr, &a) != VK_SUCCESS) { printf("FAIL=create2:0\n"); exit(1); }
    VkMemoryRequirements r{};
    vkGetImageMemoryRequirements(dev, a, &r);
    two = r.size;
    vkDestroyImage(dev, a, nullptr);
    return got * 4 > two * 3 ? 2 : 1;
}

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X4";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { printf("FAIL=instance:0\n"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(inst, &n, phys.data());
    if (!n) { printf("FAIL=no_physical_devices:0\n"); return 1; }
    vkGetPhysicalDeviceMemoryProperties(phys[0], &mp);
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    if (vkCreateDevice(phys[0], &dci, nullptr, &dev) != VK_SUCCESS) { printf("FAIL=device:0\n"); return 1; }

    // Each case has its own extent, so its own kind.
    printf("FRESH=%d\n", layers(512, 512, MAP));
    drop(512, 504, MAP, 2);
    printf("TWO_DROPS=%d\n", layers(512, 504, MAP)); // layers() drops one more: the third
    printf("THREE_DROPS=%d\n", layers(512, 504, MAP));
    // Single now; used and dropped alike, it stays so.
    use(512, 504, MAP);
    printf("STAYS=%d\n", layers(512, 504, MAP));
    // A use in between starts the count again.
    drop(512, 496, MAP, 2);
    use(512, 496, MAP);
    drop(512, 496, MAP, 2);
    printf("USED_BETWEEN=%d\n", layers(512, 496, MAP));
    // Used every time: X4's ordinary images, rebuilt at each load.
    for (int i = 0; i < 6; i++)
        use(512, 488, MAP);
    printf("ALWAYS_USED=%d\n", layers(512, 488, MAP));
    // An eye target, however often it is dropped.
    drop(512, 480, MAP | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, 6);
    printf("EYE_TARGET=%d\n", layers(512, 480, MAP | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT));
    // Another kind at the same time is untouched.
    printf("OTHER_KIND=%d\n", layers(512, 472, MAP));
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
