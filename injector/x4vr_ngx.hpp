// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_ngx.hpp -- DLSS in both eyes (X4VR_UPSCALE=dlss_*). Patola asked for a best-effort
// build on 2026-11-06, for an RTX 5090 and a Steam Frame to test. **It has never run on an
// NVIDIA card:** the development PC is AMD, so this is read from X4's code and NVIDIA's
// public headers, tested against a fake NGX (tests/ngx_stereo.cpp).
//
// **Why a patch.** X4 links NVIDIA's NGX statically and calls it directly, so it can't be
// interposed, and evaluating records one opaque launch (VK_NVX_binary_import) the layer can't
// repeat for the right eye the way it does FidelityFX's dispatches (X4VR_FFX_STEREO). Asked
// once, DLSS upscales the left eye only and the right eye's scene stays black (take 381 is
// the same for FSR 3).
//
// **What.** X4 has one call each to CreateFeature (0x20e3788), EvaluateFeature_C (0x20e35b7)
// and ReleaseFeature (0x20e28a5). Each `call` becomes a call to a stub with the same
// arguments, which does what X4 asked and then the same again for the right eye:
//   * a second feature, because each keeps its own history;
//   * a second evaluation with X4's four images swapped for layer 1 of the same images. X4's
//     images are doubled and its views name layer 0, as FidelityFX found them. The layer makes
//     the views (its ffx_right_view) and publishes how to ask (Shared::ngx_right_view).
// Only when DLSS is asked for: no other session is patched at all.
//
// Self-contained so the launcher can run the same checks over the file, and the test can run
// the logic against a fake.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace x4vr::ngx {

struct Site {
    uintptr_t va;
    uint8_t n;
    const uint8_t *bytes;
};

// What the calls reach: X4's own exports, so the injector can also ask by name.
inline constexpr uintptr_t kCreateFn = 0x2a8c5e0, kEvaluateFn = 0x2a8ce20, kReleaseFn = 0x2a8cb90;

// 0x20e35aa: the command buffer, the parameters, no progress callback, the feature, the call.
inline constexpr uint8_t kEvaluateBytes[] = {0x48, 0x8b, 0x7c, 0x24, 0x38, 0x48, 0x89, 0xda, 0x31,
                                             0xc9, 0x48, 0x89, 0xee, 0xe8, 0x64, 0x98, 0x9a, 0x00};
inline constexpr Site kEvaluate = {0x20e35aa, sizeof kEvaluateBytes, kEvaluateBytes};
inline constexpr uintptr_t kEvaluateCall = kEvaluate.va + 13;

// 0x20e3773: the command buffer, the parameters, where the handle goes, feature 1, the call.
inline constexpr uint8_t kCreateBytes[] = {0x48, 0x8b, 0x7c, 0x24, 0x10, 0x48, 0x89, 0xea, 0x49,
                                           0x8d, 0x8c, 0x24, 0xb0, 0x00, 0x00, 0x00, 0xbe, 0x01,
                                           0x00, 0x00, 0x00, 0xe8, 0x53, 0x8e, 0x9a, 0x00};
inline constexpr Site kCreate = {0x20e3773, sizeof kCreateBytes, kCreateBytes};
inline constexpr uintptr_t kCreateCall = kCreate.va + 21;

// 0x20e2899: the handle, not null, the call, keeping the result.
inline constexpr uint8_t kReleaseBytes[] = {0x48, 0x8b, 0xbf, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xff,
                                            0x74, 0x13, 0xe8, 0xe6, 0xa2, 0x9a, 0x00, 0x89, 0xc5};
inline constexpr Site kRelease = {0x20e2899, sizeof kReleaseBytes, kReleaseBytes};
inline constexpr uintptr_t kReleaseCall = kRelease.va + 12;

template <class Reader> bool verify_sites(Reader read, uintptr_t *bad) {
    for (const Site *s : {&kEvaluate, &kCreate, &kRelease}) {
        uint8_t b[64];
        if (s->n > sizeof b || !read(s->va, b, s->n)) {
            *bad = s->va;
            return false;
        }
        for (uint8_t i = 0; i < s->n; i++)
            if (b[i] != s->bytes[i]) {
                *bad = s->va + i;
                return false;
            }
    }
    return true;
}

