// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// How fast can the CPU read a dump back, and does the memory type decide it?
//
// Take 258 spent **2451 ms per frame** in the flush, of which the PNG encode
// -- measured separately on that take's own bytes -- is only 57-125 ms per
// image, four images per frame. Roughly 2 s per frame was unaccounted for.
// The suspicion: both readback buffers ask for
//
//     HOST_VISIBLE | HOST_COHERENT
//
// and take the FIRST memory type that matches, which on this GPU is
// memoryTypes[2] (0x0006) -- uncached, write-combined. memoryTypes[5] (0x000e)
// carries HOST_CACHED and lives in the same heap. CPU *reads* from
// write-combined memory are slow in a way that writes are not, and the layer
// reads 31.7 MB per frame.
//
// That was an inference from flag bits and arithmetic. This measures it, on
// this GPU, before a take is spent on the fix. It is the same discipline that
// made the cursor overlay work first try: drive the code on real hardware
// rather than reason about it.
//
// **What would make this test worthless:** both allocations landing on the
// same memory type, so the "comparison" compares a thing with itself and can
// only report ~1.0x. That has happened in this project before -- a guard that
// fetched two handles through one path and could only ever agree. So the type
// indices are printed, and a ratio is REFUSED unless they differ.
//
// Needs a GPU. Run it directly; it takes no arguments.
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../common/x4vr_dumpio.hpp"
#include "../common/x4vr_png.hpp"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

// One eye layer of a real dump: 1408x1408 BGRA, the extent every recent take
// uses. Four of these are read per frame (present x2 layers, ui x2 layers).
static const uint32_t W = 1408, H = 1408, CH = 4;
static const VkDeviceSize BYTES = (VkDeviceSize)W * H * CH;

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

