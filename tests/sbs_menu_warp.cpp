// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The present-time composite (layer/x4vr_sbs.hpp) with the full-screen menu
// warp in it, on a GPU, under the validation layer, with no X4 and no runtime.
//
// tests/menu_warp_render.cpp checks the pass alone. This checks where it sits:
// the composite takes X4's eye image out of PRESENT_SRC_KHR, copies it into
// the window's halves, then into the runtime's image -- or, asked, warps it
// there, which moves the eye image to SHADER_READ_ONLY and back -- and leaves
// everything as X4 and the runtime expect. A barrier wrong there is invisible
// in the pass's own test and a hang or a black eye in a take (take 389 lost a
// run to an integration its harness never drove).
//
// The swapchain is the test's own images behind a fake
// vkGetSwapchainImagesKHR. Each case runs the composite twice on the same
// slot, so the second run starts from the layouts the first one left.
#define X4VR_LOG_TAG "test"
#include "../layer/x4vr_sbs.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>

using namespace x4vr;

static int g_bad = 0;
static int g_validation_errors = 0;
static char g_log_path[] = "/tmp/x4vr-test-sbswarp-XXXXXX";

static void ok(const char *what, bool cond) {
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        g_bad++;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
         VkDebugUtilsMessageTypeFlagsEXT,
         const VkDebugUtilsMessengerCallbackDataEXT *d, void *) {
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_validation_errors++;
        printf("     validation: %s\n", d->pMessage ? d->pMessage : "(none)");
    }
    return VK_FALSE;
}

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties &mp,
                         uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static const uint32_t kW = 128, kH = 96, kImages = 2;
static const float kDeg = 57.2957795f;
static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDeviceMemoryProperties g_mp{};
static std::vector<VkImage> g_window; // the "real" swapchain's images

static VKAPI_ATTR VkResult VKAPI_CALL fake_images(VkDevice, VkSwapchainKHR,
                                                  uint32_t *n, VkImage *out) {
    if (!out) {
        *n = (uint32_t)g_window.size();
        return VK_SUCCESS;
    }
    for (uint32_t i = 0; i < *n && i < g_window.size(); i++)
        out[i] = g_window[i];
    return VK_SUCCESS;
}

static VkImage make_image(VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers,
                          VkImageUsageFlags usage, VkDeviceMemory *mem) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage im = VK_NULL_HANDLE;
    vkCreateImage(g_dev, &ii, nullptr, &im);
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(g_dev, im, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex =
        find_mem(g_mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g_dev, &ai, nullptr, mem);
    vkBindImageMemory(g_dev, im, *mem, 0);
    return im;
}

struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = nullptr;
};

static Buf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    Buf b;
    vkCreateBuffer(g_dev, &bci, nullptr, &b.buf);
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(g_dev, b.buf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_mem(g_mp, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g_dev, &ai, nullptr, &b.mem);
    vkBindBufferMemory(g_dev, b.buf, b.mem, 0);
    vkMapMemory(g_dev, b.mem, 0, size, 0, &b.map);
    return b;
}

static void free_buffer(Buf &b) {
    vkUnmapMemory(g_dev, b.mem);
    vkDestroyBuffer(g_dev, b.buf, nullptr);
    vkFreeMemory(g_dev, b.mem, nullptr);
}

static void barrier(VkCommandBuffer cb, VkImage im, uint32_t layers,
                    VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                    VkAccessFlags da) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

struct Ctx {
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
};

static void submit_wait(Ctx &c, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(c.queue);
    vkFreeCommandBuffers(g_dev, c.pool, 1, &cb);
}

static VkCommandBuffer begin(Ctx &c) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);
    return cb;
}

