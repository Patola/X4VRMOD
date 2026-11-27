// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// layer/x4vr_gpu_timer.hpp on a GPU (after take 490: the march's in-game stopwatch). The
// timer brackets a job in a command buffer and reads its two timestamps kMinAge presents
// later, never sooner: a reused slot answers with its old values until its new buffer has
// run. Checked here:
//   - nothing is read before kMinAge presents, and exactly the one job after;
//   - a real job (filling 256 MB) times above zero, and an empty bracket below it;
//   - each kind keeps its own times;
//   - a bracket recorded and never submitted is given up after kMaxAge presents, and no
//     time is made up for it (the negative control).
#include "../layer/x4vr_gpu_timer.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CK(x)                                                                              \
    do {                                                                                   \
        VkResult r_ = (x);                                                                 \
        if (r_ != VK_SUCCESS) {                                                            \
            printf("FAIL %s -> %d (line %d)\n", #x, r_, __LINE__);                         \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += ok ? 0 : 1;
}

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (n == 0) {
        printf("FAIL no Vulkan device\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    VkPhysicalDevice phys = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("DEVICE=%s\n", props.deviceName);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    uint32_t fam = 0;
    while (fam < qn && !(qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        fam++;
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = fam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    CK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, fam, 0, &q);

    x4vr::GpuTimerFns fns;
    fns.CreateQueryPool = vkCreateQueryPool;
    fns.CmdResetQueryPool = vkCmdResetQueryPool;
    fns.CmdWriteTimestamp = vkCmdWriteTimestamp;
    fns.GetQueryPoolResults = vkGetQueryPoolResults;
    static x4vr::GpuTimer timer;
    const char *why = "";
    const bool on = timer.create(dev, fns, props.limits.timestampPeriod,
                                 qf[fam].timestampValidBits, &why);
    check(on, "the timer is on");
    if (!on) {
        printf("       why: %s\n", why);
        return 1;
    }

    // A buffer to fill: a job long enough to time.
    const VkDeviceSize size = 256ull << 20;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    CK(vkCreateBuffer(dev, &bci, nullptr, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mt = 0;
    while (mt < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << mt)))
        mt++;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mt;
    VkDeviceMemory mem;
    CK(vkAllocateMemory(dev, &mai, nullptr, &mem));
    CK(vkBindBufferMemory(dev, buf, mem, 0));

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = fam;
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.commandBufferCount = 2;
    VkCommandBuffer cb[2];
    CK(vkAllocateCommandBuffers(dev, &cai, cb));
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    // Every query reset first, as the march's one-time setup does: none is read uninitialised.
    {
        VkCommandBuffer rc;
        cai.commandBufferCount = 1;
        CK(vkAllocateCommandBuffers(dev, &cai, &rc));
        CK(vkBeginCommandBuffer(rc, &bi));
        timer.reset_all(rc);
        CK(vkEndCommandBuffer(rc));
        VkSubmitInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        rs.commandBufferCount = 1;
        rs.pCommandBuffers = &rc;
        CK(vkQueueSubmit(q, 1, &rs, VK_NULL_HANDLE));
        CK(vkQueueWaitIdle(q));
    }

    // Buffer 0, submitted: the fill timed as kind 1, an empty bracket as kind 2.
    CK(vkBeginCommandBuffer(cb[0], &bi));
    const int s1 = timer.begin(cb[0], 1);
    vkCmdFillBuffer(cb[0], buf, 0, size, 0x3f800000u);
    timer.end(cb[0], s1);
    const int s2 = timer.begin(cb[0], 2);
    timer.end(cb[0], s2);
    CK(vkEndCommandBuffer(cb[0]));
    // Buffer 1, never submitted: kind 0.
    CK(vkBeginCommandBuffer(cb[1], &bi));
    const int s0 = timer.begin(cb[1], 0);
    timer.end(cb[1], s0);
    CK(vkEndCommandBuffer(cb[1]));
    check(s1 >= 0 && s2 >= 0 && s0 >= 0, "three brackets took slots");

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb[0];
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));

    for (uint64_t i = 0; i + 1 < x4vr::GpuTimer::kMinAge; i++)
        timer.present();
    check(timer.sorted(1).empty() && timer.sorted(2).empty(),
          "nothing read before kMinAge presents, though the job had run");
    timer.present();
    const auto fill = timer.sorted(1), empty = timer.sorted(2);
    check(fill.size() == 1 && empty.size() == 1, "after kMinAge presents, each job read once");
    if (fill.size() == 1 && empty.size() == 1) {
        printf("       fill of 256 MB: %.4f ms; empty bracket: %.4f ms\n", fill[0], empty[0]);
        check(fill[0] > 0.f && fill[0] < 1000.f, "the fill's time is above zero and finite");
        check(empty[0] < fill[0], "the empty bracket's time is below the fill's");
        // The slow ones (after take 495): every time over kSlowMs kept, with its frame, and no
        // other -- the fill is one on lavapipe (about 66 ms), not on a fast card.
        const auto slow = timer.drain_slow();
        const size_t over = (fill[0] > x4vr::GpuTimer::kSlowMs ? 1 : 0) +
                            (empty[0] > x4vr::GpuTimer::kSlowMs ? 1 : 0);
        bool kinds_ok = true;
        for (const auto &sl : slow)
            kinds_ok = kinds_ok && sl.kind == 1 && sl.ms == fill[0] && sl.frame == 0;
        printf("       slow (over %.0f ms): %zu\n", (double)x4vr::GpuTimer::kSlowMs, slow.size());
        check(slow.size() == over && kinds_ok, "the slow ones kept: exactly those over kSlowMs");
        check(timer.drain_slow().empty(), "drained once");
    }
    for (int i = 0; i < 4; i++)
        timer.present();
    check(timer.sorted(1).size() == 1, "a job is read once, not again at later presents");
    check(timer.sorted(0).empty() && timer.lost() == 0,
          "the unsubmitted bracket: no time, and not yet given up");
    for (uint64_t i = 0; i < x4vr::GpuTimer::kMaxAge; i++)
        timer.present();
    check(timer.sorted(0).empty() && timer.lost() == 1,
          "the unsubmitted bracket given up after kMaxAge presents, no time made up");

    // Stale: buffer 0 run again, unchanged -- its brackets' slots are ended and read already,
    // so the timer has freed them; a third bracket taking s1's slot again in a buffer never
    // submitted must not report the fill's old time as its own.
    {
        VkCommandBuffer xc;
        cai.commandBufferCount = 1;
        CK(vkAllocateCommandBuffers(dev, &cai, &xc));
        CK(vkBeginCommandBuffer(xc, &bi));
        int again = -1;
        for (uint32_t i = 0; i < x4vr::GpuTimer::kSlots && again != s1; i++) {
            again = timer.begin(xc, 1);
            timer.end(xc, again);
        }
        CK(vkEndCommandBuffer(xc));
        check(again == s1, "the fill's slot taken again, in a buffer never submitted");
        for (uint64_t i = 0; i <= x4vr::GpuTimer::kMaxAge; i++)
            timer.present();
        check(timer.sorted(1).size() == 1 && timer.stale() >= 1,
              "its old values not taken for new ones: counted stale, no time added");
    }

    vkDeviceWaitIdle(dev);
    printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
