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

#include <dlfcn.h>

#include <algorithm>
#include <set>
#include <atomic>
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
#include "../common/x4vr_env.hpp"
#include "../common/x4vr_share.hpp"
#include "../common/x4vr_headlook.hpp"
#ifdef X4VR_HAVE_OPENXR
#include "../common/x4vr_xr.hpp"
#endif

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
    PFN_vkGetPhysicalDeviceQueueFamilyProperties
        GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
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
    PFN_vkCreateDescriptorUpdateTemplate CreateDescriptorUpdateTemplate =
        nullptr;
    PFN_vkUpdateDescriptorSetWithTemplate UpdateDescriptorSetWithTemplate =
        nullptr;
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
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
    PFN_vkCmdDispatchIndirect CmdDispatchIndirect = nullptr;
    PFN_vkCmdDispatchBase CmdDispatchBase = nullptr;
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
// Read through env_on because the injector mirrors this exact decision to
// choose whether to hide the compositor's pointer. Two spellings of the same
// rule is how they come to disagree.
const bool g_sbs_enabled = x4vr::env_on("X4VR_SBS", false);

// Whether X4 is made to render one eye's worth (half width) rather than the
// full frame. On by default with X4VR_SBS=1; X4VR_SBS_SPLIT=0 falls back to
// duplicating the left half of a full-width frame, which is the older and
// less invasive behaviour.
const bool g_sbs_split_render = x4vr::env_on("X4VR_SBS_SPLIT", true);

// Stage 2 groundwork: give the image X4 renders into (believing it is the
// swapchain) a second array layer, so there is somewhere for the second eye
// to land. Allocation only -- X4VR_SBS_RIGHT_LAYER is what actually puts it
// on screen, and it stays 0 until a masked pass is known to write layer 1.
//
// Split in two because "the memory exists" and "the display is stereo" are
// separate claims, and a run that conflates them reports a success it has not
// earned: copying an unwritten layer into the right half is garbage on screen,
// not a stereo frame.
const bool g_sbs_two_layer = [] {
    const char *e = getenv("X4VR_SBS_LAYERS");
    return e && *e && *e != '0' && *e != '1';
}();
const uint32_t g_sbs_right_layer = [] {
    const char *e = getenv("X4VR_SBS_RIGHT_LAYER");
    return e && *e ? (uint32_t)strtoul(e, nullptr, 0) : 0u;
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

// Task #36. On a device with one graphics queue -- which is every AMD one --
// X4 and the OpenXR runtime submit to the same VkQueue from two threads, and
// external synchronisation is the application's job. These serialise the two
// sides: X4's submits and presents take it here, the runtime's take it around
// xrEndFrame on the layer's XR thread.
//
// xrWaitFrame is deliberately NOT covered: it blocks until the runtime's next
// frame boundary, and holding a queue lock across it would stall X4 for a whole
// headset frame every frame.
std::mutex g_vr_queue_mu;
bool g_vr_share_queue = false; // set when the reservation had to fall back

// #36's lock, made re-entrant per thread — because the calls it guards submit
// through our OWN vkQueueSubmit hook, which takes it again.
//
// take 114c hung exactly there:
//
//     x4vr_QueuePresentKHR -> vr_finish_blit -> swapchain_release
//       -> oxr_xrReleaseSwapchainImage -> x4vr_QueueSubmit -> lock (held)
//
// xrReleaseSwapchainImage submits, and so does xrEndFrame; both are called
// inside this lock, and both re-enter the hook on the same thread. std::mutex
// is not recursive, so the thread waited for itself.
//
// A recursive_mutex would also fix it, and is the wrong fix: it would make
// every future re-entrant path silently legal, including ones that should be
// questioned. This says the specific true thing — a thread that already owns
// the queue does not need to take it again — and still serialises the two
// threads against each other, which is the whole point of #36.
thread_local int g_vr_queue_depth = 0;

struct VrQueueLock {
    bool locked_ = false;
    VrQueueLock() {
        if (g_vr_share_queue && g_vr_queue_depth == 0) {
            g_vr_queue_mu.lock();
            locked_ = true;
        }
        g_vr_queue_depth++;
    }
    ~VrQueueLock() {
        g_vr_queue_depth--;
        if (locked_)
            g_vr_queue_mu.unlock();
    }
    VrQueueLock(const VrQueueLock &) = delete;
    VrQueueLock &operator=(const VrQueueLock &) = delete;
};

// Surfaces whose capabilities we reported at half width.
//
// This began as the halve/double pairing: only a surface recorded here could
// have its extent doubled back at swapchain creation. That reasoning is gone
// -- the split test now keys off the extent X4 actually requests, so it holds
// whichever lever moved it -- and what survives is a "have we said this yet"
// set for the log. Recorded because a set that no longer means what its name
// says is how a stale invariant gets read as a live one.
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

// Which WSI actually built each surface.
//
// The layer used to infer the display server from currentExtent ==
// 0xFFFFFFFF and print "this is the Wayland WSI". That is a sentinel, not a
// measurement: it says the surface declines to state a size, and the Wayland
// WSI is merely the one we *know* does that. Take thirty-three then produced
// the sentinel from X4's own pid while the launcher was forcing
// SDL_VIDEO_DRIVER=x11 -- so either the forcing did not take or the inference
// was wrong, and no line in the log could tell those apart. Record the entry
// point that was called instead of deducing it from a value.
//
// "unknown" is a real answer and is printed as such: a surface can also come
// from a WSI we do not hook (headless, display-plane, a platform added
// later), and silence there would read as Wayland by elimination.
std::unordered_map<VkSurfaceKHR, const char *> g_surface_wsi;

void note_surface_wsi(VkSurfaceKHR s, const char *wsi) {
    std::lock_guard<std::mutex> lock(g_surface_mu);
    g_surface_wsi[s] = wsi;
}

// Surfaces are destroyed and their handles reused, exactly as swapchain
// images were in take thirty. Forget on destroy so a later surface cannot
// inherit this one's platform.
// Surfaces whose capabilities have been reported once already.
std::unordered_set<VkSurfaceKHR> g_surfaces_seen;

bool note_surface_seen(VkSurfaceKHR s) { // true the first time only
    std::lock_guard<std::mutex> lock(g_surface_mu);
    return g_surfaces_seen.insert(s).second;
}

void forget_surface(VkSurfaceKHR s) {
    std::lock_guard<std::mutex> lock(g_surface_mu);
    g_surface_wsi.erase(s);
    g_halved_surfaces.erase(s);
    g_surfaces_seen.erase(s);
}

const char *surface_wsi(VkSurfaceKHR s) {
    std::lock_guard<std::mutex> lock(g_surface_mu);
    auto it = g_surface_wsi.find(s);
    return it == g_surface_wsi.end() ? "unknown" : it->second;
}

// Ask SDL which backend it chose, and what it thinks the driver hint says.
//
// Read-only, via dlsym on symbols that are already loaded -- deliberately not
// an LD_PRELOAD interposition. SDL2 and SDL3 disagree about the signature of
// SDL_CreateWindow, and gamescope (SDL2) shares this process tree with X4
// (SDL3), so *defining* an SDL symbol here would risk calling one through the
// other's prototype. Calling an already-resolved symbol cannot do that.
//
// SDL_GetCurrentVideoDriver() only answers once video is initialised, so this
// is called from surface creation -- by which point a window exists -- rather
// than from vkCreateInstance.
void log_sdl_backend_once() {
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;
    using driver_fn = const char *(*)();
    using hint_fn = const char *(*)(const char *);
    driver_fn cur = nullptr;
    hint_fn hint = nullptr;
    void *p = dlsym(RTLD_DEFAULT, "SDL_GetCurrentVideoDriver");
    memcpy(&cur, &p, sizeof(cur));
    p = dlsym(RTLD_DEFAULT, "SDL_GetHint");
    memcpy(&hint, &p, sizeof(hint));
    if (!cur) {
        X4VR_LOG("wsi: SDL is not loaded in pid %d — the surface came from "
                 "somewhere else",
                 (int)getpid());
        return;
    }
    const char *d = cur();
    // SDL_HINT_VIDEO_DRIVER is spelled "SDL_VIDEO_DRIVER"; SDL2 read
    // "SDL_VIDEODRIVER". The effective value is what SDL_GetHint returns,
    // which already resolves the environment-versus-SDL_SetHint precedence we
    // would otherwise have to reason about.
    const char *h = hint ? hint("SDL_VIDEO_DRIVER") : nullptr;
    X4VR_LOG("wsi: SDL_GetCurrentVideoDriver()=%s hint SDL_VIDEO_DRIVER=%s "
             "(pid %d)",
             d ? d : "(null)", h ? h : "(unset)", (int)getpid());
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
    bool logged_proj_fail = false;
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
    // patched module handle -> its constant-shift twin (task #30). Present
    // only for World modules, and only when a canvas was asked for: a module
    // that has no entry here is one the canvas must not touch.
    std::unordered_map<VkShaderModule, VkShaderModule> canvas;
    // render pass -> per-subpass "this subpass must not be sheared"
    std::unordered_map<VkRenderPass, std::vector<bool>> unsheared;
    // render pass -> per-subpass "this subpass draws the UI"
    std::unordered_map<VkRenderPass, std::vector<bool>> canvas_pass;
    uint32_t swapped = 0, canvas_swapped = 0;
} g_variants;

// render pass -> inventory serial, so the framebuffer log can name the pass
// it belongs to. Inventory only; shares g_variants.mu.
std::unordered_map<VkRenderPass, uint32_t> g_rp_serials;

// Passes we gave a view mask; the framebuffer hook needs to know so it can
// supply array views. Shares g_variants.mu with g_rp_serials.
std::unordered_set<VkRenderPass> g_masked_passes;

// The subset of those that are masked *only* because of the SRGB carve-out --
// rp #40 and #52, both of which render into #103.
//
// These were called "the tonemap" until their shaders were read, and they are
// not one. Six different pipelines draw through rp #40: one fullscreen textured
// quad and several with full vertex attributes, i.e. UI geometry. #103 is the
// LDR *composition* target, not a tonemap output. X4VR_MASK_TONEMAP keeps its
// name because the tagged runs and the doc's launch command use it, but the
// name is a misnomer -- see docs/frame-analysis.md.
std::unordered_set<VkRenderPass> g_srgb_resolve_passes;

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
    // Came from vkGetSwapchainImagesKHR, not vkCreateImage. Worth carrying
    // because the usage and layer count here are what the swapchain
    // guarantees, not what a create info said.
    bool swapchain = false;
};
// Format and extent of a swapchain, kept so its images can be registered with
// a serial when the game asks for them.
struct SwapchainInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    // Kept so the images can be forgotten when the swapchain dies. The driver
    // recycles VkImage handles across swapchains, and registration skips a
    // handle it already knows: take twenty-nine's second swapchain got serials
    // for images 1..3 and silently reused image 0's entry from the swapchain
    // before it, which shifted every later serial by one against take
    // twenty-eight and left a dead swapchain's extent attached to a live image.
    std::vector<VkImage> images;
};
std::mutex g_sc_mu;
std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;
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
    // Which layer this entry actually views. Cached alongside because the cache
    // is keyed by X4's view alone while two callers now ask for different
    // layers; an entry for the wrong layer is treated exactly like a stale one
    // and rebuilt, so the cache cannot quietly hand back the other caller's view.
    uint32_t layer;
};
std::unordered_map<VkImageView, Layer1View> g_layer1_views;
uint32_t g_img_serial = 0;

// --- the bindless survey (X4VR_BINDLESS_SURVEY=1) -----------------------
//
// X4 samples through a bindless table: 53306 descriptors at set 0 binding 7,
// indexed by S_diffuse_idx out of a uniform block. The replacement for the
// abandoned type-promotion patch is an index offset -- view 1 reads
// slot+OFFSET, where the layer has parked a layer-1 view of the same image --
// and its design depends on facts nobody has measured. This answers them and
// changes no behaviour.
//
// Two of the four questions were settled offline from the shader dumps and are
// not re-asked here: no module of 409 mentions NonUniform (so the indices are
// draw-uniform and the patch has no decoration to preserve), and every one of
// 333 declarations at set 0 binding 7 is a plain 2D sampled image -- never a
// storage image, never a subpass input. What the dumps could not say is how many
// descriptors the set actually holds and which slots receive the doubled images.
// Both are runtime facts.
const bool g_bindless_survey = [] {
    const char *e = getenv("X4VR_BINDLESS_SURVEY");
    return e && *e && *e != '0';
}();

// Step A of task #13: duplicate every image-descriptor write into slot + OFFSET,
// with a layer-1 view where the image is doubled and the descriptor verbatim
// otherwise, and patch no shader. Nothing indexes the twin region, so the frame
// must not change -- which makes any frame-time delta the mirror's cost alone and
// any validation error the mirror's fault alone.
const bool g_bindless_mirror = [] {
    const char *e = getenv("X4VR_BINDLESS_MIRROR");
    return e && *e && *e != '0';
}();

// Half of X4's declared 53,306, against a measured high-water mark of 10,980.
// Overridable because the right value is a property of the session, not of the
// build: if the used prefix ever grows past this the twin would overwrite X4's
// own textures, so the mirror watches for it rather than trusting the margin.
const uint32_t g_mirror_offset = [] {
    const char *e = getenv("X4VR_MIRROR_OFFSET");
    const long v = e && *e ? strtol(e, nullptr, 10) : 0;
    return v > 0 ? (uint32_t)v : 26653u;
}();

// The layout/set bookkeeping below serves both knobs, and the mirror cannot work
// without it: attributing a write to a table is how it knows a twin region
// exists at all. Gating that on the survey alone made X4VR_BINDLESS_MIRROR=1
// silently do nothing -- the same "knob that quietly does nothing" defect the
// survey itself had one commit earlier, found the same way, by a counter that
// prints its zero.
const bool g_desc_track = g_bindless_survey || g_bindless_mirror;

// Step B: rewrite the table index to `idx + gl_ViewIndex * OFFSET`.
//
// Requires the mirror. Without it the twin half of the table is never written,
// and a patched shader in view 1 would read a descriptor that was never filled
// in -- undefined, not merely wrong. The two knobs are separate because step A
// has to be measurable on its own, but this one is clamped to the other.
const bool g_bindless_patch = [] {
    const char *e = getenv("X4VR_BINDLESS_PATCH");
    return e && *e && *e != '0';
}();

std::mutex g_desc_mu;
// Take twenty-one keyed these by binding alone, which conflates every set that
// happens to use the same binding number -- the log grew a "binding 0" with one
// slot in it, and nothing in the bindless tables lives at binding 0. The offset
// has to be applied to one specific table, so the key is (layout, binding).
//
// A VkDescriptorSet has no "set index": which set it becomes is chosen at
// vkCmdBindDescriptorSets, not at allocation. What *is* a property of the set is
// the layout it was allocated from, and that is the thing worth distinguishing.
// Layouts carrying a descriptor array get a serial; everything else reports as
// layout ?, which is how a stray binding announces itself instead of hiding.
uint32_t g_dsl_serial = 0;
std::unordered_map<VkDescriptorSetLayout, uint32_t> g_dsl_id;
std::unordered_map<VkDescriptorSet, uint32_t> g_ds_layout;
// layout serial -> binding -> declared descriptorCount. The mirror needs it to
// refuse a twin write that would run off the end of the table X4 declared:
// writing past the end is a validation error, not a stereo bug.
std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> g_dsl_counts;
// P4: how many sets come from each table layout -- the mirror covers all of them,
// and the survey never counted.
std::unordered_map<uint32_t, uint32_t> g_dsl_sets;

// Mirror accounting. `collided` is the one that matters: it means X4 itself wrote
// into the twin region, so the offset is too small for this session and
// continuing would corrupt X4's own textures rather than merely break stereo.
// Declared up here rather than beside g_mod_mu because the summary prints them
// and is defined first. Atomic so they need no lock ordering against it.
// Declared here, with the other state the end-of-run summary reports, because
// that summary is defined before any of the hooks that fill these in.
struct XferEdge {
    uint64_t n = 0;
    uint64_t widened = 0;
};
std::mutex g_xfer_mu;
std::map<std::pair<uint32_t, uint32_t>, std::map<const char *, XferEdge>>
    g_xfer_edges;
uint64_t g_xfer_uploads = 0;
std::unordered_set<uint32_t> g_xfer_upload_targets;

// The passes that draw into a swapchain image -- the last ones in the frame,
// and the only ones whose output the player ever sees.
//
// The join lives at framebuffer creation because a render pass never names an
// image; only a framebuffer does. It is read back in the end-of-run summary
// rather than at pipeline creation, so no ordering between the two has to hold:
// X4 may create either first and this must not depend on which.
//
// Take twenty-seven asked "does a draw write the final image?" of the writer
// list, which tracks only *doubled* images. A swapchain image is single-layer
// and owned by the presentation engine, so it can never be doubled and could
// never have appeared there -- the list was structurally incapable of holding
// the answer, and returned a confident no.
std::mutex g_present_mu;
std::unordered_set<uint32_t> g_present_rps; // rp serials
struct PassFrag {
    uint32_t module = UINT32_MAX;
    bool patched = false;
    char samplers[224] = {0};
};
// rp serial -> the fragment shaders drawn through it, deduped by module.
std::map<uint32_t, std::map<uint32_t, PassFrag>> g_rp_frag;

std::mutex g_comp_mu;
std::unordered_map<VkPipeline, uint32_t> g_comp_module; // pipeline -> serial
std::unordered_map<uint32_t, uint64_t> g_comp_dispatches; // serial -> count
std::unordered_map<VkCommandBuffer, VkPipeline> g_comp_bound;
uint64_t g_comp_pipelines = 0;

std::atomic<uint64_t> g_frag_patch_ok{0}, g_frag_patch_refused{0};
// Task #30: canvas variants built, and modules the canvas could not be built
// for. A refusal leaves that module's UI mono, which looks exactly like a
// correct frame, so the count is reported whether or not it is zero.
std::atomic<uint64_t> g_canvas_built{0}, g_canvas_refused{0};
// The canvas's per-view NDC x offset, published once the variants are actually
// built and left at zero otherwise. It is the *one* definition of where the
// canvas sits: the shader patch bakes it into the UI's vertices and the cursor
// overlay offsets the pointer by it, and if those two ever read different
// numbers the pointer drifts off the button by the difference -- with both
// mechanisms working exactly as written. Published from the same branch that
// sets have_canvas so a refused canvas cannot move the cursor on its own.
std::atomic<float> g_canvas_shift{0.0f};
// Task #40. #30's canvas was a translation, so one float published all of it.
// With the affine on it is a per-eye affine, and the cursor overlay has to
// apply the SAME one -- a pointer that takes only the translation lands a
// fixed distance from every button, in a run where both features report
// success. Written once, inside the call_once that builds K_canvas, and read
// per present; `ready` is the release/acquire pair that publishes it.
struct CanvasMap {
    std::atomic<bool> ready{false};
    x4vr::CanvasNdc eye[2];
    void publish(const float kl[16], const float kr[16]) {
        eye[0] = x4vr::canvas_ndc_of(kl);
        eye[1] = x4vr::canvas_ndc_of(kr);
        ready.store(true, std::memory_order_release);
    }
    const x4vr::CanvasNdc *get() const {
        return ready.load(std::memory_order_acquire) ? eye : nullptr;
    }
} g_canvas_map;
// Task #22: deferred modules whose M_invprojection was corrected per eye.
std::atomic<uint64_t> g_invproj_patched{0};
// Task #35 piece 2, defined with offaxis_target() far below. Declared here
// because the summary block reports the affine's state alongside these
// counters, and reads it without latching -- see the note at the definition.
extern std::atomic<bool> g_offaxis_on;
extern std::atomic<bool> g_offaxis_latched;
// ...and M_invprojection_uj, the one the shadow cascades actually read.
std::atomic<uint64_t> g_invproj_uj_patched{0};
// Task #22 measurement only. Counts modules whose volumetric fog composite was
// forced to a passthrough, to separate "the fog produces the difference between
// the eyes" from "the fog carries a difference made upstream".
std::atomic<uint64_t> g_fog_disabled{0};
// Of the refusals, the ones that are refusals by construction: compute has no
// gl_ViewIndex, so a compute shader sampling the heap cannot be made per-view
// by this mechanism at all.
std::atomic<uint64_t> g_compute_tables{0};
uint64_t g_mirror_writes = 0, g_mirror_descriptors = 0, g_mirror_layer1 = 0;
// Descriptors left pointing at layer 0 because only unmasked passes write the
// image -- shadow cascades above all. Counted separately from the undoubled
// case so the log can show the difference between "there is no layer 1" and
// "there is one and nothing has ever written it".
uint64_t g_mirror_shared = 0;
uint64_t g_mirror_no_room = 0;
bool g_mirror_collided = false;

// Handles are recycled after vkFreeDescriptorSets, so a stale entry could
// misattribute a table. Allocation always precedes any write to a handle, so
// overwriting on allocate is enough -- do not erase on free.
inline uint64_t desc_key(uint32_t layout, uint32_t binding) {
    return ((uint64_t)layout << 32) | binding;
}

// "#3", or "?" for a set whose layout declared no descriptor array -- which is
// how writes that do not belong to a table stay visible instead of being
// silently folded into one.
inline void desc_layout_label(uint64_t key, char *out, size_t n) {
    const uint32_t lid = (uint32_t)(key >> 32);
    if (lid == UINT32_MAX)
        snprintf(out, n, "?");
    else
        snprintf(out, n, "#%u", lid);
}

// (layout, binding) -> the distinct array elements X4 has ever written
std::unordered_map<uint64_t, std::unordered_set<uint32_t>> g_desc_slots;
// (layout, binding) -> slot -> image serial, for slots holding a *doubled*
// image. This is what the twin region is designed around: which table, and which
// elements, actually have to be mirrored.
std::unordered_map<uint64_t, std::unordered_map<uint32_t, uint32_t>> g_desc_pe;
uint64_t g_desc_writes = 0, g_desc_late_writes = 0;
bool g_desc_first_frame_done = false;

// The road the survey was not watching. vkUpdateDescriptorSetWithTemplate is
// core 1.1 and X4 declares API 1.2, so it needs no extension string -- the log's
// silence about templates proved nothing, and take twenty-one's slot counts are
// only complete if this path is unused. Worse than incomplete: a mirror that
// hooks vkUpdateDescriptorSets alone would silently miss template writes, and
// view 1 would then read a descriptor nobody ever wrote.
//
// Counted, not assumed. The template's entries are recorded at creation because
// the update call carries no type or binding information of its own -- if this
// path turns out to be live, this is also exactly what mirroring it needs.
uint64_t g_tmpl_updates = 0, g_tmpl_image_updates = 0;
std::unordered_map<VkDescriptorUpdateTemplate, std::vector<uint32_t>>
    g_tmpl_image_bindings;

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
// Which pass the buffer is inside, so a probe capture can name the pass whose
// end it followed. #103 has two writers and they need not hold the same thing.
std::unordered_map<VkCommandBuffer, VkRenderPass> g_cb_rp;

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

// The tonemap resolve, told apart from the blit chain by format alone.
//
// The LDR domain is not one thing. X4's tonemap writes #103 in
// B8G8R8A8_SRGB; everything downstream of it -- the UI passes and the final
// blit into what X4 believes is the swapchain -- is B8G8R8A8_UNORM. In a full
// inventory the entire MONO set is 10 depth-only shadow cascades, 7 passes at
// UNORM, and exactly 2 at SRGB: rp #40 and rp #52, both writing #103. SRGB
// appears nowhere else on the mono side, so keying on it cannot reach the
// blit chain, whose attachment is the presented image and genuinely cannot
// take a second array layer.
//
// This is a *masking* question, not a shear one -- see split_note in
// pass_is_per_eye. Do not fold these formats into is_ldr_format to get the
// same effect: that would also send the tonemap down the sheared path and
// start applying K to a fullscreen triangle.
inline bool is_srgb_ldr_format(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
}

// Must this (render pass, subpass) use the unpatched modules?
bool needs_original(VkRenderPass rp, uint32_t subpass) {
    std::lock_guard<std::mutex> lock(g_variants.mu);
    auto it = g_variants.unsheared.find(rp);
    if (it == g_variants.unsheared.end() || subpass >= it->second.size())
        return false;
    return it->second[subpass];
}

// Does this (render pass, subpass) draw the UI? Task #30.
bool is_canvas_pass(VkRenderPass rp, uint32_t subpass) {
    std::lock_guard<std::mutex> lock(g_variants.mu);
    auto it = g_variants.canvas_pass.find(rp);
    if (it == g_variants.canvas_pass.end() || subpass >= it->second.size())
        return false;
    return it->second[subpass];
}

// A canvas variant that could not be built leaves that module's UI mono, which
// on screen is indistinguishable from a correct frame -- so every refusal names
// itself and is counted. Rate-limited because this runs once per module; the
// total is reported at teardown whether it is zero or not.
void canvas_refuse(const char *why) {
    if (++g_canvas_refused <= 3)
        X4VR_LOG("canvas: REFUSED for a module — %s; that module's UI stays "
                 "mono",
                 why);
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
// A single-layer view onto `layer` of one of X4's views, created on demand and
// cached.
//
// VK_NULL_HANDLE means "not a per-eye image", which is the right answer for most
// descriptors and the safe answer for one written before its framebuffer taught
// us the image was doubled: the read stays on layer 0, which is correct output
// rather than undefined memory.
//
// `layer` is a parameter and not g_mv_present_layer because the two callers want
// different things and it cost a debugging cycle to notice. The redirect wants
// "the layer we are presenting", which is what its knob names. The mirror wants
// the layer view index 1 renders to, which is 1 by definition. Those coincided
// only because g_mv_present_layer defaults to 0 and the redirect is the only
// thing that sets it -- so the mirror was silently building layer-0 twins, a
// no-op that would have passed every step-A gate and surfaced two steps later as
// an unexplained mono frame.
// Restore the pre-take-73 mirror, which substituted layer 1 for any doubled
// image whether or not anything had written it.
const bool g_mirror_all_layer1 = [] {
    const char *e = getenv("X4VR_MIRROR_ALL_LAYER1");
    return e && *e && *e != '0';
}();

// Does layer 1 of this view's image ever get written?
//
// Being doubled is not the question. X4's five 2048x2048 shadow cascades are
// doubled -- they go through the same allocation path as everything else -- but
// they are written only by *unmasked* passes, because a shadow map is light
// space and genuinely shared between the eyes, which classify_unsheared() has
// always got right. Nothing ever writes their layer 1.
//
// It was believed through takes 74 and 75 that the mirror pointed view 1 at
// that empty layer, blowing out the right eye's shading. THAT WAS WRONG, and
// the refutation is structural rather than empirical: view_of_layer() returns
// null unless the image is in g_per_eye_images, and an image only enters that
// set in CreateFramebuffer's `masked && g_active` branch. A shadow map is
// never attached to a view-masked pass, so it is never in the set, so
// view_of_layer() has always returned null for it and the mirror has always
// written the verbatim view. The right eye has been reading the real, correct
// shadow map in every take.
//
// This predicate is therefore a guard that cannot currently fire for the case
// it was written for -- the gate below reaches the same answer one line later.
// It is kept because it states the intent where the intent belongs, and
// because the gate is about "is there a layer 1 to point at" while this is
// about "should we point at it", which are different questions that happen to
// coincide today.
//
// An image with an unmasked writer and no masked one is shared by construction,
// so the verbatim copy is not a fallback for it -- it is the correct answer.
//
// Take 74 shipped this as a bool and the artifact did not move, because the
// question is asked ~19.5 s before it can be answered. From that run's log:
//
//     646088.047  img #70..#74  2048x2048 fmt=124 usage=0xa6 DOUBLED
//     646088.204  bindless mirror first present: ... 0 kept at layer 0
//     646107.581  rp #35.0: 0 colour [] depth 124 -> MONO (depth-only/shadow)
//     646107.581  fb  rp #35: 2048x2048 attachments=1 imgs=[#70]
//
// X4 creates the cascades and puts them in the heap long before it builds a
// framebuffer naming them, and g_img_writers only learns anything at
// framebuffer time. Every shadow slot written in that window took the
// unknown-writers branch, got layer 1, and was never revisited -- the mirror
// is written once and left. So the predicate was right and simply never ran on
// the descriptors it was written for.
//
// Hence a tri-state: Unknown is recorded, not guessed at. The provisional
// slots are repaired when the framebuffer finally says what the image is.
enum class Layer1State { Written, Shared, Unknown };

Layer1State layer1_state(VkImageView v, uint32_t *serial_out) {
    if (g_mirror_all_layer1)
        return Layer1State::Written;
    std::lock_guard<std::mutex> lock(g_img_mu);
    auto it = g_views.find(v);
    if (it == g_views.end())
        return Layer1State::Written;
    auto im = g_images.find(it->second.image);
    if (im == g_images.end())
        return Layer1State::Written;
    if (serial_out)
        *serial_out = im->second.serial;
    auto w = g_img_writers.find(im->second.serial);
    if (w == g_img_writers.end()) {
        // An undoubled image has no layer 1 to get wrong: view_of_layer()
        // returns null and the verbatim copy stands. Only a doubled image with
        // no writers yet is genuinely undecided.
        return im->second.doubled ? Layer1State::Unknown : Layer1State::Written;
    }
    return (w->second.masked.empty() && !w->second.unmasked.empty())
               ? Layer1State::Shared
               : Layer1State::Written;
}

// A mirror slot filled while the image's writers were still unknown, kept so
// it can be rewritten verbatim once they are known.
struct ProvisionalSlot {
    VkDescriptorSet set;
    uint32_t binding, element;
    VkDescriptorType type;
    VkDescriptorImageInfo info; // the original, pre-substitution
};
std::mutex g_prov_mu;
std::unordered_map<uint32_t, std::vector<ProvisionalSlot>> g_mirror_provisional;
uint64_t g_mirror_provisional_taken = 0, g_mirror_repaired = 0;
// Undecided slots seen at all, counted before the per-eye gate rather than
// after it. The two differ by every image that has no layer 1 to point at.
uint64_t g_mirror_unknown_seen = 0;

const bool g_mirror_repair = [] {
    const char *e = getenv("X4VR_MIRROR_REPAIR");
    return !e || !*e || *e != '0'; // on unless explicitly disabled
}();

VkImageView view_of_layer(DeviceData *d, VkDevice device, VkImageView v,
                          uint32_t layer) {
    if (v == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;
    ViewInfo vi{};
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        auto it = g_views.find(v);
        auto c = g_layer1_views.find(v);
        // A hit whose image no longer matches is a recycled handle: the entry
        // describes a view X4 has since destroyed. A hit for a different layer
        // belongs to the other caller. Either way, drop it and rebuild rather
        // than pointing the read somewhere it was never meant to go.
        if (c != g_layer1_views.end() &&
            (c->second.layer != layer ||
             (it != g_views.end() && c->second.image != it->second.image))) {
            g_layer1_views.erase(c);
            c = g_layer1_views.end();
            std::lock_guard<std::mutex> lock2(g_mv_mu);
            g_mv_stats.redirect_stale++;
        }
        if (c != g_layer1_views.end())
            return c->second.view; // misses are cached too, to stop retrying
        if (it == g_views.end() || !g_per_eye_images.count(it->second.image) ||
            it->second.range.baseArrayLayer != 0)
            return VK_NULL_HANDLE;
        vi = it->second;
    }
    VkImageView repl = VK_NULL_HANDLE;
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = vi.image;
    ci.viewType = vi.type;
    ci.format = vi.format;
    ci.components = vi.components;
    ci.subresourceRange = vi.range;
    ci.subresourceRange.baseArrayLayer = layer;
    ci.subresourceRange.layerCount = 1;
    if (d->CreateImageView(device, &ci, nullptr, &repl) != VK_SUCCESS)
        repl = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(g_img_mu);
    // Keyed with the image and the layer so a recycled handle or the other
    // caller's layer invalidates the entry above.
    g_layer1_views[v] = Layer1View{repl, vi.image, layer};
    return repl;
}

// Put the verbatim view back into every mirror slot that was filled for this
// image while its writers were still unknown.
//
// Called once, from framebuffer creation, at the moment the image is first
// shown to be written by unmasked passes only. The mirror region is ours --
// X4 never reads or writes past OFFSET -- so rewriting it needs no
// synchronisation with the game's own descriptor traffic beyond the
// update-after-bind guarantee the table is already created with.
void repair_mirror_for_image(DeviceData *d, VkDevice device, uint32_t serial) {
    std::vector<ProvisionalSlot> slots;
    {
        std::lock_guard<std::mutex> lock(g_prov_mu);
        auto it = g_mirror_provisional.find(serial);
        if (it == g_mirror_provisional.end())
            return;
        slots.swap(it->second);
        g_mirror_provisional.erase(it);
    }
    if (slots.empty())
        return;
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(slots.size());
    {
        // A set whose layout we no longer know has been freed under us; its
        // slots are not ours to write.
        std::lock_guard<std::mutex> dlock(g_desc_mu);
        for (const auto &s : slots) {
            if (!g_ds_layout.count(s.set))
                continue;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = s.set;
            w.dstBinding = s.binding;
            w.dstArrayElement = s.element;
            w.descriptorCount = 1;
            w.descriptorType = s.type;
            w.pImageInfo = &s.info; // slots outlives the call
            writes.push_back(w);
        }
    }
    if (writes.empty())
        return;
    d->UpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0,
                            nullptr);
    g_mirror_repaired += writes.size();
    X4VR_LOG("bindless mirror: repaired %zu provisional slot(s) for img #%u — "
             "its writers turned out to be unmasked-only, so view 1 must read "
             "layer 0",
             writes.size(), serial);
}

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
        // This is what left layer 1 rasterised but unlit. Geometry lands in
        // view 1 because that is ordinary rasterisation; the light
        // contribution does not, because subpassLoad reads through a
        // descriptor that cannot see the layer being rendered.
        //
        // Corrected against the 409 dumped modules: exactly 26 of them declare
        // a subpass input, every one of them declares *one*, and every one is
        // InputAttachmentIndex 0 -- the S_subpassInput_AUTOMS X4's own
        // validation errors name. An earlier version of this comment said the
        // deferred passes read the G-buffer through "the other four" subpass
        // inputs and named the passes by serial (rp 30/31/32/64). Both were
        // wrong: X4 reads one input attachment, and pass serials are per-run,
        // so naming them here dates the comment to a log nobody still has.
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
            const VkImageView repl = view_of_layer(
                d, device, infos[j].imageView, g_mv_present_layer);
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

