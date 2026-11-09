// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The headset's loading panel (layer/x4vr_loading_panel.hpp), with no headset:
// which language, the picture, where it stands, when X4 counts as drawing, and,
// when there's a Vulkan device, the one upload, read back byte for byte under the
// validation layer. Self-checking. The pictures are the real ones, from the source
// tree.
//
// The placement is checked against numbers worked by hand, not against the
// header's own formulas: a head turned 90 deg left looks down -X, so the panel
// must stand at -X, facing +X.
#include "../layer/x4vr_loading_panel.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace x4vr;

static int g_bad = 0, g_validation_errors = 0;

static void ok(const std::string &what, bool cond) {
    printf("%-4s %s\n", cond ? "ok" : "FAIL", what.c_str());
    if (!cond)
        g_bad++;
}

static bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

static png::Image load(const std::string &path) {
    png::Image im;
    FILE *f = fopen(path.c_str(), "rb");
    std::vector<uint8_t> b;
    if (f) {
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            b.insert(b.end(), buf, buf + n);
        fclose(f);
    }
    if (!png::decode(b.data(), b.size(), &im))
        printf("FAIL %s didn't decode\n", path.c_str()), g_bad++;
    return im;
}

static void test_language() {
    ok("language: what `run` passes wins",
       loading_language("ru", "de", "fr_FR.UTF-8", nullptr, "en_US.UTF-8") == "ru");
    ok("language: LANGUAGE's first offered one", loading_language(nullptr, "es:pt_BR:en", nullptr,
                                                                  nullptr, "en_US.UTF-8") == "pt_BR");
    ok("language: Portuguese of Portugal is Brazil's words",
       loading_language(nullptr, nullptr, nullptr, nullptr, "pt_PT.UTF-8") == "pt_BR");
    ok("language: German of Austria is German", loading_language(nullptr, nullptr, "de_AT.UTF-8",
                                                                 nullptr, nullptr) == "de");
    ok("language: LC_ALL decides over LANG, even unoffered",
       loading_language(nullptr, nullptr, "es_ES.UTF-8", nullptr, "fr_FR.UTF-8") == "en");
    ok("language: nothing set is English", loading_language(nullptr, nullptr, nullptr, nullptr, nullptr) == "en");
}

static void test_picture(const png::Image &art, const png::Image &words, const png::Image &ru) {
    std::vector<uint8_t> px, bx;
    uint32_t w = 0, h = 0, bw = 0, bh = 0;
    ok("picture: art and words fit together", compose_loading_panel(art, words, false, &px, &w, &h));
    ok("picture: as wide as the art, the words under it",
       w == art.w && h == art.h + words.h && px.size() == (size_t)w * h * 4);
    ok("picture: every language's words fit the same art", ru.w == art.w);
    compose_loading_panel(art, words, true, &bx, &bw, &bh);
    bool art_kept = true, swapped = true, band = false, ink = false;
    for (uint32_t y = 0; y < h && !px.empty(); y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = &px[((size_t)y * w + x) * 4], *q = &bx[((size_t)y * w + x) * 4];
            swapped = swapped && p[0] == q[2] && p[1] == q[1] && p[2] == q[0] && p[3] == 255;
            if (y < art.h) {
                const uint8_t *a = &art.rgba[((size_t)y * art.w + x) * 4];
                art_kept = art_kept && p[0] == a[0] && p[1] == a[1] && p[2] == a[2];
            } else {
                const uint8_t c = words.rgba[((size_t)(y - art.h) * w + x) * 4];
                band = band || (c == 0 && p[0] == 14 && p[1] == 18 && p[2] == 26);
                ink = ink || (c == 255 && p[0] == 236 && p[1] == 240 && p[2] == 245);
            }
        }
    ok("picture: the art's pixels are the art's", art_kept);
    ok("picture: where there are no words, the dark band", band);
    ok("picture: where the words are, light ink", ink);
    ok("picture: B,G,R,A for a BGRA format, the same picture", swapped);
    png::Image narrow = words;
    narrow.w = words.w - 1;
    ok("picture: words of another width are refused", !compose_loading_panel(art, narrow, false, &px, &w, &h));
}

static void test_pad() {
    ok("pad: 738 x 520 rounds up to 768 x 576 by 64",
       loading_round_up(738, 64) == 768 && loading_round_up(520, 64) == 576);
    ok("pad: a multiple already stays", loading_round_up(512, 64) == 512);
    std::vector<uint8_t> px(3 * 2 * 4);
    for (size_t i = 0; i < px.size(); i++)
        px[i] = (uint8_t)(i + 1);
    const std::vector<uint8_t> p = loading_pad(px, 3, 2, 4, 3);
    bool corner = p.size() == 4 * 3 * 4, clear = true;
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 4; x++)
            for (uint32_t c = 0; c < 4; c++) {
                const uint8_t v = p[((size_t)y * 4 + x) * 4 + c];
                if (y < 2 && x < 3)
                    corner = corner && v == px[((size_t)y * 3 + x) * 4 + c];
                else
                    clear = clear && v == 0;
            }
    ok("pad: the picture in the top-left corner, row by row", corner);
    ok("pad: the rest clear", clear);
}

