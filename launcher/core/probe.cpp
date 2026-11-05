// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The probes: is a headset answering, and can this GPU run the mod. Each runs
// `here` inside `x4vrmod probe ...`, and callers reach it `child`, through a
// process with a timeout: a runtime or driver that hangs or crashes must not
// take the window with it. Results cross as KEY=VALUE lines with a prefix of our
// own, because a runtime loaded into the probe may print to stdout too.
#include "x4vr_launcher.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <limits.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if X4VR_HAVE_OPENXR
#include "../../common/x4vr_xr.hpp"
#endif

namespace x4vr::launcher {

std::string self_dir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? dir_name(std::string(buf, (size_t)n)) : ".";
}

// The x4vrmod next to this program: the window ships beside it.
std::string cli_path() {
    const std::string s = self_dir() + "/x4vrmod";
    if (is_file(s))
        return s;
    const std::string p = find_on_path("x4vrmod");
    return p.empty() ? s : p;
}

static std::string one_line(std::string s) {
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

// The value of `key=` at the start of a line, every occurrence in order.
static std::vector<std::string> values_of(const std::string &text, const std::string &key) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    const std::string k = key + "=";
    while (std::getline(ss, line))
        if (line.rfind(k, 0) == 0)
            out.push_back(line.substr(k.size()));
    return out;
}

static std::string value_of(const std::string &text, const std::string &key) {
    const auto v = values_of(text, key);
    return v.empty() ? "" : v.front();
}

// ------------------------------------------------------------------- OpenXR
const char *xr_status_name(XrStatus s) {
    switch (s) {
    case XrStatus::Ready: return "ready";
    case XrStatus::NoLoader: return "no-loader";
    case XrStatus::NoRuntime: return "no-runtime";
    case XrStatus::NoHeadset: return "no-headset";
    case XrStatus::Failed: return "failed";
    case XrStatus::TimedOut: return "timed-out";
    case XrStatus::NotRun: return "not-run";
    }
    return "failed";
}

#if X4VR_HAVE_OPENXR
static void xr_say(void *, const char *line) { fprintf(stderr, "%s\n", line); }

XrProbe xr_probe_here(bool verbose) {
    XrProbe p;
    xr::Runtime rt;
    const bool ok = xr::runtime_open(rt, verbose ? xr_say : nullptr, nullptr);
    p.runtime = rt.runtime_name;
    p.system = rt.system_name;
    p.detail = rt.last_error;
    const std::string &e = rt.last_error;
    if (ok && rt.view_count > 0) {
        p.status = XrStatus::Ready;
        p.eye_w = rt.views[0].recommendedImageRectWidth;
        p.eye_h = rt.views[0].recommendedImageRectHeight;
    } else if (e.rfind("dlopen", 0) == 0 || e.rfind("loader would not", 0) == 0) {
        p.status = XrStatus::NoLoader;
    } else if (e.rfind("no active OpenXR runtime", 0) == 0 ||
               e.find("XR_ERROR_RUNTIME_UNAVAILABLE") != std::string::npos) {
        p.status = XrStatus::NoRuntime;
    } else if (e.find("XR_ERROR_FORM_FACTOR_UNAVAILABLE") != std::string::npos) {
        p.status = XrStatus::NoHeadset;
    } else {
        p.status = XrStatus::Failed;
    }
    xr::runtime_close(rt);
    return p;
}
#else
XrProbe xr_probe_here(bool) {
    XrProbe p;
    p.status = XrStatus::Failed;
    p.detail = "built without the OpenXR headers";
    return p;
}
#endif

std::string xr_probe_format(const XrProbe &p) {
    std::string s;
    s += std::string("X4VRMOD_XR_STATUS=") + xr_status_name(p.status) + "\n";
    s += "X4VRMOD_XR_RUNTIME=" + one_line(p.runtime) + "\n";
    s += "X4VRMOD_XR_SYSTEM=" + one_line(p.system) + "\n";
    s += "X4VRMOD_XR_EYE=" + std::to_string(p.eye_w) + "x" + std::to_string(p.eye_h) + "\n";
    s += "X4VRMOD_XR_DETAIL=" + one_line(p.detail) + "\n";
    return s;
}

XrProbe xr_probe_parse(const std::string &text) {
    XrProbe p;
    const std::string st = value_of(text, "X4VRMOD_XR_STATUS");
    p.status = XrStatus::Failed;
    for (XrStatus s : {XrStatus::Ready, XrStatus::NoLoader, XrStatus::NoRuntime,
                       XrStatus::NoHeadset, XrStatus::TimedOut, XrStatus::NotRun})
        if (st == xr_status_name(s))
            p.status = s;
    p.runtime = value_of(text, "X4VRMOD_XR_RUNTIME");
    p.system = value_of(text, "X4VRMOD_XR_SYSTEM");
    p.detail = value_of(text, "X4VRMOD_XR_DETAIL");
    sscanf(value_of(text, "X4VRMOD_XR_EYE").c_str(), "%ux%u", &p.eye_w, &p.eye_h);
    if (st.empty() && p.detail.empty())
        p.detail = "the probe stopped before answering";
    return p;
}

