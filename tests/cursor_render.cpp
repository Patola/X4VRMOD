// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The cursor overlay, run for real on a GPU, with no X4 and no layer.
//
// `cursor_place` checks the arithmetic; this checks the Vulkan. It builds a
// two-layer image standing in for the eye image, leaves it in PRESENT_SRC_KHR
// exactly as X4 does, hands the overlay a synthetic channel, and reads the
// pixels back. Everything it can get wrong is something a run of X4 would
// otherwise have to discover:
//
//   * whether the pointer lands on the right pixel, in eye coordinates;
//   * whether it reaches **both** layers -- one eye with a cursor and one
//     without is the exact failure mode this project spent takes 27-83 on, in
//     a different guise;
//   * whether alpha blending happened at all, rather than a 32x32 block being
//     stamped over the frame;
//   * whether the barriers are right, which the validation layer answers and
//     nothing else will until a driver crashes.
//
// A validation error fails the test. That is the point of running it here
// rather than reading the code twice.
#define X4VR_LOG_TAG "test"
#include "../layer/x4vr_cursor_draw.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace x4vr;

static int g_bad = 0;
static int g_validation_errors = 0;

static void ok(const char *what, bool cond) {
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        g_bad++;
}

static void eq(const char *what, int got, int want, int tol = 0) {
    const bool good = got >= want - tol && got <= want + tol;
    printf("%-4s %s (got %d want %d%s)\n", good ? "ok" : "FAIL", what, got, want,
           tol ? " +-tol" : "");
    if (!good)
        g_bad++;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
         VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *d,
         void *) {
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

// The eye is deliberately small and not square: 1408x1408 is this machine's
// convenience and nothing in the overlay may depend on it.
static const uint32_t kW = 96, kH = 64, kLayers = 2;
// The cursor, and where it goes. Hot spot (1,1) so the quad's top-left is one
// pixel up and left of the pointer -- a test that used (0,0) would pass whether
// or not the hot spot were subtracted at all.
static const uint32_t kCW = 4, kCH = 4;
static const int32_t kHotX = 1, kHotY = 1;
static const float kPosX = 40.f, kPosY = 20.f;
static const int32_t kQuadX = (int32_t)kPosX - kHotX; // 39
static const int32_t kQuadY = (int32_t)kPosY - kHotY; // 19

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_2;

    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    // VK_KHR_surface only because VK_KHR_swapchain depends on it; no surface is
    // ever created. See the device extension list below for why swapchain is
    // needed at all on a device that never presents.
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
    bool validated = vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS;
    if (!validated) {
        // Still worth running without it -- the pixel checks stand on their
        // own -- but say so, because "no validation errors" would otherwise be
        // reported by a run that could not have seen one.
        printf("note validation layer unavailable — barrier checking is OFF\n");
        ici.enabledLayerCount = 0;
        ici.enabledExtensionCount = 1; // keep VK_KHR_surface, drop debug_utils
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

    uint32_t qn = 0, family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    if (family == UINT32_MAX) {
        printf("FAIL no graphics queue\n");
        return 1;
    }

    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // VK_KHR_swapchain, on a device that will never make one. The eye image is
    // handed over in VK_IMAGE_LAYOUT_PRESENT_SRC_KHR because that is how X4
    // leaves it, and that layout is only a legal value when the extension that
    // defines it is enabled. Without this the run reports two validation errors
    // that belong to the test's stand-in rather than to the overlay -- which is
    // exactly what the first run of this file did.
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) {
        printf("FAIL no device\n");
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    // Resolved through vkGetDeviceProcAddr, the same way the layer does it, so
    // a name that is wrong there is wrong here too.
    CursorFns fns;
#define RESOLVE(name) fns.name = (PFN_vk##name)vkGetDeviceProcAddr(dev, "vk" #name);
    X4VR_CURSOR_FNS(RESOLVE)
#undef RESOLVE
    ok("every overlay entry point resolves", fns.complete());
    if (!fns.complete())
        return 1;

    // ---- the stand-in eye image ------------------------------------------
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {kW, kH, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage eye = VK_NULL_HANDLE;
    vkCreateImage(dev, &ii, nullptr, &eye);
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, eye, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        find_mem(mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory eye_mem = VK_NULL_HANDLE;
    vkAllocateMemory(dev, &mai, nullptr, &eye_mem);
    vkBindImageMemory(dev, eye, eye_mem, 0);

    const VkDeviceSize layer_bytes = (VkDeviceSize)kW * kH * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = layer_bytes * kLayers;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer back = VK_NULL_HANDLE;
    vkCreateBuffer(dev, &bci, nullptr, &back);
    VkMemoryRequirements br{};
    vkGetBufferMemoryRequirements(dev, back, &br);
    mai.allocationSize = br.size;
    mai.memoryTypeIndex = find_mem(mp, br.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory back_mem = VK_NULL_HANDLE;
    vkAllocateMemory(dev, &mai, nullptr, &back_mem);
    vkBindBufferMemory(dev, back, back_mem, 0);

    // ---- the synthetic channel -------------------------------------------
    // Four texels along the cursor's top row, chosen so each answers a
    // different question: opaque, half-transparent, fully transparent, opaque
    // again at the far end so the quad's width is checked as well as its origin.
    Shared shared;
    shared.cursor_w = kCW;
    shared.cursor_h = kCH;
    shared.cursor_pitch = kCW * 4;
    shared.cursor_hot_x = kHotX;
    shared.cursor_hot_y = kHotY;
    shared.cursor_format = 0x16362004u; // SDL_PIXELFORMAT_ARGB8888
    auto texel = [&](uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r,
                     uint8_t a) {
        uint8_t *p = shared.cursor_pixels + ((size_t)y * kCW + x) * 4;
        p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    };
    texel(0, 0, 255, 0, 0, 255); // opaque blue
    texel(1, 0, 255, 0, 0, 128); // half-alpha blue
    texel(2, 0, 255, 0, 0, 0);   // fully transparent: must not touch the frame
    texel(3, 0, 0, 255, 0, 255); // opaque green, at the far edge of the quad
    shared.cursor_img_seq.store(2, std::memory_order_relaxed);
    shared.cursor_x.store(kPosX, std::memory_order_relaxed);
    shared.cursor_y.store(kPosY, std::memory_order_relaxed);
    shared.cursor_visible.store(1, std::memory_order_relaxed);
    shared.seq.store(2, std::memory_order_relaxed);

    CursorOverlay overlay;
    overlay.configure(dev, fns, mp);
    ok("the overlay reports itself ready", overlay.ready());

    // ---- record ----------------------------------------------------------
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = eye;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    // Opaque black, both layers: any non-zero channel afterwards came from the
    // overlay and from nowhere else.
    VkClearColorValue clear{};
    clear.float32[3] = 1.f;
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    vkCmdClearColorImage(cb, eye, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                         &all);
    // Left exactly as X4 leaves it: it believes this is the swapchain.
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    const bool drew = overlay.record(
        cb, {eye, 0, 1, fmt, {kW, kH}, kLayers}, &shared);
    ok("record() reports that it drew", drew);

    // The compositor's own barrier, verbatim: from COLOR_ATTACHMENT because the
    // overlay drew. If this pairing is wrong, validation says so here.
    b.oldLayout = drew ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                       : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = drew ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                           : VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    VkBufferImageCopy r[kLayers]{};
    for (uint32_t l = 0; l < kLayers; l++) {
        r[l].bufferOffset = layer_bytes * l;
        r[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
        r[l].imageExtent = {kW, kH, 1};
    }
    vkCmdCopyImageToBuffer(cb, eye, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, back,
                           kLayers, r);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // ---- read it back ----------------------------------------------------
    void *mapped = nullptr;
    vkMapMemory(dev, back_mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    auto px = [&](uint32_t layer, int32_t x, int32_t y) -> const uint8_t * {
        return (const uint8_t *)mapped + layer_bytes * layer +
               ((size_t)y * kW + x) * 4;
    };

    for (uint32_t l = 0; l < kLayers; l++) {
        char what[128];
        // Blue in a B8G8R8A8 readback is byte 0, which is also the byte SDL's
        // ARGB8888 puts blue in -- the mapping under test, end to end.
        snprintf(what, sizeof(what), "layer %u: opaque texel is fully blue", l);
        eq(what, px(l, kQuadX, kQuadY)[0], 255);
        snprintf(what, sizeof(what), "layer %u: opaque texel has no green", l);
        eq(what, px(l, kQuadX, kQuadY)[1], 0);
        // 255 * (128/255) = 128. If blending were off this would be 255, and if
        // the alpha were applied twice it would be 64.
        snprintf(what, sizeof(what), "layer %u: half-alpha texel is blended", l);
        eq(what, px(l, kQuadX + 1, kQuadY)[0], 128, 2);
        snprintf(what, sizeof(what), "layer %u: transparent texel leaves the frame", l);
        eq(what, px(l, kQuadX + 2, kQuadY)[0], 0);
        snprintf(what, sizeof(what), "layer %u: the far corner of the quad is green", l);
        eq(what, px(l, kQuadX + 3, kQuadY)[1], 255);
        // One pixel outside the quad on every side: the draw must not bleed.
        snprintf(what, sizeof(what), "layer %u: one pixel left of the quad is untouched", l);
        eq(what, px(l, kQuadX - 1, kQuadY)[0], 0);
        snprintf(what, sizeof(what), "layer %u: one pixel above the quad is untouched", l);
        eq(what, px(l, kQuadX, kQuadY - 1)[0], 0);
        snprintf(what, sizeof(what), "layer %u: one row below the quad is untouched", l);
        eq(what, px(l, kQuadX, kQuadY + (int32_t)kCH)[0], 0);
        snprintf(what, sizeof(what), "layer %u: the far corner of the eye is untouched", l);
        eq(what, px(l, (int32_t)kW - 1, (int32_t)kH - 1)[0], 0);
    }
    // The claim that matters most: both eyes got it, and identically. A cursor
    // in one half only is the failure this whole project is shaped around.
    ok("both layers are pixel-identical",
       memcmp(px(0, 0, 0), px(1, 0, 0), (size_t)layer_bytes) == 0);

    vkUnmapMemory(dev, back_mem);
    overlay.shutdown();
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, back, nullptr);
    vkFreeMemory(dev, back_mem, nullptr);
    vkDestroyImage(dev, eye, nullptr);
    vkFreeMemory(dev, eye_mem, nullptr);
    vkDestroyDevice(dev, nullptr);
    if (msgr) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(inst, msgr, nullptr);
    }
    vkDestroyInstance(inst, nullptr);

    if (validated)
        eq("validation errors", g_validation_errors, 0);
    printf("\n%s\n", g_bad ? "SOME CASES FAILED" : "all cases passed");
    return g_bad ? 1 : 0;
}
