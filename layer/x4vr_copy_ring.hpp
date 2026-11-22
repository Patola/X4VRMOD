// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_copy_ring.hpp — the mod's own images between X4's picture and the
// runtime's (copyring, 2026-11-22).
//
// rc8 copied X4's picture straight into the runtime's image and had the
// runtime's queue wait for the copy (copysync); the runtimes' xrEndFrame then
// waited for that queue on the CPU, and the headset got 30-65 frames a second
// instead of 144 on an RTX 3070 laptop. Holding the runtime's images until the
// copy had finished instead (takes 455i) starved them: X4's card runs two or
// three frames behind there, the copy finished 75-90 ms after the present, and
// with three images most frames had none to copy into.
//
// So X4's picture is copied into one of these, which belong to the mod. Once
// that copy has finished (its `written` fence, read without waiting), the XR
// thread takes a runtime image, copies the picture into it on the runtime's own
// queue (`record_hand`, then the caller submits with `handed`) and hands it
// over at once. The runtime holds its images only for that short copy, and its
// own work comes after it on its queue.
//
// The slots' states are the layer's to keep, under its own mutex; this class
// owns the Vulkan objects and records the copy.
#pragma once
#include <cstdint>
#include <string>
#include <vulkan/vulkan.h>
#include "../common/x4vr_log.hpp"

namespace x4vr {

#define X4VR_RING_FNS(X)                                                       \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)               \
    X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateCommandPool)    \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer)      \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier)            \
    X(CmdCopyImage) X(CreateFence) X(DestroyFence) X(ResetFences)              \
    X(GetFenceStatus) X(WaitForFences)

struct RingFns {
#define X4VR_DECL(name) PFN_vk##name name = nullptr;
    X4VR_RING_FNS(X4VR_DECL)
#undef X4VR_DECL
    bool complete() const {
#define X4VR_CHECK(name)                                                       \
    if (!name)                                                                 \
        return false;
        X4VR_RING_FNS(X4VR_CHECK)
#undef X4VR_CHECK
        return true;
    }
};

class CopyRing {
public:
    static constexpr uint32_t kSlots = 3;

