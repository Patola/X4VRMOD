/*
 * x4vr-viewer — milestone 1: head-locked stereo quad layers (test pattern).
 *
 * Brings up an OpenXR session with a Vulkan graphics binding (via
 * XR_KHR_vulkan_enable2, so it works on WiVRn/Monado and SteamVR alike),
 * creates two quad-layer swapchains, fills them with a distinct per-eye test
 * pattern, and submits them as two head-locked XrCompositionLayerQuad layers
 * (eyeVisibility LEFT / RIGHT). Purpose: verify the OpenXR<->Vulkan handshake
 * and correct per-eye placement before wiring real content.
 *
 * Next milestones: replace the test pattern with a PipeWire capture of the
 * X4 SBS window (left half -> left eye, right half -> right eye), then fold in
 * the HMD-pose -> OpenTrack-UDP sender (absorbing bridge/xr2x4).
 *
 * Build:  cmake -S . -B build && cmake --build build
 * Run:    XR_RUNTIME_JSON=~/.config/openxr/1/active_runtime.json ./build/x4vr-viewer
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* ------------------------------- config ---------------------------------- */

/* Per-eye test-pattern swapchain size (square, matching one 1280x1280 SBS
 * half once capture is wired). Small here — it is just a flat fill. */
#define EYE_W 256
#define EYE_H 256

/* Head-locked quad placement (meters), relative to the VIEW space (the head).
 * 1.6 m wide screen, 1.6 m tall, 1.8 m in front. Tune later. */
static const float QUAD_DISTANCE = 1.8f;
static const float QUAD_SIZE_M = 1.6f;

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

/* Per-eye swapchain */
typedef struct {
    XrSwapchain swapchain;
    uint32_t image_count;
    XrSwapchainImageVulkanKHR *images; /* .image is a VkImage */
    int32_t width, height;
} EyeSwapchain;
static EyeSwapchain eye[2]; /* 0 = left, 1 = right */

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

/* Resolve an extension function pointer from the loader. */
#define XR_PFN(name) \
    PFN_##name name = NULL; \
    xrGetInstanceProcAddr(xr_instance, #name, (PFN_xrVoidFunction *)&name)

/* ------------------------------- OpenXR ---------------------------------- */

static bool create_xr_instance(void)
{
    const char *exts[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo ci = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {
            .applicationName = "x4vr-viewer",
            .applicationVersion = 1,
            .engineName = "none",
            .apiVersion = XR_API_VERSION_1_0,
        },
        .enabledExtensionCount = 1,
        .enabledExtensionNames = exts,
    };
    XR_CHECK(xrCreateInstance(&ci, &xr_instance),
             "xrCreateInstance (is WiVRn/Monado/SteamVR running? "
             "set XR_RUNTIME_JSON; needs XR_KHR_vulkan_enable2)");

    XrSystemGetInfo sgi = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XR_CHECK(xrGetSystem(xr_instance, &sgi, &xr_system), "xrGetSystem");

    XrSystemProperties sp = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(xr_instance, xr_system, &sp)))
        fprintf(stderr, "HMD: %s\n", sp.systemName);

    /* Pick an environment blend mode the runtime supports (OPAQUE for VR). */
    uint32_t bm_count = 0;
    xrEnumerateEnvironmentBlendModes(xr_instance, xr_system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bm_count, NULL);
    if (bm_count) {
        XrEnvironmentBlendMode *modes = calloc(bm_count, sizeof(*modes));
        xrEnumerateEnvironmentBlendModes(xr_instance, xr_system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bm_count, &bm_count, modes);
        xr_blend = modes[0]; /* first supported; OPAQUE on a VR HMD */
        free(modes);
    }
    return true;
}

