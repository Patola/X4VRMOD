// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_cursor_draw.hpp — X4's own pointer, blended into the eye image.
//
// Task #17, step 2. Step 1 put the cursor bitmap and the pointer position on
// the injector-to-layer channel; this draws them.
//
// **Why this exists at all.** The pointer the player sees today is gamescope's,
// composited over the finished side-by-side frame in *display* space, while
// everything X4 hit-tests lives in *eye* space. Those are different coordinate
// systems, so the cursor and the thing it is pointing at can never line up in
// both halves at once -- take 90 measured exactly that, and take 93's dumps
// contain no cursor at all because it was never in the frame to begin with. A
// cursor drawn *here* is inside the image X4 rendered, so cursor and target
// become the same kind of object and the duplication carries it into both
// halves for free.
//
// **Why a graphics pipeline.** The captured cursor is 396 non-transparent
// pixels out of 1024 -- a hollow cross with a small solid core -- so it has to
// be alpha-blended. `vkCmdCopyBufferToImage` and `vkCmdBlitImage` cannot blend;
// they would stamp a 32x32 opaque block onto the frame. Blending needs a draw,
// so a draw is what this is: four vertices, one texture, one blend state.
//
// None of it is throwaway. A textured quad alpha-blended into the eye image at
// an arbitrary rectangle is precisely what task #30's floating UI canvas needs.
//
// **Everything per-image, nothing shared across frames in flight.** Each
// swapchain image gets its own staging buffer, cursor texture, descriptor set
// and framebuffers. That is not caution, it is what makes the upload safe with
// no fence of its own: `SbsCompositor::composite()` has already waited on this
// image's fence before it calls in here, so this image's previous submission
// has retired and its resources are free to rewrite. One shared texture would
// need a stall or a ring to say the same thing.
#pragma once

#include <cstring>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../common/x4vr_log.hpp"
#include "../common/x4vr_share.hpp"
#include "cursor_shaders.hpp"

namespace x4vr {

#define X4VR_CURSOR_FNS(X)                                                     \
    X(CreateShaderModule) X(DestroyShaderModule)                               \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)                 \
    X(CreateDescriptorPool) X(DestroyDescriptorPool)                           \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets)                          \
    X(CreatePipelineLayout) X(DestroyPipelineLayout)                           \
    X(CreateGraphicsPipelines) X(DestroyPipeline)                              \
    X(CreateRenderPass) X(DestroyRenderPass)                                   \
    X(CreateFramebuffer) X(DestroyFramebuffer)                                 \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler)  \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)            \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory)                            \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements)               \
    X(AllocateMemory) X(FreeMemory) X(BindImageMemory)                         \
    X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdBeginRenderPass)        \
    X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindDescriptorSets)            \
    X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor) X(CmdDraw)

// ---------------------------------------------------------------- placement
//
// The two things here that are easy to get silently wrong live outside the
// class, as plain functions over plain numbers, so `tests/cursor_place.cpp` can
// check them without a GPU, a device, or the layer. Both failures are invisible
// in code review and obvious on screen: swapped colour channels, and a pointer
// that misses what it points at by up to 31 px.