// Step A: the twin region, written but never read.
//
// For every image-descriptor write into a table, append a second write at
// slot + OFFSET. Where the image is doubled the twin gets a view of layer 1;
// where it is not, the twin gets the identical descriptor, so a shader reading
// idx + OFFSET sees the same texture either way. That is the property the whole
// mechanism rests on: undoubled textures read the same in both views, so the
// eventual patch needs no per-shader targeting and no knowledge of which slot
// holds which image -- and a shadow map's twin *is* the same shadow map.
//
// Mirrored from `writes`, X4's original intent, rather than from the redirect's
// output: the two are alternative mechanisms and must not compose.
void bindless_mirror_writes(
    DeviceData *d, VkDevice device, uint32_t writeCount,
    const VkWriteDescriptorSet *writes,
    std::vector<VkWriteDescriptorSet> &out,
    std::vector<std::vector<VkDescriptorImageInfo>> &mpool) {
    if (g_mirror_collided)
        return;
    // The redirect points X4's *own* slot at layer 1; the mirror leaves it alone
    // and puts layer 1 in a twin slot. They are alternative answers to the same
    // question and composing them would make view 0 stereo-wrong as well, so the
    // pair is refused rather than ranked.
    if (g_mv_redirect) {
        g_mirror_collided = true;
        X4VR_LOG("bindless mirror: DISABLED — X4VR_MV_PRESENT_LAYER is also "
                 "set. The redirect retargets X4's own descriptor and the "
                 "mirror adds a twin; running both would corrupt view 0. "
                 "Pick one.");
        return;
    }
    mpool.reserve(writeCount); // no reallocation, so pImageInfo stays valid
    for (uint32_t i = 0; i < writeCount; i++) {
        const VkWriteDescriptorSet &w = writes[i];
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            w.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            continue;
        if (!w.pImageInfo || !w.descriptorCount)
            continue;
        uint32_t declared = 0;
        {
            std::lock_guard<std::mutex> dlock(g_desc_mu);
            auto la = g_ds_layout.find(w.dstSet);
            if (la == g_ds_layout.end())
                continue; // not a set from a table layout
            auto lc = g_dsl_counts.find(la->second);
            if (lc == g_dsl_counts.end())
                continue;
            auto bc = lc->second.find(w.dstBinding);
            if (bc == lc->second.end())
                continue;
            declared = bc->second;
            // P5's guard, and the only failure here that is worse than no
            // stereo: if X4's own prefix reaches OFFSET, our twins are landing
            // on descriptors X4 is using. Stop, loudly, rather than corrupt
            // them.
            if (declared > g_mirror_offset &&
                w.dstArrayElement + w.descriptorCount > g_mirror_offset) {
                g_mirror_collided = true;
                X4VR_LOG("bindless mirror: DISABLED — X4 wrote binding %u slot "
                         "%u..%u, at or past OFFSET %u. The twin region overlaps "
                         "live descriptors; raise X4VR_MIRROR_OFFSET or shrink "
                         "the mirrored range.",
                         w.dstBinding, w.dstArrayElement,
                         w.dstArrayElement + w.descriptorCount - 1,
                         g_mirror_offset);
                return;
            }
        }
        // A table too short to hold a twin is not one of the big ones -- the
        // 18-sampler and 58-input-attachment bindings land here.
        if ((uint64_t)w.dstArrayElement + w.descriptorCount + g_mirror_offset >
            declared) {
            g_mirror_no_room++;
            continue;
        }
        std::vector<VkDescriptorImageInfo> infos(
            w.pImageInfo, w.pImageInfo + w.descriptorCount);
        for (uint32_t j = 0; j < w.descriptorCount; j++) {
            // Shared by construction -- only unmasked passes write it, so its
            // layer 1 is empty and substituting it is how the right eye lost
            // its shadows. See layer1_state().
            uint32_t serial = 0;
            const Layer1State st = layer1_state(infos[j].imageView, &serial);
            if (st == Layer1State::Shared) {
                g_mirror_shared++;
                continue;
            }
            // Counted here, before the gate below, because that gate removes
            // exactly the population this number is about. Take 75 reported
            // "0 slot(s) filled on unknown writers" from a counter placed
            // after it and I read that as "the race does not exist"; it meant
            // "this cannot see the race". Third instrument in this project
            // blind to its own target -- see docs/frame-analysis.md.
            if (st == Layer1State::Unknown)
                g_mirror_unknown_seen++;
            const VkImageView l1 =
                view_of_layer(d, device, infos[j].imageView, 1);
            if (l1 == VK_NULL_HANDLE)
                continue; // undoubled: the verbatim copy is the right answer
            if (st == Layer1State::Unknown && g_mirror_repair) {
                // Recorded *before* the substitution, so the repair has the
                // original view to put back.
                std::lock_guard<std::mutex> plock(g_prov_mu);
                g_mirror_provisional[serial].push_back(
                    ProvisionalSlot{w.dstSet, w.dstBinding,
                                    w.dstArrayElement + g_mirror_offset + j,
                                    w.descriptorType, infos[j]});
                g_mirror_provisional_taken++;
            }
            infos[j].imageView = l1;
            g_mirror_layer1++;
        }
        mpool.push_back(std::move(infos));
        VkWriteDescriptorSet tw = w;
        tw.dstArrayElement = w.dstArrayElement + g_mirror_offset;
        tw.pImageInfo = mpool.back().data();
        out.push_back(tw);
        g_mirror_writes++;
        g_mirror_descriptors += w.descriptorCount;
    }
}

