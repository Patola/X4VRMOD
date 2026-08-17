// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_sbs.hpp — present-time side-by-side composite.
//
// Phase 4a scaffolding. X4 renders one frame into the full 2816x1408
// swapchain image; just before it is presented we copy the left half over
// the right half, so both halves carry identical pixels.
//
// That is deliberately zero-parallax and geometrically wrong -- each half is
// a crop of a 2:1 view, not a square eye view. What it validates is
// everything *around* the eyes: that the layer can own the present path,
// that the 2:1 split lands on exact pixel boundaries, that the result reads
// as SBS in a viewer or headset, and that the mouse is unaffected. None of
// this code is throwaway: once the two halves are rendered per eye, the same
// composite is what puts them on screen.
//
// The copy is in-place within one image, which is legal because the two
// halves do not overlap. Source and destination are the same image, so both
// use VK_IMAGE_LAYOUT_GENERAL -- an image has one layout at a time, and
// TRANSFER_SRC_OPTIMAL / TRANSFER_DST_OPTIMAL cannot both apply.
#pragma once

#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../common/x4vr_log.hpp"
#include "x4vr_cursor_draw.hpp"

namespace x4vr {

// Device entry points the composite needs, resolved by the layer.
#define X4VR_SBS_FNS(X)                                                        \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)       \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer)            \
    X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdCopyImageToBuffer)               \
    X(CreateSemaphore)                                                         \
    X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(WaitForFences)        \
    X(ResetFences) X(GetSwapchainImagesKHR) X(QueueSubmit) X(DeviceWaitIdle)   \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)               \
    X(AllocateMemory) X(FreeMemory) X(BindImageMemory)

struct SbsFns {
#define X4VR_DECL(name) PFN_vk##name name = nullptr;
    X4VR_SBS_FNS(X4VR_DECL)
#undef X4VR_DECL

    bool complete() const {
#define X4VR_CHECK(name)                                                       \
    if (!name)                                                                 \
        return false;
        X4VR_SBS_FNS(X4VR_CHECK)
#undef X4VR_CHECK
        return true;
    }
};

class SbsCompositor {
public:
    // One set of per-image resources. The command buffer and semaphore are
    // indexed by swapchain image, which is safe: an image index cannot come
    // back from vkAcquireNextImageKHR until its previous present is done
    // with it. The fence makes that guarantee explicit before we re-record.
    struct Chain {
        VkExtent2D extent{};  // the real swapchain image, e.g. 2816x1408
        // When virtualized, X4 renders into images we own, one per swapchain
        // image, each half as wide -- and it believes those *are* the
        // swapchain, because we report half width from the surface
        // capabilities it sizes itself from. Present time copies each into
        // both halves. When not virtualized, X4 renders the full width and
        // we copy its left half over its right.
        bool virtualized = false;
        VkExtent2D eye{};
        VkFormat format = VK_FORMAT_UNDEFINED; // of both the eye and the real image
        // How many array layers the eye image carries, and which one feeds the
        // right half of the composite.
        //
        // These are separate on purpose. Two layers can be allocated long
        // before anything writes the second one, and copying an unwritten
        // layer into the right half would put garbage on screen. So the
        // allocation moves first and `right_layer` stays 0 until a masked
        // pass is actually rendering into layer 1 -- at which point flipping
        // it to 1 is the whole of the change, and flipping it back is the
        // whole of the rollback.
        uint32_t eye_layers = 1;
        uint32_t right_layer = 0;
        std::vector<VkImage> eye_images;
        std::vector<VkDeviceMemory> eye_memory;
        VkCommandPool pool = VK_NULL_HANDLE;
        // Which queue family `pool` was created for. A command buffer may
        // only be submitted to a queue of its pool's family, and X4 asks for
        // more than one family, so this is discovered from the queue that
        // actually presents rather than assumed.
        uint32_t family = UINT32_MAX;
        std::vector<VkImage> images;
        std::vector<VkCommandBuffer> cmds;
        std::vector<VkSemaphore> done;
        std::vector<VkFence> fences;
        bool usable = false;
    };

    void configure(VkDevice device, const SbsFns &fns,
                   const VkPhysicalDeviceMemoryProperties &mem) {
        std::lock_guard<std::mutex> lock(mu_);
        device_ = device;
        fns_ = fns;
        mem_ = mem;
    }