static void test_shapes() {
    const VkFormat R = VK_FORMAT_R8G8B8A8_SRGB, B = VK_FORMAT_B8G8R8A8_SRGB;
    const std::vector<LoadingShape> s = loading_shapes(R, B, 738, 520);
    ok("shapes: four, when the first format isn't the eye's", s.size() == 4);
    ok("shapes: the picture's own shape first",
       s.size() == 4 && s[0] == LoadingShape{R, 738, 520, 1});
    ok("shapes: then only the format changes", s.size() == 4 && s[1] == LoadingShape{B, 738, 520, 1});
    ok("shapes: then only the size, to multiples of 64",
       s.size() == 4 && s[2] == LoadingShape{B, 768, 576, 1});
    ok("shapes: then only the layers, to the eye's two",
       s.size() == 4 && s[3] == LoadingShape{B, 768, 576, 2});
    const std::vector<LoadingShape> same = loading_shapes(B, B, 738, 520);
    ok("shapes: the eye's format already, no step that changes nothing",
       same.size() == 3 && same[0] == LoadingShape{B, 738, 520, 1} &&
           same[1] == LoadingShape{B, 768, 576, 1});
    const std::vector<LoadingShape> round = loading_shapes(R, B, 768, 576);
    ok("shapes: a size already in 64s skips the size step",
       round.size() == 3 && round[2] == LoadingShape{B, 768, 576, 2});
}

static void test_place() {
    const float id[4] = {0.f, 0.f, 0.f, 1.f};
    const float s = std::sqrt(0.5f);
    const float left[4] = {0.f, s, 0.f, s};   // 90 deg about +Y: looking down -X
    const float right[4] = {0.f, -s, 0.f, s}; // looking down +X
    const float down30[4] = {-std::sin(0.2617994f), 0.f, 0.f, std::cos(0.2617994f)};
    ok("place: ahead is heading 0", near(loading_yaw(id), 0.f));
    ok("place: turned 90 left is +90 deg", near(loading_yaw(left), 1.5707963f));
    ok("place: turned 90 right is -90 deg", near(loading_yaw(right), -1.5707963f));
    ok("place: looking down doesn't change the heading", near(loading_yaw(down30), 0.f));
    const float head[3] = {0.2f, 1.6f, -0.1f};
    LoadingPlace p = loading_place(head, 0.f, 1.8f);
    ok("place: straight ahead, 1.8 m down -Z, at eye height",
       near(p.pos[0], 0.2f) && near(p.pos[1], 1.6f) && near(p.pos[2], -1.9f));
    p = loading_place(head, loading_yaw(left), 1.8f);
    ok("place: turned left, 1.8 m down -X", near(p.pos[0], -1.6f) && near(p.pos[2], -0.1f));
    // The quad shows its +Z face: rotated by the panel's orientation, that must
    // point back at the head (+X here).
    const float nx = 2.f * (p.q[0] * p.q[2] + p.q[3] * p.q[1]);
    const float nz = 1.f - 2.f * (p.q[0] * p.q[0] + p.q[1] * p.q[1]);
    ok("place: ...facing the head", near(nx, 1.f, 1e-3f) && near(nz, 0.f, 1e-3f));
    ok("place: 60 deg apart is past the turn that moves it",
       loading_yaw_apart(1.0472f, 0.f) > kLoadingRecentreDeg);
    ok("place: 179 and -179 deg are 2 apart", near(loading_yaw_apart(3.1241f, -3.1241f), 2.f, 0.05f));
}

static void test_gate() {
    LoadingGate g;
    g.observe(0.0, 0);
    ok("gate: no frames, not drawing", !g.steady());
    g.observe(1.0, 3);
    g.observe(7.5, 3); // X4's first frames, then a pause: take of 2026-11-09
    ok("gate: three frames and a pause aren't drawing", !g.steady());
    for (int i = 1; i <= 9; i++)
        g.observe(10.0 + i * 0.1, 3 + i);
    ok("gate: 9 frames in a second aren't yet", !g.steady());
    g.observe(11.0, 13);
    ok("gate: the 10th within 1.5 s is drawing", g.steady() && near((float)g.steady_at(), 11.f));
    g.observe(30.0, 13);
    ok("gate: and stays so through a later pause", g.steady());
    LoadingGate slow;
    for (int i = 1; i <= 10; i++)
        slow.observe(i * 0.3, i);
    ok("gate: 10 frames over 3 s aren't a pace", !slow.steady());
    for (int i = 11; i <= 120; i++)
        slow.observe(i * 0.3, i);
    ok("gate: 120 frames in all are, however slow", slow.steady());
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *) {
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_validation_errors++;
        printf("     validation: %s\n", d->pMessage ? d->pMessage : "(none)");
    }
    return VK_FALSE;
}

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties &mp, uint32_t bits,
                         VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

