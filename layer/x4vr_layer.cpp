// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// VK_LAYER_X4VR_core — Phase 0: passthrough Vulkan layer + live detection of
// X4's view-constants arena.
//
// The layer chains every call through to the next layer/driver untouched.
// Its only current feature is observation (from the frame analysis in
// docs/frame-analysis.md):
//   * X4 keeps per-view constants in UNIFORM_BUFFER descriptor slots of
//     range 1792 (128-block arena; the 11 camera mat4s head each block).
//   * We record every such slot at vkUpdateDescriptorSets time, credit the
//     slots bound when draws are recorded, and at present time log which
//     (buffer, offset) won the frame — that is the main camera block.
// This validates the frame map against the live game and answers the
// intra-run offset-stability question before Phase 3 starts writing to it.

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <unistd.h>
#include <vector>

#define X4VR_LOG_TAG "layer"
#include "../common/x4vr_log.hpp"
#include "../common/x4vr_sbs.hpp"
#include "../common/x4vr_spirv.hpp"
#include "../common/x4vr_view.hpp"
#include "x4vr_sbs.hpp"

namespace {

constexpr VkDeviceSize kViewBlockRange = 1792; // from frame analysis

// ---------------------------------------------------------------- dispatch
// Loader dispatch key: the first pointer-sized word of every dispatchable
// handle.
inline void *dispatch_key(const void *handle) {
    return *(void *const *)handle;
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    // resolved instance-level next-chain entry points
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    // The API version X4 itself asked for. Decides whether the 1.1-promoted
    // entry points exist under their core names or only as KHR aliases.
    uint32_t app_api_version = 0;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdDraw CmdDraw = nullptr;
    PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkCmdResolveImage CmdResolveImage = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage2 CmdCopyImage2 = nullptr;
    PFN_vkCmdBlitImage2 CmdBlitImage2 = nullptr;
    PFN_vkCmdResolveImage2 CmdResolveImage2 = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyBufferToImage2 CmdCopyBufferToImage2 = nullptr;
    PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    VkPhysicalDeviceMemoryProperties memprops{};
    // Kept for the same reason as memprops: the physical device is in scope
    // only during creation, and the swapchain path needs to ask it which
    // present modes the surface supports.
    VkPhysicalDevice phys = VK_NULL_HANDLE;
};

std::mutex g_mu;
std::unordered_map<void *, InstanceData> g_instances;
std::unordered_map<void *, DeviceData> g_devices;

// Phase 4a scaffolding, opt-in while both halves are still the same eye.
// Flip the default once the halves are rendered per eye.
x4vr::SbsCompositor g_sbs;
const bool g_sbs_enabled = [] {
    const char *e = getenv("X4VR_SBS");
    return e && *e && *e != '0';
}();

// Whether X4 is made to render one eye's worth (half width) rather than the
// full frame. On by default with X4VR_SBS=1; X4VR_SBS_SPLIT=0 falls back to
// duplicating the left half of a full-width frame, which is the older and
// less invasive behaviour.
const bool g_sbs_split_render = [] {
    const char *e = getenv("X4VR_SBS_SPLIT");
    return !(e && *e && *e == '0');
}();

// Present mode override, for measurement only. -1 leaves X4's choice alone.
// 0 = IMMEDIATE, 1 = MAILBOX, 2 = FIFO (X4's own, and the one to play in).
//
// Exists because the first attempt to cost stage 1 produced "59.4 fps" on
// both sides of the comparison: X4 asks for FIFO, so the number measured the
// monitor. An uncapped mode is the difference between a perf claim and a
// restatement of the refresh rate.
const int g_present_mode = [] {
    const char *e = getenv("X4VR_PRESENT_MODE");
    return e && *e ? (int)strtol(e, nullptr, 0) : -1;
}();

// Multiview: how the second eye is meant to arrive. Measured on X4 (app api
// 1.2): the device supports it, X4 does not enable it, so the layer must.
// Enabling the feature alone changes nothing observable -- it only matters
// once a render pass carries a view mask -- but it is gated anyway so the
// device can be created exactly as X4 asked for it during A/B.
bool g_multiview_supported = false; // filled in by probe_multiview()
const bool g_multiview_enable = [] {
    const char *e = getenv("X4VR_MULTIVIEW");
    return !(e && *e && *e == '0');
}();

// Phase 4b stage 1: render the frame into two array layers, with the SAME
// eye matrix for both. Nothing on screen may change -- that is the whole
// point of the stage. See docs/phase4b-test-plan.md.
//
// Opt-in, so `mv_partition_measured` behaviour remains the default while this
// is under test.
const bool g_mv = [] {
    const char *e = getenv("X4VR_MV");
    return e && *e && *e != '0';
}();

// Which views a masked pass renders. 0x3 (both) is the real configuration.
//
// 0x2 is the decisive diagnostic, and it needs no redirect at all: view 0 of
// the pass maps to array layer 1, so the frame is rendered *only* into layer 1
// and layer 0 is left untouched. The game then reads layer 0 through its own
// unmodified views. A black scene therefore means the view mask really is
// steering draws into layer 1 -- proving the write path and indicting the
// read path -- and a normal-looking scene means the mask steers nothing at
// all, whatever the masked/substituted/bind counters say.
//
// It exists because every previous test of "is layer 1 shaded?" went through
// the gate-2 descriptor redirect, which is our own code and has already been
// wrong twice. This one uses X4's untouched read path as the detector.
const uint32_t kViewMask = [] {
    const char *e = getenv("X4VR_MV_MASK");
    const uint32_t m = e && *e ? (uint32_t)strtoul(e, nullptr, 0) : 0x3u;
    return m ? m : 0x3u;
}();

// Gate 2: which array layer the frame reads from.
//
// Unset  -> no redirect at all, the ordinary path.
// =1     -> read layer 1. Black means the second view was never shaded.
// =0     -> read layer 0, but *through the same substitution path*. This is
//           the control, and it is what tells a genuine multiview failure
//           apart from a broken instrument: layer 0 is known-good content, so
//           if =0 renders correctly the machinery is sound and a black =1 is
//           a real finding, whereas if =0 is also black the redirect itself
//           is at fault and =1 proved nothing. Run 2 taught this the
//           expensive way.
const bool g_mv_redirect = getenv("X4VR_MV_PRESENT_LAYER") != nullptr;
const uint32_t g_mv_present_layer = [] {
    const char *e = getenv("X4VR_MV_PRESENT_LAYER");
    return e ? (uint32_t)atoi(e) : 0u;
}();

struct MvStats {
    uint32_t doubled = 0;     // images given a second array layer
    uint32_t masked = 0;      // render passes given a view mask
    uint32_t substituted = 0; // attachment views replaced with array views
    uint32_t fallbacks = 0;   // attachments we could NOT upgrade -- must be 0
    uint32_t redirected = 0;  // gate-2 descriptor reads moved to layer 1
    uint32_t pipe_masked = 0;   // pipelines built against a view-masked pass
    uint32_t pipe_unmasked = 0; // ... against a pass with no view mask
    uint32_t pipe_dynamic = 0;  // ... against no render pass (dynamic rendering)
    uint32_t widened = 0;       // transfer regions grown to cover both layers
    // Stage-1 diagnosis, take four. Two candidates for "layer 1 is never
    // shaded", measured rather than argued:
    //
    //   bind_mismatch — a pipeline compiled against an *unmasked* render pass,
    //     bound inside a masked one. Legal (the passes are compatible; a view
    //     mask is not part of what vkCmdBindPipeline checks) and fatal: the
    //     driver decided at compile time how many views the draw replicates
    //     to, and it decided one. pipe_masked/pipe_unmasked cannot see this —
    //     they count where a pipeline was *built*, not where it is *used*.
    //
    //   barrier_narrow — an image barrier naming layerCount=1 on a doubled
    //     image. The render pass transitions both layers (its attachment is
    //     our two-layer view); an explicit barrier between passes transitions
    //     layer 0 alone, so layer 1 is sampled in the wrong layout and reads
    //     undefined. Same family as the transfers: multiview replicates
    //     draws, and nothing else has any idea there are two layers.
    uint32_t bind_ok = 0;
    uint32_t bind_mismatch = 0;
    uint32_t barrier_narrow = 0;
    uint32_t barrier_wide = 0;
    // Take five settled that draws do reach layer 1, which turns the black
    // frame into a contradiction: with one eye matrix the two layers hold the
    // same picture, so reading layer 1 instead of layer 0 should change
    // nothing, and it changes everything. The resolution is that not every
    // pixel arrives by a draw. Anything that writes an image outside a render
    // pass writes layer 0 alone unless we widen it, and one such write into a
    // per-eye image leaves layer 1 holding older content -- invisible until
    // something reads layer 1.
    uint32_t layer0_only = 0; // per-eye images written by an unwidened command
    // Gate-2 cache entries that survived their view. Non-zero means the
    // redirect had been answering with a view onto a different image, which
    // makes every black frame it reported unreliable rather than informative.
    uint32_t redirect_stale = 0;
    // Input-attachment descriptors repointed at the two-layer array view
    // that the framebuffer actually uses. Non-zero is required for a
    // view-masked deferred pass to light view 1 at all.
    uint32_t input_fixed = 0;
    uint32_t reported = 0;
} g_mv_stats;
std::mutex g_mv_mu;

// The layer is switched on through the environment, and every child process
// inherits it: under gamescope we are loaded into gamescope and its Xwayland
// as well as into X4. Compositing *their* swapchains would apply the effect a
// second time on the way to the display, and patching their shaders is
// pointless work. Act only in the game's own process.
bool g_active = false;

// VkQueue -> queue family, learned from vkGetDeviceQueue. The composite
// records into a command pool, and a command buffer may only be submitted to
// a queue of its pool's family -- X4 requests more than one family, so the
// present queue's family has to be observed, not assumed.
std::mutex g_queue_mu;
std::unordered_map<VkQueue, uint32_t> g_queue_family;

// Surfaces whose capabilities we reported at half width. Doubling the extent
// back at swapchain creation is only correct for exactly these -- a surface
// with an undefined currentExtent was never halved, and doubling it would
// silently make the swapchain twice the size the app asked for.
std::mutex g_surface_mu;
std::unordered_map<VkSurfaceKHR, bool> g_halved_surfaces;

bool note_halved_surface(VkSurfaceKHR s) { // true the first time only
    std::lock_guard<std::mutex> lock(g_surface_mu);
    return g_halved_surfaces.emplace(s, true).second;
}

void forget_halved_surface(VkSurfaceKHR s) {
    std::lock_guard<std::mutex> lock(g_surface_mu);
    g_halved_surfaces.erase(s);
}

bool surface_was_halved(VkSurfaceKHR s) {
    std::lock_guard<std::mutex> lock(g_surface_mu);
    return g_halved_surfaces.count(s) > 0;
}

bool queue_family_of(VkQueue q, uint32_t &out) {
    std::lock_guard<std::mutex> lock(g_queue_mu);
    auto it = g_queue_family.find(q);
    if (it == g_queue_family.end())
        return false;
    out = it->second;
    return true;
}

bool app_is_target(const char *app_name) {
    if (const char *e = getenv("X4VR_APP")) // override if X4 ever renames
        return app_name && !strcmp(app_name, e);
    if (app_name && !strcmp(app_name, "X4"))
        return true;
    // VkApplicationInfo is optional, so fall back to the executable name.
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return false;
    exe[n] = 0;
    const char *slash = strrchr(exe, '/');
    return !strcmp(slash ? slash + 1 : exe, "X4");
}

// ---------------------------------------------------------------- tracking
struct ViewSlot {
    VkBuffer buffer;
    VkDeviceSize offset;
    bool operator<(const ViewSlot &o) const {
        return buffer != o.buffer ? buffer < o.buffer : offset < o.offset;
    }
};

struct Tracking {
    std::mutex mu;
    // buffers created with UNIFORM_BUFFER usage -> size
    std::unordered_map<VkBuffer, VkDeviceSize> ubo_buffers;
    // descriptor set -> its range-1792 UBO slots
    std::unordered_map<VkDescriptorSet, std::vector<ViewSlot>> sets;
    // command buffer -> currently bound view slots (any set index)
    std::unordered_map<VkCommandBuffer, std::vector<ViewSlot>> bound;
    // per-frame draw credit
    std::map<ViewSlot, uint32_t> credit;
    uint64_t frame = 0;
    // X4 double-buffers the view arena (two buffers alternate per frame),
    // so "did the winner change" must ignore expected alternation: keep the
    // set of recently seen winners and only log genuinely new ones.
    std::vector<ViewSlot> known_winners;

    // --- host-memory mapping, so we can read/write the view block ---
    // X4 keeps its constant arenas persistently mapped and coherent.
    struct Mapping {
        void *ptr = nullptr;      // host pointer of the mapped range
        VkDeviceSize offset = 0;  // where in the memory object it starts
        VkDeviceSize size = 0;
    };
    std::unordered_map<VkDeviceMemory, Mapping> mapped;
    struct BufferBinding {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };
    std::unordered_map<VkBuffer, BufferBinding> buf_binding;

    x4vr::Major major = x4vr::Major::Unknown;
    bool logged_matrices = false;
} g_track;

// ------------------------------------------------------- shadow exclusion
// The per-eye shear K is derived from the *camera* projection, where clip z
// is the constant near plane. Shadow passes transform through M_shadowCSM*
// (light space) instead, so K is meaningless there and would smear the
// shadow map. X4 reuses the same vertex modules for both, so the choice
// cannot be made at module creation.
//
// It can be made at *pipeline* creation: shadow and main passes share zero
// pipelines (docs/frame-analysis.md), and shadow passes are the only
// depth-only ones (2048x2048 D16, no colour attachments). So we keep both
// module variants and pick the untouched one for depth-only pipelines.
// This is static — no per-frame cost.
// The same mechanism also solves the UI problem. X4's UI vertex shaders do
// not merely declare the set-3 block, they *read* member 0 of it, so no
// static SPIR-V test can tell them apart from world geometry. But the UI is
// drawn in its own pass: pass 46 is B8G8R8A8_SRGB and pass 47 is the
// B8G8R8A8_UNORM blit, whereas every world pass writes multi-attachment
// float targets. So "all colour attachments are 8-bit UNORM/SRGB" marks a
// screen-space pass.
//
// Keeping the UI unsheared is a *correctness* requirement, not cosmetics:
// X4 hit-tests the UI on the CPU in unshifted screen space, so a
// GPU-side-only shift moves what the player sees away from what the player
// can click (observed live: map items had to be clicked well to the right of
// where they appeared). See docs/frame-analysis.md.
struct ShaderVariants {
    std::mutex mu;
    // patched module handle -> its unpatched twin
    std::unordered_map<VkShaderModule, VkShaderModule> original;
    // render pass -> per-subpass "this subpass must not be sheared"
    std::unordered_map<VkRenderPass, std::vector<bool>> unsheared;
    uint32_t swapped = 0;
} g_variants;

// render pass -> inventory serial, so the framebuffer log can name the pass
// it belongs to. Inventory only; shares g_variants.mu.
std::unordered_map<VkRenderPass, uint32_t> g_rp_serials;

// Passes we gave a view mask; the framebuffer hook needs to know so it can
// supply array views. Shares g_variants.mu with g_rp_serials.
std::unordered_set<VkRenderPass> g_masked_passes;

// Image tracking, for the Phase 4b question the pass inventory could not
// answer: how many *images* are behind those passes? The cost of doubling is
// per image, but a render pass names only its attachments and ten passes
// sharing one colour target look like ten targets from there.
//
// vkCreateImage cannot classify anything on its own -- at creation an image
// has no render pass and no framebuffer, so "is this a per-eye attachment"
// is not yet a question the create info can answer. The hooks therefore only
// record identity, and the join image <- view <- framebuffer -> pass happens
// where the framebuffer names them all at once.
struct ImageInfo {
    uint32_t serial;
    VkExtent3D extent;
    VkFormat format;
    uint32_t layers, mips, samples;
    VkImageUsageFlags usage;
    bool doubled = false;
};
// Enough of a view's create info to rebuild it as an array view later.
struct ViewInfo {
    VkImage image;
    VkFormat format;
    VkComponentMapping components;
    VkImageSubresourceRange range;
    VkImageViewType type;
};
std::mutex g_img_mu;
std::unordered_map<VkImage, ImageInfo> g_images;
std::unordered_map<VkImageView, ViewInfo> g_views;
// original attachment view -> the 2-layer array view we substitute for it
std::unordered_map<VkImageView, VkImageView> g_array_views;
// images actually written by a view-masked pass, learned at framebuffer time
std::unordered_set<VkImage> g_per_eye_images;
// sampled view -> the same view onto layer 1, made lazily for gate 2.
//
// The image is carried alongside on purpose. Vulkan recycles handles, and this
// cache is keyed on one: once X4 destroys a view, the driver is free to hand
// the identical handle back for a completely different image, and a cache that
// only remembers the replacement would answer with a view onto the *previous*
// image's layer 1. Keeping the image makes that detectable instead of silent.
struct Layer1View {
    VkImageView view;
    VkImage image;
};
std::unordered_map<VkImageView, Layer1View> g_layer1_views;
uint32_t g_img_serial = 0;

// A draw replicates to two views only if the *pass* carries the mask and the
// *pipeline bound into it* was compiled knowing that. Those are two different
// objects and Vulkan does not require them to agree, so the pair has to be
// watched at the one moment both are known: vkCmdBindPipeline.
std::mutex g_cb_mu;
std::unordered_map<VkPipeline, bool> g_pipe_mv;      // built for multiview
std::unordered_map<VkCommandBuffer, bool> g_cb_mask; // inside a masked pass
// Which framebuffer a command buffer is currently rendering into, so that
// vkCmdEndRenderPass can name the images the pass just finished writing.
std::unordered_map<VkCommandBuffer, VkFramebuffer> g_cb_fb;

// A doubled colour attachment of a masked pass, with the layout that pass
// leaves it in. Collected at framebuffer creation, where the pass, the views
// and the images are all named together for the only time.
struct FbAtt {
    VkImage image;
    uint32_t serial;
    VkFormat format;
    VkExtent3D extent;
    VkImageLayout final_layout;
};
std::unordered_map<VkFramebuffer, std::vector<FbAtt>> g_fb_atts;

// Which kinds of pass render into each doubled image.
//
// An image attached to a *masked* pass gets both layers written; one attached
// to an unmasked pass gets layer 0 only, because an unmasked pass keeps X4's
// own single-layer view. An image with both kinds of writer therefore ends the
// frame with a layer 1 that is missing every unmasked contribution -- which
// would show up as a partial, content-shaped divergence rather than a blank or
// unrelated layer.
//
// Computed inside one run on purpose: image serials restart per run, so
// cross-referencing an old inventory log against today's probe output is not
// sound, and an earlier attempt to do exactly that gave an answer for the
// wrong run.
struct PassKinds {
    std::vector<uint32_t> masked, unmasked;
};
std::unordered_map<uint32_t, PassKinds> g_img_writers; // by image serial

// 8-bit UNORM/SRGB colour targets mean a screen-space (UI/blit) pass.
inline bool is_ldr_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return true;
    default:
        return false;
    }
}