// NVSDK_NGX_Resource_VK for an image (nvsdk_ngx_defs_vk.h), as X4 fills it at 0x20e2f40.
struct ResourceVk {
    uint64_t view;  // VkImageView
    uint64_t image; // VkImage
    uint32_t aspect, base_mip, mip_count, base_layer, layer_count;
    uint32_t format, width, height;
    uint32_t type; // 0: an image
    bool read_write;
};
static_assert(sizeof(ResourceVk) == 56, "the SDK's layout: X4 writes type at +0x30");

inline bool failed(uint32_t result) { return (result & 0xfff00000u) == 0xbad00000u; }
inline constexpr int kSuperSampling = 1; // NVSDK_NGX_Feature_SuperSampling

// Names NVIDIA's helpers set, and X4 with them at 0x20e3179.
inline constexpr const char *kImages[4] = {"Color", "Output", "Depth", "MotionVectors"};

struct Api {
    uint32_t (*create)(void *cmd, int feature, void *params, void **out);
    uint32_t (*evaluate)(void *cmd, const void *feature, const void *params, void *progress);
    uint32_t (*release)(void *feature);
    uint32_t (*get_pointer)(void *params, const char *name, void **out);
    void (*set_pointer)(void *params, const char *name, void *value);
    // Layer 1 of the image this view names, or the same view if there's none.
    uint64_t (*right_view)(void *cmd, uint64_t view);
};

struct Stats {
    std::atomic<uint64_t> features{0}, right_refused{0}, left{0}, right{0}, no_feature{0},
        no_view{0}, right_failed{0};
    std::atomic<uint32_t> last_error{0};
};

class Stereo {
public:
    Api api{};
    Stats stats;

    uint32_t create(void *cmd, int feature, void *params, void **out) {
        const uint32_t r = api.create(cmd, feature, params, out);
        if (failed(r) || feature != kSuperSampling || !out || !*out)
            return r;
        void *right = nullptr;
        const uint32_t r2 = api.create(cmd, feature, params, &right);
        if (failed(r2) || !right) {
            stats.last_error = r2;
            stats.right_refused++;
            return r;
        }
        std::lock_guard<std::mutex> lock(mu_);
        for (Pair &p : pairs_)
            if (!p.left) {
                p = Pair{*out, right};
                stats.features++;
                return r;
            }
        // More than any X4 holds. The right eye's is given back rather than lost.
        api.release(right);
        stats.right_refused++;
        return r;
    }

    uint32_t evaluate(void *cmd, const void *feature, const void *params, void *progress) {
        const uint32_t r = api.evaluate(cmd, feature, params, progress);
        stats.left++;
        if (failed(r))
            return r;
        void *right = right_of(feature);
        if (!right) {
            stats.no_feature++;
            return r;
        }
        void *p = const_cast<void *>(params);
        void *theirs[4] = {};
        ResourceVk ours[4];
        for (int i = 0; i < 4; i++) {
            if (failed(api.get_pointer(p, kImages[i], &theirs[i])) || !theirs[i]) {
                stats.no_view++;
                return r;
            }
            ours[i] = *static_cast<const ResourceVk *>(theirs[i]);
            const uint64_t v = api.right_view ? api.right_view(cmd, ours[i].view) : ours[i].view;
            if (v == ours[i].view) {
                // An input X4 draws for both eyes can be shared. The output can't: that
                // would overwrite the left eye's picture.
                if (i == 1) {
                    stats.no_view++;
                    return r;
                }
                continue;
            }
            ours[i].view = v;
            ours[i].base_layer = 1;
            ours[i].layer_count = 1;
        }
        for (int i = 0; i < 4; i++)
            api.set_pointer(p, kImages[i], &ours[i]);
        const uint32_t r2 = api.evaluate(cmd, right, params, progress);
        for (int i = 0; i < 4; i++)
            api.set_pointer(p, kImages[i], theirs[i]);
        if (failed(r2)) {
            stats.last_error = r2;
            stats.right_failed++;
        } else {
            stats.right++;
        }
        return r;
    }

    uint32_t release(void *feature) {
        void *right = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (Pair &p : pairs_)
                if (p.left == feature) {
                    right = p.right;
                    p = Pair{};
                }
        }
        if (right)
            api.release(right);
        return api.release(feature);
    }

private:
    struct Pair {
        void *left = nullptr, *right = nullptr;
    };
    void *right_of(const void *feature) {
        std::lock_guard<std::mutex> lock(mu_);
        for (const Pair &p : pairs_)
            if (p.left == feature)
                return p.right;
        return nullptr;
    }
    std::mutex mu_;
    Pair pairs_[8];
};

} // namespace x4vr::ngx
