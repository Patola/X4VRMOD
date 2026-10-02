// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// The camera write-watch, driven for real before it goes near X4.
//
// A watchpoint instrument has two ways to lie: catch nothing because it was
// never armed where the store happens (and report a clean null), or catch the
// wrong thing (reads, other threads, other addresses). The live case below
// plants a writer, a decoy writer and a reader on a watched variable, and the
// only acceptable answer is exactly the writer's stores, at the writer's
// instruction. Self-checking; says SKIP loudly if the kernel refuses perf.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include "../injector/x4vr_camwatch.hpp"

using namespace x4vr::camwatch;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok)
        g_fail++;
}

// ---------------------------------------------------------------- ELF
struct Elf {
    std::vector<uint8_t> d;
    bool load(const char *path) {
        FILE *f = fopen(path, "rb");
        if (!f)
            return false;
        fseek(f, 0, SEEK_END);
        d.resize((size_t)ftell(f));
        fseek(f, 0, SEEK_SET);
        const bool ok = fread(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        return ok && d.size() > 64;
    }
    bool read(uintptr_t va, void *out, size_t n) const {
        uint64_t phoff;
        uint16_t phentsize, phnum;
        memcpy(&phoff, &d[0x20], 8);
        memcpy(&phentsize, &d[0x36], 2);
        memcpy(&phnum, &d[0x38], 2);
        for (uint16_t i = 0; i < phnum; i++) {
            const uint8_t *p = &d[phoff + (size_t)i * phentsize];
            uint32_t type;
            uint64_t off, vaddr, filesz;
            memcpy(&type, p, 4);
            memcpy(&off, p + 8, 8);
            memcpy(&vaddr, p + 16, 8);
            memcpy(&filesz, p + 32, 8);
            if (type == 1 && va >= vaddr && va + n <= vaddr + filesz) {
                memcpy(out, &d[off + (va - vaddr)], n);
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------- live
alignas(64) static volatile uint64_t g_watched[8];
alignas(64) static volatile uint64_t g_decoy[8];
static std::atomic<int> g_go{0}, g_go2{0}, g_batch1_done{0};
static std::atomic<pid_t> g_tid_writer{0}, g_tid_decoy{0}, g_tid_reader{0};

__attribute__((noinline)) static void writer_body(int n) {
    for (int i = 0; i < n; i++)
        g_watched[0] = (uint64_t)i + 1; // the store under watch
}
__attribute__((noinline)) static void writer_end() {}

// Two batches: the first is seen by the COUNTING events only, the second by
// the sampling event the upgrade puts in their place.
static void writer(int n) {
    g_tid_writer = (pid_t)syscall(SYS_gettid);
    while (!g_go.load())
        std::this_thread::yield();
    writer_body(n);
    g_batch1_done = 1;
    while (!g_go2.load())
        std::this_thread::yield();
    writer_body(n);
}
static void decoy(int n) {
    g_tid_decoy = (pid_t)syscall(SYS_gettid);
    while (!g_go.load())
        std::this_thread::yield();
    for (int i = 0; i < n; i++)
        g_decoy[0] = (uint64_t)i; // same thread count, different address
}
static void reader(int n, uint64_t *sink) {
    g_tid_reader = (pid_t)syscall(SYS_gettid);
    while (!g_go.load())
        std::this_thread::yield();
    uint64_t s = 0;
    for (int i = 0; i < n; i++)
        s += g_watched[0]; // reads must never be reported
    *sink = s;
}

int main() {
    // ---- the record layout -------------------------------------------
    {
        std::vector<uint8_t> r;
        auto put = [&](const void *p, size_t n) {
            r.insert(r.end(), (const uint8_t *)p, (const uint8_t *)p + n);
        };
        const uint64_t ip = 0x1234567, abi = 2;
        const uint32_t pid = 77, tid = 78;
        uint64_t regs[kNumRegs];
        for (int i = 0; i < kNumRegs; i++)
            regs[i] = 0x1000 + (uint64_t)i;
        const uint64_t ssize = 64, dyn = 48;
        uint8_t stack[64];
        for (int i = 0; i < 64; i++)
            stack[i] = (uint8_t)i;
        put(&ip, 8); put(&pid, 4); put(&tid, 4); put(&abi, 8);
        put(regs, sizeof regs); put(&ssize, 8); put(stack, 64); put(&dyn, 8);
        Sample s;
        const bool ok = parse_sample(r.data(), r.size(), &s);
        check(ok && s.ip == ip && s.tid == tid && s.pid == pid,
              "record: ip, pid, tid in their places");
        check(ok && s.regs[8] == 0x1008 && s.regs[16] == 0x1010,
              "record: 17 registers in ascending bit order (rip is #8)");
        check(ok && s.stack_len == 48 && s.stack[47] == 47,
              "record: stack slice kept to dyn_size");
        check(!parse_sample(r.data(), r.size() - 9, &s),
              "record: a truncated record is refused, not half-read");
    }

    // ---- anchors, against the real binary ----------------------------
    {
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        Elf elf;
        if (!elf.load(x4)) {
            printf("skip  anchors — no X4 at %s\n", x4);
        } else {
            auto rd = [&](uintptr_t va, void *o, size_t n) {
                return elf.read(va, o, n);
            };
            const Anchors a = decode_anchors(0xa41610, rd);
            check(a.ok, "anchors: decoded and signature-verified on X4 9.00");
            check(a.fetch == 0x1925bf0, "anchors: fetch function 0x1925bf0");
            check(a.g0 == 0x3e7742c && a.g1 == 0x3e77430,
                  "anchors: buffer-index globals 0x3e7742c / 0x3e77430");
            // One byte different anywhere in the prologue must refuse.
            auto bent = [&](uintptr_t va, void *o, size_t n) {
                if (!elf.read(va, o, n))
                    return false;
                if (va == 0x1925bf0 && n > 0x40)
                    ((uint8_t *)o)[0x35] ^= 0x01; // the 0x290 offset
                return true;
            };
            const Anchors b = decode_anchors(0xa41610, bent);
            check(!b.ok, "anchors: a changed prologue is REFUSED");
        }
    }

    // ---- the entry lookup, on a synthetic camera ----------------------
    {
        alignas(16) static uint8_t cam[0x1000];
        alignas(16) static uint8_t heap[0x200];
        memset(cam, 0, sizeof cam);
        memset(heap, 0xee, sizeof heap);
        static uint32_t idx0 = 1, idx1 = 0;
        Anchors a;
        a.ok = true;
        a.g0 = (uintptr_t)&idx0;
        a.g1 = (uintptr_t)&idx1;
        const uintptr_t c = (uintptr_t)cam;
        auto mem = [](uintptr_t va, void *o, size_t n) {
            memcpy(o, (const void *)va, n);
            return true;
        };
        auto put64 = [](uintptr_t va, uint64_t v) { memcpy((void *)va, &v, 8); };
        // Buffer 1: inline storage, key 0 at the SECOND entry.
        const uintptr_t b1 = c + kBufBase + 1 * kBufStride;
        put64(b1 + kCapOff, kInlineCap);
        put64(c + 1 * kBufStride + kCountOff, 2);
        put64(b1 + kDataOff + 0 * kEntryStride, 5);
        put64(b1 + kDataOff + 1 * kEntryStride, 0);
        // Buffer 0: heap storage, key 0 first.
        const uintptr_t b0 = c + kBufBase;
        put64(b0 + kCapOff, 16);
        put64(b0 + kDataOff, (uint64_t)(uintptr_t)heap);
        put64(c + kCountOff, 1);
        put64((uintptr_t)heap, 0);
        // Buffer 2: a count that no real list has -- must be skipped, not
        // walked. Buffer 3: all zero -- no entries, skipped.
        put64(c + 2 * kBufStride + kCountOff, 999);
        uint32_t n0 = 99, n1 = 99;
        const auto t = targets(c, a, mem, &n0, &n1);
        check(n0 == 1 && n1 == 0, "targets: reports the index globals");
        check(t.size() == 2, "targets: every valid buffer, garbage skipped");
        check(t.size() == 2 && t[0].buf == 0 &&
                  t[0].addr == (uintptr_t)heap + kRow0,
              "targets: heap buffer followed through its pointer");
        check(t.size() == 2 && t[1].buf == 1 &&
                  t[1].addr == b1 + kDataOff + kEntryStride + kRow0,
              "targets: inline buffer, key 0 found past a non-zero key");
    }

    // ---- hop 2: code sites against the real binary ---------------------
    {
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        Elf elf;
        if (!elf.load(x4)) {
            printf("skip  hop-2 sites — no X4 at %s\n", x4);
        } else {
            auto rd = [&](uintptr_t va, void *o, size_t n) {
                return elf.read(va, o, n);
            };
            uintptr_t bad = 0;
            check(verify_hop2_sites(rd, &bad),
                  "hop 2: every analysed code site matches X4 9.00 byte for byte");
            auto bent = [&](uintptr_t va, void *o, size_t n) {
                if (!elf.read(va, o, n))
                    return false;
                if (va == 0x191d185)
                    ((uint8_t *)o)[3] ^= 0x08; // +0xa0 -> +0xa8
                return true;
            };
            bad = 0;
            check(!verify_hop2_sites(bent, &bad) && bad == 0x191d185,
                  "hop 2: one changed byte refuses, and names the site");
        }
    }

    // ---- hop 2b: the settings-object sites --------------------------------
    {
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        Elf elf;
        if (elf.load(x4)) {
            auto rd = [&](uintptr_t va, void *o, size_t n) {
                return elf.read(va, o, n);
            };
            uintptr_t bad = 0;
            check(verify_hop2b_sites(rd, &bad),
                  "hop 2b: the settings-object sites match X4 9.00");
            check(settings_global() == 0x3db83c8,
                  "hop 2b: the global decodes to 0x3db83c8 from its instruction");
            Chain none;
            const auto t = settings_targets(0x10000, none);
            check(t.size() == 2 && t[0].addr == 0x10000 + 0x5f0 &&
                      t[1].addr == 0x10000 + 0x5e0,
                  "hop 2b: rotation row 0 at +0x5f0, translation at +0x5e0");
        }
    }

    // ---- hop 3: the camera's own transform --------------------------------
    {
        const char *x4 = "/nvme/SteamLibrary/steamapps/common/X4 Foundations/X4";
        Elf elf;
        if (elf.load(x4)) {
            auto rd = [&](uintptr_t va, void *o, size_t n) {
                return elf.read(va, o, n);
            };
            uintptr_t bad = 0;
            check(verify_hop3_sites(rd, &bad),
                  "hop 3: the camera-transform sites match X4 9.00");
            auto bent = [&](uintptr_t va, void *o, size_t n) {
                if (!elf.read(va, o, n))
                    return false;
                if (va == 0x192a165)
                    ((uint8_t *)o)[3] ^= 0x10; // +0x5b0 -> +0x5a0
                return true;
            };
            bad = 0;
            check(!verify_hop3_sites(bent, &bad) && bad == 0x192a165,
                  "hop 3: one changed byte refuses, and names the site");
        } else {
            printf("skip  hop-3 sites — no X4 at %s\n", x4);
        }
        Chain none;
        auto t = camera_local_targets(0x20000, none);
        check(t.size() == 2 && t[0].addr == 0x20000 + 0x5b0 &&
                  !strcmp(t[0].label, "camera.rot") &&
                  t[1].addr == 0x20000 + 0x5a0 && !strcmp(t[1].label, "camera.pos"),
              "hop 3: rotation row 0 at camera+0x5b0, translation at +0x5a0");
        Chain ok;
        ok.ok = true;
        ok.controller = 0x30000;
        t = camera_local_targets(0x20000, ok);
        check(t.size() == 3 && t[2].addr == 0x30000 + kStoredRow0 &&
                  !strcmp(t[2].label, "controller"),
              "hop 3: the controller's sway is the reference when verified");
        check(camera_local_targets(0, ok).empty(), "hop 3: no camera, no targets");
    }

    // ---- the census's transform checks ------------------------------------
    {
        const float id[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        const float zero[12] = {};
        check(live_transform(id, false), "census: identity is a live rotation");
        check(!live_transform(zero, false),
              "census: an all-zero read (take 318's freed controller) is NOT");
        check(std::fabs(rotation_angle_deg(zero) - 120.f) < 0.01f,
              "census: and its trace angle is exactly the 120 deg that fooled "
              "the scorer");
        const float a = 0.5f, ca = std::cos(a), sa = std::sin(a);
        const float yrot[12] = {ca, 0, -sa, 0, 0, 1, 0, 0, sa, 0, ca, 0};
        check(live_transform(yrot, false) &&
                  std::fabs(rotation_angle_deg(yrot) - a * 57.29578f) < 0.01f,
              "census: a 28.6 deg yaw reads as 28.6 deg");
        const float refl[12] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        check(!live_transform(refl, false), "census: a reflection (det -1) is NOT");
        const float pos[12] = {12.f, -3.f, 400.f, 0, 1, 0, 0, 0, 0, 1, 0, 0};
        check(live_transform(pos, true),
              "census: a translation followed by two unit rows is live");
        const float posz[12] = {12.f, -3.f, 400.f, 0};
        check(!live_transform(posz, true),
              "census: a translation followed by zeros is NOT (freed object)");
        // The camera's angle from yaw/pitch, against the matrix it describes:
        // Ry(y) * Rx(p) built explicitly, both orders give the same angle.
        const float y = 1.1f, p = -0.4f;
        const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p),
                    sp = std::sin(p);
        const float ry_rx[12] = {cy, sy * sp, sy * cp, 0, 0, cp, -sp, 0,
                                 -sy, cy * sp, cy * cp, 0};
        const float rx_ry[12] = {cy, 0, sy, 0, sp * sy, cp, -sp * cy, 0,
                                 -cp * sy, sp, cp * cy, 0};
        check(std::fabs(yaw_pitch_angle_deg(y, p) - rotation_angle_deg(ry_rx)) <
                      0.01f &&
                  std::fabs(yaw_pitch_angle_deg(y, p) -
                            rotation_angle_deg(rx_ry)) < 0.01f,
              "census: the camera's angle from yaw/pitch equals the matrix's, "
              "in either composition order");
        check(std::fabs(yaw_pitch_angle_deg(3.14159265f, 0.f) - 180.f) < 0.01f,
              "census: looking straight back is 180 deg");
    }

    // ---- hop 2: the chain, on a synthetic controller --------------------
    {
        alignas(16) static uint8_t cam[0x100], ctl[0x200], vt_ctl[0x200];
        alignas(16) static uint8_t kids[4][0x80], vt_kid[4][0x200];
        alignas(16) static uint64_t vec[4];
        memset(cam, 0, sizeof cam);
        auto put64 = [](void *p, uint64_t v) { memcpy(p, &v, 8); };
        put64(cam + kControllerOff, (uint64_t)(uintptr_t)ctl);
        put64(ctl, (uint64_t)(uintptr_t)vt_ctl);
        put64(vt_ctl + kVtLocal, kLocalTransformFn);
        for (int i = 0; i < 4; i++) {
            put64(kids[i], (uint64_t)(uintptr_t)vt_kid[i]);
            put64(vt_kid[i] + kVtCompose, 0x1918640 + (uint64_t)i);
            put64(vt_kid[i] + kVtLocal, 0x1920000 + (uint64_t)i);
            vec[i] = (uint64_t)(uintptr_t)kids[i];
        }
        put64(ctl + kChainBegin, (uint64_t)(uintptr_t)&vec[0]);
        put64(ctl + kChainEnd, (uint64_t)(uintptr_t)&vec[4]);
        auto mem = [](uintptr_t va, void *o, size_t n) {
            memcpy(o, (const void *)va, n);
            return true;
        };
        const Chain c = read_chain((uintptr_t)cam, mem);
        check(c.ok && c.children.size() == 4, "chain: four children read");
        check(c.ok && c.children[2].compose == 0x1918642 &&
                  c.children[2].local == 0x1920002,
              "chain: each child's vtable targets recorded (build constants)");
        const auto t = chain_targets(c);
        check(t.size() == 4, "chain: controller + first THREE children (4 slots)");
        check(t.size() == 4 && t[0].addr == (uintptr_t)ctl + kStoredRow0 &&
                  !strcmp(t[0].label, "controller"),
              "chain: target 0 is the controller's stored row 0");
        check(t.size() == 4 && t[3].addr == (uintptr_t)kids[2] + kStoredRow0 &&
                  !strcmp(t[3].label, "child2"),
              "chain: target 3 is child2's stored row 0");
        // A different controller class must refuse.
        put64(vt_ctl + kVtLocal, 0x1234567);
        const Chain d = read_chain((uintptr_t)cam, mem);
        check(!d.ok && chain_targets(d).empty(),
              "chain: a controller whose vtable+0x1b0 is not 0x191d0f0 REFUSES");
        put64(vt_ctl + kVtLocal, kLocalTransformFn);
        // An implausible vector (end before begin) must refuse.
        put64(ctl + kChainEnd, (uint64_t)(uintptr_t)&vec[0] - 8);
        const Chain e = read_chain((uintptr_t)cam, mem);
        check(!e.ok, "chain: an end-before-begin vector REFUSES");
    }

    // ---- live: the kernel, a real watchpoint, three threads ------------
    //
    // As the injector runs it: COUNTING events on every thread, then only the
    // thread whose counter moved is upgraded to SAMPLING.
    {
        const int n = 1000;
        uint64_t sink = 0;
        std::thread tw(writer, n), td(decoy, n), tr(reader, n, &sink);
        while (!g_tid_writer || !g_tid_decoy || !g_tid_reader)
            std::this_thread::yield();
        WriteWatch w;
        const Target tgt{(uintptr_t)&g_watched[0], 0};
        int err = 0;
        for (pid_t t : {g_tid_writer.load(), g_tid_decoy.load(),
                        g_tid_reader.load()})
            if (int e = w.arm(t, tgt, false))
                err = e;
        if (err) {
            g_go = 1;
            g_go2 = 1;
            tw.join(); td.join(); tr.join();
            printf("SKIP  live watchpoint — perf_event_open refused (%s). "
                   "The camera probe cannot run on this kernel either.\n",
                   strerror(err));
        } else {
            g_go = 1;
            td.join(); tr.join();
            while (!g_batch1_done.load())
                std::this_thread::yield();
            check(w.sampling_events() == 0,
                  "live: counting events carry no ring (no locked memory)");
            check(w.stores() == (uint64_t)n,
                  "live: counting sees the writer's first batch exactly");
            const auto up = w.writers_to_upgrade();
            check(up.size() == 1 && up[0].first == g_tid_writer.load(),
                  "live: only the WRITER is picked for upgrade (not decoy, "
                  "not reader)");
            for (const auto &p : up)
                check(w.upgrade(p.first, p.second) == 0,
                      "live: upgrade to a sampling event succeeds");
            check(w.sampling_events() == 1, "live: exactly one ring exists");
            g_go2 = 1;
            tw.join();
            uint64_t caught = 0, wrong_tid = 0, wrong_ip = 0, rip_mismatch = 0,
                     stackless = 0;
            const uint64_t lo = (uint64_t)(uintptr_t)&writer_body;
            const uint64_t hi = (uint64_t)(uintptr_t)&writer_end + 64;
            w.poll([&](const Sample &s, const Target &) {
                caught++;
                if ((pid_t)s.tid != g_tid_writer.load())
                    wrong_tid++;
                if (s.ip < lo || s.ip >= hi)
                    wrong_ip++;
                if (s.regs[8] != s.ip)
                    rip_mismatch++;
                if (!s.stack_len)
                    stackless++;
            });
            const uint64_t stores = w.stores();
            printf("      counted %llu store(s), sampled %llu, LOST records "
                   "%llu\n",
                   (unsigned long long)stores, (unsigned long long)caught,
                   (unsigned long long)w.lost());
            // The counters are exact; the samples are what fit. The first
            // draft compared samples to n, read 74 of 1000 with "lost 0", and
            // would have made the probe report a burst as a trickle.
            check(stores == (uint64_t)(2 * n),
                  "live: the count stays exact across the upgrade (2 batches)");
            check(caught >= 1 && caught <= (uint64_t)n,
                  "live: samples come only from the second batch");
            check(wrong_tid == 0, "live: nothing from the decoy or the reader");
            check(wrong_ip == 0, "live: the ip is inside the writer's code");
            check(rip_mismatch == 0, "live: sampled rip agrees with the ip");
            check(stackless == 0, "live: every sample carries a stack slice");
        }
    }

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
