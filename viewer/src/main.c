/*
 * x4vr-viewer — milestone 2: head-locked stereo from X4's live SBS frame.
 *
 * Reads the composited side-by-side frame that the DIBR layer (our vkShade,
 * X4VR_EXPORT=1) publishes to POSIX shm, uploads it to one full-width OpenXR
 * swapchain, and presents it as two head-locked XrCompositionLayerQuad layers
 * whose imageRect selects the left / right half for each eye. Pure OpenXR via
 * XR_KHR_vulkan_enable2, so it runs on WiVRn/Monado and SteamVR.
 *
 * Next: fold in HMD-pose -> OpenTrack-UDP for 6DOF look-around (absorbing
 * bridge/xr2x4) and a recenter; tune quad distance/size.
 *
 * Build:  cmake -S . -B build && cmake --build build
 * Run:    (X4 already running with X4VR_EXPORT=1)
 *         XR_RUNTIME_JSON=~/.config/openxr/1/active_runtime.json ./build/x4vr-viewer
 */

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "x4vr_shm.h"

/* ------------------------------- config ---------------------------------- */

/* Head-locked quad placement (meters), relative to VIEW space (the head).
 * Live-tunable via env so depth/FOV/aspect can be dialled in without a
 * rebuild: X4VR_QUAD_W, X4VR_QUAD_H, X4VR_QUAD_DIST.
 *
 * Note on aspect: at 2560x1280 each eye is a square (1280x1280) holding the
 * game's 2:1-wide view squeezed in — i.e. horizontally compressed. A wider
 * quad (W > H, e.g. W=2*H) un-squishes it, which also helps stereo fusion.
 * Defaults below are a large-ish square; widen W if geometry looks squished. */
static float quad_w = 2.4f, quad_h = 2.4f, quad_dist = 1.4f;

/* Cylinder layer (XR_KHR_composition_layer_cylinder): a curved wrap-around
 * screen that fills the horizontal FOV more naturally than a flat quad.
 *   radius       = distance to the surface (m)
 *   central_angle= horizontal arc the image wraps (degrees)
 *   aspect       = displayed width:height. 2.0 un-squishes the 2:1 game view
 *                  held in each square eye; LOWER = taller. */
/* ANGLE should match X4's horizontal FOV (else content is angularly
 * magnified/minified). ASPECT (= image width:height) MUST equal the game
 * window's aspect to look undistorted — each eye holds the full window view
 * squeezed into its half, so a 2:1 window needs a 2:1 display. By default we
 * AUTO-DERIVE aspect from the shm dimensions (sbs_w/sbs_h); env overrides. */
static float cyl_radius = 1.4f, cyl_angle_deg = 120.0f, cyl_aspect = 2.0f;

/* Vertical offset of the image (m, VIEW space; negative = down). Headsets
 * (Quest 3 included) have an asymmetric vertical FOV — more visible below
 * the horizontal axis than above — so a symmetric layer centered at eye
 * level leaves a black gap at the bottom. Nudge it down to recenter. */
static float view_voffset = 0.0f;

/* layer mode: true = cylinder (default, if the runtime supports it). */
static bool want_cylinder = true;
static bool have_cylinder_ext = false; /* set after extension enumeration */
static bool cyl_aspect_set = false;    /* did the user pin aspect via env? */
static bool quad_h_set = false;

static void read_view_env(void)
{
    const char *e;
    if ((e = getenv("X4VR_LAYER")))
        want_cylinder = (strcmp(e, "quad") != 0);
    if ((e = getenv("X4VR_QUAD_W")))     quad_w = (float)atof(e);
    if ((e = getenv("X4VR_QUAD_H")))     { quad_h = (float)atof(e); quad_h_set = true; }
    if ((e = getenv("X4VR_QUAD_DIST")))  quad_dist = (float)atof(e);
    if ((e = getenv("X4VR_CYL_RADIUS"))) cyl_radius = (float)atof(e);
    if ((e = getenv("X4VR_CYL_ANGLE")))  cyl_angle_deg = (float)atof(e);
    if ((e = getenv("X4VR_CYL_ASPECT"))) { cyl_aspect = (float)atof(e); cyl_aspect_set = true; }
    if ((e = getenv("X4VR_VOFFSET")))    view_voffset = (float)atof(e);
}

