// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// X4VR_FOOTLOG=1 -- what X4's player camera does OUT OF THE PILOT SEAT.
//
// Head-look writes the cockpit's free-look matrix (camera+0x5b0) and refuses
// any camera mode but 0 (injector/x4vr_camwrite.hpp), so on foot nothing
// follows the head. 0x1933d00 has paths for modes 0 (cockpit), 1 (no angle
// update at all: the stored transform is only re-based and composed), 2/3 and
// 4 (orbiting cameras on the external parameters) -- and which one walking
// uses, and where the walking view's yaw and pitch live, is not decidable from
// the code alone. This is the read-only census that decides it:
//
//   * a TRANSITION line whenever the camera object, its mode, its attached
//     object (+0x780) or whether X4 says the player pilots / occupies a ship
//     changes, with every value beside it;
//   * a WINDOW line each second: per matrix (GetCameraRotation, the camera's
//     local transform +0x5a0.., its base +0x6f0..) the NET and TRAVEL of yaw
//     and pitch, any rotation at all (geodesic, so roll is not missed), the
//     translation's displacement -- beside the player's own mouse motion and
//     the head's. Whichever matrix moves with the mouse on foot is where the
//     walking view lives; the head's column is the control that nothing
//     follows it there yet.
//
// Nothing is written. Self-contained so tests/foot.cpp can check it.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x4vr::foot {

constexpr uintptr_t kCamAttached = 0x780; // what the camera hangs from
constexpr uintptr_t kCamAttachedRef = 0x788;
constexpr uintptr_t kCamLocal = 0x5a0; // translation; rows from +0x5b0
constexpr uintptr_t kCamBase = 0x6f0;  // translation; rows from +0x700
constexpr uintptr_t kCamMode = 0x880;

inline float wrap180(float d) {
    while (d > 180.f)
        d -= 360.f;
    while (d < -180.f)
        d += 360.f;
    return d;
}

// The inverse of camwrite::rows_for, in head-look's convention (yaw
// left-positive = the NEGATED GetCameraRotation yaw, pitch up-positive): the
// yaw and pitch of row 2, the forward axis. `rows` is 12 floats, 4 per row.
// A row that is not a unit vector (freed memory, a camera mid-teardown) is
// refused rather than read as an angle.
inline bool head_angles_of_rows(const float *rows, float *yaw_deg,
                                float *pitch_deg) {
    const float x = rows[8], y = rows[9], z = rows[10];
    const float len2 = x * x + y * y + z * z;
    if (!(len2 > 0.98f && len2 < 1.02f))
        return false;
    constexpr float r2d = 57.2957795130823f;
    const float yc = y > 1.f ? 1.f : (y < -1.f ? -1.f : y);
    *yaw_deg = -std::atan2(x, z) * r2d;
    *pitch_deg = std::asin(yc) * r2d;
    return true;
}

// The angle of the rotation taking `a` to `b` (both 12-float row sets), from
// the trace of a^T b -- any motion, roll included, which yaw/pitch alone miss.
inline float delta_angle_deg(const float *a, const float *b) {
    float tr = 0.f;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            tr += a[r * 4 + c] * b[r * 4 + c];
    float cs = 0.5f * (tr - 1.f);
    cs = cs > 1.f ? 1.f : (cs < -1.f ? -1.f : cs);
    return std::acos(cs) * 57.2957795130823f;
}

// One sample, taken on X4's thread.
struct Sample {
    uintptr_t cam = 0, attached = 0, attached_vt = 0;
    int32_t mode = -1;
    bool piloting = false, occupied = false; // GetPlayer{Controlled,Occupied}ShipID != 0
    int view = 0;                            // X4View as an int
    bool hud = false, relative = false;
    bool rot_ok = false, local_ok = false, base_ok = false, head_ok = false;
    float rot_yaw = 0.f, rot_pitch = 0.f, rot_roll = 0.f; // head-look convention
    float local[16] = {}, base[16] = {};                   // translation, then 3 rows
    float head_yaw = 0.f, head_pitch = 0.f;
};

