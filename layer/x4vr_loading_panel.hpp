// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_loading_panel.hpp — what the headset shows until X4 draws.
//
// The VR session is up seconds after X4 starts, and X4's first frame can be a
// minute later: on a first start it reads its data from disk and the driver
// compiles every shader. The frame loop had nothing to submit in between, so the
// headset showed the runtime's empty space for as long as that took, and an
// RTX 5090 player stopped the game after half a minute of it, taking it for a hang
// (2026-11-09; his log showed X4 loading, then drawing). Now a panel stands in
// front of the player for that time: our art, and "X4 is loading" in their
// language, until X4 draws steadily.
//
// Steadily, not first: X4's first frames came, and then 6.5 s of nothing while it
// loaded its menu, so the panel stays until frames come at a pace (LoadingGate).
//
// What's here is what can be tested without a headset: the language, the picture,
// where the panel stands, the gate, and the one upload into the runtime's image
// (tests/loading_panel.cpp, and on a GPU tests/run-loading-panel.sh). Where it is
// called from, and how it keeps out of X4's own copy, is in x4vr_layer.cpp's frame
// loop.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "../common/x4vr_png.hpp"

namespace x4vr {

// ------------------------------------------------------------------ language

// One of the launcher's languages for the panel's words. `x4vrmod run` passes the
// launcher's in X4VR_LANGUAGE; started some other way, the environment's, as
// gettext would read it; else English.
inline std::string loading_language(const char *x4vr_language, const char *language,
                                    const char *lc_all, const char *lc_messages,
                                    const char *lang) {
    static const char *const offered[] = {"en", "pt_BR", "fr", "ru", "de"};
    auto pick = [](const std::string &l) -> std::string {
        const std::string full = l.substr(0, l.find_first_of(".@:"));
        const std::string two = full.substr(0, full.find('_'));
        for (const char *o : offered)
            if (full == o)
                return o;
        for (const char *o : offered)
            if (two == o)
                return o;
        return two == "pt" ? "pt_BR" : "";
    };
    for (const char *v : {x4vr_language, language, lc_all, lc_messages, lang}) {
        if (!v || !*v)
            continue;
        const std::string s = v;
        // LANGUAGE is a list; the first one offered wins.
        for (size_t at = 0; at <= s.size();) {
            size_t end = s.find(':', at);
            if (end == std::string::npos)
                end = s.size();
            const std::string got = pick(s.substr(at, end - at));
            if (!got.empty())
                return got;
            at = end + 1;
        }
        if (v != language) // LC_ALL and the rest name one locale: it decides
            return "en";
    }
    return "en";
}

// ------------------------------------------------------------------- picture

// The art on top and the words under it, light on a dark band: the words come as
// coverage (tools/make_vr_loading.py draws them white on black), so any language's
// strip goes under the same art. 8-bit sRGB values, as the PNGs hold them, in the
// byte order of the runtime's format: R,G,B,A, or B,G,R,A when `bgra`.
inline bool compose_loading_panel(const png::Image &art, const png::Image &words, bool bgra,
                                  std::vector<uint8_t> *out, uint32_t *w, uint32_t *h) {
    if (!art.w || !art.h || words.w != art.w || !words.h || !out)
        return false;
    static const uint8_t band[3] = {14, 18, 26}, ink[3] = {236, 240, 245};
    *w = art.w;
    *h = art.h + words.h;
    out->assign((size_t)*w * *h * 4, 255);
    for (uint32_t y = 0; y < *h; y++)
        for (uint32_t x = 0; x < *w; x++) {
            uint8_t rgb[3];
            if (y < art.h) {
                const uint8_t *s = &art.rgba[((size_t)y * art.w + x) * 4];
                rgb[0] = s[0], rgb[1] = s[1], rgb[2] = s[2];
            } else {
                const uint32_t c = words.rgba[((size_t)(y - art.h) * words.w + x) * 4];
                for (int i = 0; i < 3; i++)
                    rgb[i] = (uint8_t)((band[i] * (255 - c) + ink[i] * c + 127) / 255);
            }
            uint8_t *o = &(*out)[((size_t)y * *w + x) * 4];
            o[0] = bgra ? rgb[2] : rgb[0];
            o[1] = rgb[1];
            o[2] = bgra ? rgb[0] : rgb[2];
        }
    return true;
}

// The picture in the top-left corner of a larger image, the rest clear: for a
// runtime that refuses an image of the picture's own size (take 425: WiVRn
// refused 738x520 in format 43 with XR_ERROR_RUNTIME_FAILURE, SteamVR took it).
// The quad then names only the picture's rectangle.
inline uint32_t loading_round_up(uint32_t v, uint32_t to) { return (v + to - 1) / to * to; }

inline std::vector<uint8_t> loading_pad(const std::vector<uint8_t> &px, uint32_t w, uint32_t h,
                                        uint32_t pw, uint32_t ph) {
    std::vector<uint8_t> out((size_t)pw * ph * 4, 0);
    for (uint32_t y = 0; y < h && y < ph; y++)
        memcpy(&out[(size_t)y * pw * 4], &px[(size_t)y * w * 4], (size_t)std::min(w, pw) * 4);
    return out;
}

// The shapes the panel's swapchain is asked for, in order, until the runtime
// takes one: each one step nearer the eye swapchain's, which every runtime has
// taken — its format, then a size in multiples of 64, then two layers. The
// picture's own shape comes first, so a runtime that refuses it says so in
// every run. A step that changes nothing is left out.
//
// WiVRn refuses every one-layer shape (takes 425, 426), and the cause is this
// layer: WiVRn's client makes the image again on X4's device, through the
// multiview hook, which doubles every one-layer colour attachment there
// (mv_double_candidate). The doubled image needs twice the memory WiVRn
// exported, and its client says so on X4's stderr: "size mismatch, exported
// 1966080 but requires 3932160". Two layers pass the hook as they are, which
// is why the eye swapchain was always taken, and the fourth shape is (take
// 426). SteamVR takes the first.
struct LoadingShape {
    VkFormat fmt;
    uint32_t w, h, layers;
    bool operator==(const LoadingShape &o) const {
        return fmt == o.fmt && w == o.w && h == o.h && layers == o.layers;
    }
};

inline std::vector<LoadingShape> loading_shapes(VkFormat first, VkFormat eye, uint32_t w,
                                                uint32_t h) {
    const uint32_t pw = loading_round_up(w, 64), ph = loading_round_up(h, 64);
    const LoadingShape all[] = {{first, w, h, 1}, {eye, w, h, 1}, {eye, pw, ph, 1},
                                {eye, pw, ph, 2}};
    std::vector<LoadingShape> out;
    for (const LoadingShape &s : all)
        if (out.empty() || !(out.back() == s))
            out.push_back(s);
    return out;
}

// ----------------------------------------------------------------- placement

inline constexpr float kLoadingDistance = 1.8f; // metres in front of the head
inline constexpr float kLoadingWidth = 1.6f;    // metres: about 48 deg across
inline constexpr float kLoadingRecentreDeg = 50.f;

// The heading of a head orientation: 0 looking down -Z, positive turning left
// (towards -X), as a rotation about +Y.
inline float loading_yaw(const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // The head's forward, -Z rotated by q.
    const float fx = -(2.f * (x * z + w * y)), fz = -(1.f - 2.f * (x * x + y * y));
    return std::atan2(-fx, -fz);
}

inline float loading_yaw_apart(float a, float b) {
    float d = a - b;
    while (d > 3.14159265f)
        d -= 6.28318531f;
    while (d < -3.14159265f)
        d += 6.28318531f;
    return std::fabs(d) * 57.2957795f;
}

struct LoadingPlace {
    float pos[3] = {0.f, 0.f, 0.f};
    float q[4] = {0.f, 0.f, 0.f, 1.f};
    float yaw = 0.f;
};

// Level, at eye height, `dist` metres along the heading, facing the head. Not
// head-locked: it stays where it stood unless the head turns well away
// (kLoadingRecentreDeg), so it can't be lost behind the player.
inline LoadingPlace loading_place(const float head[3], float yaw, float dist) {
    LoadingPlace p;
    p.yaw = yaw;
    p.pos[0] = head[0] - dist * std::sin(yaw);
    p.pos[1] = head[1];
    p.pos[2] = head[2] - dist * std::cos(yaw);
    p.q[1] = std::sin(0.5f * yaw);
    p.q[3] = std::cos(0.5f * yaw);
    return p;
}

// ---------------------------------------------------------------------- gate

// Whether X4 is drawing yet: frames at a pace, not a first one. Fed the count of
// X4 frames copied for the headset each headset frame; once steady, steady for good,
// so a later pause (a save loading) never brings the panel back.
class LoadingGate {
public:
    static constexpr int kFrames = 10;       // within...
    static constexpr double kWindow = 1.5;   // ...this many seconds,
    static constexpr uint64_t kTotal = 120;  // or this many in all
    void observe(double now, uint64_t copied) {
        if (steady_)
            return;
        while (copied > last_) {
            times_.push_back(now);
            last_++;
        }
        while (!times_.empty() && now - times_.front() > kWindow)
            times_.pop_front();
        if ((int)times_.size() >= kFrames || copied >= kTotal) {
            steady_ = true;
            steady_at_ = now;
        }
    }
    bool steady() const { return steady_; }
    double steady_at() const { return steady_at_; }

private:
    bool steady_ = false;
    double steady_at_ = 0.0;
    uint64_t last_ = 0;
    std::deque<double> times_;
};

// -------------------------------------------------------------------- upload

#define X4VR_LOADING_FNS(X)                                                    \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)            \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory)           \
    X(UnmapMemory) X(CreateCommandPool) X(DestroyCommandPool)                  \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)        \
    X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CreateFence)               \
    X(DestroyFence) X(WaitForFences) X(GetDeviceQueue) X(QueueSubmit)