/* ------------------------------- globals --------------------------------- */

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* OpenXR */
static XrInstance xr_instance = XR_NULL_HANDLE;
static XrSystemId xr_system = XR_NULL_SYSTEM_ID;
static XrSession xr_session = XR_NULL_HANDLE;
static XrSpace xr_view_space = XR_NULL_HANDLE;
static XrEnvironmentBlendMode xr_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

/* Vulkan (created via the OpenXR vulkan_enable2 helpers) */
static VkInstance vk_instance = VK_NULL_HANDLE;
static VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
static VkDevice vk_device = VK_NULL_HANDLE;
static uint32_t vk_queue_family = 0;
static VkQueue vk_queue = VK_NULL_HANDLE;
static VkCommandPool vk_cmd_pool = VK_NULL_HANDLE;

/* shm source */
static int shm_fd = -1;
static uint8_t *shm_base = NULL;
static size_t shm_size = 0;
static volatile X4VRShmHeader *shm_hdr = NULL;
static uint8_t *shm_data = NULL;
static uint32_t sbs_w = 0, sbs_h = 0; /* full SBS dimensions */
static uint32_t eye_w = 0;            /* per-eye width = sbs_w / 2 */

/* Derive undistorted aspect from the source once shm dimensions are known.
 * Each eye holds the full game-window view squeezed into its half, so the
 * correct display aspect equals the game window aspect (sbs_w/sbs_h). */
static void apply_source_aspect(void)
{
    float src = (float)sbs_w / (float)sbs_h;
    if (!cyl_aspect_set) cyl_aspect = src;
    if (!quad_h_set)     quad_h = quad_w / src; /* keep W:H = window aspect */
    fprintf(stderr, "source aspect %ux%u = %.3f -> cyl_aspect %.3f, "
            "quad %.2fx%.2f%s\n", sbs_w, sbs_h, src, cyl_aspect,
            quad_w, quad_h, cyl_aspect_set ? " (aspect pinned via env)" : "");
}

/* one full-SBS swapchain + an upload staging buffer */
static XrSwapchain swapchain = XR_NULL_HANDLE;
static uint32_t sc_image_count = 0;
static XrSwapchainImageVulkanKHR *sc_images = NULL;
static VkBuffer staging = VK_NULL_HANDLE;
static VkDeviceMemory staging_mem = VK_NULL_HANDLE;
static void *staging_mapped = NULL;

/* ------------------------------- helpers --------------------------------- */

#define XR_CHECK(call, what)                                                  \
    do {                                                                      \
        XrResult _r = (call);                                                 \
        if (XR_FAILED(_r)) {                                                  \
            char _b[XR_MAX_RESULT_STRING_SIZE] = "?";                         \
            if (xr_instance) xrResultToString(xr_instance, _r, _b);           \
            fprintf(stderr, "FATAL %s: %s (%d)\n", what, _b, _r);             \
            return false;                                                     \
        }                                                                     \
    } while (0)

#define VK_CHECK(call, what)                                                  \
    do {                                                                      \
        VkResult _r = (call);                                                 \
        if (_r != VK_SUCCESS) {                                               \
            fprintf(stderr, "FATAL %s: VkResult %d\n", what, _r);             \
            return false;                                                     \
        }                                                                     \
    } while (0)

