// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// No OpenXR loader: the layer says so and X4 carries on (issue #4).
//
// X4 crashed at start inside Steam's runtime container, which has no OpenXR
// loader: api_open read dlerror() twice, and glibc's clears the message on the
// first read, so the second returned NULL and std::string + NULL was
// strlen(NULL). Here the loader goes missing the same way -- api_open's dlopen
// is sent to a name that doesn't exist, through the real dlopen -- so dlerror()
// is glibc's own, not a model of it. The control shows it really is read-once.
// Self-checking.
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../common/x4vr_xr.hpp"

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

static const char kMissing[] = "libx4vr-test-no-such-openxr-loader.so.1";

using DlopenFn = void *(*)(const char *, int);
static DlopenFn real_dlopen() {
    static DlopenFn f = (DlopenFn)dlsym(RTLD_NEXT, "dlopen");
    return f;
}

// The executable's own dlopen comes first, for the header's calls too.
extern "C" void *dlopen(const char *file, int mode) {
    if (file && !strncmp(file, "libopenxr_loader", 16))
        file = kMissing;
    return real_dlopen()(file, mode);
}

int main() {
    // The control: glibc's dlerror() gives the message once, then NULL.
    check(real_dlopen()(kMissing, RTLD_NOW) == nullptr, "control: the missing name really is missing");
    const char *first = dlerror();
    const char *second = dlerror();
    check(first != nullptr && second == nullptr,
          "control: dlerror() is read-once, so a second read is NULL");

    x4vr::xr::Api api;
    std::string why;
    const bool opened = x4vr::xr::api_open(api, &why);
    check(!opened, "no loader: api_open says no, and returns");
    check(why.find("dlopen libopenxr_loader.so.1: ") == 0 &&
              why.find(kMissing) != std::string::npos,
          "...and says why in dlopen's own words");
    printf("       why: %s\n", why.c_str());

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