// Q2, and the question the twin region is actually designed around: which
// bindings and which array elements receive a view of a *doubled* image?
//
// That is what has to be mirrored, and it is knowable only here -- an image is
// classified per-eye at framebuffer time, long after it was created, and the
// slot it lands in is X4's choice. Counts distinct slots per binding, and
// separately the slots holding per-eye images, with the image serial so the
// result can be joined against the frame graph.
void bindless_survey_writes(uint32_t writeCount,
                            const VkWriteDescriptorSet *writes) {
    std::lock_guard<std::mutex> lock(g_desc_mu);
    for (uint32_t i = 0; i < writeCount; i++) {
        const VkWriteDescriptorSet &w = writes[i];
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
            w.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            continue;
        if (!w.pImageInfo)
            continue;
        auto la = g_ds_layout.find(w.dstSet);
        const uint32_t lid = la != g_ds_layout.end() ? la->second : UINT32_MAX;
        const uint64_t key = desc_key(lid, w.dstBinding);
        for (uint32_t j = 0; j < w.descriptorCount; j++) {
            const uint32_t slot = w.dstArrayElement + j;
            g_desc_writes++;
            // Writes arriving after the first frame are the reason mirroring
            // cannot be a one-shot pass at startup.
            if (g_desc_first_frame_done)
                g_desc_late_writes++;
            g_desc_slots[key].insert(slot);
            const VkImageView v = w.pImageInfo[j].imageView;
            if (v == VK_NULL_HANDLE)
                continue;
            std::lock_guard<std::mutex> lock2(g_img_mu);
            auto it = g_views.find(v);
            if (it == g_views.end() || !g_per_eye_images.count(it->second.image))
                continue;
            auto im = g_images.find(it->second.image);
            g_desc_pe[key][slot] =
                im != g_images.end() ? im->second.serial : UINT32_MAX;
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
    if (g_bindless_survey && g_active)
        bindless_survey_writes(writeCount, writes);
    std::vector<VkWriteDescriptorSet> redirected;
    std::vector<std::vector<VkDescriptorImageInfo>> pool, mpool;
    if (g_mv && g_active) {
        mv_redirect_writes(d, device, writeCount, writes, redirected, pool);
        // The mirror appends twin writes; mv_redirect_writes has already sized
        // `redirected` to writeCount, so anything past that is ours.
        if (g_bindless_mirror)
            bindless_mirror_writes(d, device, writeCount, writes, redirected,
                                   mpool);
        d->UpdateDescriptorSets(device, (uint32_t)redirected.size(),
                                redirected.data(), copyCount, copies);
    } else if (g_bindless_mirror && g_active) {
        redirected.assign(writes, writes + writeCount);
        bindless_mirror_writes(d, device, writeCount, writes, redirected, mpool);
        d->UpdateDescriptorSets(device, (uint32_t)redirected.size(),
                                redirected.data(), copyCount, copies);
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

// Mask the tonemap resolve so it renders into both layers of #103.
//
// Off by default: it is the first change that makes a pass outside the HDR
// domain replicate, and it costs a second full-resolution fullscreen pass. On
// its own it changes nothing visible -- the chain that reads #103 is still
// mono -- so the knob exists to make the step measurable in isolation rather
// than to be left on.
const bool g_mask_tonemap = [] {
    const char *e = getenv("X4VR_MASK_TONEMAP");
    return e && *e && *e != '0';
}();

// Mask the composite -- the pass that draws the finished frame into the image
// that gets presented -- so it renders both eyes into the two layers of the SBS
// eye image. This is the last mono link in the chain.
//
// Off by default. It is the only masking decision that doubles the *final*
// fullscreen pass, and it is inert without X4VR_SBS_LAYERS=2 (nothing has a
// second layer to write) and invisible without X4VR_SBS_RIGHT_LAYER=1 (the
// compositor still blits layer 0 into both halves). Three knobs for three
// separate claims, deliberately, so a run cannot report a stereo frame it has
// not earned.
// Report the eye extent for a surface that declines to state one, so X4's
// render size stops being its window size. See the long note on
// x4vr_GetPhysicalDeviceSurfaceCapabilitiesKHR.
const bool g_fake_extent = [] {
    const char *e = getenv("X4VR_FAKE_EXTENT");
    return e && *e && *e != '0';
}();

// Mask every all-LDR pass, not just the ones a heuristic calls "the present".
//
// Take forty-three localised the black right eye: the world images carry a
// real second eye (#97/#98 DIFFER), and the present targets have a full layer
// 0 against a layer 1 holding 0.5%-30% of it -- the HUD and little else. The
// inventory says why: 8 all-LDR/UI passes masked nothing while 6 were masked
// as present candidates. The scene reaches the screen through some of the
// eight.
//
// Worse than the miss is its instability. subpass_is_present() matches on the
// shape of whatever passes the current scene builds, so the same command
// produced 3 candidates in take thirty-three and 6 in take forty-three. A
// configuration that depends on the scene is not a configuration, and it is
// why the working state could not be restored from its knobs.
//
// This masks on the property that actually matters -- every colour attachment
// is LDR, so the pass is somewhere on the post/UI path to the screen -- and
// stops caring which one is the composite.
const bool g_mask_ldr = [] {
    const char *e = getenv("X4VR_MASK_LDR");
    return e && *e && *e != '0';
}();

const bool g_mask_present = [] {
    const char *e = getenv("X4VR_MASK_PRESENT");
    return e && *e && *e != '0';
}();

// Does this subpass actually have a depth attachment?
//
// The index, never the pointer -- a subpass may carry a valid
// pDepthStencilAttachment aimed at VK_ATTACHMENT_UNUSED, which is exactly the
// distinction that cost takes 31 through 68 in subpass_is_present().
template <typename CreateInfo, typename Subpass>
bool subpass_has_depth(const CreateInfo *ci, const Subpass &sp) {
    if (!sp.pDepthStencilAttachment)
        return false;
    const uint32_t a = sp.pDepthStencilAttachment->attachment;
    return a != VK_ATTACHMENT_UNUSED && a < ci->attachmentCount;
}

// Restore the pre-take-71 behaviour of shearing fullscreen passes, so the
// previous state stays reachable from a knob rather than from git.
const bool g_shear_nodepth = [] {
    const char *e = getenv("X4VR_SHEAR_NODEPTH");
    return e && *e && *e != '0';
}();

// Shear the all-LDR/UI passes too. A probe -- see where it is read below.
const bool g_shear_ui = [] {
    const char *e = getenv("X4VR_SHEAR_UI");
    return e && *e && *e != '0';
}();

// Classify each subpass as "must not be sheared":
//   * no colour attachments        -> shadow cascade (light space)
//   * no depth attachment          -> fullscreen post pass (screen space)
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
        // A pass with no depth attachment cannot be rasterising depth-tested
        // world geometry -- there is nothing to test against. It is a
        // fullscreen triangle: deferred lighting, SSAO, a bloom mip, an
        // exposure reduction. K must not touch those, for the reason already
        // written above about the tonemap: shearing a fullscreen triangle is
        // meaningless. It displaces the quad sideways per eye while the
        // buffers it samples stay put, so every fragment reads the wrong
        // texel and the result is lighting that disagrees between the eyes.
        //
        // Take 70 measured the consequence. The G-buffer (rp #23/24/25/53,
        // which do carry depth) agrees between eyes to 0.5%; #57, the lighting
        // output written by the no-depth rp #31/#32, disagrees by 85% -- and
        // on screen the ship's hull is shadowed in one eye and blown white in
        // the other. 29 of X4's passes were being sheared this way against 12
        // genuine world-geometry passes, including the 4096x1 exposure
        // reduction, which is as clearly not world geometry as a pass can be.
        //
        // This changes only the shear. classify_per_eye() below keeps every
        // one of these masked, so the set of doubled passes -- and the frame
        // cost -- is exactly what it was.
        if (!g_shear_nodepth && !subpass_has_depth(ci, sp)) {
            unsheared[i] = true;
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
        // **Elimination probe for the cockpit HUD, not a shipping mode.**
        // Every HUD element measures ZERO disparity against -20 to -36 px on
        // the world beside it (takes 151A/151B), so X4's cockpit panels are
        // being drawn at infinity when they sit about a metre away. They reach
        // this rule the same way the message box does -- one all-LDR pass draws
        // both -- so no pass-level predicate can separate them, which is the
        // hull-versus-menu-quad shape again.
        //
        // This knob shears that pass anyway. It is expected to be WRONG for
        // half of what it touches: the menus and the message box are screen
        // space and will shear when they should not, and because X4 hit-tests
        // its UI on the CPU in unshifted screen space, clicking will not line
        // up while it is on. That is acceptable for one look and unacceptable
        // to ship. What it answers is which elements snap back onto the
        // cockpit -- the set that wants the world treatment, and therefore what
        // a late-selected variant has to key on.
        if (g_shear_ui && any && all_ldr) {
            unsheared[i] = false;
            continue;
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
//
// split_note: this used to be exactly the inverse of classify_unsheared, and
// the comment above said so as if the two questions were one. They are not.
// classify_unsheared answers "does K apply to this pass's vertex shaders?";
// this answers "does this pass render into both layers?". Stage 1 could
// conflate them because no pass needed different answers. The tonemap is the
// first that does: it is a fullscreen triangle, so K must NOT be applied to
// it -- shearing a fullscreen triangle is meaningless -- but it must be
// masked, or there is no second layer of #103 for a per-eye tonemap to write.
// Hence one predicate for shear, one for masking, no longer inverses.
// Candidates for the composite.
//
// Take thirty-one refuted the first version of this, which required
// `finalLayout == PRESENT_SRC_KHR` and was described here as "a definition, not
// a heuristic". It is a definition of *one* way to present. X4 uses the other:
// it leaves its attachment in some other layout and transitions with an
// explicit barrier, so the predicate matched nothing and `rp #0` stayed
// `MONO (all-LDR/UI)`. The finalLayout is now printed in the inventory line so
// the next such claim can be checked instead of assumed.
//
// What is left is honestly a heuristic and is labelled as one: a single LDR
// colour attachment, no depth, in the swapchain's own format. That is seven
// passes, not one -- every full-screen LDR pass in the frame, of which the
// composite is one. Masking all seven costs a handful of extra fullscreen
// passes and is a bring-up tool, not a shipping configuration.
//
// It still has to be decided here rather than at framebuffer time, where the
// image is finally named: a pipeline is only compatible with render passes of
// the same viewMask, and X4 builds its pipelines long before any framebuffer
// exists. A pass is masked at creation or not at all.

// Every colour attachment is LDR, and there is at least one.
//
// Deliberately says nothing about attachment count or depth: those are what
// subpass_is_present() adds to guess at the composite, and they are exactly
// what excluded the eight passes the scene travels through. A depth buffer on
// an LDR pass does not make it a world pass -- classify_unsheared already
// decided that, and this only ever runs on passes it called MONO.
template <typename CreateInfo, typename Subpass>
bool subpass_is_all_ldr(const CreateInfo *ci, const Subpass &sp) {
    uint32_t seen = 0;
    for (uint32_t c = 0; c < sp.colorAttachmentCount; c++) {
        const uint32_t a = sp.pColorAttachments[c].attachment;
        if (a == VK_ATTACHMENT_UNUSED || a >= ci->attachmentCount)
            continue;
        if (!is_ldr_format(ci->pAttachments[a].format))
            return false;
        seen++;
    }
    return seen > 0;
}

// Task #30: is this the pass the UI is drawn into?
//
// All colour LDR and no depth. In takes 61, 74 and 80 that is `rp #33`
// (`1 colour [50L] no-depth`) plus the five blit passes at format 44, and
// nothing else: world passes carry depth, the HDR fullscreen post passes are
// not LDR, and the shadow cascades have no colour at all.
//
// It is deliberately *not* narrowed to sRGB to single out `rp #33`. That would
// key on a coincidence of X4's format choice; the blits are excluded instead by
// the other half of the join, at pipeline creation, because every module they
// bind is procedural or fragment-only and so never classifies World. Both
// halves do real work and each is checkable on its own.
template <typename CreateInfo, typename Subpass>
bool subpass_is_canvas(const CreateInfo *ci, const Subpass &sp) {
    return sp.colorAttachmentCount > 0 && !subpass_has_depth(ci, sp) &&
           subpass_is_all_ldr(ci, sp);
}

template <typename CreateInfo, typename Subpass>
bool subpass_is_present(const CreateInfo *ci, const Subpass &sp) {
    if (sp.colorAttachmentCount != 1)
        return false;
    // "Has no depth" is a statement about the attachment index, not about the
    // pointer. A subpass may carry a perfectly valid pDepthStencilAttachment
    // whose attachment is VK_ATTACHMENT_UNUSED, and X4's rp #7 does exactly
    // that: the inventory printed it as `1 colour [44L] no-depth` -- because
    // the inventory reads the index -- while this predicate read the pointer
    // and threw it out. Same field, two readings, opposite answers, on the one
    // pass that decides whether the second eye gets the UI.
    //
    // The cost of the disagreement was take 68: the presented eye images
    // #50-#53 and #1-#4 came back `MIXED WRITERS -- layer 1 misses the
    // unmasked ones`, so everything rp #7 draws landed in the left eye and
    // nowhere else. The probe samples after rp #0 and so never saw it, which
    // is why two tools in a row measured this frame and reported clean stereo
    // while the screen showed dark patches in one eye.
    if (sp.pDepthStencilAttachment &&
        sp.pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED)
        return false;
    const uint32_t a = sp.pColorAttachments[0].attachment;
    if (a == VK_ATTACHMENT_UNUSED || a >= ci->attachmentCount)
        return false;
    if (ci->pAttachments[a].finalLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        return true;
    // Take thirty-two: the swapchain format cannot be used here. X4 creates
    // `rp #0` *before* it creates the swapchain -- same millisecond, adjacent
    // log lines, render pass first -- so g_present_format is still UNDEFINED
    // when the only question that matters is asked, and the fallback was dead
    // on arrival. Second time this project has bet on a creation order without
    // checking it; the present-pass *report* was deliberately deferred to the
    // summary to avoid exactly this, and then the decision made the same bet.
    //
    // So: no external state at all. A single LDR colour attachment with no
    // depth is the shape of every fullscreen composition pass in the frame, the
    // real composite among them.
    return is_ldr_format(ci->pAttachments[a].format);
}

template <typename CreateInfo, typename Subpass>
bool subpass_is_srgb_resolve(const CreateInfo *ci, const Subpass &sp) {
    if (sp.colorAttachmentCount != 1)
        return false;
    const uint32_t a = sp.pColorAttachments[0].attachment;
    if (a == VK_ATTACHMENT_UNUSED || a >= ci->attachmentCount)
        return false;
    return is_srgb_ldr_format(ci->pAttachments[a].format);
}

// Does this subpass render into both eyes? Kept per-subpass so the inventory
// log can report the shear and mask verdicts separately -- a pass that reads
// MONO but is masked anyway is precisely the state that would be misread if
// only one verdict were printed.
template <typename CreateInfo>
std::vector<bool> classify_per_eye(const CreateInfo *ci) {
    const std::vector<bool> unsheared = classify_unsheared(ci);
    std::vector<bool> per_eye(ci->subpassCount, false);
    for (uint32_t i = 0; i < ci->subpassCount; i++)
        per_eye[i] = !unsheared[i] ||
                     // Fullscreen post passes reached this predicate through
                     // !unsheared until take 71 made them unsheared. They must
                     // stay masked -- unmasking them would leave layer 1 with
                     // no lighting at all -- so say it explicitly, and the set
                     // of doubled passes is exactly what it was. Guarded on
                     // having colour so depth-only shadow passes stay shared.
                     (ci->pSubpasses[i].colorAttachmentCount > 0 &&
                      !subpass_has_depth(ci, ci->pSubpasses[i])) ||
                     (g_mask_tonemap && subpass_is_srgb_resolve(ci, ci->pSubpasses[i])) ||
                     (g_mask_present && subpass_is_present(ci, ci->pSubpasses[i])) ||
                     (g_mask_ldr && subpass_is_all_ldr(ci, ci->pSubpasses[i]));
    return per_eye;
}

template <typename CreateInfo>
bool pass_is_per_eye(const CreateInfo *ci) {
    if (!g_mv || !g_multiview_supported)
        return false;
    for (bool pe : classify_per_eye(ci))
        if (pe)
            return true;
    return false;
}

template <typename CreateInfo>
void record_render_pass(const CreateInfo *ci, VkRenderPass rp) {
    std::vector<bool> unsheared = classify_unsheared(ci);
    const std::vector<bool> per_eye = classify_per_eye(ci);
    std::vector<bool> canvas(ci->subpassCount, false);
    for (uint32_t i = 0; i < ci->subpassCount; i++)
        canvas[i] = subpass_is_canvas(ci, ci->pSubpasses[i]);

    std::lock_guard<std::mutex> lock(g_variants.mu);
    for (uint32_t i = 0; i < ci->subpassCount; i++)
        if (unsheared[i] && per_eye[i])
            g_srgb_resolve_passes.insert(rp);
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
            // Candidacy is not identity. This predicate matches every
            // fullscreen LDR pass, and only one of them is the composite, so
            // the label says "candidate" and the existing verdict strings are
            // left alone.
            const bool cand = g_mask_present && subpass_is_present(ci, sp);
            const char *why = sp.colorAttachmentCount == 0 ? "depth-only/shadow"
                              : !subpass_has_depth(ci, sp) ? "fullscreen post"
                              : unsheared[i]               ? "all-LDR/UI"
                                                           : "world";
            // The MONO/STEREO verdict is about K. Since the split it no longer
            // implies the masking decision, so a pass that takes no shear but
            // does replicate has to say so on its own line -- otherwise the
            // inventory reports "MONO" for a pass rendering into both layers,
            // which is the kind of quietly-wrong instrument this file keeps
            // having to correct.
            // finalLayout of the first colour attachment. Printed because take
            // thirty-one spent a run on a predicate that assumed it.
            int fl = -1;
            if (sp.colorAttachmentCount &&
                sp.pColorAttachments[0].attachment != VK_ATTACHMENT_UNUSED &&
                sp.pColorAttachments[0].attachment < ci->attachmentCount)
                fl = (int)ci->pAttachments[sp.pColorAttachments[0].attachment]
                         .finalLayout;
            // Which rule masked it, not just that something did. Take
            // forty-three could not tell a pass masked as "the present" from
            // one masked for being LDR, and the difference between those two
            // is the difference between a stable configuration and a
            // scene-dependent one.
            const char *rule = "";
            if (unsheared[i] && per_eye[i]) {
                if (sp.colorAttachmentCount > 0 && !subpass_has_depth(ci, sp))
                    rule = " +MASKED(fullscreen)";
                else if (g_mask_tonemap && subpass_is_srgb_resolve(ci, sp))
                    rule = " +MASKED(tonemap)";
                else if (g_mask_present && subpass_is_present(ci, sp))
                    rule = " +MASKED(present)";
                else if (g_mask_ldr && subpass_is_all_ldr(ci, sp))
                    rule = " +MASKED(ldr)";
                else
                    rule = " +MASKED(?)";
            }
            // Task #30. Printed whether or not a canvas was asked for, because
            // "this pass would take the canvas" is a property of X4's frame,
            // not of the run's knobs -- and it is what makes the inventory of
            // an ordinary take enough to check the predicate against.
            X4VR_LOG("rp #%u.%u: %u colour [%s]%s final=%d -> %s (%s)%s%s%s",
                     serial, i, sp.colorAttachmentCount, fmts, dep, fl,
                     unsheared[i] ? "MONO" : "STEREO", why,
                     rule, cand ? " +PRESENT-CAND" : "",
                     canvas[i] ? " +CANVAS" : "");
        }
        // Whether each attachment is cleared, loaded or discarded on entry.
        //
        // Not tracked until take 76, and its absence is what left the bisection
        // one fact short. The probe reads #57 "after rp #24" and finds it
        // already wrong, while its co-attachments #59/#60/#61 -- written by the
        // *same* draws into the *same* framebuffer -- come out clean. Two
        // readings fit, and load-op decides between them:
        //
        //   CLEAR/DONT_CARE -> the pass starts from nothing, so the divergence
        //     is made inside rp #23/#24/#25, by one output of one shader.
        //   LOAD            -> the pass inherits last frame's #57, so what the
        //     probe sees includes rp #31/#32 from the previous frame and those
        //     two are still suspects.
        //
        // Ruling a suspect in or out with an instrument that cannot see the
        // difference is the mistake recorded for rp #7 at take 68; this line
        // exists so the same call is not made blind twice.
        char ops[224];
        int n = 0;
        ops[0] = 0;
        static const char *kLoad[] = {"LOAD", "CLEAR", "DONT_CARE", "NONE"};
        for (uint32_t a = 0; a < ci->attachmentCount && n < 190; a++) {
            const uint32_t lo = (uint32_t)ci->pAttachments[a].loadOp;
            n += snprintf(ops + n, sizeof(ops) - n, "%s%u:%s", a ? " " : "", a,
                          lo < 4 ? kLoad[lo] : "?");
        }
        X4VR_LOG("rp #%u attachments — loadOp %s", serial, ops);
        // Which attachment *indices* each subpass actually writes.
        //
        // The framebuffer line lists every attachment, and take 76 read that
        // list as the pass's outputs: "rp #23/#24/#25 write #57 alongside
        // #59/#60/#61, so the same draws produce one wrong image and three
        // clean ones". They do not. Those passes declare **1 colour**
        // attachment; the other five are attached and untouched, and
        // g_img_writers records attachment rather than authorship, so its
        // "writers" over-claims in exactly the same way.
        //
        // Attachment membership is not authorship. Printing the indices costs
        // one line and removes the guess -- two images here share format 97,
        // so the format alone cannot say which one a subpass writes.
        for (uint32_t i = 0; i < ci->subpassCount; i++) {
            const auto &sp = ci->pSubpasses[i];
            char w[224];
            int m = 0;
            w[0] = 0;
            for (uint32_t c = 0; c < sp.colorAttachmentCount && m < 150; c++) {
                const uint32_t a = sp.pColorAttachments[c].attachment;
                m += snprintf(w + m, sizeof(w) - m, "%s%s", m ? "," : "",
                              a == VK_ATTACHMENT_UNUSED ? "-" : "");
                if (a != VK_ATTACHMENT_UNUSED)
                    m += snprintf(w + m, sizeof(w) - m, "%u", a);
            }
            char in[96];
            int k = 0;
            in[0] = 0;
            for (uint32_t c = 0; c < sp.inputAttachmentCount && k < 60; c++) {
                const uint32_t a = sp.pInputAttachments[c].attachment;
                if (a != VK_ATTACHMENT_UNUSED)
                    k += snprintf(in + k, sizeof(in) - k, "%s%u", k ? "," : "",
                                  a);
            }
            const uint32_t da =
                sp.pDepthStencilAttachment ? sp.pDepthStencilAttachment->attachment
                                           : VK_ATTACHMENT_UNUSED;
            char ds[16];
            if (da == VK_ATTACHMENT_UNUSED)
                snprintf(ds, sizeof ds, "none");
            else
                snprintf(ds, sizeof ds, "%u", da);
            X4VR_LOG("rp #%u.%u writes — colour [%s] depth %s input [%s]",
                     serial, i, w, ds, in);
        }
    }

    g_variants.unsheared[rp] = std::move(unsheared);
    g_variants.canvas_pass[rp] = std::move(canvas);
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

    // The two paths the writer list above cannot see. A frame stage done by a
    // blit or a dispatch leaves no entry in it, and "no pass writes #100" is
    // only evidence of a non-draw merge if the non-draw calls are on record.
    // "0" here must not be readable as "measured, and nothing happened" when
    // the truth is "never measured". Both counters are gated on the inventory
    // flag, so with it off the only honest report is that there is none --
    // exactly the distinction the bindless survey lost a run to.
    // The passes that reach the screen, and what they sample. This is task #5's
    // whole question: the difference now exists in the per-eye images, and
    // something reads them one last time into a single-layer swapchain image.
    {
        std::lock_guard<std::mutex> lock(g_present_mu);
        X4VR_LOG("mv %s: present passes — %zu pass(es) draw into a swapchain "
                 "image", when, g_present_rps.size());
        for (uint32_t rp : g_present_rps) {
            auto f = g_rp_frag.find(rp);
            // A pass with no pipeline on record is not a pass that draws
            // nothing -- it is one we never saw a pipeline for. Say which.
            if (f == g_rp_frag.end() || f->second.empty()) {
                X4VR_LOG("mv %s: present rp #%u — no fragment pipeline recorded",
                         when, rp);
                continue;
            }
            for (const auto &m : f->second)
                X4VR_LOG("mv %s: present rp #%u <- frag module #%u samples %s"
                         " [index-offset %s]",
                         when, rp, m.second.module, m.second.samplers,
                         m.second.patched ? "APPLIED" : "NOT APPLIED");
        }

        // The same join, for every pass rather than only the ones that reach
        // the screen. The map was already being filled for all of them -- only
        // the present subset was ever printed, so "which shader writes this
        // image" stayed a search through 409 dumps when the layer had the
        // answer in hand.
        //
        // Read this together with the `fb rp #N` lines: those give the pass its
        // attachment images, this gives it its shaders, and the pair is what
        // turns an image serial into a module to disassemble. The module number
        // is the dump's `mod-%04u.spv` from the *same run*; serials are per-run,
        // so a number from one log must never be looked up in another's dumps.
        X4VR_LOG("mv %s: pass -> shader join — %zu pass(es) with a fragment "
                 "pipeline on record", when, g_rp_frag.size());
        for (const auto &p : g_rp_frag)
            for (const auto &m : p.second)
                X4VR_LOG("mv %s: rp #%u <- frag module #%u (mod-%04u.spv) "
                         "samples %s [index-offset %s]",
                         when, p.first, m.second.module, m.second.module,
                         m.second.samplers,
                         m.second.patched ? "APPLIED" : "NOT APPLIED");
    }

    if (!g_mv_inventory) {
        X4VR_LOG("mv %s: image transfers and compute not measured "
                 "(needs X4VR_MV_INVENTORY=1)", when);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_xfer_mu);
        X4VR_LOG("mv %s: image transfers — %zu image->image edge(s); "
                 "%llu buffer->image upload region(s) to %zu image(s)",
                 when, g_xfer_edges.size(),
                 (unsigned long long)g_xfer_uploads,
                 g_xfer_upload_targets.size());
        for (const auto &e : g_xfer_edges) {
            char src[16];
            if (e.first.first == UINT32_MAX)
                snprintf(src, sizeof src, "buf");
            else
                snprintf(src, sizeof src, "#%u", e.first.first);
            for (const auto &k : e.second)
                X4VR_LOG("mv %s: xfer %s -> #%u via %s — %llu region(s), "
                         "%llu widened",
                         when, src, e.first.second, k.first,
                         (unsigned long long)k.second.n,
                         (unsigned long long)k.second.widened);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_comp_mu);
        X4VR_LOG("mv %s: compute — %llu pipeline(s), %zu shader(s) dispatched",
                 when, (unsigned long long)g_comp_pipelines,
                 g_comp_dispatches.size());
        for (const auto &e : g_comp_dispatches)
            X4VR_LOG("mv %s: compute module #%u — %llu dispatch(es)", when,
                     e.first, (unsigned long long)e.second);
    }
}

// Was a canvas asked for? Read from the environment, not from whether one was
// built, so the report can tell "asked for and not built" from "never asked
// for" -- the same distinction bindless_report exists to preserve.
bool canvas_wanted() {
    static const bool on = [] {
        const char *e = getenv("X4VR_CANVAS_M");
        return e && *e;
    }();
    // Task #40: the off-axis affine asks for a canvas whether or not a
    // distance was named. Reading only X4VR_CANVAS_M would leave the report
    // silent in exactly the run that most needs it -- a canted run whose UI is
    // either being mapped or diverging by 30 degrees, with nothing in the log
    // to say which. Non-latching: this is a report, and latching the target
    // here would fix it at whichever module happened to be compiled first.
    return on || g_offaxis_on.load(std::memory_order_relaxed);
}

// Task #30. Silent when no canvas was asked for; otherwise printed whatever
// the numbers are, zeros included. The failure this is built to catch is
// "variants built, nothing swapped": the modules exist, no pipeline ever took
// one, and the frame is byte-for-byte the old mono UI -- which is exactly what
// a correct frame looks like from the outside.
void canvas_report(const char *when) {
    if (!canvas_wanted())
        return;
    uint64_t swapped;
    {
        std::lock_guard<std::mutex> lock(g_variants.mu);
        swapped = g_variants.canvas_swapped;
    }
    // The "nothing drew" clause is a *final* verdict only. At first present X4
    // has typically compiled no UI shaders yet -- take 98 reported 0 built and
    // 0 swapped there and 348/18 at teardown -- so raising the alarm that
    // early cries wolf on every healthy run, which is how a real alarm stops
    // being read.
    const bool final = strstr(when, "final") != nullptr;
    X4VR_LOG("canvas %s: %llu variant(s) built, %llu REFUSED, swapped into "
             "%llu pipeline stage(s)%s",
             when, (unsigned long long)g_canvas_built.load(),
             (unsigned long long)g_canvas_refused.load(),
             (unsigned long long)swapped,
             (final && !swapped)
                 ? " — NOTHING DREW ON THE CANVAS; the UI is still mono"
                 : "");
    // Task #40's half-applied state, named the way #39's is. With the affine
    // on and nothing drawn on the canvas, screen-locked content is still in
    // X4's symmetric frame while the declaration is canted, which asks the
    // eyes to diverge by the frusta's separation. Unlike #39's case this one
    // is about coverage rather than a knob, so it is keyed on the swap count
    // -- the number stage4 already identified as "the failure that looks like
    // success".
    if (final && g_offaxis_latched.load(std::memory_order_acquire) &&
        g_offaxis_on.load(std::memory_order_relaxed) && !swapped)
        X4VR_LOG("canvas %s: off-axis affine NOT applied to screen-locked "
                 "draws — the declaration is canted and the UI is still in "
                 "X4's symmetric frame, so it sits at the frustum centre in "
                 "each eye and the two centres are mirrored. Nobody fuses "
                 "that. Needs X4VR_STEREO=1 and a pass that classifies as a "
                 "canvas.",
                 when);
}

// Reported separately from mv_report, and *not* gated on g_mv.
//
// It was, briefly, and that made X4VR_BINDLESS_SURVEY=1 print absolutely
// nothing unless multiview happened to be on too -- a knob that silently does
// nothing is a wasted live run. It also made the suite's negative case pass
// vacuously: with no output at all, "found no per-eye slots" and "never ran"
// were the same string.
void bindless_report(const char *when) {
    if (!g_bindless_survey && !g_bindless_mirror)
        return;
    std::lock_guard<std::mutex> dlock(g_desc_mu);
    if (g_bindless_mirror) {
        // Printed first and unconditionally, zeros included: a mirror that
        // quietly did nothing must not look like a mirror that cost nothing.
        X4VR_LOG("bindless mirror %s: offset %u, %llu twin writes, %llu twin "
                 "descriptors, %llu of them layer-1, %llu kept at layer 0 as "
                 "shared (unmasked writers only), %llu skipped for no room%s",
                 when, g_mirror_offset, (unsigned long long)g_mirror_writes,
                 (unsigned long long)g_mirror_descriptors,
                 (unsigned long long)g_mirror_layer1,
                 (unsigned long long)g_mirror_shared,
                 (unsigned long long)g_mirror_no_room,
                 g_mirror_collided ? " — DISABLED, see above" : "");
        // The take-74 blind spot, now measurable. "taken" counts slots filled
        // before the image's writers were knowable; "repaired" counts the ones
        // later put back to layer 0. A large taken with a zero repaired means
        // the race exists and the repair never fired -- a different failure
        // from the race not existing at all, and the two must not read alike.
        {
            std::lock_guard<std::mutex> plock(g_prov_mu);
            size_t pending = 0;
            for (const auto &e : g_mirror_provisional)
                pending += e.second.size();
            X4VR_LOG("bindless mirror %s: %llu slot(s) undecided, %llu of them "
                     "substituted and recorded, %llu repaired to layer 0, %zu "
                     "still provisional across %zu image(s)%s",
                     when, (unsigned long long)g_mirror_unknown_seen,
                     (unsigned long long)g_mirror_provisional_taken,
                     (unsigned long long)g_mirror_repaired, pending,
                     g_mirror_provisional.size(),
                     g_mirror_repair ? "" : " — REPAIR DISABLED");
        }
        for (const auto &e : g_dsl_sets)
            X4VR_LOG("bindless mirror %s: layout #%u — %u set(s) allocated",
                     when, e.first, e.second);
        // Refusals, alongside the successes. The count of patched shaders on
        // its own reads as coverage; it is not, and take twenty-four spent a
        // whole run unable to ask whether one particular shader was in it.
        const uint64_t ok = g_frag_patch_ok.load();
        const uint64_t refused = g_frag_patch_refused.load();
        uint64_t swapped;
        {
            std::lock_guard<std::mutex> lock(g_variants.mu);
            swapped = g_variants.swapped;
        }
        X4VR_LOG("bindless mirror %s: index-offset patch — %llu modules "
                 "edited, %llu declared a mirrorable table and REFUSED "
                 "(%llu of those are compute: no gl_ViewIndex exists there)",
                 when, (unsigned long long)ok, (unsigned long long)refused,
                 (unsigned long long)g_compute_tables.load());
        // Task #22. Offline, 244 of X4's 409 dumped modules take this and 3
        // refuse because they are compute -- so a count far below 244 means
        // the deferred passes are still lighting the wrong frame in whatever
        // this run did not reach.
        X4VR_LOG("invproj %s: per-eye M_invprojection — %llu modules corrected "
                 "(offline: 244 of 409 eligible, 3 compute cannot be)",
                 when, (unsigned long long)g_invproj_patched.load());
        // Printed on its own line, never folded into the total above. Offline,
        // exactly 2 of 385 fragment modules load member 4 -- #179 and #180, the
        // two shadowed deferred lights -- and that tiny number is the point:
        // member 2 feeds the view vector, member 4 feeds the CSM lookup, so a
        // run correcting 236 and 0 is the broken one and looks fine in a total.
        X4VR_LOG("invproj %s: per-eye M_invprojection_uj — %llu modules "
                 "corrected (offline: 2 of 385 fragment modules take it; these "
                 "are the shadow cascades)",
                 when, (unsigned long long)g_invproj_uj_patched.load());
        // Task #39. The affine on gl_Position and its undoing in the deferred
        // reconstruction are one change in two places, and the state where the
        // first happened and the second did not is a DEFECT, not a partial
        // improvement: 244 modules then light a frame nothing is in, by
        // opposite amounts in the two eyes.
        //
        // That state is reachable — X4VR_PROJ_INVPROJ=0 is a documented knob,
        // and the bindless mirror gates this patch — so it is named here
        // rather than left to be inferred from two numbers on different lines.
        // The latch itself cannot split the difference: one static object
        // serves both call sites, so a run is either affine everywhere or
        // affine nowhere.
        //
        // `latched` is the acquire side of the release store that publishes
        // the target, so reading it first is what makes `on` meaningful here;
        // reading `on` alone would be a relaxed load of a relaxed store.
        if (g_offaxis_latched.load(std::memory_order_acquire) &&
            g_offaxis_on.load(std::memory_order_relaxed)) {
            const bool undone = g_invproj_patched.load() > 0;
            X4VR_LOG("invproj %s: off-axis affine %s in the deferred "
                     "reconstruction%s",
                     when, undone ? "UNDONE" : "NOT undone",
                     undone ? " — the same latched target the vertex patches "
                              "baked, composed as T(d)·M_invprojection·A⁻¹"
                            : " — HALF APPLIED: gl_Position carries the affine "
                              "and M_invprojection does not, so every deferred "
                              "pass reconstructs the wrong frame. Set "
                              "X4VR_PROJ_INVPROJ=1, or turn the affine off "
                              "with X4VR_OFFAXIS=off.");
        }
        // Reported unconditionally, including the 0. "Fog still on" and "fog
        // disabled and it changed nothing" are opposite conclusions from the
        // same probe numbers, and a line that only appears when the knob fired
        // cannot tell them apart after the fact.
        X4VR_LOG("fog %s: volumetric composite forced to passthrough in %llu "
                 "module(s) (offline: 8 of 409 match the fog signature)",
                 when, (unsigned long long)g_fog_disabled.load());
        // The unsheared twin is the module the srgb-resolve passes actually
        // run. If this is 0, no pipeline ever took one and the twin's contents
        // are irrelevant -- a distinction take twenty-three could not draw.
        X4VR_LOG("bindless mirror %s: unsheared twin swapped into %llu pipeline "
                 "stage(s)",
                 when, (unsigned long long)swapped);
    }
    if (!g_bindless_survey)
        return;
    X4VR_LOG("bindless %s: %llu image-descriptor writes, %llu of them after the "
             "first present",
             when, (unsigned long long)g_desc_writes,
             (unsigned long long)g_desc_late_writes);
    // Printed unconditionally, including the zero. "No template line" and
    // "templates never used" have to be different readings, or this becomes the
    // seventh instrument to be quietly wrong.
    X4VR_LOG("bindless %s: %llu template updates, %llu of them via a template "
             "carrying image descriptors%s",
             when, (unsigned long long)g_tmpl_updates,
             (unsigned long long)g_tmpl_image_updates,
             g_tmpl_image_updates
                 ? " — THE COUNTS ABOVE ARE INCOMPLETE, and a mirror hooking "
                   "vkUpdateDescriptorSets alone would miss these"
                 : "");
    // Per binding: how much of the table X4 uses, and how much of it the twin
    // region would have to cover. The gap between the two is the whole reason
    // the offset can be a constant.
    for (const auto &e : g_desc_slots) {
        uint32_t lo = UINT32_MAX, hi = 0;
        for (uint32_t s : e.second) {
            lo = s < lo ? s : lo;
            hi = s > hi ? s : hi;
        }
        auto pe = g_desc_pe.find(e.first);
        char lid[16];
        desc_layout_label(e.first, lid, sizeof(lid));
        X4VR_LOG("bindless %s: layout %s binding %u — %zu distinct slots, "
                 "range %u..%u, %zu holding a per-eye image",
                 when, lid, (uint32_t)e.first, e.second.size(), lo, hi,
                 pe == g_desc_pe.end() ? (size_t)0 : pe->second.size());
    }
    // The slots that matter, named with their image serial so the result joins
    // straight onto the frame graph. If this list is empty the doubled images
    // are reaching the shaders by some route other than a sampled descriptor,
    // and the index-offset plan is built on sand.
    // Take twenty-one printed 26 of 191 entries here and said nothing about
    // stopping, so the shape of the set -- do the per-eye slots cluster at the
    // top of the used prefix? -- had to be *inferred* from an arbitrary sample.
    // A summary of a set must report the set's extent and admit when it
    // truncates; otherwise a partial list reads exactly like a complete one.
    for (const auto &b : g_desc_pe) {
        char list[400];
        int n = 0;
        size_t shown = 0;
        uint32_t lo = UINT32_MAX, hi = 0;
        list[0] = 0;
        for (const auto &s : b.second) {
            lo = s.first < lo ? s.first : lo;
            hi = s.first > hi ? s.first : hi;
            if (n < 320) {
                n += snprintf(list + n, sizeof(list) - n, "%s%u=img#%u",
                              n ? " " : "", s.first, s.second);
                shown++;
            }
        }
        char lid[16];
        desc_layout_label(b.first, lid, sizeof(lid));
        X4VR_LOG("bindless %s: layout %s binding %u per-eye slots: %zu in "
                 "%u..%u, showing %zu: %s",
                 when, lid, (uint32_t)b.first, b.second.size(), lo, hi, shown,
                 list);
    }
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

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkDescriptorSetLayout *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    // Q1: is the array really 53306 descriptors long, or is it declared large
    // in the shader and bound short here? A short binding means the layer would
    // have to widen this layout and the pool behind it -- object surgery, not
    // write mirroring -- so it is the fact that decides the size of the job.
    const VkResult r = d->CreateDescriptorSetLayout(device, ci, ac, out);
    if (r != VK_SUCCESS || !g_desc_track || !ci)
        return r;

    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bf = nullptr;
    for (const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext; p;
         p = p->pNext)
        if (p->sType ==
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            bf = (const VkDescriptorSetLayoutBindingFlagsCreateInfo *)p;
    // Serial first, so every table binding below is logged under the identity
    // the write survey will report it by.
    uint32_t lid = UINT32_MAX;
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (ci->pBindings[i].descriptorCount > 1) {
            std::lock_guard<std::mutex> dlock(g_desc_mu);
            lid = g_dsl_id[*out] = g_dsl_serial++;
            auto &counts = g_dsl_counts[lid];
            for (uint32_t k = 0; k < ci->bindingCount; k++)
                counts[ci->pBindings[k].binding] =
                    ci->pBindings[k].descriptorCount;
            break;
        }
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding &b = ci->pBindings[i];
        if (b.descriptorCount <= 1)
            continue; // only the tables are interesting
        const VkDescriptorBindingFlags fl =
            (bf && i < bf->bindingCount) ? bf->pBindingFlags[i] : 0;
        X4VR_LOG("bindless: layout #%u binding %u type=%u count=%u "
                 "flags=0x%x%s%s%s",
                 lid, b.binding, (unsigned)b.descriptorType, b.descriptorCount,
                 (unsigned)fl,
                 (fl & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)
                     ? " VARIABLE" : "",
                 (fl & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT)
                     ? " PARTIALLY_BOUND" : "",
                 (fl & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)
                     ? " UPDATE_AFTER_BIND" : "");
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_AllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *ai,
    VkDescriptorSet *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    // The other half of Q1. With a VARIABLE binding the layout's count is only
    // a maximum; the real size is chosen here, per set.
    if (g_bindless_survey && ai) {
        for (const VkBaseInStructure *p = (const VkBaseInStructure *)ai->pNext;
             p; p = p->pNext)
            if (p->sType ==
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
                const auto *v =
                    (const VkDescriptorSetVariableDescriptorCountAllocateInfo *)p;
                for (uint32_t i = 0; i < v->descriptorSetCount; i++)
                    X4VR_LOG("bindless: allocated variable count %u",
                             v->pDescriptorCounts[i]);
            }
    }
    const VkResult r = d->AllocateDescriptorSets(device, ai, out);
    // Bind each new set to the layout it came from, so a write can be attributed
    // to a table rather than to a bare binding number.
    if (r == VK_SUCCESS && g_desc_track && ai && out) {
        std::lock_guard<std::mutex> dlock(g_desc_mu);
        for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
            auto it = g_dsl_id.find(ai->pSetLayouts[i]);
            if (it != g_dsl_id.end()) {
                g_ds_layout[out[i]] = it->second;
                g_dsl_sets[it->second]++;
            }
        }
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkDescriptorUpdateTemplate *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    const VkResult r = d->CreateDescriptorUpdateTemplate(device, ci, ac, out);
    if (r != VK_SUCCESS || !g_bindless_survey || !ci)
        return r;

    std::vector<uint32_t> img;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++) {
        const VkDescriptorUpdateTemplateEntry &e = ci->pDescriptorUpdateEntries[i];
        if (e.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
            e.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            img.push_back(e.dstBinding);
    }
    if (!img.empty()) {
        char list[128];
        int n = 0;
        list[0] = 0;
        for (uint32_t b : img)
            if (n < 100)
                n += snprintf(list + n, sizeof(list) - n, "%s%u", n ? "," : "",
                              b);
        X4VR_LOG("bindless: update template carries image descriptors at "
                 "binding(s) %s — the survey's slot counts are incomplete if "
                 "this template is ever used",
                 list);
        std::lock_guard<std::mutex> dlock(g_desc_mu);
        g_tmpl_image_bindings[*out] = std::move(img);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL x4vr_UpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
    const void *data) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    if (g_bindless_survey) {
        std::lock_guard<std::mutex> dlock(g_desc_mu);
        g_tmpl_updates++;
        if (g_tmpl_image_bindings.count(tmpl))
            g_tmpl_image_updates++;
    }
    d->UpdateDescriptorSetWithTemplate(device, set, tmpl, data);
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
        // This is also the first moment the mirror can be told the truth about
        // an image. Collect the newly-shared ones under the lock and repair
        // them outside it: repair takes g_prov_mu and g_desc_mu, and taking
        // those under g_img_mu would invert the order the mirror path uses.
        std::vector<uint32_t> now_shared, now_per_eye;
        {
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
                if (k.masked.empty() && !k.unmasked.empty())
                    now_shared.push_back(im->second.serial);
                else if (!k.masked.empty())
                    now_per_eye.push_back(im->second.serial);
            }
        }
        if (g_mirror_repair) {
            for (uint32_t s : now_shared)
                repair_mirror_for_image(d, device, s);
            // A masked writer confirms the guess the mirror already made, so
            // the record is just dead weight from here on. Dropping it also
            // keeps the map from growing for the lifetime of the process.
            if (!now_per_eye.empty()) {
                std::lock_guard<std::mutex> plock(g_prov_mu);
                for (uint32_t s : now_per_eye)
                    g_mirror_provisional.erase(s);
            }
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

    // Record which passes reach the screen, whether or not the inventory is on:
    // this is the join task #5 turns on, and gating it on a debug flag would
    // make the layer's own behaviour depend on whether we were watching.
    if (r == VK_SUCCESS) {
        bool presents = false;
        {
            std::lock_guard<std::mutex> lock(g_img_mu);
            for (uint32_t i = 0; i < ci->attachmentCount && !presents; i++) {
                auto v = g_views.find(ci->pAttachments[i]);
                if (v == g_views.end())
                    continue;
                auto im = g_images.find(v->second.image);
                if (im != g_images.end() && im->second.swapchain)
                    presents = true;
            }
        }
        if (presents) {
            std::lock_guard<std::mutex> lock(g_present_mu);
            g_present_rps.insert(serial);
        }
    }

    if (r == VK_SUCCESS && g_mv_inventory && g_active) {
        char imgs[256];
        int n = 0;
        imgs[0] = 0;
        bool sc = false;
        {
            std::lock_guard<std::mutex> lock(g_img_mu);
            for (uint32_t i = 0; i < ci->attachmentCount && n < 230; i++) {
                auto v = g_views.find(ci->pAttachments[i]);
                auto im = v == g_views.end() ? g_images.end()
                                             : g_images.find(v->second.image);
                if (im == g_images.end())
                    n += snprintf(imgs + n, sizeof(imgs) - n, "%s?",
                                  n ? "," : "");
                else {
                    n += snprintf(imgs + n, sizeof(imgs) - n, "%s#%u",
                                  n ? "," : "", im->second.serial);
                    sc = sc || im->second.swapchain;
                }
            }
        }
        X4VR_LOG("fb  rp #%u: %ux%u layers=%u attachments=%u imgs=[%s]%s%s",
                 serial, ci->width, ci->height, ci->layers,
                 ci->attachmentCount, imgs, sc ? " PRESENTS" : "",
                 masked ? " MASKED" : "");
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
        g_variants.canvas_pass.erase(rp);
        g_rp_serials.erase(rp);
        g_masked_passes.erase(rp);
        g_srgb_resolve_passes.erase(rp);
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
// X4VR_DUMP_SHADERS=<dir>: write every module X4 creates, and remember which
// serial each handle got.
//
// The fragment patch has to be told which (set, binding) carries the doubled
// image, and X4's tonemap shader has never been read. Dumping all ~1300 modules
// is useless on its own -- the point is the join below, where the pipelines
// built against the tonemap pass name the two serials worth disassembling.
const char *g_dump_shaders = getenv("X4VR_DUMP_SHADERS");
std::mutex g_mod_mu;
std::unordered_map<VkShaderModule, uint32_t> g_mod_serial;
std::unordered_map<VkShaderModule, std::vector<x4vr::spv::SampledTexture>>
    g_mod_samplers;
uint32_t g_mod_next = 0;
// Which modules took the index-offset edit. Take twenty-four: the log said
// "patched fragment shader #300" and nothing about the ones that refused, so
// when #103 stayed mono there was no way to ask whether its own shader was
// among them. A running count is not an identity.
std::unordered_set<VkShaderModule> g_mod_frag_patched;

void record_module(const VkShaderModuleCreateInfo *ci, VkShaderModule mod) {
    if ((!g_dump_shaders || !*g_dump_shaders) && !g_mv_inventory)
        return;
    // The texture list is what the pipeline log prints; the file is for when
    // that is not enough and the module has to be disassembled.
    std::vector<uint32_t> code(ci->codeSize / 4);
    memcpy(code.data(), ci->pCode, ci->codeSize);
    auto tex = x4vr::spv::list_sampled_textures(code);
    uint32_t serial;
    {
        std::lock_guard<std::mutex> lock(g_mod_mu);
        serial = g_mod_next++;
        g_mod_serial[mod] = serial;
        if (!tex.empty())
            g_mod_samplers[mod] = std::move(tex);
    }
    if (!g_dump_shaders || !*g_dump_shaders)
        return;
    char path[512];
    snprintf(path, sizeof path, "%s/mod-%04u.spv", g_dump_shaders, serial);
    FILE *f = fopen(path, "wb");
    if (!f) {
        // Once, not per module: a missing directory would otherwise produce
        // one line per shader and bury the run.
        static bool warned = false;
        if (!warned) {
            warned = true;
            X4VR_LOG("WARNING: X4VR_DUMP_SHADERS=%s is not writable",
                     g_dump_shaders);
        }
        return;
    }
    fwrite(ci->pCode, 1, ci->codeSize, f);
    fclose(f);
}

// The projection terms the eye shear is baked from.
//
// One definition each, because the shear is built in two places (X4VR_EYE and
// X4VR_STEREO) and checked in a third (the live-projection dump). Three copies
// of a literal default drift apart precisely when the value turns out to be
// wrong, which is the case these exist to diagnose.
//
// The defaults are the Phase 4a measurements, taken at 2816x1408. They are not
// guesses -- but sx carries the aspect, and the eye is no longer that shape.
// See read_proj_terms() and X4VR_DUMP_MATRICES.
float assumed_proj_sx() {
    static const float v = getenv("X4VR_PROJ_SX")
                               ? strtof(getenv("X4VR_PROJ_SX"), nullptr)
                               : 0.889f;
    return v;
}
float assumed_proj_near() {
    static const float v = getenv("X4VR_PROJ_NEAR")
                               ? strtof(getenv("X4VR_PROJ_NEAR"), nullptr)
                               : 0.1f;
    return v;
}

// The interpupillary distance in metres. Ours, not X4's -- this is the one
// number in the shear that is a choice rather than a measurement, which is
// exactly why it stays a baked constant when sx no longer can.
float configured_ipd() {
    static const float v =
        getenv("X4VR_IPD") ? strtof(getenv("X4VR_IPD"), nullptr) : 0.064f;
    return v;
}

// X4VR_PROJ_LIVE: read sx from X4's camera block in the shader instead of
// baking it. Off by default until a run proves it, so the tagged state is one
// unset variable away.
bool proj_live() {
    static const bool on = [] {
        const char *e = getenv("X4VR_PROJ_LIVE");
        return e && *e && *e != '0';
    }();
    return on;
}

// X4VR_PROJ_MVP: for the World modules that declare no camera block, recover
// sx from the per-object M_worldviewprojection rather than shearing them by a
// baked constant that is only right at one zoom level. Requires
// X4VR_PROJ_LIVE, because it is the same fallback chain -- it only ever runs
// where patch_vertex_eye_offset has already refused.
//
// ON by default since takes 109/110, which were the same scenario one variable
// apart. Every checkpoint of the two runs was identical except the split it
// controls -- live-sx=326 in both, mvp-sx 0 -> 12, baked-sx 12 -> 0 -- so all
// twelve modules moved, none was left behind, and no module that could already
// read the camera was touched. Zero driver rejections, and frame time differed
// by about 1% with the sign disagreeing between phases.
//
// X4VR_PROJ_MVP=0 turns it off, which restores the stage5-wide-field behaviour
// exactly: those twelve go back to the baked constant.
//
// Note this is gated on proj_live() at the call site, which is still off by
// default -- so "on" here means "on wherever the per-draw shear path is on",
// not "on in every run".
bool proj_mvp() {
    static const bool on = x4vr::env_on("X4VR_PROJ_MVP", true);
    return on;
}

// Task #35. THE frustum pair — the one the affine is baked for and the one the
// compositor is told about. Those two must be the same object, and that is the
// whole content of piece 2.
//
// **Correctness needs them equal to each other, not equal to the runtime's.**
// If the affine remaps X4's field into frustum F and we declare F, the picture
// is right whatever F is; the compositor resamples. Matching the runtime's own
// F is a quality choice — it makes the resample the identity — not a
// correctness one. Saying that plainly matters because it is what makes the
// fallback below safe rather than a silent half-measure.
//
// **Latched once, on first use, and never revisited.** The coefficients are
// baked into shader modules at vkCreateShaderModule, and X4 creates world
// modules over a 77-second stretch (take 163: first at t+0.8 s, still arriving
// at t+78 s). A target that could change between two of those calls would put
// half the world in one frustum and half in another — a defect that no single
// frame would look wrong enough to explain.
//
// Sources, in order:
//   1. X4VR_OFFAXIS="l,r,u,d" in degrees, eye 0, mirrored for eye 1 — an
//      OVERRIDE, which is what every other env knob in this layer is, and the
//      flatscreen measurement path that proved the emission in takes 164a/b/c.
//      X4VR_OFFAXIS="-55,55,55,-55" at X4VR_FOV=1.4917 is arithmetically the
//      identity, which is that measurement's negative control.
//   2. the runtime's located views (deployment)
//   3. off
//
// **The knob outranks the runtime deliberately.** The other order reads better
// -- prefer the real hardware -- but it makes X4VR_OFFAXIS silently dead in
// every VR run, so a run set up to force a target would measure the runtime's
// instead and nothing in the picture would say which. A knob whose null
// refutes only itself has cost this project takes before. When both are
// present the override is announced next to the value it displaced.
//
// Whichever wins is logged by name, because "the affine ran" and "the affine
// ran on the runtime's real frusta" are different claims and a run has to be
// able to tell them apart.
struct OffAxisPair {
    x4vr::OffAxis eye[2];
    XrFovf fov[2] = {};        // what we declare; same source as the affine
    float half_needed = 0.0f;  // union half-angle of the pair, radians
    const char *source = "off";
    bool on = false;
};

// Defined with the VR state further down. Fills `out[eye] = {l, r, u, d}` in
// radians from the first located frame and returns false until there is one.
bool vr_located_fov(float out[2][4]);
// True while waiting for that first located view could still succeed: this run
// wants VR, a session is intended or already running, nothing has told it to
// stop, and no view has been located yet. Without it the wait below would burn
// its whole cap on a machine with no runtime at all.
bool vr_awaiting_first_view();
// Spawns the session thread if a device was noted and it has not been spawned
// yet. Idempotent. Normally reached from vkGetDeviceQueue; the latch below has
// to be able to reach it too — see the note there.
void vr_start_session_deferred();
// Has the session thread been spawned? Needed here only so the log line below
// can report what it actually did rather than what it was about to try.
bool vr_session_thread_started();

// Non-latching. The per-module log line wants to *describe* the target, and
// calling the latching accessor there would fix it at the first NONWORLD
// module — which take 163 timestamps 12 ms BEFORE the first located frame, so
// the runtime would lose the race every single run without anything saying so.
std::atomic<bool> g_offaxis_latched{false};
std::atomic<bool> g_offaxis_on{false};

const OffAxisPair &offaxis_target() {
    static const OffAxisPair p = [] {
        OffAxisPair r{};
        float a[2][4] = {};
        float rt[2][4] = {};
        const char *s = getenv("X4VR_OFFAXIS");
        const bool want_off = s && (!strcmp(s, "off") || !strcmp(s, "0"));
        const bool want_rt = !want_off && s &&
                             (!strcmp(s, "runtime") || !strcmp(s, "auto"));

        // **Take 166b lost this race by ten milliseconds, and it was my
        // change that lost it.**
        //
        // The target is latched once, at the first shader module that needs
        // one, because a module's coefficients are baked at
        // vkCreateShaderModule and cannot be revised afterwards. Task #39 gave
        // the deferred-reconstruction patch the same latched target — which is
        // right, the two ends of one map must not be sourced separately — but
        // that call site runs EARLIER than the vertex one. Early enough, it
        // turned out, that X4 was compiling shaders before the XrSession
        // existed at all:
        //
        //     230978.330  first shader needing a target -> latched OFF
        //     230978.335  xrCreateSession returns
        //     230978.340  first located view
        //
        // Take 165b latched at the first WORLD vertex module and had 0.8 s of
        // margin. #39 spent it, and I checked the wrong pair of timestamps
        // before shipping: "first fragment patch" is the index-offset one, not
        // this.
        //
        // Waiting is the honest repair. The alternative — let early modules go
        // unmapped and map the later ones — puts some of the world in one
        // frustum and the rest in another, which is take 165b's failure with
        // extra steps. The decision has to be the same for every module in the
        // process, so the only question is whether it is made too early.
        //
        // **Take 167b: the wait deadlocked on itself, and the log said so to
        // the millisecond.**
        //
        //     233213.887  offaxis: waited 5000 ms — no view was located
        //     233213.888  vr: physical device …      <- the session thread's
        //     233213.889  vr: session created           FIRST line, 1 ms later
        //     233213.893  located=1
        //
        // I had written here that this was "deadlock-safe by construction: the
        // session runs on its own thread, spawned from vkGetDeviceQueue long
        // before any shader module". That premise is false, and take 166b's
        // log already showed it false — the thread's first line lands AFTER
        // the first shader module, because X4 has not called vkGetDeviceQueue
        // yet. So the wait sat in front of the very call that would have
        // started the session, and burned its whole cap. Second time on this
        // one question that I asserted an ordering I had the data to check.
        //
        // Hence: start the session here, then wait. vr_start_session_deferred
        // is idempotent and does nothing but spawn the thread, and this is not
        // inside the loader's device-creation chain — which is the one place
        // VrState says it must not be called from.
        //
        // The cap stays, and it is the honest part of this: two premises about
        // what the session thread needs have now been wrong, so a third that
        // costs five seconds once and logs itself is better than one that
        // could hang. The wait is skipped entirely unless this run asked for
        // the runtime's frusta.
        if (want_rt && !vr_located_fov(rt)) {
            // Logged as a pair with the wait below, so the next log shows the
            // causality instead of leaving it to be re-derived from
            // timestamps -- which is how the last two attempts went wrong.
            // What actually happened, not what was about to be attempted.
            // The first version of this line asked vr_awaiting_first_view()
            // BEFORE the call and reported that — which is true whenever a
            // session is pending, including when the thread was already
            // running. The bring-up suite then grepped for the line and
            // passed with the spawn deleted. A log line that cannot say "no"
            // is not evidence, and it made a test that could not fail.
            const bool was_started = vr_session_thread_started();
            vr_start_session_deferred();
            const bool spawned_here = !was_started && vr_session_thread_started();
            X4VR_LOG("offaxis: the first shader needed a target and no view "
                     "was located yet; %s",
                     spawned_here
                         ? "STARTED THE VR SESSION FROM HERE, because X4 had "
                           "not called vkGetDeviceQueue yet and the wait would "
                           "otherwise block the call that starts it"
                     : vr_session_thread_started()
                         ? "the session thread was already running, so just "
                           "waiting for it"
                         : "no session is pending, so waiting cannot help");
            // Not a knob. A run's behaviour must not depend on a timeout
            // someone tuned, and 10 ms is the number this exists to cover;
            // five seconds is slack for a headset still waking up.
            constexpr int kCapMs = 5000;
            const auto t0 = std::chrono::steady_clock::now();
            int waited = 0;
            while (waited < kCapMs && vr_awaiting_first_view()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                waited = (int)std::chrono::duration_cast<
                             std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
            }
            X4VR_LOG("offaxis: waited %d ms at the first shader that needed a "
                     "target — %s",
                     waited,
                     vr_located_fov(rt)
                         ? "the runtime located a view, so the affine is built "
                           "from real frusta"
                         : "no view was located; see the verdict below");
        }
        const bool have_rt = vr_located_fov(rt);
        // **X4VR_OFFAXIS=off, and why it has to exist.** Source 2 turns the
        // affine on by itself in any VR run, which changes what every command
        // line written before this commit does. This project's rule is that a
        // known-good state is code AND knobs, and X4VR_PROJ_INVPROJ already
        // cost it once: omitting a variable and setting it to 0 stopped being
        // the same thing the moment the default changed, and a documented
        // control silently became a non-control. So there is an explicit off,
        // it outranks the runtime, and it is what reproduces take 163 and
        // every VR take before it.
        if (want_off) {
            X4VR_LOG("offaxis: OFF by request (X4VR_OFFAXIS=%s) — X4's "
                     "symmetric field is rendered and declared, which is the "
                     "behaviour of every VR take up to 163. Any located "
                     "runtime frusta are ignored.",
                     s);
            g_offaxis_latched.store(true, std::memory_order_release);
            return r;
        }
        // **The runtime source is OPT-IN, and take 165b is why.**
        //
        // It was the default for exactly one take. The affine is correct for
        // the geometry it touches and there is a large set it does not touch --
        // every NonWorld module, every unsheared twin, and the deferred
        // reconstruction -- and declaring the canted frusta puts that set in a
        // different frame from the declaration. For anything screen-locked
        // that is a 30.07 deg divergence between the eyes, which is the
        // unfusable negative control x4vr_view.hpp:291 already describes.
        // Shipping it on by default means shipping that.
        if (want_rt && have_rt) {
            memcpy(a, rt, sizeof(a));
            r.source = "the runtime's located views";
        } else if (want_rt) {
            // "the first shader that needed a target", not "the first world
            // shader": since task #39 the deferred-reconstruction patch asks
            // for it too, and on X4's combined modules that one can run first.
            // Which call site wins does not change the outcome -- one static
            // serves both, so the whole session is on or off together -- but
            // a message naming the wrong caller would send the next reader
            // looking in the wrong place.
            X4VR_LOG("offaxis: OFF — X4VR_VR=1 but no view had been located "
                     "when the first shader that needed a target was patched, "
                     "and X4VR_OFFAXIS=%s asked for the runtime's frusta. X4's "
                     "symmetric field is rendered and declared, which is "
                     "self-consistent, not half-applied.",
                     s);
            g_offaxis_latched.store(true, std::memory_order_release);
            return r;
        } else if (s && *s) {
            float v[4] = {0, 0, 0, 0};
            int n = 0;
            for (const char *q = s; *q && n < 4; n++) {
                char *end = nullptr;
                v[n] = strtof(q, &end);
                if (end == q)
                    break;
                q = end;
                if (*q == ',')
                    q++;
            }
            if (n != 4) {
                X4VR_LOG("offaxis: REFUSED — X4VR_OFFAXIS=\"%s\" is not four "
                         "comma-separated degree values (l,r,u,d); no affine "
                         "applied",
                         s);
                g_offaxis_latched.store(true, std::memory_order_release);
                return r;
            }
            const float k = 3.14159265358979f / 180.0f;
            // The mirror, spelled out rather than assumed: eye 1's left edge
            // is the negation of eye 0's right edge, and vice versa. The
            // vertical is shared -- a headset cants its eyes horizontally.
            // This assumption belongs to the KNOB only; source 1 reads all
            // eight angles from the runtime and assumes nothing.
            for (int i = 0; i < 4; i++)
                a[0][i] = v[i] * k;
            a[1][0] = -v[1] * k;
            a[1][1] = -v[0] * k;
            a[1][2] = v[2] * k;
            a[1][3] = v[3] * k;
            r.source = "X4VR_OFFAXIS";
            // Say what was displaced, in the same units, so a run cannot
            // quietly measure the knob while the reader believes it measured
            // the headset -- or the reverse.
            if (have_rt) {
                const float d = 57.2957795130823f;
                X4VR_LOG("offaxis: X4VR_OFFAXIS OVERRIDES the runtime. The "
                         "runtime reported eye0 l=%.2f r=%.2f u=%.2f d=%.2f "
                         "deg and this run forces l=%.2f r=%.2f u=%.2f d=%.2f. "
                         "The compositor is told the forced values, so the "
                         "picture is self-consistent but will not match the "
                         "optics.",
                         rt[0][0] * d, rt[0][1] * d, rt[0][2] * d, rt[0][3] * d,
                         v[0], v[1], v[2], v[3]);
            }
        } else {
            // Unset is OFF and silent. Not a complaint: after take 165b that
            // is the correct default for a VR run too, so there is nothing
            // here to warn about.
            g_offaxis_latched.store(true, std::memory_order_release);
            return r;
        }

        x4vr::EyeFrustum f[2];
        for (int e = 0; e < 2; e++) {
            f[e] = x4vr::frustum_of_angles(a[e][0], a[e][1], a[e][2], a[e][3]);
            r.eye[e] = x4vr::make_off_axis(f[e]);
            r.fov[e].angleLeft = a[e][0];
            r.fov[e].angleRight = a[e][1];
            r.fov[e].angleUp = a[e][2];
            r.fov[e].angleDown = a[e][3];
        }
        if (!r.eye[0].ok || !r.eye[1].ok) {
            X4VR_LOG("offaxis: REFUSED — %s describes a frustum with no width "
                     "or no height; no affine applied",
                     r.source);
            r.source = "off";
            g_offaxis_latched.store(true, std::memory_order_release);
            return r;
        }
        // The precondition, checked here rather than discovered from a null.
        // The affine divides by the LIVE sx and sy, so it can only ride on the
        // patches that read them; with X4VR_PROJ_LIVE unset every world module
        // takes the baked matrix instead and this would do nothing at all
        // while looking like it had been tested.
        if (!proj_live()) {
            X4VR_LOG("offaxis: REFUSED — the target is set (%s) but "
                     "X4VR_PROJ_LIVE is not. The affine divides by the live "
                     "sx/sy and only exists inside the patches that read them, "
                     "so nothing was applied. Re-run with X4VR_PROJ_LIVE=1",
                     r.source);
            r.source = "off";
            g_offaxis_latched.store(true, std::memory_order_release);
            return r;
        }
        r.on = true;
        r.half_needed = x4vr::union_half_angle(f, 2);
        const float k = 57.2957795130823f;
        X4VR_LOG("offaxis: target from %s — eye0 l=%.2f r=%.2f u=%.2f d=%.2f, "
                 "eye1 l=%.2f r=%.2f u=%.2f d=%.2f deg",
                 r.source, a[0][0] * k, a[0][1] * k, a[0][2] * k, a[0][3] * k,
                 a[1][0] * k, a[1][1] * k, a[1][2] * k, a[1][3] * k);
        X4VR_LOG("offaxis: eye0 ax_num=%.5f bx=%+.5f ay_num=%.5f by=%+.5f, "
                 "eye1 bx=%+.5f. A_x is ax_num/sx per draw, so at sx=%.5f this "
                 "reads A_x=%.4f",
                 r.eye[0].ax_num, r.eye[0].bx, r.eye[0].ay_num, r.eye[0].by,
                 r.eye[1].bx, assumed_proj_sx(),
                 r.eye[0].ax_num / assumed_proj_sx());
        // **The guard that can actually fail.** The affine re-projects what X4
        // drew; it cannot invent geometry X4 culled. If X4's own half-angle is
        // narrower than the union of the target frusta, the corners of the
        // headset's field are fed by nothing and go black — and that reads as
        // a compositor problem rather than as a field that was too small.
        // Derived from the latched frusta, so it is a real comparison and not
        // a restatement of the knob.
        const float need = x4vr::x4_fov_for_half(r.half_needed);
        const char *fe = getenv("X4VR_FOV");
        const float have_fov = fe ? strtof(fe, nullptr) : 1.0f;
        if (have_fov < need - 1e-3f)
            X4VR_LOG("offaxis: WARNING — the target needs a union half-angle "
                     "of %.2f deg, which is X4VR_FOV %.4f, but this run asks "
                     "for %.4f. X4 draws a field %.2f deg narrower than the "
                     "headset's, so the edges of each eye are fed by geometry "
                     "X4 never rendered and will be black",
                     r.half_needed * k, need, have_fov,
                     (need - have_fov) * 73.7399f * 0.5f);
        else
            X4VR_LOG("offaxis: X4VR_FOV %.4f covers the target's union "
                     "half-angle of %.2f deg (needs %.4f) — nothing the "
                     "headset can see is outside what X4 drew",
                     have_fov, r.half_needed * k, need);
        g_offaxis_on.store(true, std::memory_order_relaxed);
        g_offaxis_latched.store(true, std::memory_order_release);
        return r;
    }();
    return p;
}

// Where X4 keeps its camera constants. Read off the dumped modules:
// BLOCK_BUFFER_BINDING_SLOT_CAMERA, member 1 = M_projection (member 0 is
// M_view, 3 is M_projection_uj), matching x4vr_view.hpp's float offsets.
//
// Not env-configurable on purpose. patch_vertex_eye_offset verifies the shape
// before it emits anything -- a Uniform block whose member really is a mat4 --
// so a build of X4 that moved these would refuse and fall back to the baked
// matrix rather than read the wrong buffer. A knob here would let someone
// force a wrong answer past that check.
constexpr uint32_t kCameraSet = 1;
constexpr uint32_t kCameraBinding = 0;
constexpr uint32_t kCameraProjMember = 1;
// The per-object block, for the modules that have no camera block at all
// (task #23). Member 0 is M_worldviewprojection -- verified by name in the
// dumps and by shape in patch_vertex_eye_offset_mvp, which refuses unless the
// member really is a mat4. Not env-configurable, for the reason above: a knob
// here would let someone force a wrong buffer past that check.
constexpr uint32_t kObjectSet = 3;
constexpr uint32_t kObjectBinding = 0;
constexpr uint32_t kObjectMvpMember = 0;
constexpr uint32_t kCameraInvProjMember = 2; // M_invprojection
// M_invprojection_uj -- the *other* inverse projection, and the one that
// mattered. Reading mod-0180 (the sun light with cascaded shadows) shows the
// shader reconstructs position twice from the same vec4(ndc.xy, depth, 1):
//
//   A = member 2 * that, /w   -> used only for the view vector (specular)
//   B = member 4 * that, /w   -> multiplied by the five CSM matrices and fed
//                                to S_sampler2DShadow
//
// Correcting member 2 alone therefore fixed the term nobody can see and left
// the shadow lookup reading the centre camera's frame while gl_FragCoord and
// the depth buffer are the sheared eye's. That is a 3.2cm position error at
// the shadow lookup -- 36px at 0.83m, one full disparity -- so a surface is
// shadowed in one eye and lit in the other. Exactly two fragment modules load
// this member, #179 and #180, and both also load member 2.
constexpr uint32_t kCameraInvProjUjMember = 4; // M_invprojection_uj

// X4VR_PROJ_INVPROJ: correct M_invprojection *and* M_invprojection_uj per eye
// in the deferred passes (task #22). Separate from X4VR_PROJ_LIVE so the two
// can be bisected apart -- they touch different stages and different failure
// modes.
//
// **ON by default since take 83.** This stopped being an experiment when
// member 4 was added to it: without this the deferred shadow lookup reads the
// centre camera's frame while the depth buffer is the sheared eye's, and one
// eye is visibly under-lit. Leaving it off by default would mean the default
// build is the broken one. It is still gated on `have_k`, so a run with no
// shear is unaffected either way.
//
// Reproduction warning for every take before 83 in docs/frame-analysis.md:
// those ran with this OFF because they *omitted* the variable. Re-running one
// of those command lines against this build now gets the correction, so it
// will not reproduce. Add X4VR_PROJ_INVPROJ=0 to reproduce a pre-83 take.
// This is the same trap the take-50 control fell into with X4VR_STEREO, which
// this file already records -- omitting a variable and setting it to 0 stopped
// being the same thing the moment the default changed.
bool proj_invproj() {
    static const bool on = [] {
        const char *e = getenv("X4VR_PROJ_INVPROJ");
        return !e || !*e || *e != '0';
    }();
    return on;
}

// The patching itself. Wrapped below so the dump happens on every path out of
// it without four copies of the same two lines.
VkResult create_shader_module_inner(
    VkDevice device, const VkShaderModuleCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkShaderModule *out,
    bool *frag_patched_out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }

    // Two matrices, chosen per module by static classification:
    //   K_world    — world geometry (set-3 per-object block, or the camera
    //                block under wide_camera): gets the eye offset
    //   K_nonworld — every module that is not that: UI and HUD shaders, but
    //                also every fullscreen triangle and every procedural vertex
    //                shader. Identity by default. Giving these the world eye
    //                offset would not just misplace the HUD, it would corrupt
    //                every fullscreen post pass, so they must never share
    //                K_world.
    //
    // The name is deliberate: this set is *not* the HUD. On X4 it is 54 modules
    // of 350, and most of them are fullscreen triangles and procedural vertex
    // shaders. Reading it as "the matrix for the HUD" sent one investigation
    // (take 82) to a conclusion broader than the knob could support.
    //
    // **Setting K_nonworld often does nothing, and that is by design.** Two
    // independent gates gate the shear: this one picks *which matrix* a module
    // is patched with, and needs_original() decides whether the patched module
    // is bound at all. An unsheared pass -- depth-only shadow, all-LDR/UI, and
    // since take 71 any colour pass with no depth -- binds the *unpatched*
    // module whatever it was patched with. So K_nonworld only reaches a draw
    // whose module is NonWorld *and* whose pass is sheared.
    static bool have_k = false;
    static float K_world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static float K_nonworld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // The right eye's counterparts. Present only in stereo: when these are
    // unset the module is patched with one matrix and behaves exactly as it
    // did through Phase 4a, so the mono path is not a special case of the
    // stereo one but the same code with nothing to select between.
    static bool have_kr = false, have_kr_nonworld = false;
    static float K_world_r[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static float K_nonworld_r[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Task #30, the third transform. A constant clip-space x offset, opposite
    // per eye: `K·(x,y,z,w) = (x + s·w, y, z, w)`, whose NDC x is `x/w + s` for
    // *any* w. That is what makes it a canvas rather than geometry -- it does
    // not matter whether X4's UI matrix is an orthographic screen transform or
    // the map's perspective one, the shift is the same.
    //
    // It is a third *variant*, not a third Kind, and that distinction is the
    // whole finding behind #30: X4 draws a ship hull in rp #13 and a menu quad
    // in rp #33 with the *same module*, measured in takes 61, 74 and 80. No
    // per-module predicate can separate those two draws, because
    // vkCreateShaderModule sees one module and patches it once. Only the
    // render pass tells them apart, so only pipeline creation can choose.
    static bool have_canvas = false;
    static float K_canvas[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static float K_canvas_r[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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
            const float ipd = configured_ipd();
            const float sx = assumed_proj_sx();
            const float nz = assumed_proj_near();
            const bool right = (eye[0] == 'r' || eye[0] == 'R');
            const float dx = (right ? +0.5f : -0.5f) * ipd;
            const x4vr::Mat4 k = x4vr::make_eye_shear(sx, 0.0f, nz, dx);
            memcpy(K_world, k.m, sizeof(k.m));
            have_k = true;
            X4VR_LOG("eye=%s ipd=%.4f sx=%.4f near=%.3f -> K shear m8=%.5f",
                     right ? "right" : "left", ipd, sx, nz, K_world[8]);
        }
        // Stage 2: one module, both eyes, selected by gl_ViewIndex.
        //
        // X4VR_EYE bakes a single eye into every draw, which is how Phase 4a
        // validated the shear; X4VR_STEREO instead bakes both and lets the
        // view index pick. The derivation is identical -- the same
        // make_eye_shear, the same sx/near -- so a stereo run and the pair of
        // one-eye runs it replaces should produce the same two images.
        // Value-sensitive, like every other knob in the layer. It used to test
        // presence alone, which made `X4VR_STEREO=0` bake the shear -- the
        // exact opposite of what it reads as. The take-50 control worked only
        // because it *omitted* the variable rather than setting it to zero,
        // and docs/known-good-runs.md described that control as
        // "X4VR_STEREO=0". Anyone reproducing it from the docs would have got
        // a stereo run, seen the control fail, and gone looking for a
        // regression that was not there.
        if (const char *st = getenv("X4VR_STEREO"); st && *st && *st != '0') {
            const float ipd = configured_ipd();
            const float sx = assumed_proj_sx();
            const float nz = assumed_proj_near();
            const x4vr::Mat4 kl = x4vr::make_eye_shear(sx, 0.0f, nz, -0.5f * ipd);
            const x4vr::Mat4 kr = x4vr::make_eye_shear(sx, 0.0f, nz, +0.5f * ipd);
            memcpy(K_world, kl.m, sizeof(kl.m));
            memcpy(K_world_r, kr.m, sizeof(kr.m));
            have_k = have_kr = true;
            X4VR_LOG("stereo: ipd=%.4f sx=%.4f near=%.3f -> shear m8 L=%.5f "
                     "R=%.5f (per-view, gl_ViewIndex selects)",
                     ipd, sx, nz, K_world[8], K_world_r[8]);
            // **The second gate.** Take 152 flipped the UI passes to sheared
            // and changed NOTHING -- 33% of the frame at infinity before, 30%
            // after, median -30 px both times. Reclassifying the pass only
            // decides whether the *patched* module is bound; the matrix that
            // module carries is a separate gate, and K_nonworld is identity
            // unless someone sets it. So the probe bound a patched module that
            // applied an identity transform, and the null refuted the knob
            // rather than the hypothesis -- the same trap as the invproj knob
            // that patched member 2 while the shadows read member 4.
            //
            // Treating the HUD as world geometry means giving it the world's
            // matrices, so X4VR_SHEAR_UI now does both halves. Still a probe:
            // the message box and the menus are genuinely screen space and get
            // this wrongly too.
            if (g_shear_ui) {
                memcpy(K_nonworld, kl.m, sizeof(kl.m));
                memcpy(K_nonworld_r, kr.m, sizeof(kr.m));
                have_kr_nonworld = true;
                X4VR_LOG("shear-ui probe: nonworld modules now carry the WORLD "
                         "shear (m8 L=%.5f R=%.5f) — without this the pass "
                         "reclassification alone is a no-op",
                         K_nonworld[8], K_nonworld_r[8]);
            }
        }
        // Direct matrices, which is what the offline suite drives: it needs
        // two layers that provably differ, without an IPD or a projection.
        if (const char *s = getenv("X4VR_CLIP_K_RIGHT"))
            have_kr |= parse16(s, K_world_r);
        if (const char *s = getenv("X4VR_CLIP_K_NONWORLD"))
            have_k |= parse16(s, K_nonworld);
        // Normally unset: the UI is CPU hit-tested and belongs in both eyes
        // identically, so it stays mono. Exists because the offline test's
        // shader declares no set-3 block and therefore classifies NonWorld.
        if (const char *s = getenv("X4VR_CLIP_K_NONWORLD_RIGHT"))
            have_kr_nonworld |= parse16(s, K_nonworld_r);
        if (const char *sh = getenv("X4VR_CLIP_SHIFT_NONWORLD")) {
            K_nonworld[12] = strtof(sh, nullptr);
            have_k = true;
        }
        // X4VR_CANVAS_M: where the UI sits, in metres. Converted here rather
        // than exposed as a raw NDC number so it stays right when the IPD or
        // the projection scale change:
        //
        //     s = sx · (ipd/2) / z
        //
        // which is the world offset formula with z pinned to a constant --
        // "the UI at z metres" and "world geometry at z metres" are the same
        // statement, so they must produce the same disparity. Left eye +s,
        // right eye −s: a near object appears displaced toward the right in
        // the left eye, which is the sign the world shear already carries
        // (m8 L=+0.42666 R=−0.42666).
        //
        // Task #40 makes this run in one more case: with the off-axis affine
        // on, screen-locked content NEEDS the map even with no distance
        // chosen, because leaving it in X4's symmetric frame under a canted
        // declaration is the 30.07 deg divergence, and "pinned at infinity"
        // is a fusable state while "not mapped at all" is not. z = 0 is that
        // infinity, and make_canvas_k takes it as such.
        {
            const char *cm = getenv("X4VR_CANVAS_M");
            const bool asked_z = cm && *cm;
            const float z = asked_z ? strtof(cm, nullptr) : 0.0f;
            const OffAxisPair &oa = offaxis_target();
            if (asked_z && !(z > 0.0f)) {
                X4VR_LOG("canvas: REFUSED — X4VR_CANVAS_M=\"%s\" is not a "
                         "positive distance; no canvas built",
                         cm);
            } else if ((asked_z || oa.on) && !have_kr) {
                // Gate on intent: this run asked for a canvas, or asked for
                // the affine, which implies one. Say why it cannot have one
                // instead of quietly drawing a mono UI that looks exactly
                // like a correct one.
                X4VR_LOG("canvas: REFUSED — a canvas needs a right eye and "
                         "none is configured (X4VR_STEREO unset?); asked by "
                         "%s",
                         asked_z ? "X4VR_CANVAS_M" : "the off-axis affine");
            } else if (asked_z || oa.on) {
                // The screen's own half-angle, from the knob that set it --
                // never from a camera block. vr_declared_fov() gives the
                // reason: several cameras run per frame with sx from 0.75 to
                // 3.78, and picking one of those is the mistake fifty takes
                // were built on. Both callers go through x4_half_for_fov so
                // the field the UI is placed against and the field the
                // compositor is told about are one number.
                const char *fe = getenv("X4VR_FOV");
                const float half =
                    x4vr::x4_half_for_fov(fe ? strtof(fe, nullptr) : 0.0f);
                const float sx_s = 1.0f / std::tan(half);
                const float sy_s = -sx_s;
                const float dl = -0.5f * configured_ipd();
                const float dr = +0.5f * configured_ipd();
                x4vr::make_canvas_k(oa.on ? oa.eye[0] : x4vr::OffAxis{}, sx_s,
                                    sy_s, dl, z, K_canvas);
                x4vr::make_canvas_k(oa.on ? oa.eye[1] : x4vr::OffAxis{}, sx_s,
                                    sy_s, dr, z, K_canvas_r);
                have_canvas = true;
                // Published for the cursor overlay, which has to move the
                // pointer by the same map or it lands a fixed distance from
                // every button it activates -- in a run where both features
                // report success. See docs/known-good-runs.md, stage4.
                g_canvas_map.publish(K_canvas, K_canvas_r);
                g_canvas_shift.store(K_canvas[12], std::memory_order_relaxed);
                // The pixel figure is what a run is scored on, so print it
                // when the eye width is known and say nothing when it is not,
                // rather than quoting a hardcoded 1408 that could go stale.
                unsigned rw = 0, rh = 0;
                if (const char *res = getenv("X4VR_RES"))
                    sscanf(res, "%ux%u", &rw, &rh);
                char px[64] = "";
                if (rw)
                    snprintf(px, sizeof(px), ", %.1f px on a %u-wide eye",
                             K_canvas[12] * 0.5f * rw, rw);
                if (oa.on)
                    X4VR_LOG("canvas: %s, CANTED — screen half-angle %.2f deg "
                             "(sx=%.5f) -> A_x=%.4f A_y=%.4f, eye0 x offset "
                             "%+.5f eye1 %+.5f NDC%s. Screen-locked draws are "
                             "placed in the DECLARED frame, so both eyes see "
                             "them at the angle X4 drew them at.",
                             z > 0.0f ? "at a chosen distance" : "at infinity",
                             half * 57.2957795130823f, sx_s, K_canvas[0],
                             K_canvas[5], K_canvas[12], K_canvas_r[12], px);
                else
                    X4VR_LOG("canvas: %.3f m -> s=%.5f NDC (L=+s R=-s)%s",
                             z, K_canvas[12], px);
            }
        }
        if (have_k)
            X4VR_LOG("clip-space enabled: K_world.x=%.3f K_nonworld.x=%.3f",
                     K_world[12], K_nonworld[12]);
    });

    if (!have_k || !ci->pCode || ci->codeSize < 20)
        return d->CreateShaderModule(device, ci, ac, out);

    std::vector<uint32_t> code(ci->codeSize / 4);
    memcpy(code.data(), ci->pCode, ci->codeSize);

    // Applied before the vertex patch, and before the NotVertex early-out,
    // because X4 ships one module carrying both entry points: the same bytes
    // may need the fragment edit and the vertex edit, or only one of them.
    //
    // Both tables get patched. 228 of the 409 dumped modules declare two, and
    // patching one would leave the other sampling view 0's slot in both eyes.
    // The transform reuses an existing ViewIndex input on the second call --
    // two variables decorated with the same builtin would be invalid SPIR-V.
    bool frag_patched = false;
    if (g_bindless_patch && g_bindless_mirror) {
        // Every table the module declares, found rather than hardcoded. X4's
        // are set 0 bindings 5 and 7, but naming them here would have been two
        // magic numbers standing in for the rule, which is: a descriptor array
        // big enough that the mirror can fit a twin region inside it.
        //
        // `count > offset` is the same bound the mirror applies before writing
        // a twin. Patching a table the mirror declines to mirror would send
        // view 1 to descriptors nobody wrote, so the two rules have to agree.
        // Runtime arrays are skipped: their length is not known here, so
        // whether a twin fits cannot be established.
        // Once per (set, binding), not once per table. The lister returns one
        // entry per *variable*, and X4 aliases two variables onto one binding,
        // so this used to call the patch twice with identical arguments -- and
        // the second call offset the first variable's index a second time, to
        // index + 2*OFFSET, off the end of the array. See take forty-eight.
        // patch_fragment_index_offset now covers every variable at the binding
        // in one call, which is what makes calling it once correct.
        std::set<std::pair<uint32_t, uint32_t>> done_tables;
        for (const auto &t : x4vr::spv::list_sampled_textures(code)) {
            if (t.count <= g_mirror_offset)
                continue;
            if (!done_tables.insert({t.set, t.binding}).second)
                continue;
            if (x4vr::spv::patch_fragment_index_offset(code, t.set, t.binding,
                                                       g_mirror_offset))
                frag_patched = true;
        }
        // Task #22: correct M_invprojection per eye.
        //
        // The deferred passes reconstruct view position from the depth buffer,
        // which was rendered through the sheared clip position, so they get the
        // position in *that eye's* frame -- and then light it with shadow
        // matrices and light positions that are still centre-frame. The two
        // eyes end up disagreeing about where a shadow falls on a surface by
        // the full IPD. Shadows are view-independent, so that is a defect, and
        // it is what Patola saw in the cockpit in takes pre-51 and 55.
        //
        // Gated on the shear being active: with no shear there is no eye frame
        // to correct back from, and applying this alone would introduce the
        // very error it exists to remove.
        if (proj_invproj() && have_k) {
            const float dl = -0.5f * configured_ipd();
            const float dr = +0.5f * configured_ipd();
            // Task #39. The same latched target the vertex side gets, from the
            // same object, because these two are a matched pair: the affine
            // applied to gl_Position and the affine undone before the deferred
            // reconstruction have to be the same map or the reconstruction is
            // wrong by the difference. Reading the target twice from two
            // places is exactly how that would happen, so it is read once.
            //
            // Both eyes unconditionally. Unlike the shear -- where the right
            // eye's `d` only exists if K_right does -- the affine is per-eye by
            // construction, and a single map applied to both eyes would put
            // them in the same canted frustum, which is the take 165b failure.
            const OffAxisPair &oa = offaxis_target();
            const x4vr::OffAxis *oal = oa.on ? &oa.eye[0] : nullptr;
            const x4vr::OffAxis *oar = oa.on ? &oa.eye[1] : nullptr;
            if (x4vr::spv::patch_fragment_invproj_eye(
                    code, kCameraSet, kCameraBinding, kCameraInvProjMember, dl,
                    dr, oal, oar)) {
                frag_patched = true;
                g_invproj_patched++;
            }
            // The same correction on member 4. Counted separately because the
            // two are wildly different populations -- 236 modules take member 2
            // and exactly 2 take member 4 -- so a single total would let a
            // healthy-looking 236 hide a 0 here, which is the whole defect.
            if (x4vr::spv::patch_fragment_invproj_eye(
                    code, kCameraSet, kCameraBinding, kCameraInvProjUjMember,
                    dl, dr, oal, oar)) {
                frag_patched = true;
                g_invproj_uj_patched++;
            }
        }
        // Coverage measured stage-agnostically, NOT from the lister above.
        // The lister is fragment-only and 2D-only; asking it "does this module
        // declare a mirrorable table?" made the answer no for X4's skybox --
        // a compute shader sampling the same 53306 heap as a cube array -- and
        // the refusal counter built on it reported a reassuring 0.
        const auto surv = x4vr::spv::survey_image_tables(code, g_mirror_offset);
        if (surv.large) {
            if (frag_patched)
                g_frag_patch_ok++;
            else {
                g_frag_patch_refused++;
                if (surv.compute)
                    g_compute_tables++;
            }
        }
        static uint32_t n_frag = 0;
        if (frag_patched && (++n_frag <= 3 || (n_frag % 100) == 0))
            X4VR_LOG("patched fragment shader #%u (index + ViewIndex*%u)",
                     n_frag, g_mirror_offset);
    } else if (g_bindless_patch) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            X4VR_LOG("X4VR_BINDLESS_PATCH ignored: it needs "
                     "X4VR_BINDLESS_MIRROR=1, or view 1 would read table slots "
                     "nobody ever wrote");
        }
    }

    // Task #22 measurement: make X4's volumetric fog a passthrough.
    //
    // Deliberately outside the bindless gate above -- this is an experiment
    // about X4's own rendering, not about our per-view sampling, and silently
    // doing nothing because an unrelated knob was off is how a control run
    // ends up measuring something other than what it claims.
    //
    // Value-based, like every other knob here. X4VR_STEREO was presence-based
    // once, so X4VR_STEREO=0 *enabled* the shear and take 50's control worked
    // only because the variable was omitted entirely. Never again by accident.
    //
    // Placed before the twin snapshot below on purpose: the unsheared twin must
    // carry the same fog treatment, or the passes that take the twin would keep
    // the fog while the rest lost it, and the measurement would be unreadable.
    if (const char *fg = getenv("X4VR_DISABLE_FOG"); fg && *fg && *fg != '0') {
        if (x4vr::spv::patch_fragment_disable_fog(code)) {
            const uint64_t n = ++g_fog_disabled;
            if (n <= 8)
                X4VR_LOG("fog passthrough #%llu: volumetric composite forced to "
                         "scene*1+0 — DIAGNOSTIC, not a fix",
                         (unsigned long long)n);
        }
    }

    // The bytes for the unsheared twin below: fragment edit kept, vertex shear
    // not yet applied.
    //
    // Take twenty-three: the twin used to be the pristine module, which threw
    // away the fragment patch along with the shear. rp #40 -- the composition,
    // masked and unsheared because it draws a fullscreen triangle -- takes the
    // twin for every one of its pipelines, so #103 was the one image in the
    // frame still sampling view 0's slots in both eyes. The twin must lose the
    // geometry edit and nothing else.
    std::vector<uint32_t> frag_only;
    if (frag_patched)
        frag_only = code;
    // Set here and cleared on every driver-rejection fallback below, so the
    // flag names what the game actually got rather than what we attempted.
    if (frag_patched_out)
        *frag_patched_out = frag_patched;

    // Task #22 / P70: also shear geometry positioned by the camera rather than
    // by a per-object matrix -- X4's instanced deferred light volumes. Default
    // off so the current known-good behaviour is what runs unless a measurement
    // asks for the other one; a state is code *and* knobs.
    static const bool wide_camera = [] {
        const char *s = getenv("X4VR_SHEAR_LIGHTS");
        return s && *s && *s != '0';
    }();
    const x4vr::spv::Kind kind = x4vr::spv::classify(code, wide_camera);
    if (kind == x4vr::spv::Kind::NotVertex) {
        if (!frag_patched)
            return d->CreateShaderModule(device, ci, ac, out);
        VkShaderModuleCreateInfo mod = *ci;
        mod.codeSize = code.size() * 4;
        mod.pCode = code.data();
        VkResult r = d->CreateShaderModule(device, &mod, ac, out);
        if (r == VK_SUCCESS)
            return r;
        X4VR_LOG("WARNING: driver rejected fragment-patched module (%d); "
                 "using original",
                 (int)r);
        if (frag_patched_out)
            *frag_patched_out = false;
        return d->CreateShaderModule(device, ci, ac, out);
    }
    const bool world = kind == x4vr::spv::Kind::World;
    const float *K = world ? K_world : K_nonworld;
    // Null unless a right eye was configured for this kind, and that null is
    // what keeps the module mono.
    const float *KR = world ? (have_kr ? K_world_r : nullptr)
                            : (have_kr_nonworld ? K_nonworld_r : nullptr);

    static uint32_t patched = 0, n_world = 0, n_nonworld = 0, n_stereo = 0;
    static uint32_t n_live = 0, n_baked = 0, n_mvp = 0;
    (world ? n_world : n_nonworld)++;
    if (KR)
        n_stereo++;

    // Task #23: world geometry reads sx out of X4's live camera block; the
    // eye offset stays baked because it is our choice, not X4's.
    //
    // World only. The derivation assumes clip z is the camera's near plane,
    // which is true for draws through M_worldviewprojection and false for the
    // UI -- so UI keeps its own baked matrix, exactly as before. (Shadow
    // passes are excluded a step later, at pipeline creation.)
    //
    // A module without the camera block falls through to patch_vertex_clip
    // and the baked sx: 18 of X4's 341 world modules have no camera block, and
    // a stale shear on those is a far better outcome than no shear at all,
    // which would leave that geometry identical in both eyes.
    bool vert_patched = false;
    if (proj_live() && world && have_k) {
        const float dl = -0.5f * configured_ipd();
        const float dr = +0.5f * configured_ipd();
        // Task #35. Rides on the same two patches as the live shear because it
        // needs the same live sx/sy; a module that falls all the way through to
        // the baked matrix gets the shear and NOT the affine, and is therefore
        // in the wrong frustum. That is the n_baked count below, and it is
        // logged rather than left to be discovered.
        const OffAxisPair &oa = offaxis_target();
        const x4vr::OffAxis *oal = oa.on ? &oa.eye[0] : nullptr;
        const x4vr::OffAxis *oar = (oa.on && KR) ? &oa.eye[1] : nullptr;
        vert_patched = x4vr::spv::patch_vertex_eye_offset(
            code, kCameraSet, kCameraBinding, kCameraProjMember, dl,
            KR ? &dr : nullptr, oal, oar);
        if (vert_patched) {
            n_live++;
        } else {
            // Task #23. Only ever as a fallback, and only for the modules the
            // camera-block patch just refused: those declare the set-3
            // per-object block and nothing else, so sx is recovered from
            // M_worldviewprojection rather than read from a camera they cannot
            // see. On by default since takes 109/110; X4VR_PROJ_MVP=0 puts
            // these twelve back on the baked constant, which is the
            // stage5-wide-field behaviour exactly.
            const bool mvp = proj_mvp() &&
                             x4vr::spv::patch_vertex_eye_offset_mvp(
                                 code, kObjectSet, kObjectBinding,
                                 kObjectMvpMember, dl, KR ? &dr : nullptr,
                                 oal, oar);
            (mvp ? n_mvp : n_baked)++;
            vert_patched = mvp;
        }
    }
    if (!vert_patched)
        vert_patched = x4vr::spv::patch_vertex_clip(code, K, KR);

    // `|| frag_patched` so a module that only needed the fragment edit still
    // gets created from the edited bytes.
    if (vert_patched || frag_patched) {
        VkShaderModuleCreateInfo mod = *ci;
        mod.codeSize = code.size() * 4;
        mod.pCode = code.data();
        VkResult r = d->CreateShaderModule(device, &mod, ac, out);
        if (r == VK_SUCCESS) {
            // Keep a twin with the original geometry path so depth-only
            // (shadow) and other unsheared pipelines can use it — see
            // g_variants and needs_original(). It carries the fragment edit:
            // "unsheared" is a statement about vertices, and an unsheared pass
            // still has to sample per view.
            VkShaderModule orig = VK_NULL_HANDLE;
            VkShaderModuleCreateInfo oci = *ci;
            if (frag_patched) {
                oci.codeSize = frag_only.size() * 4;
                oci.pCode = frag_only.data();
            }
            if (d->CreateShaderModule(device, &oci, ac, &orig) == VK_SUCCESS) {
                std::lock_guard<std::mutex> lock(g_variants.mu);
                g_variants.original[*out] = orig;
            }
            // Task #30: a third twin. Built from the same base as the
            // unpatched one -- the fragment edit and nothing else -- then
            // given the constant map where that one is given no geometry edit
            // at all. The procedural fullscreen module that shares rp #33 with
            // the UI has to keep drawing where it is, and absence from this
            // map is how pipeline creation knows that.
            //
            // Task #40 states that exclusion directly instead of through
            // `world`. #30 keyed on World and got the right answer, because
            // every World module has vertex attributes -- but "World" is a
            // statement about a per-object matrix and has nothing to do with
            // whether a quad is procedural. Measured over all 409 dumps the
            // two readings differ on exactly five modules: mod-0000, 0228,
            // 0229, 0397 and 0398, NonWorld with real attributes, which is UI
            // that #30 could not move. 0 modules are World-and-procedural, so
            // nothing that used to get a canvas loses one.
            if (have_canvas && !x4vr::spv::is_procedural_fullscreen(code)) {
                std::vector<uint32_t> cv;
                if (frag_patched) {
                    cv = frag_only;
                } else {
                    cv.resize(ci->codeSize / 4);
                    memcpy(cv.data(), ci->pCode, ci->codeSize);
                }
                VkShaderModule cm = VK_NULL_HANDLE;
                if (!x4vr::spv::patch_vertex_clip(cv, K_canvas, K_canvas_r)) {
                    canvas_refuse("patch_vertex_clip declined the module");
                } else {
                    VkShaderModuleCreateInfo cci = *ci;
                    cci.codeSize = cv.size() * 4;
                    cci.pCode = cv.data();
                    if (d->CreateShaderModule(device, &cci, ac, &cm) ==
                        VK_SUCCESS) {
                        std::lock_guard<std::mutex> lock(g_variants.mu);
                        g_variants.canvas[*out] = cm;
                        g_canvas_built++;
                    } else {
                        canvas_refuse("the driver rejected the canvas variant");
                    }
                }
            }
            if (++patched <= 3 || (patched % 50) == 0)
                X4VR_LOG("patched vertex shader #%u (%s%s) "
                         "[world=%u nonworld=%u stereo=%u live-sx=%u "
                         "mvp-sx=%u baked-sx=%u]%s",
                         patched, world ? "world" : "nonworld",
                         KR ? ", per-view" : "", n_world, n_nonworld, n_stereo,
                         n_live, n_mvp, n_baked,
                         // The affine reaches live-sx + mvp-sx and nothing
                         // else, so baked-sx is exactly the set of world
                         // modules left in X4's frustum while the rest move to
                         // the runtime's. Named on the line that counts them.
                         // Peek, never latch. Latching here would fix the
                         // target at the first NONWORLD module, which take 163
                         // timestamps 12 ms before the first located frame --
                         // the runtime would lose the race every run.
                         g_offaxis_on.load(std::memory_order_relaxed)
                             ? " +offaxis (live-sx and mvp-sx only; baked-sx "
                               "keeps X4's frustum)"
                             : "");
            return r;
        }
        // Patched module rejected by the driver: fall back to the original
        // rather than failing the game's shader creation.
        X4VR_LOG("WARNING: driver rejected patched module (%d); using original",
                 (int)r);
        if (frag_patched_out)
            *frag_patched_out = false;
    }
    return d->CreateShaderModule(device, ci, ac, out);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkShaderModule *out) {
    bool frag_patched = false;
    VkResult r = create_shader_module_inner(device, ci, ac, out, &frag_patched);
    // X4's bytes, not ours: the handle is whatever the game will bind, but the
    // code written out is what it supplied. A dump that quietly contained our
    // own edits would be worse than no dump.
    if (r == VK_SUCCESS) {
        record_module(ci, *out);
        if (frag_patched) {
            std::lock_guard<std::mutex> lock(g_mod_mu);
            g_mod_frag_patched.insert(*out);
        }
    }
    return r;
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
    {
        // Vulkan is free to hand this handle straight back for a different
        // module, and a stale entry would then make the tonemap log name the
        // wrong shader. The view caches learned this the expensive way.
        std::lock_guard<std::mutex> lock(g_mod_mu);
        g_mod_serial.erase(mod);
        g_mod_samplers.erase(mod);
    }
    if (twin != VK_NULL_HANDLE)
        d->DestroyShaderModule(device, twin, ac);
    d->DestroyShaderModule(device, mod, ac);
}

// Pick the unpatched module variant for depth-only (shadow) pipelines.
// This is the whole shadow-exclusion mechanism: one substitution at
// pipeline-creation time, nothing per frame.
// Compute, which this layer has never hooked at all.
//
// Every instrument here -- the writer lists, the masked/unmasked split, the
// pipeline provenance, the probe -- is built around render passes, so a frame
// stage performed by a dispatch is not merely unhandled but *invisible*: it
// leaves no line anywhere. Ten of the 409 dumped modules are compute, and two
// of them sample the same 53306-entry bindless heap the fragment shaders do.
//
// Nothing here changes behaviour. It records what runs, because the question
// "what merges the scene with the HUD" has so far been answered only by
// elimination, and elimination cannot see into a blind spot.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *ac,
    VkPipeline *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    const VkResult r =
        d->CreateComputePipelines(device, cache, count, ci, ac, out);
    if (r != VK_SUCCESS || !g_mv_inventory || !g_active)
        return r;
    for (uint32_t i = 0; i < count; i++) {
        if (out[i] == VK_NULL_HANDLE)
            continue;
        uint32_t serial = UINT32_MAX;
        {
            std::lock_guard<std::mutex> lock(g_mod_mu);
            auto it = g_mod_serial.find(ci[i].stage.module);
            if (it != g_mod_serial.end())
                serial = it->second;
        }
        std::lock_guard<std::mutex> lock(g_comp_mu);
        g_comp_module[out[i]] = serial;
        g_comp_pipelines++;
    }
    return r;
}

void note_dispatch(VkCommandBuffer cb) {
    if (!g_mv_inventory || !g_active)
        return;
    std::lock_guard<std::mutex> lock(g_comp_mu);
    auto b = g_comp_bound.find(cb);
    if (b == g_comp_bound.end())
        return;
    auto m = g_comp_module.find(b->second);
    g_comp_dispatches[m != g_comp_module.end() ? m->second : UINT32_MAX]++;
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdDispatch(VkCommandBuffer cb, uint32_t x,
                                            uint32_t y, uint32_t z) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    note_dispatch(cb);
    d->CmdDispatch(cb, x, y, z);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdDispatchIndirect(VkCommandBuffer cb,
                                                    VkBuffer buf,
                                                    VkDeviceSize off) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    note_dispatch(cb);
    d->CmdDispatchIndirect(cb, buf, off);
}

VKAPI_ATTR void VKAPI_CALL x4vr_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx,
                                                uint32_t by, uint32_t bz,
                                                uint32_t x, uint32_t y,
                                                uint32_t z) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(cb));
    }
    note_dispatch(cb);
    d->CmdDispatchBase(cb, bx, by, bz, x, y, z);
}

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

    // Name the shaders that draw into #103.
    //
    // This answered the question it was built for and then some: every one of
    // them samples set 0 binding 7 with a count of 53306. X4 is bindless, one
    // table for the whole game, indexed by S_diffuse_idx out of a uniform
    // block. So there is no per-texture descriptor to swap and no type to
    // promote -- see docs/frame-analysis.md, "X4 is bindless".
    //
    // Kept because it is the only join between a pass and the modules drawn
    // through it, and because the count is what any future approach has to
    // respect.
    if (g_mv && g_active && (g_mv_inventory || g_dump_shaders)) {
        for (uint32_t i = 0; i < count; i++) {
            bool tonemap;
            uint32_t rp_serial = UINT32_MAX;
            {
                std::lock_guard<std::mutex> lock(g_variants.mu);
                tonemap = ci[i].renderPass != VK_NULL_HANDLE &&
                          g_srgb_resolve_passes.count(ci[i].renderPass) != 0;
                auto s = g_rp_serials.find(ci[i].renderPass);
                if (s != g_rp_serials.end())
                    rp_serial = s->second;
            }
            // Whether this pass reaches the screen is not known yet -- its
            // framebuffer may not exist. So record every pass's fragment
            // shaders and let the summary select; only the tonemap pass is
            // reported inline, where it always has been.
            if (!tonemap && rp_serial == UINT32_MAX)
                continue;
            for (uint32_t st = 0; st < ci[i].stageCount; st++) {
                if (ci[i].pStages[st].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                    continue;
                char list[256];
                int n = 0;
                list[0] = 0;
                uint32_t serial = UINT32_MAX;
                bool patched = false;
                {
                    std::lock_guard<std::mutex> lock(g_mod_mu);
                    auto sit = g_mod_serial.find(ci[i].pStages[st].module);
                    if (sit != g_mod_serial.end())
                        serial = sit->second;
                    patched =
                        g_mod_frag_patched.count(ci[i].pStages[st].module) != 0;
                    auto tit = g_mod_samplers.find(ci[i].pStages[st].module);
                    if (tit != g_mod_samplers.end())
                        for (const auto &t : tit->second) {
                            if (n >= 200)
                                break;
                            n += snprintf(list + n, sizeof(list) - n,
                                          "%sset %u binding %u", n ? ", " : "",
                                          t.set, t.binding);
                            // The count is the load-bearing number: X4 is
                            // bindless, so this is 53306 and not 1, and that is
                            // what rules out promoting the type.
                            if (t.count != 1)
                                n += snprintf(list + n, sizeof(list) - n,
                                              "[%u]", t.count);
                            n += snprintf(list + n, sizeof(list) - n, "%s%s",
                                          t.arrayed ? " (already array)" : "",
                                          t.depth ? " (DEPTH)" : "");
                        }
                }
                if (rp_serial != UINT32_MAX) {
                    std::lock_guard<std::mutex> lock(g_present_mu);
                    auto &e = g_rp_frag[rp_serial][serial];
                    e.module = serial;
                    e.patched = patched;
                    snprintf(e.samplers, sizeof e.samplers, "%s",
                             n ? list : "nothing");
                }
                if (tonemap)
                    X4VR_LOG("srgb-resolve rp #%u: frag module #%u samples %s"
                             " [index-offset %s]",
                             rp_serial, serial, n ? list : "nothing",
                             patched ? "APPLIED" : "NOT APPLIED");
            }
        }
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
        // Task #30: on a canvas pass a World module takes the constant-shift
        // twin instead of the unpatched one. This is the second half of the
        // join and the reason it has to be decided here: the same module is
        // bound to rp #13 (a ship hull, which wants the shear) and to rp #33
        // (a menu quad, which wants the shift), so the module cannot decide
        // for itself. Modules with no canvas entry -- since task #40 that is
        // the procedural fullscreen ones, which have to keep covering the
        // screen exactly, plus every module in the blit passes -- fall through
        // to the existing swap and behave exactly as before.
        //
        // Reached only for subpasses that need the original, which is what
        // `continue` above filters on. That is not an oversight: a canvas pass
        // is one with colour, no depth and all-LDR attachments, and take 71
        // made exactly that shape unsheared. The two predicates agree by
        // construction rather than by luck, and the swap count stays small --
        // takes 98-100 built 348 variants and swapped 18 -- because only a
        // handful of pipelines are built against a UI pass.
        const bool canvas = is_canvas_pass(ci[i].renderPass, ci[i].subpass);
        for (auto &st : stages[i]) {
            std::lock_guard<std::mutex> lock(g_variants.mu);
            if (canvas) {
                auto cv = g_variants.canvas.find(st.module);
                if (cv != g_variants.canvas.end()) {
                    st.module = cv->second;
                    g_variants.canvas_swapped++;
                    continue;
                }
            }
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
    {
        std::lock_guard<std::mutex> lock(g_comp_mu);
        g_comp_module.erase(p);
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
        x4vr::format_mat(b, sizeof(b), x4vr::load(blk + x4vr::kProjectionUJ));
        X4VR_LOG("M_projectionUJ  %s", b);
        X4VR_LOG("storage order detected: %s (draws=%u)",
                 major == x4vr::Major::Column ? "column-major"
                 : major == x4vr::Major::Row  ? "row-major"
                                              : "UNKNOWN",
                 best_n);
    }

    // Task #23: the terms themselves, reported on the first read and on every
    // *change* thereafter.
    //
    // Deliberately outside the one-shot latch above. A single sample answers
    // "what is sx right now", which is not the question that decides the
    // design: the shear is baked into shader modules 26 seconds before X4 has
    // a camera to read, so a baked constant is only viable if sx never moves.
    // X4 has zoom. One play session with this line tells us whether a constant
    // can be right at all, and no amount of reasoning substitutes for it.
    //
    // The un-jittered matrix is the one to trust (TAA jitter occupies
    // m[8]/m[9], the shear's own slots); the jittered one is reported
    // alongside so a disagreement in sx/near -- which jitter cannot cause --
    // is visible rather than assumed away.
    if (dump) {
        // The terms are read per block, inside the loop below, rather than
        // once from the winner. See the comment at the loop for what reading
        // only the winner cost.
        // sy joined this test after take 104. That run proved X4's <fov> tag
        // scales the horizontal field linearly, but every CHANGED line reported
        // sx alone, so whether the *vertical* scales with it was unmeasurable
        // from the log -- and "sy tracks sx" is exactly the assumption that
        // decides whether a widened field is undistorted or stretched. A term
        // the run cannot report is a term the run cannot test.
        //
        // The state below is per *camera block*, and was global until take 105.
        // X4 runs several cameras at once and the most-drawn block changes
        // between submits of one frame, so a global "did sx move?" reports every
        // flip between two motionless cameras as a change. Take 104 spent 42 of
        // its 400-change budget on the 1.33333 <-> 0.69231 flip alone; take 105
        // exhausted the budget 181 s into a 382 s session -- mostly on a
        // degenerate block whose near drifted by 1e-3 a sample -- and was blind
        // for the whole second half, which is where the camera that run's
        // fourth prediction was about would have appeared. Keyed per slot, an
        // alternation between two steady cameras emits nothing, and a camera
        // that really moves cannot be starved by a chatty neighbour.
        struct ProjState {
            float sx = 0.0f, sy = 0.0f, near_z = 0.0f;
            uint32_t changes = 0;
            uint32_t id = 0;
        };
        static std::map<ViewSlot, ProjState> cams;
        static uint32_t next_id = 0, total = 0;
        // Per-camera budget so one noisy block cannot blind the rest, plus a
        // global backstop. Both announce themselves when they bite: a silent
        // cap reads exactly like "nothing more happened", which is the failure
        // this whole block was rewritten to stop.
        constexpr uint32_t kPerCam = 120, kTotal = 2000;
        // EVERY credited block this frame, not just the most-drawn one.
        //
        // Reading only the winner is what made take 155 unreadable. The run
        // asked whether X4 would accept <fov> 2.21; X4 accepted it and drew a
        // 163 degree field, and the log contained no camera at that field at
        // all, so the scorer reported a rejection. Take 156 measured the
        // mechanism directly by running both fields back to back:
        //
        //     fov 1.4917 -> the scene camera won  5 of 42 credited samples
        //     fov 2.21   -> the scene camera won  0 of 38
        //
        // A wide field spreads draws across more blocks, so the block actually
        // rendering the picture loses the per-frame vote precisely when the
        // field is unusual -- which is exactly when a run is asking about it.
        // Everything logged was the two cameras that ignore <fov>. A camera
        // never sampled cannot be distinguished from a camera that does not
        // exist, and that is a defect in the instrument that reads as a fact
        // about the game.
        //
        // The winner is registered before the loop so that cam#0 still means
        // "the camera that drew most". Every stored log and the scorer's
        // "proj MEASURED" line assume that, and renumbering them would be a
        // silent change to 71 logs' worth of regression material.
        if (cams.find(*best) == cams.end() && cams.size() < 256)
            cams.emplace(*best, ProjState{}).first->second.id = next_id++;

        for (auto &[slot, slot_n] : g_track.credit) {
        // No draw-count bar here, deliberately. The first version of this loop
        // kept the winner's >=50 test and take 157 still missed the 2.21
        // camera -- because take 156b shows the scene camera drawing exactly
        // **51**, one above the bar. A threshold that the subject of the
        // measurement clears by one is not a filter, it is a coin toss, and it
        // had already excluded the camera outright at the wider field.
        //
        // 50 draws was only ever a proxy for "has X4 populated this block",
        // and the loop now runs the direct test instead: an unpopulated block
        // reads back zeros, so the affine check below rejects it, and
        // read_proj_terms() refuses anything it cannot decode. Replacing a
        // proxy with the thing it stood for costs nothing and stops the
        // instrument from choosing which cameras exist.
        float *cblk = slot_host_ptr(slot);
        if (!cblk)
            continue;
        // A block X4 has not populated reads back zeros, and a zero matrix
        // would otherwise enter the tally as a camera at an absurd field.
        if (std::fabs(x4vr::load(cblk + x4vr::kView).m[15] - 1.0f) > 1e-3f)
            continue;
        const x4vr::ProjTerms t = x4vr::read_proj_terms(
            x4vr::load(cblk + x4vr::kProjectionUJ), major);
        const x4vr::ProjTerms tj = x4vr::read_proj_terms(
            x4vr::load(cblk + x4vr::kProjection), major);
        auto it = cams.find(slot);
        if (it == cams.end() && cams.size() < 256) {
            it = cams.emplace(slot, ProjState{}).first;
            // Numbered on sight, not on first log line: a camera first seen
            // after a budget bit would otherwise keep id 0 and the STEADY line
            // would attribute it to cam#0.
            it->second.id = next_id++;
        }
        if (it == cams.end())
            continue;
        ProjState &c = it->second;
        // Task #35 depends on this sign, so the run has to be able to refute
        // it. patch_vertex_eye_offset_mvp recovers |sy| from a squared length
        // and supplies the minus from measurement -- 35,783 sampled blocks
        // across every log this project has kept, all negative, which is the
        // Y-flip Vulkan's downward NDC y requires. A positive sy here would
        // make those twelve modules render upside down and nothing else would
        // say why, so it is announced rather than left in a comment.
        static bool warned_sy_sign = false;
        if (t.ok && t.sy >= 0.0f && !warned_sy_sign) {
            warned_sy_sign = true;
            X4VR_LOG("proj WARNING: cam#%u reports sy=%+.5f, which is NOT "
                     "negative. Every prior measurement was. The off-axis "
                     "affine's mvp form recovers |sy| and negates it, so this "
                     "camera's world draws through the twelve camera-blind "
                     "modules are vertically inverted",
                     c.id, t.sy);
        }
        const bool first = c.changes == 0;
        const bool moved = std::fabs(t.sx - c.sx) > 1e-4f ||
                           std::fabs(t.sy - c.sy) > 1e-4f ||
                           std::fabs(t.near_z - c.near_z) > 1e-6f;
        // Take 54: the cap was 40, and it fired ten seconds into the only deep
        // zoom of the session -- so every probe sample taken during that zoom
        // had no sx to attribute it to, which is exactly the correlation the
        // line exists to support. Raised, and a periodic heartbeat added so a
        // *quiet* stretch is also attributable: without it, "no change since
        // t" and "logging stopped at t" look identical in the log. The
        // heartbeat stays global -- one line per 30 s, naming whichever camera
        // is current -- because per-camera it would be its own firehose.
        static double last_beat = 0.0;
        timespec bts{};
        clock_gettime(CLOCK_MONOTONIC, &bts);
        const double now = bts.tv_sec + bts.tv_nsec * 1e-9;
        if (t.ok && !moved && total && now - last_beat > 30.0) {
            last_beat = now;
            X4VR_LOG("proj STEADY: cam#%u sx=%.5f near=%.5f (unchanged; %u "
                     "camera(s) seen, %u change(s) logged)",
                     c.id, t.sx, t.near_z, next_id, total);
        }
        if (moved)
            last_beat = now;
        if (!t.ok && !g_track.logged_proj_fail) {
            g_track.logged_proj_fail = true;
            X4VR_LOG("proj: could not read terms from M_projectionUJ "
                     "(sx=%.5f near=%.5f) -- shear still on assumed values",
                     t.sx, t.near_z);
        } else if (t.ok && (first || moved) && c.changes < kPerCam &&
                   total < kTotal) {
            const float ipd = configured_ipd();
            const float d = 0.5f * ipd;
            const float measured = t.sx * d / t.near_z;
            const float baked = assumed_proj_sx() * d / assumed_proj_near();
            if (first) {
                // The very first camera keeps the original three lines: 71
                // logs on disk are the regression suite for score_run.py, and
                // it requires a "proj MEASURED" before it will report anything.
                if (c.id == 0) {
                    X4VR_LOG("proj MEASURED: sx=%.5f sy=%.5f near=%.5f "
                             "(jittered sx=%.5f near=%.5f)",
                             t.sx, t.sy, t.near_z, tj.sx, tj.near_z);
                    X4VR_LOG("proj ASSUMED : sx=%.5f near=%.5f",
                             assumed_proj_sx(), assumed_proj_near());
                    X4VR_LOG("proj SHEAR   : measured |m8|=%.5f vs baked "
                             "|m8|=%.5f -> baked is %.3fx the correct magnitude "
                             "(ipd=%.4f) -- against cam#0, which is whichever "
                             "camera drew first, not necessarily the scene's",
                             std::fabs(measured), std::fabs(baked),
                             measured != 0.0f ? baked / measured : 0.0f, ipd);
                }
                X4VR_LOG("proj CAMERA cam#%u: sx=%.5f sy=%.5f near=%.5f "
                         "(draws=%u, |sy/sx|=%.4f)",
                         c.id, t.sx, t.sy, t.near_z, slot_n,
                         t.sx != 0.0f ? std::fabs(t.sy / t.sx) : 0.0f);
            } else {
                X4VR_LOG("proj CHANGED cam#%u #%u: sx %.5f -> %.5f  sy %.5f -> "
                         "%.5f  near %.5f -> %.5f  (correct |m8| now %.5f, "
                         "baked %.5f)",
                         c.id, c.changes, c.sx, t.sx, c.sy, t.sy, c.near_z,
                         t.near_z, std::fabs(measured), std::fabs(baked));
            }
            c.sx = t.sx;
            c.sy = t.sy;
            c.near_z = t.near_z;
            c.changes++;
            total++;
            if (c.changes == kPerCam)
                X4VR_LOG("proj: cam#%u reached %u changes -- further changes "
                         "from THIS camera suppressed, the others continue",
                         c.id, kPerCam);
            if (total == kTotal)
                X4VR_LOG("proj: %u changes logged across %u cameras -- all "
                         "further changes suppressed from here on",
                         kTotal, next_id);
        }
        }
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
// The eye size this run expects, in one place.
//
// It used to be the compiled `X4VR_SBS_WIDTH/2 x X4VR_SBS_HEIGHT` at every use,
// and take 102 showed the cost: the launcher told X4 to render 1408x792 for a
// 16:9 aspect test, X4 did exactly that, and the split refused because the
// constant still said 1408x1408. The frame degraded to "duplicate the left
// half" -- two copies of one eye, 704 apart -- and the run measured nothing
// about aspect. That is the third extent in task #31, and it disagreed with the
// other two while every individual component was behaving as written.
//
// X4VR_RES is what the launcher tells X4 to render, derived from the same W/H
// it gives gamescope, so reading it here makes all three follow one number.
VkExtent2D expected_eye() {
    static const VkExtent2D e = [] {
        VkExtent2D v{X4VR_SBS_WIDTH / 2, X4VR_SBS_HEIGHT};
        if (const char *res = getenv("X4VR_RES")) {
            unsigned w = 0, h = 0;
            if (sscanf(res, "%ux%u", &w, &h) == 2 && w && h)
                v = {w, h};
        }
        return v;
    }();
    return e;
}

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
    // #36: the runtime shares this queue on any device with one graphics
    // queue. Uncontended when it does not, and inert until the XR thread
    // starts submitting.
    if (g_vr_share_queue) {
        VrQueueLock qlock;
        return d->QueueSubmit(queue, submitCount, submits, fence);
    }
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
    const VkExtent2D eye = expected_eye();
    const bool split = g_sbs_enabled && g_active && g_sbs_split_render &&
                       ci->imageExtent.width == eye.width &&
                       ci->imageExtent.height == eye.height;
    if (split)
        sbs_ci.imageExtent.width *= 2;
    // Task #31, stated as one line rather than as three that have to be found
    // and compared. The three extents are X4's render, the eye this run expects
    // and the composite actually presented; take 101 had them at 1408x1408,
    // 1408x1408 and 2816x1408 against a 2816x792 window, and every individual
    // component was behaving as written. Nothing here can go wrong quietly
    // afterwards, because the line says whether they agree in the same breath
    // as it says what they are.
    {
        static bool said = false;
        if (!said && g_sbs_enabled && g_active) {
            said = true;
            const bool ok = !g_sbs_split_render || split;
            X4VR_LOG("extents: X4 renders %ux%u, this run's eye is %ux%u "
                     "(from %s), the composite presents %ux%u -- %s",
                     ci->imageExtent.width, ci->imageExtent.height, eye.width,
                     eye.height, getenv("X4VR_RES") ? "X4VR_RES" : "the "
                     "compiled SBS size", sbs_ci.imageExtent.width,
                     sbs_ci.imageExtent.height,
                     ok ? "they agree" : "THEY DISAGREE, see SPLIT OFF above");
        }
    }
    // The split test is an exact equality, and when it fails everything
    // downstream degrades quietly into "duplicate the left half". Three runs
    // went that way before anyone asked what size X4 had actually requested.
    // Say it, with both numbers and the levers, the first time it happens.
    if (g_sbs_enabled && g_active && g_sbs_split_render && !split) {
        static bool told = false;
        if (!told) {
            told = true;
            X4VR_LOG("sbs: SPLIT OFF — X4 asked for %ux%u but one eye is %ux%u. "
                     "Nothing below this is stereo; the composite will "
                     "duplicate the left half. X4 sizes its render from its "
                     "WINDOW (measured, take thirty-eight): res_width works "
                     "only because it is what X4 asks the window to be, and "
                     "anything else that resizes the window — "
                     "gamescope --force-windows-fullscreen, a compositor, a "
                     "titlebar — overrides it. If the window has to stay at "
                     "display width for input, X4VR_FAKE_EXTENT=1 offers the "
                     "render size through the surface instead.",
                     ci->imageExtent.width, ci->imageExtent.height,
                     // expected_eye(), not the compiled constant. The test
                     // three lines above uses expected_eye(); this message used
                     // X4VR_SBS_WIDTH/2 -- so with X4VR_RES set to anything
                     // else the two disagree, and the line can report "X4 asked
                     // for AxB but one eye is AxB" while declaring SPLIT OFF.
                     // An error message that contradicts itself sends the
                     // diagnosis somewhere else entirely, which is what task
                     // #31 is a list of.
                     eye.width, eye.height);
        }
    }
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
    //
    // wsi= says which of X4's surfaces is the one that presents. It creates
    // more than one -- a Wayland surface and an xcb surface, in take
    // thirty-four -- and only this one is the game's actual output path.
    // Everything the sizing argument rests on is a statement about *this*
    // surface, so it has to be named here rather than assumed to be the
    // surface some earlier line happened to mention.
    X4VR_LOG("swapchain created: %ux%u images>=%u format=%d presentMode=%d "
             "surface %p wsi=%s (pid %d) -> %s",
             ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount,
             (int)ci->imageFormat, (int)ci->presentMode, (void *)ci->surface,
             surface_wsi(ci->surface), (int)getpid(),
             r == VK_SUCCESS ? "ok" : "FAILED");
    if (r == VK_SUCCESS && *out != VK_NULL_HANDLE) {
        // Recorded from sbs_ci, not ci: that is the swapchain that actually
        // exists, and its extent is what its images have.
        std::lock_guard<std::mutex> lock(g_sc_mu);
        g_swapchains[*out].format = sbs_ci.imageFormat;
        g_swapchains[*out].extent = sbs_ci.imageExtent;
    }
    // The injector forces res_width/res_height, but X4 only honours them when
    // borderless is off; with borderless on it sizes to the display and
    // ignores them (observed: identical config gave 2816x1408 under a
    // 2816x1408 gamescope and 3440x1440 on a 3440x1440 desktop). So the
    // config alone does not guarantee the size, and the SBS split needs an
    // exact 2:1 -- an odd size here silently halves into two wrong eyes.
    // Say so loudly, once.
    // g_active only: the layer is loaded in gamescope's process too, and
    // gamescope's own swapchain is the *composite* size, which is legitimately
    // not the eye size. Take 102 warned there before X4 had even started, and
    // that stray line is what made me tell Patola to check for the absence of
    // a warning -- a check the run could not pass for a reason that had nothing
    // to do with it.
    if (r == VK_SUCCESS && g_active) {
        const uint32_t w = ci->imageExtent.width, h = ci->imageExtent.height;
        // X4VR_RES is the authority on what size we asked X4 to render;
        // without it the SBS frame is the target. Warning against the wrong
        // number is worse than not warning.
        const VkExtent2D want = expected_eye();
        const uint32_t want_w = want.width, want_h = want.height;
        static uint32_t warned_w = 0, warned_h = 0;
        if ((w != want_w || h != want_h) &&
            (w != warned_w || h != warned_h)) {
            warned_w = w;
            warned_h = h;
            // Print the size actually compared against, not the compiled-in
            // constant. Take 101 tested `want` = 1408x1408 (from X4VR_RES) and
            // printed "expected 2816x1408" (the constant), which named two
            // numbers that had nothing to do with the comparison and sent the
            // diagnosis looking at the wrong pair of extents.
            X4VR_LOG("WARNING swapchain is %ux%u, expected %ux%u (from %s) -- "
                     "X4 sized to the display, not to res_width/res_height "
                     "(borderless does that). Run under gamescope at %ux%u; "
                     "the SBS split will be wrong otherwise.",
                     w, h, want_w, want_h,
                     getenv("X4VR_RES") ? "X4VR_RES" : "the compiled SBS size",
                     want_w, want_h);
        }
            if (sbs_usage) {
                // Two layers only when multiview is actually running: without
                // it nothing renders a second view, so the extra layer would
                // be memory spent to hold whatever the allocator left there.
                const uint32_t layers =
                    (g_sbs_two_layer && g_mv && g_multiview_supported) ? 2u : 1u;
                g_sbs.add_swapchain(*out, sbs_ci, split, layers,
                                    layers > 1 ? g_sbs_right_layer : 0u);
            }
    }
    return r;
}

void forget_swapchain_images(VkSwapchainKHR sc); // defined with its registrar
void vr_start_session_deferred();               // defined in the VR section

VKAPI_ATTR void VKAPI_CALL x4vr_DestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *ac) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    g_sbs.remove_swapchain(sc); // waits for the device before freeing
    forget_swapchain_images(sc);
    d->DestroySwapchainKHR(device, sc, ac);
}

// ------------------------------------------------------------- WSI probes
//
// The three surface constructors, hooked only to record which one X4 called.
// The create-info structs are declared here rather than pulled in from
// wayland-client.h / xcb.h / Xlib.h: the layer would otherwise gain three
// build dependencies to read one integer. Their layouts are fixed by the
// Vulkan spec, and only the trailing window handle is ever read.
struct X4vrXcbSurfaceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    void *connection;
    uint32_t window; // xcb_window_t
};
struct X4vrXlibSurfaceCreateInfo {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    void *dpy;
    unsigned long window; // Window (XID)
};

