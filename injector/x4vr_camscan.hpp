// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_camscan.hpp — where does X4 keep its free-look angle?
//
// **Why this exists.** Head-look drives X4's camera by holding MOUSELOOK and
// feeding synthetic mouse motion, because X4 has no camera setter in its C
// exports or either Lua bridge (docs/x4-quirks.md, "The Lua route is closed
// for the camera"). Holding MOUSELOOK is also what kills cockpit hover (takes
// 312/313), so the cursor and head-look cannot coexist through that channel.
// Patola asked for the camera to be moved "by our own means".
//
// The survey that closed the door looked for SETTER FUNCTIONS. It never looked
// at the state itself. GetCameraRotation reads its object through a global,
// takes the camera node at +0x3e8, fetches that node's rotation matrix and
// derives yaw/pitch/roll with atan2/asin -- so the free-look angle lives in
// X4's camera controller, rebuilt into that matrix every frame. If the
// controller keeps the angle as numbers, writing them is a camera setter.
//
// This is the READ-ONLY first step: find those numbers. Every sample it takes
// the angle X4 reports and the bytes of the objects around the camera, and
// keeps the slots whose value tracks the angle -- in radians or degrees, either
// sign, float or double -- across many DIFFERENT angles. A slot that matches
// once is a coincidence; one that matches at thirty distinct angles is state.
//
// Pure: regions arrive as byte buffers, so tests/camscan.cpp drives every
// verdict without X4. The X4 side (which objects, and reading them without
// crashing) lives in x4vr_inject.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace x4vr {
namespace camscan {

struct Region {
    std::string name;             // a PATH ("cam", "cam->[0x58]"), stable
    std::vector<uint8_t> bytes;   // empty when unreadable this sample
};

struct Cand {
    uint16_t region = 0;
    uint32_t off = 0;
    uint8_t axis = 0;    // 0 yaw, 1 pitch
    uint8_t deg = 0;     // 0 radians, 1 degrees
    int8_t sign = 1;
    uint8_t dbl = 0;     // 0 float, 1 double
    uint32_t hits = 0, misses = 0;
    uint32_t distinct = 0;   // hits at a whole-degree bin unlike the last
    int32_t last_bin = 0x7fffffff;
};

// Tolerance in the value's own unit. GetCameraRotation derives the angle from
// a matrix with atan2/asin, so a stored angle matches it to float round-off;
// 0.05 degree absolute is generous against that and still far below the
// one-degree steps that make a hit "distinct".
inline bool near_angle(double x, double want, bool deg) {
    const double tol = deg ? 0.05 : 0.05 / 57.29577951308232;
    return std::fabs(x - want) <= tol;
}

class Scanner {
public:
    // Minimum |angle| for a DISCOVERY sample: at small angles too many
    // unrelated floats sit near zero, and zero itself matches everything.
    static constexpr double kDiscoverRad = 5.0 / 57.29577951308232;
    static constexpr size_t kMaxCands = 8192;

    // One sample: the angle X4 reported (radians) and the regions as read now.
    // `regions` must be in the same order, with the same names, every call.
    void observe(const std::vector<Region> &regions, double yaw, double pitch) {
        samples_++;
        yaw_lo_ = std::fmin(yaw_lo_, yaw);
        yaw_hi_ = std::fmax(yaw_hi_, yaw);
        pitch_lo_ = std::fmin(pitch_lo_, pitch);
        pitch_hi_ = std::fmax(pitch_hi_, pitch);
        if (names_.empty())
            for (const Region &r : regions)
                names_.push_back(r.name);

        // PER AXIS. Discovery used to run once, at the first sample where
        // either axis left +-5 degrees -- and a player who turns before he
        // nods then had pitch at ~0 at that instant, so no pitch slot was
        // ever collected and none could be found (tests/camscan.cpp caught
        // it). Each axis is discovered when IT first becomes distinctive, and
        // again if all of its candidates die.
        const double v[2] = {yaw, pitch};
        bool fresh[2] = {false, false};
        for (uint8_t axis = 0; axis < 2; axis++) {
            size_t live = 0;
            for (const Cand &c : cands_)
                live += c.axis == axis;
            if (live == 0 && std::fabs(v[axis]) >= kDiscoverRad) {
                if (had_axis_[axis])
                    emptied_++;
                discover(regions, axis, v[axis]);
                had_axis_[axis] = true;
                fresh[axis] = true;
            }
        }
        for (Cand &c : cands_) {
            if (fresh[c.axis])
                continue; // just discovered from this very sample
            if (c.region >= regions.size())
                continue;
            const std::vector<uint8_t> &b = regions[c.region].bytes;
            const size_t w = c.dbl ? 8 : 4;
            if (c.off + w > b.size())
                continue; // unreadable this sample: neither hit nor miss
            const double x = read(b, c.off, c.dbl);
            const double want =
                c.sign * v[c.axis] * (c.deg ? 57.29577951308232 : 1.0);
            if (near_angle(x, want, c.deg)) {
                c.hits++;
                const int32_t bin =
                    (int32_t)std::lround(v[c.axis] * 57.29577951308232);
                if (bin != c.last_bin) {
                    c.distinct++;
                    c.last_bin = bin;
                }
            } else {
                c.misses++;
            }
        }
        // Prune: a slot that has missed more than it could plausibly miss by
        // timing alone is not the angle. Keeps the check cheap as it runs.
        size_t k = 0;
        for (const Cand &c : cands_)
            if (!(c.misses > 3 && c.misses > c.hits / 8))
                cands_[k++] = c;
        eliminated_ += cands_.size() - k;
        cands_.resize(k);
    }

