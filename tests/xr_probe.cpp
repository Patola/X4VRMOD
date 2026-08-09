// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Does an OpenXR session come up on a Vulkan device created the way X4
// creates one -- and what does the runtime then tell us about the eyes?
//
// This is the bring-up in common/x4vr_xr.hpp driven on a real GPU, before a
// line of it runs inside the game. Everything it prints is a number the next
// design step depends on and that nothing in the repository can currently
// answer:
//
//   * which VkPhysicalDevice the runtime insists on, and whether that is the
//     one a Vulkan application would have picked on its own;
//   * which Vulkan instance/device extensions the runtime adds behind the
//     application's back (reported via the v1 extension when the runtime also
//     offers it -- enable2 merges them silently by design);
//   * the recommended per-eye image size, against this mod's 1408x1408;
//   * the per-view field of view, and above all whether it is SYMMETRIC.
//     X4's projection is symmetric by construction (row0 = [sx 0 0 0], no
//     off-axis term). If the headset's frusta are not, then rendering X4's
//     frustum and submitting it as the runtime's is wrong, and the eye
//     transform needs an off-axis term it does not have today.
//
// It also paints a test card, because the same session that answers those
// questions can settle P5's gate ("left eye sees the left view") for free:
//
//   left view : dark BLUE  background, white bar hard against the LEFT edge
//   right view: dark GREEN background, white bar hard against the RIGHT edge
//   both      : a centre bar with equal and opposite disparity, so it fuses
//               and floats IN FRONT of the background if the sign is right
//
// Close one eye: blue with a bar on the outside is the left eye. If the tint
// and the outer bar disagree, the eyes are swapped.
//
// Two modes:
//   (no args) | <seconds>   the real thing; needs a runtime and a headset
//   selftest                the card only, on a plain 2-layer image with no
//                           OpenXR at all -- so the thing being read in a
//                           headset has already been read here, by a program
//                           that knows what it painted. Includes a case that
//                           must fail.
//
// Prints KEY=VALUE lines for a runner; exits non-zero with FAIL= on any
// bring-up failure.
#include "../common/x4vr_xr.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace xr = x4vr::xr;

static void sink(void *, const char *s) { printf("%s\n", s); }

#define FAIL(fmt, ...)                                                         \
    do {                                                                       \
        printf("FAIL=" fmt "\n", ##__VA_ARGS__);                               \
        return 1;                                                              \
    } while (0)

#define VK_OK(expr)                                                            \
    do {                                                                       \
        VkResult r_ = (expr);                                                  \
        if (r_ != VK_SUCCESS)                                                  \
            FAIL("%s -> VkResult %d", #expr, (int)r_);                         \
    } while (0)

static double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void on_state(void *, XrSessionState s) {
    printf("xr: session -> %s\n", xr::session_state_name(s));
}

// ------------------------------------------------------------- the card

// One description of the card, so the painter and the checker cannot drift
// apart. Everything downstream derives from these.
struct Card {
    uint32_t w = 0, h = 0;
    uint32_t bar_w = 0, bar_h = 0;
    int32_t disp = 12; // centre bar: +disp in the LEFT eye, -disp in the right

    static Card of(uint32_t w, uint32_t h) {
        Card c;
        c.w = w;
        c.h = h;
        c.bar_w = w / 12;
        c.bar_h = h / 3;
        return c;
    }
    int32_t bar_y() const { return (int32_t)(h / 2 - bar_h / 2); }
    int32_t outer_x(uint32_t eye) const {
        return eye == 0 ? 0 : (int32_t)(w - bar_w);
    }
    int32_t centre_x(uint32_t eye) const {
        return (int32_t)(w / 2 - bar_w / 2) + (eye == 0 ? disp : -disp);
    }
};

// Recorded with clears and buffer-to-image copies: no render pass, no
// pipeline, no shaders. The card has to be trustworthy, and the fewer moving
// parts between "what I meant" and "what the headset shows", the fewer ways it
// can lie about which eye is which.
//
// swap_bug paints view 1 as if it were view 0. It exists so the checker can be
// shown failing: a card verifier whose only case is the good one cannot tell
// "the eyes are correct" from "the check does nothing".
static void paint_card(VkCommandBuffer cb, VkImage img, VkBuffer white_bar,
                       const Card &c, VkImageLayout final_layout,
                       VkAccessFlags final_access,
                       VkPipelineStageFlags final_stage, bool swap_bug) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);

    // Backgrounds: blue is left, green is right.
    const VkClearColorValue bg[2] = {
        {{0.05f, 0.07f, 0.35f, 1.0f}},
        {{0.05f, 0.35f, 0.07f, 1.0f}},
    };
    for (uint32_t eye = 0; eye < 2; eye++) {
        const uint32_t src = swap_bug ? 0 : eye;
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &bg[src], 1, &rr);
    }

    for (uint32_t eye = 0; eye < 2; eye++) {
        const uint32_t src = swap_bug ? 0 : eye;
        const int32_t xs[2] = {c.outer_x(src), c.centre_x(src)};
        for (int32_t x : xs) {
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
            cp.imageOffset = {x, c.bar_y(), 0};
            cp.imageExtent = {c.bar_w, c.bar_h, 1};
            vkCmdCopyBufferToImage(cb, white_bar, img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &cp);
        }
    }

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = final_layout;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = final_access;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, final_stage, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