/* Create the Vulkan instance + device through OpenXR so they satisfy the
 * runtime's requirements (XR_KHR_vulkan_enable2 path). */
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
    if (vk_err != VK_SUCCESS) {
        fprintf(stderr, "FATAL: VkInstance creation failed: %d\n", vk_err);
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR gdi = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = xr_system,
        .vulkanInstance = vk_instance,
    };
    XR_CHECK(xrGetVulkanGraphicsDevice2KHR(xr_instance, &gdi, &vk_phys),
             "xrGetVulkanGraphicsDevice2KHR");

    /* Find a graphics-capable queue family. */
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qf_count, NULL);
    VkQueueFamilyProperties *qfs = calloc(qf_count, sizeof(*qfs));
    vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &qf_count, qfs);
    bool found = false;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vk_queue_family = i;
            found = true;
            break;
        }
    }
    free(qfs);
    if (!found) {
        fprintf(stderr, "FATAL: no graphics queue family\n");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
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
    if (vk_err != VK_SUCCESS) {
        fprintf(stderr, "FATAL: VkDevice creation failed: %d\n", vk_err);
        return false;
    }

    vkGetDeviceQueue(vk_device, vk_queue_family, 0, &vk_queue);

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk_queue_family,
    };
    VK_CHECK(vkCreateCommandPool(vk_device, &pci, NULL, &vk_cmd_pool),
             "vkCreateCommandPool");
    return true;
}

static bool create_session_and_space(void)
{
    XrGraphicsBindingVulkan2KHR gb = {
        .type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = vk_instance,
        .physicalDevice = vk_phys,
        .device = vk_device,
        .queueFamilyIndex = vk_queue_family,
        .queueIndex = 0,
    };
    XrSessionCreateInfo sci = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &gb,
        .systemId = xr_system,
    };
    XR_CHECK(xrCreateSession(xr_instance, &sci, &xr_session), "xrCreateSession");

    /* VIEW space => the quads are head-locked (XR-glasses-like screen). */
    XrReferenceSpaceCreateInfo rsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
        .poseInReferenceSpace = { .orientation = { .w = 1.0f } },
    };
    XR_CHECK(xrCreateReferenceSpace(xr_session, &rsci, &xr_view_space),
             "xrCreateReferenceSpace(VIEW)");
    return true;
}

/* Pick a color swapchain format the runtime offers (prefer 8-bit BGRA/RGBA). */
static int64_t choose_format(void)
{
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(xr_session, 0, &n, NULL);
    int64_t *fmts = calloc(n, sizeof(int64_t));
    xrEnumerateSwapchainFormats(xr_session, n, &n, fmts);
    int64_t want[] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                       VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
    int64_t chosen = fmts[0];
    for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++)
        for (uint32_t i = 0; i < n; i++)
            if (fmts[i] == want[w]) { chosen = fmts[i]; goto done; }
done:
    free(fmts);
    return chosen;
}

static bool create_eye_swapchain(EyeSwapchain *e, int64_t format)
{
    XrSwapchainCreateInfo ci = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = format,
        .sampleCount = 1,
        .width = EYE_W,
        .height = EYE_H,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    e->width = EYE_W;
    e->height = EYE_H;
    XR_CHECK(xrCreateSwapchain(xr_session, &ci, &e->swapchain),
             "xrCreateSwapchain");

    XR_CHECK(xrEnumerateSwapchainImages(e->swapchain, 0, &e->image_count, NULL),
             "xrEnumerateSwapchainImages(count)");
    e->images = calloc(e->image_count, sizeof(XrSwapchainImageVulkanKHR));
    for (uint32_t i = 0; i < e->image_count; i++)
        e->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    XR_CHECK(xrEnumerateSwapchainImages(
                 e->swapchain, e->image_count, &e->image_count,
                 (XrSwapchainImageBaseHeader *)e->images),
             "xrEnumerateSwapchainImages");
    return true;
}

/* Clear one acquired swapchain image to a flat color (the test pattern). */
static bool fill_image(VkImage image, VkClearColorValue color)
{
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk_device, &ai, &cmd),
             "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &bi);

    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1, .layerCount = 1,
    };

    /* UNDEFINED -> TRANSFER_DST for the clear. */
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .image = image,
        .subresourceRange = range,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_dst);

    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color, 1, &range);

    /* TRANSFER_DST -> COLOR_ATTACHMENT for the compositor. */
    VkImageMemoryBarrier to_color = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .image = image,
        .subresourceRange = range,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_color);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(vk_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    vkQueueWaitIdle(vk_queue); /* simple sync for milestone 1 */
    vkFreeCommandBuffers(vk_device, vk_cmd_pool, 1, &cmd);
    return true;
}