// Must this (render pass, subpass) use the unpatched modules?
bool needs_original(VkRenderPass rp, uint32_t subpass) {
    std::lock_guard<std::mutex> lock(g_variants.mu);
    auto it = g_variants.unsheared.find(rp);
    if (it == g_variants.unsheared.end() || subpass >= it->second.size())
        return false;
    return it->second[subpass];
}

// Host pointer for a (buffer, offset) view slot, or nullptr if the backing
// memory is not currently mapped. Caller holds g_track.mu.
float *slot_host_ptr(const ViewSlot &s) {
    auto b = g_track.buf_binding.find(s.buffer);
    if (b == g_track.buf_binding.end())
        return nullptr;
    auto m = g_track.mapped.find(b->second.memory);
    if (m == g_track.mapped.end() || !m->second.ptr)
        return nullptr;
    // absolute offset within the memory object
    const VkDeviceSize abs = b->second.offset + s.offset;
    if (abs < m->second.offset)
        return nullptr;
    const VkDeviceSize rel = abs - m->second.offset;
    if (m->second.size != VK_WHOLE_SIZE &&
        rel + x4vr::kViewBlockBytes > m->second.size)
        return nullptr;
    return (float *)((uint8_t *)m->second.ptr + rel);
}

void credit_draw(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> lock(g_track.mu);
    auto it = g_track.bound.find(cb);
    if (it == g_track.bound.end())
        return;
    for (const ViewSlot &s : it->second)
        g_track.credit[s]++;
}

// Set by the submit-time patch when X4VR_TEST_VERIFY=1: the block we wrote
// zeros into, checked again at present time. If it is no longer zero, X4
// rewrote the constants after our patch (a timing problem we can fix). If
// it is still zero yet the image is fine, the GPU is reading a different
// copy of the data (an indirection we must find).
float *g_verify_ptr = nullptr;

// Frame pacing, reported as percentiles rather than an average.
//
// Gate 3 of docs/phase4b-test-plan.md needs a number that survives being
// compared across runs, and a mean does not: a single load hitch or a menu
// stretch moves it more than the change under test does. Percentiles over a
// fixed window are stable enough to compare, and p99 keeps the hitches
// visible instead of averaging them away.
//
// Built in rather than left to MangoHud so the measurement does not add
// another layer to the chain during a test whose premise is that nothing
// should change.
void frame_timing(uint64_t frame) {
    static std::vector<float> ms;
    static timespec prev{};
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (prev.tv_sec) {
        ms.push_back((float)(now.tv_sec - prev.tv_sec) * 1000.0f +
                     (float)(now.tv_nsec - prev.tv_nsec) / 1.0e6f);
    }
    prev = now;
    if (ms.size() < 600)
        return;
    std::vector<float> s = ms;
    std::sort(s.begin(), s.end());
    auto pct = [&s](double p) {
        return s[(size_t)(p * (double)(s.size() - 1))];
    };
    double sum = 0;
    for (float v : s)
        sum += v;
    const float mean = (float)(sum / (double)s.size());
    // fps from the median frame, which is the one a player perceives as
    // "the" framerate; the tail is reported in milliseconds where it is
    // easier to reason about.
    X4VR_LOG("perf frame %llu: median %.2f ms (%.1f fps)  mean %.2f  "
             "p90 %.2f  p99 %.2f  worst %.2f  [%zu frames]",
             (unsigned long long)frame, pct(0.5), 1000.0f / pct(0.5), mean,
             pct(0.9), pct(0.99), s.back(), s.size());
    ms.clear();
}

void frame_flush() {
    std::lock_guard<std::mutex> lock(g_track.mu);
    g_track.frame++;
    // Only the game's. gamescope presents too, into the same log file, and
    // its series is pinned to the display refresh while X4's is not -- two
    // interleaved sequences of "perf frame N" that cannot be told apart
    // afterwards, because both restart at 1.
    if (g_active)
        frame_timing(g_track.frame);
    if (g_verify_ptr && (g_track.frame % 120) == 0) {
        const x4vr::Mat4 vp = x4vr::load(g_verify_ptr + x4vr::kViewProjection);
        bool still_zero = true;
        for (float f : vp.m)
            if (f != 0.0f)
                still_zero = false;
        X4VR_LOG("VERIFY frame %llu: block we zeroed at submit is %s at "
                 "present (m0=%.4f m5=%.4f)",
                 (unsigned long long)g_track.frame,
                 still_zero ? "STILL ZERO -> GPU reads elsewhere"
                            : "REWRITTEN by X4 -> timing problem",
                 vp.m[0], vp.m[5]);
    }
    if (g_track.credit.empty())
        return;
    // winner = the most-drawn view slot this frame (the main camera)
    const ViewSlot *best = nullptr;
    uint32_t best_n = 0;
    for (auto &[slot, n] : g_track.credit) {
        if (n > best_n) {
            best = &slot;
            best_n = n;
        }
    }
    static const bool log_every = [] {
        const char *e = getenv("X4VR_LOG_EVERY_FRAME");
        return e && *e && *e != '0';
    }();
    bool is_new = false;
    if (best) {
        is_new = true;
        for (const ViewSlot &k : g_track.known_winners)
            if (k.buffer == best->buffer && k.offset == best->offset)
                is_new = false;
        if (is_new) {
            g_track.known_winners.push_back(*best);
            if (g_track.known_winners.size() > 8) // window: 2 arenas x views
                g_track.known_winners.erase(g_track.known_winners.begin());
        }
    }
    bool heartbeat = (g_track.frame % 600) == 0;
    if (best && (is_new || heartbeat || log_every)) {
        VkDeviceSize bufsize = 0;
        auto b = g_track.ubo_buffers.find(best->buffer);
        if (b != g_track.ubo_buffers.end())
            bufsize = b->second;
        X4VR_LOG("frame %llu%s: main view slot buffer=%p offset=%llu "
                 "(block #%llu) draws=%u slots=%zu arena_size=%llu",
                 (unsigned long long)g_track.frame,
                 is_new ? " NEW" : (heartbeat ? " hb" : ""),
                 (void *)best->buffer, (unsigned long long)best->offset,
                 (unsigned long long)(best->offset / kViewBlockRange), best_n,
                 g_track.credit.size(), (unsigned long long)bufsize);
    }
    g_track.credit.clear();
}

