// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The march's own GPU objects (after take 486; layer/fog_march.comp): the volume it writes
// (kW x kH x kK, RGBA16F: per quarter-resolution pixel, what its ray gathered up to each of
// kK depths), the noise it reads (a 64^3 RGBA8 lattice, four noises a fetch), and their
// samplers. All of it is set up once by a command buffer of ours, put in front of X4's
// first submission after it exists: both images to GENERAL, the noise uploaded, the
// volume cleared to no fog. X4's fog blend samples the volume from its first frame on
// (patched, it mixes ours with X4's by a switch), so the volume must be valid before any
// of X4's work can reach it.
//
// The pose block's descriptor sets (set pose_set_index()) carry them: binding 1 the volume
// as a storage image (the march writes it), binding 2 as a combined image sampler (X4's
// blend reads it), binding 3 the noise (the march reads it).
#pragma once
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vulkan/vulkan.h>

namespace x4vr {

#define X4VR_MARCH_FNS(X)                                                                  \
    X(CreateImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(BindImageMemory)      \
    X(CreateImageView) X(CreateSampler) X(CreateBuffer) X(GetBufferMemoryRequirements)     \
    X(BindBufferMemory) X(MapMemory) X(CreateCommandPool) X(AllocateCommandBuffers)        \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
    X(CmdClearColorImage) X(UpdateDescriptorSets)

struct MarchFns {
#define X4VR_DECL(name) PFN_vk##name name = nullptr;
    X4VR_MARCH_FNS(X4VR_DECL)
#undef X4VR_DECL
    bool complete() const {
#define X4VR_CHECK(name)                                                                   \
    if (!name)                                                                             \
        return false;
        X4VR_MARCH_FNS(X4VR_CHECK)
#undef X4VR_CHECK
        return true;
    }
};

class FogMarch {
public:
    static constexpr uint32_t kW = 704, kH = 704, kK = 16, kNoise = 64;
    static constexpr uint32_t kBindStorage = 1, kBindVolume = 2, kBindNoise = 3;

    void configure(VkDevice device, const MarchFns &fns,
                   const VkPhysicalDeviceMemoryProperties &mem) {
        device_ = device;
        fns_ = fns;
        mem_ = mem;
    }
    bool configured() const { return device_ != VK_NULL_HANDLE && fns_.complete(); }
    bool created() const { return created_; }
    VkDeviceSize bytes() const { return bytes_; }

    // The images, views, samplers and the noise's bytes in a host buffer. False, with
    // `why`, and nothing usable, on any failure.
    bool create(std::string *why) {
        if (created_)
            return true;
        if (!configured())
            return fail(why, "its entry points weren't all found");
        if (!image(VK_FORMAT_R16G16B16A16_SFLOAT, {kW, kH, kK},
                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &volume_, &volume_mem_, &volume_view_))
            return fail(why, "the volume (704x704x16 RGBA16F)");
        if (!image(VK_FORMAT_R8G8B8A8_UNORM, {kNoise, kNoise, kNoise},
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &noise_,
                   &noise_mem_, &noise_view_))
            return fail(why, "the noise (64^3 RGBA8)");
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (fns_.CreateSampler(device_, &si, nullptr, &clamp_) != VK_SUCCESS)
            return fail(why, "the volume's sampler");
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (fns_.CreateSampler(device_, &si, nullptr, &repeat_) != VK_SUCCESS)
            return fail(why, "the noise's sampler");
        // The noise's bytes: a fixed seed, so every run draws the same smoke.
        const VkDeviceSize n = (VkDeviceSize)kNoise * kNoise * kNoise * 4;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = n;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (fns_.CreateBuffer(device_, &bi, nullptr, &staging_) != VK_SUCCESS)
            return fail(why, "the noise's upload buffer");
        VkMemoryRequirements mr{};
        fns_.GetBufferMemoryRequirements(device_, staging_, &mr);
        const uint32_t ht = type_of(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ht == UINT32_MAX || !alloc(mr.size, ht, &staging_mem_) ||
            fns_.BindBufferMemory(device_, staging_, staging_mem_, 0) != VK_SUCCESS)
            return fail(why, "the noise's upload memory");
        void *p = nullptr;
        if (fns_.MapMemory(device_, staging_mem_, 0, n, 0, &p) != VK_SUCCESS || !p)
            return fail(why, "mapping the noise's upload memory");
        std::mt19937 rng(0x58345652u); // "X4VR"
        for (VkDeviceSize i = 0; i < n; i++)
            ((uint8_t *)p)[i] = (uint8_t)(rng() & 255u);
        created_ = true;
        return true;
    }

    // Bindings 1-3 of one of the pose block's descriptor sets.
    void write_set(VkDescriptorSet ds) const {
        VkDescriptorImageInfo st{VK_NULL_HANDLE, volume_view_, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo vo{clamp_, volume_view_, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo no{repeat_, noise_view_, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3]{};
        const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        const uint32_t binds[3] = {kBindStorage, kBindVolume, kBindNoise};
        const VkDescriptorImageInfo *infos[3] = {&st, &vo, &no};
        for (int i = 0; i < 3; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = ds;
            w[i].dstBinding = binds[i];
            w[i].descriptorCount = 1;
            w[i].descriptorType = types[i];
            w[i].pImageInfo = infos[i];
        }
        fns_.UpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }

    // The one-time setup, recorded into a command buffer of ours for queue family `family`
    // (VK_NULL_HANDLE, with `why`, on failure). The caller submits it once, before X4's.
    VkCommandBuffer setup_cb(uint32_t family, std::string *why) {
        if (!created_) {
            *why = "nothing created";
            return VK_NULL_HANDLE;
        }
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = family;
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (fns_.CreateCommandPool(device_, &pi, nullptr, &pool_) != VK_SUCCESS ||
            (ai.commandPool = pool_, fns_.AllocateCommandBuffers(device_, &ai, &cb) != VK_SUCCESS)) {
            *why = "its command buffer";
            return VK_NULL_HANDLE;
        }
        // Made here, not through the loader's trampoline: the device's dispatch pointer
        // is copied in by hand, as the layer interface asks.
        *(void **)cb = *(void **)device_;
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fns_.BeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b[2]{};
        const VkImage ims[2] = {volume_, noise_};
        for (int i = 0; i < 2; i++) {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = ims[i];
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        VkBufferImageCopy rg{};
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {kNoise, kNoise, kNoise};
        fns_.CmdCopyBufferToImage(cb, staging_, noise_, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        VkClearColorValue none{};
        none.float32[3] = 1.f; // no light, everything behind shows
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fns_.CmdClearColorImage(cb, volume_, VK_IMAGE_LAYOUT_GENERAL, &none, 1, &all);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                                nullptr);
        if (fns_.EndCommandBuffer(cb) != VK_SUCCESS) {
            *why = "ending its command buffer";
            return VK_NULL_HANDLE;
        }
        return cb;
    }

    // Around the march's dispatch, in X4's command buffer: the previous frame's reads of
    // the volume (X4's blend) finish before it is written, and the write before the
    // blend reads it.
    void barrier_before(VkCommandBuffer cb) const {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        fns_.CmdPipelineBarrier(cb,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                nullptr);
    }
    void barrier_after(VkCommandBuffer cb) const {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, 1, &mb, 0, nullptr, 0, nullptr);
    }

private:
    bool fail(std::string *why, const char *what) {
        *why = what;
        return false;
    }
    uint32_t type_of(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mem_.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mem_.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    }
    bool alloc(VkDeviceSize size, uint32_t type, VkDeviceMemory *out) {
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = size;
        ai.memoryTypeIndex = type;
        return fns_.AllocateMemory(device_, &ai, nullptr, out) == VK_SUCCESS;
    }
    bool image(VkFormat fmt, VkExtent3D ext, VkImageUsageFlags usage, VkImage *img,
               VkDeviceMemory *mem, VkImageView *view) {
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_3D;
        ci.format = fmt;
        ci.extent = ext;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (fns_.CreateImage(device_, &ci, nullptr, img) != VK_SUCCESS)
            return false;
        VkMemoryRequirements mr{};
        fns_.GetImageMemoryRequirements(device_, *img, &mr);
        const uint32_t t = type_of(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (t == UINT32_MAX || !alloc(mr.size, t, mem) ||
            fns_.BindImageMemory(device_, *img, *mem, 0) != VK_SUCCESS)
            return false;
        bytes_ += mr.size;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = *img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return fns_.CreateImageView(device_, &vi, nullptr, view) == VK_SUCCESS;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    MarchFns fns_{};
    VkPhysicalDeviceMemoryProperties mem_{};
    bool created_ = false;
    VkDeviceSize bytes_ = 0;
    VkImage volume_ = VK_NULL_HANDLE, noise_ = VK_NULL_HANDLE;
    VkDeviceMemory volume_mem_ = VK_NULL_HANDLE, noise_mem_ = VK_NULL_HANDLE,
                   staging_mem_ = VK_NULL_HANDLE;
    VkImageView volume_view_ = VK_NULL_HANDLE, noise_view_ = VK_NULL_HANDLE;
    VkSampler clamp_ = VK_NULL_HANDLE, repeat_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

} // namespace x4vr