static uint32_t find_mem_type(uint32_t filter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((filter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

/* --------------------------------- shm ----------------------------------- */

/* Open the shm and wait for the writer to publish a frame. Returns false on
 * timeout (X4 not running with X4VR_EXPORT=1). */
static bool open_shm(int timeout_s)
{
    fprintf(stderr, "waiting for X4 SBS frame on shm %s "
                    "(start X4 with X4VR_EXPORT=1)...\n", X4VR_SHM_NAME);
    for (int waited = 0; waited <= timeout_s * 10 && !g_quit; waited++) {
        if (shm_fd < 0)
            shm_fd = shm_open(X4VR_SHM_NAME, O_RDONLY, 0);
        if (shm_fd >= 0) {
            X4VRShmHeader h;
            if (pread(shm_fd, &h, sizeof(h), 0) == (long)sizeof(h) &&
                h.magic == X4VR_SHM_MAGIC && h.seq > 0 &&
                h.width > 1 && h.height > 0) {
                shm_size = (size_t)h.data_offset + (size_t)h.buffers * h.buffer_bytes;
                shm_base = mmap(NULL, shm_size, PROT_READ, MAP_SHARED, shm_fd, 0);
                if (shm_base == MAP_FAILED) { perror("mmap shm"); return false; }
                shm_hdr = (volatile X4VRShmHeader *)shm_base;
                shm_data = shm_base + h.data_offset;
                sbs_w = h.width; sbs_h = h.height; eye_w = h.width / 2;
                fprintf(stderr, "SBS source: %ux%u (per-eye %ux%u), fmt=%u\n",
                        sbs_w, sbs_h, eye_w, sbs_h, h.format);
                return true;
            }
        }
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "timed out waiting for shm\n");
    return false;
}

/* Map the shm pixel format to a Vulkan swapchain format (match channel order
 * so the upload is a straight copy; UNORM — the bytes are display-ready). */
static int64_t shm_to_vk_format(void)
{
    return (shm_hdr->format == X4VR_FMT_R8G8B8A8)
        ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
}

/* ------------------------------- OpenXR ---------------------------------- */

#define XR_PFN(name) \
    PFN_##name name = NULL; \
    xrGetInstanceProcAddr(xr_instance, #name, (PFN_xrVoidFunction *)&name)

static bool create_xr_instance(void)
{
    /* Discover whether the cylinder layer extension is available. */
    uint32_t ext_n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_n, NULL);
    XrExtensionProperties *ext_props = calloc(ext_n, sizeof(XrExtensionProperties));
    for (uint32_t i = 0; i < ext_n; i++) ext_props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, ext_n, &ext_n, ext_props);
    for (uint32_t i = 0; i < ext_n; i++)
        if (!strcmp(ext_props[i].extensionName,
                    XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
            have_cylinder_ext = true;
    free(ext_props);

    const char *exts[2] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    uint32_t ext_count = 1;
    if (want_cylinder && have_cylinder_ext)
        exts[ext_count++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
    else if (want_cylinder && !have_cylinder_ext)
        fprintf(stderr, "note: runtime lacks %s — falling back to flat quad\n",
                XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);

    XrInstanceCreateInfo ci = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationName = "x4vr-viewer",
            .applicationVersion = 1,
            .engineName = "none",
            .apiVersion = XR_API_VERSION_1_0,
        },
        .enabledExtensionCount = ext_count,
        .enabledExtensionNames = exts,
    };
    XR_CHECK(xrCreateInstance(&ci, &xr_instance),
             "xrCreateInstance (is WiVRn/Monado/SteamVR running?)");

    XrSystemGetInfo sgi = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XR_CHECK(xrGetSystem(xr_instance, &sgi, &xr_system), "xrGetSystem");

    XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(xr_instance, xr_system, &sp)))
        fprintf(stderr, "HMD: %s\n", sp.systemName);

    uint32_t bm_count = 0;
    xrEnumerateEnvironmentBlendModes(xr_instance, xr_system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bm_count, NULL);
    if (bm_count) {
        XrEnvironmentBlendMode *modes = calloc(bm_count, sizeof(*modes));
        xrEnumerateEnvironmentBlendModes(xr_instance, xr_system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bm_count, &bm_count, modes);
        xr_blend = modes[0];
        free(modes);
    }
    return true;
}

static bool create_vulkan_via_xr(void)
{
    XR_PFN(xrGetVulkanGraphicsRequirements2KHR);
    XR_PFN(xrCreateVulkanInstanceKHR);
    XR_PFN(xrGetVulkanGraphicsDevice2KHR);
    XR_PFN(xrCreateVulkanDeviceKHR);
    if (!xrGetVulkanGraphicsRequirements2KHR || !xrCreateVulkanInstanceKHR ||
        !xrGetVulkanGraphicsDevice2KHR || !xrCreateVulkanDeviceKHR) {
        fprintf(stderr, "FATAL: vulkan_enable2 entry points missing\n");
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR req = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    XR_CHECK(xrGetVulkanGraphicsRequirements2KHR(xr_instance, xr_system, &req),
             "xrGetVulkanGraphicsRequirements2KHR");

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "x4vr-viewer",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo vici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    XrVulkanInstanceCreateInfoKHR xvi = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
        .systemId = xr_system,
        .pfnGetInstanceProcAddr = &vkGetInstanceProcAddr,
        .vulkanCreateInfo = &vici,
    };
    VkResult vk_err = VK_SUCCESS;
    XR_CHECK(xrCreateVulkanInstanceKHR(xr_instance, &xvi, &vk_instance, &vk_err),
             "xrCreateVulkanInstanceKHR");
    if (vk_err != VK_SUCCESS) { fprintf(stderr, "VkInstance err %d\n", vk_err); return false; }

    XrVulkanGraphicsDeviceGetInfoKHR gdi = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = xr_system,
        .vulkanInstance = vk_instance,
    };
    XR_CHECK(xrGetVulkanGraphicsDevice2KHR(xr_instance, &gdi, &vk_phys),
             "xrGetVulkanGraphicsDevice2KHR");

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qf_count, NULL);
    VkQueueFamilyProperties *qfs = calloc(qf_count, sizeof(*qfs));
    vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qf_count, qfs);
    bool found = false;
    for (uint32_t i = 0; i < qf_count; i++)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk_queue_family = i; found = true; break; }
    free(qfs);
    if (!found) { fprintf(stderr, "no graphics queue family\n"); return false; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk_queue_family, .queueCount = 1, .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
    };
    XrVulkanDeviceCreateInfoKHR xdi = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
        .systemId = xr_system,
        .pfnGetInstanceProcAddr = &vkGetInstanceProcAddr,
        .vulkanPhysicalDevice = vk_phys,
        .vulkanCreateInfo = &dci,
    };
    XR_CHECK(xrCreateVulkanDeviceKHR(xr_instance, &xdi, &vk_device, &vk_err),
             "xrCreateVulkanDeviceKHR");
    if (vk_err != VK_SUCCESS) { fprintf(stderr, "VkDevice err %d\n", vk_err); return false; }

    vkGetDeviceQueue(vk_device, vk_queue_family, 0, &vk_queue);
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk_queue_family,
    };
    VK_CHECK(vkCreateCommandPool(vk_device, &pci, NULL, &vk_cmd_pool), "vkCreateCommandPool");
    return true;
}