// ------------------------------------------------------------------ hooks

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateBuffer(
    VkDevice device, const VkBufferCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkBuffer *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    VkResult r = d->CreateBuffer(device, ci, ac, out);
    if (r == VK_SUCCESS && (ci->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        std::lock_guard<std::mutex> lock(g_track.mu);
        g_track.ubo_buffers[*out] = ci->size;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    {
        std::lock_guard<std::mutex> lock(g_track.mu);
        g_track.ubo_buffers.erase(buffer);
    }
    d->DestroyBuffer(device, buffer, ac);
}

// Gate 2 of docs/phase4b-test-plan.md, applied where it is actually correct.
//
// The point is to make the frame read the *second* view, so that a view that
// was never shaded shows up as black. Doing that at image-view creation was
// wrong: 92 images are doubled but only ~21 are ever written by a view-masked
// pass, and redirecting the other 71 pointed their reads at a layer nothing
// had rendered into. That is what produced a mostly-black frame with an
// intact HUD -- a broken instrument, not a broken frame.
//
// A descriptor update is the right moment. By then the framebuffers exist, so
// g_per_eye_images knows which images really carry two views, and only those
// are redirected. If a descriptor is written before its framebuffer is built
// the image is simply not in the set yet and the read stays on layer 0 --
// correct output, one less sample for the test, which is the safe direction
// to fail in.
void mv_redirect_writes(DeviceData *d, VkDevice device, uint32_t writeCount,
                        const VkWriteDescriptorSet *writes,
                        std::vector<VkWriteDescriptorSet> &out,
                        std::vector<std::vector<VkDescriptorImageInfo>> &pool) {
    out.assign(writes, writes + writeCount);
    pool.resize(writeCount);
    for (uint32_t i = 0; i < writeCount; i++) {
        const VkWriteDescriptorSet &w = writes[i];
        if (!w.pImageInfo || !w.descriptorCount)
            continue;
        // A subpass input must name the same subresource as the framebuffer
        // attachment it reads. We replaced those attachments with two-layer
        // array views and left X4's descriptors pointing at its own
        // single-layer views, so in a view-masked pass the two no longer
        // agree: view 1 is meant to read layer 1 of the attachment, and the
        // descriptor describes an image that only has layer 0.
        //
        // This is what left layer 1 rasterised but unlit. X4's deferred
        // lighting passes -- rp 30/31/32/64, six attachments, one colour and
        // one depth -- read the G-buffer through the other four as subpass
        // inputs, the S_subpassInput_AUTOMS its own validation errors name.
        // Geometry lands in view 1 because that is ordinary rasterisation;
        // the light contribution does not, because subpassLoad reads through
        // a descriptor that cannot see the layer being rendered.
        //
        // Substituting our array view is the fix, and it is not part of gate
        // 2: it has to happen whenever multiview is on, redirect or not.
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            std::vector<VkDescriptorImageInfo> infos(
                w.pImageInfo, w.pImageInfo + w.descriptorCount);
            bool fixed = false;
            {
                std::lock_guard<std::mutex> lock(g_img_mu);
                for (uint32_t j = 0; j < w.descriptorCount; j++) {
                    auto it = g_array_views.find(infos[j].imageView);
                    if (it == g_array_views.end())
                        continue;
                    infos[j].imageView = it->second;
                    fixed = true;
                }
            }
            if (fixed) {
                pool[i] = std::move(infos);
                out[i].pImageInfo = pool[i].data();
                std::lock_guard<std::mutex> lock(g_mv_mu);
                g_mv_stats.input_fixed += w.descriptorCount;
            }
            continue;
        }
        // Everything below is gate 2's instrument rather than a fix.
        if (!g_mv_redirect)
            continue;
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            w.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            continue;
        bool touched = false;
        std::vector<VkDescriptorImageInfo> infos(
            w.pImageInfo, w.pImageInfo + w.descriptorCount);
        for (uint32_t j = 0; j < w.descriptorCount; j++) {
            VkImageView v = infos[j].imageView;
            if (v == VK_NULL_HANDLE)
                continue;
            VkImageView repl = VK_NULL_HANDLE;
            ViewInfo vi{};
            bool make = false;
            {
                std::lock_guard<std::mutex> lock(g_img_mu);
                auto it = g_views.find(v);
                auto c = g_layer1_views.find(v);
                // A hit whose image no longer matches is a recycled handle:
                // the entry describes a view X4 has since destroyed. Drop it
                // and rebuild rather than redirecting the read to whatever
                // that image used to be.
                if (c != g_layer1_views.end() && it != g_views.end() &&
                    c->second.image != it->second.image) {
                    g_layer1_views.erase(c);
                    c = g_layer1_views.end();
                    std::lock_guard<std::mutex> lock2(g_mv_mu);
                    g_mv_stats.redirect_stale++;
                }
                if (c != g_layer1_views.end()) {
                    repl = c->second.view;
                } else {
                    if (it != g_views.end() &&
                        g_per_eye_images.count(it->second.image) &&
                        it->second.range.baseArrayLayer == 0) {
                        vi = it->second;
                        make = true;
                    }
                }
            }
            if (make) {
                VkImageViewCreateInfo ci{};
                ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                ci.image = vi.image;
                ci.viewType = vi.type;
                ci.format = vi.format;
                ci.components = vi.components;
                ci.subresourceRange = vi.range;
                ci.subresourceRange.baseArrayLayer = g_mv_present_layer;
                ci.subresourceRange.layerCount = 1;
                if (d->CreateImageView(device, &ci, nullptr, &repl) !=
                    VK_SUCCESS)
                    repl = VK_NULL_HANDLE;
                std::lock_guard<std::mutex> lock(g_img_mu);
                // Misses cached too, to stop retrying -- keyed with the image
                // so a recycled handle invalidates the entry above.
                g_layer1_views[v] = Layer1View{repl, vi.image};
            }
            if (repl != VK_NULL_HANDLE) {
                infos[j].imageView = repl;
                touched = true;
                std::lock_guard<std::mutex> lock(g_mv_mu);
                g_mv_stats.redirected++;
            }
        }
        if (touched) {
            pool[i] = std::move(infos);
            out[i].pImageInfo = pool[i].data();
        }
    }
}

VKAPI_ATTR void VKAPI_CALL x4vr_UpdateDescriptorSets(
    VkDevice device, uint32_t writeCount,
    const VkWriteDescriptorSet *writes, uint32_t copyCount,
    const VkCopyDescriptorSet *copies) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    std::vector<VkWriteDescriptorSet> redirected;
    std::vector<std::vector<VkDescriptorImageInfo>> pool;
    if (g_mv && g_active) {
        mv_redirect_writes(d, device, writeCount, writes, redirected, pool);
        d->UpdateDescriptorSets(device, writeCount, redirected.data(),
                                copyCount, copies);
    } else {
        d->UpdateDescriptorSets(device, writeCount, writes, copyCount, copies);
    }

    std::lock_guard<std::mutex> lock(g_track.mu);
    for (uint32_t i = 0; i < writeCount; i++) {
        const VkWriteDescriptorSet &w = writes[i];
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
            w.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            continue;
        if (!w.pBufferInfo)
            continue;
        for (uint32_t j = 0; j < w.descriptorCount; j++) {
            const VkDescriptorBufferInfo &bi = w.pBufferInfo[j];
            if (bi.range != kViewBlockRange)
                continue;
            auto &slots = g_track.sets[w.dstSet];
            ViewSlot s{bi.buffer, bi.offset};
            bool dup = false;
            for (auto &e : slots)
                if (e.buffer == s.buffer && e.offset == s.offset)
                    dup = true;
            if (!dup)
                slots.push_back(s);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdBindDescriptorSets(
    VkCommandBuffer cb, VkPipelineBindPoint bindPoint,
    VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
    const VkDescriptorSet *sets, uint32_t dynCount, const uint32_t *dyn) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    d->CmdBindDescriptorSets(cb, bindPoint, layout, firstSet, setCount, sets,
                             dynCount, dyn);
    // Only descriptor set 1 carries the per-view camera block for the main
    // geometry (established from the renderdoc capture: the range-1792 UBO
    // is bound at set 1 in 84% of the G-buffer pass's draws). Crediting any
    // set index made an auxiliary/UI view win the frame, which is why
    // patching it had no visible effect.
    static const uint32_t view_set = [] {
        const char *e = getenv("X4VR_VIEW_SET");
        return e ? (uint32_t)atoi(e) : 1u;
    }();
    std::lock_guard<std::mutex> lock(g_track.mu);
    auto &bound = g_track.bound[cb];
    for (uint32_t i = 0; i < setCount; i++) {
        if (firstSet + i != view_set)
            continue;
        auto it = g_track.sets.find(sets[i]);
        if (it == g_track.sets.end())
            continue;
        // replace: a new bind supersedes previously bound view slots
        bound = it->second;
    }
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdDraw(VkCommandBuffer cb, uint32_t vc,
                                        uint32_t ic, uint32_t fv,
                                        uint32_t fi) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    d->CmdDraw(cb, vc, ic, fv, fi);
    credit_draw(cb);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdDrawIndexed(VkCommandBuffer cb,
                                               uint32_t idxc, uint32_t ic,
                                               uint32_t fi, int32_t vo,
                                               uint32_t fin) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    d->CmdDrawIndexed(cb, idxc, ic, fi, vo, fin);
    credit_draw(cb);
}

// Phase 4b stage 0: print the render-pass partition so it can be read
// against docs/frame-analysis.md before anything is doubled.
//
// The same classification that decides the shear also seeds which passes
// become two-view, so this log is the seed made visible: every pass, why it
// landed where it did, and what it would cost. Opt-in -- X4 creates a lot of
// passes and this is a startup-time inventory, not a per-frame trace.
// Not gated on g_active at init -- g_active is only known once the instance
// is created -- so every user of this checks both. Without that, gamescope
// and Xwayland write their passes into the same log and the serials collide.
// (Today gamescope happens to composite with compute and creates none, which
// is luck, not a reason to skip the check.)
const bool g_mv_inventory = [] {
    const char *e = getenv("X4VR_MV_INVENTORY");
    return e && *e && *e != '0';
}();
uint32_t g_rp_serial = 0;

// Classify each subpass as "must not be sheared":
//   * no colour attachments        -> shadow cascade (light space)
//   * all colour attachments LDR   -> UI / final blit (screen space)
// Everything else is world geometry rendered through the camera projection,
// which is what K was derived for.
template <typename CreateInfo>
std::vector<bool> classify_unsheared(const CreateInfo *ci) {
    std::vector<bool> unsheared(ci->subpassCount, false);
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const auto &sp = ci->pSubpasses[i];
        if (sp.colorAttachmentCount == 0) {
            unsheared[i] = true; // depth-only: shadow pass
            continue;
        }
        bool all_ldr = true, any = false;
        for (uint32_t c = 0; c < sp.colorAttachmentCount; c++) {
            const uint32_t a = sp.pColorAttachments[c].attachment;
            if (a == VK_ATTACHMENT_UNUSED || a >= ci->attachmentCount)
                continue;
            any = true;
            if (!is_ldr_format(ci->pAttachments[a].format))
                all_ldr = false;
        }
        unsheared[i] = any && all_ldr;
    }
    return unsheared;
}

// Does this pass render into both eyes?
//
// Stage 1 deliberately reuses the inverse of the shear classification, which
// excludes two groups for two different reasons:
//
//   * depth-only (shadow cascades) -- light space, genuinely shared;
//   * all-LDR (UI and the final blit) -- deferred, not shared. The UI *does*
//     belong in both eyes, but the final blit's attachment is the swapchain
//     image, which cannot take a second array layer because it is the thing
//     being presented. Handing that off needs SbsCompositor's images to
//     become one two-layer image, which is stage 2. Leaving the UI mono here
//     costs nothing under test, because with one K both eyes would draw it
//     identically anyway.
//
// It also over-includes the exposure reductions, which are indistinguishable
// from legitimate post passes at this point (a render pass names no extents,
// and 4096x1 is only visible on the framebuffer). Harmless while both eyes
// match; must be fixed before K differs. Recorded in the test plan under
// "what these gates cannot catch".
template <typename CreateInfo>
bool pass_is_per_eye(const CreateInfo *ci) {
    if (!g_mv || !g_multiview_supported)
        return false;
    for (bool un : classify_unsheared(ci))
        if (!un)
            return true;
    return false;
}

template <typename CreateInfo>
void record_render_pass(const CreateInfo *ci, VkRenderPass rp) {
    std::vector<bool> unsheared = classify_unsheared(ci);

    std::lock_guard<std::mutex> lock(g_variants.mu);
    if (g_mv_inventory && g_active) {
        const uint32_t serial = g_rp_serial++;
        g_rp_serials[rp] = serial;
        for (uint32_t i = 0; i < ci->subpassCount; i++) {
            const auto &sp = ci->pSubpasses[i];
            // Formats say more than the verdict does: they are how a
            // misclassification is recognised on sight (an HDR target in the
            // mono bucket, an LDR one in the stereo bucket).
            char fmts[192];
            int n = 0;
            fmts[0] = 0;
            for (uint32_t c = 0; c < sp.colorAttachmentCount && n < 160; c++) {
                const uint32_t a = sp.pColorAttachments[c].attachment;
                if (a == VK_ATTACHMENT_UNUSED || a >= ci->attachmentCount)
                    continue;
                n += snprintf(fmts + n, sizeof(fmts) - n, "%s%u%s",
                              n ? "," : "", ci->pAttachments[a].format,
                              is_ldr_format(ci->pAttachments[a].format) ? "L"
                                                                        : "H");
            }
            const uint32_t da =
                sp.pDepthStencilAttachment
                    ? sp.pDepthStencilAttachment->attachment
                    : VK_ATTACHMENT_UNUSED;
            char dep[32] = " no-depth";
            if (da != VK_ATTACHMENT_UNUSED && da < ci->attachmentCount)
                snprintf(dep, sizeof(dep), " depth %u",
                         ci->pAttachments[da].format);
            const char *why = sp.colorAttachmentCount == 0 ? "depth-only/shadow"
                              : unsheared[i]               ? "all-LDR/UI"
                                                           : "world";
            X4VR_LOG("rp #%u.%u: %u colour [%s]%s -> %s (%s)", serial, i,
                     sp.colorAttachmentCount, fmts, dep,
                     unsheared[i] ? "MONO" : "STEREO", why);
        }
    }

    g_variants.unsheared[rp] = std::move(unsheared);
}

// What layout each attachment is left in when the pass ends. This is the one
// piece of information that makes the layer-1 readback sound: a barrier's
// oldLayout has to match the image's actual layout, and VK_IMAGE_LAYOUT_
// UNDEFINED -- the only "don't care" -- permits the driver to discard the
// contents, which is precisely what we are trying to read. Taken from the
// render pass rather than tracked, because the render pass performs the
// transition itself and therefore cannot be wrong about it.
std::unordered_map<VkRenderPass, std::vector<VkImageLayout>> g_rp_final;

template <class CreateInfo>
void record_final_layouts(const CreateInfo *ci, VkRenderPass rp) {
    std::vector<VkImageLayout> f(ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; i++)
        f[i] = ci->pAttachments[i].finalLayout;
    std::lock_guard<std::mutex> lock(g_variants.mu);
    g_rp_final[rp] = std::move(f);
}

void mark_masked(VkRenderPass rp) {
    std::lock_guard<std::mutex> lock(g_variants.mu);
    g_masked_passes.insert(rp);
    std::lock_guard<std::mutex> lock2(g_mv_mu);
    g_mv_stats.masked++;
}

static const uint32_t corr2 = kViewMask;

// Gate 1's pass condition, in one line. Reported at the first present (early
// enough to abort a bad run) and again at device teardown (the final tally,
// since framebuffers keep being created during play).
//
// fallbacks is the load-bearing number and it must be 0. Individual
// fallbacks are logged by name as they happen, so a non-zero total here is a
// summary of something already explained above it, never a first mention.
void mv_report(const char *when) {
    if (!g_mv)
        return;
    std::lock_guard<std::mutex> lock(g_mv_mu);
    size_t per_eye_imgs;
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        per_eye_imgs = g_per_eye_images.size();
    }
    X4VR_LOG("mv %s: viewMask=0x%x doubled=%u masked=%u substituted=%u "
             "per_eye_images=%zu redirected=%u fallbacks=%u%s",
             when, kViewMask, g_mv_stats.doubled, g_mv_stats.masked,
             g_mv_stats.substituted, per_eye_imgs, g_mv_stats.redirected,
             g_mv_stats.fallbacks,
             g_mv_stats.fallbacks ? "  <-- NOT CLEAN" : "");
    X4VR_LOG("mv %s: pipelines masked=%u unmasked=%u dynamic_rendering=%u "
             "transfers_widened=%u",
             when, g_mv_stats.pipe_masked, g_mv_stats.pipe_unmasked,
             g_mv_stats.pipe_dynamic, g_mv_stats.widened);
    // Images with writers of both kinds. Layer 1 misses every unmasked
    // contribution, so this list is the shortlist for any partial divergence.
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        // Printed even when nothing is mixed. An empty result otherwise means
        // either "no image has both kinds of writer" or "the detector never
        // ran", and those are not the same answer.
        X4VR_LOG("mv %s: writers tracked for %zu doubled images",
                 when, g_img_writers.size());
        for (const auto &e : g_img_writers) {
            // Every doubled image's writers, not only the mixed ones. Layer 1
            // holds correctly rasterised but unlit geometry, so the pass that
            // applies lighting is the one to find, and it can only be found
            // by name here -- image serials restart per run, which makes
            // cross-referencing an older inventory log unsound.
            //
            // Pass serials only exist when the inventory is on, so without it
            // every entry here is unnumbered. That prints as "?" rather than
            // as UINT32_MAX, which reads like a pass that does not exist.
            char ms[160] = {0}, us[160] = {0};
            int n = 0;
            for (uint32_t rp : e.second.masked)
                if (n < 140) {
                    if (rp == UINT32_MAX)
                        n += snprintf(ms + n, sizeof(ms) - n, "%s?",
                                      n ? "," : "");
                    else
                        n += snprintf(ms + n, sizeof(ms) - n, "%s%u",
                                      n ? "," : "", rp);
                }
            n = 0;
            for (uint32_t rp : e.second.unmasked)
                if (n < 140) {
                    if (rp == UINT32_MAX)
                        n += snprintf(us + n, sizeof(us) - n, "%s?",
                                      n ? "," : "");
                    else
                        n += snprintf(us + n, sizeof(us) - n, "%s%u",
                                      n ? "," : "", rp);
                }
            X4VR_LOG("mv %s: img #%u writers — masked rp [%s] unmasked rp [%s]",
                     when, e.first, ms, us);
            if (e.second.masked.empty() || e.second.unmasked.empty())
                continue;
            X4VR_LOG("mv %s: img #%u MIXED WRITERS — layer 1 misses the "
                     "unmasked ones", when, e.first);
        }
    }
    // The two candidates, side by side. bind_mismatch > 0 means the draws
    // themselves never reached view 1 and nothing downstream matters yet;
    // barrier_narrow > 0 means they did but layer 1 is read in a layout no
    // barrier ever moved it to.
    X4VR_LOG("mv %s: binds ok=%u MISMATCHED=%u | image barriers narrow=%u "
             "wide=%u | per-eye images written layer-0-only=%u | "
             "stale redirect entries=%u | input attachments fixed=%u",
             when, g_mv_stats.bind_ok, g_mv_stats.bind_mismatch,
             g_mv_stats.barrier_narrow, g_mv_stats.barrier_wide,
             g_mv_stats.layer0_only, g_mv_stats.redirect_stale,
             g_mv_stats.input_fixed);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkRenderPass *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    // Every subpass shares one mask: X4's passes are all single-subpass, and
    // the spec forbids mixing zero and non-zero masks within a render pass.
    const bool per_eye = pass_is_per_eye(ci);
    std::vector<uint32_t> masks;
    uint32_t corr = kViewMask;
    VkRenderPassMultiviewCreateInfo mv{};
    VkRenderPassCreateInfo mod = *ci;
    if (per_eye) {
        masks.assign(ci->subpassCount, kViewMask);
        mv.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
        mv.subpassCount = ci->subpassCount;
        mv.pViewMasks = masks.data();
        mv.correlationMaskCount = 1;
        mv.pCorrelationMasks = &corr;
        mv.pNext = ci->pNext;
        mod.pNext = &mv;
    }
    static bool logged_v1 = false;
    if (g_mv && g_active && !logged_v1) {
        logged_v1 = true;
        X4VR_LOG("mv: X4 uses vkCreateRenderPass (v1)");
    }
    VkResult r = d->CreateRenderPass(device, per_eye ? &mod : ci, ac, out);
    if (r == VK_SUCCESS) {
        record_render_pass(ci, *out);
        if (per_eye) {
            record_final_layouts(ci, *out);
            mark_masked(*out);
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateRenderPass2(
    VkDevice device, const VkRenderPassCreateInfo2 *ci,
    const VkAllocationCallbacks *ac, VkRenderPass *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    // Core only since Vulkan 1.2; fall back to the KHR alias if the device
    // exposes only that, and refuse cleanly if neither exists.
    PFN_vkCreateRenderPass2 next = d->CreateRenderPass2;
    if (!next)
        next = (PFN_vkCreateRenderPass2)d->gdpa(device, "vkCreateRenderPass2KHR");
    if (!next)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    // RenderPass2 carries the mask on each subpass instead of in a pNext
    // struct, so the subpass array has to be copied to set it.
    const bool per_eye = pass_is_per_eye(ci);
    std::vector<VkSubpassDescription2> subs;
    VkRenderPassCreateInfo2 mod = *ci;
    if (per_eye) {
        subs.assign(ci->pSubpasses, ci->pSubpasses + ci->subpassCount);
        for (auto &sp : subs)
            sp.viewMask = kViewMask;
        mod.pSubpasses = subs.data();
        mod.correlatedViewMaskCount = 1;
        mod.pCorrelatedViewMasks = &corr2;
    }
    static bool logged_v2 = false;
    if (g_mv && g_active && !logged_v2) {
        logged_v2 = true;
        X4VR_LOG("mv: X4 uses vkCreateRenderPass2");
    }
    VkResult r = next(device, per_eye ? &mod : ci, ac, out);
    if (r == VK_SUCCESS) {
        record_render_pass(ci, *out);
        if (per_eye) {
            record_final_layouts(ci, *out);
            mark_masked(*out);
        }
    }
    return r;
}

// Should this image get a second array layer?
//
// The decision has to be made here, where an image has no framebuffer and no
// render pass, so it cannot be the precise one. It does not need to be: the
// two errors are not symmetric.
//
//   * Doubled but never rendered per-eye -> layer 1 is simply never touched.
//     Costs memory, nothing else.
//   * Rendered per-eye but not doubled -> a hard validation error at
//     vkCreateFramebuffer, naming the framebuffer and the attachment.
//
// So be permissive here and precise at the render pass, and let validation
// catch anything this rule is too narrow for. In particular the shadow atlas
// is doubled and wasted (~40 MB): a depth image gives no hint at creation
// whether it will back a shadow cascade or the main depth buffer, and 40 MB
// is far below where that matters.
bool mv_double_candidate(const VkImageCreateInfo *ci) {
    if (!g_mv || !g_multiview_supported)
        return false;
    if (ci->imageType != VK_IMAGE_TYPE_2D || ci->arrayLayers != 1 ||
        ci->samples != VK_SAMPLE_COUNT_1_BIT)
        return false;
    if (ci->flags & (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
                     VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                     VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                     VK_IMAGE_CREATE_SPARSE_ALIASED_BIT))
        return false;
    return (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateImage(
    VkDevice device, const VkImageCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkImage *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    const bool dbl = g_active && mv_double_candidate(ci);
    VkImageCreateInfo mod = *ci;
    if (dbl)
        mod.arrayLayers = 2;
    VkResult r = d->CreateImage(device, dbl ? &mod : ci, ac, out);
    // A driver that refuses the doubled image must not take the game down
    // with it: fall back to exactly what X4 asked for and let the render pass
    // stage report the resulting fallback.
    if (r != VK_SUCCESS && dbl) {
        X4VR_LOG("mv: vkCreateImage refused arrayLayers=2 for %ux%u fmt=%u "
                 "(%d) — falling back to single layer",
                 ci->extent.width, ci->extent.height, ci->format, r);
        r = d->CreateImage(device, ci, ac, out);
        if (r == VK_SUCCESS) {
            std::lock_guard<std::mutex> lock(g_mv_mu);
            g_mv_stats.fallbacks++;
        }
    }
    if (r != VK_SUCCESS || !g_active)
        return r;
    ImageInfo info{};
    info.extent = ci->extent;
    info.format = ci->format;
    info.layers = ci->arrayLayers;
    info.mips = ci->mipLevels;
    info.samples = (uint32_t)ci->samples;
    info.usage = ci->usage;
    info.doubled = dbl;
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        info.serial = g_img_serial++;
        g_images[*out] = info;
    }
    if (dbl) {
        std::lock_guard<std::mutex> lock(g_mv_mu);
        g_mv_stats.doubled++;
    }
    if (g_mv_inventory)
        X4VR_LOG("img #%u: %ux%ux%u layers=%u mips=%u samples=%u fmt=%u "
                 "usage=0x%x%s",
                 info.serial, ci->extent.width, ci->extent.height,
                 ci->extent.depth, ci->arrayLayers, ci->mipLevels,
                 (uint32_t)ci->samples, ci->format, ci->usage,
                 dbl ? " DOUBLED" : "");
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyImage(VkDevice device, VkImage img,
                                             const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        auto it = g_images.find(img);
        // Logged so the inventory can tell a live image from one belonging to
        // a scene that has since been torn down. X4 builds a full set of
        // targets for the menu and another for the game a second later, and
        // counting both inflates the doubling estimate.
        if (it != g_images.end() && g_mv_inventory)
            X4VR_LOG("img #%u: destroyed", it->second.serial);
        if (it != g_images.end())
            g_images.erase(it);
    }
    d->DestroyImage(device, img, ac);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateImageView(
    VkDevice device, const VkImageViewCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkImageView *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    bool doubled = false;
    if (g_active) {
        std::lock_guard<std::mutex> lock(g_img_mu);
        auto it = g_images.find(ci->image);
        doubled = it != g_images.end() && it->second.doubled;
    }

    // Non-array views over a doubled image need two things done to them.
    //
    // First, the predicted failure mode, headed off rather than waited for:
    // X4 asks for a plain 2D view with layerCount = VK_REMAINING_ARRAY_LAYERS,
    // which used to mean "the one layer there is" and now resolves to 2 -- and
    // a non-array 2D view may only span one. Pin it to 1, so every view X4
    // makes keeps meaning what X4 meant by it. The array views the framebuffer
    // needs are built separately, by us.
    //
    // The layer-1 redirect used to live here and was wrong: it moved
    // baseArrayLayer on *every* doubled image. 92 images are doubled but only
    // ~21 are ever written by a view-masked pass, so the other 71 had their
    // reads pointed at a layer nothing had rendered into. It is applied at
    // descriptor-update time now, where the set of genuinely per-eye images
    // is known. See mv_redirect_writes().
    VkImageViewCreateInfo mod = *ci;
    const bool non_array = ci->viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY &&
                           ci->viewType != VK_IMAGE_VIEW_TYPE_CUBE &&
                           ci->viewType != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    if (doubled && non_array) {
        mod.subresourceRange.layerCount = 1;
        ci = &mod;
    }

    VkResult r = d->CreateImageView(device, ci, ac, out);
    if (r == VK_SUCCESS && g_active) {
        ViewInfo vi{};
        vi.image = ci->image;
        vi.format = ci->format;
        vi.components = ci->components;
        vi.range = ci->subresourceRange;
        vi.type = ci->viewType;
        std::lock_guard<std::mutex> lock(g_img_mu);
        g_views[*out] = vi; // the only link a framebuffer gives us
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyImageView(
    VkDevice device, VkImageView view, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    VkImageView sub = VK_NULL_HANDLE, l1 = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        g_views.erase(view);
        auto it = g_array_views.find(view);
        if (it != g_array_views.end()) {
            sub = it->second;
            g_array_views.erase(it);
        }
        // Both caches are keyed on a handle the driver is about to be free to
        // reuse. Leaving the gate-2 entry behind meant a later view with the
        // same handle inherited a redirect built for a different image -- and
        // the replacements were never destroyed either, so they accumulated
        // for the whole session.
        auto l = g_layer1_views.find(view);
        if (l != g_layer1_views.end()) {
            l1 = l->second.view;
            g_layer1_views.erase(l);
        }
    }
    // Our substitutes are owned by us and outlive nothing: they die with the
    // view they stood in for.
    if (sub != VK_NULL_HANDLE)
        d->DestroyImageView(device, sub, ac);
    if (l1 != VK_NULL_HANDLE)
        d->DestroyImageView(device, l1, ac);
    d->DestroyImageView(device, view, ac);
}

// The render pass says what a pass *is*; only the framebuffer says how big it
// is, and size is what decides whether doubling it costs 16 MB or 60 KB. It
// also carries the layer count, which is the thing the doubling has to change
// and which validation checks against the view mask.
//
// And it is the one place that names a pass and its images together, so this
// is where the image serials get attached. An attachment that resolves to no
// image (printed "?") came from the swapchain, which the driver creates
// behind vkGetSwapchainImagesKHR rather than through vkCreateImage.
//
// Joined to the pass inventory by serial, so the logs read together.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkFramebuffer *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    uint32_t serial = UINT32_MAX;
    bool masked = false;
    std::vector<VkImageLayout> finals;
    {
        std::lock_guard<std::mutex> lock(g_variants.mu);
        auto it = g_rp_serials.find(ci->renderPass);
        if (it != g_rp_serials.end())
            serial = it->second;
        masked = g_masked_passes.count(ci->renderPass) != 0;
        auto f = g_rp_final.find(ci->renderPass);
        if (f != g_rp_final.end())
            finals = f->second;
    }

    // A view-masked pass needs every attachment to be an array view spanning
    // both layers. X4's own views are single-layer by construction, so we
    // build array views over the same images and swap them in. X4 never sees
    // these -- they exist only inside the framebuffer.
    std::vector<VkImageView> subs;
    VkFramebufferCreateInfo mod = *ci;
    if (masked && g_active) {
        subs.assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            VkImageView orig = ci->pAttachments[i];
            ViewInfo vi{};
            bool have = false, doubled = false, cached = false;
            {
                std::lock_guard<std::mutex> lock(g_img_mu);
                auto v = g_views.find(orig);
                if (v != g_views.end()) {
                    vi = v->second;
                    have = true;
                    auto im = g_images.find(vi.image);
                    doubled = im != g_images.end() && im->second.doubled;
                }
                auto c = g_array_views.find(orig);
                if (c != g_array_views.end()) {
                    subs[i] = c->second;
                    cached = true;
                }
            }
            if (cached)
                continue;
            if (!have || !doubled) {
                // Loud, and by name. A silent single-layer attachment here is
                // the failure that resurfaces later as an unexplained
                // artifact -- see docs/phase4b-test-plan.md, gate 1.
                X4VR_LOG("mv: FALLBACK rp #%u attachment %u — %s; pass is "
                         "view-masked but this attachment cannot be doubled",
                         serial, i,
                         !have ? "view not tracked (swapchain image?)"
                               : "backing image is single-layer");
                std::lock_guard<std::mutex> lock(g_mv_mu);
                g_mv_stats.fallbacks++;
                continue;
            }
            VkImageViewCreateInfo avci{};
            avci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            avci.image = vi.image;
            avci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            avci.format = vi.format;
            avci.components = vi.components;
            avci.subresourceRange = vi.range;
            avci.subresourceRange.baseArrayLayer = 0;
            avci.subresourceRange.layerCount = 2;
            VkImageView av = VK_NULL_HANDLE;
            if (d->CreateImageView(device, &avci, nullptr, &av) == VK_SUCCESS) {
                subs[i] = av;
                std::lock_guard<std::mutex> lock2(g_img_mu);
                g_array_views[orig] = av;
                // This image really is written into both layers -- the only
                // set for which reading layer 1 is meaningful.
                g_per_eye_images.insert(vi.image);
                std::lock_guard<std::mutex> lock3(g_mv_mu);
                g_mv_stats.substituted++;
            } else {
                X4VR_LOG("mv: FALLBACK rp #%u attachment %u — array view "
                         "creation failed", serial, i);
                std::lock_guard<std::mutex> lock3(g_mv_mu);
                g_mv_stats.fallbacks++;
            }
        }
        mod.pAttachments = subs.data();
        // Multiview draws into array layers, so the framebuffer itself is
        // one layer deep; the view mask supplies the rest.
        mod.layers = 1;
    }

    VkResult r = d->CreateFramebuffer(device, masked && g_active ? &mod : ci,
                                      ac, out);

    // Who writes each doubled image, masked or not. Every pass, not just the
    // masked ones -- the whole point is to find images that get both.
    if (r == VK_SUCCESS && g_mv && g_active) {
        std::lock_guard<std::mutex> lock(g_img_mu);
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            auto v = g_views.find(ci->pAttachments[i]);
            if (v == g_views.end())
                continue;
            auto im = g_images.find(v->second.image);
            if (im == g_images.end() || !im->second.doubled)
                continue;
            auto &k = g_img_writers[im->second.serial];
            auto &side = masked ? k.masked : k.unmasked;
            if (std::find(side.begin(), side.end(), serial) == side.end())
                side.push_back(serial);
        }
    }

    // Everything the readback needs about this pass's attachments, recorded
    // while the framebuffer still names them. Colour only: a depth copy has
    // aspect and format rules of its own, and probing depth is not what the
    // open question needs.
    if (r == VK_SUCCESS && masked && g_active && !finals.empty()) {
        std::vector<FbAtt> atts;
        {
            std::lock_guard<std::mutex> lock(g_img_mu);
            for (uint32_t i = 0; i < ci->attachmentCount && i < finals.size();
                 i++) {
                auto v = g_views.find(ci->pAttachments[i]);
                if (v == g_views.end())
                    continue;
                auto im = g_images.find(v->second.image);
                if (im == g_images.end() || !im->second.doubled)
                    continue;
                if (!(v->second.range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT))
                    continue;
                // Reading it back requires it to be a transfer source. Every
                // per-eye image we have seen is, but assuming so would put an
                // unchecked precondition inside the instrument.
                if (!(im->second.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                    continue;
                atts.push_back(FbAtt{v->second.image, im->second.serial,
                                     im->second.format, im->second.extent,
                                     finals[i]});
            }
        }
        if (!atts.empty()) {
            std::lock_guard<std::mutex> lock(g_cb_mu);
            g_fb_atts[*out] = std::move(atts);
        }
    }

    if (r == VK_SUCCESS && g_mv_inventory && g_active) {
        char imgs[256];
        int n = 0;
        imgs[0] = 0;
        {
            std::lock_guard<std::mutex> lock(g_img_mu);
            for (uint32_t i = 0; i < ci->attachmentCount && n < 230; i++) {
                auto v = g_views.find(ci->pAttachments[i]);
                auto im = v == g_views.end() ? g_images.end()
                                             : g_images.find(v->second.image);
                if (im == g_images.end())
                    n += snprintf(imgs + n, sizeof(imgs) - n, "%s?",
                                  n ? "," : "");
                else
                    n += snprintf(imgs + n, sizeof(imgs) - n, "%s#%u",
                                  n ? "," : "", im->second.serial);
            }
        }
        X4VR_LOG("fb  rp #%u: %ux%u layers=%u attachments=%u imgs=[%s]%s",
                 serial, ci->width, ci->height, ci->layers,
                 ci->attachmentCount, imgs, masked ? " MASKED" : "");
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyRenderPass(
    VkDevice device, VkRenderPass rp, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    {
        std::lock_guard<std::mutex> lock(g_variants.mu);
        g_variants.unsheared.erase(rp);
        g_rp_serials.erase(rp);
        g_masked_passes.erase(rp);
    }
    d->DestroyRenderPass(device, rp, ac);
}

// Phase 3b: bake a constant clip-space matrix into every scene vertex
// shader (see common/x4vr_spirv.hpp). Zero per-frame cost — the whole
// point of the camera-relative clip-space identity.
//
// X4VR_CLIP_K = 16 comma-separated floats (column-major). For a quick
// visible proof, X4VR_CLIP_SHIFT=<x> is shorthand for a clip-space
// x-translation, which slides the image sideways in NDC.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkShaderModule *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }

    // Two matrices, chosen per module by static classification:
    //   K_world — world geometry (set-3 per-object block): gets the eye offset
    //   K_ui    — screen-space modules (UI/HUD *and* fullscreen post passes):
    //             identity by default. Giving these the world eye offset would
    //             not just misplace the HUD, it would corrupt every fullscreen
    //             post pass, so they must never share K_world.
    static bool have_k = false;
    static float K_world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static float K_ui[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static std::once_flag once;
    std::call_once(once, [] {
        auto parse16 = [](const char *s, float *m) {
            int i = 0;
            for (const char *p = s; *p && i < 16; i++) {
                m[i] = strtof(p, (char **)&p);
                if (*p == ',')
                    p++;
            }
            return i == 16;
        };
        if (const char *s = getenv("X4VR_CLIP_K"))
            have_k |= parse16(s, K_world);
        if (const char *sh = getenv("X4VR_CLIP_SHIFT")) {
            K_world[12] = strtof(sh, nullptr); // column-major: col 3 = translation
            have_k = true;
        }
        // Phase 4a: a real per-eye matrix derived from X4's projection.
        //   X4VR_EYE=left|right, X4VR_IPD=<metres>
        // sx / near default to the values measured at 2816x1408 (see
        // docs/frame-analysis.md); they are overridable until the layer
        // derives them from the live camera block automatically.
        if (const char *eye = getenv("X4VR_EYE")) {
            const float ipd = getenv("X4VR_IPD")
                                  ? strtof(getenv("X4VR_IPD"), nullptr)
                                  : 0.064f;
            const float sx = getenv("X4VR_PROJ_SX")
                                 ? strtof(getenv("X4VR_PROJ_SX"), nullptr)
                                 : 0.889f;
            const float nz = getenv("X4VR_PROJ_NEAR")
                                 ? strtof(getenv("X4VR_PROJ_NEAR"), nullptr)
                                 : 0.1f;
            const bool right = (eye[0] == 'r' || eye[0] == 'R');
            const float dx = (right ? +0.5f : -0.5f) * ipd;
            const x4vr::Mat4 k = x4vr::make_eye_shear(sx, 0.0f, nz, dx);
            memcpy(K_world, k.m, sizeof(k.m));
            have_k = true;
            X4VR_LOG("eye=%s ipd=%.4f sx=%.4f near=%.3f -> K shear m8=%.5f",
                     right ? "right" : "left", ipd, sx, nz, K_world[8]);
        }
        if (const char *s = getenv("X4VR_CLIP_K_UI"))
            have_k |= parse16(s, K_ui);
        if (const char *sh = getenv("X4VR_CLIP_SHIFT_UI")) {
            K_ui[12] = strtof(sh, nullptr);
            have_k = true;
        }
        if (have_k)
            X4VR_LOG("clip-space enabled: K_world.x=%.3f K_ui.x=%.3f",
                     K_world[12], K_ui[12]);
    });

    if (!have_k || !ci->pCode || ci->codeSize < 20)
        return d->CreateShaderModule(device, ci, ac, out);

    std::vector<uint32_t> code(ci->codeSize / 4);
    memcpy(code.data(), ci->pCode, ci->codeSize);

    const x4vr::spv::Kind kind = x4vr::spv::classify(code);
    if (kind == x4vr::spv::Kind::NotVertex)
        return d->CreateShaderModule(device, ci, ac, out);
    const float *K = (kind == x4vr::spv::Kind::World) ? K_world : K_ui;

    static uint32_t patched = 0, n_world = 0, n_ui = 0;
    (kind == x4vr::spv::Kind::World ? n_world : n_ui)++;
    if (x4vr::spv::patch_vertex_clip(code, K)) {
        VkShaderModuleCreateInfo mod = *ci;
        mod.codeSize = code.size() * 4;
        mod.pCode = code.data();
        VkResult r = d->CreateShaderModule(device, &mod, ac, out);
        if (r == VK_SUCCESS) {
            // Keep an unpatched twin so depth-only (shadow) pipelines can
            // use the original geometry path — see g_variants.
            VkShaderModule orig = VK_NULL_HANDLE;
            if (d->CreateShaderModule(device, ci, ac, &orig) == VK_SUCCESS) {
                std::lock_guard<std::mutex> lock(g_variants.mu);
                g_variants.original[*out] = orig;
            }
            if (++patched <= 3 || (patched % 50) == 0)
                X4VR_LOG("patched vertex shader #%u (%s) [world=%u ui=%u]",
                         patched,
                         kind == x4vr::spv::Kind::World ? "world" : "ui",
                         n_world, n_ui);
            return r;
        }
        // Patched module rejected by the driver: fall back to the original
        // rather than failing the game's shader creation.
        X4VR_LOG("WARNING: driver rejected patched module (%d); using original",
                 (int)r);
    }
    return d->CreateShaderModule(device, ci, ac, out);
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyShaderModule(
    VkDevice device, VkShaderModule mod, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    VkShaderModule twin = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_variants.mu);
        auto it = g_variants.original.find(mod);
        if (it != g_variants.original.end()) {
            twin = it->second;
            g_variants.original.erase(it);
        }
    }
    if (twin != VK_NULL_HANDLE)
        d->DestroyShaderModule(device, twin, ac);
    d->DestroyShaderModule(device, mod, ac);
}

// Pick the unpatched module variant for depth-only (shadow) pipelines.
// This is the whole shadow-exclusion mechanism: one substitution at
// pipeline-creation time, nothing per frame.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *ac,
    VkPipeline *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }

    // Phase 4b diagnosis: a pipeline is compiled for multiview only if the
    // render pass it is created against already carries the view mask. If X4
    // builds pipelines against unmasked passes -- or against no render pass
    // at all, i.e. dynamic rendering, where the mask lives in a pNext struct
    // we do not touch -- the draws replicate to one view and layer 1 stays
    // exactly as allocated. Counting them separates that from every other
    // explanation.
    if (g_mv && g_active) {
        uint32_t masked = 0, unmasked = 0, dynamic = 0;
        {
            std::lock_guard<std::mutex> lock(g_variants.mu);
            for (uint32_t i = 0; i < count; i++) {
                if (ci[i].renderPass == VK_NULL_HANDLE)
                    dynamic++;
                else if (g_masked_passes.count(ci[i].renderPass))
                    masked++;
                else
                    unmasked++;
            }
        }
        std::lock_guard<std::mutex> lock(g_mv_mu);
        g_mv_stats.pipe_masked += masked;
        g_mv_stats.pipe_unmasked += unmasked;
        g_mv_stats.pipe_dynamic += dynamic;
    }

    // Remember which of them were compiled for two views, so binding one into
    // a masked pass can be checked. Recorded after the call, below, once the
    // handles exist.
    auto record_provenance = [&](VkResult r) {
        if (!g_mv || !g_active || r != VK_SUCCESS)
            return;
        std::vector<bool> mv(count);
        {
            std::lock_guard<std::mutex> lock(g_variants.mu);
            for (uint32_t i = 0; i < count; i++)
                mv[i] = ci[i].renderPass != VK_NULL_HANDLE &&
                        g_masked_passes.count(ci[i].renderPass) != 0;
        }
        std::lock_guard<std::mutex> lock(g_cb_mu);
        for (uint32_t i = 0; i < count; i++)
            if (out[i] != VK_NULL_HANDLE)
                g_pipe_mv[out[i]] = mv[i];
    };

    // Copy only if we actually need to substitute something.
    std::vector<VkGraphicsPipelineCreateInfo> infos;
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages;
    bool any = false;
    for (uint32_t i = 0; i < count; i++) {
        if (ci[i].renderPass == VK_NULL_HANDLE ||
            !needs_original(ci[i].renderPass, ci[i].subpass))
            continue;
        if (!any) {
            infos.assign(ci, ci + count);
            stages.resize(count);
            any = true;
        }
        stages[i].assign(ci[i].pStages, ci[i].pStages + ci[i].stageCount);
        for (auto &st : stages[i]) {
            std::lock_guard<std::mutex> lock(g_variants.mu);
            auto it = g_variants.original.find(st.module);
            if (it != g_variants.original.end()) {
                st.module = it->second;
                g_variants.swapped++;
            }
        }
        infos[i].pStages = stages[i].data();
    }
    if (!any) {
        VkResult r = d->CreateGraphicsPipelines(device, cache, count, ci, ac,
                                                out);
        record_provenance(r);
        return r;
    }

    static bool logged = false;
    if (!logged) {
        logged = true;
        X4VR_LOG("unsheared pipeline: using unpatched modules (shadow + UI "
                 "exclusion active)");
    }
    VkResult r =
        d->CreateGraphicsPipelines(device, cache, count, infos.data(), ac, out);
    record_provenance(r);
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyPipeline(
    VkDevice device, VkPipeline p, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    if (g_mv) {
        std::lock_guard<std::mutex> lock(g_cb_mu);
        g_pipe_mv.erase(p);
    }
    d->DestroyPipeline(device, p, ac);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_MapMemory(VkDevice device,
                                              VkDeviceMemory memory,
                                              VkDeviceSize offset,
                                              VkDeviceSize size,
                                              VkMemoryMapFlags flags,
                                              void **ppData) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    VkResult r = d->MapMemory(device, memory, offset, size, flags, ppData);
    if (r == VK_SUCCESS && ppData) {
        std::lock_guard<std::mutex> lock(g_track.mu);
        g_track.mapped[memory] = {*ppData, offset, size};
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_UnmapMemory(VkDevice device,
                                            VkDeviceMemory memory) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    {
        std::lock_guard<std::mutex> lock(g_track.mu);
        g_track.mapped.erase(memory);
    }
    d->UnmapMemory(device, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_BindBufferMemory(VkDevice device,
                                                     VkBuffer buffer,
                                                     VkDeviceMemory memory,
                                                     VkDeviceSize offset) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    VkResult r = d->BindBufferMemory(device, buffer, memory, offset);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_track.mu);
        g_track.buf_binding[buffer] = {memory, offset};
    }
    return r;
}

// Read (and optionally perturb) the main view block just before the GPU
// consumes it. Submit time is the correct moment: X4 has finished its CPU
// writes for this frame, and the command buffers that read the block have
// already been recorded, so the winning slot for this frame is known.
void patch_view_before_submit() {
    static const float ox = [] {
        const char *e = getenv("X4VR_TEST_EYE_X");
        return e ? (float)atof(e) : 0.0f;
    }();
    static const float oy = [] {
        const char *e = getenv("X4VR_TEST_EYE_Y");
        return e ? (float)atof(e) : 0.0f;
    }();
    static const float oz = [] {
        const char *e = getenv("X4VR_TEST_EYE_Z");
        return e ? (float)atof(e) : 0.0f;
    }();
    static const bool dump = [] {
        const char *e = getenv("X4VR_DUMP_MATRICES");
        return e && *e && *e != '0';
    }();
    if (ox == 0.0f && oy == 0.0f && oz == 0.0f && !dump)
        return;

    std::lock_guard<std::mutex> lock(g_track.mu);
    // winner of the frame so far = the main camera
    const ViewSlot *best = nullptr;
    uint32_t best_n = 0;
    for (auto &[slot, n] : g_track.credit)
        if (n > best_n) {
            best = &slot;
            best_n = n;
        }
    // Require a real scene: the splash/menu transition renders a couple of
    // draws against a block X4 has not populated yet (reads back all zeros).
    if (!best || best_n < 50)
        return;
    float *blk = slot_host_ptr(*best);
    if (!blk)
        return;

    x4vr::Mat4 view = x4vr::load(blk + x4vr::kView);
    // Sanity: a real view matrix is affine with m[15] == 1.
    if (std::fabs(view.m[15] - 1.0f) > 1e-3f)
        return;

    const x4vr::Mat4 proj_probe = x4vr::load(blk + x4vr::kProjection);
    if (g_track.major == x4vr::Major::Unknown) {
        // Prefer the projection matrix: X4 renders camera-relative, so its
        // M_view is identity and cannot disambiguate the storage order.
        g_track.major = x4vr::detect_major_proj(proj_probe);
        if (g_track.major == x4vr::Major::Unknown)
            g_track.major = x4vr::detect_major(view);
    }
    const x4vr::Major major = g_track.major;

    if (dump && !g_track.logged_matrices) {
        g_track.logged_matrices = true;
        char b[512];
        x4vr::format_mat(b, sizeof(b), view);
        X4VR_LOG("M_view          %s", b);
        x4vr::format_mat(b, sizeof(b), x4vr::load(blk + x4vr::kProjection));
        X4VR_LOG("M_projection    %s", b);
        x4vr::format_mat(b, sizeof(b), x4vr::load(blk + x4vr::kViewProjection));
        X4VR_LOG("M_viewprojection%s", b);
        x4vr::format_mat(b, sizeof(b), x4vr::load(blk + x4vr::kViewInverse));
        X4VR_LOG("M_viewinverse   %s", b);
        X4VR_LOG("storage order detected: %s (draws=%u)",
                 major == x4vr::Major::Column ? "column-major"
                 : major == x4vr::Major::Row  ? "row-major"
                                              : "UNKNOWN",
                 best_n);
    }

    if (ox == 0.0f && oy == 0.0f && oz == 0.0f)
        return;
    if (major == x4vr::Major::Unknown)
        return; // refuse to guess

    // X4VR_TEST_PERIOD=<seconds>: alternate the offset on/off so a single
    // run shows both viewpoints. The menu background scene is randomised per
    // launch, so comparing across runs proves nothing; comparing within one
    // run does.
    static const double period = [] {
        const char *e = getenv("X4VR_TEST_PERIOD");
        return e ? atof(e) : 0.0;
    }();
    if (period > 0.0) {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double t = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (((long)(t / period)) % 2 == 0)
            return; // "off" half of the cycle: leave X4's camera alone
    }

    // X4VR_TEST_ZAP=vp|view|proj — destroy one matrix outright. A blunt but
    // decisive probe of whether the geometry actually consumes this block:
    // if zeroing it does not change the image, the vertex path is getting
    // its transform from somewhere else (e.g. per-object constants).
    static const char *zap = getenv("X4VR_TEST_ZAP");
    if (zap && *zap) {
        x4vr::Mat4 zero{};
        if (!strcmp(zap, "survey")) {
            // Walk every block of the arena and report the ones that look
            // like real view constants. The main camera is identified by a
            // perspective projection whose aspect matches the swapchain and
            // (unlike the block draw-crediting picks) a non-identity view.
            static bool surveyed = false;
            if (!surveyed) {
                surveyed = true;
                auto bb = g_track.buf_binding.find(best->buffer);
                auto sz = g_track.ubo_buffers.find(best->buffer);
                auto mp = (bb != g_track.buf_binding.end())
                              ? g_track.mapped.find(bb->second.memory)
                              : g_track.mapped.end();
                if (mp != g_track.mapped.end() && mp->second.ptr &&
                    sz != g_track.ubo_buffers.end()) {
                    uint8_t *base = (uint8_t *)mp->second.ptr +
                                    (bb->second.offset - mp->second.offset);
                    const uint32_t nblocks =
                        (uint32_t)(sz->second / x4vr::kViewBlockBytes);
                    X4VR_LOG("SURVEY of %u blocks in arena %p:", nblocks,
                             (void *)best->buffer);
                    for (uint32_t b = 0; b < nblocks; b++) {
                        float *pb = (float *)(base + (size_t)b *
                                              x4vr::kViewBlockBytes);
                        x4vr::Mat4 v = x4vr::load(pb + x4vr::kView);
                        x4vr::Mat4 pr = x4vr::load(pb + x4vr::kProjection);
                        const bool view_ok =
                            std::fabs(v.m[15] - 1.0f) < 1e-3f;
                        const bool proj_ok =
                            std::fabs(pr.m[15]) < 1e-4f &&
                            std::fabs(std::fabs(pr.m[11]) - 1.0f) < 1e-3f &&
                            std::fabs(pr.m[0]) > 1e-6f;
                        if (!view_ok || !proj_ok)
                            continue;
                        uint32_t cred = 0;
                        for (auto &[sl, n] : g_track.credit)
                            if (sl.buffer == best->buffer &&
                                sl.offset == (VkDeviceSize)b *
                                                 x4vr::kViewBlockBytes)
                                cred = n;
                        const bool ident =
                            std::fabs(v.m[0] - 1.0f) < 1e-4f &&
                            std::fabs(v.m[5] - 1.0f) < 1e-4f &&
                            std::fabs(v.m[12]) < 1e-4f &&
                            std::fabs(v.m[13]) < 1e-4f &&
                            std::fabs(v.m[14]) < 1e-4f;
                        X4VR_LOG("  blk %3u draws=%4u view=%s trans=(%.1f,%.1f,%.1f) "
                                 "proj m0=%.4f m5=%.4f aspect=%.3f near=%.3f",
                                 b, cred, ident ? "IDENTITY" : "real   ",
                                 v.m[12], v.m[13], v.m[14], pr.m[0], pr.m[5],
                                 pr.m[0] != 0.0f ? pr.m[5] / pr.m[0] : 0.0f,
                                 pr.m[14]);
                    }
                }
            }
            return;
        }
        if (!strcmp(zap, "arena")) {
            // Zero the WHOLE arena buffer, not just one block. If the image
            // still renders, this buffer is not consumed by the GPU at all
            // and our identification of the view arena is wrong.
            auto bb = g_track.buf_binding.find(best->buffer);
            auto sz = g_track.ubo_buffers.find(best->buffer);
            if (bb != g_track.buf_binding.end() && sz != g_track.ubo_buffers.end()) {
                auto mp = g_track.mapped.find(bb->second.memory);
                if (mp != g_track.mapped.end() && mp->second.ptr) {
                    uint8_t *base = (uint8_t *)mp->second.ptr +
                                    (bb->second.offset - mp->second.offset);
                    memset(base, 0, (size_t)sz->second);
                    static bool once1 = false;
                    if (!once1) {
                        once1 = true;
                        X4VR_LOG("TEST: zeroing ENTIRE arena buffer %p (%llu bytes)",
                                 (void *)best->buffer,
                                 (unsigned long long)sz->second);
                    }
                }
            }
            return;
        }
        if (!strcmp(zap, "arena")) {
            // Zero the WHOLE arena buffer, not just one block. If the image
            // still renders, this buffer is not consumed by the GPU at all
            // and our identification of the view arena is wrong.
            auto bb = g_track.buf_binding.find(best->buffer);
            auto sz = g_track.ubo_buffers.find(best->buffer);
            if (bb != g_track.buf_binding.end() && sz != g_track.ubo_buffers.end()) {
                auto mp = g_track.mapped.find(bb->second.memory);
                if (mp != g_track.mapped.end() && mp->second.ptr) {
                    uint8_t *base = (uint8_t *)mp->second.ptr +
                                    (bb->second.offset - mp->second.offset);
                    memset(base, 0, (size_t)sz->second);
                    static bool once1 = false;
                    if (!once1) {
                        once1 = true;
                        X4VR_LOG("TEST: zeroing ENTIRE arena buffer %p (%llu bytes)",
                                 (void *)best->buffer,
                                 (unsigned long long)sz->second);
                    }
                }
            }
            return;
        }
        if (!strcmp(zap, "vp"))
            x4vr::store(blk + x4vr::kViewProjection, zero);
        else if (!strcmp(zap, "view"))
            x4vr::store(blk + x4vr::kView, zero);
        else if (!strcmp(zap, "proj"))
            x4vr::store(blk + x4vr::kProjection, zero);
        g_verify_ptr = blk;
        static bool once = false;
        if (!once) {
            once = true;
            X4VR_LOG("TEST: zapping %s of block at %p (draws=%u)", zap,
                     (void *)blk, best_n);
        }
        return;
    }

    // Shift the camera and rebuild every matrix derived from the view.
    const x4vr::Mat4 proj = x4vr::load(blk + x4vr::kProjection);
    const x4vr::Mat4 view2 = x4vr::offset_camera(view, major, ox, oy, oz);
    x4vr::store(blk + x4vr::kView, view2);
    x4vr::store(blk + x4vr::kViewProjection, x4vr::mul(proj, view2, major));
    x4vr::Mat4 vinv;
    if (x4vr::invert(view2, vinv))
        x4vr::store(blk + x4vr::kViewInverse, vinv);
}

// X4VR_TEST_HAMMER=1 — continuously zero M_viewprojection in every known
// view block from a background thread. This is a race-based oracle: if the
// GPU ever reads these blocks, hammering them at ~10 kHz is guaranteed to
// corrupt the image. If the image stays pristine, the blocks we identified
// are not what the visible geometry consumes, and no amount of careful
// timing on our side would have helped.
void hammer_thread() {
    X4VR_LOG("TEST: hammer thread started");
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_track.mu);
            for (auto &[slot, n] : g_track.credit) {
                (void)n;
                if (float *blk = slot_host_ptr(slot)) {
                    x4vr::Mat4 zero{};
                    x4vr::store(blk + x4vr::kViewProjection, zero);
                    x4vr::store(blk + x4vr::kProjection, zero);
                }
            }
        }
        usleep(100);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_QueueSubmit(VkQueue queue,
                                                uint32_t submitCount,
                                                const VkSubmitInfo *submits,
                                                VkFence fence) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(queue));
    }
    patch_view_before_submit();
    return d->QueueSubmit(queue, submitCount, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *ci,
    const VkAllocationCallbacks *ac, VkSwapchainKHR *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    // The composite copies one half of the image over the other, so the
    // images must be usable as both transfer source and destination. Ask for
    // that, and fall back to X4's own usage if the surface refuses -- a
    // direct test of what matters, rather than a caps query.
    VkSwapchainCreateInfoKHR sbs_ci = *ci;
    // We halved the width X4 read from the surface, so double it back: the
    // real swapchain has to match the surface, and it is what holds both
    // eyes. X4 never sees these images -- it gets ours.
    // X4 can be brought to the eye size by either route -- the halved
    // surface capabilities (X11, where currentExtent wins) or res_width in
    // the config (Wayland, where the surface declines to dictate a size and
    // X4 falls back to it). Both end here asking for exactly one eye, so key
    // off the request itself rather than off which lever moved it.
    const bool split = g_sbs_enabled && g_active && g_sbs_split_render &&
                       ci->imageExtent.width == X4VR_SBS_WIDTH / 2 &&
                       ci->imageExtent.height == X4VR_SBS_HEIGHT;
    if (split)
        sbs_ci.imageExtent.width *= 2;
    // X4 asks for FIFO, which pins the frame rate to the display and makes
    // every perf number a statement about the monitor. Nothing can be A/B'd
    // against a baseline that is also 59.4 fps, so allow the mode to be
    // overridden for measurement runs -- never by default, because FIFO is
    // the right mode to actually play in.
    //
    // Checked against the surface rather than assumed: IMMEDIATE is optional,
    // and creating a swapchain with an unsupported mode is undefined rather
    // than a clean failure we could fall back from.
    if (g_present_mode >= 0 && g_active) {
        VkPresentModeKHR want = (VkPresentModeKHR)g_present_mode;
        bool supported = false;
        if (d->phys != VK_NULL_HANDLE) {
            VkInstance inst = VK_NULL_HANDLE;
            PFN_vkGetInstanceProcAddr gipa = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                auto it = g_instances.find(dispatch_key(d->phys));
                if (it != g_instances.end()) {
                    inst = it->second.instance;
                    gipa = it->second.gipa;
                }
            }
            auto q = gipa ? (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(
                                inst, "vkGetPhysicalDeviceSurfacePresentModesKHR")
                          : nullptr;
            uint32_t n = 0;
            if (q && q(d->phys, ci->surface, &n, nullptr) == VK_SUCCESS && n) {
                std::vector<VkPresentModeKHR> modes(n);
                if (q(d->phys, ci->surface, &n, modes.data()) == VK_SUCCESS)
                    supported = std::find(modes.begin(), modes.end(), want) !=
                                modes.end();
            }
        }
        if (supported) {
            sbs_ci.presentMode = want;
            X4VR_LOG("perf: present mode forced %d -> %d (measurement run; "
                     "frame rate is no longer display-locked)",
                     (int)ci->presentMode, (int)want);
        } else {
            X4VR_LOG("perf: present mode %d not supported by this surface — "
                     "staying on %d, and any perf number from this run is "
                     "capped by the display",
                     (int)want, (int)ci->presentMode);
        }
    }
    bool sbs_usage = false;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (g_sbs_enabled && g_active) {
        sbs_ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        r = d->CreateSwapchainKHR(device, &sbs_ci, ac, out);
        sbs_usage = r == VK_SUCCESS;
        if (!sbs_usage)
            X4VR_LOG("sbs: swapchain rejected transfer usage (%d) — retrying "
                     "with X4's own flags; composite off",
                     (int)r);
    }
    if (!sbs_usage) {
        VkSwapchainCreateInfoKHR plain = sbs_ci;
        plain.imageUsage = ci->imageUsage;
        r = d->CreateSwapchainKHR(device, &plain, ac, out);
    }
    // The definitive record of what resolution the game is actually running
    // at (Phase 1 verification: should be the SBS size we forced).
    X4VR_LOG("swapchain created: %ux%u images>=%u format=%d presentMode=%d -> %s",
             ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount,
             (int)ci->imageFormat, (int)ci->presentMode,
             r == VK_SUCCESS ? "ok" : "FAILED");
    // The injector forces res_width/res_height, but X4 only honours them when
    // borderless is off; with borderless on it sizes to the display and
    // ignores them (observed: identical config gave 2816x1408 under a
    // 2816x1408 gamescope and 3440x1440 on a 3440x1440 desktop). So the
    // config alone does not guarantee the size, and the SBS split needs an
    // exact 2:1 -- an odd size here silently halves into two wrong eyes.
    // Say so loudly, once.
    if (r == VK_SUCCESS) {
        const uint32_t w = ci->imageExtent.width, h = ci->imageExtent.height;
        // X4VR_RES (set by the launcher's one-eye mode) is the authority on
        // what size we asked for; without it the SBS frame is the target.
        // Warning against the wrong number is worse than not warning.
        uint32_t want_w = X4VR_SBS_WIDTH, want_h = X4VR_SBS_HEIGHT;
        if (const char *res = getenv("X4VR_RES")) {
            unsigned rw = 0, rh = 0;
            if (sscanf(res, "%ux%u", &rw, &rh) == 2 && rw && rh) {
                want_w = rw;
                want_h = rh;
            }
        }
        static uint32_t warned_w = 0, warned_h = 0;
        if ((w != want_w || h != want_h) &&
            (w != warned_w || h != warned_h)) {
            warned_w = w;
            warned_h = h;
            X4VR_LOG("WARNING swapchain is %ux%u, expected %dx%d -- X4 sized "
                     "to the display, not to res_width/res_height (borderless "
                     "does that). Run under gamescope at %dx%d; the SBS split "
                     "will be wrong otherwise.",
                     w, h, X4VR_SBS_WIDTH, X4VR_SBS_HEIGHT, X4VR_SBS_WIDTH,
                     X4VR_SBS_HEIGHT);
        }
            if (sbs_usage)
            g_sbs.add_swapchain(*out, sbs_ci, split);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    g_sbs.remove_swapchain(sc); // waits for the device before freeing
    d->DestroySwapchainKHR(device, sc, ac);
}

// X4 sizes its whole pipeline from the surface's currentExtent -- that is
// exactly why it ignores res_width/res_height while borderless. Reporting
// half the width therefore makes it render one eye's worth *natively*:
// G-buffer, luminance pyramid, bloom, the AO compute dispatches and
// V_viewportpixelsize all come out consistent at 1408x1408, with nothing
// wasted and no per-pass viewport surgery. The layer keeps the real
// full-width swapchain and copies the eye into both halves at present.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_GetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice phys, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR next;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(phys));
        if (it == g_instances.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        next = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)it->second.gipa(
            it->second.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    }
    if (!next)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = next(phys, surface, caps);
    if (r != VK_SUCCESS || !g_sbs_enabled || !g_active || !g_sbs_split_render)
        return r;

    // 0xFFFFFFFF means "the surface has no preferred size" -- the app picks
    // its own extent and never consults this, so there is nothing to halve
    // and, crucially, nothing to double back at swapchain creation. Halving
    // and doubling must be one decision, recorded per surface.
    const uint32_t was = caps->currentExtent.width;
    static bool first = true;
    if (first) {
        first = false;
        X4VR_LOG("sbs: surface caps currentExtent=%ux%u min=%ux%u max=%ux%u",
                 caps->currentExtent.width, caps->currentExtent.height,
                 caps->minImageExtent.width, caps->minImageExtent.height,
                 caps->maxImageExtent.width, caps->maxImageExtent.height);
    }
    if (was == 0xFFFFFFFFu || was < 2) {
        forget_halved_surface(surface);
        static bool told = false;
        if (!told) {
            told = true;
            X4VR_LOG("sbs: surface reports no preferred extent "
                     "(currentExtent=0x%X) — this is the Wayland WSI. The "
                     "config's res_width still brings X4 to the eye size, but "
                     "on Wayland the buffer *is* the surface: presenting the "
                     "full-width image resizes the window, X4 follows, and "
                     "the split collapses to duplicating the left half. Force "
                     "the X11 driver — X4 links SDL3, so the variable is "
                     "SDL_VIDEO_DRIVER=x11 (SDL2 spells it SDL_VIDEODRIVER).",
                     was);
        }
        return r;
    }
    caps->currentExtent.width = was / 2;
    if (caps->minImageExtent.width >= 2)
        caps->minImageExtent.width /= 2;
    if (caps->maxImageExtent.width >= 2)
        caps->maxImageExtent.width /= 2;
    if (note_halved_surface(surface))
        X4VR_LOG("sbs: reporting surface width %u instead of %u — X4 renders "
                 "one eye at %ux%u",
                 caps->currentExtent.width, was, caps->currentExtent.width,
                 caps->currentExtent.height);
    return r;
}

// X4 must see the images it will actually render into, which when the split
// render is active are ours, not the swapchain's.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR sc, uint32_t *count, VkImage *images) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    const std::vector<VkImage> *eyes =
        (g_sbs_enabled && g_active) ? g_sbs.eye_images(sc) : nullptr;
    if (!eyes)
        return d->GetSwapchainImagesKHR(device, sc, count, images);
    if (!images) {
        *count = (uint32_t)eyes->size();
        return VK_SUCCESS;
    }
    const uint32_t n =
        *count < eyes->size() ? *count : (uint32_t)eyes->size();
    for (uint32_t i = 0; i < n; i++)
        images[i] = (*eyes)[i];
    *count = n;
    return n < eyes->size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL x4vr_GetDeviceQueue(VkDevice device, uint32_t family,
                                               uint32_t index, VkQueue *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    d->GetDeviceQueue(device, family, index, out);
    if (out && *out) {
        std::lock_guard<std::mutex> lock(g_queue_mu);
        g_queue_family[*out] = family;
    }
}

VKAPI_ATTR void VKAPI_CALL x4vr_GetDeviceQueue2(VkDevice device,
                                                const VkDeviceQueueInfo2 *qi,
                                                VkQueue *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    d->GetDeviceQueue2(device, qi, out);
    if (out && *out && qi) {
        std::lock_guard<std::mutex> lock(g_queue_mu);
        g_queue_family[*out] = qi->queueFamilyIndex;
    }
}


// Predicted failure mode 4, and the one that bit: X4 copies between its render
// targets, and every per-eye image carries TRANSFER_SRC|TRANSFER_DST. A copy
// region names layerCount=1, so it moves layer 0 and leaves layer 1 holding
// whatever it held before. One such copy anywhere in the chain drains the
// second view, and the frame goes black exactly as observed -- with no
// validation error, because copying one layer of a two-layer image is
// perfectly legal.
//
// Multiview replicates *draws*, never transfers. Anything outside a render
// pass has to be widened by hand.
bool img_doubled(VkImage im) {
    std::lock_guard<std::mutex> lock(g_img_mu);
    auto it = g_images.find(im);
    return it != g_images.end() && it->second.doubled;
}

const bool g_mv_fix_copies = [] {
    const char *e = getenv("X4VR_MV_FIX_COPIES");
    return !(e && *e && *e == '0');
}();

// Widen a one-layer region to both layers. Only when it starts at layer 0 and
// covers exactly one: anything else is a deliberate per-layer access and must
// be left alone.
bool widen(VkImageSubresourceLayers &a, VkImageSubresourceLayers &b) {
    if (a.baseArrayLayer != 0 || a.layerCount != 1 ||
        b.baseArrayLayer != 0 || b.layerCount != 1)
        return false;
    a.layerCount = 2;
    b.layerCount = 2;
    return true;
}

void count_widened(uint32_t n) {
    if (!n)
        return;
    std::lock_guard<std::mutex> lock(g_mv_mu);
    g_mv_stats.widened += n;
}

// A write that lands in layer 0 alone, on an image a view-masked pass also
// renders into. Named by image serial for the first few, because "something
// writes only layer 0" is not actionable and "image #37 does, via
// vkCmdCopyBufferToImage" is.
//
// Only per-eye images count. A doubled image nothing reads through layer 1 is
// free to have a stale second layer -- that is the whole reason doubling is
// allowed to be permissive at vkCreateImage.
void note_layer0_only(VkImage im, const char *what) {
    if (!g_mv || !g_active)
        return;
    uint32_t serial;
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        if (!g_per_eye_images.count(im))
            return;
        auto it = g_images.find(im);
        serial = it != g_images.end() ? it->second.serial : UINT32_MAX;
    }
    std::lock_guard<std::mutex> lock(g_mv_mu);
    if (++g_mv_stats.layer0_only <= 12)
        X4VR_LOG("mv: img #%u written to layer 0 only by %s — a masked pass "
                 "also renders it, so layer 1 is now stale",
                 serial, what);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdCopyImage(
    VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
    VkImageLayout dl, uint32_t n, const VkImageCopy *regions) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageCopy> mod;
    uint32_t w = 0;
    if (g_mv && g_mv_fix_copies && g_active && img_doubled(src) &&
        img_doubled(dst)) {
        mod.assign(regions, regions + n);
        for (auto &r : mod)
            w += widen(r.srcSubresource, r.dstSubresource) ? 1 : 0;
    }
    count_widened(w);
    if (w < n)
        note_layer0_only(dst, "vkCmdCopyImage");
    d->CmdCopyImage(cb, src, sl, dst, dl, n, w ? mod.data() : regions);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdBlitImage(
    VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
    VkImageLayout dl, uint32_t n, const VkImageBlit *regions,
    VkFilter filter) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageBlit> mod;
    uint32_t w = 0;
    if (g_mv && g_mv_fix_copies && g_active && img_doubled(src) &&
        img_doubled(dst)) {
        mod.assign(regions, regions + n);
        for (auto &r : mod)
            w += widen(r.srcSubresource, r.dstSubresource) ? 1 : 0;
    }
    count_widened(w);
    if (w < n)
        note_layer0_only(dst, "vkCmdBlitImage");
    d->CmdBlitImage(cb, src, sl, dst, dl, n, w ? mod.data() : regions, filter);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdResolveImage(
    VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
    VkImageLayout dl, uint32_t n, const VkImageResolve *regions) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageResolve> mod;
    uint32_t w = 0;
    if (g_mv && g_mv_fix_copies && g_active && img_doubled(src) &&
        img_doubled(dst)) {
        mod.assign(regions, regions + n);
        for (auto &r : mod)
            w += widen(r.srcSubresource, r.dstSubresource) ? 1 : 0;
    }
    count_widened(w);
    if (w < n)
        note_layer0_only(dst, "vkCmdResolveImage");
    d->CmdResolveImage(cb, src, sl, dst, dl, n, w ? mod.data() : regions);
}