    // Free: nobody's. Claimed: X4's present thread is having the composite
    // write it. Writing: that copy is submitted, `written` not yet signalled.
    // Ready: X4's picture is in it, whole. Reserved: the XR thread chose it and
    // is getting a runtime image. Copying: its copy into that image is
    // submitted, `handed` not yet signalled.
    enum class State : uint8_t { Free, Claimed, Writing, Ready, Reserved, Copying };

    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFence written = VK_NULL_HANDLE; // X4's copy into it has run
        VkFence handed = VK_NULL_HANDLE;  // its copy into a runtime image has run
        VkCommandBuffer cb = VK_NULL_HANDLE;
        State state = State::Free;
        uint64_t serial = 0; // the order it was written in
        double t_written = 0.0;
    };

    void configure(VkDevice device, const RingFns &fns,
                   const VkPhysicalDeviceMemoryProperties &mem) {
        device_ = device;
        fns_ = fns;
        mem_ = mem;
    }
    bool configured() const { return device_ != VK_NULL_HANDLE && fns_.complete(); }
    bool built() const { return built_; }
    uint32_t layers() const { return layers_; }
    VkExtent2D extent() const { return extent_; }
    VkFormat format() const { return format_; }
    VkDeviceSize bytes() const { return bytes_; }
    Slot &slot(uint32_t i) { return slots_[i]; }

    // The images, as the runtime's: `format`, `extent`, `layers` layers. The
    // command buffers are for `family`, the runtime's queue's.
    bool build(VkFormat format, VkExtent2D extent, uint32_t layers, uint32_t family,
               std::string *why) {
        if (built_)
            return true;
        if (!configured()) {
            *why = "its entry points weren't all found";
            return false;
        }
        format_ = format;
        extent_ = extent;
        layers_ = layers;
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        if (fns_.CreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS) {
            *why = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBuffer cbs[kSlots] = {};
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kSlots;
        if (fns_.AllocateCommandBuffers(device_, &ai, cbs) != VK_SUCCESS) {
            *why = "vkAllocateCommandBuffers failed";
            destroy();
            return false;
        }
        for (uint32_t i = 0; i < kSlots; i++) {
            Slot &s = slots_[i];
            s.cb = cbs[i];
            VkImageCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = format;
            ci.extent = {extent.width, extent.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = layers;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            // Written by the composite's copy or the menu warp's pass, read by
            // the copy into the runtime's image.
            ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (fns_.CreateImage(device_, &ci, nullptr, &s.image) != VK_SUCCESS) {
                *why = "vkCreateImage failed";
                destroy();
                return false;
            }
            VkMemoryRequirements req{};
            fns_.GetImageMemoryRequirements(device_, s.image, &req);
            uint32_t type = UINT32_MAX;
            for (uint32_t t = 0; t < mem_.memoryTypeCount; t++)
                if ((req.memoryTypeBits & (1u << t)) &&
                    (mem_.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = t;
                    break;
                }
            VkMemoryAllocateInfo mi{};
            mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mi.allocationSize = req.size;
            mi.memoryTypeIndex = type;
            if (type == UINT32_MAX ||
                fns_.AllocateMemory(device_, &mi, nullptr, &s.memory) != VK_SUCCESS ||
                fns_.BindImageMemory(device_, s.image, s.memory, 0) != VK_SUCCESS) {
                *why = "no device memory for it";
                destroy();
                return false;
            }
            bytes_ += req.size;
            // Signalled to start with: a slot never used has nothing running.
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (fns_.CreateFence(device_, &fi, nullptr, &s.written) != VK_SUCCESS ||
                fns_.CreateFence(device_, &fi, nullptr, &s.handed) != VK_SUCCESS) {
                *why = "vkCreateFence failed";
                destroy();
                return false;
            }
        }
        built_ = true;
        return true;
    }

    // Once nothing of theirs can be running (the caller waits for the fences).
    void destroy() {
        if (!device_)
            return;
        for (Slot &s : slots_) {
            if (s.written)
                fns_.DestroyFence(device_, s.written, nullptr);
            if (s.handed)
                fns_.DestroyFence(device_, s.handed, nullptr);
            if (s.image)
                fns_.DestroyImage(device_, s.image, nullptr);
            if (s.memory)
                fns_.FreeMemory(device_, s.memory, nullptr);
            s = Slot{};
        }
        if (pool_)
            fns_.DestroyCommandPool(device_, pool_, nullptr); // frees the buffers
        pool_ = VK_NULL_HANDLE;
        bytes_ = 0;
        built_ = false;
    }

    // Without waiting.
    bool done(VkFence f) const { return fns_.GetFenceStatus(device_, f) == VK_SUCCESS; }
    bool reset(VkFence f) const { return fns_.ResetFences(device_, 1, &f) == VK_SUCCESS; }
    // At the end, before destroy(): each fence that was submitted, up to `ns`.
    void wait_all(uint64_t ns) const {
        for (const Slot &s : slots_)
            for (VkFence f : {s.written, s.handed})
                if (f)
                    fns_.WaitForFences(device_, 1, &f, VK_TRUE, ns);
    }

    // Slot i's picture into `dst`, a runtime image of the same shape, left in
    // COLOR_ATTACHMENT_OPTIMAL as the runtime expects it at the release. The
    // caller submits slot i's command buffer to the runtime's queue with
    // `handed`, which it resets first. The slot is left TRANSFER_SRC_OPTIMAL;
    // the composite writes it next from UNDEFINED.
    bool record_hand(uint32_t i, VkImage dst) {
        Slot &s = slots_[i];
        if (fns_.ResetCommandBuffer(s.cb, 0) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fns_.BeginCommandBuffer(s.cb, &bi) != VK_SUCCESS)
            return false;
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers_};
        VkImageMemoryBarrier b[2] = {};
        for (VkImageMemoryBarrier &m : b) {
            m.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            m.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.subresourceRange = range;
        }
        // The composite's copy and the menu warp's pass both leave it here.
        b[0].image = s.image;
        b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        // Entirely overwritten, so its old contents and layout don't matter.
        b[1].image = dst;
        b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcAccessMask = 0;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fns_.CmdPipelineBarrier(s.cb,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers_};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers_};
        region.extent = {extent_.width, extent_.height, 1};
        fns_.CmdCopyImage(s.cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        fns_.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                                nullptr, 1, &b[1]);
        return fns_.EndCommandBuffer(s.cb) == VK_SUCCESS;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    RingFns fns_;
    VkPhysicalDeviceMemoryProperties mem_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Slot slots_[kSlots];
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    uint32_t layers_ = 0;
    VkDeviceSize bytes_ = 0;
    bool built_ = false;
};

} // namespace x4vr