using PFN_x4vrCreateSurface = VkResult(VKAPI_PTR *)(
    VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);

// `window` is the X11 window id where there is one; kNoWindow means the
// platform has no such handle (Wayland), which is printed rather than
// silently omitted so the two cases stay distinguishable in the log.
constexpr unsigned long kNoWindow = ~0UL;

VkResult create_surface_observed(VkInstance instance, const char *entry,
                                 const char *wsi, const void *ci,
                                 const VkAllocationCallbacks *ac,
                                 VkSurfaceKHR *out, unsigned long window) {
    PFN_x4vrCreateSurface next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(instance));
        if (it == g_instances.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        next = (PFN_x4vrCreateSurface)it->second.gipa(instance, entry);
    }
    if (!next)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = next(instance, ci, ac, out);
    if (r != VK_SUCCESS || !out)
        return r;
    note_surface_wsi(*out, wsi);
    // The handle is printed because the label alone cannot be checked. Take
    // thirty-five has X4 creating a Wayland surface and an xcb surface, then
    // presenting on the one labelled Wayland while SDL's video driver is x11 --
    // which is either true and strange, or a handle this map has mixed up.
    // Without the values there is no way to tell those apart.
    if (window == kNoWindow)
        X4VR_LOG("wsi: surface %p created via %s (pid %d)", (void *)*out, entry,
                 (int)getpid());
    else
        X4VR_LOG("wsi: surface %p created via %s, window 0x%lx (pid %d)",
                 (void *)*out, entry, window, (int)getpid());
    log_sdl_backend_once();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateWaylandSurfaceKHR(
    VkInstance instance, const void *ci, const VkAllocationCallbacks *ac,
    VkSurfaceKHR *out) {
    return create_surface_observed(instance, "vkCreateWaylandSurfaceKHR",
                                   "wayland", ci, ac, out, kNoWindow);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateXcbSurfaceKHR(
    VkInstance instance, const void *ci, const VkAllocationCallbacks *ac,
    VkSurfaceKHR *out) {
    const auto *info = (const X4vrXcbSurfaceCreateInfo *)ci;
    return create_surface_observed(instance, "vkCreateXcbSurfaceKHR", "xcb", ci,
                                   ac, out, info ? info->window : kNoWindow);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateXlibSurfaceKHR(
    VkInstance instance, const void *ci, const VkAllocationCallbacks *ac,
    VkSurfaceKHR *out) {
    const auto *info = (const X4vrXlibSurfaceCreateInfo *)ci;
    return create_surface_observed(instance, "vkCreateXlibSurfaceKHR", "xlib",
                                   ci, ac, out, info ? info->window : kNoWindow);
}

VKAPI_ATTR void VKAPI_CALL x4vr_DestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *ac) {
    PFN_vkDestroySurfaceKHR next = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end())
            next = (PFN_vkDestroySurfaceKHR)it->second.gipa(
                instance, "vkDestroySurfaceKHR");
    }
    X4VR_LOG("wsi: surface %p destroyed (was %s, pid %d)", (void *)surface,
             surface_wsi(surface), (int)getpid());
    forget_surface(surface);
    if (next)
        next(instance, surface, ac);
}