inline bool cursor_is_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// SDL's pixel-format id -> the Vulkan format that consumes those bytes
// unchanged, or VK_FORMAT_UNDEFINED for one this does not know.
//
// SDL names packed formats most-significant-byte-first, so ARGB8888 is the word
// 0xAARRGGBB and on a little-endian machine its *memory* order is B,G,R,A --
// which is exactly what VK_FORMAT_B8G8R8A8 reads. That is a re-labelling, not a
// conversion: no bytes move. Take 94 measured 0x16362004, and both constants
// below were printed from SDL3's own headers rather than recalled.
//
// The sRGB-ness follows the eye image. A cursor sampled as sRGB and written to
// an sRGB attachment round-trips exactly; mismatch either end and the glyph
// comes out visibly too bright or too dark. Both candidate formats are in
// Vulkan's mandatory table for SAMPLED_IMAGE, so there is nothing to query.
inline VkFormat cursor_pixel_format(uint32_t sdl_format, VkFormat eye) {
    const bool srgb = cursor_is_srgb(eye);
    switch (sdl_format) {
    case 0x16362004u: // SDL_PIXELFORMAT_ARGB8888 -> bytes B,G,R,A
        return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    case 0x16762004u: // SDL_PIXELFORMAT_ABGR8888 -> bytes R,G,B,A
        return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

// The quad, in normalised device coordinates.
//
// The hot spot is the pixel that *is* the pointer, so the bitmap hangs off it
// rather than starting at it. X4 varies it per cursor -- take 94 saw (7,8),
// (0,0), (12,19) and (11,6) -- which is why it travels on the channel instead
// of being assumed to be a corner or a centre. Ignore it and a 32x32 arrow
// points at something up to 31 px away from what X4 hit-tests.
//
// Vulkan's NDC y runs the same way as the framebuffer's, top to bottom, so the
// mapping is the same in both axes and no flip belongs here.
struct CursorRect {
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    bool onscreen = false; // false when the glyph misses the eye entirely
};

inline CursorRect cursor_rect(float px, float py, int32_t hot_x, int32_t hot_y,
                              uint32_t cw, uint32_t ch, uint32_t eye_w,
                              uint32_t eye_h) {
    CursorRect r;
    if (!eye_w || !eye_h || !cw || !ch)
        return r;
    const float x0 = px - (float)hot_x, y0 = py - (float)hot_y;
    const float x1 = x0 + (float)cw, y1 = y0 + (float)ch;
    const float w = (float)eye_w, h = (float)eye_h;
    if (x1 <= 0.f || y1 <= 0.f || x0 >= w || y0 >= h)
        return r;
    r.x0 = 2.f * x0 / w - 1.f;
    r.y0 = 2.f * y0 / h - 1.f;
    r.x1 = 2.f * x1 / w - 1.f;
    r.y1 = 2.f * y1 / h - 1.f;
    r.onscreen = true;
    return r;
}

// ------------------------------------------------------------------ the draw

struct CursorFns {
#define X4VR_DECL(name) PFN_vk##name name = nullptr;
    X4VR_CURSOR_FNS(X4VR_DECL)
#undef X4VR_DECL

    bool complete() const {
#define X4VR_CHECK(name)                                                       \
    if (!name)                                                                 \
        return false;
        X4VR_CURSOR_FNS(X4VR_CHECK)
#undef X4VR_CHECK
        return true;
    }
};

class CursorOverlay {
public:
    // What the draw is aimed at. Passed as a struct because six positional
    // arguments of which three are uint32_t is exactly the shape of call that
    // silently swaps two of them.
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        uint32_t index = 0; // swapchain image index -- keys the per-image state
        uint32_t count = 1; // how many swapchain images there are
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        uint32_t layers = 1;
    };

    void configure(VkDevice device, const CursorFns &fns,
                   const VkPhysicalDeviceMemoryProperties &mem) {
        device_ = device;
        fns_ = fns;
        mem_ = mem;
    }

    bool ready() const { return device_ && fns_.complete(); }

    // Blend the cursor into every layer of `t.image`.
    //
    // `canvas_shift` is task #30's per-view NDC x offset, or 0 for no canvas.
    // The pointer has to take the same shift the UI takes or it separates from
    // the thing it activates: X4 hit-tests CPU-side at an unshifted window
    // coordinate, so if the menu moves and the cursor does not, every button
    // still *works* while the pointer sits `s` away from it -- both halves
    // behaving exactly as designed and the result visibly wrong. Layer 0 is
    // view 0 is the left eye and takes +s, matching the sign gl_ViewIndex
    // selects in the patched module. Keyed on the view index rather than on
    // X4VR_SBS_RIGHT_LAYER, because that knob swaps which half of the
    // composite a layer lands in, not which eye the layer *is*.
    //
    // Returns true if it drew, in which case the image is left in
    // COLOR_ATTACHMENT_OPTIMAL and the caller's own barrier must say so. On
    // false it has recorded nothing and the image is untouched -- so a failure
    // here costs the cursor, never the frame.
    bool record(VkCommandBuffer cb, const Target &t, const Shared *shared,
                float canvas_shift = 0.f) {
        if (!ready() || !shared || t.image == VK_NULL_HANDLE || !t.extent.width ||
            !t.extent.height)
            return false;

        float px = 0.f, py = 0.f;
        bool visible = false;
        if (!share_read(shared, &px, &py, &visible))
            return false;

        uint32_t cw = 0, ch = 0, fmt = 0, pitch = 0;
        int32_t hot_x = 0, hot_y = 0;
        if (!share_read_cursor(shared, scratch_, &cw, &ch, &hot_x, &hot_y, &fmt,
                               &pitch))
            return false; // X4 has not built a cursor yet
        // The injector repacks every capture to a tight w*4 stride, so this is
        // an invariant of the channel rather than a case to handle. Checked
        // because the upload below reads rows at that stride and a silent
        // mismatch would shear the glyph instead of failing.
        if (pitch != cw * 4u)
            return false;

        const VkFormat tex_fmt = cursor_format(fmt, t.format);
        if (tex_fmt == VK_FORMAT_UNDEFINED)
            return false; // already logged, once
        if (cursor_tex_format_ == VK_FORMAT_UNDEFINED) {
            cursor_tex_format_ = tex_fmt;
        } else if (cursor_tex_format_ != tex_fmt) {
            // The per-image textures are allocated with the first format seen.
            // X4 has only ever produced one, so this is a refusal rather than a
            // reallocation path that would never be exercised.
            static bool said = false;
            if (!said) {
                said = true;
                X4VR_LOG("cursor: X4 switched cursor pixel format mid-session "
                         "(%s -> %s) — keeping the first, later cursors are not "
                         "drawn",
                         format_name(cursor_tex_format_), format_name(tex_fmt));
            }
            return false;
        }

        // Drawn whether or not the channel calls it visible. X4 imports neither
        // SDL_ShowCursor nor SDL_HideCursor (checked with `nm -D` rather than
        // assumed -- four instruments in this project have been hooked to
        // symbols X4 never calls), so `visible` is the injector's *inference*
        // from relative-mouse mode, not something X4 said. Acting on an
        // inference would make the cursor vanish for reasons no measurement has
        // pinned down. If a stray pointer turns out to sit in the middle of the
        // cockpit during mouse-look, gating on `visible` is the one-line change
        // -- but that should follow a take that shows it, not precede one.
        (void)visible;

        if (!ensure_shared(t.format))
            return false;
        Slot *s = ensure_slot(t);
        if (!s)
            return false;

        // The cursor image changes only when X4 switches cursor, so the upload
        // is keyed on the channel's own sequence number rather than done every
        // frame. Sequence 0 means nothing captured, which share_read_cursor has
        // already refused.
        const uint32_t seq = shared->cursor_img_seq.load(std::memory_order_relaxed);
        if (s->uploaded_seq != seq || !s->tex_ready) {
            upload(cb, *s, scratch_, cw, ch);
            s->uploaded_seq = seq;
            s->tex_ready = true;
        }

        const CursorRect q = cursor_rect(px, py, hot_x, hot_y, cw, ch,
                                         t.extent.width, t.extent.height);
        if (!q.onscreen)
            return false; // wholly outside the eye -- nothing to draw

        struct Push {
            float rect[4];
            float uv[4];
        } push{{q.x0, q.y0, q.x1, q.y1},
               // Only the top-left cw x ch of the atlas holds this cursor; the
               // texture is allocated at the channel's maximum so a cursor
               // change never reallocates.
               {0.f, 0.f, (float)cw / (float)Shared::kCursorMax,
                (float)ch / (float)Shared::kCursorMax}};

        const float w = (float)t.extent.width, h = (float)t.extent.height;

        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t.image;
        // X4 left it ready to present, believing it was the swapchain. Every
        // layer, because both eyes are drawn into.
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, t.layers};
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &b);

        VkViewport vp{0.f, 0.f, w, h, 0.f, 1.f};
        VkRect2D sc{{0, 0}, t.extent};
        for (uint32_t l = 0; l < s->fbs.size(); l++) {
            // +s for view 0, -s for view 1. Both x components, so the quad
            // translates rather than stretching -- the cursor is a 1:1 bitmap
            // and any scale would show as a blurred pointer.
            Push p = push;
            const float dx = l == 0 ? canvas_shift : -canvas_shift;
            p.rect[0] += dx;
            p.rect[2] += dx;
            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = pass_;
            rp.framebuffer = s->fbs[l];
            rp.renderArea = sc;
            fns_.CmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
            fns_.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
            fns_.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       layout_, 0, 1, &s->set, 0, nullptr);
            fns_.CmdSetViewport(cb, 0, 1, &vp);
            fns_.CmdSetScissor(cb, 0, 1, &sc);
            fns_.CmdPushConstants(cb, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                  sizeof(p), &p);
            fns_.CmdDraw(cb, 4, 1, 0, 0);
            fns_.CmdEndRenderPass(cb);
        }

        if (!drew_) {
            drew_ = true;
            X4VR_LOG("cursor: drawing %ux%u hot=(%d,%d) into %u layer(s) of the "
                     "%ux%u eye — first at x=%.1f y=%.1f (channel says %s), "
                     "texture %s into an eye of %s, canvas shift %.5f NDC "
                     "(%.1f px per eye)",
                     cw, ch, hot_x, hot_y, (unsigned)s->fbs.size(),
                     t.extent.width, t.extent.height, px, py,
                     visible ? "visible" : "hidden",
                     format_name(tex_fmt), format_name(t.format), canvas_shift,
                     canvas_shift * 0.5f * (float)t.extent.width);
        }
        return true;
    }

    // Drop everything hanging off one eye image, because the image is about to
    // be destroyed. A swapchain recreate (a resize, or gamescope changing the
    // output) does exactly that, and a framebuffer that outlives its attachment
    // is a use-after-free the validation layers would be right to shout about.
    void forget(VkImage image) {
        if (!device_ || !fns_.complete())
            return;
        auto it = slots_.find(image);
        if (it == slots_.end())
            return;
        destroy_slot(it->second);
        slots_.erase(it);
    }

    void shutdown() {
        if (!device_ || !fns_.complete())
            return;
        for (auto &kv : slots_)
            destroy_slot(kv.second);
        slots_.clear();
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
        cursor_tex_format_ = VK_FORMAT_UNDEFINED;
        shared_failed_ = false;
        drew_ = false;
        device_ = VK_NULL_HANDLE;
    }

