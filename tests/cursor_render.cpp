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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
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
// X4VR_TEST_EYE=WxH overrides it for the steering cases, whose line needs
// more than the 16 px gap between the eye's centre and the pointer.
static uint32_t kW = 96, kH = 64;
static const uint32_t kLayers = 2;
// The cursor, and where it goes. Hot spot (1,1) so the quad's top-left is one
// pixel up and left of the pointer -- a test that used (0,0) would pass whether
// or not the hot spot were subtracted at all.
static const uint32_t kCW = 4, kCH = 4;
static const int32_t kHotX = 1, kHotY = 1;
static const float kPosX = 40.f, kPosY = 20.f;
static const int32_t kQuadX = (int32_t)kPosX - kHotX; // 39
static const int32_t kQuadY = (int32_t)kPosY - kHotY; // 19

// The overlay's census goes through X4VR_LOG. Pointed at a file of our own
// before anything logs (the stream is chosen once, on first use), so the report
// line can be read back and checked rather than scrolled past.
static char g_log_path[] = "/tmp/x4vr-cursor-render-XXXXXX";

static std::string read_log() {
    std::string out;
    if (FILE *f = fopen(g_log_path, "r")) {
        char b[1024];
        while (fgets(b, sizeof b, f))
            out += b;
        fclose(f);
    }
    return out;
}