    // Task #17. Arming the overlay is what turns the cursor on: unconfigured it
    // is never ready, so the knob is a decision made once at device creation
    // rather than a branch in the present path.
    //
    // Owned here rather than by the layer so its lifetime is the compositor's.
    // Its resources hang off the eye images, and destroying those first would
    // leave framebuffers pointing at nothing.
    void configure_cursor(const CursorFns &fns,
                          const VkPhysicalDeviceMemoryProperties &mem) {
        std::lock_guard<std::mutex> lock(mu_);
        cursor_.configure(device_, fns, mem);
    }

    // Images X4 should be handed instead of the real swapchain's, or nullptr
    // if this chain is not virtualized.
    const std::vector<VkImage> *eye_images(VkSwapchainKHR sc) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = chains_.find(sc);
        if (it == chains_.end() || !it->second.virtualized)
            return nullptr;
        return &it->second.eye_images;
    }

    // What the eye images actually are, for the layer's own image tracking.
    // Without this they reach X4 untracked and every serial-keyed instrument
    // prints `?` for the images the frame is built in -- the same sentinel that
    // hid the composite for twenty-seven takes, in a second place.
    struct EyeInfo {
        uint32_t layers = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
    };
    // Set by request_dump(), consumed and cleared by the next composite().
    VkBuffer dump_buf_ = VK_NULL_HANDLE;
    VkDeviceSize dump_layer_bytes_ = 0;
    // Set by request_vr_blit(), likewise one-shot.
    VkImage vr_dst_ = VK_NULL_HANDLE;
    VkExtent2D vr_extent_{};
    uint32_t vr_layers_ = 0;
    // Counted so the log can say "the blit ran N times" rather than the caller
    // inferring it from the absence of a complaint.
    uint64_t vr_blits_ = 0, vr_refused_ = 0;

    bool eye_info(VkSwapchainKHR sc, EyeInfo *out) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = chains_.find(sc);
        if (it == chains_.end() || !it->second.virtualized)
            return false;
        out->layers = it->second.eye_layers;
        out->format = it->second.format;
        out->extent = it->second.eye;
        return true;
    }

    // Task #29. A one-shot request to copy the eye image into `buf` during the
    // next composite.
    //
    // It has to happen *there* and not in a command buffer of its own: the eye
    // image sits in PRESENT_SRC_KHR because X4 believes it is the swapchain,
    // and composite() is the one place that already barriers it to
    // TRANSFER_SRC with a range covering every layer. A separate submit would
    // have to reproduce that transition and would be a second thing to keep
    // correct as the layouts here change.
    //
    // Cleared as soon as it is recorded, so a request costs exactly one frame
    // and a caller that stops asking stops paying.
    void request_dump(VkBuffer buf, VkDeviceSize layer_bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        dump_buf_ = buf;
        dump_layer_bytes_ = layer_bytes;
    }

    // Task #38: a one-shot request to copy the eye image into an OpenXR
    // swapchain image during the next composite.
    //
    // Same reasoning as request_dump, and it shares its barrier: the eye image
    // is in PRESENT_SRC because X4 believes it is the swapchain, and this is
    // the one place that already transitions it to TRANSFER_SRC across every
    // layer. A separate submit would have to reproduce that, on X4's single
    // graphics queue, as a second thing to keep correct.
    //
    // `dst` must be an image of the runtime's swapchain with at least as many
    // array layers as the eye image, at the SAME extent: this is
    // vkCmdCopyImage, not a blit, because probe run 3 established that the
    // runtime offers B8G8R8A8_SRGB (50) — the same channel order as X4's
    // format 44 — so the copy is byte-preserving and needs no conversion.
    // Mismatched extents are refused rather than silently scaled.
    //
    // Cleared as soon as it is recorded, so a caller that stops asking stops
    // paying, and a frame where the runtime had no image ready simply does not
    // ask.
    void request_vr_blit(VkImage dst, VkExtent2D extent, uint32_t layers) {
        std::lock_guard<std::mutex> lock(mu_);
        vr_dst_ = dst;
        vr_extent_ = extent;
        vr_layers_ = layers;
    }

    // What the eye image actually is, for the caller that has to create a
    // swapchain matching it before it can ask for a blit.
    bool eye_shape(VkSwapchainKHR sc, VkExtent2D *extent, uint32_t *layers,
                   VkFormat *format) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = chains_.find(sc);
        if (it == chains_.end() || !it->second.virtualized)
            return false;
        if (extent)
            *extent = it->second.eye;
        if (layers)
            *layers = it->second.eye_layers;
        if (format)
            *format = it->second.format;
        return true;
    }

    // For the final report: how many frames actually reached the headset, and
    // how many were refused. Reported rather than inferred from silence.
    void vr_counts(uint64_t *blits, uint64_t *refused) {
        std::lock_guard<std::mutex> lock(mu_);
        if (blits)
            *blits = vr_blits_;
        if (refused)
            *refused = vr_refused_;
    }

    bool ready() const { return device_ && fns_.complete(); }

    // Called after the real swapchain has been created. Failure here is not
    // fatal: the chain is simply left unusable and frames present untouched.
    void add_swapchain(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR &ci,
                       bool want_split, uint32_t want_layers = 1,
                       uint32_t right_layer = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_ || !fns_.complete())
            return;
        Chain c;
        c.eye_layers = want_layers < 1 ? 1 : want_layers;
        c.right_layer = right_layer < c.eye_layers ? right_layer : 0;
        c.extent = ci.imageExtent;
        c.format = ci.imageFormat;
        if (c.extent.width < 2 || (c.extent.width & 1u)) {
            X4VR_LOG("sbs: swapchain width %u cannot be halved — composite off",
                     c.extent.width);
            chains_[sc] = c;
            return;
        }

        uint32_t n = 0;
        if (fns_.GetSwapchainImagesKHR(device_, sc, &n, nullptr) !=
                VK_SUCCESS ||
            n == 0) {
            chains_[sc] = c;
            return;
        }
        c.images.resize(n);
        if (fns_.GetSwapchainImagesKHR(device_, sc, &n, c.images.data()) !=
            VK_SUCCESS) {
            chains_[sc] = c;
            return;
        }

        // Try to give X4 its own half-width images. If this fails we keep
        // the chain usable in the un-virtualized form (X4 renders full width,
        // we copy left over right) rather than losing the composite -- but
        // the caller has already reported half width from the surface caps,
        // so it must be told to stop doing that.
        c.eye = {c.extent.width / 2, c.extent.height};
        c.virtualized = want_split && make_eye_images(c, ci, n);

        // The command pool waits until the first present tells us which
        // queue family to build it for.
        c.done.resize(n, VK_NULL_HANDLE);
        c.fences.resize(n, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first use must not block
        for (uint32_t i = 0; i < n; i++) {
            if (fns_.CreateSemaphore(device_, &si, nullptr, &c.done[i]) !=
                    VK_SUCCESS ||
                fns_.CreateFence(device_, &fi, nullptr, &c.fences[i]) !=
                    VK_SUCCESS) {
                destroy_chain(c);
                chains_[sc] = Chain{};
                X4VR_LOG("sbs: sync object creation failed — composite off");
                return;
            }
        }

        c.usable = true;
        const VkExtent2D eye = c.eye;
        const bool virt = c.virtualized;
        const uint32_t layers = c.eye_layers, rl = c.right_layer;
        chains_[sc] = std::move(c);
        X4VR_LOG("sbs: composite armed for %ux%u (%u images), each eye %ux%u "
                 "(%s), eye layers=%u right half from layer %u%s",
                 ci.imageExtent.width, ci.imageExtent.height, n, eye.width,
                 eye.height,
                 virt ? "X4 renders one eye, we own its images"
                      : "X4 renders full width, left half duplicated",
                 layers, rl,
                 // Named explicitly, because "2 layers" and "actually stereo"
                 // are different claims and the gap between them is where a
                 // run gets misread as a success.
                 rl ? "  <-- STEREO composite" : "  (both halves are layer 0)");
    }

    void remove_swapchain(VkSwapchainKHR sc) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = chains_.find(sc);
        if (it == chains_.end())
            return;
        if (device_ && fns_.DeviceWaitIdle)
            fns_.DeviceWaitIdle(device_);
        destroy_chain(it->second);
        chains_.erase(it);
    }

    // Records and submits the half-to-half copy. Returns the semaphore the
    // present must wait on, or VK_NULL_HANDLE to present as X4 asked.
    VkSemaphore composite(VkQueue queue, uint32_t family, VkSwapchainKHR sc,
                          uint32_t image, const VkSemaphore *wait,
                          uint32_t wait_count, const Shared *cursor_channel,
                          const x4vr::CanvasNdc *canvas = nullptr) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = chains_.find(sc);
        if (it == chains_.end() || !it->second.usable)
            return VK_NULL_HANDLE;
        Chain &c = it->second;
        if (image >= c.images.size())
            return VK_NULL_HANDLE;
        if (!ensure_cmds(c, family))
            return VK_NULL_HANDLE;

        // The previous use of this slot's command buffer must have retired.
        // In steady state it has, so this does not stall.
        if (fns_.WaitForFences(device_, 1, &c.fences[image], VK_TRUE,
                               UINT64_MAX) != VK_SUCCESS ||
            fns_.ResetFences(device_, 1, &c.fences[image]) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkCommandBuffer cb = c.cmds[image];
        if (fns_.ResetCommandBuffer(cb, 0) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fns_.BeginCommandBuffer(cb, &bi) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkImageMemoryBarrier b[2]{};
        for (auto &x : b) {
            x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            x.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            x.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        const uint32_t half = c.extent.width / 2;
        // Both copies write the real swapchain image; the source is either
        // the eye image X4 rendered into, or -- un-virtualized -- the left
        // half of that same image.
        const VkImage src = c.virtualized ? c.eye_images[image] : c.images[image];
        VkImageCopy region[2]{};
        for (auto &r : region) {
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.srcOffset = {0, 0, 0};
            r.extent = {half, c.extent.height, 1};
        }
        region[0].dstOffset = {0, 0, 0};
        region[1].dstOffset = {(int32_t)half, 0, 0};
        // The one line that makes the display stereo. Left half always comes
        // from layer 0; the right half's source is whatever `right_layer`
        // says, which is 0 until something is known to be rendering layer 1.
        // The destination is the real swapchain image and stays single-layer
        // in both cases -- it is presented, so it has exactly one layer.
        if (c.virtualized)
            region[1].srcSubresource.baseArrayLayer = c.right_layer;

        if (c.virtualized) {
            // Task #17: the pointer goes into the eye image *before* the
            // duplication, so one draw reaches both halves and lands at the
            // same place in each. Drawing it after the copy would mean two
            // draws at two offsets in display space -- which is precisely the
            // coordinate system the cursor is being moved out of.
            //
            // It leaves the image in COLOR_ATTACHMENT_OPTIMAL, so the barrier
            // below starts from there instead of PRESENT_SRC. That is one
            // transition rather than two, and it is why record() reports
            // whether it drew rather than being fired and forgotten.
            const bool drew =
                cursor_.record(cb, {src, image, (uint32_t)c.images.size(),
                                    c.format, c.eye, c.eye_layers},
                               cursor_channel, canvas);

            // Distinct images, so each gets its optimal layout. X4 left the
            // eye image ready to present, believing it was the swapchain.
            b[0].image = src;
            b[0].oldLayout = drew ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b[0].srcAccessMask = drew ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                      : VK_ACCESS_MEMORY_READ_BIT;
            b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            // Every layer, not just the one being copied: X4 left the whole
            // image in PRESENT_SRC and a copy reads whichever layer
            // `right_layer` names, so a barrier covering only layer 0 would
            // be a layout violation the moment that becomes 1.
            b[0].subresourceRange.layerCount = c.eye_layers;
            // The real image's previous contents are entirely overwritten by
            // the two copies, so its old layout does not matter.
            b[1].image = c.images[image];
            b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                    nullptr, 0, nullptr, 2, b);
            fns_.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              c.images[image],
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, region);
            // Task #29: the finished eye image, every layer, while it is still
            // in TRANSFER_SRC. This is the frame as X4 left it -- after every
            // present pass, so after the UI and anything drawn over it, which
            // is exactly what the end-of-render-pass probe cannot see.
            if (dump_buf_ != VK_NULL_HANDLE) {
                const uint32_t n = c.eye_layers > 2 ? 2 : c.eye_layers;
                VkBufferImageCopy r[2]{};
                for (uint32_t l = 0; l < n; l++) {
                    r[l].bufferOffset = dump_layer_bytes_ * l;
                    r[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
                    r[l].imageExtent = {c.eye.width, c.eye.height, 1};
                }
                fns_.CmdCopyImageToBuffer(
                    cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buf_, n,
                    r);
                dump_buf_ = VK_NULL_HANDLE;
            }
            // Task #38: the same finished frame, into the runtime's swapchain.
            // Also while it is still in TRANSFER_SRC, and for the same reason.
            if (vr_dst_ != VK_NULL_HANDLE) {
                if (vr_extent_.width != c.eye.width ||
                    vr_extent_.height != c.eye.height || vr_layers_ == 0) {
                    // Refused, loudly and once: a copy with mismatched extents
                    // is invalid, and scaling here would silently disagree with
                    // the FOV the layer declares to the compositor.
                    if (!vr_refused_++)
                        X4VR_LOG("vr: NOT blitting — swapchain is %ux%u x%u "
                                 "but the eye image is %ux%u x%u. The copy "
                                 "needs them equal; a scale here would "
                                 "disagree with the FOV we declare.",
                                 vr_extent_.width, vr_extent_.height,
                                 vr_layers_, c.eye.width, c.eye.height,
                                 c.eye_layers);
                } else {
                    const uint32_t n =
                        vr_layers_ < c.eye_layers ? vr_layers_ : c.eye_layers;
                    VkImageMemoryBarrier vb{};
                    vb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    vb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vb.image = vr_dst_;
                    vb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                           n};
                    // Entirely overwritten, so its previous contents and layout
                    // do not matter.
                    vb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    vb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    vb.srcAccessMask = 0;
                    vb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    fns_.CmdPipelineBarrier(
                        cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                        nullptr, 1, &vb);
                    VkImageCopy vr{};
                    vr.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
                    vr.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
                    vr.extent = {c.eye.width, c.eye.height, 1};
                    fns_.CmdCopyImage(cb, src,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      vr_dst_,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                      &vr);
                    vb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    vb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    vb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    vb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    fns_.CmdPipelineBarrier(
                        cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                        nullptr, 0, nullptr, 1, &vb);
                    vr_blits_++;
                }
                vr_dst_ = VK_NULL_HANDLE;
            }
            b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            b[0].subresourceRange.layerCount = c.eye_layers;
            b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                    nullptr, 0, nullptr, 2, b);
        } else {
            // One image as both source and destination, so it must be
            // GENERAL -- an image has a single layout at a time. Only the
            // right half is written; region[0] would be a no-op copy onto
            // itself, which is not legal, so just do region[1].
            b[0].image = c.images[image];
            b[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            b[0].dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                    nullptr, 0, nullptr, 1, b);
            fns_.CmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL,
                              c.images[image], VK_IMAGE_LAYOUT_GENERAL, 1,
                              &region[1]);
            b[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                    nullptr, 0, nullptr, 1, b);
        }

        if (fns_.EndCommandBuffer(cb) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        // Take over X4's present waits: we wait on them, the present waits
        // on us. Dropping them here would let the copy race the rendering.
        std::vector<VkPipelineStageFlags> stages(wait_count,
                                                 VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = wait_count;
        si.pWaitSemaphores = wait_count ? wait : nullptr;
        si.pWaitDstStageMask = wait_count ? stages.data() : nullptr;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &c.done[image];
        if (fns_.QueueSubmit(queue, 1, &si, c.fences[image]) != VK_SUCCESS) {
            // The fence stays unsignalled, which would deadlock the next
            // pass through this slot; drop the slot instead.
            c.usable = false;
            X4VR_LOG("sbs: composite submit failed — presenting untouched");
            return VK_NULL_HANDLE;
        }
        return c.done[image];
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mu_);
        if (device_ && fns_.DeviceWaitIdle)
            fns_.DeviceWaitIdle(device_);
        // Before the chains: the overlay's framebuffers and views are built on
        // the eye images, and a view must not outlive the image it views.
        cursor_.shutdown();
        for (auto &kv : chains_)
            destroy_chain(kv.second);
        chains_.clear();
        device_ = VK_NULL_HANDLE;
    }