// Read the card back out of the pixels, with no knowledge of what was
// intended beyond the Card geometry. Returns the number of complaints and
// prints each one.
static int verify_card(const uint8_t *px, size_t layer_bytes, const Card &c) {
    auto at = [&](uint32_t eye, int32_t x, int32_t y) {
        return px + layer_bytes * eye + ((size_t)y * c.w + x) * 4;
    };
    auto is_white = [&](const uint8_t *p) {
        return p[0] > 200 && p[1] > 200 && p[2] > 200;
    };
    const int32_t y = (int32_t)(c.h / 2);
    int bad = 0;
    auto complain = [&](const char *fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        printf("card: %s\n", buf);
        bad++;
    };

    // Tint. Sampled well away from every bar.
    const uint8_t *b0 = at(0, (int32_t)(c.w / 2), (int32_t)(c.h / 12));
    const uint8_t *b1 = at(1, (int32_t)(c.w / 2), (int32_t)(c.h / 12));
    if (!(b0[2] > b0[1] && b0[2] > b0[0]))
        complain("view 0 background is not blue (rgb %u %u %u)", b0[0], b0[1],
                 b0[2]);
    if (!(b1[1] > b1[2] && b1[1] > b1[0]))
        complain("view 1 background is not green (rgb %u %u %u)", b1[0], b1[1],
                 b1[2]);

    // Outer bars: present in one view, absent in the other, at both edges.
    const int32_t left_x = (int32_t)(c.bar_w / 2);
    const int32_t right_x = (int32_t)(c.w - c.bar_w / 2);
    if (!is_white(at(0, left_x, y)))
        complain("view 0 has no bar at the LEFT edge");
    if (is_white(at(1, left_x, y)))
        complain("view 1 has a bar at the LEFT edge — that is view 0's marker");
    if (!is_white(at(1, right_x, y)))
        complain("view 1 has no bar at the RIGHT edge");
    if (is_white(at(0, right_x, y)))
        complain("view 0 has a bar at the RIGHT edge — that is view 1's marker");

    // Centre bar disparity, measured rather than assumed: scan the middle
    // third of the row (which the outer bars cannot reach) for the first white
    // pixel in each view.
    int32_t edge[2] = {-1, -1};
    for (uint32_t eye = 0; eye < 2; eye++)
        for (int32_t x = (int32_t)(c.w / 3); x < (int32_t)(2 * c.w / 3); x++)
            if (is_white(at(eye, x, y))) {
                edge[eye] = x;
                break;
            }
    if (edge[0] < 0 || edge[1] < 0) {
        complain("centre bar missing in view %d", edge[0] < 0 ? 0 : 1);
    } else {
        const int32_t got = edge[0] - edge[1];
        printf("card: centre bar at x=%d (view 0) and x=%d (view 1) — "
               "disparity %+d px, expected %+d\n",
               edge[0], edge[1], got, 2 * c.disp);
        if (got != 2 * c.disp)
            complain("centre disparity is %+d px, expected %+d", got,
                     2 * c.disp);
    }
    return bad;
}