// A clear outside a render pass has the same problem from the other side:
// clearing only layer 0 leaves layer 1 stale rather than blank.
VKAPI_ATTR void VKAPI_CALL x4vr_CmdClearColorImage(
    VkCommandBuffer cb, VkImage img, VkImageLayout l,
    const VkClearColorValue *colour, uint32_t n,
    const VkImageSubresourceRange *ranges) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageSubresourceRange> mod;
    uint32_t w = 0;
    if (g_mv && g_mv_fix_copies && g_active && img_doubled(img)) {
        mod.assign(ranges, ranges + n);
        for (auto &r : mod)
            if (r.baseArrayLayer == 0 && r.layerCount == 1) {
                r.layerCount = 2;
                w++;
            }
    }
    count_widened(w);
    if (w < n)
        note_layer0_only(img, "vkCmdClearColorImage");
    d->CmdClearColorImage(cb, img, l, colour, n, w ? mod.data() : ranges);
}

// The paths the transfer fix missed. Two kinds, and they need opposite
// treatment:
//
//   image -> image, in the Vulkan 1.3 spelling. Same shape as the v1 forms
//     above and the same widening applies. If X4 uses these at all, not
//     hooking them left a hole exactly as wide as the one we closed.
//
//   buffer -> image. This one CANNOT be widened: the source holds one
//     layer's worth of data and asking for two would read past its end.
//     Counted instead, so the image shows up by name.
template <class Info, class Region>
uint32_t widen2(const Info *info, std::vector<Region> &mod) {
    if (!(g_mv && g_mv_fix_copies && g_active && img_doubled(info->srcImage) &&
          img_doubled(info->dstImage)))
        return 0;
    mod.assign(info->pRegions, info->pRegions + info->regionCount);
    uint32_t w = 0;
    for (auto &r : mod)
        w += widen(r.srcSubresource, r.dstSubresource) ? 1 : 0;
    return w;
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdCopyImage2(VkCommandBuffer cb,
                                              const VkCopyImageInfo2 *info) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageCopy2> mod;
    const uint32_t w = widen2(info, mod);
    count_widened(w);
    if (w < info->regionCount)
        note_layer0_only(info->dstImage, "vkCmdCopyImage2");
    VkCopyImageInfo2 m = *info;
    if (w)
        m.pRegions = mod.data();
    d->CmdCopyImage2(cb, w ? &m : info);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdBlitImage2(VkCommandBuffer cb,
                                              const VkBlitImageInfo2 *info) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageBlit2> mod;
    const uint32_t w = widen2(info, mod);
    count_widened(w);
    if (w < info->regionCount)
        note_layer0_only(info->dstImage, "vkCmdBlitImage2");
    VkBlitImageInfo2 m = *info;
    if (w)
        m.pRegions = mod.data();
    d->CmdBlitImage2(cb, w ? &m : info);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdResolveImage2(
    VkCommandBuffer cb, const VkResolveImageInfo2 *info) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageResolve2> mod;
    const uint32_t w = widen2(info, mod);
    count_widened(w);
    if (w < info->regionCount)
        note_layer0_only(info->dstImage, "vkCmdResolveImage2");
    VkResolveImageInfo2 m = *info;
    if (w)
        m.pRegions = mod.data();
    d->CmdResolveImage2(cb, w ? &m : info);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdCopyBufferToImage(
    VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout l, uint32_t n,
    const VkBufferImageCopy *regions) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    note_layer0_only(dst, "vkCmdCopyBufferToImage");
    d->CmdCopyBufferToImage(cb, src, dst, l, n, regions);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdCopyBufferToImage2(
    VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    note_layer0_only(info->dstImage, "vkCmdCopyBufferToImage2");
    d->CmdCopyBufferToImage2(cb, info);
}

// Depth is the one attachment the whole deferred chain reconstructs position
// from, so a depth clear that misses layer 1 blacks the scene by itself.
VKAPI_ATTR void VKAPI_CALL x4vr_CmdClearDepthStencilImage(
    VkCommandBuffer cb, VkImage img, VkImageLayout l,
    const VkClearDepthStencilValue *value, uint32_t n,
    const VkImageSubresourceRange *ranges) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    std::vector<VkImageSubresourceRange> mod;
    uint32_t w = 0;
    if (g_mv && g_mv_fix_copies && g_active && img_doubled(img)) {
        mod.assign(ranges, ranges + n);
        for (auto &r : mod)
            if (r.baseArrayLayer == 0 && r.layerCount == 1) {
                r.layerCount = 2;
                w++;
            }
    }
    count_widened(w);
    if (w < n)
        note_layer0_only(img, "vkCmdClearDepthStencilImage");
    d->CmdClearDepthStencilImage(cb, img, l, value, n,
                                 w ? mod.data() : ranges);
}

