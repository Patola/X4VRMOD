// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// Does an OpenXR session come up on a Vulkan device created the way X4
// creates one -- and what does the runtime then tell us about the eyes?
//
// This is the bring-up in common/x4vr_xr.hpp driven on a real GPU, before a
// line of it runs inside the game. Everything it prints is a number the next
// design step depends on and that nothing in the repository can currently
// answer: which VkPhysicalDevice the runtime insists on, which Vulkan
// extensions it adds behind the application's back, the recommended per-eye
// image size, the per-view field of view, the runtime's own IPD, and whether
// the head pose actually moves.
//
// It also paints an eye test card. The card is placed BY ANGLE, not by pixel,
// and that is the whole lesson of the first run: WiVRn's views are asymmetric
// (view 0 spans -54..+40 degrees, view 1 spans -40..+54), so the centre of the
// image is 15 degrees off-axis in each eye, in OPPOSITE directions. A bar
// painted at x = W/2 in both eyes asks them to diverge by 30 degrees; it
// cannot fuse, and it shows up as two separate bars. Which is exactly what a
// naive submission of X4's symmetric frustum would do to the whole scene.
//
//   both eyes : one white bar straight ahead, converged by 1.8 degrees, so it
//               fuses and sits about 2 m away
//   view 0    : dark BLUE  background, wide marker bar at -45 degrees
//   view 1    : dark GREEN background, wide marker bar at +45 degrees
//
// The markers are monocular on purpose -- they answer "which eye am I?", which
// is a one-eye-at-a-time question. Close one eye: blue with the wide bar off
// to your LEFT is the left eye.
//
// Two modes:
//   (no args) | <seconds>   the real thing; needs a runtime and a headset
//   selftest                the card only, on a plain 2-layer image with no
//                           OpenXR at all, using the FOV WiVRn actually
//                           reported. Includes the two cases that must fail.
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

static float rad(float d) { return d * 0.01745329251994330f; }

// ------------------------------------------------------------- the card

// One description of the card, so the painter and the checker cannot drift
// apart -- and one place where "where does this angle land" is written down.
//
// A rectilinear projection is linear in TANGENT, not in angle, so a bar at
// theta lands at (tan theta - tan L) / (tan R - tan L) across the image. With
// a symmetric FOV that puts theta = 0 at x = W/2 and the distinction never
// comes up. With WiVRn's canted views it puts theta = 0 at 62% of the width in
// one eye and 38% in the other, and getting it wrong asks the eyes to diverge.
struct Card {
    uint32_t w = 0, h = 0;
    XrFovf fov[2] = {};

    uint32_t marker_w = 0, marker_h = 0, fuse_w = 0;
    float marker_deg = 45.0f; // |angle| of the per-eye identity marker
    // Half the convergence of the fusible bar. 0.9 deg each way is 1.8 deg
    // total, which for a 0.063 m IPD is an object at ipd/angle = 2.0 m.
    float fuse_deg = 0.9f;

    static Card of(uint32_t w, uint32_t h, const XrFovf *fov) {
        Card c;
        c.w = w;
        c.h = h;
        c.fov[0] = fov[0];
        c.fov[1] = fov[1];
        c.marker_w = w / 12;
        c.marker_h = h / 3;
        c.fuse_w = w / 48;
        return c;
    }

    float u_of_angle(uint32_t eye, float radians) const {
        const float tl = tanf(fov[eye].angleLeft);
        const float tr = tanf(fov[eye].angleRight);
        return (tanf(radians) - tl) / (tr - tl);
    }
    float angle_of_x(uint32_t eye, float x) const {
        const float tl = tanf(fov[eye].angleLeft);
        const float tr = tanf(fov[eye].angleRight);
        return atanf(tl + (x / (float)w) * (tr - tl));
    }
    int32_t x_centre_of_angle(uint32_t eye, float radians) const {
        return (int32_t)(u_of_angle(eye, radians) * (float)w + 0.5f);
    }
    // Vertical: OpenXR's angleUp is positive and row 0 is the top.
    int32_t y_centre() const {
        const float tu = tanf(fov[0].angleUp), td = tanf(fov[0].angleDown);
        return (int32_t)(((tu - 0.0f) / (tu - td)) * (float)h + 0.5f);
    }