struct LoadingFns {
#define X4VR_DECL(name) PFN_vk##name name = nullptr;
    X4VR_LOADING_FNS(X4VR_DECL)
#undef X4VR_DECL
    bool complete() const {
#define X4VR_CHECK(name)                                                       \
    if (!name)                                                                 \
        return false;
        X4VR_LOADING_FNS(X4VR_CHECK)
#undef X4VR_CHECK
        return true;
    }
};

// Copies `px` into `dst` once, on the queue the runtime was given, and leaves it in
// COLOR_ATTACHMENT_OPTIMAL, the layout OpenXR takes a released colour image in.
// `submit` makes the one vkQueueSubmit, so the caller can take whatever lock that
// queue needs; nothing here is held across a call into the runtime, and the wait
// is on our own fence, bounded.
class LoadingUpload {
public:
    using Submit = std::function<VkResult(VkQueue, const VkSubmitInfo &, VkFence)>;

    void configure(VkDevice device, const LoadingFns &fns,
                   const VkPhysicalDeviceMemoryProperties &mem) {
        device_ = device;
        fns_ = fns;
        mem_ = mem;
    }
    bool ready() const { return device_ != VK_NULL_HANDLE && fns_.complete(); }

    bool upload(VkImage dst, uint32_t w, uint32_t h, const std::vector<uint8_t> &px,
                uint32_t family, uint32_t index, const Submit &submit, std::string *why) {
        auto fail = [&](const char *what) {
            if (why)
                *why = what;
            return false;
        };
        if (!ready())
            return fail("not configured");
        if (px.size() != (size_t)w * h * 4)
            return fail("the picture isn't the image's size");
        bool ok = false;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        do {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = px.size();
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (fns_.CreateBuffer(device_, &bi, nullptr, &buf) != VK_SUCCESS) {
                fail("staging buffer");
                break;
            }
            VkMemoryRequirements mr{};
            fns_.GetBufferMemoryRequirements(device_, buf, &mr);
            const uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < mem_.memoryTypeCount && type == UINT32_MAX; i++)
                if ((mr.memoryTypeBits & (1u << i)) &&
                    (mem_.memoryTypes[i].propertyFlags & want) == want)
                    type = i;
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = type;
            void *map = nullptr;
            if (type == UINT32_MAX ||
                fns_.AllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS ||
                fns_.BindBufferMemory(device_, buf, mem, 0) != VK_SUCCESS ||
                fns_.MapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
                fail("staging memory");
                break;
            }
            memcpy(map, px.data(), px.size());
            fns_.UnmapMemory(device_, mem);

            VkCommandPoolCreateInfo pi{};
            pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pi.queueFamilyIndex = family;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ci.commandBufferCount = 1;
            ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            if (fns_.CreateCommandPool(device_, &pi, nullptr, &pool) != VK_SUCCESS ||
                (ci.commandPool = pool,
                 fns_.AllocateCommandBuffers(device_, &ci, &cb) != VK_SUCCESS)) {
                fail("command buffer");
                break;
            }
            // A command buffer made here never went through the loader's
            // trampoline, which writes the device's dispatch pointer into it; the
            // layer below this one reads that pointer, so it is copied in by hand,
            // as Vulkan's layer interface asks.
            *(void **)cb = *(void **)device_;
            VkCommandBufferBeginInfo cbi{};
            cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            fns_.BeginCommandBuffer(cb, &cbi);
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dst;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &b);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {w, h, 1};
            fns_.CmdCopyBufferToImage(cb, buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      1, &r);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            fns_.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                                    nullptr, 1, &b);
            if (fns_.EndCommandBuffer(cb) != VK_SUCCESS) {
                fail("recording");
                break;
            }
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (fns_.CreateFence(device_, &fi, nullptr, &fence) != VK_SUCCESS) {
                fail("fence");
                break;
            }
            VkQueue q = VK_NULL_HANDLE;
            fns_.GetDeviceQueue(device_, family, index, &q);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            if (!q || submit(q, si, fence) != VK_SUCCESS) {
                fail("submit");
                break;
            }
            // Five seconds is far past a copy of a megabyte; past it, the fence
            // is left to the device rather than freeing what the GPU may still read.
            if (fns_.WaitForFences(device_, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
                fence = VK_NULL_HANDLE;
                pool = VK_NULL_HANDLE;
                buf = VK_NULL_HANDLE;
                mem = VK_NULL_HANDLE;
                fail("the copy didn't finish in 5 s");
                break;
            }
            ok = true;
        } while (false);
        if (fence)
            fns_.DestroyFence(device_, fence, nullptr);
        if (pool)
            fns_.DestroyCommandPool(device_, pool, nullptr);
        if (buf)
            fns_.DestroyBuffer(device_, buf, nullptr);
        if (mem)
            fns_.FreeMemory(device_, mem, nullptr);
        return ok;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    LoadingFns fns_;
    VkPhysicalDeviceMemoryProperties mem_{};
};

} // namespace x4vr
