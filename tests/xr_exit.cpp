// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4 calling exit() with the VR session running and its device never destroyed.
//
// Take 497L's first attempt (2026-11-28): gamescope had died, and libX11's handler for the lost
// connection called exit() from inside X4's SDL_DestroyWindow, before X4 had destroyed its
// Vulkan device, so the layer's vr_shutdown never ran. exit() ran the OpenXR loader's exit
// handler, which closed the runtime's library (SteamVR's), and the layer's XR thread, never
// told to stop, called xrEndFrame into the unmapped code: SIGSEGV. X4 was still inside
// OPENSSL_cleanup, an exit handler registered before the loader's, when it happened.
//
// Here: the layer in a program named X4 with X4VR_VR=1, a session on whichever runtime
// XR_RUNTIME_JSON names (Monado's null compositor in tests/run-xr-exit.sh), twenty frames
// ended, then exit(3) from main with the device alive. `slow_teardown` is registered before
// anything else, so it runs last, as OPENSSL_cleanup did, and keeps the process alive past the
// XR thread's next frames. The process must end with main's status, 3; without a session it
// prints SKIP and exits 0, having tested nothing. `quit` instead quits as X4 normally does,
// destroying the device and the instance before returning 0: the layer's vr_shutdown ends the
// session, and its stop at exit then finds nothing to do.
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void slow_teardown() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printf("test: the last exit handler returned\n");
    fflush(stdout);
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

} // namespace

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv) {
    const bool quit = argc > 1 && !strcmp(argv[1], "quit");
    atexit(slow_teardown);
    const char *log = getenv("X4VR_LOG");
    if (!log || !*log) {
        printf("FAIL X4VR_LOG must name the layer's log: the test reads it\n");
        return 1;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X4";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, nullptr, &inst));

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, nullptr));
    std::vector<VkPhysicalDevice> pds(n);
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pds.data()));
    if (!n) {
        printf("FAIL no physical devices\n");
        return 1;
    }
    // The discrete card if there is one: the runtime's compositor takes it too.
    VkPhysicalDevice phys = pds[0];
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = p;
            break;
        }
    }
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
    uint32_t fam = 0;
    while (fam < qn && !(qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        fam++;
    const float prio = 1.f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = fam;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    VkDevice dev;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, fam, 0, &queue); // the layer starts the session here

    // Twenty frames ended, read from the layer's own trace; 20 s at most.
    const char *ended = "trace: XR thread, frame 20: xrEndFrame returned";
    std::string text;
    for (int i = 0; i < 200; i++) {
        text = slurp(log);
        if (text.find(ended) != std::string::npos || text.find("NO SESSION") != std::string::npos)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (text.find(ended) == std::string::npos) {
        const size_t at = text.find("NO SESSION");
        printf("SKIP no session ran twenty frames%s%s\n", at == std::string::npos ? "" : ": ",
               at == std::string::npos ? "" : text.substr(at, text.find('\n', at) - at).c_str());
        fflush(stdout);
        _exit(0);
    }
    if (quit) {
        printf("test: twenty frames ended; the device and the instance destroyed, then return\n");
        fflush(stdout);
        vkDestroyDevice(dev, nullptr);
        vkDestroyInstance(inst, nullptr);
        return 0;
    }
    printf("test: twenty frames ended; exit(3) with the device alive, as X4 did\n");
    fflush(stdout);
    exit(3);
}