private:
    struct Slot {
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkImage tex = VK_NULL_HANDLE;
        VkDeviceMemory tex_mem = VK_NULL_HANDLE;
        VkImageView tex_view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::vector<VkImageView> views; // one per eye layer
        std::vector<VkFramebuffer> fbs;
        uint32_t uploaded_seq = 0;
        bool tex_ready = false;
    };

    static const char *format_name(VkFormat f) {
        switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
        default:                       return "other";
        }
    }

    // cursor_pixel_format, plus the one-time complaint that belongs to a
    // running layer rather than to a pure function a test can call.
    VkFormat cursor_format(uint32_t sdl_format, VkFormat eye) const {
        const VkFormat f = cursor_pixel_format(sdl_format, eye);
        if (f != VK_FORMAT_UNDEFINED)
            return f;
        static uint32_t said = 0;
        if (said != sdl_format) {
            said = sdl_format;
            X4VR_LOG("cursor: SDL pixel format 0x%08x is not one this knows how "
                     "to consume — no cursor drawn. Add it to "
                     "cursor_pixel_format() rather than letting a guess mangle "
                     "the channels.",
                     sdl_format);
        }
        return VK_FORMAT_UNDEFINED;
    }

    uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mem_.memoryTypeCount; i++)
            if ((bits & (1u << i)) &&
                (mem_.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    }

    // The pipeline, and everything it hangs off. Built once, on the first frame
    // that has something to draw, because it needs the eye's format and that is
    // not known until then.
    bool ensure_shared(VkFormat eye_format) {
        if (pipe_ != VK_NULL_HANDLE)
            return pass_format_ == eye_format;
        // One attempt, ever. A retry would overwrite the handles of whatever
        // the failed attempt did manage to create, and leak them past
        // shutdown() -- which destroys exactly what the members name.
        if (shared_failed_)
            return false;
        shared_failed_ = true;
        pass_format_ = eye_format;

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // Nearest, deliberately. The quad is drawn at exactly 1:1 pixel scale,
        // so linear filtering could only soften a crisp 32x32 glyph that X4
        // authored to be crisp.
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.f;
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
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

        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 8 * sizeof(float)};
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &dsl_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        if (fns_.CreatePipelineLayout(device_, &pli, nullptr, &layout_) !=
            VK_SUCCESS)
            return fail("pipeline layout");

        // LOAD/STORE, because the frame is already there and the cursor goes on
        // top of it. A CLEAR here would erase X4's frame, which is the single
        // most destructive thing this file could get wrong.
        VkAttachmentDescription at{};
        at.format = eye_format;
        at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ar;
        VkSubpassDependency dep[2]{};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_SHADER_READ_BIT;
        dep[1] = dep[0];
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_MEMORY_READ_BIT;
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
        smi.codeSize = sizeof(kCursorQuadVert);
        smi.pCode = kCursorQuadVert;
        if (fns_.CreateShaderModule(device_, &smi, nullptr, &vs) != VK_SUCCESS)
            return fail("vertex shader");
        smi.codeSize = sizeof(kCursorQuadFrag);
        smi.pCode = kCursorQuadFrag;
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
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
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
        // Straight (non-premultiplied) alpha, which is what SDL cursor surfaces
        // carry. The colour channels are scaled by alpha here rather than in
        // the shader; doing both would darken the glyph by its own alpha twice.
        VkPipelineColorBlendAttachmentState ba{};
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
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
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &ds;
        gp.layout = layout_;
        gp.renderPass = pass_;
        gp.subpass = 0;
        const VkResult r = fns_.CreateGraphicsPipelines(device_, VK_NULL_HANDLE,
                                                        1, &gp, nullptr, &pipe_);
        fns_.DestroyShaderModule(device_, vs, nullptr);
        fns_.DestroyShaderModule(device_, fs, nullptr);
        if (r != VK_SUCCESS)
            return fail("graphics pipeline");
        shared_failed_ = false;
        X4VR_LOG("cursor: overlay pipeline built for a %s eye",
                 format_name(eye_format));
        return true;
    }

    Slot *ensure_slot(const Target &t) {
        auto it = slots_.find(t.image);
        if (it != slots_.end())
            return &it->second;

        if (pool_ == VK_NULL_HANDLE) {
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    t.count};
            VkDescriptorPoolCreateInfo pi{};
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.maxSets = t.count;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            if (fns_.CreateDescriptorPool(device_, &pi, nullptr, &pool_) !=
                VK_SUCCESS) {
                fail("descriptor pool");
                return nullptr;
            }
        }

        Slot s;
        const VkDeviceSize bytes =
            (VkDeviceSize)Shared::kCursorMax * Shared::kCursorMax * 4;

        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (fns_.CreateBuffer(device_, &bi, nullptr, &s.staging) != VK_SUCCESS) {
            fail("staging buffer");
            return nullptr;
        }
        VkMemoryRequirements br{};
        fns_.GetBufferMemoryRequirements(device_, s.staging, &br);
        const uint32_t bt =
            find_mem(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = br.size;
        ai.memoryTypeIndex = bt;
        if (bt == UINT32_MAX ||
            fns_.AllocateMemory(device_, &ai, nullptr, &s.staging_mem) !=
                VK_SUCCESS ||
            fns_.BindBufferMemory(device_, s.staging, s.staging_mem, 0) !=
                VK_SUCCESS ||
            fns_.MapMemory(device_, s.staging_mem, 0, VK_WHOLE_SIZE, 0,
                           &s.mapped) != VK_SUCCESS) {
            destroy_slot(s);
            fail("staging memory");
            return nullptr;
        }

        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = cursor_tex_format_;
        ii.extent = {Shared::kCursorMax, Shared::kCursorMax, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (fns_.CreateImage(device_, &ii, nullptr, &s.tex) != VK_SUCCESS) {
            destroy_slot(s);
            fail("cursor texture");
            return nullptr;
        }
        VkMemoryRequirements ir{};
        fns_.GetImageMemoryRequirements(device_, s.tex, &ir);
        const uint32_t itype =
            find_mem(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ai.allocationSize = ir.size;
        ai.memoryTypeIndex = itype;
        if (itype == UINT32_MAX ||
            fns_.AllocateMemory(device_, &ai, nullptr, &s.tex_mem) !=
                VK_SUCCESS ||
            fns_.BindImageMemory(device_, s.tex, s.tex_mem, 0) != VK_SUCCESS) {
            destroy_slot(s);
            fail("cursor texture memory");
            return nullptr;
        }

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = cursor_tex_format_;
        vi.image = s.tex;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (fns_.CreateImageView(device_, &vi, nullptr, &s.tex_view) !=
            VK_SUCCESS) {
            destroy_slot(s);
            fail("cursor texture view");
            return nullptr;
        }

        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = pool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl_;
        if (fns_.AllocateDescriptorSets(device_, &dai, &s.set) != VK_SUCCESS) {
            destroy_slot(s);
            fail("descriptor set");
            return nullptr;
        }
        VkDescriptorImageInfo dii{sampler_, s.tex_view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{};
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = s.set;
        wr.dstBinding = 0;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &dii;
        fns_.UpdateDescriptorSets(device_, 1, &wr, 0, nullptr);

        // One view and one framebuffer per eye layer. Two tiny render passes
        // beat one multiview pass here: multiview would need the pipeline to
        // know the view mask, and both eyes want the cursor in the *same* place
        // -- a pointer sits on the convergence plane, not at some depth the
        // shim would have to invent.
        for (uint32_t l = 0; l < t.layers; l++) {
            VkImageViewCreateInfo ev{};
            ev.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ev.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ev.format = t.format;
            ev.image = t.image;
            ev.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (fns_.CreateImageView(device_, &ev, nullptr, &view) !=
                VK_SUCCESS) {
                destroy_slot(s);
                fail("eye image view");
                return nullptr;
            }
            s.views.push_back(view);
            VkFramebufferCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fi.renderPass = pass_;
            fi.attachmentCount = 1;
            fi.pAttachments = &view;
            fi.width = t.extent.width;
            fi.height = t.extent.height;
            fi.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (fns_.CreateFramebuffer(device_, &fi, nullptr, &fb) !=
                VK_SUCCESS) {
                destroy_slot(s);
                fail("framebuffer");
                return nullptr;
            }
            s.fbs.push_back(fb);
        }

        auto ins = slots_.emplace(t.image, std::move(s));
        return &ins.first->second;
    }

    // Copy the channel's pixels into this slot's staging buffer and record the
    // upload. Safe without a fence of its own: composite() waited on this
    // image's fence, so the previous submission that read this buffer and wrote
    // this texture has retired.
    void upload(VkCommandBuffer cb, Slot &s, const uint8_t *src, uint32_t cw,
                uint32_t ch) {
        // Repacked into the atlas's stride so the copy below is one region.
        auto *dst = (uint8_t *)s.mapped;
        memset(dst, 0, (size_t)Shared::kCursorMax * Shared::kCursorMax * 4);
        for (uint32_t y = 0; y < ch; y++)
            memcpy(dst + (size_t)y * Shared::kCursorMax * 4,
                   src + (size_t)y * cw * 4, (size_t)cw * 4);

        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s.tex;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // UNDEFINED on the first upload and on every later one: the whole image
        // is overwritten, so discarding the old contents is both legal and what
        // we want.
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                nullptr, 1, &b);

        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {Shared::kCursorMax, Shared::kCursorMax, 1};
        fns_.CmdCopyBufferToImage(cb, s.staging, s.tex,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                nullptr, 0, nullptr, 1, &b);
    }

    void destroy_slot(Slot &s) {
        for (VkFramebuffer f : s.fbs)
            if (f)
                fns_.DestroyFramebuffer(device_, f, nullptr);
        for (VkImageView v : s.views)
            if (v)
                fns_.DestroyImageView(device_, v, nullptr);
        if (s.tex_view)
            fns_.DestroyImageView(device_, s.tex_view, nullptr);
        if (s.tex)
            fns_.DestroyImage(device_, s.tex, nullptr);
        if (s.tex_mem)
            fns_.FreeMemory(device_, s.tex_mem, nullptr);
        if (s.mapped)
            fns_.UnmapMemory(device_, s.staging_mem);
        if (s.staging)
            fns_.DestroyBuffer(device_, s.staging, nullptr);
        if (s.staging_mem)
            fns_.FreeMemory(device_, s.staging_mem, nullptr);
        s = Slot{};
    }

    bool fail(const char *what) {
        X4VR_LOG("cursor: %s creation failed — no cursor drawn", what);
        return false;
    }

    VkDevice device_ = VK_NULL_HANDLE;
    CursorFns fns_;
    VkPhysicalDeviceMemoryProperties mem_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkFormat pass_format_ = VK_FORMAT_UNDEFINED;
    VkFormat cursor_tex_format_ = VK_FORMAT_UNDEFINED;
    std::unordered_map<VkImage, Slot> slots_;
    uint8_t scratch_[Shared::kCursorMax * Shared::kCursorMax * 4] = {};
    bool shared_failed_ = false;
    bool drew_ = false;
};

} // namespace x4vr
