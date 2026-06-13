/*
 * x4vr_shm.h — shared-memory protocol for handing X4's composited SBS frame
 * from the DIBR layer (our vkShade, the writer) to a consumer (the dumper
 * tool now, x4vr-viewer later).
 *
 * Transport: a POSIX shm object (X4VR_SHM_NAME) holding a header followed by
 * N frame buffers. The writer round-robins through the buffers and publishes
 * the newest by bumping `seq` (release); the reader loads `seq` (acquire) and
 * reads buffer index (seq - 1) % buffers. With N>=3 the reader never observes
 * a buffer the writer is mid-write on, so no locks and no tearing.
 *
 * This header is the single source of truth and is copied verbatim into the
 * vkShade tree (keep the two in sync; bump X4VR_SHM_VERSION on any change).
 */
#ifndef X4VR_SHM_H
#define X4VR_SHM_H

#include <stdint.h>

#define X4VR_SHM_NAME    "/x4vr_sbs"
#define X4VR_SHM_MAGIC   0x52565834u /* 'X4VR' little-endian */
#define X4VR_SHM_VERSION 1u
#define X4VR_SHM_BUFFERS 3u

/* Pixel layout of each frame. Values mirror the Vulkan formats vkShade is
 * likely to present; the consumer swizzles/ignores sRGB as needed. */
enum {
    X4VR_FMT_UNKNOWN = 0,
    X4VR_FMT_B8G8R8A8 = 1, /* BGRA, 4 bytes/px (covers UNORM and SRGB) */
    X4VR_FMT_R8G8B8A8 = 2, /* RGBA, 4 bytes/px */
};

typedef struct {
    uint32_t magic;        /* X4VR_SHM_MAGIC once the writer is live */
    uint32_t version;      /* X4VR_SHM_VERSION */
    uint32_t width;        /* full SBS width (e.g. 2560) */
    uint32_t height;       /* full SBS height (e.g. 1280) */
    uint32_t stride;       /* bytes per row in each buffer */
    uint32_t format;       /* one of X4VR_FMT_* */
    uint32_t buffers;      /* number of frame buffers (X4VR_SHM_BUFFERS) */
    uint32_t buffer_bytes; /* size of one frame buffer (stride * height) */
    uint32_t data_offset;  /* byte offset from header start to buffer 0 */
    uint32_t _pad;
    /* Bumped (release) after each fully-written frame. Reader uses acquire.
     * Newest frame lives at buffer index (seq - 1) % buffers. 0 => none yet. */
    uint64_t seq;
} X4VRShmHeader;

/* Buffer 0 starts at data_offset; we 4 KiB-align it for mmap friendliness. */
#define X4VR_DATA_OFFSET 4096u

#endif /* X4VR_SHM_H */