// What a transition line is keyed on.
inline bool same_regime(const Sample &a, const Sample &b) {
    return a.cam == b.cam && a.mode == b.mode && a.attached == b.attached &&
           a.piloting == b.piloting && a.occupied == b.occupied;
}

// Yaw/pitch net and travel of one angle source over a window.
struct Track {
    bool have = false;
    float yaw = 0.f, pitch = 0.f; // last value
    float net_yaw = 0.f, net_pitch = 0.f, travel_yaw = 0.f, travel_pitch = 0.f;
    void step(float y, float p) {
        if (have) {
            const float dy = wrap180(y - yaw), dp = p - pitch;
            net_yaw += dy;
            net_pitch += dp;
            travel_yaw += std::fabs(dy);
            travel_pitch += std::fabs(dp);
        }
        have = true;
        yaw = y;
        pitch = p;
    }
    void restart() { net_yaw = net_pitch = travel_yaw = travel_pitch = 0.f; }
};

// A matrix source: its angles, any rotation at all, its translation's
// displacement within the window. Rows carry over a window boundary, so a
// step is never lost between windows; the translation is measured from the
// window's first sample.
struct MatTrack {
    Track ang;
    bool have = false, pos0_set = false;
    float rows[12] = {}, pos0[3] = {};
    float rot_travel = 0.f, pos_max = 0.f;
    int moved = 0; // samples whose rotation changed by more than 0.01 deg
    void step(const float *m) {
        const float *r = m + 4;
        float y = 0.f, p = 0.f;
        if (head_angles_of_rows(r, &y, &p))
            ang.step(y, p);
        if (have) {
            const float d = delta_angle_deg(rows, r);
            rot_travel += d;
            if (d > 0.01f)
                moved++;
        }
        if (!pos0_set) {
            for (int i = 0; i < 3; i++)
                pos0[i] = m[i];
            pos0_set = true;
        }
        const float dx = m[0] - pos0[0], dy = m[1] - pos0[1], dz = m[2] - pos0[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > pos_max)
            pos_max = dist;
        for (int i = 0; i < 12; i++)
            rows[i] = r[i];
        have = true;
    }
    void restart() {
        ang.restart();
        rot_travel = pos_max = 0.f;
        moved = 0;
        pos0_set = false;
    }
};

// One second of samples.
struct Window {
    double t0 = -1.0;
    int n = 0;
    Sample last;
    Track rot, head;
    MatTrack local, base;
    double mouse_dx = 0.0, mouse_dy = 0.0; // what X4 received
    uint64_t mouse_n = 0, mouse_dropped = 0; // motion events: received, zeroed by us
    uint64_t mouse_pitch_zeroed = 0;         // events whose pitch alone we zeroed

    void add(double now, const Sample &s) {
        if (t0 < 0.0)
            t0 = now;
        n++;
        last = s;
        if (s.rot_ok)
            rot.step(s.rot_yaw, s.rot_pitch);
        if (s.head_ok)
            head.step(s.head_yaw, s.head_pitch);
        if (s.local_ok)
            local.step(s.local);
        if (s.base_ok)
            base.step(s.base);
    }
    // A camera change mid-window would read as one huge step.
    void forget_matrices() {
        local = MatTrack{};
        base = MatTrack{};
        rot.have = false;
    }
    void restart(double now) {
        t0 = now;
        n = 0;
        rot.restart();
        head.restart();
        local.restart();
        base.restart();
        mouse_dx = mouse_dy = 0.0;
        mouse_n = mouse_dropped = mouse_pitch_zeroed = 0;
    }
};

inline const char *view_name(int v) {
    switch (v) {
    case 0: return "cockpit";
    case 1: return "external";
    case 2: return "floating";
    case 3: return "paused";
    default: return "?";
    }
}