// --------------------------------------------------------------- selftest

// The card on a plain 2-layer image: no runtime, no headset, no compositor.
// This is the part that can be wrong in a way a person wearing a headset would
// misread as "the eyes are swapped", so it gets checked by a program that
// knows exactly what it asked for.
static int selftest() {
    const uint32_t W = 1408, H = 1408;
    const Card c = Card::of(W, H);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X4";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance vk = VK_NULL_HANDLE;
    VK_OK(vkCreateInstance(&ici, nullptr, &vk));

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(vk, &ndev, nullptr);
    if (!ndev)
        FAIL("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(vk, &ndev, devs.data());
    VkPhysicalDevice phys = devs[0];

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
    uint32_t gfx = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            gfx = i;
            break;
        }
    if (gfx == UINT32_MAX)
        FAIL("no graphics queue family");

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    VK_OK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    auto find_mem = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    };

    // UNORM, so a readback byte is exactly the value that was cleared and the
    // check is not reading an sRGB curve.
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {W, H, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 2;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VK_OK(vkCreateImage(dev, &ii, nullptr, &img));
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo imai{};
    imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex = find_mem(imr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imai.memoryTypeIndex == UINT32_MAX)
        FAIL("no device-local memory type for the image");
    VkDeviceMemory imem = VK_NULL_HANDLE;
    VK_OK(vkAllocateMemory(dev, &imai, nullptr, &imem));
    VK_OK(vkBindImageMemory(dev, img, imem, 0));

    // The white bar source, and the readback destination.
    const VkDeviceSize bar_bytes = (VkDeviceSize)c.bar_w * c.bar_h * 4;
    const VkDeviceSize layer_bytes = (VkDeviceSize)W * H * 4;
    auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                           VkBuffer *buf, VkDeviceMemory *mem) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        if (vkCreateBuffer(dev, &bci, nullptr, buf) != VK_SUCCESS)
            return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, *buf, &mr);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex =
            find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX)
            return false;
        return vkAllocateMemory(dev, &mai, nullptr, mem) == VK_SUCCESS &&
               vkBindBufferMemory(dev, *buf, *mem, 0) == VK_SUCCESS;
    };

    VkBuffer bar = VK_NULL_HANDLE, back = VK_NULL_HANDLE;
    VkDeviceMemory bar_mem = VK_NULL_HANDLE, back_mem = VK_NULL_HANDLE;
    if (!make_buffer(bar_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &bar,
                     &bar_mem))
        FAIL("could not allocate the bar buffer");
    if (!make_buffer(layer_bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &back,
                     &back_mem))
        FAIL("could not allocate the readback buffer");
    void *p = nullptr;
    VK_OK(vkMapMemory(dev, bar_mem, 0, bar_bytes, 0, &p));
    memset(p, 0xff, (size_t)bar_bytes);
    vkUnmapMemory(dev, bar_mem);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = gfx;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_OK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_OK(vkAllocateCommandBuffers(dev, &cbi, &cb));

    int failures = 0;
    // Case 1 is the card as it will be submitted. Case 2 paints view 1 as a
    // copy of view 0 -- the exact defect a person in a headset would report as
    // "no stereo" -- and MUST be caught, or the checker proves nothing.
    for (int pass = 0; pass < 2; pass++) {
        const bool swap_bug = pass == 1;
        printf("card: %s\n", swap_bug ? "negative control (view 1 painted as "
                                        "view 0) — this one must fail"
                                      : "the card as submitted");
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        paint_card(cb, img, bar, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   swap_bug);
        VkBufferImageCopy rc{};
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
        rc.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               back, 1, &rc);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK_OK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        VK_OK(vkQueueWaitIdle(queue));

        void *rp = nullptr;
        VK_OK(vkMapMemory(dev, back_mem, 0, layer_bytes * 2, 0, &rp));
        const int bad = verify_card((const uint8_t *)rp, layer_bytes, c);
        vkUnmapMemory(dev, back_mem);

        if (!swap_bug && bad) {
            printf("card: the card is wrong — %d complaint(s)\n", bad);
            failures++;
        } else if (swap_bug && !bad) {
            printf("card: the checker passed a card with both views the same "
                   "— it is not checking anything\n");
            failures++;
        } else {
            printf("card: %s\n", swap_bug ? "caught, as required" : "correct");
        }
    }

    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, bar, nullptr);
    vkDestroyBuffer(dev, back, nullptr);
    vkFreeMemory(dev, bar_mem, nullptr);
    vkFreeMemory(dev, back_mem, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, imem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(vk, nullptr);

    printf("KEY_SELFTEST_FAILURES=%d\n", failures);
    if (failures)
        FAIL("%d selftest case(s) went the wrong way", failures);
    printf("PASS=1\n");
    return 0;
}

