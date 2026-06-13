/*
 * shmdump — read the latest X4 SBS frame from the x4vr shm and write a PPM.
 *
 * Lets us verify the vkShade export path on the flat screen, with no headset
 * or viewer needed: run X4 with the export-enabled vkShade, then run this.
 *
 * Build: cc -O2 -o shmdump tools/shmdump.c -I shared
 * Use:   ./shmdump out.ppm        # one shot
 *        ./shmdump --watch        # print header + seq once per second
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "x4vr_shm.h"

int main(int argc, char **argv)
{
    const char *out = "x4vr_frame.ppm";
    int watch = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--watch")) watch = 1;
        else out = argv[i];
    }

    int fd = shm_open(X4VR_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "shm_open(%s) failed — is X4 running with the "
                "export-enabled vkShade (X4VR_EXPORT=1)?\n", X4VR_SHM_NAME);
        return 1;
    }

    /* Map the header first to learn the real size, then remap the whole thing. */
    X4VRShmHeader hdr;
    if (pread(fd, &hdr, sizeof(hdr), 0) != (long)sizeof(hdr)) {
        perror("pread header"); return 1;
    }
    if (hdr.magic != X4VR_SHM_MAGIC) {
        fprintf(stderr, "bad magic 0x%x (writer not live yet?)\n", hdr.magic);
        return 1;
    }
    if (hdr.version != X4VR_SHM_VERSION)
        fprintf(stderr, "warning: version %u != %u\n", hdr.version, X4VR_SHM_VERSION);

    size_t total = (size_t)hdr.data_offset + (size_t)hdr.buffers * hdr.buffer_bytes;
    uint8_t *base = mmap(NULL, total, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }
    volatile X4VRShmHeader *h = (volatile X4VRShmHeader *)base;

    if (watch) {
        fprintf(stderr, "%ux%u stride=%u fmt=%u buffers=%u — Ctrl-C to stop\n",
                hdr.width, hdr.height, hdr.stride, hdr.format, hdr.buffers);
        for (;;) {
            fprintf(stderr, "\rseq=%llu", (unsigned long long)h->seq);
            fflush(stderr);
            sleep(1);
        }
    }

    uint64_t seq = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
    if (seq == 0) { fprintf(stderr, "no frame published yet (seq=0)\n"); return 1; }
    uint32_t idx = (uint32_t)((seq - 1) % hdr.buffers);
    const uint8_t *buf = base + hdr.data_offset + (size_t)idx * hdr.buffer_bytes;

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", hdr.width, hdr.height);
    /* Both candidate formats are 4 bytes/px; pick RGB channel order by format. */
    int bgr = (hdr.format == X4VR_FMT_B8G8R8A8);
    for (uint32_t y = 0; y < hdr.height; y++) {
        const uint8_t *row = buf + (size_t)y * hdr.stride;
        for (uint32_t x = 0; x < hdr.width; x++) {
            const uint8_t *px = row + (size_t)x * 4;
            uint8_t rgb[3];
            if (bgr) { rgb[0] = px[2]; rgb[1] = px[1]; rgb[2] = px[0]; }
            else     { rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2]; }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%ux%u, seq=%llu, buffer %u)\n",
            out, hdr.width, hdr.height, (unsigned long long)seq, idx);
    return 0;
}
