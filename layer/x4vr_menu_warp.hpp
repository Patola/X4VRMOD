// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_menu_warp.hpp — a full-screen menu, redrawn for where the head points.
//
// **Why.** In a full-screen menu X4's camera holds still, so the layer declares
// that still pose and the window stays put in the room. The runtime turns the
// picture for the head, which looks right, but streamers build their video
// frame from the pose we declare. WiVRn's layer squasher takes the projection
// layer's pose and field "so we avoid one resampling step", and its foveated
// encoding keeps full detail in the middle of THAT frame. So the sharp area
// stayed in the middle of the map whichever way the player looked: takes
// 414-418 (2026-11-08), gone with WiVRn's foveation at 0. In flight the
// declared pose follows the head and the sharp area with it.
//
// **What.** While a full-screen menu is up, the copy into the runtime's image
// becomes this pass: each pixel's direction in the head's current view, turned
// into the view X4 rendered, sampled there. The frame thread then declares the
// head's orientation instead of the still one, at the same eye positions. Every
// pixel keeps its place in the room, so the window stays exactly where it was,
// and the streamer's sharp middle is where the player looks. A turn about the
// eye is exact at every depth, so the cockpit around the menu is right too.
//
// **What it costs.** One more resampling of the menu frame, filtered over each
// pixel's footprint in linear light (menu_warp.frag), and exact where the head
// faces the way X4 rendered. Past X4's picture is black, as it is now.
//
// The arithmetic is plain functions, so tests/menu_warp_math.cpp checks it
// without a GPU and tests/menu_warp_render.cpp checks the shader against it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../common/x4vr_log.hpp"
#include "menu_warp_shaders.hpp"
#include "x4vr_cursor_draw.hpp"