static bool create_session_and_space(void)
{
    XrGraphicsBindingVulkan2KHR gb = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = vk_instance, .physicalDevice = vk_phys, .device = vk_device,
        .queueFamilyIndex = vk_queue_family, .queueIndex = 0,
    };
    XrSessionCreateInfo sci = {
        .type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = xr_system,
    };
    XR_CHECK(xrCreateSession(xr_instance, &sci, &xr_session), "xrCreateSession");

    XrReferenceSpaceCreateInfo rsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
        .poseInReferenceSpace = { .orientation = { .w = 1.0f } },
    };
    XR_CHECK(xrCreateReferenceSpace(xr_session, &rsci, &xr_view_space),
             "xrCreateReferenceSpace(VIEW)");
    return true;
}

/* One swapchain at the full SBS size; verify the runtime offers our format. */
static bool create_swapchain(void)
{
    int64_t want = shm_to_vk_format();
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(xr_session, 0, &n, NULL);
    int64_t *fmts = calloc(n, sizeof(int64_t));
    xrEnumerateSwapchainFormats(xr_session, n, &n, fmts);
    bool ok = false;
    for (uint32_t i = 0; i < n; i++) if (fmts[i] == want) { ok = true; break; }
    int64_t chosen = ok ? want : fmts[0];
    free(fmts);
    if (!ok)
        fprintf(stderr, "warning: preferred format %ld unsupported, using %ld "
                "(colors/channels may be off)\n", (long)want, (long)chosen);

    XrSwapchainCreateInfo ci = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = chosen, .sampleCount = 1,
        .width = sbs_w, .height = sbs_h,
        .faceCount = 1, .arraySize = 1, .mipCount = 1,
    };
    XR_CHECK(xrCreateSwapchain(xr_session, &ci, &swapchain), "xrCreateSwapchain");
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, 0, &sc_image_count, NULL),
             "xrEnumerateSwapchainImages(count)");
    sc_images = calloc(sc_image_count, sizeof(XrSwapchainImageVulkanKHR));
    for (uint32_t i = 0; i < sc_image_count; i++)
        sc_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    XR_CHECK(xrEnumerateSwapchainImages(swapchain, sc_image_count, &sc_image_count,
                                        (XrSwapchainImageBaseHeader *)sc_images),
             "xrEnumerateSwapchainImages");
    return true;
}