/* Acquire/clear/release one eye and fill out its quad layer descriptor. */
static bool render_eye(int idx, XrCompositionLayerQuad *quad)
{
    EyeSwapchain *e = &eye[idx];

    uint32_t img_index = 0;
    XrSwapchainImageAcquireInfo ai = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XR_CHECK(xrAcquireSwapchainImage(e->swapchain, &ai, &img_index),
             "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wi = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION };
    XR_CHECK(xrWaitSwapchainImage(e->swapchain, &wi), "xrWaitSwapchainImage");

    /* Distinct per-eye color so L/R mapping is unambiguous in-headset. */
    VkClearColorValue color = idx == 0
        ? (VkClearColorValue){ { 0.20f, 0.02f, 0.02f, 1.0f } }  /* L: red  */
        : (VkClearColorValue){ { 0.02f, 0.02f, 0.20f, 1.0f } }; /* R: blue */
    if (!fill_image(e->images[img_index].image, color))
        return false;

    XrSwapchainImageReleaseInfo ri = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    XR_CHECK(xrReleaseSwapchainImage(e->swapchain, &ri),
             "xrReleaseSwapchainImage");

    *quad = (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = 0,
        .space = xr_view_space,
        .eyeVisibility = idx == 0 ? XR_EYE_VISIBILITY_LEFT
                                  : XR_EYE_VISIBILITY_RIGHT,
        .subImage = {
            .swapchain = e->swapchain,
            .imageRect = { { 0, 0 }, { e->width, e->height } },
            .imageArrayIndex = 0,
        },
        .pose = {
            .orientation = { 0, 0, 0, 1 },
            .position = { 0, 0, -QUAD_DISTANCE },
        },
        .size = { QUAD_SIZE_M, QUAD_SIZE_M },
    };
    return true;
}

static bool render_frame(void)
{
    XrFrameWaitInfo fwi = { .type = XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
    XR_CHECK(xrWaitFrame(xr_session, &fwi, &fs), "xrWaitFrame");

    XrFrameBeginInfo fbi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    XR_CHECK(xrBeginFrame(xr_session, &fbi), "xrBeginFrame");

    XrCompositionLayerQuad quads[2];
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layer_count = 0;

    if (fs.shouldRender) {
        for (int i = 0; i < 2; i++) {
            if (!render_eye(i, &quads[i]))
                return false;
            layers[i] = (const XrCompositionLayerBaseHeader *)&quads[i];
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
    bool session_running = false;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;

    while (!g_quit) {
        /* Drain events. */
        XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(xr_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrEventDataSessionStateChanged *sc = (void *)&ev;
                state = sc->state;
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {
                        .type = XR_TYPE_SESSION_BEGIN_INFO,
                        .primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
                    XR_CHECK(xrBeginSession(xr_session, &bi), "xrBeginSession");
                    session_running = true;
                    fprintf(stderr, "session running\n");
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(xrEndSession(xr_session), "xrEndSession");
                    session_running = false;
                } else if (state == XR_SESSION_STATE_EXITING ||
                           state == XR_SESSION_STATE_LOSS_PENDING) {
                    g_quit = 1;
                }
            }
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        }

        if (session_running) {
            if (!render_frame())
                return false;
        } else {
            usleep(20 * 1000);
        }
    }
    return true;
}

static void cleanup(void)
{
    for (int i = 0; i < 2; i++) {
        if (eye[i].swapchain) xrDestroySwapchain(eye[i].swapchain);
        free(eye[i].images);
    }
    if (xr_view_space) xrDestroySpace(xr_view_space);
    if (xr_session) xrDestroySession(xr_session);
    if (vk_cmd_pool) vkDestroyCommandPool(vk_device, vk_cmd_pool, NULL);
    if (vk_device) vkDestroyDevice(vk_device, NULL);
    if (vk_instance) vkDestroyInstance(vk_instance, NULL);
    if (xr_instance) xrDestroyInstance(xr_instance);
}

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int rc = 1;
    if (!create_xr_instance()) goto out;
    if (!create_vulkan_via_xr()) goto out;
    if (!create_session_and_space()) goto out;

    int64_t format = choose_format();
    fprintf(stderr, "swapchain format: %ld\n", (long)format);
    if (!create_eye_swapchain(&eye[0], format)) goto out;
    if (!create_eye_swapchain(&eye[1], format)) goto out;

    fprintf(stderr, "init OK — left eye should be red, right eye blue. "
                    "Ctrl-C to quit.\n");
    if (!run_loop()) goto out;
    rc = 0;

out:
    cleanup();
    return rc;
}
