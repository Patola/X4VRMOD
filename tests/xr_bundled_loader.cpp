// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The mod's own OpenXR loader, beside the code that opens it (take 441).
//
// Started from Steam, X4 runs inside Steam's runtime container, which has no OpenXR loader, so
// api_open falls back to the copy beside the library it's built into. Here that library is this
// program: tests/run-xr-bundled-loader.sh copies it into a folder with and without the loader
// and reads what it prints. Not self-checking on its own.
//
//     x4vr_test_xr_bundled [nosystem] [runtime]
//
// nosystem  the system's loader goes missing, as in the container: api_open's dlopen of a
//           soname is sent to a name that doesn't exist, through the real dlopen
// runtime   the whole bring-up after it (runtime_open), narrated: needs a running runtime
#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../common/x4vr_xr.hpp"

static bool g_no_system = false;

using DlopenFn = void *(*)(const char *, int);
static DlopenFn real_dlopen() {
    static DlopenFn f = (DlopenFn)dlsym(RTLD_NEXT, "dlopen");
    return f;
}

// The executable's own dlopen comes first, for the header's calls too. A path is the mod's own
// copy and always goes through.
extern "C" void *dlopen(const char *file, int mode) {
    if (g_no_system && file && !strncmp(file, "libopenxr_loader", 16))
        file = "libx4vr-test-no-such-openxr-loader.so.1";
    return real_dlopen()(file, mode);
}

static void say(void *, const char *line) { printf("say: %s\n", line); }

int main(int argc, char **argv) {
    bool runtime = false;
    for (int i = 1; i < argc; i++) {
        g_no_system |= !strcmp(argv[i], "nosystem");
        runtime |= !strcmp(argv[i], "runtime");
    }
    printf("own dir: %s\n", x4vr::xr::own_library_dir().c_str());
    if (runtime) {
        x4vr::xr::Runtime rt;
        const bool ok = x4vr::xr::runtime_open(rt, say, nullptr);
        printf("%s: %s\n", ok ? "runtime" : "failed", ok ? rt.runtime_name
                                                         : rt.last_error.c_str());
        if (ok)
            printf("system: %s\n", rt.system_name);
        printf("loader: %s\n", rt.api.loader.c_str());
        x4vr::xr::runtime_close(rt);
        return ok ? 0 : 2;
    }
    x4vr::xr::Api api;
    std::string why;
    if (!x4vr::xr::api_open(api, &why)) {
        printf("failed: %s\n", why.c_str());
        return 2;
    }
    printf("opened: %s\n", api.loader.c_str());
    return 0;
}