// "Can I present on this surface, from this queue family?"
//
// X4 creates a Wayland surface and an xcb surface and then, on take
// thirty-five, appears to present on the Wayland one while SDL's video driver
// is x11. If it asked this question of the xcb surface and got VK_FALSE, that
// is the entire explanation and it is one line away; if it never asked, the
// choice was made some other way and the search moves elsewhere. Either answer
// is worth more than another round of reasoning about it.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_GetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice phys, uint32_t family, VkSurfaceKHR surface,
    VkBool32 *supported) {
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR next;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(phys));
        if (it == g_instances.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        next = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)it->second.gipa(
            it->second.instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    }
    if (!next)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = next(phys, family, surface, supported);
    // Every call, not the first: the question is asked once per (surface,
    // family) pair and the interesting answer is whichever one is VK_FALSE.
    // A once-per-process flag is what hid the second surface last take.
    X4VR_LOG("wsi: present support? surface %p (%s) family %u -> %s (pid %d)",
             (void *)surface, surface_wsi(surface), family,
             r != VK_SUCCESS      ? "query FAILED"
             : (supported && *supported) ? "YES"
                                         : "NO",
             (int)getpid());
    return r;
}

// The *other* way to ask the same question.
//
// X4 enables VK_KHR_get_surface_capabilities2, and this entry point was never
// hooked -- so a caps query made through it has always bypassed the halving
// lever entirely. That matters beyond the missing log line: the "two levers"
// story told throughout this code says X11 sizes X4 by halved capabilities,
// and if X4 asks through the 2-variant then that lever has never once fired
// and the story was never tested.
//
// Deliberately observation-only for now. Halving here would change how X4
// sizes itself, on the very run meant to establish what it currently does,
// and the split render took four takes to stabilise. Measure first.
VKAPI_ATTR VkResult VKAPI_CALL x4vr_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice phys, const VkPhysicalDeviceSurfaceInfo2KHR *info,
    VkSurfaceCapabilities2KHR *caps) {
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR next;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(phys));
        if (it == g_instances.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        next = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)it->second.gipa(
            it->second.instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    }
    if (!next)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = next(phys, info, caps);
    if (r != VK_SUCCESS || !info || !caps)
        return r;
    if (note_surface_seen(info->surface))
        X4VR_LOG("sbs: surface %p caps2 currentExtent=%ux%u min=%ux%u max=%ux%u "
                 "wsi=%s (pid %d) — NOT halved, this path is observation only",
                 (void *)info->surface,
                 caps->surfaceCapabilities.currentExtent.width,
                 caps->surfaceCapabilities.currentExtent.height,
                 caps->surfaceCapabilities.minImageExtent.width,
                 caps->surfaceCapabilities.minImageExtent.height,
                 caps->surfaceCapabilities.maxImageExtent.width,
                 caps->surfaceCapabilities.maxImageExtent.height,
                 surface_wsi(info->surface), (int)getpid());
    return r;
}

