// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_xr.hpp — the OpenXR side: a session on *X4's* Vulkan device.
//
// Two constraints shape everything here, and both are easy to get wrong.
//
// 1. The runtime goes first. It decides which Vulkan instance extensions,
//    which device extensions and which VkPhysicalDevice the session may use,
//    and it decides them *before* the device exists. So the XrInstance has to
//    be created inside our vkCreateInstance hook, ahead of the down-chain
//    create, not lazily at first present. XR_KHR_vulkan_enable2 is built for
//    exactly this shape: xrCreateVulkanInstanceKHR takes the application's own
//    VkInstanceCreateInfo and a pfnGetInstanceProcAddr, merges in what the
//    runtime needs and calls through. In a layer, "the application's create
//    info" is X4's, and "call through" is the next layer down — so the game
//    still creates its own instance, with the runtime's additions, and never
//    knows. That is the non-intrusive shape this project asks for.
//
// 2. The loader is dlopen'd, never linked. A machine with no OpenXR installed
//    must still run the mod flatscreen. Linking libopenxr_loader.so would make
//    the layer fail to load on such a machine — the layer is injected into
//    someone else's process, so a missing DT_NEEDED is not a graceful
//    degradation, it is X4 refusing to start. The headers are a *build*
//    dependency; the loader is a *runtime* one, and only when VR is asked for.
//
// Everything is reached through function pointers resolved from
// xrGetInstanceProcAddr, which is the only symbol taken from the .so.
//
// Nothing in this header is called unless the run asked for VR. It reports
// what it finds and what it cannot do; it never decides on its own that VR is
// a bad idea and quietly carries on flat.
#pragma once

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_reflection.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x4vr {
namespace xr {

// ---------------------------------------------------------------- names

// The reflection header lists every enumerant, so the table is the spec's
// rather than one I typed out and let drift.
inline const char *result_name(XrResult r) {
    switch (r) {
#define X4VR_XR_CASE(name, value) case value: return #name;
        XR_LIST_ENUM_XrResult(X4VR_XR_CASE)
#undef X4VR_XR_CASE
    default:
        break;
    }
    static char other[32];
    snprintf(other, sizeof(other), "XrResult(%d)", (int)r);
    return other;
}

inline const char *session_state_name(XrSessionState s) {
    switch (s) {
#define X4VR_XR_CASE(name, value) case value: return #name;
        XR_LIST_ENUM_XrSessionState(X4VR_XR_CASE)
#undef X4VR_XR_CASE
    default:
        break;
    }
    return "XrSessionState(?)";
}

inline float deg(float radians) { return radians * 57.29577951308232f; }

// ------------------------------------------------------------------ api

// Every entry point the mod uses, resolved through xrGetInstanceProcAddr.
// Split in two because the global ones are available before an XrInstance
// exists and the rest are not.
struct Api {
    void *dl = nullptr;
    PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;

    // global (XR_NULL_HANDLE)
    PFN_xrEnumerateApiLayerProperties EnumerateApiLayerProperties = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties
        EnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance CreateInstance = nullptr;

    // instance
    PFN_xrDestroyInstance DestroyInstance = nullptr;
    PFN_xrGetInstanceProperties GetInstanceProperties = nullptr;
    PFN_xrGetSystem GetSystem = nullptr;
    PFN_xrGetSystemProperties GetSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurations EnumerateViewConfigurations = nullptr;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews =
        nullptr;
    PFN_xrEnumerateEnvironmentBlendModes EnumerateEnvironmentBlendModes =
        nullptr;
    PFN_xrCreateSession CreateSession = nullptr;
    PFN_xrDestroySession DestroySession = nullptr;
    PFN_xrEnumerateReferenceSpaces EnumerateReferenceSpaces = nullptr;
    PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
    PFN_xrDestroySpace DestroySpace = nullptr;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain CreateSwapchain = nullptr;
    PFN_xrDestroySwapchain DestroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
    PFN_xrBeginSession BeginSession = nullptr;
    PFN_xrEndSession EndSession = nullptr;
    PFN_xrRequestExitSession RequestExitSession = nullptr;
    PFN_xrPollEvent PollEvent = nullptr;
    PFN_xrWaitFrame WaitFrame = nullptr;
    PFN_xrBeginFrame BeginFrame = nullptr;
    PFN_xrEndFrame EndFrame = nullptr;
    PFN_xrLocateViews LocateViews = nullptr;
    PFN_xrLocateSpace LocateSpace = nullptr;

    // XR_KHR_vulkan_enable2
    PFN_xrGetVulkanGraphicsRequirements2KHR GetVulkanGraphicsRequirements2KHR =
        nullptr;
    PFN_xrCreateVulkanInstanceKHR CreateVulkanInstanceKHR = nullptr;
    PFN_xrCreateVulkanDeviceKHR CreateVulkanDeviceKHR = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR GetVulkanGraphicsDevice2KHR = nullptr;

    // XR_KHR_vulkan_enable — the v1 extension, and the one a Vulkan LAYER has
    // to use. See the note above create_vk_instance.
    PFN_xrGetVulkanGraphicsRequirementsKHR GetVulkanGraphicsRequirementsKHR =
        nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR GetVulkanInstanceExtensionsKHR =
        nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR GetVulkanDeviceExtensionsKHR = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR GetVulkanGraphicsDeviceKHR = nullptr;
};

// The soname, not the linker name: a machine that has the runtime but not the
// SDK's -dev package has only libopenxr_loader.so.1.
inline bool api_open(Api &api, std::string *why) {
    if (api.GetInstanceProcAddr)
        return true;
    const char *sonames[] = {"libopenxr_loader.so.1", "libopenxr_loader.so"};
    for (const char *n : sonames) {
        api.dl = dlopen(n, RTLD_NOW | RTLD_LOCAL);
        if (api.dl)
            break;
    }
    if (!api.dl) {
        if (why)
            *why = std::string("dlopen libopenxr_loader.so.1: ") +
                   (dlerror() ? dlerror() : "?");
        return false;
    }
    api.GetInstanceProcAddr =
        (PFN_xrGetInstanceProcAddr)dlsym(api.dl, "xrGetInstanceProcAddr");
    if (!api.GetInstanceProcAddr) {
        if (why)
            *why = "libopenxr_loader has no xrGetInstanceProcAddr";
        return false;
    }
#define X4VR_XR_GLOBAL(name)                                                   \
    api.GetInstanceProcAddr(XR_NULL_HANDLE, "xr" #name,                        \
                            (PFN_xrVoidFunction *)&api.name)
    X4VR_XR_GLOBAL(EnumerateApiLayerProperties);
    X4VR_XR_GLOBAL(EnumerateInstanceExtensionProperties);
    X4VR_XR_GLOBAL(CreateInstance);
#undef X4VR_XR_GLOBAL
    if (!api.CreateInstance) {
        if (why)
            *why = "loader would not resolve xrCreateInstance";
        return false;
    }
    return true;
}

inline void api_resolve_instance(Api &api, XrInstance inst) {
#define X4VR_XR_RESOLVE(name)                                                  \
    api.GetInstanceProcAddr(inst, "xr" #name, (PFN_xrVoidFunction *)&api.name)
    X4VR_XR_RESOLVE(DestroyInstance);
    X4VR_XR_RESOLVE(GetInstanceProperties);
    X4VR_XR_RESOLVE(GetSystem);
    X4VR_XR_RESOLVE(GetSystemProperties);
    X4VR_XR_RESOLVE(EnumerateViewConfigurations);
    X4VR_XR_RESOLVE(EnumerateViewConfigurationViews);
    X4VR_XR_RESOLVE(EnumerateEnvironmentBlendModes);
    X4VR_XR_RESOLVE(CreateSession);
    X4VR_XR_RESOLVE(DestroySession);
    X4VR_XR_RESOLVE(EnumerateReferenceSpaces);
    X4VR_XR_RESOLVE(CreateReferenceSpace);
    X4VR_XR_RESOLVE(DestroySpace);
    X4VR_XR_RESOLVE(EnumerateSwapchainFormats);
    X4VR_XR_RESOLVE(CreateSwapchain);
    X4VR_XR_RESOLVE(DestroySwapchain);
    X4VR_XR_RESOLVE(EnumerateSwapchainImages);
    X4VR_XR_RESOLVE(AcquireSwapchainImage);
    X4VR_XR_RESOLVE(WaitSwapchainImage);
    X4VR_XR_RESOLVE(ReleaseSwapchainImage);
    X4VR_XR_RESOLVE(BeginSession);
    X4VR_XR_RESOLVE(EndSession);
    X4VR_XR_RESOLVE(RequestExitSession);
    X4VR_XR_RESOLVE(PollEvent);
    X4VR_XR_RESOLVE(WaitFrame);
    X4VR_XR_RESOLVE(BeginFrame);
    X4VR_XR_RESOLVE(EndFrame);
    X4VR_XR_RESOLVE(LocateViews);
    X4VR_XR_RESOLVE(LocateSpace);
    X4VR_XR_RESOLVE(GetVulkanGraphicsRequirements2KHR);
    X4VR_XR_RESOLVE(CreateVulkanInstanceKHR);
    X4VR_XR_RESOLVE(CreateVulkanDeviceKHR);
    X4VR_XR_RESOLVE(GetVulkanGraphicsDevice2KHR);
    X4VR_XR_RESOLVE(GetVulkanGraphicsRequirementsKHR);
    X4VR_XR_RESOLVE(GetVulkanInstanceExtensionsKHR);
    X4VR_XR_RESOLVE(GetVulkanDeviceExtensionsKHR);
    X4VR_XR_RESOLVE(GetVulkanGraphicsDeviceKHR);
#undef X4VR_XR_RESOLVE
}

// -------------------------------------------------------------- runtime

// What the runtime is, and what it wants — everything knowable before a
// session exists. The per-view *field of view* is deliberately not here: it
// comes from xrLocateViews, which needs a session and a frame, so a design
// that assumes the FOV is static configuration would find out too late.
struct Runtime {
    Api api;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;

    char runtime_name[XR_MAX_RUNTIME_NAME_SIZE] = {0};
    XrVersion runtime_version = 0;
    XrVersion api_version = 0;
    char system_name[XR_MAX_SYSTEM_NAME_SIZE] = {0};

    static constexpr uint32_t kMaxViews = 4;
    uint32_t view_count = 0;
    XrViewConfigurationView views[kMaxViews] = {};
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrGraphicsRequirementsVulkan2KHR vk_req = {};

    std::string last_error;

    bool ok() const { return instance != XR_NULL_HANDLE; }
};

inline bool have_extension(const std::vector<XrExtensionProperties> &v,
                           const char *name) {
    for (const auto &e : v)
        if (strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

// The XrResult is returned rather than folded into an empty vector, because
// "the runtime offers no extensions" and "there is no runtime" are different
// facts that a bare empty list cannot tell apart -- and the second is the one
// a user will actually hit, since active_runtime.json only exists while WiVRn
// or SteamVR is running.
inline XrResult enumerate_extensions(Api &api,
                                     std::vector<XrExtensionProperties> &out) {
    out.clear();
    if (!api.EnumerateInstanceExtensionProperties)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    uint32_t n = 0;
    XrResult r = api.EnumerateInstanceExtensionProperties(nullptr, 0, &n,
                                                          nullptr);
    if (r != XR_SUCCESS)
        return r;
    std::vector<XrExtensionProperties> v(n);
    for (auto &e : v)
        e.type = XR_TYPE_EXTENSION_PROPERTIES;
    r = api.EnumerateInstanceExtensionProperties(nullptr, n, &n, v.data());
    if (r != XR_SUCCESS)
        return r;
    v.resize(n);
    out.swap(v);
    return XR_SUCCESS;
}

// Create the XrInstance and pick the system. Returns false with last_error set
// on any failure; the caller decides what to do about it, because "no headset
// connected" and "no runtime installed" want very different words in the log
// and this function is not the place that knows which the user meant.
//
// sink, if given, receives one human-readable line per fact discovered. The
// layer passes X4VR_LOG and the probe passes printf, so the same bring-up is
// narrated identically in a game log and on a terminal.
using Sink = void (*)(void *, const char *);

inline void say_to(Sink sink, void *user, const char *fmt, ...) {
    if (!sink)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sink(user, buf);
}

inline bool runtime_open(Runtime &rt, Sink sink, void *user) {
#define say(...) say_to(sink, user, __VA_ARGS__)
    if (!api_open(rt.api, &rt.last_error)) {
        say("xr: no loader — %s", rt.last_error.c_str());
        return false;
    }

    std::vector<XrExtensionProperties> exts;
    XrResult er = enumerate_extensions(rt.api, exts);
    if (er != XR_SUCCESS) {
        rt.last_error =
            std::string("no active OpenXR runtime (") + result_name(er) + ")";
        say("xr: no active runtime — xrEnumerateInstanceExtensionProperties "
            "returned %s. active_runtime.json exists only while a runtime is "
            "running: start WiVRn or SteamVR first.",
            result_name(er));
        return false;
    }
    std::string names;
    for (const auto &e : exts) {
        names += ' ';
        names += e.extensionName;
    }
    say("xr: runtime offers %u instance extension(s):%s", (unsigned)exts.size(),
        names.empty() ? " (none)" : names.c_str());

    // v1 is the one a layer needs (see the note above vk_instance_extensions);
    // enable2 is asked for as well when offered, because the standalone probe
    // uses it and because it makes the merged lists observable. v1 is the hard
    // requirement now, not enable2.
    if (!have_extension(exts, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        rt.last_error = "runtime lacks " XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
        say("xr: %s — the v1 extension is what a Vulkan layer can use; enable2 "
            "hands the runtime a down-chain vkGetInstanceProcAddr and puts its "
            "handles in the wrong space (take 111)",
            rt.last_error.c_str());
        return false;
    }
    const char *want[2] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, nullptr};
    uint32_t want_n = 1;
    if (have_extension(exts, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
        want[want_n++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    XrInstanceCreateInfo ici{};
    ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
    snprintf(ici.applicationInfo.applicationName,
             sizeof(ici.applicationInfo.applicationName), "X4: Foundations");
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName,
             sizeof(ici.applicationInfo.engineName), "X4VRMOD");
    ici.applicationInfo.engineVersion = 1;
    ici.enabledExtensionCount = want_n;
    ici.enabledExtensionNames = want;

    // Ask for 1.0 first. A 1.0 instance is the widest contract that satisfies
    // everything this mod uses, and a runtime that only implements 1.0 would
    // reject a 1.1 request outright. If the runtime turns out to want 1.1,
    // take it — but do not lead with it.
    const XrVersion tries[] = {XR_API_VERSION_1_0, XR_API_VERSION_1_1};
    XrResult r = XR_ERROR_RUNTIME_FAILURE;
    for (XrVersion v : tries) {
        ici.applicationInfo.apiVersion = v;
        r = rt.api.CreateInstance(&ici, &rt.instance);
        if (r == XR_SUCCESS) {
            rt.api_version = v;
            break;
        }
        say("xr: xrCreateInstance at %d.%d -> %s",
            (int)XR_VERSION_MAJOR(v), (int)XR_VERSION_MINOR(v), result_name(r));
        if (r != XR_ERROR_API_VERSION_UNSUPPORTED)
            break;
    }
    if (r != XR_SUCCESS) {
        rt.instance = XR_NULL_HANDLE;
        rt.last_error = std::string("xrCreateInstance: ") + result_name(r);
        return false;
    }
    api_resolve_instance(rt.api, rt.instance);

    XrInstanceProperties props{};
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (rt.api.GetInstanceProperties &&
        rt.api.GetInstanceProperties(rt.instance, &props) == XR_SUCCESS) {
        memcpy(rt.runtime_name, props.runtimeName, sizeof(rt.runtime_name));
        rt.runtime_version = props.runtimeVersion;
    }
    say("xr: runtime \"%s\" %d.%d.%d, instance at api %d.%d", rt.runtime_name,
        (int)XR_VERSION_MAJOR(rt.runtime_version),
        (int)XR_VERSION_MINOR(rt.runtime_version),
        (int)XR_VERSION_PATCH(rt.runtime_version),
        (int)XR_VERSION_MAJOR(rt.api_version),
        (int)XR_VERSION_MINOR(rt.api_version));

    XrSystemGetInfo sgi{};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = rt.api.GetSystem(rt.instance, &sgi, &rt.system);
    if (r != XR_SUCCESS) {
        rt.last_error = std::string("xrGetSystem: ") + result_name(r);
        // This is the failure a user will actually hit, so it gets the
        // sentence rather than the enumerant.
        say("xr: no head-mounted system — %s%s", result_name(r),
            r == XR_ERROR_FORM_FACTOR_UNAVAILABLE
                ? " (the runtime is up, but no headset is connected to it)"
                : "");
        return false;
    }

    XrSystemProperties sp{};
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    if (rt.api.GetSystemProperties(rt.instance, rt.system, &sp) == XR_SUCCESS) {
        memcpy(rt.system_name, sp.systemName, sizeof(rt.system_name));
        say("xr: system \"%s\" — max swapchain %ux%u, %u layer(s), "
            "orientation=%d position=%d",
            rt.system_name, sp.graphicsProperties.maxSwapchainImageWidth,
            sp.graphicsProperties.maxSwapchainImageHeight,
            sp.graphicsProperties.maxLayerCount,
            (int)sp.trackingProperties.orientationTracking,
            (int)sp.trackingProperties.positionTracking);
    }

    uint32_t n = 0;
    if (rt.api.EnumerateViewConfigurationViews(
            rt.instance, rt.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &n, nullptr) != XR_SUCCESS ||
        n == 0) {
        rt.last_error = "runtime has no PRIMARY_STEREO view configuration";
        say("xr: %s", rt.last_error.c_str());
        return false;
    }
    if (n > Runtime::kMaxViews)
        n = Runtime::kMaxViews;
    for (uint32_t i = 0; i < n; i++)
        rt.views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (rt.api.EnumerateViewConfigurationViews(
            rt.instance, rt.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            n, &n, rt.views) != XR_SUCCESS) {
        rt.last_error = "xrEnumerateViewConfigurationViews failed";
        return false;
    }
    rt.view_count = n;
    for (uint32_t i = 0; i < n; i++)
        say("xr: view %u — recommended %ux%u (max %ux%u), samples %u (max %u)",
            i, rt.views[i].recommendedImageRectWidth,
            rt.views[i].recommendedImageRectHeight,
            rt.views[i].maxImageRectWidth, rt.views[i].maxImageRectHeight,
            rt.views[i].recommendedSwapchainSampleCount,
            rt.views[i].maxSwapchainSampleCount);

    uint32_t bn = 0;
    XrEnvironmentBlendMode modes[8] = {};
    if (rt.api.EnumerateEnvironmentBlendModes(
            rt.instance, rt.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            8, &bn, modes) == XR_SUCCESS &&
        bn)
        rt.blend = modes[0];

    // Both spellings, when both are enabled. The runtime records "has the
    // application asked?" per extension and refuses xrCreateSession with
    // XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING against whichever it decides
    // to check -- so calling only one is a coin flip settled inside the
    // runtime.
    rt.vk_req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    XrGraphicsRequirementsVulkanKHR req1{};
    req1.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    const XrResult r1 = rt.api.GetVulkanGraphicsRequirementsKHR
                            ? rt.api.GetVulkanGraphicsRequirementsKHR(
                                  rt.instance, rt.system, &req1)
                            : XR_ERROR_FUNCTION_UNSUPPORTED;
    const XrResult r2 = rt.api.GetVulkanGraphicsRequirements2KHR
                            ? rt.api.GetVulkanGraphicsRequirements2KHR(
                                  rt.instance, rt.system, &rt.vk_req)
                            : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (r1 != XR_SUCCESS && r2 != XR_SUCCESS) {
        rt.last_error = std::string("xrGetVulkanGraphicsRequirements: v1 ") +
                        result_name(r1) + ", v2 " + result_name(r2);
        say("xr: %s", rt.last_error.c_str());
        return false;
    }
    if (r2 != XR_SUCCESS) { // carry v1's answer into the field we report
        rt.vk_req.minApiVersionSupported = req1.minApiVersionSupported;
        rt.vk_req.maxApiVersionSupported = req1.maxApiVersionSupported;
    }
    say("xr: wants Vulkan %d.%d.%d .. %d.%d.%d",
        (int)XR_VERSION_MAJOR(rt.vk_req.minApiVersionSupported),
        (int)XR_VERSION_MINOR(rt.vk_req.minApiVersionSupported),
        (int)XR_VERSION_PATCH(rt.vk_req.minApiVersionSupported),
        (int)XR_VERSION_MAJOR(rt.vk_req.maxApiVersionSupported),
        (int)XR_VERSION_MINOR(rt.vk_req.maxApiVersionSupported),
        (int)XR_VERSION_PATCH(rt.vk_req.maxApiVersionSupported));
    return true;
#undef say
}

inline void runtime_close(Runtime &rt) {
    if (rt.instance != XR_NULL_HANDLE && rt.api.DestroyInstance)
        rt.api.DestroyInstance(rt.instance);
    rt.instance = XR_NULL_HANDLE;
    rt.system = XR_NULL_SYSTEM_ID;
}

// ------------------------------------------------- Vulkan, the runtime's way

// The three calls that must replace plain vkCreateInstance / device selection /
// vkCreateDevice. Each takes the create info the *application* wanted and the
// down-chain entry point, so in the layer these wrap X4's own structs and in
// the probe they wrap ones shaped like X4's.
//
// vk_result is the VkResult the inner create returned; the XrResult and the
// VkResult can disagree (the runtime succeeded at asking, the driver refused),
// and collapsing them into one boolean is how a misconfigured device gets
// blamed on the runtime.
inline XrResult create_vk_instance(Runtime &rt, PFN_vkGetInstanceProcAddr gipa,
                                   const VkInstanceCreateInfo *ci,
                                   const VkAllocationCallbacks *ac,
                                   VkInstance *out, VkResult *vk_result) {
    XrVulkanInstanceCreateInfoKHR info{};
    info.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    info.systemId = rt.system;
    info.pfnGetInstanceProcAddr = gipa;
    info.vulkanCreateInfo = ci;
    info.vulkanAllocator = ac;
    *vk_result = VK_ERROR_INITIALIZATION_FAILED;
    return rt.api.CreateVulkanInstanceKHR(rt.instance, &info, out, vk_result);
}

inline XrResult graphics_device(Runtime &rt, VkInstance vk,
                                VkPhysicalDevice *out) {
    XrVulkanGraphicsDeviceGetInfoKHR info{};
    info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    info.systemId = rt.system;
    info.vulkanInstance = vk;
    return rt.api.GetVulkanGraphicsDevice2KHR(rt.instance, &info, out);
}

// ---------------------------------------------- Vulkan, the v1 way (a layer)
//
// enable2 is the better contract for an APPLICATION and the wrong one for a
// LAYER, and take 111 is the proof.
//
// xrCreateVulkanInstanceKHR takes a pfnGetInstanceProcAddr. From inside a layer
// the only one available is the down-chain one, and handing that to the runtime
// puts the runtime's Vulkan calls in a different space from the loader's public
// entry points -- which is what the graphics binding, and Monado's own client
// compositor, are obliged to use. X4 aborted in vkGetPhysicalDeviceMemoryProperties
// with "Invalid physicalDevice", and the follow-up attempt hit
// XR_ERROR_VALIDATION_FAILURE from oxr_session.c:1151, which requires the
// binding's device to be exactly the one xrGetVulkanGraphicsDeviceKHR returned.
//
// The v1 extension has no such channel. It returns *lists of extension names*
// and lets the application add them to its own create-infos -- which is an edit
// this layer already knows how to make, for multiview -- and its
// xrGetVulkanGraphicsDeviceKHR uses the runtime's own linked
// vkGetInstanceProcAddr, i.e. the loader's public one. Everything stays in the
// space the runtime will actually use.
//
// The extension names come back space-separated in a single buffer.
inline bool split_names(const char *s, std::vector<std::string> &out) {
    out.clear();
    if (!s)
        return false;
    std::string cur;
    for (; *s; s++) {
        if (*s == ' ') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += *s;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return true;
}

inline bool vk_instance_extensions(Runtime &rt, std::vector<std::string> &out) {
    out.clear();
    if (!rt.api.GetVulkanInstanceExtensionsKHR)
        return false;
    uint32_t n = 0;
    if (rt.api.GetVulkanInstanceExtensionsKHR(rt.instance, rt.system, 0, &n,
                                              nullptr) != XR_SUCCESS)
        return false;
    if (!n)
        return true; // legitimately none
    std::vector<char> buf(n);
    if (rt.api.GetVulkanInstanceExtensionsKHR(rt.instance, rt.system, n, &n,
                                              buf.data()) != XR_SUCCESS)
        return false;
    return split_names(buf.data(), out);
}

inline bool vk_device_extensions(Runtime &rt, std::vector<std::string> &out) {
    out.clear();
    if (!rt.api.GetVulkanDeviceExtensionsKHR)
        return false;
    uint32_t n = 0;
    if (rt.api.GetVulkanDeviceExtensionsKHR(rt.instance, rt.system, 0, &n,
                                            nullptr) != XR_SUCCESS)
        return false;
    if (!n)
        return true;
    std::vector<char> buf(n);
    if (rt.api.GetVulkanDeviceExtensionsKHR(rt.instance, rt.system, n, &n,
                                            buf.data()) != XR_SUCCESS)
        return false;
    return split_names(buf.data(), out);
}

// The handle the graphics binding MUST carry: Monado compares the binding's
// physicalDevice against this one and fails the session if they differ.
inline XrResult graphics_device_v1(Runtime &rt, VkInstance vk,
                                   VkPhysicalDevice *out) {
    if (!rt.api.GetVulkanGraphicsDeviceKHR)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    return rt.api.GetVulkanGraphicsDeviceKHR(rt.instance, rt.system, vk, out);
}

// Must be called before xrCreateSession or the runtime refuses with
// XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING. The v1 spelling, to match.
inline XrResult graphics_requirements_v1(Runtime &rt) {
    if (!rt.api.GetVulkanGraphicsRequirementsKHR)
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrGraphicsRequirementsVulkanKHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    return rt.api.GetVulkanGraphicsRequirementsKHR(rt.instance, rt.system, &req);
}

inline XrResult create_vk_device(Runtime &rt, PFN_vkGetInstanceProcAddr gipa,
                                 VkPhysicalDevice phys,
                                 const VkDeviceCreateInfo *ci,
                                 const VkAllocationCallbacks *ac, VkDevice *out,
                                 VkResult *vk_result) {
    XrVulkanDeviceCreateInfoKHR info{};
    info.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    info.systemId = rt.system;
    info.pfnGetInstanceProcAddr = gipa;
    info.vulkanPhysicalDevice = phys;
    info.vulkanCreateInfo = ci;
    info.vulkanAllocator = ac;
    *vk_result = VK_ERROR_INITIALIZATION_FAILED;
    return rt.api.CreateVulkanDeviceKHR(rt.instance, &info, out, vk_result);
}

// -------------------------------------------------------------- session

struct Session {
    Runtime *rt = nullptr;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;     // the reference space we submit against
    XrSpace view_space = XR_NULL_HANDLE; // VIEW, for "where is the head"
    XrReferenceSpaceType space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;

    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;   // between xrBeginSession and xrEndSession
    bool exit_requested = false;

    XrFrameState frame = {};
    std::string last_error;

    bool ok() const { return session != XR_NULL_HANDLE; }
};

inline XrResult session_create(Session &s, Runtime &rt, VkInstance vk,
                               VkPhysicalDevice phys, VkDevice dev,
                               uint32_t queue_family, uint32_t queue_index) {
    s.rt = &rt;
    XrGraphicsBindingVulkan2KHR bind{};
    bind.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
    bind.instance = vk;
    bind.physicalDevice = phys;
    bind.device = dev;
    bind.queueFamilyIndex = queue_family;
    bind.queueIndex = queue_index;

    XrSessionCreateInfo sci{};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &bind;
    sci.systemId = rt.system;
    XrResult r = rt.api.CreateSession(rt.instance, &sci, &s.session);
    if (r != XR_SUCCESS) {
        s.last_error = std::string("xrCreateSession: ") + result_name(r);
        s.session = XR_NULL_HANDLE;
        return r;
    }

    // STAGE if the runtime has it (a floor-relative origin), LOCAL otherwise.
    // Which one we get changes what "sitting still" means, so it is recorded
    // rather than assumed.
    uint32_t n = 0;
    XrReferenceSpaceType avail[8] = {};
    bool stage = false;
    if (rt.api.EnumerateReferenceSpaces(s.session, 8, &n, avail) == XR_SUCCESS)
        for (uint32_t i = 0; i < n; i++)
            if (avail[i] == XR_REFERENCE_SPACE_TYPE_STAGE)
                stage = true;
    s.space_type = stage ? XR_REFERENCE_SPACE_TYPE_STAGE
                         : XR_REFERENCE_SPACE_TYPE_LOCAL;

    XrReferenceSpaceCreateInfo rsi{};
    rsi.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsi.referenceSpaceType = s.space_type;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    r = rt.api.CreateReferenceSpace(s.session, &rsi, &s.space);
    if (r != XR_SUCCESS) {
        s.last_error = std::string("xrCreateReferenceSpace: ") + result_name(r);
        return r;
    }
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rt.api.CreateReferenceSpace(s.session, &rsi, &s.view_space);
    return XR_SUCCESS;
}

inline void session_destroy(Session &s) {
    if (!s.rt)
        return;
    if (s.view_space != XR_NULL_HANDLE)
        s.rt->api.DestroySpace(s.view_space);
    if (s.space != XR_NULL_HANDLE)
        s.rt->api.DestroySpace(s.space);
    if (s.session != XR_NULL_HANDLE)
        s.rt->api.DestroySession(s.session);
    s.view_space = s.space = XR_NULL_HANDLE;
    s.session = XR_NULL_HANDLE;
}

// Drain the event queue and drive the session state machine. Returns false
// once the runtime has told us to go away.
//
// on_state, if given, is called for every transition — the caller usually
// wants to log them, and a state machine that logs from inside itself cannot
// be reused by something that logs differently.
inline bool session_poll(Session &s, void (*on_state)(void *, XrSessionState),
                         void *user) {
    if (!s.rt)
        return false;
    XrEventDataBuffer ev{};
    for (;;) {
        ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult r = s.rt->api.PollEvent(s.rt->instance, &ev);
        if (r == XR_EVENT_UNAVAILABLE)
            break;
        if (r != XR_SUCCESS)
            break;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto *c = (const XrEventDataSessionStateChanged *)&ev;
            s.state = c->state;
            if (on_state)
                on_state(user, c->state);
            if (c->state == XR_SESSION_STATE_READY && !s.running) {
                XrSessionBeginInfo bi{};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (s.rt->api.BeginSession(s.session, &bi) == XR_SUCCESS)
                    s.running = true;
            } else if (c->state == XR_SESSION_STATE_STOPPING && s.running) {
                s.rt->api.EndSession(s.session);
                s.running = false;
            } else if (c->state == XR_SESSION_STATE_EXITING ||
                       c->state == XR_SESSION_STATE_LOSS_PENDING) {
                s.exit_requested = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            s.exit_requested = true;
        }
    }
    return !s.exit_requested;
}

// Returns true if the frame was actually begun, in which case frame_end() MUST
// be called for it. *should_render says whether the runtime wants pixels —
// which is a different question, and one that is false for plenty of frames
// that still have to be begun and ended.
//
// Those two were one boolean here until the layer needed them apart. Calling
// xrEndFrame after a failed xrBeginFrame is an error, and skipping xrEndFrame
// after a successful one leaves the runtime's pacing waiting for a frame that
// never arrives — so a single "did anything happen" return could only ever be
// wrong in one direction or the other.
inline bool frame_begin(Session &s, bool *should_render) {
    if (should_render)
        *should_render = false;
    if (!s.running)
        return false;
    XrFrameWaitInfo fwi{};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO;
    s.frame = {};
    s.frame.type = XR_TYPE_FRAME_STATE;
    if (s.rt->api.WaitFrame(s.session, &fwi, &s.frame) != XR_SUCCESS)
        return false;
    XrFrameBeginInfo fbi{};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
    const XrResult r = s.rt->api.BeginFrame(s.session, &fbi);
    // XR_FRAME_DISCARDED is a success code: the frame was begun.
    if (r != XR_SUCCESS && r != XR_FRAME_DISCARDED)
        return false;
    if (should_render)
        *should_render = s.frame.shouldRender == XR_TRUE;
    return true;
}

// Where the eyes are, at the display time of the frame begun above. Both the
// pose and the FOV come from here, per view, per frame: on some runtimes the
// FOV is fixed and on others it tracks eye relief, so nothing may cache it.
inline bool locate_views(Session &s, XrView *out, uint32_t count,
                         XrViewStateFlags *flags) {
    XrViewLocateInfo vli{};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = s.frame.predictedDisplayTime;
    vli.space = s.space;
    XrViewState vs{};
    vs.type = XR_TYPE_VIEW_STATE;
    for (uint32_t i = 0; i < count; i++)
        out[i] = XrView{XR_TYPE_VIEW, nullptr, {}, {}};
    uint32_t n = 0;
    if (s.rt->api.LocateViews(s.session, &vli, &vs, count, &n, out) !=
        XR_SUCCESS)
        return false;
    if (flags)
        *flags = vs.viewStateFlags;
    return n == count &&
           (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

inline XrResult frame_end(Session &s, const XrCompositionLayerBaseHeader *const *layers,
                          uint32_t layer_count) {
    XrFrameEndInfo fei{};
    fei.type = XR_TYPE_FRAME_END_INFO;
    fei.displayTime = s.frame.predictedDisplayTime;
    fei.environmentBlendMode = s.rt->blend;
    fei.layerCount = layer_count;
    fei.layers = layers;
    return s.rt->api.EndFrame(s.session, &fei);
}

// ------------------------------------------------------------ swapchain

struct Swapchain {
    Session *s = nullptr;
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t width = 0, height = 0, layers = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

// Pick a format the runtime offers, preferring the first of our candidates
// that appears in *its* order-of-preference list rather than ours: the runtime
// lists them best-first, and overriding that is how a mod ends up doing an
// sRGB conversion the compositor was going to do for free.
inline VkFormat choose_format(Session &s, const VkFormat *want, uint32_t nwant,
                              Sink sink = nullptr, void *user = nullptr) {
#define say(...) say_to(sink, user, __VA_ARGS__)
    uint32_t n = 0;
    if (s.rt->api.EnumerateSwapchainFormats(s.session, 0, &n, nullptr) !=
            XR_SUCCESS ||
        n == 0) {
        say("xr: the runtime offers NO swapchain formats");
        return VK_FORMAT_UNDEFINED;
    }
    std::vector<int64_t> have(n);
    if (s.rt->api.EnumerateSwapchainFormats(s.session, n, &n, have.data()) !=
        XR_SUCCESS)
        return VK_FORMAT_UNDEFINED;

    // Printed in the runtime's own order, which is its preference order. The
    // layer's copy from X4's B8G8R8A8_UNORM eye image is only byte-preserving
    // for a format with the same channel order, so which of these exists is a
    // load-bearing fact and not a detail -- and a silent VK_FORMAT_UNDEFINED
    // is how it would otherwise be discovered, one X4 take later.
    {
        char buf[512];
        int at = snprintf(buf, sizeof(buf), "xr: runtime offers %u swapchain "
                                            "format(s), best first:", n);
        for (uint32_t i = 0; i < n && at > 0 && at < (int)sizeof(buf); i++)
            at += snprintf(buf + at, sizeof(buf) - at, " %d", (int)have[i]);
        say("%s", buf);
    }

    for (int64_t f : have)
        for (uint32_t i = 0; i < nwant; i++)
            if ((VkFormat)f == want[i]) {
                say("xr: chose format %d (candidate %u of %u)", (int)f, i + 1,
                    nwant);
                return (VkFormat)f;
            }
    say("xr: NONE of our %u candidate format(s) is offered — a copy from X4's "
        "eye image cannot be byte-preserving, so this needs deciding rather "
        "than defaulting",
        nwant);
    return VK_FORMAT_UNDEFINED;
#undef say
}

inline XrResult swapchain_create(Swapchain &sc, Session &s, VkFormat fmt,
                                 uint32_t w, uint32_t h, uint32_t array_layers,
                                 uint32_t samples) {
    sc.s = &s;
    XrSwapchainCreateInfo ci{};
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = (int64_t)fmt;
    ci.sampleCount = samples;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = array_layers;
    ci.mipCount = 1;
    XrResult r = s.rt->api.CreateSwapchain(s.session, &ci, &sc.handle);
    if (r != XR_SUCCESS)
        return r;
    sc.width = w;
    sc.height = h;
    sc.layers = array_layers;
    sc.format = fmt;

    uint32_t n = 0;
    s.rt->api.EnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n);
    for (auto &i : imgs)
        i.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    r = s.rt->api.EnumerateSwapchainImages(
        sc.handle, n, &n, (XrSwapchainImageBaseHeader *)imgs.data());
    if (r != XR_SUCCESS)
        return r;
    sc.images.clear();
    for (uint32_t i = 0; i < n; i++)
        sc.images.push_back(imgs[i].image);
    return XR_SUCCESS;
}

inline bool swapchain_acquire(Swapchain &sc, uint32_t *index) {
    XrSwapchainImageAcquireInfo ai{};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (sc.s->rt->api.AcquireSwapchainImage(sc.handle, &ai, index) != XR_SUCCESS)
        return false;
    XrSwapchainImageWaitInfo wi{};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    return sc.s->rt->api.WaitSwapchainImage(sc.handle, &wi) == XR_SUCCESS;
}

inline void swapchain_release(Swapchain &sc) {
    XrSwapchainImageReleaseInfo ri{};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    sc.s->rt->api.ReleaseSwapchainImage(sc.handle, &ri);
}

inline void swapchain_destroy(Swapchain &sc) {
    if (sc.handle != XR_NULL_HANDLE && sc.s)
        sc.s->rt->api.DestroySwapchain(sc.handle);
    sc.handle = XR_NULL_HANDLE;
    sc.images.clear();
}

} // namespace xr
} // namespace x4vr