static bool create_staging(void)
{
    VkDeviceSize bytes = (VkDeviceSize)sbs_w * sbs_h * 4;
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(vk_device, &bi, NULL, &staging), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk_device, staging, &mr);
    /* CPU writes into this buffer (memcpy from shm), GPU reads it — coherent
     * (write-combined) is fine for writes. */
    uint32_t mt = find_mem_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) { fprintf(stderr, "no host-visible mem type\n"); return false; }
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = mt,
    };
    VK_CHECK(vkAllocateMemory(vk_device, &ai, NULL, &staging_mem), "vkAllocateMemory");
    VK_CHECK(vkBindBufferMemory(vk_device, staging, staging_mem, 0), "vkBindBufferMemory");
    VK_CHECK(vkMapMemory(vk_device, staging_mem, 0, bytes, 0, &staging_mapped), "vkMapMemory");
    return true;
}

/* Copy the newest shm frame into the acquired swapchain image. */
static bool upload_frame(VkImage image)
{
    /* newest published frame; (seq-1)%buffers is the buffer not being written */
    uint64_t seq = __atomic_load_n(&shm_hdr->seq, __ATOMIC_ACQUIRE);
    if (seq == 0) return true;
    uint32_t idx = (uint32_t)((seq - 1) % shm_hdr->buffers);
    memcpy(staging_mapped, shm_data + (size_t)idx * shm_hdr->buffer_bytes,
           (size_t)sbs_w * sbs_h * 4);

    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk_cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk_device, &ai, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .image = image, .subresourceRange = range,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);

    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = sbs_w, .bufferImageHeight = sbs_h,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 }, .imageExtent = { sbs_w, sbs_h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_color = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .image = image, .subresourceRange = range,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_color);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK_CHECK(vkQueueSubmit(vk_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    vkQueueWaitIdle(vk_queue);
    vkFreeCommandBuffers(vk_device, vk_cmd_pool, 1, &cmd);
    return true;
}

static bool render_frame(void)
{
    XrFrameWaitInfo fwi = { .type = XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
    XR_CHECK(xrWaitFrame(xr_session, &fwi, &fs), "xrWaitFrame");
    XrFrameBeginInfo fbi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XR_CHECK(xrBeginFrame(xr_session, &fbi), "xrBeginFrame");

    /* Storage for whichever layer type we emit (cylinder or quad), 2 each. */
    XrCompositionLayerCylinderKHR cyls[2];
    XrCompositionLayerQuad quads[2];
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layer_count = 0;
    bool use_cyl = want_cylinder && have_cylinder_ext;

    if (fs.shouldRender) {
        uint32_t img = 0;
        XrSwapchainImageAcquireInfo ai = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XR_CHECK(xrAcquireSwapchainImage(swapchain, &ai, &img), "xrAcquireSwapchainImage");
        XrSwapchainImageWaitInfo wi = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION };
        XR_CHECK(xrWaitSwapchainImage(swapchain, &wi), "xrWaitSwapchainImage");

        if (!upload_frame(sc_images[img].image)) return false;

        XrSwapchainImageReleaseInfo ri = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        XR_CHECK(xrReleaseSwapchainImage(swapchain, &ri), "xrReleaseSwapchainImage");

        /* Both layer types: same swapchain, each eye sees its SBS half via
         * imageRect + eyeVisibility, centered in front in head-locked VIEW
         * space. */
        for (int e = 0; e < 2; e++) {
            XrSwapchainSubImage sub = {
                .swapchain = swapchain,
                .imageRect = { { (int32_t)(e * eye_w), 0 },
                               { (int32_t)eye_w, (int32_t)sbs_h } },
                .imageArrayIndex = 0,
            };
            XrEyeVisibility eye = e == 0 ? XR_EYE_VISIBILITY_LEFT
                                         : XR_EYE_VISIBILITY_RIGHT;
            XrPosef pose = { .orientation = { 0, 0, 0, 1 },
                             .position = { 0, view_voffset,
                                           use_cyl ? 0.0f : -quad_dist } };
            if (use_cyl) {
                cyls[e] = (XrCompositionLayerCylinderKHR){
                    .type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,
                    .space = xr_view_space,
                    .eyeVisibility = eye,
                    .subImage = sub,
                    .pose = pose,
                    .radius = cyl_radius,
                    .centralAngle = (float)(cyl_angle_deg * M_PI / 180.0),
                    .aspectRatio = cyl_aspect,
                };
                layers[e] = (const XrCompositionLayerBaseHeader *)&cyls[e];
            } else {
                quads[e] = (XrCompositionLayerQuad){
                    .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
                    .space = xr_view_space,
                    .eyeVisibility = eye,
                    .subImage = sub,
                    .pose = pose,
                    .size = { quad_w, quad_h },
                };
                layers[e] = (const XrCompositionLayerBaseHeader *)&quads[e];
            }
        }
        layer_count = 2;
    }

    XrFrameEndInfo fei = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = fs.predictedDisplayTime,
        .environmentBlendMode = xr_blend,
        .layerCount = layer_count,
        .layers = layer_count ? layers : NULL,
    };
    XR_CHECK(xrEndFrame(xr_session, &fei), "xrEndFrame");
    return true;
}