// The window line. score_run.py's `foot` block parses it: keep the two in
// step (tests/foot.cpp checks the shape, tests/run-census-scorer.sh the
// parse).
inline int format_window(char *out, size_t cap, const Window &w, double secs) {
    const Sample &s = w.last;
    auto t = [](const Track &k, char *o, size_t c) {
        if (!k.have)
            return snprintf(o, c, "-");
        return snprintf(o, c, "net %.2f %.2f travel %.2f %.2f at %.1f %.1f",
                        k.net_yaw, k.net_pitch, k.travel_yaw, k.travel_pitch,
                        k.yaw, k.pitch);
    };
    auto m = [&t](const MatTrack &k, char *o, size_t c) {
        if (!k.have)
            return snprintf(o, c, "-");
        char a[160];
        t(k.ang, a, sizeof a);
        return snprintf(o, c, "%s rot %.2f moved %d pos %.3f", a, k.rot_travel,
                        k.moved, k.pos_max);
    };
    char rot[160], head[160], loc[224], bas[224];
    t(w.rot, rot, sizeof rot);
    t(w.head, head, sizeof head);
    m(w.local, loc, sizeof loc);
    m(w.base, bas, sizeof bas);
    return snprintf(out, cap,
                    "foot: window %.2f s n %d mode %d piloting %d occupied %d "
                    "view %s hud %d rel %d | mouse n %llu dx %.0f dy %.0f "
                    "dropped %llu pitchzero %llu | head %s | rot %s | local %s | "
                    "base %s",
                    secs, w.n, (int)s.mode, (int)s.piloting, (int)s.occupied,
                    view_name(s.view), (int)s.hud, (int)s.relative,
                    (unsigned long long)w.mouse_n, w.mouse_dx, w.mouse_dy,
                    (unsigned long long)w.mouse_dropped,
                    (unsigned long long)w.mouse_pitch_zeroed, head,
                    rot, loc, bas);
}

// The transition line, same contract as format_window.
inline int format_regime(char *out, size_t cap, const Sample &s) {
    float ly = 0.f, lp = 0.f, by = 0.f, bp = 0.f;
    const bool la = head_angles_of_rows(s.local + 4, &ly, &lp);
    const bool ba = head_angles_of_rows(s.base + 4, &by, &bp);
    return snprintf(
        out, cap,
        "foot: regime camera %p mode %d attached %p (vtable %p) | piloting %d "
        "occupied %d | view %s hud %d rel %d | rot %.2f %.2f %.2f | local pos "
        "%.3f %.3f %.3f yaw/pitch %s%.2f %.2f | base pos %.3f %.3f %.3f "
        "yaw/pitch %s%.2f %.2f rows (%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f "
        "%.3f %.3f)",
        (void *)s.cam, (int)s.mode, (void *)s.attached, (void *)s.attached_vt,
        (int)s.piloting, (int)s.occupied, view_name(s.view), (int)s.hud,
        (int)s.relative, s.rot_yaw, s.rot_pitch, s.rot_roll, s.local[0],
        s.local[1], s.local[2], la ? "" : "(not a rotation) ", ly, lp, s.base[0],
        s.base[1], s.base[2], ba ? "" : "(not a rotation) ", by, bp, s.base[4],
        s.base[5], s.base[6], s.base[8], s.base[9], s.base[10], s.base[12],
        s.base[13], s.base[14]);
}