// --- take four: measure the two remaining ways layer 1 can stay dark -------
//
// Neither hook changes anything. They exist because take three spent a live
// run on a hypothesis that turned out to be true but not sufficient
// (transfers_widened=14554, still black), and the next guess should cost a
// counter rather than another run.

void cb_enter_pass(VkCommandBuffer cb, VkRenderPass rp) {
    if (!g_mv || !g_active)
        return;
    bool masked;
    {
        std::lock_guard<std::mutex> lock(g_variants.mu);
        masked = g_masked_passes.count(rp) != 0;
    }
    std::lock_guard<std::mutex> lock(g_cb_mu);
    g_cb_mask[cb] = masked;
}

void cb_leave_pass(VkCommandBuffer cb) {
    if (!g_mv || !g_active)
        return;
    std::lock_guard<std::mutex> lock(g_cb_mu);
    g_cb_mask.erase(cb);
}

// ---- the layer-1 readback ------------------------------------------------
//
// Everything the diagnosis now rests on is one unmeasured fact: whether layer
// 0 and layer 1 of a per-eye image hold the same bytes in the *game*. The
// offline suite says they do in miniature. Eight live runs inferred it from
// what appeared on screen, which is exactly the kind of inference that has
// been wrong three times here.
//
// So read both layers and hash them. The copy rides X4's own command buffer
// immediately after a masked pass ends, which is the one instant the layout
// is known rather than guessed -- the render pass performed the transition to
// finalLayout itself, so it cannot be wrong about it. Nothing here owns a
// queue, allocates a command buffer, or reorders the frame.
//
// One image per frame, cycling, so a session covers the whole per-eye set
// rather than whichever image happens to be first.
const bool g_mv_probe = [] {
    const char *e = getenv("X4VR_MV_PROBE");
    return e && *e && *e != '0';
}();

