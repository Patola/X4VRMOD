// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
// x4vr_log.hpp — minimal shared logging for the layer and the injector.
//
// Destination: $X4VR_LOG (file path) or stderr. Each line is prefixed with
// a monotonic timestamp and the component tag so interleaved layer/injector
// output stays readable.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace x4vr {

inline FILE *log_stream() {
    static FILE *f = [] {
        const char *path = getenv("X4VR_LOG");
        if (path && *path) {
            FILE *fp = fopen(path, "a");
            if (fp) {
                setvbuf(fp, nullptr, _IOLBF, 0); // line-buffered
                return fp;
            }
        }
        return stderr;
    }();
    return f;
}

inline void logf(const char *tag, const char *fmt, ...) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *f = log_stream();
    fprintf(f, "[%7ld.%03ld] %-7s ", ts.tv_sec, ts.tv_nsec / 1000000, tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

} // namespace x4vr

#define X4VR_LOG(...) ::x4vr::logf(X4VR_LOG_TAG, __VA_ARGS__)