XrProbe xr_probe_child(int timeout_ms) {
    const ChildResult r = run_child({cli_path(), "probe", "xr"}, timeout_ms);
    if (r.timed_out) {
        XrProbe p;
        p.status = XrStatus::TimedOut;
        p.detail = "the VR runtime didn't answer in time";
        return p;
    }
    if (!r.started) {
        XrProbe p;
        p.status = XrStatus::Failed;
        p.detail = "couldn't start the probe";
        return p;
    }
    return xr_probe_parse(r.out);
}

// ------------------------------------------------------------------- Vulkan
bool GpuInfo::capable() const {
    // X4's widest pipeline layout declares 6 sets and the layer binds its own at
    // the top of the device's range (pose_set_index), so 7 is the real floor.
    return multiview && max_sets >= 7 &&
           (api_major > 1 || (api_major == 1 && api_minor >= 2));
}

VulkanProbe vulkan_probe_here() {
    VulkanProbe out;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        out.detail = "no Vulkan loader (libvulkan.so.1)";
        return out;
    }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    auto create = gipa ? (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance") : nullptr;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "x4vrmod probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (!create || create(&ci, nullptr, &inst) != VK_SUCCESS) {
        out.detail = "a Vulkan instance wouldn't start";
        return out;
    }
    out.ok = true;
#define VK_GET(name) auto name = (PFN_##name)gipa(inst, #name)
    VK_GET(vkEnumeratePhysicalDevices);
    VK_GET(vkGetPhysicalDeviceProperties);
    VK_GET(vkGetPhysicalDeviceFeatures2);
    VK_GET(vkDestroyInstance);
#undef VK_GET
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue; // llvmpipe can't run X4; don't let it decide the row
        VkPhysicalDeviceVulkan11Features v11{};
        v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &v11;
        vkGetPhysicalDeviceFeatures2(d, &f2);
        GpuInfo g;
        g.name = p.deviceName;
        g.api_major = VK_API_VERSION_MAJOR(p.apiVersion);
        g.api_minor = VK_API_VERSION_MINOR(p.apiVersion);
        g.max_sets = p.limits.maxBoundDescriptorSets;
        g.multiview = v11.multiview == VK_TRUE;
        g.discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        out.gpus.push_back(std::move(g));
    }
    vkDestroyInstance(inst, nullptr);
    return out;
}

std::string vulkan_probe_format(const VulkanProbe &p) {
    std::string s = std::string("X4VRMOD_VK_OK=") + (p.ok ? "1" : "0") + "\n";
    s += "X4VRMOD_VK_DETAIL=" + one_line(p.detail) + "\n";
    for (const GpuInfo &g : p.gpus) {
        char b[96];
        snprintf(b, sizeof b, "X4VRMOD_VK_GPU=%u %u %u %d %d ", g.api_major,
                 g.api_minor, g.max_sets, g.multiview ? 1 : 0, g.discrete ? 1 : 0);
        s += b + one_line(g.name) + "\n";
    }
    return s;
}

VulkanProbe vulkan_probe_parse(const std::string &text) {
    VulkanProbe p;
    p.ok = value_of(text, "X4VRMOD_VK_OK") == "1";
    p.detail = value_of(text, "X4VRMOD_VK_DETAIL");
    for (const std::string &v : values_of(text, "X4VRMOD_VK_GPU")) {
        GpuInfo g;
        int mv = 0, disc = 0, used = 0;
        if (sscanf(v.c_str(), "%u %u %u %d %d %n", &g.api_major, &g.api_minor,
                   &g.max_sets, &mv, &disc, &used) < 5)
            continue;
        g.multiview = mv != 0;
        g.discrete = disc != 0;
        g.name = v.substr((size_t)used);
        p.gpus.push_back(std::move(g));
    }
    return p;
}

VulkanProbe vulkan_probe_child(int timeout_ms) {
    // Implicit layers (overlays, recorders) have no business in a capability
    // question, and one that misbehaves shouldn't fail it.
    const ChildResult r = run_child({cli_path(), "probe", "vulkan"}, timeout_ms, false,
                                    {"VK_LOADER_LAYERS_DISABLE=~implicit~"});
    if (r.timed_out || !r.started) {
        VulkanProbe p;
        p.detail = r.timed_out ? "the Vulkan driver didn't answer in time"
                               : "couldn't start the probe";
        return p;
    }
    VulkanProbe p = vulkan_probe_parse(r.out);
    if (!p.ok && p.detail.empty())
        p.detail = "the probe stopped before answering";
    return p;
}

} // namespace x4vr::launcher