    // Survivors worth reporting: many distinct angles, few misses.
    std::vector<Cand> survivors(uint32_t min_distinct = 5) const {
        std::vector<Cand> out;
        for (const Cand &c : cands_)
            if (c.distinct >= min_distinct)
                out.push_back(c);
        // Most distinct first; ties by fewest misses.
        for (size_t i = 1; i < out.size(); i++)
            for (size_t j = i; j > 0 && better(out[j], out[j - 1]); j--)
                std::swap(out[j], out[j - 1]);
        return out;
    }

    std::string describe(const Cand &c) const {
        char b[256];
        snprintf(b, sizeof b,
                 "%s+0x%x %s %s%s %s — %u hit(s) at %u distinct angle(s), "
                 "%u miss(es)",
                 c.region < names_.size() ? names_[c.region].c_str() : "?",
                 c.off, c.axis ? "pitch" : "yaw", c.sign < 0 ? "-" : "+",
                 c.deg ? "deg" : "rad", c.dbl ? "double" : "float", c.hits,
                 c.distinct, c.misses);
        return b;
    }

    uint64_t samples() const { return samples_; }
    uint64_t discoveries() const { return discoveries_; }
    uint64_t eliminated() const { return eliminated_; }
    uint64_t emptied() const { return emptied_; }
    bool capped() const { return capped_; }
    size_t alive() const { return cands_.size(); }
    // The range the camera actually covered: a search that never saw the
    // angle leave +-5 degrees had nothing to find, and must say so.
    double yaw_span_deg() const {
        return samples_ ? (yaw_hi_ - yaw_lo_) * 57.29577951308232 : 0.0;
    }
    double pitch_span_deg() const {
        return samples_ ? (pitch_hi_ - pitch_lo_) * 57.29577951308232 : 0.0;
    }

private:
    static double read(const std::vector<uint8_t> &b, size_t off, bool dbl) {
        if (dbl) {
            double d;
            memcpy(&d, b.data() + off, 8);
            return d;
        }
        float f;
        memcpy(&f, b.data() + off, 4);
        return f;
    }

    static bool better(const Cand &a, const Cand &b) {
        if (a.distinct != b.distinct)
            return a.distinct > b.distinct;
        return a.misses < b.misses;
    }

    void discover(const std::vector<Region> &regions, uint8_t axis,
                  double value) {
        discoveries_++;
        for (uint16_t r = 0; r < regions.size(); r++) {
            const std::vector<uint8_t> &b = regions[r].bytes;
            for (uint8_t dbl = 0; dbl < 2; dbl++) {
                const size_t w = dbl ? 8 : 4;
                for (size_t off = 0; off + w <= b.size(); off += w) {
                    const double x = read(b, off, dbl);
                    if (!std::isfinite(x))
                        continue;
                    {
                        for (uint8_t deg = 0; deg < 2; deg++)
                            for (int8_t sign = -1; sign <= 1; sign += 2) {
                                const double want =
                                    sign * value *
                                    (deg ? 57.29577951308232 : 1.0);
                                if (!near_angle(x, want, deg))
                                    continue;
                                if (cands_.size() >= kMaxCands) {
                                    capped_ = true;
                                    return;
                                }
                                Cand c;
                                c.region = r;
                                c.off = (uint32_t)off;
                                c.axis = axis;
                                c.deg = deg;
                                c.sign = sign;
                                c.dbl = dbl;
                                c.hits = 1;
                                c.distinct = 1;
                                c.last_bin = (int32_t)std::lround(
                                    value * 57.29577951308232);
                                cands_.push_back(c);
                            }
                    }
                }
            }
        }
    }

    std::vector<Cand> cands_;
    std::vector<std::string> names_;
    uint64_t samples_ = 0, discoveries_ = 0, eliminated_ = 0, emptied_ = 0;
    bool capped_ = false;
    bool had_axis_[2] = {false, false};
    double yaw_lo_ = 1e9, yaw_hi_ = -1e9, pitch_lo_ = 1e9, pitch_hi_ = -1e9;
};

} // namespace camscan
} // namespace x4vr
