// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The menu warp (layer/x4vr_menu_warp.hpp), run for real on a GPU, with no X4
// and no layer, under the validation layer.
//
// A two-layer stand-in for X4's eye image, B8G8R8A8_UNORM holding sRGB bytes
// as X4's does, warped into a two-layer B8G8R8A8_SRGB stand-in for the
// runtime's swapchain image, and read back:
//
//   * unturned, the bytes come out as they went in, in both layers, and the
//     layers stay apart -- the copy this replaces is byte-exact, so a blur or a
//     tint here would be a regression in every menu;
//   * turned, a bright column lands where plain trigonometry puts it, and every
//     pixel matches a CPU rendering of the same filter;
//   * past X4's picture is black;
//   * a one-pixel checkerboard seen minified averages towards grey instead of
//     aliasing to black or white.
//
// A validation error fails the test.
#define X4VR_LOG_TAG "test"
#include "../layer/x4vr_menu_warp.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>

using namespace x4vr;

static int g_bad = 0;
static int g_validation_errors = 0;
static char g_log_path[] = "/tmp/x4vr-test-menuwarp-XXXXXX";

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

static const uint32_t kW = 256, kH = 256, kLayers = 2;
static const float kDeg = 57.2957795f;

// ---- the CPU rendering of menu_warp.frag -----------------------------------