// ---- X4VR_HEADLOOK_FOOT: the patch B needs (Patola, 2026-10-27: "Yes,
// patch for B approved") ----------------------------------------------------
//
// 0x1929f70, the per-frame evaluation of a camera, composes the attached
// object, then the controller (+0x18), then the camera's own transform
// (+0x5a0..+0x5d0, head-look's free-look). For the PLAYER's camera, when the
// player's chain has a type-0x16 element and 0x1628210() is true -- walking --
// it returns at 0x192a2f3 (`jne 0x192a216`) before either: our write is
// dropped there (takes 334/335). The patch retargets that ONE jne to
// 0x192a284, X4's own tail that reloads the attached object's transform and
// composes ONLY the camera's own (the path modes 1 and 6 take). Walking then
// renders player x head; the controller (the cockpit's sway chain) stays out,
// and in the cockpit, where the branch is not taken, nothing changes.
// Four bytes, the jne's rel32; every byte around them verified first.
struct CodeBytes {
    uintptr_t va;
    uint8_t n;
    uint8_t bytes[40];
};
// 0x192a2dd..0x192a302: call 0x1de5df0; test; je; call 0x1de5df0;
// call 0x1628210; test %al,%al; jne 0x192a216; mov 0x18(%rbx),%r12; jmp.
inline constexpr CodeBytes kSkipBranch = {
    0x192a2dd, 37,
    {0xe8, 0x0e, 0xbb, 0x4b, 0x00, 0x48, 0x85, 0xc0, 0x74, 0x12, 0xe8, 0x04,
     0xbb, 0x4b, 0x00, 0xe8, 0x1f, 0xdf, 0xcf, 0xff, 0x84, 0xc0, 0x0f, 0x85,
     0x1d, 0xff, 0xff, 0xff, 0x4c, 0x8b, 0x63, 0x18, 0xe9, 0x32, 0xfd, 0xff,
     0xff}};
// 0x192a284: movaps 0x10/0x20/0x30/0x0(%rbp) -> xmm6/7/5/8; jmp 0x192a15e.
inline constexpr CodeBytes kLocalTail = {
    0x192a284, 22,
    {0x0f, 0x28, 0x75, 0x10, 0x0f, 0x28, 0x7d, 0x20, 0x0f, 0x28, 0x6d, 0x30,
     0x44, 0x0f, 0x28, 0x45, 0x00, 0xe9, 0xc4, 0xfe, 0xff, 0xff}};
// 0x192a15e: movaps 0x5a0/0x5b0/0x5c0/0x5d0(%rbx) -- the camera's own
// transform, composed onto the attached object's.
inline constexpr CodeBytes kLocalCompose = {
    0x192a15e, 28,
    {0x0f, 0x28, 0x9b, 0xa0, 0x05, 0x00, 0x00, 0x0f, 0x28, 0x93, 0xb0, 0x05,
     0x00, 0x00, 0x0f, 0x28, 0x8b, 0xc0, 0x05, 0x00, 0x00, 0x0f, 0x28, 0x83,
     0xd0, 0x05, 0x00, 0x00}};
constexpr uintptr_t kSkipJne = 0x192a2f3;   // 0f 85 <rel32>
constexpr uintptr_t kSkipJneEnd = 0x192a2f9;
constexpr uintptr_t kSkipRel32 = kSkipJne + 2;
constexpr uintptr_t kSkipTarget = 0x192a216; // return: nothing composed
constexpr uintptr_t kLocalTailVa = 0x192a284;

inline int32_t rel32_to(uintptr_t insn_end, uintptr_t target) {
    return (int32_t)(int64_t)(target - insn_end);
}

// Every byte the patch depends on, as X4 9.00 has them. `*bad` names the
// first site that differs.
template <class Reader> bool verify_skip_sites(Reader read, uintptr_t *bad) {
    const CodeBytes *sites[] = {&kSkipBranch, &kLocalTail, &kLocalCompose};
    for (const CodeBytes *c : sites) {
        uint8_t b[40];
        if (!read(c->va, b, c->n)) {
            *bad = c->va;
            return false;
        }
        for (uint8_t i = 0; i < c->n; i++)
            if (b[i] != c->bytes[i]) {
                *bad = c->va + i;
                return false;
            }
    }
    // The decoded targets, not just the bytes: the jne must land on the
    // return, the tail's jmp on the compose.
    int32_t r = 0;
    memcpy(&r, kSkipBranch.bytes + (kSkipRel32 - kSkipBranch.va), 4);
    if (kSkipJneEnd + (intptr_t)r != kSkipTarget) {
        *bad = kSkipJne;
        return false;
    }
    memcpy(&r, kLocalTail.bytes + 18, 4);
    if (kLocalTail.va + 22 + (intptr_t)r != kLocalCompose.va) {
        *bad = kLocalTail.va + 17;
        return false;
    }
    return true;
}

} // namespace x4vr::foot