// What X4 does: render the eye image (here, upload it) and leave it ready to
// present.
static void x4_frame(Ctx &c, VkImage eye, const std::vector<uint8_t> &pix,
                     VkImageLayout from) {
    const VkDeviceSize bytes = pix.size();
    Buf up = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(up.map, pix.data(), bytes);
    VkCommandBuffer cb = begin(c);
    barrier(cb, eye, 2, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r[2]{};
    for (uint32_t l = 0; l < 2; l++) {
        r[l].bufferOffset = (VkDeviceSize)kW * kH * 4 * l;
        r[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
        r[l].imageExtent = {kW, kH, 1};
    }
    vkCmdCopyBufferToImage(cb, up.buf, eye, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           2, r);
    barrier(cb, eye, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT);
    submit_wait(c, cb);
    free_buffer(up);
}

// What the runtime does with its image after the release: read it.
static std::vector<uint8_t> runtime_reads(Ctx &c, VkImage xr) {
    const VkDeviceSize bytes = (VkDeviceSize)kW * kH * 4 * 2;
    Buf back = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cb = begin(c);
    barrier(cb, xr, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r[2]{};
    for (uint32_t l = 0; l < 2; l++) {
        r[l].bufferOffset = (VkDeviceSize)kW * kH * 4 * l;
        r[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
        r[l].imageExtent = {kW, kH, 1};
    }
    vkCmdCopyImageToBuffer(cb, xr, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back.buf,
                           2, r);
    // Back to where the next frame's composite will find it: the runtime
    // hands images out again in whatever layout, and the composite writes
    // them from UNDEFINED.
    barrier(cb, xr, 2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    submit_wait(c, cb);
    std::vector<uint8_t> out((const uint8_t *)back.map,
                             (const uint8_t *)back.map + bytes);
    free_buffer(back);
    return out;
}

int main() {
    {
        const int fd = mkstemp(g_log_path);
        if (fd >= 0)
            close(fd);
        setenv("X4VR_LOG", g_log_path, 1);
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
                          VK_KHR_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    VkInstance inst = VK_NULL_HANDLE;
    const bool validated = vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS;
    if (!validated) {
        printf("note validation layer unavailable — barrier checking is OFF\n");
        ici.enabledLayerCount = 0;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = &exts[1];
        if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
            printf("FAIL no Vulkan instance\n");
            return 1;
        }
    }
    VkDebugUtilsMessengerEXT msgr = VK_NULL_HANDLE;
    if (validated) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = debug_cb;
        if (create)
            create(inst, &mi, nullptr, &msgr);
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    if (!n || vkEnumeratePhysicalDevices(inst, &n, phys.data()) != VK_SUCCESS) {
        printf("FAIL no physical device\n");
        return 1;
    }
    VkPhysicalDevice pd = phys[0];
    Ctx c;
    uint32_t qn = 0;
    c.family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            c.family = i;
            break;
        }
    if (c.family == UINT32_MAX) {
        printf("FAIL no graphics queue\n");
        return 1;
    }
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = c.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // VK_KHR_swapchain for PRESENT_SRC_KHR, the layout X4 leaves its frame in,
    // as in tests/cursor_render.cpp. No swapchain is made.
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    if (vkCreateDevice(pd, &dci, nullptr, &g_dev) != VK_SUCCESS) {
        printf("FAIL no device\n");
        return 1;
    }
    vkGetDeviceQueue(g_dev, c.family, 0, &c.queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &g_mp);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = c.family;
    vkCreateCommandPool(g_dev, &pci, nullptr, &c.pool);

    SbsFns sf;
#define RESOLVE(name) sf.name = (PFN_vk##name)vkGetDeviceProcAddr(g_dev, "vk" #name);
    X4VR_SBS_FNS(RESOLVE)
#undef RESOLVE
    sf.GetSwapchainImagesKHR = fake_images;
    CursorFns wf;
#define RESOLVE(name) wf.name = (PFN_vk##name)vkGetDeviceProcAddr(g_dev, "vk" #name);
    X4VR_CURSOR_FNS(RESOLVE)
#undef RESOLVE
    ok("every composite and warp entry point resolves",
       sf.complete() && wf.complete());
    if (!sf.complete() || !wf.complete())
        return 1;

    // The window's images, twice an eye wide: what X4's real swapchain holds.
    std::vector<VkDeviceMemory> window_mem(kImages);
    for (uint32_t i = 0; i < kImages; i++)
        g_window.push_back(make_image(VK_FORMAT_B8G8R8A8_UNORM, 2 * kW, kH, 1,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      &window_mem[i]));
    // The runtime's image: two layers, sRGB, as the runtime offers it.
    VkDeviceMemory xr_mem = VK_NULL_HANDLE;
    const VkImage xr =
        make_image(VK_FORMAT_B8G8R8A8_SRGB, kW, kH, 2,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &xr_mem);

    SbsCompositor sbs;
    sbs.configure(g_dev, sf, g_mp);
    sbs.configure_warp(wf);
    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sci.imageExtent = {2 * kW, kH};
    sci.imageArrayLayers = 1;
    // TRANSFER_DST so this test can stand in for X4's rendering by a copy.
    sci.imageUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkSwapchainKHR sc = (VkSwapchainKHR)(uintptr_t)0x5c;
    sbs.add_swapchain(sc, sci, true, 2, 1);
    const std::vector<VkImage> *eyes = sbs.eye_images(sc);
    ok("the composite made two-layer eye images for X4", eyes && eyes->size() == kImages);
    if (!eyes || eyes->size() != kImages)
        return 1;
    const VkImage eye = (*eyes)[0];

    std::vector<uint8_t> pic((size_t)kW * kH * 4 * 2);
    for (uint32_t l = 0; l < 2; l++)
        for (uint32_t y = 0; y < kH; y++)
            for (uint32_t x = 0; x < kW; x++) {
                uint8_t *p = &pic[((size_t)l * kH * kW + (size_t)y * kW + x) * 4];
                p[0] = (uint8_t)(x * 2 + l * 50);
                p[1] = (uint8_t)(y * 2);
                p[2] = (uint8_t)((x + y) * 3);
                p[3] = 255;
            }
    const WarpTan field = warp_tan(-55.f / kDeg, 55.f / kDeg, 55.f / kDeg, -55.f / kDeg);
    const float id[4] = {0, 0, 0, 1};

    auto frame = [&](const MenuWarp::Params *warp, VkImageLayout from) {
        x4_frame(c, eye, pic, from);
        sbs.request_vr_blit(xr, {kW, kH}, 2, VK_FORMAT_B8G8R8A8_SRGB, warp);
        const VkSemaphore done =
            sbs.composite(c.queue, c.family, sc, 0, nullptr, 0, nullptr);
        // The present waits on it; here an empty submit does.
        if (done != VK_NULL_HANDLE) {
            const VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &done;
            si.pWaitDstStageMask = &st;
            vkQueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE);
        }
        vkQueueWaitIdle(c.queue);
        return done != VK_NULL_HANDLE;
    };

    // ---- the copy, as every frame outside a full-screen menu --------------
    ok("copy: the composite ran", frame(nullptr, VK_IMAGE_LAYOUT_UNDEFINED));
    ok("copy: and says it did not warp", !sbs.vr_last_warped());
    std::vector<uint8_t> got = runtime_reads(c, xr);
    ok("copy: the runtime's image is the eye image, byte for byte", got == pic);

    // ---- warped, unturned: the same bytes, through the pass ----------------
    MenuWarp::Params p;
    warp_matrix(id, id, p.m);
    p.src[0] = p.src[1] = p.dst[0] = p.dst[1] = field;
    ok("warp, unturned: the composite ran",
       frame(&p, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR));
    ok("warp, unturned: and says it warped", sbs.vr_last_warped());
    got = runtime_reads(c, xr);
    int worst = 0;
    for (size_t i = 0; i < pic.size(); i++)
        worst = std::max(worst, std::abs((int)got[i] - (int)pic[i]));
    printf("     warp, unturned: worst byte %d\n", worst);
    ok("warp, unturned: the runtime's image is the eye image within 1", worst <= 1);

    // ---- warped, turned: the eye image went back to PRESENT_SRC ------------
    float shown[4] = {0.f, std::sin(-10.f / kDeg), 0.f, std::cos(-10.f / kDeg)};
    warp_matrix(id, shown, p.m);
    ok("warp, turned 20 right: the composite ran",
       frame(&p, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR));
    got = runtime_reads(c, xr);
    // Turned right, X4's middle is left of the view's middle: the byte that
    // was in X4's middle column is now further left.
    const int mid = kW / 2;
    const uint8_t want_b = pic[((size_t)(kH / 2) * kW + mid) * 4];
    int found = -1;
    for (int x = 0; x < (int)kW; x++)
        if (std::abs((int)got[((size_t)(kH / 2) * kW + x) * 4] - want_b) <= 2) {
            found = x;
            break;
        }
    const double want_x =
        (std::tan(-20.0 / kDeg) - field.l) / (field.r - field.l) * kW - 0.5;
    printf("     warp, turned: X4's middle column is at x=%d, trigonometry says %.1f\n",
           found, want_x);
    ok("warp, turned: X4's middle column moved left by tan(20)",
       found >= 0 && std::fabs(found - want_x) <= 1.5);

    // ---- and back to the copy, from the layouts the warp left ---------------
    ok("copy again: the composite ran", frame(nullptr, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR));
    ok("copy again: not warped", !sbs.vr_last_warped());
    got = runtime_reads(c, xr);
    ok("copy again: byte for byte", got == pic);
    ok("the warp counted one frame per warped composite", sbs.vr_warped() == 2);

    sbs.forget_target(xr);
    sbs.shutdown();
    vkDestroyImage(g_dev, xr, nullptr);
    vkFreeMemory(g_dev, xr_mem, nullptr);
    for (uint32_t i = 0; i < kImages; i++) {
        vkDestroyImage(g_dev, g_window[i], nullptr);
        vkFreeMemory(g_dev, window_mem[i], nullptr);
    }
    vkDestroyCommandPool(g_dev, c.pool, nullptr);
    vkDestroyDevice(g_dev, nullptr);
    if (msgr) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(inst, msgr, nullptr);
    }
    vkDestroyInstance(inst, nullptr);
    unlink(g_log_path);

    if (validated)
        ok("no validation errors", g_validation_errors == 0);
    printf("%s\n", g_bad ? "FAILED" : "all passed");
    return g_bad ? 1 : 0;
}