namespace x4vr {

// A field, by the tangents of its edges: left and down are negative.
struct WarpTan {
    float l = -1.f, r = 1.f, u = 1.f, d = -1.f;
};

inline WarpTan warp_tan(float angle_left, float angle_right, float angle_up,
                        float angle_down) {
    return {std::tan(angle_left), std::tan(angle_right), std::tan(angle_up),
            std::tan(angle_down)};
}

// q = a * b, quaternions as OpenXR stores them: x, y, z, w.
inline void warp_quat_mul(const float a[4], const float b[4], float q[4]) {
    q[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    q[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    q[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    q[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// The turn from the view shown to the view rendered: conj(rendered) * shown.
// A pose's orientation takes view directions into the reference space, so this
// takes a direction of the shown view into the room and back out through the
// rendered one.
inline void warp_relative(const float rendered[4], const float shown[4],
                          float q[4]) {
    const float c[4] = {-rendered[0], -rendered[1], -rendered[2], rendered[3]};
    warp_quat_mul(c, shown, q);
    const float n =
        std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.f)
        for (int i = 0; i < 4; i++)
            q[i] /= n;
}

// That turn as a row-major 3x3: m * (shown view direction) = rendered view
// direction.
inline void warp_matrix(const float rendered[4], const float shown[4],
                        float m[9]) {
    float q[4];
    warp_relative(rendered, shown, q);
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1.f - 2.f * (y * y + z * z);
    m[1] = 2.f * (x * y - z * w);
    m[2] = 2.f * (x * z + y * w);
    m[3] = 2.f * (x * y + z * w);
    m[4] = 1.f - 2.f * (x * x + z * z);
    m[5] = 2.f * (y * z - x * w);
    m[6] = 2.f * (x * z - y * w);
    m[7] = 2.f * (y * z + x * w);
    m[8] = 1.f - 2.f * (x * x + y * y);
}

// How far apart the two poses are, in degrees: the size of the turn the pass
// applies, for the log.
inline float warp_angle_deg(const float rendered[4], const float shown[4]) {
    float q[4];
    warp_relative(rendered, shown, q);
    const float w = std::fabs(q[3]) > 1.f ? 1.f : std::fabs(q[3]);
    return 2.f * std::acos(w) * 57.2957795f;
}

// menu_warp.frag's source_px(), on the CPU: where a point of the target, in
// pixels, lands in the source, in pixels. False behind the rendered eye.
inline bool warp_source_px(const float m[9], const WarpTan &dst,
                           const WarpTan &src, float dw, float dh, float sw,
                           float sh, float px, float py, float *sx, float *sy) {
    const float ux = px / dw, uy = py / dh;
    const float d[3] = {dst.l + (dst.r - dst.l) * ux,
                        dst.u + (dst.d - dst.u) * uy, -1.f};
    const float r[3] = {m[0] * d[0] + m[1] * d[1] + m[2] * d[2],
                        m[3] * d[0] + m[4] * d[1] + m[5] * d[2],
                        m[6] * d[0] + m[7] * d[1] + m[8] * d[2]};
    if (!(r[2] < -1e-4f))
        return false;
    const float tx = r[0] / -r[2], ty = r[1] / -r[2];
    *sx = (tx - src.l) / (src.r - src.l) * sw;
    *sy = (src.u - ty) / (src.u - src.d) * sh;
    return true;
}

// The push constants, in menu_warp.frag's layout.
struct WarpPush {
    float m0[4], m1[4], m2[4];
    float tan_dst[4], tan_src[4];
    float ext[4];
};
static_assert(sizeof(WarpPush) == 96, "menu_warp.frag's push block");

enum : uint32_t { kWarpDecodeSource = 1, kWarpEncodeResult = 2 };

inline WarpPush warp_push(const float m[9], const WarpTan &dst,
                          const WarpTan &src, VkExtent2D dst_ext,
                          VkExtent2D src_ext, uint32_t layer, uint32_t flags) {
    WarpPush p{};
    for (int i = 0; i < 3; i++) {
        p.m0[i] = m[i];
        p.m1[i] = m[3 + i];
        p.m2[i] = m[6 + i];
    }
    p.m0[3] = (float)layer;
    p.m1[3] = (float)flags;
    const float td[4] = {dst.l, dst.r, dst.u, dst.d};
    const float ts[4] = {src.l, src.r, src.u, src.d};
    std::memcpy(p.tan_dst, td, sizeof td);
    std::memcpy(p.tan_src, ts, sizeof ts);
    p.ext[0] = (float)dst_ext.width;
    p.ext[1] = (float)dst_ext.height;
    p.ext[2] = (float)src_ext.width;
    p.ext[3] = (float)src_ext.height;
    return p;
}

// Bytes in, bytes out (menu_warp.frag): decode a source whose view does not,
// encode for a target whose view does not.
inline uint32_t warp_flags(VkFormat src, VkFormat dst) {
    return (cursor_is_srgb(src) ? 0u : kWarpDecodeSource) |
           (cursor_is_srgb(dst) ? 0u : kWarpEncodeResult);
}

class MenuWarp {
public:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        uint32_t layers = 1;
    };
    // Per eye: the turn and the two fields. The fields are the declared ones,
    // which differ per eye when the off-axis affine is on.
    struct Params {
        float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        WarpTan src[2], dst[2];
    };

    void configure(VkDevice device, const CursorFns &fns) {
        device_ = device;
        fns_ = fns;
    }
    bool ready() const { return device_ && fns_.complete() && !failed_; }

    // Every layer of `src`, which must be in SHADER_READ_ONLY_OPTIMAL and
    // SAMPLED, into the same layer of `dst`, whose contents are discarded and
    // which is left in COLOR_ATTACHMENT_OPTIMAL. False with nothing recorded,
    // so the caller can still copy instead.
    bool record(VkCommandBuffer cb, const Image &src, const Image &dst,
                const Params &p) {
        if (!ready() || src.layers < 1 || dst.layers < src.layers ||
            !ensure_shared(dst.format))
            return false;
        const VkDescriptorSet set = source_set(src);
        if (set == VK_NULL_HANDLE)
            return false;
        const uint32_t n = src.layers > 2 ? 2 : src.layers;
        VkFramebuffer fbs[2] = {};
        for (uint32_t l = 0; l < n; l++)
            if ((fbs[l] = target_fb(dst, l)) == VK_NULL_HANDLE)
                return false;
        const uint32_t flags = warp_flags(src.format, dst.format);
        for (uint32_t l = 0; l < n; l++) {
            VkRenderPassBeginInfo rb{};
            rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rb.renderPass = pass_;
            rb.framebuffer = fbs[l];
            rb.renderArea = {{0, 0}, dst.extent};
            fns_.CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
            fns_.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
            fns_.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       layout_, 0, 1, &set, 0, nullptr);
            const VkViewport vp{0.f, 0.f, (float)dst.extent.width,
                                (float)dst.extent.height, 0.f, 1.f};
            fns_.CmdSetViewport(cb, 0, 1, &vp);
            const VkRect2D sc{{0, 0}, dst.extent};
            fns_.CmdSetScissor(cb, 0, 1, &sc);
            const WarpPush pc = warp_push(p.m, p.dst[l], p.src[l], dst.extent,
                                          src.extent, l, flags);
            fns_.CmdPushConstants(cb, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof pc, &pc);
            fns_.CmdDraw(cb, 3, 1, 0, 0);
            fns_.CmdEndRenderPass(cb);
        }
        recorded_++;
        return true;
    }

    // An image going away, as a source or as a target.
    void forget(VkImage image) {
        if (!device_ || !fns_.complete())
            return;
        auto s = sources_.find(image);
        if (s != sources_.end()) {
            // The set stays in the pool: it is freed with it at shutdown, and
            // images are recreated only when the swapchain is.
            fns_.DestroyImageView(device_, s->second.view, nullptr);
            sources_.erase(s);
        }
        auto t = targets_.find(image);
        if (t != targets_.end()) {
            destroy_target(t->second);
            targets_.erase(t);
        }
    }

    void shutdown() {
        if (!device_ || !fns_.complete())
            return;
        for (auto &kv : sources_)
            fns_.DestroyImageView(device_, kv.second.view, nullptr);
        sources_.clear();
        for (auto &kv : targets_)
            destroy_target(kv.second);
        targets_.clear();
        if (pipe_)
            fns_.DestroyPipeline(device_, pipe_, nullptr);
        if (layout_)
            fns_.DestroyPipelineLayout(device_, layout_, nullptr);
        if (pass_)
            fns_.DestroyRenderPass(device_, pass_, nullptr);
        if (pool_)
            fns_.DestroyDescriptorPool(device_, pool_, nullptr);
        if (dsl_)
            fns_.DestroyDescriptorSetLayout(device_, dsl_, nullptr);
        if (sampler_)
            fns_.DestroySampler(device_, sampler_, nullptr);
        pipe_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        pass_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        dsl_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        pass_format_ = VK_FORMAT_UNDEFINED;
        failed_ = false;
        device_ = VK_NULL_HANDLE;
    }

    uint64_t recorded() const { return recorded_; }

private:
    struct Source {
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t layers = 0;
    };
    struct Target {
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> fbs;
        VkExtent2D extent{};
        VkFormat format = VK_FORMAT_UNDEFINED;
    };

    bool fail(const char *what) {
        failed_ = true;
        X4VR_LOG("menu warp: could not create the %s — full-screen menus are "
                 "copied as before, and declared still",
                 what);
        return false;
    }

    // Built once, for the runtime's swapchain format, which is fixed for the
    // session.
    bool ensure_shared(VkFormat dst_format) {
        if (pipe_ != VK_NULL_HANDLE)
            return pass_format_ == dst_format;
        if (failed_)
            return false;
        pass_format_ = dst_format;

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // texelFetch ignores it; a combined image sampler needs one.
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (fns_.CreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS)
            return fail("sampler");

        VkDescriptorSetLayoutBinding db{};
        db.binding = 0;
        db.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        db.descriptorCount = 1;
        db.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{};
        dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dli.bindingCount = 1;
        dli.pBindings = &db;
        if (fns_.CreateDescriptorSetLayout(device_, &dli, nullptr, &dsl_) !=
            VK_SUCCESS)
            return fail("descriptor set layout");

        // One set per eye image X4 has had, over the session.
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                kMaxSources};
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = kMaxSources;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (fns_.CreateDescriptorPool(device_, &dpi, nullptr, &pool_) !=
            VK_SUCCESS)
            return fail("descriptor pool");

        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(WarpPush)};
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &dsl_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        if (fns_.CreatePipelineLayout(device_, &pli, nullptr, &layout_) !=
            VK_SUCCESS)
            return fail("pipeline layout");

        // DONT_CARE: every pixel is written. UNDEFINED in, COLOR_ATTACHMENT
        // out, which is where the copy left the runtime's image too and what
        // the OpenXR Vulkan binding expects at release.
        VkAttachmentDescription at{};
        at.format = dst_format;
        at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        at.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ar;
        VkSubpassDependency dep[2]{};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask = 0;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        VkRenderPassCreateInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpi.attachmentCount = 1;
        rpi.pAttachments = &at;
        rpi.subpassCount = 1;
        rpi.pSubpasses = &sp;
        rpi.dependencyCount = 2;
        rpi.pDependencies = dep;
        if (fns_.CreateRenderPass(device_, &rpi, nullptr, &pass_) != VK_SUCCESS)
            return fail("render pass");

        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = sizeof(kMenuWarpVert);
        smi.pCode = kMenuWarpVert;
        if (fns_.CreateShaderModule(device_, &smi, nullptr, &vs) != VK_SUCCESS)
            return fail("vertex shader");
        smi.codeSize = sizeof(kMenuWarpFrag);
        smi.pCode = kMenuWarpFrag;
        if (fns_.CreateShaderModule(device_, &smi, nullptr, &fs) != VK_SUCCESS) {
            fns_.DestroyShaderModule(device_, vs, nullptr);
            return fail("fragment shader");
        }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1] = st[0];
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // No blending: the result replaces the image, alpha included, as the
        // copy did.
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = 1;
        cbs.pAttachments = &ba;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount = 2;
        gp.pStages = st;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cbs;
        gp.pDynamicState = &ds;
        gp.layout = layout_;
        gp.renderPass = pass_;
        const VkResult r = fns_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE,
                                                        1, &gp, nullptr, &pipe_);
        fns_.DestroyShaderModule(device_, vs, nullptr);
        fns_.DestroyShaderModule(device_, fs, nullptr);
        if (r != VK_SUCCESS) {
            pipe_ = VK_NULL_HANDLE;
            return fail("graphics pipeline");
        }
        X4VR_LOG("menu warp: pipeline built for format %d", (int)dst_format);
        return true;
    }

    VkDescriptorSet source_set(const Image &src) {
        auto it = sources_.find(src.image);
        if (it != sources_.end() && it->second.format == src.format &&
            it->second.layers == src.layers)
            return it->second.set;
        if (it != sources_.end()) {
            fns_.DestroyImageView(device_, it->second.view, nullptr);
            sources_.erase(it);
        }
        if (sources_made_ >= kMaxSources) {
            if (!said_full_) {
                said_full_ = true;
                X4VR_LOG("menu warp: %u eye images over the session, the pool's "
                         "size — further ones are copied as before",
                         kMaxSources);
            }
            return VK_NULL_HANDLE;
        }
        Source s;
        s.format = src.format;
        s.layers = src.layers;
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = src.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = src.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, src.layers};
        if (fns_.CreateImageView(device_, &vi, nullptr, &s.view) != VK_SUCCESS) {
            X4VR_LOG("menu warp: the eye image's view failed — copied as before");
            return VK_NULL_HANDLE;
        }
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsl_;
        if (fns_.AllocateDescriptorSets(device_, &ai, &s.set) != VK_SUCCESS) {
            fns_.DestroyImageView(device_, s.view, nullptr);
            X4VR_LOG("menu warp: descriptor set allocation failed — copied as "
                     "before");
            return VK_NULL_HANDLE;
        }
        sources_made_++;
        VkDescriptorImageInfo ii{sampler_, s.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = s.set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        fns_.UpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        sources_[src.image] = s;
        return s.set;
    }

    VkFramebuffer target_fb(const Image &dst, uint32_t layer) {
        auto it = targets_.find(dst.image);
        if (it != targets_.end() &&
            (it->second.format != dst.format ||
             it->second.extent.width != dst.extent.width ||
             it->second.extent.height != dst.extent.height)) {
            destroy_target(it->second);
            targets_.erase(it);
            it = targets_.end();
        }
        if (it == targets_.end()) {
            Target t;
            t.format = dst.format;
            t.extent = dst.extent;
            t.views.assign(dst.layers, VK_NULL_HANDLE);
            t.fbs.assign(dst.layers, VK_NULL_HANDLE);
            it = targets_.emplace(dst.image, std::move(t)).first;
        }
        Target &t = it->second;
        if (layer >= t.fbs.size())
            return VK_NULL_HANDLE;
        if (t.fbs[layer])
            return t.fbs[layer];
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = dst.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = dst.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        if (fns_.CreateImageView(device_, &vi, nullptr, &t.views[layer]) !=
            VK_SUCCESS) {
            t.views[layer] = VK_NULL_HANDLE;
            X4VR_LOG("menu warp: the runtime image's view failed — copied as "
                     "before");
            return VK_NULL_HANDLE;
        }
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &t.views[layer];
        fi.width = dst.extent.width;
        fi.height = dst.extent.height;
        fi.layers = 1;
        if (fns_.CreateFramebuffer(device_, &fi, nullptr, &t.fbs[layer]) !=
            VK_SUCCESS) {
            t.fbs[layer] = VK_NULL_HANDLE;
            X4VR_LOG("menu warp: framebuffer creation failed — copied as before");
            return VK_NULL_HANDLE;
        }
        return t.fbs[layer];
    }

    void destroy_target(Target &t) {
        for (VkFramebuffer f : t.fbs)
            if (f)
                fns_.DestroyFramebuffer(device_, f, nullptr);
        for (VkImageView v : t.views)
            if (v)
                fns_.DestroyImageView(device_, v, nullptr);
        t.fbs.clear();
        t.views.clear();
    }

    static constexpr uint32_t kMaxSources = 64;
    VkDevice device_ = VK_NULL_HANDLE;
    CursorFns fns_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkFormat pass_format_ = VK_FORMAT_UNDEFINED;
    bool failed_ = false, said_full_ = false;
    uint32_t sources_made_ = 0;
    uint64_t recorded_ = 0;
    std::unordered_map<VkImage, Source> sources_;
    std::unordered_map<VkImage, Target> targets_;
};

} // namespace x4vr