struct MvProbe {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *ptr = nullptr;
    VkDeviceSize half = 0; // bytes reserved per layer; grows as needed
    bool failed = false;
    bool pending = false; // a copy is recorded and not yet read
    bool armed = true;
    uint64_t presents = 0;
    uint32_t stalled = 0;
    std::unordered_set<uint32_t> done; // serials covered this round
    uint32_t serial = 0;               // the one being probed
    VkDeviceSize bytes = 0;
    uint32_t w = 0, h = 0, bpp = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t captures = 0;
} g_probe;
std::mutex g_probe_mu;

// A full frame is copied, not a corner.
//
// The first version hashed a 64x64 patch at the origin and reported
// IDENTICAL for 4759 of 5994 captures with both sides all zero -- in X4 that
// corner is blank most frames, so the comparison was between two empty
// regions and meant nothing. An instrument that agrees with itself on absent
// data is the same failure as one that only ever says "identical".
//
// One capture every this many presents, since a full-extent copy of a
// 1408x1408 RGBA16F target is ~16 MB per layer.
constexpr uint64_t kProbeEvery = 30;

// Only the colour formats X4 uses as attachments. Anything absent is skipped
// rather than guessed at: a wrong size here would hash the wrong bytes and
// produce a confident, meaningless answer.
uint32_t format_bpp(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT:            return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SFLOAT:                                  return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32_SFLOAT:      return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:                         return 8;
    default:                                                    return 0;
    }
}

// Dump both layers to disk on the first capture that differs.
//
// Twelve runs have been spent narrowing what layer 1 contains by counting
// bytes. Writing the two images out and looking at them answers in one glance
// what "22% of texels differ" can only circle: whether layer 1 is the same
// scene lit differently, a subset of the passes, or something unrelated.
const char *g_mv_dump = getenv("X4VR_MV_DUMP");

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1;
    uint32_t e = (h >> 10) & 0x1f, m = h & 0x3ff, f;
    if (e == 0) {
        if (!m) {
            f = s << 31;
        } else { // subnormal: normalise it by hand
            e = 127 - 15 + 1;
            while (!(m & 0x400)) {
                m <<= 1;
                e--;
            }
            m &= 0x3ff;
            f = (s << 31) | (e << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = (s << 31) | (0xffu << 23) | (m << 13);
    } else {
        f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    }
    float out;
    memcpy(&out, &f, sizeof out);
    return out;
}

// Reinhard then gamma, so the whole HDR range shows structure rather than
// clipping to white wherever the sun is -- which is exactly the region under
// suspicion.
void write_ppm(const char *path, const uint16_t *px, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint32_t c = 0; c < 3; c++) {
                float v = half_to_float(px[((size_t)y * w + x) * 4 + c]);
                if (!(v > 0.0f))
                    v = 0.0f;
                v = v / (1.0f + v);
                row[(size_t)x * 3 + c] =
                    (uint8_t)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f);
            }
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