static bool run_loop(void)
{
    bool running = false;
    while (!g_quit) {
        XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(xr_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrEventDataSessionStateChanged *sc = (void *)&ev;
                if (sc->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {
                        .type = XR_TYPE_SESSION_BEGIN_INFO,
                        .primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                    XR_CHECK(xrBeginSession(xr_session, &bi), "xrBeginSession");
                    running = true;
                    fprintf(stderr, "session running\n");
                } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(xrEndSession(xr_session), "xrEndSession");
                    running = false;
                } else if (sc->state == XR_SESSION_STATE_EXITING ||
                           sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                    g_quit = 1;
                }
            }
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        }
        if (running) { if (!render_frame()) return false; }
        else { struct timespec ts = {0, 20 * 1000 * 1000}; nanosleep(&ts, NULL); }
    }
    return true;
}

static void cleanup(void)
{
    if (swapchain) xrDestroySwapchain(swapchain);
    free(sc_images);
    if (staging_mapped) vkUnmapMemory(vk_device, staging_mem);
    if (staging) vkDestroyBuffer(vk_device, staging, NULL);
    if (staging_mem) vkFreeMemory(vk_device, staging_mem, NULL);
    if (xr_view_space) xrDestroySpace(xr_view_space);
    if (xr_session) xrDestroySession(xr_session);
    if (vk_cmd_pool) vkDestroyCommandPool(vk_device, vk_cmd_pool, NULL);
    if (vk_device) vkDestroyDevice(vk_device, NULL);
    if (vk_instance) vkDestroyInstance(vk_instance, NULL);
    if (xr_instance) xrDestroyInstance(xr_instance);
    if (shm_base) munmap(shm_base, shm_size);
    if (shm_fd >= 0) close(shm_fd);
}

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int rc = 1;
    read_view_env();
    if (!open_shm(60)) goto out;           /* wait up to 60s for X4 */
    apply_source_aspect();                 /* auto aspect from shm dims */
    if (!create_xr_instance()) goto out;   /* sets have_cylinder_ext */
    if (!create_vulkan_via_xr()) goto out;
    if (!create_session_and_space()) goto out;
    if (!create_swapchain()) goto out;
    if (!create_staging()) goto out;

    if (want_cylinder && have_cylinder_ext) {
        /* derived vertical extent: height = radius*angle/aspect */
        double h = cyl_radius * (cyl_angle_deg * M_PI / 180.0) / cyl_aspect;
        double vdeg = 2.0 * atan((h / 2.0) / cyl_radius) * 180.0 / M_PI;
        fprintf(stderr, "layer: cylinder (radius %.2f m, %.0f deg H arc, "
                "aspect %.2f -> ~%.0f deg V, voffset %.2f)\n",
                cyl_radius, cyl_angle_deg, cyl_aspect, vdeg, view_voffset);
    }
    else
        fprintf(stderr, "layer: flat quad (%.2f x %.2f m at %.2f m, ~%.0f deg, voffset %.2f)\n",
                quad_w, quad_h, quad_dist,
                2.0 * atan((quad_w / 2.0) / quad_dist) * 180.0 / M_PI, view_voffset);
    fprintf(stderr, "init OK — streaming X4 SBS to the headset. Ctrl-C to quit.\n");
    if (!run_loop()) goto out;
    rc = 0;
out:
    cleanup();
    return rc;
}