int main() {
    if (const char *e = getenv("X4VR_TEST_EYE")) {
        unsigned w = 0, h = 0;
        if (sscanf(e, "%ux%u", &w, &h) == 2 && w >= 96 && h >= 64)
            kW = w, kH = h;
    }
    // X4VR_STEER's target (x4vr_steer.hpp): X4VR_TEST_STEER=b|a, and for A the
    // nose's offset from the eye's centre in pixels, X4VR_TEST_NOSE_PX=dx,dy.
    // Drawn with no canvas, as in the cockpit, where X4 shows its line.
    const x4vr::steer::Mode steer_mode =
        x4vr::steer::mode_from_env(getenv("X4VR_TEST_STEER"));
    int32_t nose_px = 0, nose_py = 0;
    if (const char *e = getenv("X4VR_TEST_NOSE_PX"))
        sscanf(e, "%d,%d", &nose_px, &nose_py);
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

    // Task #30: with X4VR_TEST_CANVAS_PX=n the pointer must follow the canvas
    // -- layer 0 (view 0, left eye) by +n px and layer 1 by -n px. Driven as a
    // second run of the same harness rather than a second code path, so the
    // shifted case is checked against exactly the assertions the unshifted one
    // passes. n=0 is the default and must reproduce the old behaviour byte for
    // byte, which is what makes "both layers are pixel-identical" below still
    // a real claim rather than one the knob quietly retired.
    int32_t canvas_px = 0;
    if (const char *e = getenv("X4VR_TEST_CANVAS_PX"))
        canvas_px = (int32_t)strtol(e, nullptr, 10);
    // cursor_rect maps a pixel x to 2x/w - 1, so n pixels is 2n/w in NDC.
    const float canvas_ndc = 2.f * (float)canvas_px / (float)kW;
    // Task #40: the canvas is no longer only a translation. With the off-axis
    // affine on it scales as well -- about 1.29 in x and 1.19 in y for this
    // headset -- and the overlay must move the pointer by the map applied to
    // the HOTSPOT while leaving the bitmap its own size. X4VR_TEST_CANVAS_AX
    // and _AY drive that scale; unset they are 1, which is #30's case exactly
    // and is what keeps the assertions below the same ones that already pass.
    x4vr::CanvasNdc canvas[2];
    canvas[0].bx = +canvas_ndc;
    canvas[1].bx = -canvas_ndc;
    if (const char *e = getenv("X4VR_TEST_CANVAS_AX"))
        canvas[0].ax = canvas[1].ax = strtof(e, nullptr);
    if (const char *e = getenv("X4VR_TEST_CANVAS_AY"))
        canvas[0].ay = canvas[1].ay = strtof(e, nullptr);

    // Where the map says this layer's quad goes, in pixels, derived the same
    // way the overlay derives it: map the hotspot, translate by the delta.
    // Written out here rather than reusing the overlay's own arithmetic --
    // a test that computes the expectation with the code under test cannot
    // fail.
    const float hx = 2.f * kPosX / (float)kW - 1.f;
    const float hy = 2.f * kPosY / (float)kH - 1.f;
    auto quad_dx = [&](uint32_t l) {
        return (int32_t)lrintf((canvas[l].x(hx) - hx) * 0.5f * (float)kW);
    };
    auto quad_dy = [&](uint32_t l) {
        return (int32_t)lrintf((canvas[l].y(hy) - hy) * 0.5f * (float)kH);
    };

    x4vr::steer::Frame sf;
    sf.mode = steer_mode;
    sf.nose_ok = true;
    sf.nose_x = 2.f * (float)nose_px / (float)kW;
    sf.nose_y = 2.f * (float)nose_py / (float)kH;
    const bool steering = steer_mode != x4vr::steer::kOff;
    // Where the ring goes: in A, by the nose; in B, nowhere.
    const int32_t ring_dx = steer_mode == x4vr::steer::kNose ? nose_px : 0;
    const int32_t ring_dy = steer_mode == x4vr::steer::kNose ? nose_py : 0;
    const bool drew = overlay.record(
        cb, {eye, 0, 1, fmt, {kW, kH}, kLayers}, &shared,
        steering ? nullptr : canvas, steering ? &sf : nullptr);
    ok("record() reports that it drew", drew);

    // The census line #46 is scored from. Its REACHABLE box must be the map's
    // images of the window edges, computed here independently of the overlay
    // -- take 313's scorer called a protocol-limited 41% "geometry" because
    // this box did not exist and the drawn extent was all it had.
    {
        overlay.report("test", kW, kH);
        const std::string log = read_log();
        const size_t at = log.find("| reachable x=");
        ok("the drawn line reports the reachable box",
           at != std::string::npos);
        if (at != std::string::npos) {
            float rxl = 0, rxh = 0, ryl = 0, ryh = 0;
            const int n = sscanf(log.c_str() + at,
                                 "| reachable x=%f..%f y=%f..%f", &rxl, &rxh,
                                 &ryl, &ryh);
            ok("the reachable box parses", n == 4);
            const float ex0 = canvas[0].ax * -1.f + canvas[0].bx;
            const float ex1 = canvas[0].ax * +1.f + canvas[0].bx;
            const float ey0 = canvas[0].ay * -1.f + canvas[0].by;
            const float ey1 = canvas[0].ay * +1.f + canvas[0].by;
            auto near = [](float a, float b) { return fabsf(a - b) < 1e-3f; };
            ok("reachable x is the map's image of the window's x edges",
               near(rxl, fminf(ex0, ex1)) && near(rxh, fmaxf(ex0, ex1)));
            ok("reachable y is the map's image of the window's y edges",
               near(ryl, fminf(ey0, ey1)) && near(ryh, fmaxf(ey0, ey1)));
            printf("      reachable x=%.4f..%.4f y=%.4f..%.4f\n", rxl, rxh,
                   ryl, ryh);
        }
    }

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

    // What the steering line leaves under a pixel, from its definition: X4's
    // white at 12% times the coverage at the pixel's centre -- full within
    // 1 px of the segment, fading to 0 at 2 px. 0 when not steering, which
    // keeps every assertion below exactly what it was.
    auto under = [&](int32_t x, int32_t y) -> float {
        if (!steering)
            return 0.f;
        const float ox = 0.5f * (float)kW + (float)ring_dx;
        const float oy = 0.5f * (float)kH + (float)ring_dy;
        const float ex = kPosX + (float)ring_dx, ey = kPosY + (float)ring_dy;
        const float len = hypotf(ex - ox, ey - oy);
        const float ux = (ex - ox) / len, uy = (ey - oy) / len;
        const float sx = ox + ux * x4vr::steer::kLineGapPx;
        const float sy = oy + uy * x4vr::steer::kLineGapPx;
        const float seg = len - x4vr::steer::kLineGapPx;
        const float cx = (float)x + 0.5f - sx, cy = (float)y + 0.5f - sy;
        const float along = cx * ux + cy * uy;
        const float perp = fabsf(cx * uy - cy * ux);
        if (along < 0.f || along > seg)
            return 0.f;
        const float half = 0.5f * x4vr::steer::kLineWidthPx +
                           x4vr::steer::kLineFeatherPx;
        const float cov = fminf(1.f, fmaxf(0.f, (half - perp) /
                                                    x4vr::steer::kLineFeatherPx));
        return 255.f * x4vr::steer::kLineRGBA[3] * cov;
    };
    for (uint32_t l = 0; l < kLayers; l++) {
        char what[128];
        // Where this layer's quad has to be. The sign is the claim: view 0 is
        // the left eye and takes +s, matching gl_ViewIndex in the patched UI
        // module. Backwards here and the pointer would sit on the wrong side
        // of the button in both eyes -- symmetrically, so it would still look
        // deliberate.
        const int32_t qx = kQuadX + quad_dx(l) + ring_dx;
        const int32_t qy = kQuadY + quad_dy(l) + ring_dy;
        // Blue in a B8G8R8A8 readback is byte 0, which is also the byte SDL's
        // ARGB8888 puts blue in -- the mapping under test, end to end.
        snprintf(what, sizeof(what), "layer %u: opaque texel is fully blue", l);
        eq(what, px(l, qx, qy)[0], 255);
        snprintf(what, sizeof(what), "layer %u: opaque texel has no green", l);
        eq(what, px(l, qx, qy)[1], 0);
        // 255 * (128/255) = 128. If blending were off this would be 255, and if
        // the alpha were applied twice it would be 64. Over the steering line
        // it is the glyph over the line: 128 + (1 - 128/255) * under.
        const float a = 128.f / 255.f;
        snprintf(what, sizeof(what), "layer %u: half-alpha texel is blended", l);
        eq(what, px(l, qx + 1, qy)[0],
           (int)lrintf(255.f * a + (1.f - a) * under(qx + 1, qy)),
           steering ? 3 : 2);
        // Green tells the ORDER: the glyph has none, so glyph-over-line keeps
        // half the line's green (~15) where line-over-glyph would show ~31.
        if (steering && under(qx + 1, qy) > 20.f) {
            snprintf(what, sizeof(what), "layer %u: the cursor is drawn OVER "
                                         "the line", l);
            eq(what, px(l, qx + 1, qy)[1],
               (int)lrintf((1.f - a) * under(qx + 1, qy)), 3);
        }
        snprintf(what, sizeof(what), "layer %u: transparent texel leaves the frame", l);
        eq(what, px(l, qx + 2, qy)[0], (int)lrintf(under(qx + 2, qy)), steering ? 3 : 0);
        snprintf(what, sizeof(what), "layer %u: the far corner of the quad is green", l);
        eq(what, px(l, qx + 3, qy)[1], 255);
        // One pixel outside the quad on every side: the draw must not bleed.
        snprintf(what, sizeof(what), "layer %u: one pixel left of the quad is untouched", l);
        eq(what, px(l, qx - 1, qy)[0], (int)lrintf(under(qx - 1, qy)), steering ? 3 : 0);
        snprintf(what, sizeof(what), "layer %u: one pixel above the quad is untouched", l);
        eq(what, px(l, qx, qy - 1)[0], (int)lrintf(under(qx, qy - 1)), steering ? 3 : 0);
        snprintf(what, sizeof(what), "layer %u: one row below the quad is untouched", l);
        eq(what, px(l, qx, qy + (int32_t)kCH)[0],
           (int)lrintf(under(qx, qy + (int32_t)kCH)), steering ? 3 : 0);
        snprintf(what, sizeof(what), "layer %u: the far corner of the eye is untouched", l);
        eq(what, px(l, (int32_t)kW - 1, (int32_t)kH - 1)[0], 0);
    }
    // The steering line, computed here from the definition rather than with
    // the overlay's own arithmetic: from the eye's centre (B) or the nose (A),
    // starting 16 px out, ending at the ring's hotspot. X4's white at 12%
    // over a cleared eye is 0.12 * 255 = 31 in every channel.
    if (steering) {
        const float ox = 0.5f * (float)kW + (float)ring_dx;
        const float oy = 0.5f * (float)kH + (float)ring_dy;
        const float ex = kPosX + (float)ring_dx, ey = kPosY + (float)ring_dy;
        const float len = hypotf(ex - ox, ey - oy);
        const float ux = (ex - ox) / len, uy = (ey - oy) / len;
        const float sx = ox + ux * x4vr::steer::kLineGapPx;
        const float sy = oy + uy * x4vr::steer::kLineGapPx;
        const float mx = 0.5f * (sx + ex), my = 0.5f * (sy + ey);
        for (uint32_t l = 0; l < kLayers; l++) {
            char what[160];
            const uint8_t *m = px(l, (int32_t)floorf(mx), (int32_t)floorf(my));
            snprintf(what, sizeof what,
                     "layer %u: the line's midpoint is X4's white at 12%%", l);
            ok(what, abs((int)m[0] - 31) <= 3 && abs((int)m[1] - 31) <= 3 &&
                         abs((int)m[2] - 31) <= 3);
            // 4 px square to the line: outside the 2+1+1 px it covers.
            const uint8_t *o = px(l, (int32_t)floorf(mx - uy * 4.f),
                                  (int32_t)floorf(my + ux * 4.f));
            snprintf(what, sizeof what, "layer %u: 4 px beside the line is "
                                        "untouched", l);
            ok(what, o[0] == 0 && o[1] == 0 && o[2] == 0);
            // Inside the gap: X4 leaves its crosshair clear, and so do we.
            const uint8_t *g = px(l, (int32_t)floorf(ox + ux * 6.f),
                                  (int32_t)floorf(oy + uy * 6.f));
            snprintf(what, sizeof what, "layer %u: 6 px from the origin (inside "
                                        "the gap) is untouched", l);
            ok(what, g[0] == 0 && g[1] == 0 && g[2] == 0);
        }
        overlay.report("steer-test", kW, kH);
        const std::string log = read_log();
        ok("the census names the mode drawn",
           log.find(steer_mode == x4vr::steer::kNose
                        ? "steer drawn (steer-test): B 0 frame(s), A 1 frame(s)"
                        : "steer drawn (steer-test): B 1 frame(s), A 0 frame(s)") !=
               std::string::npos);
        printf("      line from (%.1f,%.1f) to (%.1f,%.1f), midpoint %.1f,%.1f\n",
               sx, sy, ex, ey, mx, my);
    }

    // The claim that matters most: both eyes got it, and -- with no canvas --
    // identically. A cursor in one half only is the failure this whole project
    // is shaped around.
    //
    // With a canvas the layers must NOT match, and that is the assertion doing
    // the work: every per-layer check above would still pass if the shift were
    // silently dropped, because it would place both quads at kQuadX and both
    // would be found where the unshifted case looks. Only the comparison
    // between the layers can tell "shifted by n" from "not shifted at all".
    const bool eyes_differ = quad_dx(0) != quad_dx(1) || quad_dy(0) != quad_dy(1);
    if (!eyes_differ) {
        ok("both layers are pixel-identical",
           memcmp(px(0, 0, 0), px(1, 0, 0), (size_t)layer_bytes) == 0);
    } else {
        ok("the canvas makes the layers differ",
           memcmp(px(0, 0, 0), px(1, 0, 0), (size_t)layer_bytes) != 0);
        // ...and differ by a translation, not by anything else: the pixel the
        // left eye now covers must be clear in the right eye, and the two
        // quads must be exactly 2n apart.
        ok("the left eye's quad is clear in the right eye",
           px(1, kQuadX + quad_dx(0), kQuadY + quad_dy(0))[0] == 0);
        ok("the right eye's quad is clear in the left eye",
           px(0, kQuadX + quad_dx(1), kQuadY + quad_dy(1))[0] == 0);
    }

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
    // The census log is kept only when it explains a failure: every passing
    // run used to leave one in /tmp (69 by take 335).
    if (g_bad)
        printf("census log kept: %s\n", g_log_path);
    else
        unlink(g_log_path);
    return g_bad ? 1 : 0;
}