uint64_t fnv1a(const void *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= ((const uint8_t *)p)[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Grown to fit whatever image is being probed, since the whole of mip 0 is
// copied. Offsets are 256-aligned so the second layer's bufferOffset stays
// legal for every format.
bool probe_buffer_ready(DeviceData *d, VkDevice dev, VkDeviceSize need) {
    need = (need + 255) & ~(VkDeviceSize)255;
    if (g_probe.buf != VK_NULL_HANDLE && g_probe.half >= need)
        return true;
    if (g_probe.failed || !d->AllocateMemory || !d->CmdCopyImageToBuffer)
        return false;
    if (g_probe.buf != VK_NULL_HANDLE) {
        // Only ever called between captures, so nothing is in flight.
        if (g_probe.ptr)
            d->UnmapMemory(dev, g_probe.mem);
        d->DestroyBuffer(dev, g_probe.buf, nullptr);
        d->FreeMemory(dev, g_probe.mem, nullptr);
        g_probe.buf = VK_NULL_HANDLE;
        g_probe.mem = VK_NULL_HANDLE;
        g_probe.ptr = nullptr;
    }
    g_probe.half = need;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = need * 2;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d->CreateBuffer(dev, &bci, nullptr, &g_probe.buf) != VK_SUCCESS) {
        g_probe.failed = true;
        return false;
    }
    VkMemoryRequirements req{};
    d->GetBufferMemoryRequirements(dev, g_probe.buf, &req);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < d->memprops.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (d->memprops.memoryTypes[i].propertyFlags & want) == want) {
            type = i;
            break;
        }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX ||
        d->AllocateMemory(dev, &mai, nullptr, &g_probe.mem) != VK_SUCCESS ||
        d->BindBufferMemory(dev, g_probe.buf, g_probe.mem, 0) != VK_SUCCESS ||
        d->MapMemory(dev, g_probe.mem, 0, VK_WHOLE_SIZE, 0, &g_probe.ptr) !=
            VK_SUCCESS) {
        X4VR_LOG("mv probe: could not create the readback buffer — probe off");
        g_probe.failed = true;
        return false;
    }
    return true;
}

// Emitted straight after vkCmdEndRenderPass, into the same command buffer.
void probe_emit(DeviceData *d, VkCommandBuffer cb, const FbAtt &a) {
    const uint32_t bpp = format_bpp(a.format);
    if (!bpp)
        return;
    const uint32_t w = a.extent.width, h = a.extent.height;
    if (!w || !h)
        return;
    if (!probe_buffer_ready(d, d->device, (VkDeviceSize)w * h * bpp))
        return;

    VkImageMemoryBarrier to{};
    to.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to.oldLayout = a.final_layout;
    to.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to.image = a.image;
    to.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &to);

    VkBufferImageCopy regions[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        regions[i].bufferOffset = g_probe.half * i;
        regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[i].imageSubresource.mipLevel = 0;
        regions[i].imageSubresource.baseArrayLayer = i;
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageExtent = {w, h, 1};
    }
    d->CmdCopyImageToBuffer(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            g_probe.buf, 2, regions);

    // Put it back exactly as the pass left it, or the next command X4 records
    // is operating on an image in a layout it never asked for.
    VkImageMemoryBarrier back = to;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = a.final_layout;
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &back);

    g_probe.serial = a.serial;
    g_probe.bytes = (VkDeviceSize)w * h * bpp;
    g_probe.w = w;
    g_probe.h = h;
    g_probe.bpp = bpp;
    g_probe.format = a.format;
    g_probe.pending = true;
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdBeginRenderPass(
    VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
    VkSubpassContents contents) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    cb_enter_pass(cb, bi->renderPass);
    if (g_mv_probe && g_mv && g_active) {
        std::lock_guard<std::mutex> lock(g_cb_mu);
        g_cb_fb[cb] = bi->framebuffer;
    }
    d->CmdBeginRenderPass(cb, bi, contents);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdEndRenderPass(VkCommandBuffer cb) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    d->CmdEndRenderPass(cb);

    if (g_mv_probe && g_mv && g_active) {
        std::vector<FbAtt> atts;
        bool masked = false;
        {
            std::lock_guard<std::mutex> lock(g_cb_mu);
            auto m = g_cb_mask.find(cb);
            masked = m != g_cb_mask.end() && m->second;
            auto f = g_cb_fb.find(cb);
            if (masked && f != g_cb_fb.end()) {
                auto a = g_fb_atts.find(f->second);
                if (a != g_fb_atts.end())
                    atts = a->second;
            }
        }
        if (!atts.empty()) {
            std::lock_guard<std::mutex> lock(g_probe_mu);
            // Take the first attachment this round has not covered yet, and
            // start a fresh round once every one has been. Indexing into the
            // framebuffer's own list instead reached only 7 of ~20 per-eye
            // images, because which framebuffers end a frame is not something
            // a counter can enumerate.
            if (!g_probe.pending && g_probe.armed) {
                for (const FbAtt &a : atts) {
                    if (g_probe.done.count(a.serial))
                        continue;
                    probe_emit(d, cb, a);
                    if (g_probe.pending) {
                        g_probe.done.insert(a.serial);
                        g_probe.armed = false;
                    }
                    break;
                }
            }
        }
    }
    cb_leave_pass(cb);
}

// Read what the frame just wrote. Present is the safe moment: the work is
// submitted, and idling the queue here costs a diagnostic run nothing.
// queue == VK_NULL_HANDLE means the caller has already guaranteed the work is
// finished -- vkDestroyDevice requires an idle device. That path exists so the
// offline suite, which never presents, can still exercise this code: an
// instrument nothing tests is how the last three misreadings happened.
void probe_collect(DeviceData *d, VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_probe_mu);
    if (queue != VK_NULL_HANDLE) {
        g_probe.presents++;
        if (!g_probe.armed && g_probe.presents % kProbeEvery == 0)
            g_probe.armed = true;
        // Armed but never satisfied means the round has covered everything
        // that actually appears; start the next sweep.
        if (g_probe.armed && !g_probe.pending) {
            if (++g_probe.stalled > 4 * kProbeEvery) {
                g_probe.done.clear();
                g_probe.stalled = 0;
            }
        } else {
            g_probe.stalled = 0;
        }
    }
    if (!g_probe.pending)
        return;
    g_probe.pending = false;
    if (queue != VK_NULL_HANDLE &&
        (!d->QueueWaitIdle || d->QueueWaitIdle(queue) != VK_SUCCESS))
        return;
    const uint8_t *l0 = (const uint8_t *)g_probe.ptr;
    const uint8_t *l1 = l0 + g_probe.half;
    const size_t n = (size_t)g_probe.bytes;
    const uint64_t h0 = fnv1a(l0, n), h1 = fnv1a(l1, n);
    bool z0 = true, z1 = true;
    // "Differ" alone does not say whether one texel moved or the whole frame
    // is unrelated, and those need entirely different explanations. Count the
    // differing texels and locate the first, so the next question is about a
    // region of the screen rather than about the whole image.
    size_t dtex = 0, first = SIZE_MAX;
    const uint32_t bpp = g_probe.bpp ? g_probe.bpp : 1;
    for (size_t i = 0; i < n; i++) {
        if (l0[i]) z0 = false;
        if (l1[i]) z1 = false;
    }
    // How many texels carry anything at all in layer 0. A space scene is
    // mostly empty, so "27% of texels differ" means one thing if 27% of the
    // frame has content and something quite different if 90% does.
    // The differing set turned out to be exactly the set where layer 0 has
    // content, which leaves two very different readings still open:
    //
    //   missing — layer 1 is empty where layer 0 has something. Draws are not
    //     reaching view 1, and the question is which passes.
    //   changed — both layers have content there and the values differ. The
    //     draws did reach view 1 and something is being applied per view.
    //
    // One counter each settles it. Guessing between them is what the last
    // several runs did.
    size_t nz0 = 0, nz1 = 0, missing = 0, changed = 0, extra = 0;
    static const uint8_t zero[16] = {0};
    for (size_t t = 0; t * bpp < n; t++) {
        const bool a = memcmp(l0 + t * bpp, zero, bpp) != 0;
        const bool b = memcmp(l1 + t * bpp, zero, bpp) != 0;
        if (a)
            nz0++;
        if (b)
            nz1++;
        if (memcmp(l0 + t * bpp, l1 + t * bpp, bpp) != 0) {
            if (first == SIZE_MAX)
                first = t;
            dtex++;
            if (a && !b)
                missing++;
            else if (a && b)
                changed++;
            else
                extra++;
        }
    }
    const size_t total = n / bpp;
    if (h0 == h1) {
        X4VR_LOG("mv probe: img #%u %ux%u  layer0=%016llx%s  "
                 "layer1=%016llx%s  IDENTICAL",
                 g_probe.serial, g_probe.w, g_probe.h, (unsigned long long)h0,
                 z0 ? " (all zero)" : "", (unsigned long long)h1,
                 z1 ? " (all zero)" : "");
    } else {
        X4VR_LOG("mv probe: img #%u %ux%u  layer0=%016llx%s  "
                 "layer1=%016llx%s  DIFFER %zu/%zu (%.2f%%)  "
                 "non-empty %zu/%zu  missing=%zu changed=%zu extra=%zu  "
                 "first at (%u,%u)",
                 g_probe.serial, g_probe.w, g_probe.h, (unsigned long long)h0,
                 z0 ? " (all zero)" : "", (unsigned long long)h1,
                 z1 ? " (all zero)" : "", dtex, total,
                 total ? 100.0 * (double)dtex / (double)total : 0.0, nz0, nz1,
                 missing, changed, extra,
                 g_probe.w ? (uint32_t)(first % g_probe.w) : 0,
                 g_probe.w ? (uint32_t)(first / g_probe.w) : 0);
        static bool dumped = false;
        if (g_mv_dump && !dumped &&
            g_probe.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
            dumped = true;
            char p[512];
            snprintf(p, sizeof p, "%s-img%u-layer0.ppm", g_mv_dump,
                     g_probe.serial);
            write_ppm(p, (const uint16_t *)l0, g_probe.w, g_probe.h);
            snprintf(p, sizeof p, "%s-img%u-layer1.ppm", g_mv_dump,
                     g_probe.serial);
            write_ppm(p, (const uint16_t *)l1, g_probe.w, g_probe.h);
            X4VR_LOG("mv probe: wrote %s-img%u-layer{0,1}.ppm", g_mv_dump,
                     g_probe.serial);
        }
    }
    g_probe.captures++;
}

// A secondary command buffer is recorded outside any vkCmdBeginRenderPass but
// still executes inside one, named here. Without this its draws would look
// like they belong to no pass and the mismatch would go uncounted.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_BeginCommandBuffer(
    VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    if (bi->pInheritanceInfo &&
        (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) &&
        bi->pInheritanceInfo->renderPass != VK_NULL_HANDLE)
        cb_enter_pass(cb, bi->pInheritanceInfo->renderPass);
    else
        cb_leave_pass(cb);
    return d->BeginCommandBuffer(cb, bi);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdBindPipeline(VkCommandBuffer cb,
                                                VkPipelineBindPoint bp,
                                                VkPipeline p) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    if (g_mv && g_active && bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        bool in_masked = false, pipe_mv = false, known = false;
        {
            std::lock_guard<std::mutex> lock(g_cb_mu);
            auto c = g_cb_mask.find(cb);
            in_masked = c != g_cb_mask.end() && c->second;
            auto q = g_pipe_mv.find(p);
            known = q != g_pipe_mv.end();
            pipe_mv = known && q->second;
        }
        if (in_masked && known) {
            std::lock_guard<std::mutex> lock(g_mv_mu);
            if (pipe_mv)
                g_mv_stats.bind_ok++;
            else if (++g_mv_stats.bind_mismatch <= 3)
                X4VR_LOG("mv: pipeline built against an unmasked pass is bound "
                         "inside a masked one — its draws replicate to one "
                         "view");
        }
    }
    d->CmdBindPipeline(cb, bp, p);
}

// Same family as the transfers. The render pass transitions both layers,
// because its attachment is the two-layer view we substituted; a barrier
// between passes names layerCount=1 and transitions layer 0 alone. Counted
// only -- widening a barrier changes what the driver may do to the image, and
// that is a behaviour change, not a measurement.
VKAPI_ATTR void VKAPI_CALL x4vr_CmdPipelineBarrier(
    VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t mbc, const VkMemoryBarrier *mb,
    uint32_t bbc, const VkBufferMemoryBarrier *bb, uint32_t ibc,
    const VkImageMemoryBarrier *ib) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    if (g_mv && g_active && ibc) {
        uint32_t narrow = 0, wide = 0;
        for (uint32_t i = 0; i < ibc; i++) {
            if (!img_doubled(ib[i].image))
                continue;
            const VkImageSubresourceRange &r = ib[i].subresourceRange;
            if (r.baseArrayLayer == 0 &&
                (r.layerCount == VK_REMAINING_ARRAY_LAYERS || r.layerCount >= 2))
                wide++;
            else
                narrow++;
        }
        if (narrow || wide) {
            std::lock_guard<std::mutex> lock(g_mv_mu);
            g_mv_stats.barrier_narrow += narrow;
            g_mv_stats.barrier_wide += wide;
        }
    }
    d->CmdPipelineBarrier(cb, src, dst, flags, mbc, mb, bbc, bb, ibc, ib);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pi) {
    if (g_mv && g_active) {
        static bool once = false;
        if (!once) {
            once = true;
            mv_report("first present");
        }
    }
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(queue));
    }
    if (g_mv_probe && g_mv && g_active)
        probe_collect(d, queue);
    // One swapchain is all X4 presents; anything else is not a case we have
    // seen, so leave it alone rather than guess which image is the eye pair.
    VkSemaphore composited = VK_NULL_HANDLE;
    uint32_t family = 0;
    if (g_sbs_enabled && g_active && pi->swapchainCount == 1) {
        if (queue_family_of(queue, family)) {
            composited = g_sbs.composite(queue, family, pi->pSwapchains[0],
                                         pi->pImageIndices[0],
                                         pi->pWaitSemaphores,
                                         pi->waitSemaphoreCount);
        } else {
            static bool warned = false;
            if (!warned) {
                warned = true;
                X4VR_LOG("sbs: present queue was never seen through "
                         "vkGetDeviceQueue — composite off (would risk "
                         "submitting to the wrong queue family)");
            }
        }
    }

    VkResult r;
    if (composited != VK_NULL_HANDLE) {
        // Our submit already waits on X4's semaphores, so the present now
        // waits only on ours -- waiting on both would deadlock, since a
        // binary semaphore can only be consumed once.
        VkPresentInfoKHR sbs_pi = *pi;
        sbs_pi.waitSemaphoreCount = 1;
        sbs_pi.pWaitSemaphores = &composited;
        r = d->QueuePresentKHR(queue, &sbs_pi);
    } else {
        r = d->QueuePresentKHR(queue, pi);
    }
    frame_flush();
    return r;
}

// -------------------------------------------------- instance/device bring-up

VkLayerInstanceCreateInfo *find_instance_link(const VkInstanceCreateInfo *ci) {
    auto *item = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (item &&
           !(item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
             item->function == VK_LAYER_LINK_INFO))
        item = (VkLayerInstanceCreateInfo *)item->pNext;
    return item;
}

VkLayerDeviceCreateInfo *find_device_link(const VkDeviceCreateInfo *ci) {
    auto *item = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (item &&
           !(item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
             item->function == VK_LAYER_LINK_INFO))
        item = (VkLayerDeviceCreateInfo *)item->pNext;
    return item;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *ac,
    VkInstance *out) {
    VkLayerInstanceCreateInfo *link = find_instance_link(ci);
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext; // advance the chain

    auto next_create =
        (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkResult r = next_create(ci, ac, out);
    if (r != VK_SUCCESS)
        return r;

    InstanceData data;
    data.instance = *out;
    data.gipa = gipa;
    data.DestroyInstance =
        (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    data.EnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(
            *out, "vkEnumerateDeviceExtensionProperties");
    // Core since 1.1, KHR alias before that. Which name resolves depends on
    // the instance's apiVersion, so try both rather than assuming.
    data.GetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(
            *out, "vkGetPhysicalDeviceProperties2");
    if (!data.GetPhysicalDeviceProperties2)
        data.GetPhysicalDeviceProperties2 =
            (PFN_vkGetPhysicalDeviceProperties2)gipa(
                *out, "vkGetPhysicalDeviceProperties2KHR");
    data.GetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(
            *out, "vkGetPhysicalDeviceFeatures2");
    if (!data.GetPhysicalDeviceFeatures2)
        data.GetPhysicalDeviceFeatures2 =
            (PFN_vkGetPhysicalDeviceFeatures2)gipa(
                *out, "vkGetPhysicalDeviceFeatures2KHR");
    data.app_api_version =
        ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_instances[dispatch_key(*out)] = data;
    }
    const char *app = ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName
                          ? ci->pApplicationInfo->pApplicationName
                          : nullptr;
    g_active = app_is_target(app);
    X4VR_LOG("instance created (app=%s)%s", app ? app : "?",
             g_active ? "" : " — not the game, layer inert in this process");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *ac) {
    PFN_vkDestroyInstance next;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        next = g_instances.at(dispatch_key(instance)).DestroyInstance;
        g_instances.erase(dispatch_key(instance));
    }
    next(instance, ac);
}