// The v1 extension, used only to *report* what enable2 merges silently. Absent
// on a runtime that only implements enable2, which is not an error -- the
// session works either way, we just cannot narrate it.
static void report_merged_extensions(xr::Runtime &rt) {
    PFN_xrGetVulkanInstanceExtensionsKHR gi = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR gd = nullptr;
    rt.api.GetInstanceProcAddr(rt.instance, "xrGetVulkanInstanceExtensionsKHR",
                               (PFN_xrVoidFunction *)&gi);
    rt.api.GetInstanceProcAddr(rt.instance, "xrGetVulkanDeviceExtensionsKHR",
                               (PFN_xrVoidFunction *)&gd);
    if (!gi && !gd) {
        printf("xr: runtime does not expose the v1 extension queries — the "
               "merged extension lists are not observable from here\n");
        return;
    }
    auto dump = [&](const char *what,
                    XrResult (*fn)(XrInstance, XrSystemId, uint32_t, uint32_t *,
                                   char *)) {
        if (!fn)
            return;
        uint32_t n = 0;
        if (fn(rt.instance, rt.system, 0, &n, nullptr) != XR_SUCCESS || !n) {
            printf("xr: runtime adds no %s extensions\n", what);
            return;
        }
        std::vector<char> buf(n);
        if (fn(rt.instance, rt.system, n, &n, buf.data()) != XR_SUCCESS)
            return;
        printf("xr: runtime adds %s extensions: %s\n", what, buf.data());
    };
    dump("instance", gi);
    dump("device", gd);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "selftest") == 0)
        return selftest();

    const double seconds =
        argc > 1 ? atof(argv[1])
                 : (getenv("X4VR_XR_SECONDS") ? atof(getenv("X4VR_XR_SECONDS"))
                                              : 20.0);

    // ------------------------------------------------------------ runtime
    xr::Runtime rt;
    if (!xr::runtime_open(rt, sink, nullptr))
        FAIL("runtime: %s", rt.last_error.c_str());
    report_merged_extensions(rt);

    printf("KEY_RUNTIME=%s\n", rt.runtime_name);
    printf("KEY_SYSTEM=%s\n", rt.system_name);
    printf("KEY_VIEWS=%u\n", rt.view_count);
    printf("KEY_EYE_RECOMMENDED=%ux%u\n", rt.views[0].recommendedImageRectWidth,
           rt.views[0].recommendedImageRectHeight);
    printf("KEY_EYE_MAX=%ux%u\n", rt.views[0].maxImageRectWidth,
           rt.views[0].maxImageRectHeight);
    if (rt.view_count != 2)
        FAIL("expected 2 views for PRIMARY_STEREO, got %u", rt.view_count);

    // ---------------------------------------------- Vulkan, the runtime's way
    //
    // Declare what X4 declares: the instance says it is X4 at Vulkan 1.2 and
    // asks for VK_KHR_surface. A probe that presented itself differently would
    // exercise a different path in both the runtime and our own layer.
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X4";
    app.apiVersion = VK_API_VERSION_1_2;
    const char *inst_ext[] = {"VK_KHR_surface"};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = inst_ext;

    VkInstance vk = VK_NULL_HANDLE;
    VkResult vkr = VK_ERROR_INITIALIZATION_FAILED;
    XrResult xrr = xr::create_vk_instance(rt, vkGetInstanceProcAddr, &ici,
                                          nullptr, &vk, &vkr);
    if (xrr != XR_SUCCESS)
        FAIL("xrCreateVulkanInstanceKHR -> %s", xr::result_name(xrr));
    if (vkr != VK_SUCCESS)
        FAIL("the runtime's vkCreateInstance -> VkResult %d", (int)vkr);
    printf("xr: VkInstance created through the runtime\n");

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    xrr = xr::graphics_device(rt, vk, &phys);
    if (xrr != XR_SUCCESS)
        FAIL("xrGetVulkanGraphicsDevice2KHR -> %s", xr::result_name(xrr));

    // Which one is it, of the ones on offer? The known risk for the layer is
    // that X4 has already chosen a different device by the time we are asked,
    // so the answer that matters is the *index*, not just the name.
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(vk, &ndev, nullptr);
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(vk, &ndev, devs.data());
    int chosen = -1;
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        const bool is_it = devs[i] == phys;
        if (is_it)
            chosen = (int)i;
        printf("xr: physical device %u/%u \"%s\"%s\n", i, ndev, p.deviceName,
               is_it ? "  <-- the runtime requires this one" : "");
    }
    printf("KEY_PHYSICAL_DEVICES=%u\n", ndev);
    printf("KEY_PHYSICAL_CHOSEN=%d\n", chosen);
    if (chosen < 0)
        FAIL("the runtime's device is not in vkEnumeratePhysicalDevices");

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
    uint32_t gfx = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            gfx = i;
            break;
        }
    if (gfx == UINT32_MAX)
        FAIL("no graphics queue family on the runtime's device");

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // Multiview on, because the layer turns it on in X4's device and the eye
    // image is a 2-layer array. A probe without it would be creating a
    // different device than the one this mod actually runs against.
    VkPhysicalDeviceMultiviewFeatures mv{};
    mv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    mv.multiview = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &mv;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice dev = VK_NULL_HANDLE;
    xrr = xr::create_vk_device(rt, vkGetInstanceProcAddr, phys, &dci, nullptr,
                               &dev, &vkr);
    if (xrr != XR_SUCCESS)
        FAIL("xrCreateVulkanDeviceKHR -> %s", xr::result_name(xrr));
    if (vkr != VK_SUCCESS)
        FAIL("the runtime's vkCreateDevice -> VkResult %d", (int)vkr);
    printf("xr: VkDevice created through the runtime (queue family %u)\n", gfx);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    // ------------------------------------------------------------ session
    xr::Session s;
    xrr = xr::session_create(s, rt, vk, phys, dev, gfx, 0);
    if (xrr != XR_SUCCESS)
        FAIL("session: %s", s.last_error.c_str());
    printf("xr: session created, reference space = %s\n",
           s.space_type == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL");
    printf("KEY_SPACE=%s\n",
           s.space_type == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL");

    const VkFormat want[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                             VK_FORMAT_R8G8B8A8_UNORM,
                             VK_FORMAT_B8G8R8A8_UNORM};
    const VkFormat fmt = xr::choose_format(s, want, 4);
    if (fmt == VK_FORMAT_UNDEFINED)
        FAIL("the runtime offers none of the four 8-bit RGBA/BGRA formats");
    printf("KEY_FORMAT=%d\n", (int)fmt);

    const uint32_t W = rt.views[0].recommendedImageRectWidth;
    const uint32_t H = rt.views[0].recommendedImageRectHeight;
    const Card card = Card::of(W, H);
    xr::Swapchain sc;
    // One 2-layer swapchain, one layer per eye -- the same shape as the eye
    // image the compositor already builds, so the eventual submission is a
    // copy and not a re-layout.
    xrr = xr::swapchain_create(sc, s, fmt, W, H, 2, 1);
    if (xrr != XR_SUCCESS)
        FAIL("xrCreateSwapchain (%ux%u, 2 layers) -> %s", W, H,
             xr::result_name(xrr));
    printf("xr: swapchain %ux%u x2 layers, %u image(s)\n", W, H,
           (unsigned)sc.images.size());
    printf("KEY_SWAPCHAIN_IMAGES=%u\n", (unsigned)sc.images.size());

    // The white bar the card is painted with.
    const VkDeviceSize bar_bytes = (VkDeviceSize)card.bar_w * card.bar_h * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bar_bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer stage = VK_NULL_HANDLE;
    VK_OK(vkCreateBuffer(dev, &bci, nullptr, &stage));
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, stage, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        const auto f = mp.memoryTypes[i].propertyFlags;
        if ((mr.memoryTypeBits & (1u << i)) &&
            (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX)
        FAIL("no host-visible coherent memory type");
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VK_OK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    VK_OK(vkBindBufferMemory(dev, stage, mem, 0));
    void *ptr = nullptr;
    VK_OK(vkMapMemory(dev, mem, 0, bar_bytes, 0, &ptr));
    memset(ptr, 0xff, (size_t)bar_bytes);
    vkUnmapMemory(dev, mem);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = gfx;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_OK(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_OK(vkAllocateCommandBuffers(dev, &cbi, &cb));

    // ------------------------------------------------------------ the loop
    uint32_t frames = 0, located = 0, submitted = 0;
    double last_report = 0.0;
    bool printed_fov = false;
    float fov_l[2] = {0, 0}, fov_r[2] = {0, 0}, fov_u[2] = {0, 0},
          fov_d[2] = {0, 0};
    float ipd_m = 0.0f;
    bool asymmetric = false;
    // A head that never moves is the interesting failure, so the span of the
    // reported position is reported at the end rather than only sampled.
    float pmin[3] = {1e9f, 1e9f, 1e9f}, pmax[3] = {-1e9f, -1e9f, -1e9f};
    const double t0 = now_s();

    while (now_s() - t0 < seconds) {
        if (!xr::session_poll(s, on_state, nullptr))
            break;
        if (!s.running) {
            struct timespec nap = {0, 20 * 1000 * 1000};
            nanosleep(&nap, nullptr);
            continue;
        }
        const bool render = xr::frame_begin(s);

        XrView views[2] = {};
        XrViewStateFlags vflags = 0;
        const bool have = xr::locate_views(s, views, 2, &vflags);
        if (have)
            located++;

        std::vector<XrCompositionLayerProjectionView> pv(2);
        XrCompositionLayerProjection proj{};
        const XrCompositionLayerBaseHeader *layers[1] = {
            (const XrCompositionLayerBaseHeader *)&proj};
        uint32_t layer_count = 0;

        if (render && have) {
            uint32_t idx = 0;
            if (xr::swapchain_acquire(sc, &idx)) {
                VkCommandBufferBeginInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkResetCommandBuffer(cb, 0);
                vkBeginCommandBuffer(cb, &bi);
                paint_card(cb, sc.images[idx], stage, card,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           false);
                vkEndCommandBuffer(cb);

                VkSubmitInfo si{};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cb;
                vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(queue);
                xr::swapchain_release(sc);

                for (uint32_t eye = 0; eye < 2; eye++) {
                    pv[eye] = {};
                    pv[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                    pv[eye].pose = views[eye].pose;
                    pv[eye].fov = views[eye].fov;
                    pv[eye].subImage.swapchain = sc.handle;
                    pv[eye].subImage.imageRect = {{0, 0},
                                                  {(int32_t)W, (int32_t)H}};
                    pv[eye].subImage.imageArrayIndex = eye;
                }
                proj = {};
                proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
                proj.space = s.space;
                proj.viewCount = 2;
                proj.views = pv.data();
                layer_count = 1;
            }
        }

        const XrResult er = xr::frame_end(s, layers, layer_count);
        if (er != XR_SUCCESS && frames < 5)
            printf("xr: xrEndFrame -> %s\n", xr::result_name(er));
        else if (er == XR_SUCCESS && layer_count)
            submitted++;
        frames++;

        if (have && !printed_fov) {
            printed_fov = true;
            for (uint32_t e = 0; e < 2; e++) {
                fov_l[e] = views[e].fov.angleLeft;
                fov_r[e] = views[e].fov.angleRight;
                fov_u[e] = views[e].fov.angleUp;
                fov_d[e] = views[e].fov.angleDown;
                const float hsym = fabsf(fabsf(fov_l[e]) - fabsf(fov_r[e]));
                const float vsym = fabsf(fabsf(fov_u[e]) - fabsf(fov_d[e]));
                if (hsym > 1e-4f || vsym > 1e-4f)
                    asymmetric = true;
                printf("xr: view %u fov L%.3f R%.3f U%.3f D%.3f deg "
                       "(h=%.2f v=%.2f)\n",
                       e, xr::deg(fov_l[e]), xr::deg(fov_r[e]),
                       xr::deg(fov_u[e]), xr::deg(fov_d[e]),
                       xr::deg(fov_r[e] - fov_l[e]),
                       xr::deg(fov_u[e] - fov_d[e]));
            }
            const float dx = views[1].pose.position.x - views[0].pose.position.x;
            const float dy = views[1].pose.position.y - views[0].pose.position.y;
            const float dz = views[1].pose.position.z - views[0].pose.position.z;
            ipd_m = sqrtf(dx * dx + dy * dy + dz * dz);
            printf("xr: eye separation %.4f m (view 1 is %+.4f m in x from view "
                   "0 — positive means view 1 is the RIGHT eye)\n",
                   ipd_m, dx);
        }

        if (have) {
            const float pp[3] = {views[0].pose.position.x,
                                 views[0].pose.position.y,
                                 views[0].pose.position.z};
            for (int i = 0; i < 3; i++) {
                if (pp[i] < pmin[i])
                    pmin[i] = pp[i];
                if (pp[i] > pmax[i])
                    pmax[i] = pp[i];
            }
            if (now_s() - last_report > 1.0) {
                last_report = now_s();
                const auto &o = views[0].pose.orientation;
                printf("xr: head at (%+.3f %+.3f %+.3f) q(%+.3f %+.3f %+.3f "
                       "%+.3f) flags=0x%x\n",
                       pp[0], pp[1], pp[2], o.x, o.y, o.z, o.w,
                       (unsigned)vflags);
            }
        }
    }

    printf("KEY_FRAMES=%u\n", frames);
    printf("KEY_LOCATED=%u\n", located);
    printf("KEY_SUBMITTED=%u\n", submitted);
    printf("KEY_IPD_M=%.4f\n", ipd_m);
    printf("KEY_FOV_ASYMMETRIC=%d\n", (int)asymmetric);
    for (uint32_t e = 0; e < 2; e++)
        printf("KEY_FOV%u=%.4f,%.4f,%.4f,%.4f\n", e, xr::deg(fov_l[e]),
               xr::deg(fov_r[e]), xr::deg(fov_u[e]), xr::deg(fov_d[e]));
    if (located)
        printf("KEY_HEAD_SPAN_M=%.4f,%.4f,%.4f\n", pmax[0] - pmin[0],
               pmax[1] - pmin[1], pmax[2] - pmin[2]);

    vkDeviceWaitIdle(dev);
    xr::swapchain_destroy(sc);
    xr::session_destroy(s);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, stage, nullptr);
    vkFreeMemory(dev, mem, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(vk, nullptr);
    xr::runtime_close(rt);

    if (!frames)
        FAIL("the session never reached a running state in %.0f s — the "
             "headset is connected but asleep, or the runtime is not "
             "compositing",
             seconds);
    if (!located)
        FAIL("%u frames, but xrLocateViews never returned a valid orientation",
             frames);
    if (!submitted)
        FAIL("%u frames located, but no projection layer was ever accepted",
             located);
    printf("PASS=1\n");
    return 0;
}
