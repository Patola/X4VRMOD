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

#include <cmath>
#include <ctime>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
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
    PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
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

void frame_flush() {
    std::lock_guard<std::mutex> lock(g_track.mu);
    g_track.frame++;
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

VKAPI_ATTR void VKAPI_CALL x4vr_UpdateDescriptorSets(
    VkDevice device, uint32_t writeCount,
    const VkWriteDescriptorSet *writes, uint32_t copyCount,
    const VkCopyDescriptorSet *copies) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    d->UpdateDescriptorSets(device, writeCount, writes, copyCount, copies);

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

// Classify each subpass as "must not be sheared":
//   * no colour attachments        -> shadow cascade (light space)
//   * all colour attachments LDR   -> UI / final blit (screen space)
// Everything else is world geometry rendered through the camera projection,
// which is what K was derived for.
template <typename CreateInfo>
void record_render_pass(const CreateInfo *ci, VkRenderPass rp) {
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
    std::lock_guard<std::mutex> lock(g_variants.mu);
    g_variants.unsheared[rp] = std::move(unsheared);
}

VKAPI_ATTR VkResult VKAPI_CALL x4vr_CreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo *ci,
    const VkAllocationCallbacks *ac, VkRenderPass *out) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(device));
    }
    VkResult r = d->CreateRenderPass(device, ci, ac, out);
    if (r == VK_SUCCESS)
        record_render_pass(ci, *out);
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
    VkResult r = next(device, ci, ac, out);
    if (r == VK_SUCCESS)
        record_render_pass(ci, *out);
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
    if (!any)
        return d->CreateGraphicsPipelines(device, cache, count, ci, ac, out);

    static bool logged = false;
    if (!logged) {
        logged = true;
        X4VR_LOG("unsheared pipeline: using unpatched modules (shadow + UI "
                 "exclusion active)");
    }
    return d->CreateGraphicsPipelines(device, cache, count, infos.data(), ac,
                                      out);
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
    const bool split = g_sbs_enabled && g_active && g_sbs_split_render &&
                       surface_was_halved(ci->surface);
    if (split)
        sbs_ci.imageExtent.width *= 2;
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
        static uint32_t warned_w = 0, warned_h = 0;
        if ((w != X4VR_SBS_WIDTH || h != X4VR_SBS_HEIGHT) &&
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
                     "(currentExtent=0x%X) — this is the Wayland WSI; X11 "
                     "reports the real window size. Cannot make X4 render one "
                     "eye, falling back to duplicating the left half. Force "
                     "the X11 driver (SDL_VIDEODRIVER=x11).",
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

VKAPI_ATTR VkResult VKAPI_CALL x4vr_QueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pi) {
    DeviceData *d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = &g_devices.at(dispatch_key(queue));
    }
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
    RESOLVE(CreateRenderPass2);
    RESOLVE(DestroyRenderPass);
    RESOLVE(CreateGraphicsPipelines);
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