static double dec(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
static double enc(double c) {
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

struct Px {
    double c[4];
};

static Px texel(const std::vector<uint8_t> &src, uint32_t layer, int x, int y) {
    Px p{{0, 0, 0, 0}};
    if (x < 0 || y < 0 || x >= (int)kW || y >= (int)kH)
        return p;
    const uint8_t *s = &src[((size_t)layer * kH * kW + (size_t)y * kW + x) * 4];
    for (int i = 0; i < 3; i++)
        p.c[i] = dec(s[i] / 255.0);
    p.c[3] = s[3] / 255.0;
    return p;
}

static Px bilinear(const std::vector<uint8_t> &src, uint32_t layer, double px,
                   double py) {
    double bx = px - 0.5, by = py - 0.5;
    int ix = (int)std::floor(bx), iy = (int)std::floor(by);
    double fx = bx - std::floor(bx), fy = by - std::floor(by);
    if (fx < 1e-3) fx = 0;
    if (fy < 1e-3) fy = 0;
    if (fx > 1 - 1e-3) { fx = 0; ix++; }
    if (fy > 1 - 1e-3) { fy = 0; iy++; }
    const Px a = texel(src, layer, ix, iy), b = texel(src, layer, ix + 1, iy),
             c = texel(src, layer, ix, iy + 1), d = texel(src, layer, ix + 1, iy + 1);
    Px r;
    for (int i = 0; i < 4; i++) {
        const double top = a.c[i] + (b.c[i] - a.c[i]) * fx;
        const double bot = c.c[i] + (d.c[i] - c.c[i]) * fx;
        r.c[i] = top + (bot - top) * fy;
    }
    return r;
}

static bool cpu_pixel(const std::vector<uint8_t> &src, uint32_t layer,
                      const float m[9], const WarpTan &f, int x, int y,
                      uint8_t out[4]) {
    float sx = 0, sy = 0, ax = 0, ay = 0, bx = 0, by = 0;
    if (!warp_source_px(m, f, f, kW, kH, kW, kH, x + 0.5f, y + 0.5f, &sx, &sy)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return true;
    }
    const bool okx = warp_source_px(m, f, f, kW, kH, kW, kH, x + 1.5f, y + 0.5f, &ax, &ay);
    const bool oky = warp_source_px(m, f, f, kW, kH, kW, kH, x + 0.5f, y + 1.5f, &bx, &by);
    const double dxx = ax - sx, dxy = ay - sy, dyx = bx - sx, dyy = by - sy;
    auto taps = [](double len) {
        const double c = std::ceil(len - 0.01);
        return (int)(c < 1 ? 1 : c > 4 ? 4 : c);
    };
    const int nx = okx ? taps(std::hypot(dxx, dxy)) : 1;
    const int ny = oky ? taps(std::hypot(dyx, dyy)) : 1;
    double acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            const double ox = (i + 0.5) / nx - 0.5, oy = (j + 0.5) / ny - 0.5;
            const Px p = bilinear(src, layer, sx + ox * dxx + oy * dyx,
                                  sy + ox * dxy + oy * dyy);
            for (int k = 0; k < 4; k++)
                acc[k] += p.c[k];
        }
    for (int k = 0; k < 4; k++)
        acc[k] /= nx * ny;
    for (int k = 0; k < 3; k++)
        out[k] = (uint8_t)std::lround(enc(acc[k]) * 255.0);
    out[3] = (uint8_t)std::lround(acc[3] * 255.0);
    return nx * ny > 1;
}

// ---- the GPU -----------------------------------------------------------------

struct Gpu {
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp{};
};

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

static Img make_image(Gpu &g, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {kW, kH, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Img im;
    vkCreateImage(g.dev, &ii, nullptr, &im.image);
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(g.dev, im.image, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex =
        find_mem(g.mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g.dev, &ai, nullptr, &im.mem);
    vkBindImageMemory(g.dev, im.image, im.mem, 0);
    return im;
}

struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = nullptr;
};

static Buf make_buffer(Gpu &g, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    Buf b;
    vkCreateBuffer(g.dev, &bci, nullptr, &b.buf);
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(g.dev, b.buf, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = find_mem(g.mp, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(g.dev, &ai, nullptr, &b.mem);
    vkBindBufferMemory(g.dev, b.buf, b.mem, 0);
    vkMapMemory(g.dev, b.mem, 0, size, 0, &b.map);
    return b;
}

static void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from,
                    VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Upload `pixels` into src, warp into dst, read dst back.
static bool run_warp(Gpu &g, MenuWarp &warp, const Img &src, const Img &dst,
                     const std::vector<uint8_t> &pixels, const MenuWarp::Params &p,
                     std::vector<uint8_t> *out) {
    const VkDeviceSize bytes = (VkDeviceSize)kW * kH * 4 * kLayers;
    Buf up = make_buffer(g, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Buf back = make_buffer(g, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::memcpy(up.map, pixels.data(), bytes);

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g.dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    barrier(cb, src.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r[kLayers]{};
    for (uint32_t l = 0; l < kLayers; l++) {
        r[l].bufferOffset = (VkDeviceSize)kW * kH * 4 * l;
        r[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
        r[l].imageExtent = {kW, kH, 1};
    }
    vkCmdCopyBufferToImage(cb, up.buf, src.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kLayers, r);
    barrier(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    const MenuWarp::Image s{src.image, VK_FORMAT_B8G8R8A8_UNORM, {kW, kH}, kLayers};
    const MenuWarp::Image d{dst.image, VK_FORMAT_B8G8R8A8_SRGB, {kW, kH}, kLayers};
    const bool recorded = warp.record(cb, s, d, p);

    barrier(cb, dst.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyImageToBuffer(cb, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           back.buf, kLayers, r);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    out->assign((const uint8_t *)back.map, (const uint8_t *)back.map + bytes);
    vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
    for (Buf *b : {&up, &back}) {
        vkUnmapMemory(g.dev, b->mem);
        vkDestroyBuffer(g.dev, b->buf, nullptr);
        vkFreeMemory(g.dev, b->mem, nullptr);
    }
    return recorded;
}

static uint8_t *at(std::vector<uint8_t> &v, uint32_t layer, uint32_t x,
                   uint32_t y) {
    return &v[((size_t)layer * kH * kW + (size_t)y * kW + x) * 4];
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
    const char *exts[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = exts;
    VkInstance inst = VK_NULL_HANDLE;
    const bool validated = vkCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS;
    if (!validated) {
        printf("note validation layer unavailable — barrier checking is OFF\n");
        ici.enabledLayerCount = 0;
        ici.enabledExtensionCount = 0;
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
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    Gpu g;
    if (vkCreateDevice(pd, &dci, nullptr, &g.dev) != VK_SUCCESS) {
        printf("FAIL no device\n");
        return 1;
    }
    vkGetDeviceQueue(g.dev, family, 0, &g.queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &g.mp);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool);

    CursorFns fns;
#define RESOLVE(name) fns.name = (PFN_vk##name)vkGetDeviceProcAddr(g.dev, "vk" #name);
    X4VR_CURSOR_FNS(RESOLVE)
#undef RESOLVE
    ok("every entry point resolves", fns.complete());
    if (!fns.complete())
        return 1;
    MenuWarp warp;
    warp.configure(g.dev, fns);
    ok("the warp reports itself ready", warp.ready());

    const Img src = make_image(g, VK_FORMAT_B8G8R8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const Img dst = make_image(g, VK_FORMAT_B8G8R8A8_SRGB,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const WarpTan field = warp_tan(-55.f / kDeg, 55.f / kDeg, 55.f / kDeg, -55.f / kDeg);
    const float id[4] = {0, 0, 0, 1};

    // The picture: a smooth gradient with every byte value in it, different in
    // each layer, so an unturned warp that blurs, tints, or mixes the eyes up
    // shows in the bytes.
    std::vector<uint8_t> pic((size_t)kW * kH * 4 * kLayers);
    for (uint32_t l = 0; l < kLayers; l++)
        for (uint32_t y = 0; y < kH; y++)
            for (uint32_t x = 0; x < kW; x++) {
                uint8_t *p = at(pic, l, x, y);
                p[0] = (uint8_t)(x ^ (l ? 0x5a : 0));
                p[1] = (uint8_t)y;
                p[2] = (uint8_t)((x * 7 + y * 3 + l * 101) & 0xff);
                p[3] = 255;
            }

    // ---- unturned ------------------------------------------------------------
    {
        MenuWarp::Params p;
        warp_matrix(id, id, p.m);
        p.src[0] = p.src[1] = p.dst[0] = p.dst[1] = field;
        std::vector<uint8_t> out;
        ok("unturned: recorded", run_warp(g, warp, src, dst, pic, p, &out));
        int worst = 0;
        size_t off = 0;
        for (size_t i = 0; i < pic.size(); i++) {
            const int dlt = std::abs((int)out[i] - (int)pic[i]);
            if (dlt)
                off++;
            if (dlt > worst)
                worst = dlt;
        }
        printf("     unturned: worst byte %d, %zu of %zu bytes not exact\n", worst,
               off, pic.size());
        ok("unturned: every byte within 1 of what went in, both layers", worst <= 1);
        ok("unturned: at least 99.9% of bytes exact", off * 1000 < pic.size());
        ok("unturned: layer 1 is layer 1's picture, not layer 0's",
           at(out, 1, 10, 10)[0] == (uint8_t)(10 ^ 0x5a));
    }

    // ---- turned right 20 deg: a bright column moves by trigonometry -----------
    {
        // A white column at X4's angle +10 deg, on black.
        std::vector<uint8_t> col((size_t)kW * kH * 4 * kLayers, 0);
        const double tc = std::tan(10.0 / kDeg);
        const int c = (int)std::floor((tc - field.l) / (field.r - field.l) * kW);
        for (uint32_t l = 0; l < kLayers; l++)
            for (uint32_t y = 0; y < kH; y++) {
                uint8_t *p = at(col, l, c, y);
                p[0] = p[1] = p[2] = 255;
                p[3] = 255;
                at(col, l, 0, y)[3] = 255;
            }
        float shown[4] = {0.f, std::sin(-10.f / kDeg), 0.f, std::cos(-10.f / kDeg)};
        MenuWarp::Params p;
        warp_matrix(id, shown, p.m);
        p.src[0] = p.src[1] = p.dst[0] = p.dst[1] = field;
        std::vector<uint8_t> out;
        run_warp(g, warp, src, dst, col, p, &out);
        // Turned 20 right, the column at +10 is at -10 in the new view.
        const double angle = (std::atan(((c + 0.5) / kW) * (field.r - field.l) + field.l)) * kDeg - 20.0;
        const double want = (std::tan(angle / kDeg) - field.l) / (field.r - field.l) * kW;
        int best = -1, bestv = -1;
        for (uint32_t x = 0; x < kW; x++) {
            const int v = at(out, 0, x, kH / 2)[1];
            if (v > bestv)
                bestv = v, best = (int)x;
        }
        printf("     turned right 20: column at x=%d, trigonometry says %.2f\n", best,
               want - 0.5);
        ok("turned right 20: the bright column is where trigonometry puts it",
           std::fabs(best - (want - 0.5)) <= 1.0);
        ok("turned right 20: and in layer 1 too",
           at(out, 1, best, kH / 2)[1] == bestv);
        // Past +55 in X4's view, i.e. past +35 in the new one: black.
        ok("turned right 20: the right edge, past X4's picture, is black",
           at(out, 0, kW - 1, kH / 2)[0] == 0 && at(out, 0, kW - 1, kH / 2)[1] == 0 &&
               at(out, 0, kW - 1, kH / 2)[3] == 0);
    }

    // ---- turned right 30 deg: every pixel against the CPU rendering ----------
    {
        float shown[4] = {0.f, std::sin(-15.f / kDeg), 0.f, std::cos(-15.f / kDeg)};
        MenuWarp::Params p;
        warp_matrix(id, shown, p.m);
        p.src[0] = p.src[1] = p.dst[0] = p.dst[1] = field;
        std::vector<uint8_t> out;
        run_warp(g, warp, src, dst, pic, p, &out);
        int worst = 0;
        size_t over2 = 0, multi = 0;
        for (uint32_t l = 0; l < kLayers; l++)
            for (uint32_t y = 0; y < kH; y += 3)
                for (uint32_t x = 0; x < kW; x += 3) {
                    uint8_t want[4];
                    if (cpu_pixel(pic, l, p.m, field, (int)x, (int)y, want))
                        multi++;
                    const uint8_t *got = at(out, l, x, y);
                    for (int k = 0; k < 4; k++) {
                        const int dlt = std::abs((int)got[k] - (int)want[k]);
                        worst = dlt > worst ? dlt : worst;
                        over2 += dlt > 2;
                    }
                }
        printf("     turned right 30: worst %d, %zu channel(s) off by more than 2, "
               "%zu sampled pixel(s) used more than one tap\n",
               worst, over2, multi);
        ok("turned right 30: some pixels are minified, so the multi-tap path ran",
           multi > 0);
        ok("turned right 30: every sampled pixel within 2 of the CPU rendering",
           over2 == 0);
    }

    // ---- minified: a 1-pixel checkerboard goes grey, not black or white ------
    {
        std::vector<uint8_t> chk((size_t)kW * kH * 4 * kLayers);
        for (uint32_t l = 0; l < kLayers; l++)
            for (uint32_t y = 0; y < kH; y++)
                for (uint32_t x = 0; x < kW; x++) {
                    uint8_t *q = at(chk, l, x, y);
                    q[0] = q[1] = q[2] = ((x ^ y) & 1) ? 255 : 0;
                    q[3] = 255;
                }
        // 40 deg right: the middle of the view is where X4's picture is packed
        // 1.7x tighter across than the view.
        float shown[4] = {0.f, std::sin(-20.f / kDeg), 0.f, std::cos(-20.f / kDeg)};
        MenuWarp::Params p;
        warp_matrix(id, shown, p.m);
        p.src[0] = p.src[1] = p.dst[0] = p.dst[1] = field;
        std::vector<uint8_t> out;
        run_warp(g, warp, src, dst, chk, p, &out);
        int lo = 255, hi = 0;
        for (uint32_t y = kH / 2 - 8; y < kH / 2 + 8; y++)
            for (uint32_t x = kW / 2 - 8; x < kW / 2 + 8; x++) {
                const int v = at(out, 0, x, y)[1];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
        printf("     minified checkerboard: %d..%d (linear grey encodes to 188)\n",
               lo, hi);
        ok("minified checkerboard: no pixel black or white, it averaged",
           lo >= 100 && hi <= 240);
    }

    warp.shutdown();
    for (const Img *im : {&src, &dst}) {
        vkDestroyImage(g.dev, im->image, nullptr);
        vkFreeMemory(g.dev, im->mem, nullptr);
    }
    vkDestroyCommandPool(g.dev, g.pool, nullptr);
    vkDestroyDevice(g.dev, nullptr);
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
