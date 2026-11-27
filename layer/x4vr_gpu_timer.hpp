// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A stopwatch on the GPU for one kind of dispatch (after take 490: every phase ran at the
// headset's 90 fps cap, so frame times could not say what the march costs). Two
// timestamps around a dispatch in the command buffer that records it; read back later on
// the host, without waiting.
//
// A query slot is read only when the frame that recorded it is kMinAge presents old, and
// slots are reused round a ring of kSlots: a slot reset and written again in a command
// buffer still answers with its OLD values until that buffer runs, and reading it in that
// window would report a stale time as a new one. A slot whose times never arrive (a buffer
// recorded and never submitted) is given up after kMaxAge presents. Every query is reset
// once (reset_all, in a buffer that runs before any bracket's), so none is ever read
// uninitialised; and a reading whose first timestamp is the slot's previous one is that
// previous use's, stale, and dropped -- the GPU's clock only moves on.
//
// The timestamps sit where Vulkan's rules make them bracket the dispatch: both at the
// bottom of the pipe, so the first is written when everything recorded before it has
// finished and the second when the dispatch has.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

namespace x4vr {

struct GpuTimerFns {
    PFN_vkCreateQueryPool CreateQueryPool = nullptr;
    PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp CmdWriteTimestamp = nullptr;
    PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;
    bool complete() const {
        return CreateQueryPool && CmdResetQueryPool && CmdWriteTimestamp && GetQueryPoolResults;
    }
};

class GpuTimer {
public:
    static constexpr uint32_t kSlots = 1024, kKinds = 6;
    // A time over kSlowMs is kept with its frame (after take 495: a rare 4 ms dispatch).
    static constexpr float kSlowMs = 2.f;
    struct Slow {
        uint32_t kind;
        float ms;
        uint64_t frame; // the present count when its bracket was recorded
    };
    static constexpr uint64_t kMinAge = 4, kMaxAge = 600;

    // False, with `why`, and the timer off, when the queue family keeps no timestamps or
    // the pool cannot be made.
    bool create(VkDevice device, const GpuTimerFns &fns, float period_ns, uint32_t valid_bits,
                const char **why) {
        if (!fns.complete())
            return *why = "not every entry point it needs was found", false;
        if (valid_bits == 0)
            return *why = "X4's queue family keeps no timestamps", false;
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kSlots;
        if (fns.CreateQueryPool(device, &qi, nullptr, &pool_) != VK_SUCCESS)
            return *why = "the driver refused the query pool", false;
        device_ = device;
        fns_ = fns;
        period_ms_ = period_ns * 1e-6;
        mask_ = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
        for (auto &s : state_)
            s.store(0, std::memory_order_relaxed);
        on_ = true;
        return true;
    }
    bool on() const { return on_; }
    // Every query reset, once, in a buffer that runs before any bracket's.
    void reset_all(VkCommandBuffer cb) const {
        if (on_)
            fns_.CmdResetQueryPool(cb, pool_, 0, 2 * kSlots);
    }

    // Before the dispatch: the slot it took, or -1 (the timer off, or the ring full).
    int begin(VkCommandBuffer cb, uint32_t kind) {
        if (!on_ || kind >= kKinds)
            return -1;
        const uint32_t slot = next_.fetch_add(1, std::memory_order_relaxed) % kSlots;
        uint8_t free = 0;
        if (!state_[slot].compare_exchange_strong(free, 1, std::memory_order_acq_rel)) {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        kind_[slot] = (uint8_t)kind;
        frame_[slot] = frame_now_.load(std::memory_order_relaxed);
        fns_.CmdResetQueryPool(cb, pool_, 2 * slot, 2);
        fns_.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, 2 * slot);
        return (int)slot;
    }
    // After it.
    void end(VkCommandBuffer cb, int slot) {
        if (slot < 0)
            return;
        fns_.CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, 2 * slot + 1);
        state_[slot].store(2, std::memory_order_release);
    }

    // Once a present: the frame count moves on, and every slot old enough is read.
    void present() {
        if (!on_)
            return;
        const uint64_t now = frame_now_.fetch_add(1, std::memory_order_relaxed) + 1;
        for (uint32_t s = 0; s < kSlots; s++) {
            if (state_[s].load(std::memory_order_acquire) != 2)
                continue;
            const uint64_t age = now - frame_[s];
            if (age < kMinAge)
                continue;
            uint64_t r[4] = {};
            const VkResult vr = fns_.GetQueryPoolResults(
                device_, pool_, 2 * s, 2, sizeof r, r, 2 * sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            const bool fresh = (r[0] & mask_) != last_[s];
            if ((vr == VK_SUCCESS || vr == VK_NOT_READY) && r[1] && r[3] && fresh) {
                last_[s] = r[0] & mask_;
                const uint64_t dt = ((r[2] & mask_) - (r[0] & mask_)) & mask_;
                std::lock_guard<std::mutex> lock(mu_);
                std::vector<float> &v = ms_[kind_[s]];
                const float ms = (float)(dt * period_ms_);
                if (v.size() < 200000)
                    v.push_back(ms);
                if (ms > kSlowMs && slow_.size() < 1000)
                    slow_.push_back({kind_[s], ms, frame_[s]});
                state_[s].store(0, std::memory_order_release);
            } else if (age > kMaxAge) {
                (r[1] && r[3] && !fresh ? stale_ : lost_).fetch_add(1, std::memory_order_relaxed);
                state_[s].store(0, std::memory_order_release);
            }
        }
    }

    // A kind's times so far, sorted.
    std::vector<float> sorted(uint32_t kind) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<float> v = kind < kKinds ? ms_[kind] : std::vector<float>();
        std::sort(v.begin(), v.end());
        return v;
    }
    // The slow ones read since the last call, oldest first.
    std::vector<Slow> drain_slow() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Slow> out;
        out.swap(slow_);
        return out;
    }
    uint64_t now() const { return frame_now_.load(std::memory_order_relaxed); }
    uint64_t skipped() const { return skipped_.load(); }
    uint64_t lost() const { return lost_.load(); }
    uint64_t stale() const { return stale_.load(); }

private:
    bool on_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    GpuTimerFns fns_;
    double period_ms_ = 0.0;
    uint64_t mask_ = ~0ull;
    std::atomic<uint8_t> state_[kSlots]; // 0 free, 1 begun, 2 ended (in a buffer)
    uint8_t kind_[kSlots] = {};
    uint64_t frame_[kSlots] = {};
    uint64_t last_[kSlots] = {}; // each slot's last first timestamp read
    std::atomic<uint32_t> next_{0};
    std::atomic<uint64_t> frame_now_{0}, skipped_{0}, lost_{0}, stale_{0};
    mutable std::mutex mu_; // ms_: filled at presents, read by the reports
    std::vector<float> ms_[kKinds];
    std::vector<Slow> slow_;
};

} // namespace x4vr
