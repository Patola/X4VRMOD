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

namespace x4vr {

// Device entry points the composite needs, resolved by the layer.
#define X4VR_SBS_FNS(X)                                                        \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)       \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer)            \
    X(CmdPipelineBarrier) X(CmdCopyImage) X(CreateSemaphore)                   \
    X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(WaitForFences)        \
    X(ResetFences) X(GetSwapchainImagesKHR) X(QueueSubmit) X(DeviceWaitIdle)

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
        VkExtent2D extent{};
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

    void configure(VkDevice device, const SbsFns &fns) {
        std::lock_guard<std::mutex> lock(mu_);
        device_ = device;
        fns_ = fns;
    }

    bool ready() const { return device_ && fns_.complete(); }

    // Called after the real swapchain has been created. Failure here is not
    // fatal: the chain is simply left unusable and frames present untouched.
    void add_swapchain(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR &ci) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!device_ || !fns_.complete())
            return;
        Chain c;
        c.extent = ci.imageExtent;
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
        chains_[sc] = std::move(c);
        X4VR_LOG("sbs: composite armed for %ux%u (%u images), each eye %ux%u",
                 ci.imageExtent.width, ci.imageExtent.height, n,
                 ci.imageExtent.width / 2, ci.imageExtent.height);
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
                          uint32_t wait_count) {
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

        // X4 leaves the image ready to present; take it to GENERAL so it can
        // be both source and destination of the copy, then hand it back.
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = c.images[image];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask =
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &b);

        const uint32_t half = c.extent.width / 2;
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {0, 0, 0};
        region.dstOffset = {(int32_t)half, 0, 0};
        region.extent = {half, c.extent.height, 1};
        fns_.CmdCopyImage(cb, c.images[image], VK_IMAGE_LAYOUT_GENERAL,
                          c.images[image], VK_IMAGE_LAYOUT_GENERAL, 1, &region);

        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                nullptr, 0, nullptr, 1, &b);

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
        for (auto &kv : chains_)
            destroy_chain(kv.second);
        chains_.clear();
        device_ = VK_NULL_HANDLE;
    }

private:
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
    std::unordered_map<VkSwapchainKHR, Chain> chains_;
};

} // namespace x4vr