// The upload into an image like the runtime's, then read back.
// Into an image of `nlayers` layers, as each shape of loading_shapes asks; the
// picture goes to layer 0, the one the quad names.
static void test_upload(const std::vector<uint8_t> &px, uint32_t w, uint32_t h, uint32_t nlayers,
                        const std::string &shape) {
    printf("     upload into %ux%u x%u (%s)\n", w, h, nlayers, shape.c_str());
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
        ici.enabledLayerCount = ici.enabledExtensionCount = 0;
        if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
            printf("skip upload: no Vulkan here\n");
            return;
        }
        printf("note the validation layer isn't here: layouts go unchecked\n");
    }
    VkDebugUtilsMessengerEXT msgr = VK_NULL_HANDLE;
    if (validated) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = debug_cb;
        if (create)
            create(inst, &mi, nullptr, &msgr);
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> phys(n);
    if (!n || vkEnumeratePhysicalDevices(inst, &n, phys.data()) != VK_SUCCESS) {
        printf("skip upload: no GPU here\n");
        vkDestroyInstance(inst, nullptr);
        return;
    }
    VkPhysicalDevice pd = phys[0];
    uint32_t qn = 0, family = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn && family == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            family = i;
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
    VkDevice dev = VK_NULL_HANDLE;
    if (family == UINT32_MAX || vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) {
        printf("FAIL no device\n");
        g_bad++;
        return;
    }
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    // The runtime's image: sRGB, a colour attachment it can be told to copy into.
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_SRGB;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = nlayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    vkCreateImage(dev, &ii, nullptr, &img);
    VkMemoryRequirements ir{};
    vkGetImageMemoryRequirements(dev, img, &ir);
    VkMemoryAllocateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ia.allocationSize = ir.size;
    ia.memoryTypeIndex = find_mem(mp, ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev, &ia, nullptr, &imem);
    vkBindImageMemory(dev, img, imem, 0);

    LoadingFns lf;
#define RESOLVE(name) lf.name = (PFN_vk##name)vkGetDeviceProcAddr(dev, "vk" #name);
    X4VR_LOADING_FNS(RESOLVE)
#undef RESOLVE
    ok("upload: every entry point resolves", lf.complete());
    LoadingUpload up;
    up.configure(dev, lf, mp);
    std::string why;
    int submits = 0;
    const bool done = up.upload(
        img, w, h, px, family, 0,
        [&](VkQueue q, const VkSubmitInfo &si, VkFence f) {
            submits++;
            return vkQueueSubmit(q, 1, &si, f);
        },
        &why);
    ok("upload: done, through the one submit the caller gave it" + (why.empty() ? "" : " (" + why + ")"),
       done && submits == 1);

    // Read back, from the layout OpenXR takes a released image in.
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rm = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = px.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(dev, &bi, nullptr, &rb);
    VkMemoryRequirements br{};
    vkGetBufferMemoryRequirements(dev, rb, &br);
    VkMemoryAllocateInfo ba{};
    ba.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ba.allocationSize = br.size;
    ba.memoryTypeIndex = find_mem(mp, br.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &ba, nullptr, &rm);
    vkBindBufferMemory(dev, rb, rm, 0);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    vkCreateCommandPool(dev, &pci, nullptr, &pool);
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &cbi);
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &r);
    vkEndCommandBuffer(cb);
    VkQueue q = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, family, 0, &q);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    void *map = nullptr;
    vkMapMemory(dev, rm, 0, VK_WHOLE_SIZE, 0, &map);
    ok("upload: the image holds the picture, byte for byte",
       map && memcmp(map, px.data(), px.size()) == 0);
    vkUnmapMemory(dev, rm);
    ok("upload: the validation layer had nothing to say" +
           std::string(validated ? "" : " (it isn't here)"),
       g_validation_errors == 0);

    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, rb, nullptr);
    vkFreeMemory(dev, rm, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, imem, nullptr);
    vkDestroyDevice(dev, nullptr);
    if (msgr) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(inst, msgr, nullptr);
    }
    vkDestroyInstance(inst, nullptr);
}

int main() {
    const std::string art_dir = X4VR_SOURCE_DIR "/launcher/art/";
    const png::Image art = load(art_dir + "loading-screen.png");
    const png::Image en = load(art_dir + "vr-loading-en.png");
    const png::Image ru = load(art_dir + "vr-loading-ru.png");
    test_language();
    test_picture(art, en, ru);
    test_pad();
    test_shapes();
    test_place();
    test_gate();
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    if (compose_loading_panel(art, ru, false, &px, &w, &h)) {
        test_upload(px, w, h, 1, "the picture's own shape");
        const uint32_t pw = loading_round_up(w, 64), ph = loading_round_up(h, 64);
        test_upload(loading_pad(px, w, h, pw, ph), pw, ph, 2, "the last shape, padded");
    }
    printf("%s\n", g_bad ? "FAILED" : "all passed");
    return g_bad ? 1 : 0;
}