// What actually sizes X4's pipeline, corrected by take thirty-eight.
//
// This comment used to open "X4 sizes its whole pipeline from the surface's
// currentExtent". That was never measured on this machine's path, because the
// surface here always answers 0xFFFFFFFF and the halving branch never ran.
// Take thirty-eight tested it by accident: gamescope's
// --force-windows-fullscreen made X4's X11 window 2816 wide, and X4 asked for
// a 2816-wide swapchain -- with res_width still 1408 and currentExtent still
// 0xFFFFFFFF. **X4 sizes from its window.** res_width matters only because it
// is what X4 asks the window to be; when something else resizes the window,
// X4 follows it.
//
// That makes the window a single knob for two things we need to differ: it is
// the render size *and* the input space. Setting it to the display width
// fixes the 704 input offset and costs the split render, which is exactly
// what take thirty-eight showed on screen -- pointer spanning the full width,
// two left halves.
//
// So the render size has to come from somewhere that is not the window. The
// Vulkan idiom is the obvious candidate and it has never been tried here:
// almost every engine reads currentExtent and uses its own size only when the
// answer is 0xFFFFFFFF. X4 has only ever been shown the 0xFFFFFFFF branch. If
// it honours a real answer, reporting the eye extent sizes the render while
// leaving the window alone -- and if it does not, then X4 ignores the surface
// entirely and this whole comment, in both its versions, was fiction.
//
// X4VR_FAKE_EXTENT=1 is that experiment. Reporting half of a real extent (the
// original design, for a surface that states a size) is kept separate below.
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

    // 0xFFFFFFFF means "the surface has no preferred size": the app is invited
    // to pick, and there is no real extent to halve.
    const uint32_t was = caps->currentExtent.width;
    // Once per *surface*, not once per process.
    //
    // Take thirty-four: X4 creates a Wayland surface and an xcb surface
    // milliseconds apart. A one-shot flag logged the first and hid the second,
    // so the log said "wsi=wayland" while SDL's driver was x11 -- and I read
    // that as "X4 is on Wayland" and rebuilt the whole diagnosis on it. The
    // hole this task was opened to fix was inference from a sentinel; the fix
    // reproduced it one level down, because a first sample is a sentinel for
    // the set when the set has more than one member.
    if (note_surface_seen(surface)) {
        // The pid disambiguates whose surface this is. The layer loads into
        // gamescope as well as into X4, X4VR_LOG is one append-only file shared
        // by both, and take thirty-one could not tell from this line whether
        // the Wayland surface reporting no preferred extent was X4's or
        // gamescope's own -- which is the difference between "force the driver"
        // and "nothing is wrong here".
        X4VR_LOG("sbs: surface %p caps currentExtent=%ux%u min=%ux%u max=%ux%u "
                 "wsi=%s (pid %d)",
                 (void *)surface, caps->currentExtent.width,
                 caps->currentExtent.height, caps->minImageExtent.width,
                 caps->minImageExtent.height, caps->maxImageExtent.width,
                 caps->maxImageExtent.height, surface_wsi(surface),
                 (int)getpid());
    }
    if (was == 0xFFFFFFFFu && g_fake_extent) {
        // Invent the answer the surface declined to give. Nothing is being
        // halved here -- there was no number to halve; we are supplying the
        // eye extent so that X4 has a source for its render size other than
        // its window, which we need to keep at display width for input.
        // expected_eye(), not the compiled constant: X4VR_RES is what the
        // launcher tells X4 to render, and offering a different number here
        // would make the surface lever and the config lever disagree about the
        // eye in the same run. Latent until now only because this path needs
        // X4VR_FAKE_EXTENT=1, which is off by default -- and the SPLIT OFF
        // message above recommends exactly that knob, so a person following
        // the advice with a non-default X4VR_RES would have been handed the
        // wrong extent by the fix.
        const VkExtent2D fe = expected_eye();
        caps->currentExtent.width = fe.width;
        caps->currentExtent.height = fe.height;
        if (note_halved_surface(surface))
            X4VR_LOG("sbs: surface %p had no preferred extent — reporting "
                     "%ux%u (the eye) so the render size stops following the "
                     "window. If X4 still asks for the window width, it does "
                     "not consult the surface at all.",
                     (void *)surface, caps->currentExtent.width,
                     caps->currentExtent.height);
        return r;
    }
    if (was == 0xFFFFFFFFu || was < 2) {
        forget_halved_surface(surface);
        static bool told = false;
        if (!told) {
            told = true;
            X4VR_LOG("sbs: surface reports no preferred extent "
                     "(currentExtent=0x%X), wsi=%s. There is nothing to halve, "
                     "so the layer's surface lever is inert and only the "
                     "config's res_width sizes X4. On Wayland the buffer *is* "
                     "the surface, so presenting a full-width image makes the "
                     "surface full width while X4 still believes it owns the "
                     "eye — that gap is the input offset measured in take "
                     "thirty-three, not a thing to paper over here.",
                     was, surface_wsi(surface));
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
// Give the images X4 will render the frame into a serial like any other.
//
// They arrive through vkGetSwapchainImagesKHR, not vkCreateImage, so they never
// entered g_images -- and every instrument keyed by serial printed `?` for
// them. The framebuffer log has said `fb rp #0: ... imgs=[?]` since the
// beginning; it was noted as a gap and left, because nothing then depended on
// naming those images.
//
// Take twenty-seven depended on it. "No render pass writes #100, so the merge
// is not a draw" was reasoning over a writer list that structurally could not
// contain the one pass that matters: rp #0 and rp #1 draw the finished frame
// straight into these. The sentinel did not just omit information, it made a
// false conclusion look supported.
void register_swapchain_images(VkSwapchainKHR sc, const VkImage *images,
                               uint32_t n, VkFormat fmt, VkExtent2D extent) {
    if (!g_active)
        return;
    std::lock_guard<std::mutex> lock(g_img_mu);
    for (uint32_t i = 0; i < n; i++) {
        if (images[i] == VK_NULL_HANDLE || g_images.count(images[i]))
            continue;
        ImageInfo info{};
        info.extent = {extent.width, extent.height, 1};
        info.format = fmt;
        info.layers = 1;
        info.mips = 1;
        info.samples = 1;
        // Not what vkCreateImage would have reported -- it was never called.
        // COLOR_ATTACHMENT is what the swapchain guarantees and what makes the
        // writer list correct; anything more would be invention.
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.doubled = false;
        info.serial = g_img_serial++;
        info.swapchain = true;
        g_images[images[i]] = info;
        if (g_mv_inventory)
            X4VR_LOG("img #%u: %ux%u fmt=%u SWAPCHAIN (image %u of %u)",
                     info.serial, extent.width, extent.height, (unsigned)fmt, i,
                     n);
    }
    {
        std::lock_guard<std::mutex> lock(g_sc_mu);
        auto &si = g_swapchains[sc];
        si.images.assign(images, images + n);
    }
}

// The SBS eye images: X4's render target when the layer virtualizes the
// swapchain. Registered like swapchain images, but they are ours, so we know
// their real layer count -- and a two-layer one is genuinely `doubled`, which
// is what lets the framebuffer path build an array view over it and what makes
// masking the composite possible at all.
void register_eye_images(VkSwapchainKHR sc, const VkImage *images, uint32_t n,
                         const x4vr::SbsCompositor::EyeInfo &ei) {
    if (!g_active)
        return;
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        for (uint32_t i = 0; i < n; i++) {
            if (images[i] == VK_NULL_HANDLE || g_images.count(images[i]))
                continue;
            ImageInfo info{};
            info.extent = {ei.extent.width, ei.extent.height, 1};
            info.format = ei.format;
            info.layers = ei.layers;
            info.mips = 1;
            info.samples = 1;
            // What make_eye_images actually asked for. Unlike a real swapchain
            // image this one has a create-info we wrote, so nothing here is
            // inferred.
            info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.doubled = ei.layers > 1;
            info.serial = g_img_serial++;
            info.swapchain = true;
            g_images[images[i]] = info;
            if (info.doubled)
                g_per_eye_images.insert(images[i]);
            if (g_mv_inventory)
                X4VR_LOG("img #%u: %ux%u fmt=%u layers=%u EYE (image %u of %u)%s",
                         info.serial, ei.extent.width, ei.extent.height,
                         (unsigned)ei.format, ei.layers, i, n,
                         info.doubled ? " doubled" : " single-layer");
        }
    }
    std::lock_guard<std::mutex> lock(g_sc_mu);
    g_swapchains[sc].images.assign(images, images + n);
}

// Forget a dead swapchain's images so a recycled handle registers afresh
// instead of inheriting the old swapchain's serial, extent and format.
void forget_swapchain_images(VkSwapchainKHR sc) {
    std::vector<VkImage> images;
    {
        std::lock_guard<std::mutex> lock(g_sc_mu);
        auto it = g_swapchains.find(sc);
        if (it == g_swapchains.end())
            return;
        images.swap(it->second.images);
        g_swapchains.erase(it);
    }
    std::lock_guard<std::mutex> lock(g_img_mu);
    for (VkImage im : images) {
        auto it = g_images.find(im);
        // Only ever drop an entry this layer created for a swapchain. If X4
        // somehow handed us a handle from vkCreateImage, leaving it is the
        // conservative error.
        if (it != g_images.end() && it->second.swapchain)
            g_images.erase(it);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_GetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR sc, uint32_t *count, VkImage *images) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    const std::vector<VkImage> *eyes =
        (g_sbs_enabled && g_active) ? g_sbs.eye_images(sc) : nullptr;
    if (!eyes) {
        const VkResult r = d->GetSwapchainImagesKHR(device, sc, count, images);
        if (r >= 0 && images && count) {
            SwapchainInfo si{};
            {
                std::lock_guard<std::mutex> lock(g_sc_mu);
                auto it = g_swapchains.find(sc);
                if (it != g_swapchains.end())
                    si = it->second;
            }
            register_swapchain_images(sc, images, *count, si.format,
                                      si.extent);
        }
        return r;
    }
    if (!images) {
        *count = (uint32_t)eyes->size();
        return VK_SUCCESS;
    }
    const uint32_t n =
        *count < eyes->size() ? *count : (uint32_t)eyes->size();
    for (uint32_t i = 0; i < n; i++)
        images[i] = (*eyes)[i];
    // These are the images the whole frame is built in when SBS is on, and
    // until now they reached X4 untracked -- so every serial-keyed instrument
    // printed `?` for them. That is the sentinel that hid the composite for
    // twenty-seven takes, in a second place, and it is also a hard prerequisite
    // for masking the composite: the framebuffer path refuses to build an array
    // view over an image it does not know is doubled.
    x4vr::SbsCompositor::EyeInfo ei{};
    if (g_sbs.eye_info(sc, &ei))
        register_eye_images(sc, images, n, ei);
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
    // The first thing X4 does after vkCreateDevice returns, and the earliest
    // point at which the loader is not inside its own device-creation chain.
    // See vr_app_level_physical_device for why that matters.
    vr_start_session_deferred();
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
    vr_start_session_deferred(); // X4 may use either spelling; it is idempotent
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
// Which image went into which, by what call. The frame's writer list is built
// from render passes only, so a merge performed by a blit or a copy leaves no
// trace in it at all -- and #103 (the HUD) and the scene images are joined by
// *something* that is not a draw. Counting the widened transfers, which is all
// this did before, says how many happened but never which images they touched.
void note_transfer(VkImage src, VkImage dst, const char *what, uint32_t n,
                   uint32_t widened) {
    if (!g_mv_inventory || !g_active)
        return;
    uint32_t s = UINT32_MAX, t = UINT32_MAX;
    {
        std::lock_guard<std::mutex> lock(g_img_mu);
        auto a = g_images.find(src);
        if (a != g_images.end())
            s = a->second.serial;
        auto b = g_images.find(dst);
        if (b != g_images.end())
            t = b->second.serial;
    }
    std::lock_guard<std::mutex> lock(g_xfer_mu);
    // Buffer->image is texture streaming: hundreds of distinct destinations,
    // one per asset, and never a merge. Take twenty-seven let them share one
    // budget with the image->image edges and they took all 256 of it, so the
    // single edge that mattered was one line in a wall. They are counted in
    // aggregate instead; only image->image keeps per-edge detail, and there
    // are single digits of those.
    if (src == VK_NULL_HANDLE) {
        g_xfer_uploads += n;
        g_xfer_upload_targets.insert(t);
        return;
    }
    if (g_xfer_edges.size() >= 256 && !g_xfer_edges.count({s, t}))
        return;
    auto &e = g_xfer_edges[{s, t}][what];
    e.n += n;
    e.widened += widened;
}

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
    note_transfer(src, dst, "vkCmdCopyImage", n, w);
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
    note_transfer(src, dst, "vkCmdBlitImage", n, w);
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
    note_transfer(src, dst, "vkCmdResolveImage", n, w);
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
    note_transfer(info->srcImage, info->dstImage, "vkCmdCopyImage2",
                  info->regionCount, w);
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
    note_transfer(info->srcImage, info->dstImage, "vkCmdBlitImage2",
                  info->regionCount, w);
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
    note_transfer(info->srcImage, info->dstImage, "vkCmdResolveImage2",
                  info->regionCount, w);
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
    note_transfer(VK_NULL_HANDLE, dst, "vkCmdCopyBufferToImage", n, 0);
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
    note_transfer(VK_NULL_HANDLE, info->dstImage,
                  "vkCmdCopyBufferToImage2", info->regionCount, 0);
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
    g_cb_rp[cb] = rp;
}

void cb_leave_pass(VkCommandBuffer cb) {
    if (!g_mv || !g_active)
        return;
    std::lock_guard<std::mutex> lock(g_cb_mu);
    g_cb_mask.erase(cb);
    g_cb_rp.erase(cb);
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
    uint32_t rp_serial = UINT32_MAX;   // the pass whose end this capture follows
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
// The injector's channel, resolved by name from the process's global symbols.
// Absent whenever the injector is not preloaded -- gamescope's process being
// the normal case -- and that is not an error, it just means there is no
// pointer to draw. Resolved once and remembered, including the failure, so a
// missing injector costs one dlsym rather than one per frame.
const x4vr::Shared *shared_state() {
    static const x4vr::Shared *s = [] () -> const x4vr::Shared * {
        auto fn = (x4vr::Shared * (*)())dlsym(RTLD_DEFAULT, "x4vr_shared_state");
        if (!fn) {
            X4VR_LOG("share: no injector in this process — no cursor channel");
            return nullptr;
        }
        x4vr::Shared *p = fn();
        if (!p || p->magic != x4vr::kShareMagic ||
            p->version != x4vr::kShareVersion) {
            X4VR_LOG("share: symbol found but magic/version mismatch — "
                     "injector and layer are from different builds");
            return nullptr;
        }
        X4VR_LOG("share: injector channel v%u connected", x4vr::kShareVersion);
        return p;
    }();
    return s;
}

// X4VR_CURSOR — blend X4's own pointer into the eye image (task #17).
//
// **On by default since takes 95/96**, which confirmed it against an exhaustive
// exercise of X4's map. Same precedent as X4VR_PROJ_INVPROJ after take 83: a
// confirmed correction becomes the behaviour, and reproducing a take from before
// it has to say so explicitly.
//
// The knob gates *configuration*, not a per-frame branch: with X4VR_CURSOR=0 the
// overlay is never configured, so it is never ready and the present path costs
// nothing.
const bool g_cursor_enabled = x4vr::env_on("X4VR_CURSOR", true);

const char *g_mv_dump = getenv("X4VR_MV_DUMP");
// X4VR_MV_DUMP_PRESENT=N — write the finished eye image every N presents.
// Distinct from X4VR_MV_DUMP_IMG, which names images to catch at end-of-pass
// and therefore cannot see anything drawn by a later pass into the same image.
const uint64_t g_dump_present_every = [] {
    const char *e = getenv("X4VR_MV_DUMP_PRESENT");
    if (!e || !*e)
        return (uint64_t)0;
    const long long n = atoll(e);
    return n > 0 ? (uint64_t)n : (uint64_t)0;
}();

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

// Two-channel half-float: X4's #60 and #61, which are the G-buffer's packed
// normals and are therefore a prime suspect for a *shading input* that differs
// per eye. The probe has always been able to read them -- format_bpp() covers
// R16G16_SFLOAT -- so the only reason they could not be looked at was this
// function, exactly as with the 8-bit targets below.
//
// These carry negative values, so the unsigned tone map above would clip half
// the range to black. Use its signed analogue, v/(1+|v|) remapped to [0,1]:
// monotonic over the whole real line, 0 maps to mid-grey, and no value is
// clipped. Blue is left at zero so the encoding cannot be mistaken for colour.
void write_ppm_rg(const char *path, const uint16_t *px, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint32_t c = 0; c < 2; c++) {
                const float v = half_to_float(px[((size_t)y * w + x) * 2 + c]);
                const float s = 0.5f + 0.5f * (v / (1.0f + fabsf(v)));
                row[(size_t)x * 3 + c] = (uint8_t)(s * 255.0f + 0.5f);
            }
            row[(size_t)x * 3 + 2] = 0;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

// The 8-bit BGRA targets -- the whole late half of the frame, #103 included.
// Their absence here is why the one image that most needed looking at was the
// one the dumper could not write.
void write_ppm8(const char *path, const uint8_t *px, uint32_t w, uint32_t h,
                bool bgra) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row((size_t)w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            for (uint32_t c = 0; c < 3; c++)
                row[(size_t)x * 3 + c] = bgra ? p[2 - c] : p[c];
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
        uint32_t rp_serial = UINT32_MAX;
        {
            std::lock_guard<std::mutex> lock(g_cb_mu);
            auto m = g_cb_mask.find(cb);
            masked = m != g_cb_mask.end() && m->second;
            auto r = g_cb_rp.find(cb);
            if (r != g_cb_rp.end()) {
                std::lock_guard<std::mutex> vl(g_variants.mu);
                auto s = g_rp_serials.find(r->second);
                if (s != g_rp_serials.end())
                    rp_serial = s->second;
            }
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
                        g_probe.rp_serial = rp_serial;
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
    // Said once: this instrument is not free and it is not cheap. Every sample
    // drains the GPU and then walks both layers on the CPU -- about 16 MB of
    // hashing and per-texel comparison. Take 99 sampled every 4.87 s and the
    // game stuttered on exactly that period: half a second of motion, then
    // seconds of nothing. That is fine for a measurement take and fatal for one
    // that has to hit a target with the mouse, so the log says so rather than
    // leaving it to be rediscovered.
    static bool said = false;
    if (!said) {
        said = true;
        X4VR_LOG("mv probe: each sample drains the queue and reads both layers "
                 "on the CPU — expect visible stalls. Leave X4VR_MV_PROBE unset "
                 "for any run that has to be interacted with.");
    }
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
    // Is the layer one texel repeated? All-zero is the special case that was
    // already caught; every *other* constant clear was not, and read as
    // content instead.
    //
    // #101 cost an investigation to this. Three of its four agreeing captures
    // were the whole 1408x1408 R8_UINT image filled with byte 0x10 -- a
    // cleared mask -- but because the test was "is any byte non-zero", they
    // counted as real content whose two layers happened to match, and the
    // image read as mysteriously intermittent. Two layers that were merely
    // cleared to the same value agree trivially, and taking that as evidence
    // of mono behaviour is exactly how a mono target passes for a stereo one.
    // The verdict this instrument exists to give is about to gate the tonemap
    // change, so it has to be able to say "nothing was in here".
    bool u0 = true, u1 = true;
    // Mean level per layer.
    //
    // DIFFER counts texels that differ; it cannot say *how* they differ, and
    // takes 55 and 56 turned on exactly that distinction. The screenshots show
    // the right eye's cockpit panels at 1.7-1.9x the left eye's brightness
    // while the background matches to 0.2% -- so one layer is systematically
    // brighter, which a pixel count cannot express and which sent two
    // diagnoses down the wrong road.
    //
    // Summing the first component is deliberately crude: for the HDR and
    // 8-bit colour formats in this chain it is the red channel, which is
    // enough to say "layer 1 is brighter here and not there" and so to name
    // the first buffer in the chain where the two eyes diverge in level rather
    // than merely in content. That name is the thing worth having; a
    // photometrically correct luminance would not make it any more of a name.
    double sum0 = 0.0, sum1 = 0.0;
    const bool f16 = g_probe.format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                     g_probe.format == VK_FORMAT_R16_SFLOAT ||
                     g_probe.format == VK_FORMAT_R16G16_SFLOAT;
    auto level = [&](const uint8_t *p) -> double {
        if (f16) {
            uint16_t h;
            memcpy(&h, p, 2);
            // half -> float, enough for a mean: sign/exp/mantissa by hand so
            // this needs no <cmath> conversion helper or compiler extension.
            const int sgn = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
            double v;
            if (exp == 0)
                v = man * 5.9604645e-8;
            else if (exp == 31)
                v = 65504.0; // inf/nan clamped: a mean is not the place to care
            else
                v = (1.0 + man / 1024.0) * std::pow(2.0, exp - 15);
            return sgn ? -v : v;
        }
        return (double)p[0];
    };
    static const uint8_t zero[16] = {0};
    for (size_t t = 0; t * bpp < n; t++) {
        sum0 += level(l0 + t * bpp);
        sum1 += level(l1 + t * bpp);
        const bool a = memcmp(l0 + t * bpp, zero, bpp) != 0;
        const bool b = memcmp(l1 + t * bpp, zero, bpp) != 0;
        if (a)
            nz0++;
        if (b)
            nz1++;
        if (u0 && memcmp(l0 + t * bpp, l0, bpp) != 0)
            u0 = false;
        if (u1 && memcmp(l1 + t * bpp, l1, bpp) != 0)
            u1 = false;
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
    // "(all zero)" is kept verbatim for the case it always named, so every
    // existing log, doc reference and test grep still reads the same.
    char ann0[64], ann1[64];
    auto annotate = [&](char *out, size_t cap, const uint8_t *p, bool z,
                        bool u) {
        if (z) {
            snprintf(out, cap, " (all zero)");
            return;
        }
        if (!u || !total) {
            out[0] = 0;
            return;
        }
        int k = snprintf(out, cap, " (uniform 0x");
        for (uint32_t b = 0; b < bpp && k > 0 && (size_t)k + 2 < cap; b++)
            k += snprintf(out + k, cap - k, "%02x", p[b]);
        snprintf(out + k, cap - k, ")");
    };
    annotate(ann0, sizeof ann0, l0, z0, u0);
    annotate(ann1, sizeof ann1, l1, z1, u1);
    // Dumped before the DIFFER/IDENTICAL split, because "why is this one
    // mono?" is a question about an image that does *not* differ, and gating
    // the dump on DIFFER meant the instrument could only ever show what was
    // already known. X4VR_MV_DUMP_IMG names the image; without it the old
    // first-differing-image behaviour stands.
    // Sequence-numbered, not one-shot. Take twenty-five: the first probe of
    // #103 lands in the start menu, so a single dump could only ever show the
    // menu -- and in the menu "UI layer" and "final composite" are the same
    // picture, which is exactly the distinction the dump was for. Capped so a
    // long run cannot fill /tmp.
    // A comma-separated list, and the cap is *per image*. Bisecting the frame
    // means holding several images from the same run side by side: serials are
    // per-run, so #55 from one take and #57 from the next cannot be compared,
    // and one image per run would need five runs of a moving scene to collect
    // what one run collects at a single view.
    const char *want = getenv("X4VR_MV_DUMP_IMG");
    const bool have_want = want && *want;
    bool named = false;
    for (const char *s = want; have_want && s && *s;) {
        char *end = nullptr;
        const unsigned long v = strtoul(s, &end, 10);
        if (end == s)
            break;
        if ((uint32_t)v == g_probe.serial) {
            named = true;
            break;
        }
        s = (*end == ',') ? end + 1 : end;
        while (*s == ' ')
            s++;
    }
    const uint32_t kMaxDumps = 6;
    static std::mutex dump_mu;
    static std::unordered_map<uint32_t, uint32_t> dumps_by_img;
    uint32_t dumps = 0;
    {
        std::lock_guard<std::mutex> lock(dump_mu);
        dumps = dumps_by_img[g_probe.serial];
    }
    if (g_mv_dump && dumps < (named ? kMaxDumps : 1u) &&
        (named || (!have_want && h0 != h1))) {
        const bool hdr = g_probe.format == VK_FORMAT_R16G16B16A16_SFLOAT;
        const bool rg16f = g_probe.format == VK_FORMAT_R16G16_SFLOAT;
        const bool bgra8 = g_probe.format == VK_FORMAT_B8G8R8A8_SRGB ||
                           g_probe.format == VK_FORMAT_B8G8R8A8_UNORM;
        const bool rgba8 = g_probe.format == VK_FORMAT_R8G8B8A8_SRGB ||
                           g_probe.format == VK_FORMAT_R8G8B8A8_UNORM;
        if (hdr || rg16f || bgra8 || rgba8) {
            uint32_t seq;
            {
                std::lock_guard<std::mutex> lock(dump_mu);
                seq = dumps_by_img[g_probe.serial]++;
            }
            char p[512];
            for (int L = 0; L < 2; L++) {
                const void *src = L ? l1 : l0;
                snprintf(p, sizeof p, "%s-img%u-n%u-layer%d.ppm", g_mv_dump,
                         g_probe.serial, seq, L);
                if (hdr)
                    write_ppm(p, (const uint16_t *)src, g_probe.w, g_probe.h);
                else if (rg16f)
                    write_ppm_rg(p, (const uint16_t *)src, g_probe.w,
                                 g_probe.h);
                else
                    write_ppm8(p, (const uint8_t *)src, g_probe.w, g_probe.h,
                               bgra8);
            }
            // The pass is named because #103 has two writers and they need not
            // hold the same thing: reading "after rp #40" and "after rp #52"
            // as one measurement is how an intermediate state gets mistaken
            // for the finished frame.
            X4VR_LOG("mv probe: wrote %s-img%u-n%u-layer{0,1}.ppm "
                     "(fmt %u, after rp #%u, %s)",
                     g_mv_dump, g_probe.serial, seq, (unsigned)g_probe.format,
                     g_probe.rp_serial, h0 != h1 ? "DIFFER" : "IDENTICAL");
        } else if (named) {
            // Named but unwritable is a fact worth one line: silence here
            // would read as "the probe never reached that image".
            X4VR_LOG("mv probe: img #%u requested for dump but format %u is "
                     "not one this writes",
                     g_probe.serial, (unsigned)g_probe.format);
        }
    }
    const double mean0 = total ? sum0 / (double)total : 0.0;
    const double mean1 = total ? sum1 / (double)total : 0.0;
    // Printed on both branches: an IDENTICAL image has equal means by
    // construction, and seeing that stated is what makes the ratio on the
    // differing ones mean something.
    char lvl[96];
    snprintf(lvl, sizeof lvl, "  level %.4g/%.4g (l1/l0 %.3f)", mean0, mean1,
             mean0 != 0.0 ? mean1 / mean0 : 0.0);
    if (h0 == h1) {
        X4VR_LOG("mv probe: img #%u %ux%u  layer0=%016llx%s  "
                 "layer1=%016llx%s  IDENTICAL%s",
                 g_probe.serial, g_probe.w, g_probe.h, (unsigned long long)h0,
                 ann0, (unsigned long long)h1, ann1, lvl);
    } else {
        X4VR_LOG("mv probe: img #%u %ux%u  layer0=%016llx%s  "
                 "layer1=%016llx%s  DIFFER %zu/%zu (%.2f%%)  "
                 "non-empty %zu/%zu  missing=%zu changed=%zu extra=%zu  "
                 "first at (%u,%u)%s",
                 g_probe.serial, g_probe.w, g_probe.h, (unsigned long long)h0,
                 ann0, (unsigned long long)h1, ann1, dtex, total,
                 total ? 100.0 * (double)dtex / (double)total : 0.0, nz0, nz1,
                 missing, changed, extra,
                 g_probe.w ? (uint32_t)(first % g_probe.w) : 0,
                 g_probe.w ? (uint32_t)(first / g_probe.w) : 0, lvl);
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
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE && g_mv_inventory && g_active) {
        std::lock_guard<std::mutex> lock(g_comp_mu);
        g_comp_bound[cb] = p;
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

// Task #38, defined with the rest of the VR section further down. Declared
// here because the present hook is what drives them and it comes first in the
// file; both compile to no-ops when the layer is built without OpenXR headers.
#ifdef X4VR_HAVE_OPENXR
bool vr_begin_blit(VkSwapchainKHR x4sc);
void vr_finish_blit();
#else
inline bool vr_begin_blit(VkSwapchainKHR) { return false; }
inline void vr_finish_blit() {}
#endif

VKAPI_ATTR VkResult VKAPI_CALL x4vr_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pi) {
    // Backstop. The session normally starts at the first vkGetDeviceQueue, but
    // an application that reached a present without calling either spelling of
    // it would otherwise never get one, and "no session" would be reported for
    // a reason that has nothing to do with the runtime.
    vr_start_session_deferred();
    // #36: everything below that touches the queue -- the SBS composite, the
    // cursor overlay, the readback's QueueWaitIdle, and the present itself --
    // is inside this scope, so one lock covers X4's whole side of the shared
    // queue. A std::unique_lock rather than a guard because it must be taken
    // conditionally.
    // Task #38: acquire the runtime's swapchain image BEFORE taking the queue
    // lock, and never inside it.
    //
    // The runtime frees swapchain images in xrEndFrame, and xrEndFrame needs
    // this same mutex — so a thread that waits for an image while holding it
    // is waiting for something that cannot happen until it lets go. With
    // XR_INFINITE_DURATION that is a hard deadlock and it took X4's present
    // thread down with it: take 114b, black in both the headset and the
    // window. A bounded wait turns it into "no VR frame ever arrives, 2 ms
    // wasted per present", which is quieter and just as wrong.
    //
    // The acquire touches no Vulkan queue, so it does not belong under this
    // lock at all. The release does not either, but it must follow the
    // composite's submit, so it stays below.
    bool vr_blit = false;
    if (g_sbs_enabled && g_active && pi->swapchainCount == 1)
        vr_blit = vr_begin_blit(pi->pSwapchains[0]);

    VrQueueLock qlock;
    if (g_active) {
        static bool once = false;
        if (!once) {
            once = true;
            mv_report("first present");
            bindless_report("first present");
            canvas_report("first present");
            // Everything written from here on is a *late* write, which is what
            // says whether mirroring can be done once at startup or has to
            // track X4 for the whole session.
            if (g_bindless_survey) {
                std::lock_guard<std::mutex> lock(g_desc_mu);
                g_desc_first_frame_done = true;
            }
        }
    }
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(queue));
    }
    if (g_mv_probe && g_mv && g_active)
        probe_collect(d, queue);

    // Task #29: the finished eye image.
    //
    // The end-of-render-pass probe cannot produce this. It fires after the
    // *first* pass that writes a given image, and the eye image is written by
    // several present passes (P76: rp #0/#1/#4/#7/#10), so what it captures is
    // the frame before the UI -- and the cursor, the HUD and the logo are all
    // drawn after that. Every question about them was unanswerable from a dump.
    //
    // Here the frame is complete: X4 has submitted everything and is asking for
    // it to be shown. The copy is recorded by the compositor, which already
    // owns the only correct transition of this image.
    VkDeviceSize dump_layer_bytes = 0;
    uint32_t dump_layers = 0, dump_w = 0, dump_h = 0;
    bool dump_bgra = false;
    if (g_dump_present_every && g_active && pi->swapchainCount == 1) {
        // Said once, loudly, because this knob reads like a boolean and is a
        // cadence. Take 98 was run with =1 and dumped every frame: a ~12 MB
        // readback plus a full pipeline stall per present took the game to
        // about 1 fps, which made the run almost impossible to interact with
        // and cost the interaction half of what it was measuring. The number
        // is the cost, so the log states it in the units that hurt.
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("mv dump: writing the eye image every %llu present(s) — "
                     "at 1 this is a full readback and stall on EVERY frame "
                     "(~1 fps). Use a few hundred unless you need every frame.",
                     (unsigned long long)g_dump_present_every);
        }
        static uint64_t presents = 0;
        if (presents++ % g_dump_present_every == 0) {
            x4vr::SbsCompositor::EyeInfo ei;
            if (g_sbs.eye_info(pi->pSwapchains[0], &ei)) {
                const uint32_t bpp = format_bpp(ei.format);
                if (bpp) {
                    const VkDeviceSize per =
                        (VkDeviceSize)ei.extent.width * ei.extent.height * bpp;
                    const uint32_t n = ei.layers > 2 ? 2 : ei.layers;
                    std::lock_guard<std::mutex> lock(g_probe_mu);
                    if (probe_buffer_ready(d, d->device, per * n)) {
                        g_sbs.request_dump(g_probe.buf, per);
                        dump_layer_bytes = per;
                        dump_layers = n;
                        dump_w = ei.extent.width;
                        dump_h = ei.extent.height;
                        dump_bgra = ei.format == VK_FORMAT_B8G8R8A8_UNORM ||
                                    ei.format == VK_FORMAT_B8G8R8A8_SRGB;
                    }
                }
            }
        }
    }

    // One swapchain is all X4 presents; anything else is not a case we have
    // seen, so leave it alone rather than guess which image is the eye pair.
    VkSemaphore composited = VK_NULL_HANDLE;
    uint32_t family = 0;
    if (g_sbs_enabled && g_active && pi->swapchainCount == 1) {
        if (queue_family_of(queue, family)) {
            composited = g_sbs.composite(
                queue, family, pi->pSwapchains[0], pi->pImageIndices[0],
                pi->pWaitSemaphores, pi->waitSemaphoreCount,
                g_cursor_enabled ? shared_state() : nullptr,
                g_canvas_map.get());
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
    // Released here, after composite() has submitted. OpenXR requires the
    // writes to have been SUBMITTED before release, not completed, and
    // composite() submits before returning. Released even when composite did
    // not run, because an acquired image that is never released deadlocks the
    // swapchain after three frames — the image the runtime gets is then simply
    // whatever the copy left in it, which is a stale frame rather than a hang.
    if (vr_blit)
        vr_finish_blit();

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

    // Read it back after the present has been chained down, so the wait costs
    // the frame that was going to be shown anyway rather than one the game is
    // still building. Idling is acceptable here for the same reason it is in
    // probe_collect: this runs only when asked for, and a diagnostic run is not
    // a performance measurement.
    if (dump_layers && d->QueueWaitIdle) {
        d->QueueWaitIdle(queue);
        std::lock_guard<std::mutex> lock(g_probe_mu);
        if (g_probe.ptr) {
            static uint64_t seq = 0;
            const uint64_t n = seq++;
            for (uint32_t l = 0; l < dump_layers; l++) {
                char path[512];
                snprintf(path, sizeof(path), "%s-present-n%llu-layer%u.ppm",
                         g_mv_dump ? g_mv_dump : "/tmp/x4vr",
                         (unsigned long long)n, l);
                write_ppm8(path,
                           (const uint8_t *)g_probe.ptr + dump_layer_bytes * l,
                           dump_w, dump_h, dump_bgra);
            }
            // The pointer, paired with the frame. Without this the dump says
            // what the frame held but not where the cursor was, which is
            // exactly why take 93 could not answer whether one was in it --
            // a search through a busy picture instead of a lookup.
            float cx = 0.f, cy = 0.f;
            bool vis = false;
            const bool have = x4vr::share_read(shared_state(), &cx, &cy, &vis);
            X4VR_LOG("mv dump: present frame %llu — %ux%u, %u layer(s), bgra=%d,"
                     " cursor %s",
                     (unsigned long long)n, dump_w, dump_h, dump_layers,
                     (int)dump_bgra,
                     have ? (vis ? "" : "hidden ") : "unknown (no channel)");
            if (have)
                X4VR_LOG("mv dump: frame %llu cursor at x=%.1f y=%.1f%s",
                         (unsigned long long)n, cx, cy,
                         vis ? "" : " (not visible)");
            // Once, when an image first arrives. The format is passed through
            // from SDL unconverted on purpose, so this line is the measurement
            // that decides how to unpack it -- a guessed conversion table would
            // mangle the colours quietly instead.
            static bool img_said = false;
            if (!img_said) {
                static uint8_t px[x4vr::Shared::kCursorMax *
                                  x4vr::Shared::kCursorMax * 4];
                uint32_t cw = 0, ch = 0, fmt = 0, pitch = 0;
                int32_t hx = 0, hy = 0;
                if (x4vr::share_read_cursor(shared_state(), px, &cw, &ch, &hx,
                                            &hy, &fmt, &pitch)) {
                    img_said = true;
                    uint32_t opaque = 0, nonzero = 0;
                    for (uint32_t i = 0; i < cw * ch; i++) {
                        const uint8_t *p = px + i * 4;
                        if (p[0] | p[1] | p[2] | p[3])
                            nonzero++;
                        if (p[3] == 0xff)
                            opaque++;
                    }
                    X4VR_LOG("share: cursor image %ux%u fmt=0x%08x hot=(%d,%d) "
                             "— %u/%u px non-zero, %u with byte3=0xff",
                             cw, ch, fmt, hx, hy, nonzero, cw * ch, opaque);
                    X4VR_LOG("share: first row bytes %02x %02x %02x %02x | "
                             "%02x %02x %02x %02x",
                             px[0], px[1], px[2], px[3], px[4], px[5], px[6],
                             px[7]);
                }
            }
        }
    }
    frame_flush();
    return r;
}

// ------------------------------------------------------------------ VR
//
// Task #34, second half: the bring-up `tests/xr_probe.cpp` proved in a headset,
// now on X4's own instance, device and queue.
//
// This step deliberately submits NOTHING. Its whole job is to isolate the risky
// part -- the runtime adds instance and device extensions to structs X4 owns,
// and X4 has to keep running with them -- from the part that changes pixels.
// The headset shows whatever the runtime shows an application that submits no
// layers; X4 keeps rendering to the monitor exactly as before.
//
// The frame loop runs on its own thread rather than out of vkQueuePresentKHR,
// for a reason that outlives this step: xrWaitFrame blocks until the runtime's
// next frame boundary, so driving it from the present hook would peg X4's frame
// rate to the headset's refresh and couple two cadences that have no reason to
// agree. When submission arrives, the pacing stays here and only the recording
// moves into the present path.
#ifdef X4VR_HAVE_OPENXR

const bool g_vr = x4vr::env_on("X4VR_VR", false);

struct VrState {
    x4vr::xr::Runtime rt;
    x4vr::xr::Session session;
    std::thread thread;
    std::atomic<bool> stop{false};
    // Two flags, not one. `started` means the thread was spawned; `session_ok`
    // means xrCreateSession returned a session. Conflating them made a run
    // with no runtime at all report session=1, which is the sort of thing the
    // scorer would then have to disbelieve.
    std::atomic<bool> started{false};
    std::atomic<bool> session_ok{false};
    // A third, and it is not a refinement of the other two. `started` says the
    // thread was spawned and `session_ok` says a session exists; neither says
    // "this thread has finished trying". vr_session_thread returns silently
    // from four different failure paths -- no runtime, a refused graphics
    // device, mismatched physical-device handles, a failed xrCreateSession --
    // and the task #41 waiter needs to know that, or a machine with no headset
    // pays the full five-second cap on every run. Measured headlessly at
    // 5.014 s with XR_RUNTIME_JSON pointed at nothing, before this existed.
    std::atomic<bool> session_settled{false};

    std::mutex mu;
    uint64_t frames = 0, located = 0, begun = 0;
    bool focused = false;
    bool have_span = false;
    float pmin[3] = {0, 0, 0}, pmax[3] = {0, 0, 0};
    uint32_t queue_family = 0;
    uint32_t queue_index = 0;
    bool device_is_the_runtime_s = false;

    // Noted at vkCreateDevice, used once X4 has come back out of it. The
    // session is NOT created inside the loader's device-creation chain: the
    // handle lookup it needs calls back into the loader on the instance side,
    // and re-entering the loader from inside one of its own chain calls is a
    // hazard worth not taking for the sake of a few milliseconds.
    VkInstance vk = VK_NULL_HANDLE;
    VkPhysicalDevice chain_phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    InstanceData inst;
    bool pending = false;

    // Task #38. The swapchain is created lazily on X4's present thread,
    // because its shape comes from the eye image and that does not exist until
    // X4 has created its own swapchain — which happens long after the session.
    x4vr::xr::Swapchain sc;
    std::mutex sc_mu;               // XrSwapchain wants external synchronisation
    std::atomic<bool> sc_ready{false};
    std::atomic<bool> sc_failed{false}; // tried once and could not; do not retry
    std::atomic<uint64_t> released{0};  // images handed to the runtime
    std::atomic<uint64_t> acquire_fail{0};
    std::atomic<uint64_t> submitted{0}; // frames that carried a projection layer
    // Set once the VR thread has completed a frame, cleared when the session
    // stops running. The present thread reads ONLY this before touching the
    // swapchain: it is the difference between "a session object exists" and
    // "the runtime is asking for pixels", and take 114b hung on the gap.
    std::atomic<bool> loop_live{false};

    // Task #35 piece 2. The runtime's own per-eye frusta, captured from the
    // first located frame and then immutable: the affine's coefficients are
    // baked into shader modules and cannot follow a value that moves. Written
    // once by the VR thread, published with a release store, read by
    // offaxis_target() on whichever thread patches the first world module.
    //
    // A later frame that disagrees is logged rather than applied -- see the
    // capture site for why that is the honest response and not laziness.
    std::atomic<bool> fov_seen{false};
    float fov_rad[2][4] = {};   // [eye] = {left, right, up, down}
    std::atomic<uint32_t> fov_drift{0};
};

// The field we declare WHEN THE OFF-AXIS AFFINE IS OFF, which must then be the
// field X4 actually rendered or the world comes out the wrong size. X4's
// half-angle follows X4VR_FOV by the measured law
// `full degrees = X4VR_FOV * 73.7399`, so this is derived from the same knob
// rather than from a second constant that could drift away from it.
//
// With the affine on (task #35 piece 2) this is not what is declared: the
// affine has re-projected X4's symmetric field into the target frusta, so the
// target is what the compositor must be told about. See offaxis_target() —
// the declaration and the shader coefficients come from one latched object
// precisely so that they cannot be sourced differently.
//
// Deliberately NOT read back from X4's projection: the layer sees several
// cameras per frame with sx from 0.75 to 3.78, and picking one of those is the
// mistake that fifty takes were built on. The knob is what we set, so the knob
// is what we declare.
inline XrFovf vr_declared_fov() {
    const char *e = getenv("X4VR_FOV");
    const float knob = e ? (float)atof(e) : 0.0f;
    const float full_deg = (knob > 0.0f ? knob : 1.0f) * 73.7399f;
    const float half = full_deg * 0.5f * 0.01745329251994330f;
    XrFovf f{};
    f.angleLeft = -half;
    f.angleRight = half;
    f.angleUp = half;
    f.angleDown = -half;
    return f;
}
const XrFovf g_vr_fov = vr_declared_fov();
VrState g_vrs;

// Task #35 piece 2. Forward-declared up with offaxis_target(), which is the
// only caller: it needs the runtime's frusta at shader-patch time and must not
// block waiting for them.
bool vr_session_thread_started() {
    return g_vrs.started.load(std::memory_order_acquire);
}

bool vr_awaiting_first_view() {
    // `g_active`, and it is load-bearing. gamescope loads this layer too and
    // creates its own VkDevice, and vr_note_device is gated on g_vr alone, so
    // `pending` is set in that process as well. Without this the wait would
    // stall gamescope's startup for its whole cap, every run — a five-second
    // regression bought by a fix for a ten-millisecond race, in a process the
    // layer already announces it is inert in.
    //
    // `pending` as well as `started`: the session is created outside the
    // loader's device-creation chain, so there is a window where the intent
    // exists and the thread does not. Treating that window as "give up" would
    // reintroduce exactly the race this exists to close.
    return g_vr && g_active &&
           (g_vrs.pending || g_vrs.started.load(std::memory_order_acquire)) &&
           !g_vrs.stop.load(std::memory_order_acquire) &&
           !g_vrs.session_settled.load(std::memory_order_acquire) &&
           !g_vrs.fov_seen.load(std::memory_order_acquire);
}

bool vr_located_fov(float out[2][4]) {
    if (!g_vr || !g_vrs.fov_seen.load(std::memory_order_acquire))
        return false;
    memcpy(out, g_vrs.fov_rad, sizeof(g_vrs.fov_rad));
    return true;
}

void vr_say(void *, const char *s) { X4VR_LOG("%s", s); }

void vr_on_state(void *, XrSessionState s) {
    X4VR_LOG("vr: session -> %s", x4vr::xr::session_state_name(s));
    if (s == XR_SESSION_STATE_FOCUSED) {
        std::lock_guard<std::mutex> lock(g_vrs.mu);
        g_vrs.focused = true;
    }
}

// Called from vkCreateInstance, before the down-chain create. That ordering is
// the whole reason this lives here: the runtime decides X4's instance
// extensions, and it has to decide them before the instance exists.
bool vr_open_runtime() {
    if (!g_vr)
        return false;
    if (g_vrs.rt.ok())
        return true;
    X4VR_LOG("vr: X4VR_VR=1 — bringing the runtime up ahead of X4's instance");
    if (!x4vr::xr::runtime_open(g_vrs.rt, vr_say, nullptr)) {
        // Loud, and then out of the way. A missing runtime must not stop the
        // game starting; the scorer is what fails the run, not X4.
        X4VR_LOG("vr: NO SESSION THIS RUN — %s. X4 continues flat.",
                 g_vrs.rt.last_error.c_str());
        return false;
    }
    return true;
}

void vr_report(const char *when); // defined below; the loop ticks through it

// Task #38, on X4's present thread.
//
// Acquires an image of the runtime's swapchain and asks SbsCompositor to copy
// the finished eye image into it during the composite that is about to be
// recorded. The release happens in vr_finish_blit(), AFTER composite() has
// submitted -- OpenXR requires the writes to have been submitted, not
// completed, before an image is released, and composite() submits before it
// returns.
//
// Everything here is on X4's thread and none of it holds a lock across a call
// that can block: xrAcquireSwapchainImage can wait, but only for one of three
// images to come free, never for a headset frame the way xrWaitFrame does.
//
// Returns true when a blit was requested, so the caller knows whether it owes a
// release. On any failure it returns false and X4's frame proceeds untouched --
// this path must never be able to stop the flatscreen game.
bool vr_begin_blit(VkSwapchainKHR x4sc) {
    if (!g_vr || g_vrs.sc_failed.load(std::memory_order_relaxed))
        return false;
    // Not `session_ok`. That only says xrCreateSession returned, and X4
    // presents its first frame long before the runtime is ready to take
    // pixels — acquiring then blocks, on X4's present thread, and takes the
    // flatscreen down with it. `loop_live` says the VR thread has actually
    // completed a frame, which is the only evidence that the runtime wants
    // images at all. Take 114b hung exactly here.
    if (!g_vrs.loop_live.load(std::memory_order_acquire))
        return false;

    VkExtent2D eye{};
    uint32_t layers = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    if (!g_sbs.eye_shape(x4sc, &eye, &layers, &fmt))
        return false;
    if (layers < 2)
        return false; // mono: nothing stereo to send, and #2 owns that gap

    std::lock_guard<std::mutex> lock(g_vrs.sc_mu);
    if (!g_vrs.sc_ready.load(std::memory_order_relaxed)) {
        // B8G8R8A8_SRGB first: it is X4's channel order, so the copy is a raw
        // byte copy. Probe run 3 measured the runtime offering it (format 50).
        // UNORM second, which needs no sRGB reinterpretation if that ever
        // proves to be the wrong reading of X4's output.
        const VkFormat want[] = {VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_FORMAT_B8G8R8A8_UNORM};
        const VkFormat pick = x4vr::xr::choose_format(g_vrs.session, want, 2,
                                                      vr_say, nullptr);
        if (pick == VK_FORMAT_UNDEFINED) {
            X4VR_LOG("vr: NO SUBMISSION THIS RUN — the runtime offers no "
                     "B8G8R8A8 format, so a copy from X4's %d eye image "
                     "cannot be byte-preserving. This needs a swizzle pass, "
                     "which is not written.",
                     (int)fmt);
            g_vrs.sc_failed.store(true, std::memory_order_relaxed);
            return false;
        }
        const XrResult r = x4vr::xr::swapchain_create(
            g_vrs.sc, g_vrs.session, pick, eye.width, eye.height, 2, 1);
        if (r != XR_SUCCESS) {
            X4VR_LOG("vr: NO SUBMISSION THIS RUN — xrCreateSwapchain "
                     "(%ux%u, 2 layers, format %d) -> %s",
                     eye.width, eye.height, (int)pick,
                     x4vr::xr::result_name(r));
            g_vrs.sc_failed.store(true, std::memory_order_relaxed);
            return false;
        }
        X4VR_LOG("vr: swapchain %ux%u x2 layers, %zu image(s), format %d — "
                 "X4's eye image is %ux%u x%u format %d, so the copy is "
                 "byte-preserving",
                 eye.width, eye.height, g_vrs.sc.images.size(), (int)pick,
                 eye.width, eye.height, layers, (int)fmt);
        // Said once, with the arithmetic, because a mismatch between what X4
        // renders and what we declare shows up as a world that is subtly the
        // wrong size rather than as an error.
        const char *knob = getenv("X4VR_FOV");
        const OffAxisPair &oat = offaxis_target();
        const float k = 57.2957795130823f;
        if (oat.on)
            X4VR_LOG("vr: declaring the CANTED field the affine was baked for "
                     "(%s) — eye0 l=%.2f r=%.2f u=%.2f d=%.2f, eye1 l=%.2f "
                     "r=%.2f u=%.2f d=%.2f deg. Same source as the shader "
                     "coefficients, which is the only thing that makes the "
                     "pair correct; X4 still renders a symmetric %.2f deg "
                     "half-field and the affine re-projects it.",
                     oat.source, oat.fov[0].angleLeft * k,
                     oat.fov[0].angleRight * k, oat.fov[0].angleUp * k,
                     oat.fov[0].angleDown * k, oat.fov[1].angleLeft * k,
                     oat.fov[1].angleRight * k, oat.fov[1].angleUp * k,
                     oat.fov[1].angleDown * k, g_vr_fov.angleRight * k);
        else
            X4VR_LOG("vr: declaring a symmetric field of +-%.2f deg per eye, "
                     "from X4VR_FOV=%s%s. No off-axis affine this run, so "
                     "render and declaration are both symmetric and agree. "
                     "The runtime's own views are canted (the FOV centre is "
                     "15.04 deg out) and it honours ours instead — probe run 3 "
                     "confirmed that on hardware.",
                     g_vr_fov.angleRight * k, knob ? knob : "1.0",
                     knob ? "" : " (UNSET — X4 is at its default 73.74 deg "
                                 "field, which is narrower than the headset's; "
                                 "set X4VR_FOV=1.4917 to cover it)");
        g_vrs.sc_ready.store(true, std::memory_order_relaxed);
    }

    // Two milliseconds, not forever. We hold an image only for the length of
    // one composite and there are three, so the common case returns at once;
    // if the runtime cannot produce one in that time, X4 presents this frame
    // without it and the headset reprojects the previous one. A dropped VR
    // frame is a far smaller defect than a stalled game.
    uint32_t idx = 0;
    bool owed = false;
    const bool ready = x4vr::xr::swapchain_acquire_timeout(
        g_vrs.sc, &idx, (XrDuration)2000000, &owed);
    if (!ready) {
        g_vrs.acquire_fail.fetch_add(1, std::memory_order_relaxed);
        // Acquired but not ready: hand it straight back. Keeping it would leak
        // one image per frame and the pool would drain into the hang this
        // timeout exists to prevent.
        if (owed)
            x4vr::xr::swapchain_release(g_vrs.sc);
        return false;
    }
    g_sbs.request_vr_blit(g_vrs.sc.images[idx], eye, 2);
    return true;
}

// The other half. Separate from vr_begin_blit because the release must not
// happen until composite() has submitted the copy.
void vr_finish_blit() {
    std::lock_guard<std::mutex> lock(g_vrs.sc_mu);
    x4vr::xr::swapchain_release(g_vrs.sc);
    g_vrs.released.fetch_add(1, std::memory_order_relaxed);
}

void vr_thread() {
    double last = 0.0;
    auto now = [] {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    };
    while (!g_vrs.stop.load(std::memory_order_relaxed)) {
        if (!x4vr::xr::session_poll(g_vrs.session, vr_on_state, nullptr))
            break;
        if (!g_vrs.session.running) {
            g_vrs.loop_live.store(false, std::memory_order_release);
            struct timespec nap = {0, 20 * 1000 * 1000};
            nanosleep(&nap, nullptr);
            continue;
        }
        bool should_render = false;
        if (!x4vr::xr::frame_begin(g_vrs.session, &should_render)) {
            struct timespec nap = {0, 2 * 1000 * 1000};
            nanosleep(&nap, nullptr);
            continue;
        }
        XrView views[2] = {};
        XrViewStateFlags flags = 0;
        const bool have = x4vr::xr::locate_views(g_vrs.session, views, 2, &flags);
        // Task #35 piece 2: capture the runtime's frusta on the FIRST located
        // frame, so the affine has them before X4 creates its first world
        // shader module. Take 163's timing says that is comfortable -- first
        // located frame at t+0.007 s, first world module at t+0.8 s -- but the
        // margin is not a guarantee, which is why offaxis_target() has a
        // named fallback rather than assuming it wins.
        if (have && !g_vrs.fov_seen.load(std::memory_order_acquire)) {
            for (uint32_t e = 0; e < 2; e++) {
                g_vrs.fov_rad[e][0] = views[e].fov.angleLeft;
                g_vrs.fov_rad[e][1] = views[e].fov.angleRight;
                g_vrs.fov_rad[e][2] = views[e].fov.angleUp;
                g_vrs.fov_rad[e][3] = views[e].fov.angleDown;
            }
            g_vrs.fov_seen.store(true, std::memory_order_release);
            const float k = 57.2957795130823f;
            X4VR_LOG("vr fov: runtime reports eye0 l=%.2f r=%.2f u=%.2f "
                     "d=%.2f, eye1 l=%.2f r=%.2f u=%.2f d=%.2f deg — latched "
                     "as the off-axis target for this run",
                     g_vrs.fov_rad[0][0] * k, g_vrs.fov_rad[0][1] * k,
                     g_vrs.fov_rad[0][2] * k, g_vrs.fov_rad[0][3] * k,
                     g_vrs.fov_rad[1][0] * k, g_vrs.fov_rad[1][1] * k,
                     g_vrs.fov_rad[1][2] * k, g_vrs.fov_rad[1][3] * k);
        } else if (have) {
            // Drift. NOT applied: the coefficients are already baked into
            // however many shader modules X4 has compiled, and re-latching
            // would leave the modules patched before the change in one frustum
            // and those after it in another. Counted and reported, because a
            // runtime whose frusta move is a fact about this hardware that the
            // design would have to answer, and silence here would hide it.
            const float *f0 = g_vrs.fov_rad[0];
            if (std::fabs(views[0].fov.angleLeft - f0[0]) > 1e-3f ||
                std::fabs(views[0].fov.angleRight - f0[1]) > 1e-3f ||
                std::fabs(views[0].fov.angleUp - f0[2]) > 1e-3f ||
                std::fabs(views[0].fov.angleDown - f0[3]) > 1e-3f) {
                const uint32_t n =
                    g_vrs.fov_drift.fetch_add(1, std::memory_order_relaxed);
                if (n == 0)
                    X4VR_LOG("vr fov: WARNING — eye0's frustum MOVED to "
                             "l=%.2f r=%.2f u=%.2f d=%.2f deg. The affine is "
                             "baked and still uses the latched values, so the "
                             "declaration and the pixels now disagree by that "
                             "difference.",
                             views[0].fov.angleLeft * 57.2957795130823f,
                             views[0].fov.angleRight * 57.2957795130823f,
                             views[0].fov.angleUp * 57.2957795130823f,
                             views[0].fov.angleDown * 57.2957795130823f);
            }
        }
        // Task #38: submit the eye image, once X4's present thread has put one
        // in the swapchain. Until then this is still a zero-layer frame, which
        // is what takes 112/113 ran.
        XrCompositionLayerProjectionView pv[2] = {};
        XrCompositionLayerProjection proj = {};
        const XrCompositionLayerBaseHeader *layers[1] = {nullptr};
        uint32_t layer_count = 0;
        if (have && should_render && g_vrs.sc_ready.load() &&
            g_vrs.released.load() > 0) {
            for (uint32_t e = 0; e < 2; e++) {
                pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                // The pose we RENDERED from, which is not the pose the head is
                // in. X4 has no head tracking yet (#33): it renders along its
                // own forward axis, so declaring the live orientation would
                // glue the world to the face — the whole scene would rotate
                // with the head, which is both wrong and sickening. Identity
                // orientation makes the image world-locked, so looking around
                // moves the eye WITHIN the rendered field and past its edge is
                // black. That is the honest description of what we drew, and
                // the black edge is #33's job to remove.
                // #33: the pose X4 RENDERED from, which is now a real
                // orientation rather than identity. The injector drives X4's
                // free-look and publishes where it believes the camera ended
                // up; that belief is what this frame was drawn with, so it is
                // what has to be declared.
                //
                // Deliberately the injector's *command*, not the live head
                // pose. X4's camera lags the head by a frame and stops dead at
                // 56.5 deg, so declaring the head pose would claim the image
                // was rendered from somewhere it was not, and the compositor
                // would reproject a frame that does not match -- the world
                // would swim exactly when the head moves fastest or reaches
                // the clamp. Declaring what we actually drove keeps the
                // reprojection honest, and past the clamp the image simply
                // stops following, which is the truth.
                //
                // Falls back to identity when nothing is driving, which is the
                // stage9 behaviour and stays correct for a run without
                // head-look.
                // **This line had no instrument, and it is the most load-
                // bearing one in the VR path.** Nothing in any log said whether
                // shared_state() resolved, whether cam_valid was ever 1, or
                // what orientation was declared -- so "the HUD swims against
                // the head and leaves through the far side" had two mechanisms
                // that fit it equally well and no way to choose:
                //
                //   identity   the image is world-locked, HUD offset == the
                //              head angle, growing from zero
                //   correct    HUD offset == (head - X4 camera), which take 149
                //              measured as sub-degree below the clamp, so it
                //              would only appear past +-65 deg
                //
                // Opposite fixes. Report the declared angles next to X4's own,
                // at the same cadence as the rest of the periodic block, so the
                // next run picks one from data instead of from a description.
                float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                float decl_yaw = 0.0f, decl_pitch = 0.0f;
                unsigned valid = 0, linked = 0;
                if (const x4vr::Shared *sh = shared_state()) {
                    linked = 1;
                    valid = sh->cam_valid.load(std::memory_order_relaxed);
                    if (valid) {
                        decl_yaw = sh->cam_yaw_deg.load(std::memory_order_relaxed);
                        decl_pitch =
                            sh->cam_pitch_deg.load(std::memory_order_relaxed);
                        x4vr::quat_of_angles(decl_yaw, decl_pitch, q);
                    }
                }
                if (e == 0) {
                    static uint64_t n = 0;
                    if ((n++ % 600) == 0)
                        X4VR_LOG("vr pose: shared=%u cam_valid=%u declared "
                                 "yaw=%.2f pitch=%.2f deg%s",
                                 linked, valid, decl_yaw, decl_pitch,
                                 valid ? "" : "  <- IDENTITY, the image is "
                                              "world-locked");
                }
                pv[e].pose.orientation.x = q[0];
                pv[e].pose.orientation.y = q[1];
                pv[e].pose.orientation.z = q[2];
                pv[e].pose.orientation.w = q[3];
                pv[e].pose.position = views[e].pose.position;
                // Task #35 piece 2: the frustum the AFFINE was baked for, from
                // whichever source offaxis_target() latched. The two must name
                // the same rectangle or the compositor maps our pixels onto a
                // field they were not computed for -- and every piece would
                // still look individually correct.
                //
                // With the affine off this falls back to our own symmetric
                // field, which is what takes 112-163 declared: X4 renders
                // symmetric, we declare symmetric, self-consistent. Monado
                // reads this to build its UV-to-tangent map and probe run 3
                // confirmed on hardware that it honours what we give it rather
                // than substituting its own.
                const OffAxisPair &oat = offaxis_target();
                pv[e].fov = oat.on ? oat.fov[e] : g_vr_fov;
                pv[e].subImage.swapchain = g_vrs.sc.handle;
                pv[e].subImage.imageRect = {
                    {0, 0},
                    {(int32_t)g_vrs.sc.width, (int32_t)g_vrs.sc.height}};
                pv[e].subImage.imageArrayIndex = e;
            }
            proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            proj.space = g_vrs.session.space;
            proj.viewCount = 2;
            proj.views = pv;
            layers[0] = (const XrCompositionLayerBaseHeader *)&proj;
            layer_count = 1;
        }
        // The lock covers xrEndFrame because that is where the runtime's
        // client compositor submits, and on a one-graphics-queue device that
        // is X4's queue (#36). It is not held across xrWaitFrame above, which
        // blocks for a whole headset frame.
        {
            VrQueueLock qlock;
            x4vr::xr::frame_end(g_vrs.session, layers, layer_count);
        }
        if (layer_count)
            g_vrs.submitted.fetch_add(1, std::memory_order_relaxed);
        // Only now: a frame has been begun, located and ended, so the runtime
        // is demonstrably taking frames and the present thread may acquire.
        g_vrs.loop_live.store(true, std::memory_order_release);

        // #33: publish the head orientation for the injector's SDL hooks.
        //
        // views[0] carries it directly rather than needing a separate VIEW-space
        // locate: probe run 2 measured this runtime's eye poses as PARALLEL
        // (VIEW_REL_DEG 0.0000, and Monado's quirks.parallel_views folds the
        // 15.04 deg cant into the fov instead), so either eye's orientation IS
        // the head's. If that ever stops holding, this is where it breaks and
        // the probe is what would say so.
        //
        // Gated on `have` -- a located pose, not merely a session. Driving X4's
        // camera from a pose without ORIENTATION_VALID would swing the view to
        // wherever an uninitialised quaternion points.
        {
            static auto head_fn =
                (x4vr::HeadShared * (*)())dlsym(RTLD_DEFAULT, "x4vr_head_state");
            static bool said = false;
            if (head_fn) {
                const auto &o = views[0].pose.orientation;
                const x4vr::HeadAngles a =
                    x4vr::head_angles(o.x, o.y, o.z, o.w);
                x4vr::head_share_write(head_fn(), a.yaw_deg, a.pitch_deg, have);
            } else if (!said) {
                said = true;
                X4VR_LOG("vr: no x4vr_head_state — injector not preloaded, so "
                         "head-look has nothing to drive");
            }
        }

        bool tick = false;
        {
            std::lock_guard<std::mutex> lock(g_vrs.mu);
            g_vrs.begun++;
            g_vrs.frames++;
            if (have) {
                g_vrs.located++;
                const float p[3] = {views[0].pose.position.x,
                                    views[0].pose.position.y,
                                    views[0].pose.position.z};
                for (int i = 0; i < 3; i++) {
                    if (!g_vrs.have_span || p[i] < g_vrs.pmin[i])
                        g_vrs.pmin[i] = p[i];
                    if (!g_vrs.have_span || p[i] > g_vrs.pmax[i])
                        g_vrs.pmax[i] = p[i];
                }
                g_vrs.have_span = true;
            }
            const double t = now();
            tick = t - last > 5.0;
            if (tick)
                last = t;
        }
        // Outside that scope on purpose: vr_report takes the same mutex, and
        // std::mutex is not recursive. Reported in the same shape as the final
        // line rather than in a second format saying the same things, so a run
        // that is killed rather than quit still leaves the scorer something it
        // can parse.
        if (tick)
            vr_report("periodic");
    }
}

// Add the runtime's required extensions to a create-info X4 owns.
//
// This is the v1 contract, and the reason the layer uses it rather than
// enable2 is in the note above x4vr::xr::vk_instance_extensions: enable2 wants
// a pfnGetInstanceProcAddr, a layer only has a down-chain one, and handing
// that over puts the runtime's handles in a space the loader's public entry
// points reject. Merging name lists is the same edit already made for
// multiview, and it keeps every Vulkan object in exactly one space.
//
// `storage` and `names` must outlive the create call: the driver reads them.
const char **vr_merge_extensions(const char *const *have, uint32_t have_n,
                                 const std::vector<std::string> &add,
                                 std::vector<std::string> &storage,
                                 std::vector<const char *> &names,
                                 const char *what) {
    storage.clear();
    names.clear();
    for (uint32_t i = 0; i < have_n; i++)
        storage.push_back(have[i]);
    uint32_t added = 0;
    std::string added_names;
    for (const auto &e : add) {
        bool dup = false;
        for (const auto &s : storage)
            if (s == e)
                dup = true;
        if (dup)
            continue;
        storage.push_back(e);
        added_names += ' ';
        added_names += e;
        added++;
    }
    for (const auto &s : storage)
        names.push_back(s.c_str());
    X4VR_LOG("vr: %s extensions — X4 asked for %u, the runtime needs %u, "
             "added %u:%s",
             what, have_n, (unsigned)add.size(), added,
             added ? added_names.c_str() : " (none)");
    return names.data();
}

// The handle a layer sees is not the handle a runtime can use.
//
// Take 111 aborted inside xrCreateSession, in Monado's vk_init_from_given ->
// vkGetPhysicalDeviceMemoryProperties, with the loader reporting "Invalid
// physicalDevice". The cause is structural rather than a bug in either
// program:
//
//   * the Vulkan loader hands the APPLICATION a wrapped VkPhysicalDevice and
//     passes the unwrapped one down the layer chain, so the `phys` we are
//     given in vkCreateDevice is not the handle X4 itself holds;
//   * XrGraphicsBindingVulkan2KHR carries handles and no
//     pfnGetInstanceProcAddr, so a runtime has no choice but to use the
//     loader's public entry points on whatever it is given.
//
// Monado made the mismatch hard to see by being inconsistent: it cached the
// pfnGetInstanceProcAddr we passed to xrCreateVulkanInstanceKHR and used it
// for vkEnumeratePhysicalDevices -- which is why the "does the runtime want a
// different device" check passed, both sides being chain-level handles -- and
// then used the public loader for the session's Vulkan bundle.
//
// So the session is given the application-level handle, found by matching
// device UUIDs rather than pointers. The log prints both handles: the claim
// that they differ is the diagnosis, and a diagnosis that cannot be read back
// out of the log is a guess.
VkPhysicalDevice vr_app_level_physical_device(VkInstance vk,
                                              VkPhysicalDevice chain_phys,
                                              const InstanceData &inst) {
    if (!inst.GetPhysicalDeviceProperties2)
        return VK_NULL_HANDLE;
    VkPhysicalDeviceIDProperties mine_id{};
    mine_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 mine{};
    mine.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    mine.pNext = &mine_id;
    inst.GetPhysicalDeviceProperties2(chain_phys, &mine);

    void *dl = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        X4VR_LOG("vr: cannot dlopen libvulkan.so.1 to reach the loader's own "
                 "entry points");
        return VK_NULL_HANDLE;
    }
    auto pub =
        (PFN_vkGetInstanceProcAddr)dlsym(dl, "vkGetInstanceProcAddr");
    if (!pub)
        return VK_NULL_HANDLE;
    auto enumerate =
        (PFN_vkEnumeratePhysicalDevices)pub(vk, "vkEnumeratePhysicalDevices");
    auto props2 = (PFN_vkGetPhysicalDeviceProperties2)pub(
        vk, "vkGetPhysicalDeviceProperties2");
    if (!props2)
        props2 = (PFN_vkGetPhysicalDeviceProperties2)pub(
            vk, "vkGetPhysicalDeviceProperties2KHR");
    if (!enumerate || !props2)
        return VK_NULL_HANDLE;

    uint32_t n = 0;
    if (enumerate(vk, &n, nullptr) != VK_SUCCESS || !n)
        return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> all(n);
    if (enumerate(vk, &n, all.data()) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    for (VkPhysicalDevice p : all) {
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 pr{};
        pr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        pr.pNext = &id;
        props2(p, &pr);
        if (memcmp(id.deviceUUID, mine_id.deviceUUID, VK_UUID_SIZE) == 0) {
            X4VR_LOG("vr: physical device \"%s\" — this layer was handed %p, "
                     "the loader's public handle is %p — %s",
                     mine.properties.deviceName, (void *)chain_phys, (void *)p,
                     p == chain_phys
                         ? "the same, so this loader does not wrap them"
                         : "not the same handle, which is what aborted take "
                           "111; the session gets the public one");
            return p;
        }
    }
    X4VR_LOG("vr: no public physical device matches the UUID of the one X4 "
             "chose — cannot bind a session to it");
    return VK_NULL_HANDLE;
}

// Task #36: give the runtime a queue of its own.
//
// Measured in takes 111 and 113 -- X4 creates queue family 0 with exactly ONE
// graphics queue and family 1 with one. The runtime's client compositor
// submits on the queue the graphics binding names, and a VkQueue is externally
// synchronised, so sharing X4's would put two threads on one queue the moment
// we hand over a composition layer. That is undefined behaviour of the worst
// kind to debug: it shows up as a hang or a torn frame, intermittently, a long
// way from the change that caused it.
//
// The layer already rewrites VkDeviceCreateInfo for multiview and for the
// runtime's extensions, so it asks for one more queue on X4's graphics family
// and binds the session to that index, leaving X4's own index 0 untouched. If
// the family has no room, a graphics family X4 did not use is taken instead.
// If there is neither, the session is REFUSED rather than shared: a wrong
// picture is recoverable and a data race is not.
//
// `queues` and `prio` must outlive the vkCreateDevice call -- the driver reads
// them -- so they are the caller's.
bool vr_reserve_queue(VkPhysicalDevice phys, const InstanceData &inst,
                      const VkDeviceCreateInfo *ci,
                      std::vector<VkDeviceQueueCreateInfo> &queues,
                      std::vector<float> &prio, uint32_t *out_family,
                      uint32_t *out_index) {
    std::vector<VkQueueFamilyProperties> props;
    if (inst.GetPhysicalDeviceQueueFamilyProperties) {
        uint32_t n = 0;
        inst.GetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
        props.resize(n);
        inst.GetPhysicalDeviceQueueFamilyProperties(phys, &n, props.data());
    }
    queues.assign(ci->pQueueCreateInfos,
                  ci->pQueueCreateInfos + ci->queueCreateInfoCount);
    for (const auto &q : queues) {
        const bool gfx = q.queueFamilyIndex < props.size() &&
                         (props[q.queueFamilyIndex].queueFlags &
                          VK_QUEUE_GRAPHICS_BIT);
        X4VR_LOG("vr: X4 created queue family %u x%u%s (the device offers %u)",
                 q.queueFamilyIndex, q.queueCount, gfx ? " graphics" : "",
                 q.queueFamilyIndex < props.size()
                     ? props[q.queueFamilyIndex].queueCount
                     : 0);
    }

    // 1. One more on a graphics family X4 already uses.
    for (size_t i = 0; i < queues.size(); i++) {
        const uint32_t fam = queues[i].queueFamilyIndex;
        if (fam >= props.size() ||
            !(props[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            continue;
        if (queues[i].queueCount >= props[fam].queueCount)
            continue;
        prio.assign(queues[i].queueCount + 1, 1.0f);
        for (uint32_t q = 0; q < queues[i].queueCount; q++)
            prio[q] = queues[i].pQueuePriorities ? queues[i].pQueuePriorities[q]
                                                 : 1.0f;
        *out_family = fam;
        *out_index = queues[i].queueCount;
        queues[i].queueCount++;
        queues[i].pQueuePriorities = prio.data();
        X4VR_LOG("vr: reserved queue family %u index %u for the runtime — X4 "
                 "keeps index 0..%u",
                 *out_family, *out_index, *out_index - 1);
        return true;
    }

    // 2. A graphics family X4 did not ask for at all.
    for (uint32_t fam = 0; fam < props.size(); fam++) {
        if (!(props[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
            !props[fam].queueCount)
            continue;
        bool used = false;
        for (const auto &q : queues)
            if (q.queueFamilyIndex == fam)
                used = true;
        if (used)
            continue;
        prio.assign(1, 1.0f);
        VkDeviceQueueCreateInfo add{};
        add.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        add.queueFamilyIndex = fam;
        add.queueCount = 1;
        add.pQueuePriorities = prio.data();
        queues.push_back(add);
        *out_family = fam;
        *out_index = 0;
        X4VR_LOG("vr: X4's graphics families are full — added family %u index 0"
                 " for the runtime, which X4 does not use",
                 fam);
        return true;
    }

    // 3. Share X4's, serialised by us.
    //
    // Measured on this machine, and it is not a quirk: RADV Navi31 exposes
    // family 0 as GRAPHICS|COMPUTE|TRANSFER with queueCount **1**, family 1 as
    // 4x COMPUTE only, and the rest video/sparse. There is exactly one graphics
    // queue on the device, so on AMD the runtime cannot have its own and
    // refusing would mean "no VR on AMD".
    //
    // Sharing is legal -- a VkQueue is externally synchronised, which is a
    // requirement on the application, not a prohibition -- and the layer is in
    // the right place to meet it. X4's submissions come through
    // x4vr_QueueSubmit and x4vr_QueuePresentKHR (which also covers the SBS
    // composite and the cursor overlay, since both submit from inside the
    // present hook), and the runtime's happen inside xrEndFrame on our own
    // thread. One mutex across both sides is the whole of it.
    for (const auto &q : queues) {
        const uint32_t fam = q.queueFamilyIndex;
        if (fam < props.size() && (props[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            *out_family = fam;
            *out_index = 0;
            g_vr_share_queue = true;
            X4VR_LOG("vr: this device has no spare graphics queue — the "
                     "runtime shares X4's (family %u index 0), serialised by "
                     "the layer. Submissions from both sides take one lock.",
                     fam);
            return true;
        }
    }

    X4VR_LOG("vr: NO SESSION THIS RUN — X4 created no graphics queue at all, "
             "so there is nothing to bind a session to.");
    return false;
}

// Recorded inside vkCreateDevice; acted on afterwards.
// Gated on intent: the run asked for VR, so the handle comparison happens and
// is logged whether or not a runtime turned up. That is not tidiness -- it is
// the only way this measurement can be taken on a machine with no headset
// attached, which is where it needed taking after take 111.
void vr_note_device(VkInstance vk, VkPhysicalDevice phys, VkDevice dev,
                    const InstanceData &inst) {
    if (!g_vr || g_vrs.pending || g_vrs.started.load())
        return;
    g_vrs.vk = vk;
    g_vrs.chain_phys = phys;
    g_vrs.dev = dev;
    g_vrs.inst = inst;
    g_vrs.pending = true;
}

// The thread does the whole of it -- resolve the handle, create the session,
// then run the frame loop -- so that none of it happens on a thread the loader
// is currently inside.
void vr_session_thread() {
    // Set on EVERY exit from this function, including the four silent early
    // returns below and normal shutdown after vr_thread(). A waiter that can
    // only see `started` and `fov_seen` cannot tell "still coming up" from
    // "gave up two seconds ago", and would spend its whole cap on the second.
    struct Settle {
        ~Settle() {
            g_vrs.session_settled.store(true, std::memory_order_release);
        }
    } settle;
    VkPhysicalDevice app_phys = vr_app_level_physical_device(
        g_vrs.vk, g_vrs.chain_phys, g_vrs.inst);
    if (!g_vrs.rt.ok())
        return; // the comparison above was the point; there is no runtime

    // The handle the binding MUST carry. Monado compares it against exactly
    // this and fails the session with XR_ERROR_VALIDATION_FAILURE otherwise
    // (oxr_session.c:1151) -- which is how take 112 ended.
    VkPhysicalDevice want = VK_NULL_HANDLE;
    const XrResult gr = x4vr::xr::graphics_device_v1(g_vrs.rt, g_vrs.vk, &want);
    if (gr != XR_SUCCESS) {
        X4VR_LOG("vr: NO SESSION THIS RUN — xrGetVulkanGraphicsDeviceKHR -> %s",
                 x4vr::xr::result_name(gr));
        return;
    }

    // A guard with two independent sources, which the one it replaces was not:
    // `want` comes from the runtime, `app_phys` from the loader's own public
    // enumeration matched by device UUID. If they disagree, the runtime is
    // working in a handle space we cannot reach, and the session is refused --
    // logged, not aborted, because X4 is running and a VR knob must not take
    // it down.
    X4VR_LOG("vr: physical device handles — layer %p, loader public %p, "
             "runtime asks for %p",
             (void *)g_vrs.chain_phys, (void *)app_phys, (void *)want);
    if (app_phys != VK_NULL_HANDLE && want != app_phys) {
        X4VR_LOG("vr: NO SESSION THIS RUN — the runtime's device is not the "
                 "one the loader publishes for the GPU X4 chose. Binding it "
                 "would abort inside the runtime, as it did in take 111.");
        return;
    }
    const XrResult r =
        x4vr::xr::session_create(g_vrs.session, g_vrs.rt, g_vrs.vk, want,
                                 g_vrs.dev, g_vrs.queue_family,
                                 g_vrs.queue_index);
    if (r != XR_SUCCESS) {
        X4VR_LOG("vr: NO SESSION THIS RUN — %s",
                 g_vrs.session.last_error.c_str());
        return;
    }
    X4VR_LOG("vr: session created on X4's own device — queue family %u index %u"
             " (reserved for the runtime), reference space %s",
             g_vrs.queue_family, g_vrs.queue_index,
             g_vrs.session.space_type == XR_REFERENCE_SPACE_TYPE_STAGE
                 ? "STAGE" : "LOCAL");
    X4VR_LOG("vr: submitting NO layers this run by design — the headset will "
             "show the runtime's own idle scene, not X4");
    g_vrs.session_ok.store(true);
    vr_thread();
}

// Called once X4 is back out of vkCreateDevice. vkGetDeviceQueue is the first
// thing it does there, and it is a call the application makes rather than one
// the loader makes into us.
void vr_start_session_deferred() {
    if (!g_vrs.pending || g_vrs.started.exchange(true))
        return;
    g_vrs.thread = std::thread(vr_session_thread);
}

// One line the scorer can key on, whatever else the run did.
void vr_report(const char *when) {
    if (!g_vr)
        return;
    uint64_t blits = 0, refused = 0;
    g_sbs.vr_counts(&blits, &refused);
    std::lock_guard<std::mutex> lock(g_vrs.mu);
    X4VR_LOG("vr summary (%s): runtime=%s session=%d focused=%d frames=%llu "
             "located=%llu submitted=%llu span=%.4f,%.4f,%.4f",
             when, g_vrs.rt.ok() ? g_vrs.rt.runtime_name : "none",
             (int)g_vrs.session_ok.load(), (int)g_vrs.focused,
             (unsigned long long)g_vrs.frames, (unsigned long long)g_vrs.located,
             (unsigned long long)g_vrs.submitted.load(),
             g_vrs.pmax[0] - g_vrs.pmin[0], g_vrs.pmax[1] - g_vrs.pmin[1],
             g_vrs.pmax[2] - g_vrs.pmin[2]);
    // The other half of the path, on X4's thread and at X4's rate. Reported
    // separately because "the runtime got 5400 frames" and "X4 produced 900
    // of them" are different facts, and a single number cannot say both --
    // submitted counts headset frames, blits counts X4 frames that reached it.
    X4VR_LOG("vr copy (%s): swapchain=%d blits=%llu released=%llu "
             "acquire_failed=%llu refused=%llu",
             when, (int)g_vrs.sc_ready.load(), (unsigned long long)blits,
             (unsigned long long)g_vrs.released.load(),
             (unsigned long long)g_vrs.acquire_fail.load(),
             (unsigned long long)refused);
}

void vr_shutdown() {
    if (!g_vrs.started.load()) {
        vr_report("final");
        x4vr::xr::runtime_close(g_vrs.rt);
        return;
    }
    g_vrs.stop.store(true);
    if (g_vrs.thread.joinable())
        g_vrs.thread.join(); // must finish before the device goes away
    vr_report("final");
    // Before the session, which owns it. The present thread is gone by now:
    // this runs from vkDestroyDevice, after X4 has stopped presenting.
    if (g_vrs.sc_ready.load()) {
        std::lock_guard<std::mutex> lock(g_vrs.sc_mu);
        x4vr::xr::swapchain_destroy(g_vrs.sc);
        g_vrs.sc_ready.store(false);
    }
    if (g_vrs.session_ok.load())
        x4vr::xr::session_destroy(g_vrs.session);
    x4vr::xr::runtime_close(g_vrs.rt);
    g_vrs.started.store(false);
}

#else  // X4VR_HAVE_OPENXR

const bool g_vr = false;
bool vr_located_fov(float[2][4]) { return false; }
bool vr_open_runtime() { return false; }
void vr_report(const char *) {}
void vr_shutdown() {}
void vr_start_session_deferred() {}

#endif // X4VR_HAVE_OPENXR

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

    const char *app_name =
        ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName
            ? ci->pApplicationInfo->pApplicationName
            : nullptr;
    const bool target = app_is_target(app_name);

    // The runtime creates X4's instance when VR is asked for, so that its own
    // extensions are merged into the create-info X4 wrote. It calls back
    // through `gipa`, which is the next layer down -- so X4 still gets an
    // instance built from its own struct, and never learns anything happened.
    const VkInstanceCreateInfo *use = ci;
    bool via_runtime = false;
#ifdef X4VR_HAVE_OPENXR
    VkInstanceCreateInfo vr_ci{};
    std::vector<std::string> vr_ext_store;
    std::vector<const char *> vr_ext_names;
    if (target && vr_open_runtime()) {
        std::vector<std::string> need;
        if (x4vr::xr::vk_instance_extensions(g_vrs.rt, need)) {
            vr_ci = *ci;
            vr_ci.ppEnabledExtensionNames =
                vr_merge_extensions(ci->ppEnabledExtensionNames,
                                    ci->enabledExtensionCount, need,
                                    vr_ext_store, vr_ext_names, "instance");
            vr_ci.enabledExtensionCount = (uint32_t)vr_ext_names.size();
            use = &vr_ci;
            via_runtime = true;
        } else {
            X4VR_LOG("vr: NO SESSION THIS RUN — the runtime would not list its "
                     "required Vulkan instance extensions");
            x4vr::xr::runtime_close(g_vrs.rt);
        }
    }
#endif
    VkResult r = next_create(use, ac, out);
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
    data.GetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
            *out, "vkGetPhysicalDeviceQueueFamilyProperties");
    data.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gipa(
        *out, "vkGetPhysicalDeviceProperties");
    data.app_api_version =
        ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_instances[dispatch_key(*out)] = data;
    }
    g_active = target;
    X4VR_LOG("instance created (app=%s)%s%s", app_name ? app_name : "?",
             g_active ? "" : " — not the game, layer inert in this process",
             via_runtime ? " — with the OpenXR runtime's extensions merged in"
                         : "");
    // Which WSIs this process could possibly use. If a surface later reports
    // no preferred extent and only one *_surface extension is enabled, the
    // platform follows without any inference at all.
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (strstr(e, "_surface"))
            X4VR_LOG("wsi: instance enables %s (pid %d)", e, (int)getpid());
    }
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

    // The runtime creates X4's device too, for the same reason it created the
    // instance -- external_memory_fd, timeline_semaphore and the rest are its
    // requirements, not X4's, and they have to be in the create-info before
    // the device exists. It only gets to do that if it agrees with X4 about
    // which physical device, and if it does not, X4's choice wins: a mod that
    // moves the game onto a different GPU to suit a headset has stopped being
    // non-intrusive.
#ifdef X4VR_HAVE_OPENXR
    VkDeviceCreateInfo vr_ci{};
    std::vector<std::string> vr_ext_store;
    std::vector<const char *> vr_ext_names;
    std::vector<VkDeviceQueueCreateInfo> vr_queues;
    std::vector<float> vr_prio;
    // Gated on intent for the queue, on the runtime for the extensions. The
    // extension list can only come from a runtime; the queue reservation
    // cannot, and gating it on rt.ok() would mean the one code path that
    // rewrites X4's queues is never exercised on a machine with no headset --
    // which is every machine this gets developed on. A spare queue X4 does not
    // use costs nothing.
    if (g_active && g_vr) {
        InstanceData inst{};
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_instances.find(dispatch_key(phys));
            if (it != g_instances.end())
                inst = it->second;
        }
        vr_ci = *ci;
        if (vr_reserve_queue(phys, inst, ci, vr_queues, vr_prio,
                             &g_vrs.queue_family, &g_vrs.queue_index)) {
            vr_ci.queueCreateInfoCount = (uint32_t)vr_queues.size();
            vr_ci.pQueueCreateInfos = vr_queues.data();
            ci = &vr_ci;
        } else if (g_vrs.rt.ok()) {
            x4vr::xr::runtime_close(g_vrs.rt);
        }
    }
    if (g_active && g_vrs.rt.ok()) {
        InstanceData inst{};
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_instances.find(dispatch_key(phys));
            if (it != g_instances.end())
                inst = it->second;
        }
        std::vector<std::string> need;
        if (!x4vr::xr::vk_device_extensions(g_vrs.rt, need)) {
            X4VR_LOG("vr: NO SESSION THIS RUN — the runtime would not list its "
                     "required Vulkan device extensions");
            x4vr::xr::runtime_close(g_vrs.rt);
        } else {
            // Only what the driver actually advertises. An unsupported name in
            // ppEnabledExtensionNames fails vkCreateDevice outright, which
            // would take X4 down over a VR knob -- and several of these are
            // core in 1.2, where a driver is free to stop listing them.
            std::vector<std::string> ok;
            std::string dropped;
            if (inst.EnumerateDeviceExtensionProperties) {
                uint32_t n = 0;
                inst.EnumerateDeviceExtensionProperties(phys, nullptr, &n,
                                                        nullptr);
                std::vector<VkExtensionProperties> have(n);
                inst.EnumerateDeviceExtensionProperties(phys, nullptr, &n,
                                                        have.data());
                for (const auto &e : need) {
                    bool found = false;
                    for (uint32_t i = 0; i < n; i++)
                        if (e == have[i].extensionName)
                            found = true;
                    if (found) {
                        ok.push_back(e);
                    } else {
                        dropped += ' ';
                        dropped += e;
                    }
                }
            } else {
                ok = need;
            }
            if (!dropped.empty())
                X4VR_LOG("vr: the driver does not advertise%s — not adding "
                         "them; the session may fail for want of them",
                         dropped.c_str());
            // vr_ci already carries the queue edit above; add to it rather
            // than re-copying ci, which by now may BE vr_ci.
            vr_ci.ppEnabledExtensionNames =
                vr_merge_extensions(ci->ppEnabledExtensionNames,
                                    ci->enabledExtensionCount, ok,
                                    vr_ext_store, vr_ext_names, "device");
            vr_ci.enabledExtensionCount = (uint32_t)vr_ext_names.size();
            ci = &vr_ci;
        }
    }
#endif
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
    RESOLVE(CreateDescriptorUpdateTemplate);
    RESOLVE(UpdateDescriptorSetWithTemplate);
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
    RESOLVE(CreateDescriptorSetLayout);
    RESOLVE(AllocateDescriptorSets);
    RESOLVE(DestroyImageView);
    RESOLVE(CreateRenderPass2);
    RESOLVE(DestroyRenderPass);
    RESOLVE(CreateGraphicsPipelines);
    RESOLVE(CreateComputePipelines);
    RESOLVE(CmdDispatch);
    RESOLVE(CmdDispatchIndirect);
    RESOLVE(CmdDispatchBase);
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
            if (g_cursor_enabled) {
                x4vr::CursorFns cf;
#define RESOLVE(name) cf.name = (PFN_vk##name)gdpa(*out, "vk" #name);
                X4VR_CURSOR_FNS(RESOLVE)
#undef RESOLVE
                if (cf.complete()) {
                    g_sbs.configure_cursor(cf, mem);
                    X4VR_LOG("cursor: overlay armed (X4VR_CURSOR=1) — X4's own "
                             "pointer will be blended into the eye image");
                } else {
                    X4VR_LOG("cursor: could not resolve every entry point — "
                             "overlay off");
                }
            }
        } else {
            X4VR_LOG("sbs: could not resolve every entry point — composite off");
        }
    }
    if (const char *h = getenv("X4VR_TEST_HAMMER"); h && *h && *h != '0') {
        static std::thread t(hammer_thread);
        t.detach();
    }
#ifdef X4VR_HAVE_OPENXR
    // g_vr, not rt.ok(): the physical-device comparison is worth logging even
    // when no runtime turned up, because that is exactly the machine on which
    // it can be checked without a headset.
    if (g_active && g_vr) {
        InstanceData inst{};
        {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_instances.find(dispatch_key(phys));
            if (it != g_instances.end())
                inst = it->second;
        }
        vr_note_device(inst.instance, phys, *out, inst);
    }
#endif
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
    bindless_report("final");
    canvas_report("final");
    // Before anything device-owned goes away: the frame loop holds a session
    // bound to this device, and a thread still calling xrEndFrame while the
    // device is destroyed is the one crash this step could plausibly cause.
    vr_shutdown();
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
    {"vkCreateDescriptorUpdateTemplate",
     (PFN_vkVoidFunction)x4vr_CreateDescriptorUpdateTemplate},
    {"vkUpdateDescriptorSetWithTemplate",
     (PFN_vkVoidFunction)x4vr_UpdateDescriptorSetWithTemplate},
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
    {"vkCreateComputePipelines",
     (PFN_vkVoidFunction)x4vr_CreateComputePipelines},
    {"vkCmdDispatch", (PFN_vkVoidFunction)x4vr_CmdDispatch},
    {"vkCmdDispatchIndirect", (PFN_vkVoidFunction)x4vr_CmdDispatchIndirect},
    {"vkCmdDispatchBase", (PFN_vkVoidFunction)x4vr_CmdDispatchBase},
    {"vkCmdDispatchBaseKHR", (PFN_vkVoidFunction)x4vr_CmdDispatchBase},
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
    {"vkCreateDescriptorSetLayout",
     (PFN_vkVoidFunction)x4vr_CreateDescriptorSetLayout},
    {"vkAllocateDescriptorSets",
     (PFN_vkVoidFunction)x4vr_AllocateDescriptorSets},
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

// Hooked only when the chain below really has them.
//
// An app is entitled to pick its backend by asking for
// vkCreateWaylandSurfaceKHR and taking the null answer as "no Wayland here".
// Returning our own pointer unconditionally would hand it a Wayland it cannot
// use, and would do so *because we were watching* — an instrument that
// changes the thing it measures. Kept out of kHooks so the lookup can be
// conditioned on the real one existing; see x4vr_GetInstanceProcAddr.
const NameFunc kSurfaceHooks[] = {
    {"vkCreateWaylandSurfaceKHR",
     (PFN_vkVoidFunction)x4vr_CreateWaylandSurfaceKHR},
    {"vkCreateXcbSurfaceKHR", (PFN_vkVoidFunction)x4vr_CreateXcbSurfaceKHR},
    {"vkCreateXlibSurfaceKHR", (PFN_vkVoidFunction)x4vr_CreateXlibSurfaceKHR},
    {"vkDestroySurfaceKHR", (PFN_vkVoidFunction)x4vr_DestroySurfaceKHR},
    // Gated for the same reason, and for a second one: this is an extension
    // entry point, so an app may reasonably read a null as "no
    // VK_KHR_get_surface_capabilities2 here".
    {"vkGetPhysicalDeviceSurfaceCapabilities2KHR",
     (PFN_vkVoidFunction)x4vr_GetPhysicalDeviceSurfaceCapabilities2KHR},
    {"vkGetPhysicalDeviceSurfaceSupportKHR",
     (PFN_vkVoidFunction)x4vr_GetPhysicalDeviceSurfaceSupportKHR},
};

PFN_vkVoidFunction find_surface_hook(const char *name) {
    for (const auto &h : kSurfaceHooks)
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
    PFN_vkVoidFunction real;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_instances.find(dispatch_key(instance));
        if (it == g_instances.end())
            return nullptr;
        real = it->second.gipa(instance, name);
    }
    // Only shadow a surface constructor the chain below actually provides:
    // a null answer here is load-bearing information for the app.
    if (real)
        if (PFN_vkVoidFunction fn = find_surface_hook(name))
            return fn;
    return real;
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