// Phase 4b groundwork: report what multiview this device can do, and whether
// X4 already switched it on.
//
// Multiview is how the second eye is meant to arrive: X4 keeps rendering one
// eye's worth into what it believes is a normal image, the attachments become
// 2-layer arrays behind its back, and the vertex shader picks its K from
// gl_ViewIndex. That keeps the render size equal to the window size, which is
// the one arrangement observed to work end to end (tag one_eye_baseline) --
// see docs/x4-quirks.md on X4 laying its UI out from the *window* size.
//
// Purely diagnostic for now. It answers, before any SPIR-V work is committed,
// three questions the plan rests on: is multiview supported at all, are two
// views within maxMultiviewViewCount, and does X4 enable the feature itself
// (if not, we must add it to the device's pNext chain, which means editing a
// const struct X4 owns -- worth knowing early).
void probe_multiview(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci) {
    InstanceData inst;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(phys));
        if (it == g_instances.end())
            return;
        inst = it->second;
    }

    // Did X4 ask for it? Either as a 1.0-era extension...
    bool ext_requested = false;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (strcmp(ci->ppEnabledExtensionNames[i], "VK_KHR_multiview") == 0)
            ext_requested = true;
    // ...or as a feature struct in the chain (either spelling).
    bool feature_requested = false;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext; p;
         p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)
            feature_requested |=
                ((const VkPhysicalDeviceMultiviewFeatures *)p)->multiview;
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            feature_requested |=
                ((const VkPhysicalDeviceVulkan11Features *)p)->multiview;
    }

    if (!inst.GetPhysicalDeviceProperties2 || !inst.GetPhysicalDeviceFeatures2) {
        X4VR_LOG("multiview: cannot probe — no vkGetPhysicalDevice*2 "
                 "(app apiVersion %u.%u)",
                 VK_API_VERSION_MAJOR(inst.app_api_version),
                 VK_API_VERSION_MINOR(inst.app_api_version));
        return;
    }

    VkPhysicalDeviceMultiviewProperties mvp{};
    mvp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &mvp;
    inst.GetPhysicalDeviceProperties2(phys, &props);

    VkPhysicalDeviceMultiviewFeatures mvf{};
    mvf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    VkPhysicalDeviceFeatures2 feats{};
    feats.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    feats.pNext = &mvf;
    inst.GetPhysicalDeviceFeatures2(phys, &feats);

    // The feature struct we add is core 1.1. An app that declared 1.0 would
    // need the VK_KHR_multiview *extension* enabled as well -- a second edit,
    // to ppEnabledExtensionNames -- so refuse rather than build an invalid
    // chain. X4 declares 1.2, so this is a guard, not a code path we use.
    const bool api_ok = inst.app_api_version >= VK_API_VERSION_1_1;
    g_multiview_supported =
        mvf.multiview && mvp.maxMultiviewViewCount >= 2 && api_ok;

    X4VR_LOG("multiview: supported=%d maxViews=%u maxInstanceIndex=%u "
             "geomShader=%d tessShader=%d",
             (int)mvf.multiview, mvp.maxMultiviewViewCount,
             mvp.maxMultiviewInstanceIndex, (int)mvf.multiviewGeometryShader,
             (int)mvf.multiviewTessellationShader);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (strstr(e, "dynamic_rendering") || strstr(e, "multiview") ||
            strstr(e, "shader_draw_parameters"))
            X4VR_LOG("multiview: X4 enables %s", e);
    }
    X4VR_LOG("multiview: X4 requests it? ext=%d feature=%d — device api %u.%u, "
             "app api %u.%u",
             (int)ext_requested, (int)feature_requested,
             VK_API_VERSION_MAJOR(props.properties.apiVersion),
             VK_API_VERSION_MINOR(props.properties.apiVersion),
             VK_API_VERSION_MAJOR(inst.app_api_version),
             VK_API_VERSION_MINOR(inst.app_api_version));
    if (mvf.multiview && mvp.maxMultiviewViewCount < 2)
        X4VR_LOG("multiview: WARNING maxViews < 2 — two eyes cannot share a "
                 "render pass on this device");
    if (mvf.multiview && !api_ok)
        X4VR_LOG("multiview: not enabling — app declared api %u.%u, needs 1.1 "
                 "for the core feature struct",
                 VK_API_VERSION_MAJOR(inst.app_api_version),
                 VK_API_VERSION_MINOR(inst.app_api_version));
}

// Turn multiview on in X4's own VkDeviceCreateInfo.
//
// Measured: X4 targets Vulkan 1.2, where multiview is core -- so there is no
// extension to add and no KHR alias to worry about -- but it leaves the
// feature disabled, and a disabled feature makes every multiview render pass
// we would later create invalid.
//
// Three cases, because the chain may already speak for multiview:
//   * VkPhysicalDeviceVulkan11Features present -> flip its bit. Adding a
//     separate VkPhysicalDeviceMultiviewFeatures alongside it is forbidden.
//   * VkPhysicalDeviceMultiviewFeatures present -> flip its bit.
//   * neither -> prepend our own struct to a copy of the create info.
//
// The first two write through the application's const chain. That is
// deliberate: relinking instead would mean copying every struct ahead of the
// target, and we cannot know the size of extension structs we do not
// recognise. The write is one VkBool32 in a transient struct the application
// discards after the call, and it sets it to what we want for any later
// vkCreateDevice too.
const VkDeviceCreateInfo *enable_multiview(
    const VkDeviceCreateInfo *ci, VkDeviceCreateInfo &copy,
    VkPhysicalDeviceMultiviewFeatures &feat) {
    if (!g_multiview_supported)
        return ci;
    for (auto *p = (VkBaseOutStructure *)ci->pNext; p; p = p->pNext) {
        VkBool32 *bit = nullptr;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            bit = &((VkPhysicalDeviceVulkan11Features *)p)->multiview;
        else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES)
            bit = &((VkPhysicalDeviceMultiviewFeatures *)p)->multiview;
        if (!bit)
            continue;
        if (!*bit) {
            *bit = VK_TRUE;
            X4VR_LOG("multiview: enabled in X4's existing feature struct");
        }
        return ci;
    }
    feat = {};
    feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES;
    feat.multiview = VK_TRUE;
    feat.pNext = (void *)ci->pNext;
    copy = *ci;
    copy.pNext = &feat;
    X4VR_LOG("multiview: enabled — feature struct added to X4's device");
    return &copy;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateDevice(
    VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkDevice *out) {
    VkLayerDeviceCreateInfo *link = find_device_link(ci);
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa =
        link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    auto next_create =
        (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

    // Probe first: it reads physical-device state only, and it has to report
    // what X4 asked for rather than what we are about to add to the chain.
    // These two must outlive next_create() -- the driver reads the chain.
    VkDeviceCreateInfo mv_ci{};
    VkPhysicalDeviceMultiviewFeatures mv_feat{};
    if (g_active) {
        probe_multiview(phys, ci);
        if (g_multiview_enable)
            ci = enable_multiview(ci, mv_ci, mv_feat);
    }

    VkResult r = next_create(phys, ci, ac, out);
    if (r != VK_SUCCESS)
        return r;

    DeviceData d;
    d.device = *out;
    d.gdpa = gdpa;
#define RESOLVE(name) d.name = (PFN_vk##name)gdpa(*out, "vk" #name)
    RESOLVE(DestroyDevice);
    RESOLVE(CreateBuffer);
    RESOLVE(DestroyBuffer);
    RESOLVE(UpdateDescriptorSets);
    RESOLVE(CmdBindDescriptorSets);
    RESOLVE(CmdDraw);
    RESOLVE(CmdDrawIndexed);
    RESOLVE(QueuePresentKHR);
    RESOLVE(CreateSwapchainKHR);
    RESOLVE(DestroySwapchainKHR);
    RESOLVE(MapMemory);
    RESOLVE(UnmapMemory);
    RESOLVE(BindBufferMemory);
    RESOLVE(QueueSubmit);
    RESOLVE(GetDeviceQueue);
    RESOLVE(GetDeviceQueue2);
    RESOLVE(GetSwapchainImagesKHR);
    RESOLVE(CreateShaderModule);
    RESOLVE(DestroyShaderModule);
    RESOLVE(CreateRenderPass);
    RESOLVE(CreateFramebuffer);
    RESOLVE(CmdCopyImage);
    RESOLVE(CmdBlitImage);
    RESOLVE(CmdResolveImage);
    RESOLVE(CmdClearColorImage);
    RESOLVE(CreateImage);
    RESOLVE(DestroyImage);
    RESOLVE(CreateImageView);
    RESOLVE(DestroyImageView);
    RESOLVE(CreateRenderPass2);
    RESOLVE(DestroyRenderPass);
    RESOLVE(CreateGraphicsPipelines);
    RESOLVE(DestroyPipeline);
    RESOLVE(BeginCommandBuffer);
    RESOLVE(CmdBeginRenderPass);
    RESOLVE(CmdEndRenderPass);
    RESOLVE(CmdBindPipeline);
    RESOLVE(CmdPipelineBarrier);
    RESOLVE(CmdCopyBufferToImage);
    RESOLVE(CmdClearDepthStencilImage);
    RESOLVE(CmdCopyImageToBuffer);
    RESOLVE(AllocateMemory);
    RESOLVE(FreeMemory);
    RESOLVE(GetBufferMemoryRequirements);
    RESOLVE(QueueWaitIdle);
    // Needed to place the readback buffer in host-visible memory. Resolved
    // here because the physical device is in scope only during creation.
    {
        VkInstance inst = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_instances.find(dispatch_key(phys));
            if (it != g_instances.end())
                inst = it->second.instance;
        }
        if (auto gpmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
                inst, "vkGetPhysicalDeviceMemoryProperties"))
            gpmp(phys, &d.memprops);
        d.phys = phys;
    }
    // Core in 1.3; the KHR alias is what a 1.2 device exposes. Left null if
    // neither exists, and the matching hook is then never reachable, because
    // the application cannot call an entry point its device does not have.
#define RESOLVE2(name)                                                         \
    d.name = (PFN_vk##name)gdpa(*out, "vk" #name);                             \
    if (!d.name)                                                               \
        d.name = (PFN_vk##name)gdpa(*out, "vk" #name "KHR");
    RESOLVE2(CmdCopyImage2);
    RESOLVE2(CmdBlitImage2);
    RESOLVE2(CmdResolveImage2);
    RESOLVE2(CmdCopyBufferToImage2);
#undef RESOLVE2
#undef RESOLVE
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_devices[dispatch_key(*out)] = d;
    }
    X4VR_LOG("device created");

    if (g_sbs_enabled && g_active) {
        x4vr::SbsFns f;
        // The X-macro list has no separators, so each expansion terminates
        // itself.
#define RESOLVE(name) f.name = (PFN_vk##name)gdpa(*out, "vk" #name);
        X4VR_SBS_FNS(RESOLVE)
#undef RESOLVE
        if (f.complete()) {
            // gipa resolves physical-device entry points only against a
            // real instance, not VK_NULL_HANDLE (that is for global ones).
            VkInstance inst = VK_NULL_HANDLE;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                auto it = g_instances.find(dispatch_key(phys));
                if (it != g_instances.end())
                    inst = it->second.instance;
            }
            VkPhysicalDeviceMemoryProperties mem{};
            if (auto gpmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(
                    inst, "vkGetPhysicalDeviceMemoryProperties"))
                gpmp(phys, &mem);
            g_sbs.configure(*out, f, mem);
        } else {
            X4VR_LOG("sbs: could not resolve every entry point — composite off");
        }
    }
    if (const char *h = getenv("X4VR_TEST_HAMMER"); h && *h && *h != '0') {
        static std::thread t(hammer_thread);
        t.detach();
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroyDevice(
    VkDevice device, const VkAllocationCallbacks *ac) {
    if (g_mv_probe && g_mv && g_active) {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end())
            probe_collect(&it->second, VK_NULL_HANDLE);
    }
    mv_report("final");
    g_sbs.shutdown(); // idles the device, then frees pools/semaphores/fences
    PFN_vkDestroyDevice next;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        next = g_devices.at(dispatch_key(device)).DestroyDevice;
        g_devices.erase(dispatch_key(device));
    }
    next(device, ac);
}

// ------------------------------------------------------------ proc address

struct NameFunc {
    const char *name;
    PFN_vkVoidFunction fn;
};

const NameFunc kHooks[] = {
    {"vkCreateInstance", (PFN_vkVoidFunction)x4vr_CreateInstance},
    {"vkDestroyInstance", (PFN_vkVoidFunction)x4vr_DestroyInstance},
    {"vkCreateDevice", (PFN_vkVoidFunction)x4vr_CreateDevice},
    {"vkDestroyDevice", (PFN_vkVoidFunction)x4vr_DestroyDevice},
    {"vkCreateBuffer", (PFN_vkVoidFunction)x4vr_CreateBuffer},
    {"vkDestroyBuffer", (PFN_vkVoidFunction)x4vr_DestroyBuffer},
    {"vkUpdateDescriptorSets", (PFN_vkVoidFunction)x4vr_UpdateDescriptorSets},
    {"vkCmdBindDescriptorSets", (PFN_vkVoidFunction)x4vr_CmdBindDescriptorSets},
    {"vkCmdDraw", (PFN_vkVoidFunction)x4vr_CmdDraw},
    {"vkCmdDrawIndexed", (PFN_vkVoidFunction)x4vr_CmdDrawIndexed},
    {"vkQueuePresentKHR", (PFN_vkVoidFunction)x4vr_QueuePresentKHR},
    {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
     (PFN_vkVoidFunction)x4vr_GetPhysicalDeviceSurfaceCapabilitiesKHR},
    {"vkGetSwapchainImagesKHR",
     (PFN_vkVoidFunction)x4vr_GetSwapchainImagesKHR},
    {"vkGetDeviceQueue", (PFN_vkVoidFunction)x4vr_GetDeviceQueue},
    {"vkGetDeviceQueue2", (PFN_vkVoidFunction)x4vr_GetDeviceQueue2},
    {"vkCreateSwapchainKHR", (PFN_vkVoidFunction)x4vr_CreateSwapchainKHR},
    {"vkDestroySwapchainKHR", (PFN_vkVoidFunction)x4vr_DestroySwapchainKHR},
    {"vkMapMemory", (PFN_vkVoidFunction)x4vr_MapMemory},
    {"vkUnmapMemory", (PFN_vkVoidFunction)x4vr_UnmapMemory},
    {"vkBindBufferMemory", (PFN_vkVoidFunction)x4vr_BindBufferMemory},
    {"vkQueueSubmit", (PFN_vkVoidFunction)x4vr_QueueSubmit},
    {"vkCreateShaderModule", (PFN_vkVoidFunction)x4vr_CreateShaderModule},
    {"vkDestroyShaderModule", (PFN_vkVoidFunction)x4vr_DestroyShaderModule},
    {"vkCreateRenderPass", (PFN_vkVoidFunction)x4vr_CreateRenderPass},
    {"vkCreateFramebuffer", (PFN_vkVoidFunction)x4vr_CreateFramebuffer},
    {"vkCmdCopyImage", (PFN_vkVoidFunction)x4vr_CmdCopyImage},
    {"vkCmdBlitImage", (PFN_vkVoidFunction)x4vr_CmdBlitImage},
    {"vkCmdResolveImage", (PFN_vkVoidFunction)x4vr_CmdResolveImage},
    {"vkCmdClearColorImage", (PFN_vkVoidFunction)x4vr_CmdClearColorImage},
    {"vkCmdBeginRenderPass", (PFN_vkVoidFunction)x4vr_CmdBeginRenderPass},
    {"vkCmdEndRenderPass", (PFN_vkVoidFunction)x4vr_CmdEndRenderPass},
    {"vkBeginCommandBuffer", (PFN_vkVoidFunction)x4vr_BeginCommandBuffer},
    {"vkCmdBindPipeline", (PFN_vkVoidFunction)x4vr_CmdBindPipeline},
    {"vkCmdPipelineBarrier", (PFN_vkVoidFunction)x4vr_CmdPipelineBarrier},
    {"vkDestroyPipeline", (PFN_vkVoidFunction)x4vr_DestroyPipeline},
    {"vkCmdCopyImage2", (PFN_vkVoidFunction)x4vr_CmdCopyImage2},
    {"vkCmdCopyImage2KHR", (PFN_vkVoidFunction)x4vr_CmdCopyImage2},
    {"vkCmdBlitImage2", (PFN_vkVoidFunction)x4vr_CmdBlitImage2},
    {"vkCmdBlitImage2KHR", (PFN_vkVoidFunction)x4vr_CmdBlitImage2},
    {"vkCmdResolveImage2", (PFN_vkVoidFunction)x4vr_CmdResolveImage2},
    {"vkCmdResolveImage2KHR", (PFN_vkVoidFunction)x4vr_CmdResolveImage2},
    {"vkCmdCopyBufferToImage",
     (PFN_vkVoidFunction)x4vr_CmdCopyBufferToImage},
    {"vkCmdCopyBufferToImage2",
     (PFN_vkVoidFunction)x4vr_CmdCopyBufferToImage2},
    {"vkCmdCopyBufferToImage2KHR",
     (PFN_vkVoidFunction)x4vr_CmdCopyBufferToImage2},
    {"vkCmdClearDepthStencilImage",
     (PFN_vkVoidFunction)x4vr_CmdClearDepthStencilImage},
    {"vkCreateImage", (PFN_vkVoidFunction)x4vr_CreateImage},
    {"vkDestroyImage", (PFN_vkVoidFunction)x4vr_DestroyImage},
    {"vkCreateImageView", (PFN_vkVoidFunction)x4vr_CreateImageView},
    {"vkDestroyImageView", (PFN_vkVoidFunction)x4vr_DestroyImageView},
    {"vkCreateRenderPass2", (PFN_vkVoidFunction)x4vr_CreateRenderPass2},
    {"vkDestroyRenderPass", (PFN_vkVoidFunction)x4vr_DestroyRenderPass},
    {"vkCreateGraphicsPipelines",
     (PFN_vkVoidFunction)x4vr_CreateGraphicsPipelines},
};

PFN_vkVoidFunction find_hook(const char *name) {
    for (const auto &h : kHooks)
        if (!strcmp(name, h.name))
            return h.fn;
    return nullptr;
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
x4vr_GetDeviceProcAddr(VkDevice device, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
x4vr_GetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)x4vr_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)x4vr_GetDeviceProcAddr;
    if (PFN_vkVoidFunction fn = find_hook(name))
        return fn;
    if (instance == VK_NULL_HANDLE)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_instances.find(dispatch_key(instance));
    if (it == g_instances.end())
        return nullptr;
    return it->second.gipa(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
x4vr_GetDeviceProcAddr(VkDevice device, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)x4vr_GetDeviceProcAddr;
    if (PFN_vkVoidFunction fn = find_hook(name))
        return fn;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_devices.find(dispatch_key(device));
    if (it == g_devices.end())
        return nullptr;
    return it->second.gdpa(device, name);
}

// Classic layer entry points (referenced from the manifest "functions").
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return x4vr_GetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return x4vr_GetDeviceProcAddr(device, name);
}

} // extern "C"