private:
    // One image per swapchain image, half width, with the same format and
    // usage X4 asked the swapchain for (plus TRANSFER_SRC so we can copy out
    // of it). These are what X4 gets back from vkGetSwapchainImagesKHR.
    bool make_eye_images(Chain &c, const VkSwapchainCreateInfoKHR &ci,
                         uint32_t n) {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = ci.imageFormat;
        ii.extent = {c.eye.width, c.eye.height, 1};
        ii.mipLevels = 1;
        // The swapchain's own imageArrayLayers is 1 and cannot be otherwise --
        // it is the thing being presented. The eye image is ours, so it can
        // carry the second eye, which is the whole reason it exists.
        ii.arrayLayers = c.eye_layers > 1
                             ? c.eye_layers
                             : (ci.imageArrayLayers ? ci.imageArrayLayers : 1);
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = ci.imageSharingMode;
        ii.queueFamilyIndexCount = ci.queueFamilyIndexCount;
        ii.pQueueFamilyIndices = ci.pQueueFamilyIndices;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        c.eye_images.assign(n, VK_NULL_HANDLE);
        c.eye_memory.assign(n, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < n; i++) {
            if (fns_.CreateImage(device_, &ii, nullptr, &c.eye_images[i]) !=
                VK_SUCCESS) {
                X4VR_LOG("sbs: eye image %u creation failed", i);
                free_eye_images(c);
                return false;
            }
            VkMemoryRequirements req{};
            fns_.GetImageMemoryRequirements(device_, c.eye_images[i], &req);
            uint32_t type = UINT32_MAX;
            for (uint32_t t = 0; t < mem_.memoryTypeCount; t++)
                if ((req.memoryTypeBits & (1u << t)) &&
                    (mem_.memoryTypes[t].propertyFlags &
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = t;
                    break;
                }
            if (type == UINT32_MAX) {
                X4VR_LOG("sbs: no device-local memory type for the eye image");
                free_eye_images(c);
                return false;
            }
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (fns_.AllocateMemory(device_, &ai, nullptr, &c.eye_memory[i]) !=
                    VK_SUCCESS ||
                fns_.BindImageMemory(device_, c.eye_images[i], c.eye_memory[i],
                                     0) != VK_SUCCESS) {
                X4VR_LOG("sbs: eye image %u memory failed (%llu bytes)", i,
                         (unsigned long long)req.size);
                free_eye_images(c);
                return false;
            }
        }
        return true;
    }

    void free_eye_images(Chain &c) {
        // Same ordering rule as shutdown(), for the swapchain-recreate path:
        // a resize destroys these images while the overlay still holds views
        // and framebuffers on them.
        for (VkImage im : c.eye_images)
            if (im)
                cursor_.forget(im);
        for (VkImage im : c.eye_images)
            if (im)
                fns_.DestroyImage(device_, im, nullptr);
        for (VkDeviceMemory m : c.eye_memory)
            if (m)
                fns_.FreeMemory(device_, m, nullptr);
        c.eye_images.clear();
        c.eye_memory.clear();
    }

    // Build (or rebuild) the command pool for the family that actually
    // presents. Called under mu_ from composite(), so the first frame pays
    // for it and the rest hit the fast path.
    bool ensure_cmds(Chain &c, uint32_t family) {
        if (c.pool != VK_NULL_HANDLE && c.family == family)
            return true;
        if (c.pool != VK_NULL_HANDLE) {
            // Presenting from a different family than last time would be
            // very strange; rebuild rather than submit to the wrong one.
            X4VR_LOG("sbs: present queue moved from family %u to %u — "
                     "rebuilding command pool",
                     c.family, family);
            if (fns_.DeviceWaitIdle)
                fns_.DeviceWaitIdle(device_);
            fns_.DestroyCommandPool(device_, c.pool, nullptr);
            c.pool = VK_NULL_HANDLE;
            c.cmds.clear();
        }

        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        if (fns_.CreateCommandPool(device_, &pci, nullptr, &c.pool) !=
            VK_SUCCESS) {
            X4VR_LOG("sbs: command pool creation failed — composite off");
            c.usable = false;
            return false;
        }
        c.cmds.resize(c.images.size());
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = c.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = (uint32_t)c.cmds.size();
        if (fns_.AllocateCommandBuffers(device_, &ai, c.cmds.data()) !=
            VK_SUCCESS) {
            fns_.DestroyCommandPool(device_, c.pool, nullptr);
            c.pool = VK_NULL_HANDLE;
            c.usable = false;
            X4VR_LOG("sbs: command buffer allocation failed — composite off");
            return false;
        }
        c.family = family;
        X4VR_LOG("sbs: composite recording on queue family %u", family);
        return true;
    }

    void destroy_chain(Chain &c) {
        if (!device_)
            return;
        free_eye_images(c);
        for (VkSemaphore s : c.done)
            if (s)
                fns_.DestroySemaphore(device_, s, nullptr);
        for (VkFence f : c.fences)
            if (f)
                fns_.DestroyFence(device_, f, nullptr);
        if (c.pool)
            fns_.DestroyCommandPool(device_, c.pool, nullptr);
        c = Chain{};
    }

    std::mutex mu_;
    VkDevice device_ = VK_NULL_HANDLE;
    SbsFns fns_;
    VkPhysicalDeviceMemoryProperties mem_{};
    std::unordered_map<VkSwapchainKHR, Chain> chains_;
    CursorOverlay cursor_;
};

} // namespace x4vr