static std::string flag_str(VkMemoryPropertyFlags f) {
    std::string s;
    auto add = [&](VkMemoryPropertyFlags b, const char *n) {
        if (f & b) { if (!s.empty()) s += "|"; s += n; }
    };
    add(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL");
    add(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE");
    add(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT");
    add(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED");
    return s.empty() ? "(none)" : s;
}

struct Dev {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mp{};
};

// The layer's rule, verbatim: first type whose flags include `want`.
static uint32_t first_match(const Dev &d, uint32_t bits,
                            VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d.mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (d.mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // NOT "X4": this test must not be treated as the target by our own layer
    // if it happens to be enabled in the environment. It is measuring the
    // driver, not the layer.
    app.pApplicationName = "x4vr_readback_speed";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> phys(n);
    CHECK(vkEnumeratePhysicalDevices(inst, &n, phys.data()));
    if (!n) { printf("FAIL no physical devices\n"); return 1; }

    Dev d;
    vkGetPhysicalDeviceMemoryProperties(phys[0], &d.mp);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys[0], &props);
    printf("device: %s\n", props.deviceName);
    printf("memory types:\n");
    for (uint32_t i = 0; i < d.mp.memoryTypeCount; i++)
        printf("  [%2u] heap %u  0x%04x  %s\n", i,
               d.mp.memoryTypes[i].heapIndex,
               d.mp.memoryTypes[i].propertyFlags,
               flag_str(d.mp.memoryTypes[i].propertyFlags).c_str());

    uint32_t qfam = 0, qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qn, qfs.data());
    for (uint32_t i = 0; i < qn; i++)
        if (qfs[i].queueFlags & VK_QUEUE_TRANSFER_BIT) { qfam = i; break; }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = qfam;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    CHECK(vkCreateDevice(phys[0], &dci, nullptr, &d.dev));
    VkQueue queue;
    vkGetDeviceQueue(d.dev, qfam, 0, &queue);

    auto mk = [&](VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                  bool exact_cached, VkBuffer *buf, VkDeviceMemory *mem,
                  void **ptr, uint32_t *chosen) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = BYTES;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(d.dev, &bci, nullptr, buf) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(d.dev, *buf, &req);
        uint32_t type = exact_cached
                            ? first_match(d, req.memoryTypeBits,
                                          want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                            : first_match(d, req.memoryTypeBits, want);
        if (type == UINT32_MAX)
            return false;
        *chosen = type;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (vkAllocateMemory(d.dev, &mai, nullptr, mem) != VK_SUCCESS ||
            vkBindBufferMemory(d.dev, *buf, *mem, 0) != VK_SUCCESS)
            return false;
        if (ptr && vkMapMemory(d.dev, *mem, 0, VK_WHOLE_SIZE, 0, ptr) !=
                       VK_SUCCESS)
            return false;
        return true;
    };

    const VkMemoryPropertyFlags HV_HC = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // Source: device-local, filled once. The copy the layer does is
    // image -> buffer; a buffer -> buffer copy reads from the same place and
    // is what makes this test need no image, no layout transition and no
    // render pass.
    VkBuffer src = VK_NULL_HANDLE, up = VK_NULL_HANDLE;
    VkDeviceMemory srcm = VK_NULL_HANDLE, upm = VK_NULL_HANDLE;
    void *upp = nullptr;
    uint32_t t_src = 0, t_up = 0;
    if (!mk(VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &src, &srcm, nullptr,
            &t_src)) {
        printf("FAIL could not allocate a DEVICE_LOCAL source\n");
        return 1;
    }
    if (!mk(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HV_HC, false, &up, &upm, &upp,
            &t_up)) {
        printf("FAIL could not allocate an upload buffer\n");
        return 1;
    }
    // Content that compresses like a frame rather than like a blank: a smooth
    // gradient with noise on top. Only the PNG timing depends on this; the
    // read timing does not depend on the bytes at all.
    {
        uint8_t *p = (uint8_t *)upp;
        uint32_t s = 12345u;
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                s = s * 1664525u + 1013904223u;
                const uint8_t nz = (uint8_t)((s >> 24) & 0x1f);
                uint8_t *q = p + ((size_t)y * W + x) * CH;
                q[0] = (uint8_t)((x >> 2) + nz);
                q[1] = (uint8_t)((y >> 2) + nz);
                q[2] = (uint8_t)(((x + y) >> 3) + nz);
                q[3] = 0xff;
            }
    }

    // The two readback buffers: the layer's current choice, and the same
    // request with HOST_CACHED added.
    VkBuffer wc = VK_NULL_HANDLE, cached = VK_NULL_HANDLE;
    VkDeviceMemory wcm = VK_NULL_HANDLE, cm = VK_NULL_HANDLE;
    void *wcp = nullptr, *cp = nullptr;
    uint32_t t_wc = UINT32_MAX, t_cached = UINT32_MAX;
    if (!mk(VK_BUFFER_USAGE_TRANSFER_DST_BIT, HV_HC, false, &wc, &wcm, &wcp,
            &t_wc)) {
        printf("FAIL could not allocate the layer's current readback buffer\n");
        return 1;
    }
    const bool have_cached =
        mk(VK_BUFFER_USAGE_TRANSFER_DST_BIT, HV_HC, true, &cached, &cm, &cp,
           &t_cached);

    printf("\nlayer's current rule (first HOST_VISIBLE|HOST_COHERENT)"
           " -> memoryTypes[%u]  %s\n",
           t_wc, flag_str(d.mp.memoryTypes[t_wc].propertyFlags).c_str());
    if (!have_cached) {
        printf("this GPU offers no HOST_VISIBLE|HOST_COHERENT|HOST_CACHED "
               "type. The fix does not apply here and this test says nothing "
               "about it — that is a real answer, not a failure.\n");
        return 0;
    }
    printf("with HOST_CACHED added                                 "
           " -> memoryTypes[%u]  %s\n",
           t_cached, flag_str(d.mp.memoryTypes[t_cached].propertyFlags).c_str());

    // THE GUARD. Two names for one allocation would compare a thing with
    // itself and could only ever report ~1.0x.
    if (t_wc == t_cached) {
        printf("\nREFUSED: both requests chose memoryTypes[%u], so there is "
               "nothing to compare. The layer is ALREADY getting cached "
               "memory here and the 2 s/frame is something else.\n", t_wc);
        return 0;
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = qfam;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(d.dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(d.dev, &cbi, &cb));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(cb, &bi));
    VkBufferCopy rg{};
    rg.size = BYTES;
    vkCmdCopyBuffer(cb, up, src, 1, &rg);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    vkCmdCopyBuffer(cb, src, wc, 1, &rg);
    vkCmdCopyBuffer(cb, src, cached, 1, &rg);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    // ---- the measurement -------------------------------------------------
    // Sequential read of the whole buffer, which is what write_png_n does
    // while it builds its filtered rows. Summed and printed so no compiler
    // can drop the loads, and so the two reads can be compared: they came
    // from one source and MUST agree. A ratio between two different pictures
    // would not be a memory measurement.
    auto read_sum = [](const void *p, VkDeviceSize bytes, double *out_ms) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t sum = 0;
        const uint8_t *q = (const uint8_t *)p;
        for (VkDeviceSize i = 0; i < bytes; i += 8) {
            uint64_t v;
            memcpy(&v, q + i, sizeof v);
            sum += v;
        }
        *out_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        return sum;
    };

    // Bulk copy, which is how write_png_n actually touches the buffer: it
    // memcpy's whole rows into its filtered-row vector. **This is the number
    // that matters and the scalar loop below is not.** A scalar 8-byte read
    // from write-combined memory is a separate uncached round trip per read
    // and is pathological in a way no real code path is; reporting its ratio
    // as "the cost" would be an instrument answering a question the layer
    // never asks.
    std::vector<uint8_t> sink(BYTES);
    auto read_bulk = [&](const void *p, double *out_ms) {
        const auto t0 = std::chrono::steady_clock::now();
        memcpy(sink.data(), p, (size_t)BYTES);
        *out_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
    };
    const double megs = (double)BYTES / (1024.0 * 1024.0);
    printf("\nBULK memcpy of one %ux%u layer (%.1f MB) — the pattern "
           "write_png_n uses, 3 passes:\n", W, H, megs);
    double bulk_wc = 1e18, bulk_c = 1e18;
    for (int r = 0; r < 3; r++) {
        double a = 0, b = 0;
        read_bulk(wcp, &a);
        read_bulk(cp, &b);
        printf("  pass %d  current(type %u) %8.2f ms (%7.1f MB/s)   "
               "cached(type %u) %8.2f ms (%7.1f MB/s)\n",
               r, t_wc, a, megs / (a / 1000.0), t_cached, b,
               megs / (b / 1000.0));
        if (a < bulk_wc) bulk_wc = a;
        if (b < bulk_c) bulk_c = b;
    }

    printf("\nSCALAR 8-byte read of the same layer, 3 passes — a pathological "
           "pattern, kept only to show it is not the one that matters:\n");
    double best_wc = 1e18, best_c = 1e18;
    uint64_t sum_wc = 0, sum_c = 0;
    for (int r = 0; r < 3; r++) {
        double a = 0, b = 0;
        sum_wc = read_sum(wcp, BYTES, &a);
        sum_c = read_sum(cp, BYTES, &b);
        printf("  pass %d  current(type %u) %8.1f ms (%6.1f MB/s)   "
               "cached(type %u) %8.1f ms (%6.1f MB/s)\n",
               r, t_wc, a, megs / (a / 1000.0), t_cached, b, megs / (b / 1000.0));
        if (a < best_wc) best_wc = a;
        if (b < best_c) best_c = b;
    }
    if (sum_wc != sum_c) {
        printf("\nFAIL the two buffers hold different bytes (%llu vs %llu) — "
               "they were copied from one source, so this is a bug in the "
               "test and every number above is void\n",
               (unsigned long long)sum_wc, (unsigned long long)sum_c);
        return 1;
    }
    printf("  checksums agree (%llu), so both reads saw the same picture\n",
           (unsigned long long)sum_wc);

    // ---- and the thing the layer actually does ---------------------------
    // ---- the loop the flush ACTUALLY runs, verbatim ----------------------
    // layer/x4vr_layer.cpp's burst flush counts UI coverage per pixel, with
    // four separate byte loads per pixel, straight over the mapped readback
    // buffer -- and it does this AFTER write_ui8 has already read the same
    // bytes. On write-combined memory every one of those loads is an uncached
    // round trip that no streaming-load path can coalesce. Copied here rather
    // than approximated, because the point is what that code costs, not what
    // something like it costs.
    auto coverage = [&](const void *p, double *out_ms) {
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t nz = 0, na = 0;
        const uint8_t *src = (const uint8_t *)p;
        for (size_t i = 0; i < (size_t)W * H; i++) {
            const uint8_t *q = src + i * 4;
            if (q[0] | q[1] | q[2])
                nz++;
            if (q[3])
                na++;
        }
        *out_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
        return nz + na;
    };
    printf("\nthe flush's per-pixel COVERAGE loop, copied verbatim, 3 passes:\n");
    double cov_wc = 1e18, cov_c = 1e18;
    uint64_t cv_a = 0, cv_b = 0;
    for (int r = 0; r < 3; r++) {
        double a = 0, b = 0;
        cv_a = coverage(wcp, &a);
        cv_b = coverage(cp, &b);
        printf("  pass %d  current(type %u) %8.1f ms   cached(type %u) "
               "%8.1f ms\n", r, t_wc, a, t_cached, b);
        if (a < cov_wc) cov_wc = a;
        if (b < cov_c) cov_c = b;
    }
    if (cv_a != cv_b) {
        printf("FAIL coverage differs between buffers (%llu vs %llu)\n",
               (unsigned long long)cv_a, (unsigned long long)cv_b);
        return 1;
    }

    // ---- the actual change: scalar-from-WC vs snapshot-then-work ---------
    // The two hot terms of take 259's flush, A/B'd on write-combined memory.
    // OLD is write_ppm8's loop verbatim: three scalar byte loads per pixel,
    // with the channel-order test inside the inner loop. NEW is one bulk
    // memcpy followed by the same work from RAM.
    {
        std::vector<uint8_t> rgb((size_t)W * H * 3), snap;
        auto old_way = [&](const uint8_t *px, bool bgra, double *ms) {
            const auto t0 = std::chrono::steady_clock::now();
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++) {
                    const uint8_t *p = px + ((size_t)y * W + x) * 4;
                    for (uint32_t c = 0; c < 3; c++)
                        rgb[((size_t)y * W + x) * 3 + c] =
                            bgra ? p[2 - c] : p[c];
                }
            *ms = ms_since(t0);
        };
        auto new_way = [&](const uint8_t *px, bool bgra, double *ms) {
            const auto t0 = std::chrono::steady_clock::now();
            x4vr::dump::snapshot(snap, px, (size_t)W * H * 4);
            x4vr::dump::to_rgb(rgb, snap.data(), W, H, bgra);
            *ms = ms_since(t0);
        };
        double o = 0, nw = 0, o2 = 0, n2 = 0;
        old_way((const uint8_t *)wcp, true, &o);
        new_way((const uint8_t *)wcp, true, &nw);
        old_way((const uint8_t *)wcp, true, &o2);
        new_way((const uint8_t *)wcp, true, &n2);
        const double ob = o < o2 ? o : o2, nb = nw < n2 ? nw : n2;
        printf("\nthe eye write's pixel loop, on WRITE-COMBINED memory:\n"
               "  OLD scalar-from-WC  %7.1f ms      NEW snapshot+RAM  %7.1f ms"
               "   (%.1fx)\n", ob, nb, ob / nb);
        printf("  take 259 measured 549 ms per layer for this term.\n");
    }

    const char *out = getenv("X4VR_TEST_TMP") ? getenv("X4VR_TEST_TMP")
                                              : "/tmp/x4vr-readback-test";
    char pa[512], pb[512];
    snprintf(pa, sizeof pa, "%s-wc.png", out);
    snprintf(pb, sizeof pb, "%s-cached.png", out);
    double png_wc = 0, png_c = 0;
    {
        auto t0 = std::chrono::steady_clock::now();
        x4vr::png::write_png_n(pa, (const uint8_t *)wcp, W, H, CH);
        png_wc = ms_since(t0);
        t0 = std::chrono::steady_clock::now();
        x4vr::png::write_png_n(pb, (const uint8_t *)cp, W, H, CH);
        png_c = ms_since(t0);
    }
    printf("\nwrite_png_n on the same bytes (read + filter + deflate + write):\n"
           "  current(type %u) %8.1f ms      cached(type %u) %8.1f ms\n",
           t_wc, png_wc, t_cached, png_c);
    remove(pa);
    remove(pb);

    // **The verdict is keyed on write_png_n, not on either read loop.** The
    // scalar ratio can be enormous while the thing the layer spends barely
    // moves, and a ratio computed from a quantity the code never exercises is
    // exactly the mistake that has cost this project takes before.
    const double ratio = bulk_wc / bulk_c;
    const double scalar_ratio = best_wc / best_c;
    const double png_ratio = png_wc / png_c;
    const double per_frame_now = 4 * png_wc, per_frame_fix = 4 * png_c;
    printf("\n  bulk read speedup    %.2fx   (scalar loop said %.1fx — "
           "ignore it)\n", ratio, scalar_ratio);
    printf("  write_png_n speedup  %.2fx   <- THE ONE THAT DECIDES\n",
           png_ratio);
    printf("  a frame is 4 of these (present x2 layers, ui x2 layers):\n"
           "    %.0f ms now  ->  %.0f ms cached\n",
           per_frame_now, per_frame_fix);
    printf("    a 6-frame burst: %.1f s  ->  %.1f s\n",
           6 * per_frame_now / 1000.0, 6 * per_frame_fix / 1000.0);
    printf("  take 258 measured 2451 ms per frame in the flush and 12.3 s per "
           "burst.\n");
    // Per frame the flush does, for the UI: 2 x write_ui8 + 2 x coverage.
    // The present dump adds 2 x write_ppm8 and counts nothing.
    const double flush_now = 2 * png_wc + 2 * cov_wc + 2 * png_wc;
    const double flush_fix = 2 * png_c + 2 * cov_c + 2 * png_c;
    printf("\n  COVERAGE loop alone: %.0f ms on the current type, %.1f ms "
           "cached  (%.0fx)\n", cov_wc, cov_c, cov_wc / cov_c);
    printf("  modelled flush per frame (2 ui writes + 2 coverage + 2 present "
           "writes):\n"
           "    %.0f ms now  ->  %.0f ms cached      [take 258 measured "
           "2451 ms]\n", flush_now, flush_fix);
    const double saved_per_frame = per_frame_now - per_frame_fix;
    if (png_ratio < 1.5)
        printf("\n  VERDICT: the memory type is NOT what take 258 spent its "
               "time on.\n"
               "  Changing it saves %.0f ms of the 2451 ms per frame (%.1f%%). "
               "The rest is\n  somewhere else and has not been found — do not "
               "change the layer on this.\n",
               saved_per_frame, 100.0 * saved_per_frame / 2451.0);
    else
        printf("\n  VERDICT: the memory type IS the cost — write_png_n itself "
               "is %.2fx faster,\n  saving %.0f ms of the 2451 ms per frame "
               "(%.1f%%).\n",
               png_ratio, saved_per_frame, 100.0 * saved_per_frame / 2451.0);
    return 0;
}