    float marker_angle(uint32_t eye) const {
        return rad(eye == 0 ? -marker_deg : marker_deg);
    }
    float fuse_angle(uint32_t eye) const {
        // The left eye sees a near object slightly to the RIGHT of its own
        // forward direction, and vice versa. That is convergence: get the sign
        // backwards and the bar fuses BEHIND the background instead of in
        // front, which is the defect worth catching.
        return rad(eye == 0 ? fuse_deg : -fuse_deg);
    }
};

// Recorded with clears and buffer-to-image copies: no render pass, no
// pipeline, no shaders. The card has to be trustworthy, and the fewer moving
// parts between "what I meant" and "what the headset shows", the fewer ways it
// can lie about which eye is which.
enum CardBug {
    kCardGood = 0,
    kCardSwapped,     // view 1 painted as a copy of view 0
    kCardPixelCentre, // the fusible bar at x = W/2 -- the bug the first run found
};

static void paint_card(VkCommandBuffer cb, VkImage img, VkBuffer white,
                       const Card &c, VkImageLayout final_layout,
                       VkAccessFlags final_access,
                       VkPipelineStageFlags final_stage, CardBug bug) {
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

    const VkClearColorValue bg[2] = {
        {{0.05f, 0.07f, 0.35f, 1.0f}},
        {{0.05f, 0.35f, 0.07f, 1.0f}},
    };
    for (uint32_t eye = 0; eye < 2; eye++) {
        const uint32_t src = bug == kCardSwapped ? 0 : eye;
        VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &bg[src], 1, &rr);
    }

    const int32_t y = c.y_centre() - (int32_t)(c.marker_h / 2);
    for (uint32_t eye = 0; eye < 2; eye++) {
        const uint32_t src = bug == kCardSwapped ? 0 : eye;
        const int32_t mx =
            c.x_centre_of_angle(src, c.marker_angle(src)) -
            (int32_t)(c.marker_w / 2);
        int32_t fx;
        if (bug == kCardPixelCentre)
            fx = (int32_t)(c.w / 2) - (int32_t)(c.fuse_w / 2);
        else
            fx = c.x_centre_of_angle(src, c.fuse_angle(src)) -
                 (int32_t)(c.fuse_w / 2);

        const int32_t xs[2] = {mx, fx};
        const uint32_t ws[2] = {c.marker_w, c.fuse_w};
        for (int i = 0; i < 2; i++) {
            VkBufferImageCopy cp{};
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
            cp.imageOffset = {xs[i], y, 0};
            cp.imageExtent = {ws[i], c.marker_h, 1};
            vkCmdCopyBufferToImage(cb, white, img,
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

// Read the card back out of the pixels and check it in ANGLES, which is the
// only space in which the two views are comparable at all. Returns the number
// of complaints and prints each one.
static int verify_card(const uint8_t *px, size_t layer_bytes, const Card &c) {
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
    auto at = [&](uint32_t eye, int32_t x, int32_t y) {
        return px + layer_bytes * eye + ((size_t)y * c.w + x) * 4;
    };
    auto is_white = [](const uint8_t *p) {
        return p[0] > 200 && p[1] > 200 && p[2] > 200;
    };

    // Tint, sampled well above every bar.
    const uint8_t *b0 = at(0, (int32_t)(c.w / 2), (int32_t)(c.h / 24));
    const uint8_t *b1 = at(1, (int32_t)(c.w / 2), (int32_t)(c.h / 24));
    if (!(b0[2] > b0[1] && b0[2] > b0[0]))
        complain("view 0 background is not blue (rgb %u %u %u)", b0[0], b0[1],
                 b0[2]);
    if (!(b1[1] > b1[2] && b1[1] > b1[0]))
        complain("view 1 background is not green (rgb %u %u %u)", b1[0], b1[1],
                 b1[2]);

    // Find the white runs along the bar row, and name them by width rather
    // than by where they were expected -- a checker that looks only where it
    // expects cannot report "the bar is somewhere else".
    const int32_t y = c.y_centre();
    float marker_ang[2] = {0, 0}, fuse_ang[2] = {0, 0};
    bool have_marker[2] = {false, false}, have_fuse[2] = {false, false};
    for (uint32_t eye = 0; eye < 2; eye++) {
        int runs = 0;
        int32_t x = 0;
        while (x < (int32_t)c.w) {
            if (!is_white(at(eye, x, y))) {
                x++;
                continue;
            }
            int32_t start = x;
            while (x < (int32_t)c.w && is_white(at(eye, x, y)))
                x++;
            const int32_t width = x - start;
            const float centre = (float)(start + x) * 0.5f;
            const float ang = xr::deg(c.angle_of_x(eye, centre));
            runs++;
            // The marker is four times the fusible bar's width, so the split
            // is unambiguous even after a pixel or two of rounding.
            if (width > (int32_t)(c.marker_w + c.fuse_w) / 2) {
                marker_ang[eye] = ang;
                have_marker[eye] = true;
            } else {
                fuse_ang[eye] = ang;
                have_fuse[eye] = true;
            }
        }
        if (runs != 2)
            complain("view %u has %d white bar(s) on the centre row, expected 2",
                     eye, runs);
    }

    for (uint32_t eye = 0; eye < 2; eye++) {
        if (!have_marker[eye]) {
            complain("view %u has no identity marker", eye);
            continue;
        }
        const float want = xr::deg(c.marker_angle(eye));
        printf("card: view %u marker at %+.2f deg (want %+.2f)\n", eye,
               marker_ang[eye], want);
        if (fabsf(marker_ang[eye] - want) > 1.5f)
            complain("view %u marker is at %+.2f deg, expected %+.2f — this is "
                     "the eye-order check",
                     eye, marker_ang[eye], want);
    }

    if (!have_fuse[0] || !have_fuse[1]) {
        complain("the fusible bar is missing from view %d",
                 have_fuse[0] ? 1 : 0);
        return bad;
    }
    const float disparity = fuse_ang[0] - fuse_ang[1];
    const float want = 2.0f * c.fuse_deg;
    printf("card: fusible bar at %+.3f deg (view 0) and %+.3f deg (view 1) — "
           "convergence %+.3f deg, expected %+.3f\n",
           fuse_ang[0], fuse_ang[1], disparity, want);
    if (fabsf(disparity - want) > 0.15f) {
        complain("convergence is %+.3f deg, expected %+.3f%s", disparity, want,
                 disparity < -1.0f ? " — the eyes are being asked to DIVERGE, "
                                     "which no one can fuse"
                                   : "");
    }
    return bad;
}

// --------------------------------------------------------------- selftest

// What WiVRn reported on this machine. Using the real asymmetry rather than a
// tidy symmetric stand-in is the point: a symmetric FOV makes the angle-vs-
// pixel distinction invisible, which is how the first card shipped wrong.
static void wivrn_fov(XrFovf *fov) {
    fov[0].angleLeft = rad(-54.0f);
    fov[0].angleRight = rad(40.0f);
    fov[0].angleUp = rad(44.0f);
    fov[0].angleDown = rad(-55.0f);
    fov[1].angleLeft = rad(-40.0f);
    fov[1].angleRight = rad(54.0f);
    fov[1].angleUp = rad(44.0f);
    fov[1].angleDown = rad(-55.0f);
}

static int selftest() {
    const uint32_t W = 3096, H = 3243; // WiVRn's recommended per-eye extent
    XrFovf fov[2];
    wivrn_fov(fov);
    const Card c = Card::of(W, H, fov);

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
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VK_OK(vkCreateImage(dev, &ii, nullptr, &img));
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(dev, img, &imr);
    VkMemoryAllocateInfo imai{};
    imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex =
        find_mem(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imai.memoryTypeIndex == UINT32_MAX)
        FAIL("no device-local memory type for the image");
    VkDeviceMemory imem = VK_NULL_HANDLE;
    VK_OK(vkAllocateMemory(dev, &imai, nullptr, &imem));
    VK_OK(vkBindImageMemory(dev, img, imem, 0));

    const VkDeviceSize bar_bytes = (VkDeviceSize)c.marker_w * c.marker_h * 4;
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
        mai.memoryTypeIndex = find_mem(
            mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
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

    struct Case {
        CardBug bug;
        const char *what;
        bool must_fail;
    };
    const Case cases[] = {
        {kCardGood, "the card as submitted", false},
        {kCardSwapped, "negative control: view 1 painted as view 0", true},
        {kCardPixelCentre,
         "negative control: fusible bar at x = W/2 in both views (the defect "
         "the first headset run found)",
         true},
    };

    int failures = 0;
    for (const Case &k : cases) {
        printf("card: %s\n", k.what);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &bi);
        paint_card(cb, img, bar, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   k.bug);
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

        if (!k.must_fail && bad) {
            printf("card: the card is wrong — %d complaint(s)\n", bad);
            failures++;
        } else if (k.must_fail && !bad) {
            printf("card: the checker passed a card it should have rejected — "
                   "it is not checking anything\n");
            failures++;
        } else {
            printf("card: %s\n", k.must_fail ? "caught, as required" : "correct");
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

// The v1 extension queries, used only to *report* what enable2 merges
// silently. They resolve only if XR_KHR_vulkan_enable is enabled on the
// instance as well, which runtime_open does when the runtime offers it.
static void report_merged_extensions(xr::Runtime &rt) {
    PFN_xrGetVulkanInstanceExtensionsKHR gi = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR gd = nullptr;
    rt.api.GetInstanceProcAddr(rt.instance, "xrGetVulkanInstanceExtensionsKHR",
                               (PFN_xrVoidFunction *)&gi);
    rt.api.GetInstanceProcAddr(rt.instance, "xrGetVulkanDeviceExtensionsKHR",
                               (PFN_xrVoidFunction *)&gd);
    if (!gi && !gd) {
        printf("xr: the v1 extension queries did not resolve — the merged "
               "extension lists are not observable from here\n");
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

    // The white bar the card is painted with, sized for the widest bar.
    XrFovf guess[2];
    wivrn_fov(guess);
    const Card sizing = Card::of(W, H, guess);
    const VkDeviceSize bar_bytes =
        (VkDeviceSize)sizing.marker_w * sizing.marker_h * 4;
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
        bool render = false;
        if (!xr::frame_begin(s, &render)) {
            struct timespec nap = {0, 2 * 1000 * 1000};
            nanosleep(&nap, nullptr);
            continue;
        }

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
                // Built from the FOV this frame reported, not from a constant:
                // the runtime is allowed to change it, and the whole point of
                // the card is that where a bar belongs depends on it.
                const XrFovf frame_fov[2] = {views[0].fov, views[1].fov};
                const Card card = Card::of(W, H, frame_fov);

                VkCommandBufferBeginInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkResetCommandBuffer(cb, 0);
                vkBeginCommandBuffer(cb, &bi);
                paint_card(cb, sc.images[idx], stage, card,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           kCardGood);
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
            // Where straight ahead lands in each image. This is the number
            // that made the first card unfusible, so it is printed rather than
            // left to be rediscovered.
            const XrFovf kf[2] = {views[0].fov, views[1].fov};
            const Card k = Card::of(W, H, kf);
            printf("xr: the image CENTRE is %+.2f deg in view 0 and %+.2f deg "
                   "in view 1 — a bar painted at x=W/2 in both would ask the "
                   "eyes to diverge by %.2f deg\n",
                   xr::deg(k.angle_of_x(0, (float)W * 0.5f)),
                   xr::deg(k.angle_of_x(1, (float)W * 0.5f)),
                   fabsf(xr::deg(k.angle_of_x(0, (float)W * 0.5f) -
                                 k.angle_of_x(1, (float)W * 0.5f))));
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
