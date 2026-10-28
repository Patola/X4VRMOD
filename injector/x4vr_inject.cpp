// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// libx4vr_inject.so — LD_PRELOAD injector.
//
// Phase 1: give X4 its own settings file when running under the mod.
// config.xml is forked once into config-x4vrmod.xml; from then on every read
// is answered from the profile (with our overrides applied in memory) and
// every write is redirected into it, so the player's settings file on disk
// is never modified. See injector/README.md and x4vr_config.hpp.
//
// X4 both reads and writes the file with fopen() (confirmed live), so that
// is the primary hook; the open()/openat() family is interposed for
// observation, and redirects writes as a safety net in case a future patch
// changes the access path.
//
// Later phases add LuaJIT FFI and SDL hooks.

#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <set>
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>

extern char **environ;

#define X4VR_LOG_TAG "inject"
#include "../common/x4vr_log.hpp"
#include "../common/x4vr_immortal.hpp"
#include "../common/x4vr_env.hpp"
#include "../common/x4vr_share.hpp"
#include "../common/x4vr_steer.hpp"
#include "x4vr_foot.hpp"
#include "x4vr_menus.hpp"
#include "x4vr_config.hpp"
#include "x4vr_inputmap.hpp"
#include "../common/x4vr_camring.hpp"
#include "../common/x4vr_headlook.hpp"
#include "../common/x4vr_plantid.hpp"
#include "x4vr_riprel.hpp"
#include "x4vr_camscan.hpp"
#include "x4vr_camwatch.hpp"
#include "x4vr_camwrite.hpp"
#include "x4vr_shadowfov.hpp"
#include "x4vr_fieldab.hpp"
#include "x4vr_csmfit.hpp"
#include "x4vr_content.hpp"
#include "x4vr_lua.hpp"
#include "../common/x4vr_shquote.hpp"

namespace {

bool is_config_xml(const char *path) {
    return path && strstr(path, "config.xml");
}

// Logging only, and deliberately broader than x4vr::is_x4_inputmap: reads are
// served from fopen() alone, so if X4 opens its inputmap through open() or
// openat() instead the rebind would silently never happen. The log has to be
// able to distinguish that from "we rebound it and it did not take".
bool is_inputmap_path(const char *path) {
    return path && strstr(path, "inputmap");
}

// X4VR_FILEPROBE -- task #68: which door does X4 knock on?
//
// X4's free-look clamp is `<lookaround><internal maxpitch="35" maxyaw="65">`
// in libraries/parameters.xml, which lives INSIDE 08.cat. The injector serves
// config.xml and inputmap.xml because X4 opens those as loose files; an
// archive member is not reachable that way, and building a delivery route
// before knowing which paths X4 actually tries would be guessing.
//
// So this records the attempts instead. Three candidate doors:
//
//   <userdir>/parameters.xml   X4 already reads a user-level override of
//                              libraries/camerasettings.xml (the player has
//                              one), so it may do the same here. If it does,
//                              the EXISTING fopen path serves it and #68 is
//                              nearly free.
//   extensions/*/libraries/…   the mod route. Real and already enumerated --
//                              the install has DLCs and workshop mods -- but
//                              an unsigned extension risks the save's
//                              modified="0", which is not ours to spend.
//   *.cat / *.dat              archive reads, the fallback that means owning
//                              X4's container format.
//
// Deduped and capped: X4 opens thousands of files and a flood would cost the
// take it rides on. Nothing is served differently -- this only watches.
bool file_probe_on() {
    static const bool on = [] {
        const char *e = getenv("X4VR_FILEPROBE");
        return e && *e && *e != '0';
    }();
    return on;
}

void file_probe(const char *how, const char *path) {
    if (!file_probe_on() || !path)
        return;
    const bool interesting =
        strstr(path, "parameters.xml") || strstr(path, "camerasettings") ||
        strstr(path, "content.xml") || strstr(path, "extensions/") ||
        strstr(path, ".cat") || strstr(path, ".dat");
    if (!interesting)
        return;
    static std::mutex mu;
    static std::set<std::string> &seen = x4vr::immortal<std::set<std::string>>();
    std::lock_guard<std::mutex> lock(mu);
    if (seen.size() >= 400)
        return;
    if (!seen.insert(std::string(how) + " " + path).second)
        return;
    X4VR_LOG("fileprobe: %s(%s)", how, path);
    if (seen.size() == 400)
        X4VR_LOG("fileprobe: 400 distinct paths — capping, so this run says "
                 "nothing about anything X4 opened after this point");
}

// ---- X4VR_CATPATCH: substitute archive bytes on the way out of read() ------
//
// Task #68. X4's free-look clamp lives in libraries/parameters.xml INSIDE
// 08.cat/08.dat, and take 204 proved that raising it works -- via an
// extension, which costs the savegame's `modified` flag. That flag exists to
// stop mods that give an advantage from being equated with vanilla play. This
// mod gives none, is Linux-only and niche, and Egosoft will not sign anything
// built on an injector plus a Vulkan layer, so there is no legitimate way to
// carry the change without tainting every user's saves.
//
// So: leave the archive alone on disk and substitute the bytes as X4 reads
// them. The substitution is the SAME LENGTH -- `maxpitch="35" maxyaw="65"` ->
// `maxpitch="70" maxyaw="85"` -- so no offset shifts, no .cat index rewrite,
// and every other member stays exactly where it was.
//
// **Whether this is enough is the open question, and the reason it is a knob
// rather than a default.** 08.cat carries an MD5 per member, and 08_sig.dat
// carries a .sig blob per file. If X4 verifies at load and hashes what it
// received, it will hash OUR bytes and mismatch. That is why the table can
// carry the .cat's MD5 line too (kCatMd5* below): patch both and the archive
// is self-consistent from X4's point of view. If signatures are checked as
// well, this cannot win and the extension route is the only one left.
//
// Nothing is written to the player's install, which is the whole point.
struct BytePatch {
    const char *file;      // basename to match on open
    long long offset;      // absolute, within that file
    const char *from;
    const char *to;
    const char *env;       // extra knob this entry needs, or nullptr
};

// Offsets computed from 08.cat's index (a running sum of member sizes) against
// this build of X4; `catpatch: MISS` in the log means the bytes were not what
// was expected and NOTHING was substituted -- a version change, not a silent
// wrong edit.
const BytePatch kPatches[] = {
    // libraries/parameters.xml, member offset 17604207 + 14573.
    {"08.dat", 17618780LL, "maxpitch=\"35\" maxyaw=\"65\"",
     "maxpitch=\"70\" maxyaw=\"85\"", nullptr},
    // X4VR_CATPATCH_MD5: the member's MD5 in 08.cat's index, so the archive is
    // self-consistent. Take 205b substituted the content and the save came
    // back modified="1" against a byte-identical control run where the
    // substitution silently did not fire -- so X4 verified what it read. This
    // entry tests WHICH check caught it.
    //
    // Its own knob because it is one hypothesis, not an improvement: MD5 is
    // trivially recomputable (this value took seconds), so it cannot be what
    // protects the archive. X4 also opens every `_sig.cat`/`_sig.dat` at
    // startup and 08_sig.dat holds `libraries/parameters.xml.sig`, 1024 bytes
    // -- an RSA signature, which is what an anti-tamper check would use and
    // which cannot be forged. If this entry does not clear the flag, that is
    // the answer and the route is closed.
    {"08.cat", 37267LL, "384543b4b1b4c30cc23dad489eccb130",
     "9b40b775c51422f5b7e97b0bc0fb9664", "X4VR_CATPATCH_MD5"},
};

bool catpatch_on() {
    static const bool on = [] {
        const char *e = getenv("X4VR_CATPATCH");
        return e && *e && *e != '0';
    }();
    return on;
}

std::mutex &catfd_mu() {
    static std::mutex m;
    return m;
}
std::map<int, std::string> &catfds() {
    static std::map<int, std::string> &m = x4vr::immortal<std::map<int, std::string>>();
    return m;
}

const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Called from the open family: remember fds that name a file we patch.
void catpatch_track(int fd, const char *path) {
    if (!catpatch_on() || fd < 0 || !path)
        return;
    const char *b = basename_of(path);
    for (const BytePatch &p : kPatches) {
        if (strcmp(b, p.file) == 0) {
            std::lock_guard<std::mutex> lock(catfd_mu());
            catfds()[fd] = p.file;
            return;
        }
    }
}

void catpatch_untrack(int fd) {
    if (!catpatch_on())
        return;
    std::lock_guard<std::mutex> lock(catfd_mu());
    catfds().erase(fd);
}

// ...and the same by FILE*, which is what X4 actually uses.
//
// Take 205b fired nothing: the fd hooks above were wired into open/open64/
// openat, and X4 opens its archives with **fopen** -- `fileprobe: fopen(
// ./08.dat)`, against 175 fopen calls and 2 open64 in that run. The fixture I
// validated against used open()+read() directly, so it exercised a path X4
// does not take, and take 203's fileprobe had already recorded which one it
// does. A green test on the wrong door.
//
// Hooking read() alone would not have rescued it either: glibc's stdio calls
// its own hidden alias rather than the interposable `read`, so fread never
// reaches an LD_PRELOAD hook on it. The substitution has to happen at the
// level X4 actually calls, which is fread.
std::map<FILE *, std::string> &catfiles() {
    static std::map<FILE *, std::string> &m = x4vr::immortal<std::map<FILE *, std::string>>();
    return m;
}

void catpatch_track_file(FILE *f, const char *path) {
    if (!catpatch_on() || !f || !path)
        return;
    const char *b = basename_of(path);
    for (const BytePatch &p : kPatches) {
        if (strcmp(b, p.file) == 0) {
            std::lock_guard<std::mutex> lock(catfd_mu());
            catfiles()[f] = p.file;
            return;
        }
    }
}

// Rewrite whatever of this read overlaps a patch window. `start` is the file
// offset the returned bytes begin at.
void catpatch_apply_named(const std::string &file, void *buf, ssize_t n,
                          long long start) {
    if (!catpatch_on() || n <= 0)
        return;
    for (const BytePatch &p : kPatches) {
        if (file != p.file)
            continue;
        if (p.env) {
            const char *e = getenv(p.env);
            if (!(e && *e && *e != '0'))
                continue;
        }
        const long long len = (long long)strlen(p.from);
        const long long lo = p.offset, hi = p.offset + len;
        if (start >= hi || start + n <= lo)
            continue;
        // Overlap. Only substitute the part of the window this read covers,
        // because X4 is free to split it across two reads.
        for (long long i = 0; i < len; i++) {
            const long long abs = lo + i;
            if (abs < start || abs >= start + n)
                continue;
            uint8_t *dst = (uint8_t *)buf + (abs - start);
            // Verified before written: if the byte is not what the table
            // expects, the offsets belong to a different build of X4 and
            // substituting would corrupt whatever is really there.
            if (*dst != (uint8_t)p.from[i]) {
                static bool said = false;
                if (!said) {
                    said = true;
                    X4VR_LOG("catpatch: MISS at %s+%lld — expected '%c', found "
                             "'%c'. Offsets are for another build of X4; "
                             "NOTHING substituted",
                             p.file, abs, p.from[i], *dst);
                }
                return;
            }
            *dst = (uint8_t)p.to[i];
        }
        // Every hit, up to a bound, not just the first. If X4 reads the window
        // more than once -- say a verification pass and then a parse -- that
        // is visible here and only here, and it would be the one seam worth
        // knowing about. Take 205b logged a single 144 KB page-aligned read
        // covering the whole member, but it was capped at one report so the
        // count was never established.
        static std::atomic<int> hits{0};
        const int k = hits.fetch_add(1);
        if (k < 8)
            X4VR_LOG("catpatch: hit #%d %s+%lld '%s' -> '%s' (read %lld..%lld)"
                     " — the archive on disk is UNTOUCHED; if the save still "
                     "comes back modified, X4 verified what it read (#68)",
                     k + 1, p.file, p.offset, p.from, p.to, start, start + n);
        else if (k == 8)
            X4VR_LOG("catpatch: 8 hits reported, further ones silent");
    }
}

void catpatch_apply(int fd, void *buf, ssize_t n, long long start) {
    if (!catpatch_on() || n <= 0)
        return;
    std::string file;
    {
        std::lock_guard<std::mutex> lock(catfd_mu());
        auto it = catfds().find(fd);
        if (it == catfds().end())
            return;
        file = it->second;
    }
    catpatch_apply_named(file, buf, n, start);
}

void catpatch_apply_file(FILE *f, void *buf, ssize_t n, long long start) {
    if (!catpatch_on() || n <= 0)
        return;
    std::string file;
    {
        std::lock_guard<std::mutex> lock(catfd_mu());
        auto it = catfiles().find(f);
        if (it == catfiles().end())
            return;
        file = it->second;
    }
    catpatch_apply_named(file, buf, n, start);
}

// ---- X4VR_PARAMSCAN: find the clamp AFTER X4 has parsed it ----------------
//
// Task #68's one remaining honest route. Read-substitution is closed: X4
// verifies the bytes it reads and the protection is an RSA signature we cannot
// forge, and feeding a verifier different bytes than the parser is not
// something this project will do. But there is nothing dishonest about letting
// X4 read and verify its genuine, untouched files -- the check runs on real
// data and truthfully reports unmodified -- and then changing the value it
// parsed. That is the same class as the SPIR-V, projection and descriptor work
// the layer already does.
//
// **This only REPORTS.** Whether the parsed values are locatable at all, and
// uniquely, is unknown, and building a writer before knowing that would be
// building on a guess. The signature is strong: X4's own element is
//
//   <internal maxpitch="35" maxyaw="65" anglespeed="200" angleacc="500"
//             trackmaxpitch="85" trackmaxyaw="85"/>
//
// so a 35 adjacent to a 65 is already selective, and the neighbours let a real
// hit be told from a coincidence by eye. Both float and int32 are searched
// because how X4 stores them is not established -- guessing one and finding
// nothing would be indistinguishable from "not there".
struct ScanHit {
    uintptr_t addr;
    const char *kind;
};

// The element's own text, exactly as parameters.xml carries it. Same length
// in and out, so nothing around it moves.
const char kElemFrom[] = "maxpitch=\"35\" maxyaw=\"65\"";
const char kElemTo[]   = "maxpitch=\"70\" maxyaw=\"85\"";
const size_t kElemLen = sizeof(kElemFrom) - 1;
static_assert(sizeof(kElemFrom) == sizeof(kElemTo), "same length");

bool paramscan_on() {
    static const bool on = [] {
        const char *e = getenv("X4VR_PARAMSCAN");
        return e && *e && *e != '0';
    }();
    return on;
}

// Take 206: searching for a 35 adjacent to a 65 returned 64 hits, hit the cap,
// and every one was the same repeating 5-tuple with a stepping coordinate
// (35, 1524, ..., 65 / 35, 1528, ..., 65) -- glyph metrics or an atlas. One
// false-positive family ate the entire budget, so the run was not evidence
// about the float case at all.
//
// The anchor is now (200, 500) -- `anglespeed` and `angleacc`, far rarer than
// two small round numbers -- and a hit additionally requires 35, 65 and two
// 85s within the surrounding window. All six values of X4's element, which no
// glyph table is going to reproduce by accident.
template <typename T>
void paramscan_region(const uint8_t *base, size_t len, const char *kind,
                      std::vector<ScanHit> &out, size_t cap, uintptr_t at,
                      size_t &seen) {
    const size_t W = 16;                    // slots either side of the anchor
    if (len < sizeof(T) * (W * 2 + 2))
        return;
    for (size_t i = sizeof(T) * W;
         i + sizeof(T) * (W + 2) <= len && out.size() < cap; i += sizeof(T)) {
        T x, y;
        memcpy(&x, base + i, sizeof(T));
        memcpy(&y, base + i + sizeof(T), sizeof(T));
        if (!((x == (T)200 && y == (T)500) || (x == (T)500 && y == (T)200)))
            continue;
        int n35 = 0, n65 = 0, n85 = 0;
        for (size_t k = 0; k < W * 2; k++) {
            T v;
            memcpy(&v, base + i - sizeof(T) * W + sizeof(T) * k, sizeof(T));
            if (v == (T)35) n35++;
            else if (v == (T)65) n65++;
            else if (v == (T)85) n85++;
        }
        seen++;
        if (n35 && n65 && n85 >= 2)
            out.push_back({at + i, kind});
    }
}

void paramscan_run() {
    std::vector<ScanHit> hits;
    const size_t kCap = 64;
    const size_t chunk_bytes = 1u << 20;
    size_t regions = 0, bytes = 0, skipped = 0, anchors = 0;
    std::vector<uintptr_t> strhits, elemhits;
    const int memfd = open("/proc/self/mem", O_RDONLY);
    if (memfd < 0) {
        X4VR_LOG("paramscan: cannot open /proc/self/mem");
        return;
    }
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) {
        X4VR_LOG("paramscan: cannot read /proc/self/maps");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), m) && hits.size() < kCap) {
        uintptr_t lo = 0, hi = 0;
        char perms[8] = {0}, path[256] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255s", &lo, &hi, perms,
                   path) < 3)
            continue;
        if (perms[0] != 'r' || perms[1] != 'w')
            continue;
        // Anonymous and heap only. File-backed rw regions are libraries' data
        // segments and X4's parsed parameters are not in one.
        if (path[0] && strcmp(path, "[heap]") != 0)
            continue;
        const size_t len = hi - lo;
        if (len < 64 || len > (size_t)512 << 20)
            continue;
        regions++;
        // Copied out through /proc/self/mem rather than dereferenced.
        //
        // The first version walked the pointers directly and took a SIGSEGV:
        // /proc/self/maps lists regions that are not all safely readable, and
        // one bad page kills the process -- which in X4 would be a crash
        // blamed on the mod. pread returns EIO/EFAULT instead, so an
        // unreadable region is skipped and counted.
        std::vector<uint8_t> chunk(chunk_bytes);
        for (size_t o = 0; o < len && hits.size() < kCap; o += chunk_bytes) {
            const size_t want = std::min(chunk_bytes, len - o);
            const ssize_t got = pread(memfd, chunk.data(), want,
                                      (off_t)(lo + o));
            if (got <= 0) {
                skipped++;
                break;
            }
            bytes += (size_t)got;
            paramscan_region<float>(chunk.data(), (size_t)got, "f32", hits,
                                    kCap, lo + o, anchors);
            paramscan_region<int32_t>(chunk.data(), (size_t)got, "i32", hits,
                                      kCap, lo + o, anchors);
            // A DOM kept in memory would be a far better handle than any
            // number, so look for the attribute name too.
            for (size_t k = 0; k + 6 <= (size_t)got; k++)
                if (memcmp(chunk.data() + k, "maxyaw", 6) == 0 &&
                    strhits.size() < 8)
                    strhits.push_back(lo + o + k);
            // Take 207: 8 "maxyaw" strings in WRITABLE memory, clustered
            // within ~4 KB, while the numeric search found nothing.
            // parameters.xml contains "maxyaw" exactly four times, so eight is
            // two copies of the file's text retained after parsing.
            //
            // That is the handle. The exact element text is same-length
            // substitutable, like the archive bytes were, and rewriting our
            // own process's memory deceives no verifier -- X4's files are
            // genuinely untouched and its integrity check has already run and
            // truthfully passed.
            for (size_t k = 0; k + kElemLen <= (size_t)got; k++) {
                if (memcmp(chunk.data() + k, kElemFrom, kElemLen) != 0)
                    continue;
                elemhits.push_back(lo + o + k);
                if (elemhits.size() > 16)
                    break;
            }
        }
    }
    fclose(m);
    close(memfd);
    X4VR_LOG("paramscan: %zu (200,500) anchor(s) examined, %zu \"maxyaw\" "
             "string(s) in memory", anchors, strhits.size());
    // The element text, and the patch if this run asked for one.
    const char *pe = getenv("X4VR_PARAMPATCH");
    const bool want_patch = pe && *pe && *pe != '0';
    X4VR_LOG("paramscan: %zu copy/copies of '%s' in writable memory%s",
             elemhits.size(), kElemFrom,
             elemhits.empty() ? " — the text route is CLOSED for this build"
                              : "");
    for (size_t i = 0; i < elemhits.size() && i < 8; i++) {
        char ctx[121];
        const ssize_t g = pread(memfd, ctx, sizeof(ctx) - 1,
                                (off_t)(elemhits[i] - 40));
        if (g > 0) {
            ctx[g] = 0;
            for (ssize_t c = 0; c < g; c++)
                if ((unsigned char)ctx[c] < 32 || (unsigned char)ctx[c] > 126)
                    ctx[c] = '.';
            X4VR_LOG("paramscan: elem @%p  ...%s...", (void *)elemhits[i], ctx);
        }
    }
    if (want_patch && !elemhits.empty()) {
        size_t done = 0;
        for (uintptr_t a : elemhits) {
            // Verified before written, exactly as the archive patch is: only
            // memory that still reads as the expected text is touched.
            if (memcmp((const void *)a, kElemFrom, kElemLen) != 0)
                continue;
            memcpy((void *)a, kElemTo, kElemLen);
            done++;
        }
        X4VR_LOG("paramscan: PATCHED %zu of %zu copy/copies in place. X4's "
                 "FILES ARE UNTOUCHED and its integrity check already ran on "
                 "the real bytes — this only rewrites text this process is "
                 "holding. Whether it has any effect depends on X4 re-reading "
                 "it, which take 208 is what tests",
                 done, elemhits.size());
    } else if (!elemhits.empty()) {
        X4VR_LOG("paramscan: not patched — set X4VR_PARAMPATCH=1 to rewrite "
                 "them");
    }
    for (size_t i = 0; i < strhits.size() && i < 4; i++)
        X4VR_LOG("paramscan: \"maxyaw\" @%p — X4 kept the parsed text, which "
                 "is a better handle than any number", (void *)strhits[i]);
    X4VR_LOG("paramscan: %zu region(s), %zu MB read, %zu unreadable, "
             "%zu hit(s)%s", regions, bytes >> 20, skipped, hits.size(),
             hits.size() >= kCap ? " (CAPPED — more exist)" : "");
    if (hits.empty()) {
        X4VR_LOG("paramscan: NOTHING found. Either X4 stores these as another "
                 "type (double? scaled to radians?), or not adjacently, or the "
                 "scan ran before the parse. It is not evidence that the value "
                 "is unreachable");
        return;
    }
    // Context, so a real camera struct can be told from a coincidence: the
    // element's other attributes are 200, 500, 85, 85 and should be nearby.
    for (size_t i = 0; i < hits.size() && i < 12; i++) {
        // Printed in the type that MATCHED. Take 206 dumped int bit patterns
        // as floats -- 4.90454e-44 for 35 -- and the rows were unreadable
        // until decoded by hand.
        char buf[320];
        int n = snprintf(buf, sizeof(buf), "paramscan: %s @%p  ",
                         hits[i].kind, (void *)hits[i].addr);
        const bool isf = hits[i].kind[0] == 'f';
        for (int k = -8; k < 12 && n < (int)sizeof(buf) - 16; k++) {
            const uint8_t *q = (const uint8_t *)hits[i].addr + k * 4;
            if (isf) { float v; memcpy(&v, q, 4);
                       n += snprintf(buf + n, sizeof(buf) - n, "%g ", v); }
            else     { int32_t v; memcpy(&v, q, 4);
                       n += snprintf(buf + n, sizeof(buf) - n, "%d ", v); }
        }
        X4VR_LOG("%s", buf);
    }
    X4VR_LOG("paramscan: the row that is X4's camera block should read "
             "... 35 65 200 500 85 85 ... — anything else is a coincidence");
}

bool overrides_disabled() {
    static const bool off = [] {
        const char *e = getenv("X4VR_NO_CONFIG");
        return e && *e && *e != '0';
    }();
    return off;
}

// The mod's settings file and its contents. `xml` is empty if we could not
// establish one, in which case the caller leaves X4's own file alone.
struct Profile {
    std::string path;
    std::string xml;
};

// Load config-x4vrmod.xml, forking it verbatim from the player's config.xml
// the first time. The fork is a plain copy: the overrides are applied in
// memory on every read (see open_patched_config), so baking them into the
// file would be redundant — and keeping the copy verbatim means the profile
// starts out as an honest snapshot of the player's own preferences.
Profile load_profile(const char *config_path) {
    Profile p;
    p.path = x4vr::profile_path(config_path);
    if (p.path.empty())
        return p;
    p.xml = x4vr::read_file(p.path.c_str());
    if (!p.xml.empty())
        return p; // already forked (also re-forks if it was left empty)

    p.xml = x4vr::read_file(config_path);
    if (p.xml.empty()) {
        X4VR_LOG("config: could not read %s — passing through", config_path);
        return p;
    }
    if (x4vr::write_file(p.path.c_str(), p.xml))
        X4VR_LOG("config: created %s from your config.xml — X4's settings go "
                 "there while modded; delete it to reset",
                 p.path.c_str());
    else
        X4VR_LOG("config: WARNING could not create %s — settings changed this "
                 "session will not persist",
                 p.path.c_str());
    return p;
}

// `mousesteeringline`: false while X4VR_STEER's overlay draws the line, and
// the player's own value back otherwise. The value it replaced is kept in a
// sidecar (x4vr_config.hpp, sidecar_path), because X4 saves the served false
// into the profile and a run without the overlay must not inherit it.
// `hide`: steer_hide_x4_line(), passed in by the fopen hooks below, where it
// is defined.
static void steer_config(std::string &xml, const std::string &profile,
                         bool hide) {
    const char *tag = "mousesteeringline";
    const std::string side = x4vr::sidecar_path(profile, tag);
    const std::string was = x4vr::read_file(side.c_str());
    const bool have_side = !was.empty();
    if (hide) {
        std::string old;
        if (x4vr::set_or_insert_tag(xml, tag, "false", &old)) {
            if (!have_side)
                x4vr::write_file(side.c_str(), old.empty() ? "absent" : old);
            X4VR_LOG("config: %s: '%s' -> 'false' (X4VR_STEER: the layer draws "
                     "the steering line; your value is kept in %s)",
                     tag, old.empty() ? "absent" : old.c_str(), side.c_str());
        } else {
            // Already false in the profile -- X4 saved it last run. Said
            // anyway: the scorer checks the line was hidden, not that it
            // changed.
            X4VR_LOG("config: %s: already 'false' (X4VR_STEER: the layer draws "
                     "the steering line; your value is kept in %s)",
                     tag, side.c_str());
        }
        return;
    }
    if (!have_side)
        return;
    // The player's value was "absent" (X4's default, true) or a literal.
    const std::string v = (was == "absent" || was.empty()) ? "true" : was;
    std::string old;
    x4vr::set_or_insert_tag(xml, tag, v.c_str(), &old);
    using unlink_fn = int (*)(const char *);
    static unlink_fn real_unlink = (unlink_fn)dlsym(RTLD_NEXT, "unlink");
    if (real_unlink)
        real_unlink(side.c_str());
    X4VR_LOG("config: %s: '%s' -> '%s' — restored (no X4VR_STEER this run)",
             tag, old.c_str(), v.c_str());
}

// Build the rewritten config and return a stream over it, or nullptr to let
// the caller fall back to the real file.
FILE *open_patched_config(const char *path, bool hide_steer_line) {
    Profile prof = load_profile(path);
    if (prof.xml.empty())
        return nullptr;
    std::string xml = std::move(prof.xml);

    // Applied on every read, not just the first: if the player changes one of
    // these in the options menu, X4 writes it to the profile and we override
    // it again next launch. Every *other* setting they change persists.
    int changed = 0;
    for (const auto &ov : x4vr::default_overrides()) {
        std::string old;
        if (x4vr::set_tag(xml, ov.tag, ov.value, &old)) {
            X4VR_LOG("config: %s: '%s' -> '%s'", ov.tag, old.c_str(), ov.value);
            changed++;
        }
    }
    steer_config(xml, prof.path, hide_steer_line);

    // Back the stream with a memfd rather than fmemopen(): an fmemopen
    // stream has no real file descriptor, so any fileno()/fstat()/mmap() in
    // the consumer's XML reader fails and X4 silently falls back to its
    // defaults (observed: desktop resolution instead of ours). A memfd is
    // still purely in memory — nothing is written to disk.
    int fd = memfd_create("x4vr-config", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    size_t off = 0;
    while (off < xml.size()) {
        ssize_t w = ::write(fd, xml.data() + off, xml.size() - off);
        if (w <= 0) {
            ::close(fd);
            return nullptr;
        }
        off += (size_t)w;
    }
    ::lseek(fd, 0, SEEK_SET);
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        ::close(fd);
        return nullptr;
    }
    // Stated on every run, overridden or not: this is what X4 will build its
    // projection from, and a log that omits it cannot reproduce its own run.
    // See get_tag() for why the un-overridden case is the dangerous one.
    X4VR_LOG("config: effective fov=%s res=%sx%s (%s)",
             x4vr::get_tag(xml, "fov").c_str(),
             x4vr::get_tag(xml, "res_width").c_str(),
             x4vr::get_tag(xml, "res_height").c_str(),
             getenv("X4VR_FOV") ? "fov from X4VR_FOV"
                                : "fov from the profile, not this run");
    // Same reasoning as fov: state the effective value every run, overridden or
    // not. This one decides whether X4 draws the radar as a flat screen-space
    // element or on the cockpit's target monitor, and a run that changed the
    // routing without recording it could not be told from one that did not.
    X4VR_LOG("config: effective separateRadar=%s (%s) — 0 routes the radar to "
             "the cockpit target monitor, 1 to the flat element",
             x4vr::get_tag(xml, "separateRadar").c_str(),
             getenv("X4VR_SEPARATE_RADAR")
                 ? "from X4VR_SEPARATE_RADAR"
                 : "from the profile, not this run");
    // This tag also selects X4's upscaler, so it is not a cosmetic setting: the
    // temporal modes jitter the projection into the eye shear's own slots. A run
    // that changed it must say so, or a broken stereo pair looks like a
    // regression in the shear rather than the knob that caused it.
    X4VR_LOG("config: effective antialiasing=%s (%s) — this tag also selects "
             "the upscaler; temporal/dlaa/fsr_* all jitter m[8]/m[9]",
             x4vr::get_tag(xml, "antialiasing").c_str(),
             getenv("X4VR_AA") ? "from X4VR_AA"
                               : "our default, not the profile");
    X4VR_LOG("config: serving %s in memory (%zu bytes, %d overrides applied); "
             "your %s is not touched",
             prof.path.c_str(), xml.size(), changed, path);
    return f;
}

// ------------------------------------------------------------- inputmap
//
// #33 gives head-look its own binding rather than synthesising shift+middle
// mouse, which would go through X4's binding interpreter and corrupt every
// combo built on those. See x4vr_inputmap.hpp for why it is a held key and not
// a toggle. Same fork-and-serve shape as config.xml above: the player's file is
// read once and never written.
bool headlook_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_HEADLOOK");
        return e && *e && *e != '0';
    }();
    return on;
}

Profile load_inputmap_profile(const char *path) {
    Profile p;
    p.path = x4vr::inputmap_profile_path(path);
    if (p.path.empty())
        return p;
    p.xml = x4vr::read_file(p.path.c_str());
    if (!p.xml.empty())
        return p;
    p.xml = x4vr::read_file(path);
    if (p.xml.empty()) {
        X4VR_LOG("inputmap: could not read %s — passing through", path);
        return p;
    }
    if (!x4vr::write_file(p.path.c_str(), p.xml))
        X4VR_LOG("inputmap: WARNING could not create %s", p.path.c_str());
    return p;
}

// Build the rebound inputmap and return a stream over it, or nullptr to fall
// back to the real file.
FILE *open_patched_inputmap(const char *path) {
    Profile prof = load_inputmap_profile(path);
    if (prof.xml.empty())
        return nullptr;
    std::string xml = std::move(prof.xml);

    std::string old;
    const int n = x4vr::bind_headlook(xml, &old);
    if (n == 0) {
        // Stated rather than passed over: an inputmap X4 reads that has no
        // free-look binding means head-look will not work in that profile, and
        // silence here would look exactly like success.
        X4VR_LOG("inputmap: %s has no INPUT_STATE_CAMERA_MOUSELOOK — head-look "
                 "will not be bound from this file",
                 path);
        return nullptr;
    }
    X4VR_LOG("inputmap: %s: %d binding(s) -> %s (was %s)", path, n,
             x4vr::headlook_code(), old.c_str());

    // memfd, not fmemopen: an fmemopen stream has no real descriptor, and a
    // reader that calls fileno()/fstat()/mmap() silently falls back to its
    // defaults. Learned on config.xml; the same reader is likely behind this.
    int fd = memfd_create("x4vr-inputmap", MFD_CLOEXEC);
    if (fd < 0)
        return nullptr;
    size_t off = 0;
    while (off < xml.size()) {
        ssize_t w = ::write(fd, xml.data() + off, xml.size() - off);
        if (w <= 0) {
            ::close(fd);
            return nullptr;
        }
        off += (size_t)w;
    }
    ::lseek(fd, 0, SEEK_SET);
    FILE *f = fdopen(fd, "rb");
    if (!f) {
        ::close(fd);
        return nullptr;
    }
    X4VR_LOG("inputmap: serving %s in memory (%zu bytes); your %s is not touched",
             prof.path.c_str(), xml.size(), path);
    return f;
}

// ------------------------------------------------------------- content.xml
//
// X4VR_EXTENSIONS=1: our extensions (extension/ in the repo, deployed by the
// launcher) enabled for the modded session only. content.xml lives in the
// profile directory vanilla X4 reads too, so it is forked like config.xml:
// read once to seed content-x4vrmod.xml, served from the fork with our
// extensions forced on IN MEMORY, writes redirected into the fork. Served on
// BOTH the fopen and the open() read paths, because nothing on record says
// which one X4 uses for this file -- the log line names the one it did.
bool extensions_enabled() {
    static const bool on = x4vr::env_on("X4VR_EXTENSIONS", false);
    return on;
}

// The extensions we own, and whether this session wants each. The look-around
// limits ride with X4VR_EXTENSIONS; x4vrmod_headlook (anglespeed 0: X4's
// cockpit look follower holds still) only with X4VR_HEADLOOK_WRITE, and is
// forced OFF otherwise -- X4 writes content.xml into our fork, so a session
// that enabled it would leave it enabled for every later one.
bool headlook_write_enabled() {
    static const bool on = x4vr::env_on("X4VR_HEADLOOK_WRITE", false);
    return on;
}

// X4VR_HEADLOOK_MENU_CENTRE -- X4's camera back to centre while the HUD is
// down in the cockpit (the map and every full-screen menu). **ON by default
// with X4VR_HEADLOOK_WRITE; =0 restores take 328B for a bisect.**
//
// X4 draws its UI on a plane fixed to the cockpit and seen through the
// player camera, and hit-tests the pointer by the same projection, so a
// rotated camera keystones and shifts the whole UI inside the window the
// pointer is confined to. X4's own look follower recentres the camera when
// look-around ends (take 311: every map opened at 0,0); x4vrmod_headlook
// sets anglespeed 0 so a written angle STAYS, and that also stopped the
// follower from recentring. Take 328B opened the map at -1.99,6.37 and kept
// it there: 13.6% of X4's UI -- a left wedge widening downwards and the
// bottom band -- was outside the window, unreachable while visible in VR.
bool headlook_menu_centre() {
    static const bool on = x4vr::env_on("X4VR_HEADLOOK_MENU_CENTRE", true);
    return on;
}
// X4VR_HEADLOOK_SPEED_EXT=1 serves x4vrmod_headlook again (anglespeed 0 for
// the whole session, as takes 321-350 ran): the numpad orbit of F2/F3 is then
// frozen (take 350).
bool headlook_speed_extension() {
    static const bool on = headlook_write_enabled() &&
                           x4vr::env_on("X4VR_HEADLOOK_SPEED_EXT", false);
    return on;
}
struct OurExtension {
    const char *id;
    bool (*wanted)();
};
bool always_wanted() { return true; }
const OurExtension kOurExtensions[] = {
    {"x4vrmod_lookaround", always_wanted},
    // X4's G-force camera sway and boost shake off: in VR the view moves only
    // with the head. Take 321 saw the sway as 1-17 s stretches of up to 22 deg
    // once a written head-look no longer cancelled it (MOUSELOOK's loop had).
    {"x4vrmod_nosway", always_wanted},
    // Since take 351 only on request: the injector holds the speed at zero
    // per camera mode instead (headlook_speed_tick), so the external views'
    // orbit is not frozen with it.
    {"x4vrmod_headlook", headlook_speed_extension},
};

Profile load_content_profile(const char *path) {
    Profile p;
    p.path = x4vr::content_profile_path(path);
    p.xml = x4vr::read_file(p.path.c_str());
    if (!p.xml.empty())
        return p;
    p.xml = x4vr::read_file(path);
    if (p.xml.empty()) {
        X4VR_LOG("content: could not read %s — passing through", path);
        return p;
    }
    if (!x4vr::write_file(p.path.c_str(), p.xml))
        X4VR_LOG("content: WARNING could not create %s", p.path.c_str());
    return p;
}

// A memfd holding the fork with our extensions on, or -1 to pass through.
int content_memfd(const char *path, const char *how) {
    Profile prof = load_content_profile(path);
    if (prof.xml.empty())
        return -1;
    std::string xml = std::move(prof.xml);
    char what[512];
    int k = 0;
    for (const OurExtension &ext : kOurExtensions) {
        const bool on = ext.wanted();
        const int r = x4vr::set_extension(xml, ext.id, on);
        k += snprintf(what + k, sizeof what - (size_t)k, "%s%s %s", k ? ", " : "",
                      ext.id,
                      r == 0   ? (on ? "already enabled" : "already disabled")
                      : r == 1 ? (on ? "ENABLED (was off)" : "DISABLED (was on)")
                      : r == 2 ? (on ? "ENABLED (added)" : "DISABLED (added)")
                               : "COULD NOT be set — no </content>");
        if (k >= (int)sizeof what)
            k = (int)sizeof what - 1; // truncated: never index past the end
    }
    const int fd = memfd_create("x4vr-content", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < xml.size()) {
        const ssize_t w = ::write(fd, xml.data() + off, xml.size() - off);
        if (w <= 0) {
            ::close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    ::lseek(fd, 0, SEEK_SET);
    X4VR_LOG("content: serving %s in memory via %s — %s; your %s is not "
             "touched",
             prof.path.c_str(), how, what, path);
    return fd;
}

// If this is a write-mode open of X4's config.xml, the path to use instead
// (empty = leave the call alone). X4 saving its settings must never reach the
// player's file. Absolute paths only: an *at() call with a relative path
// resolves against dirfd, which our profile lookup cannot see.
std::string redirect_write(const char *path) {
    if (overrides_disabled() || !path || path[0] != '/')
        return {};
    if (x4vr::is_x4_config(path)) {
        Profile prof = load_profile(path);
        return prof.xml.empty() ? std::string{} : prof.path;
    }
    if (headlook_enabled() && x4vr::is_x4_inputmap(path)) {
        Profile prof = load_inputmap_profile(path);
        return prof.xml.empty() ? std::string{} : prof.path;
    }
    if (extensions_enabled() && x4vr::is_x4_profile_content(path)) {
        Profile prof = load_content_profile(path);
        return prof.xml.empty() ? std::string{} : prof.path;
    }
    return {};
}

// ------------------------------------------------------- the environment
//
// The launcher composes an environment for X4 and has never checked that X4
// received it. Take thirty-three came up on a Wayland surface while the
// launcher was passing `env -u WAYLAND_DISPLAY SDL_VIDEO_DRIVER=x11`, and
// nothing in the log distinguished "the environment did not arrive" from "X4
// overrode it" from "SDL fell back". These two hooks separate those: the
// startup dump is what arrived, the setenv/putenv watch is what changed after.
const char *const kWatchedEnv[] = {
    "SDL_VIDEODRIVER", "SDL_VIDEO_DRIVER", "WAYLAND_DISPLAY",
    "DISPLAY",         "XDG_SESSION_TYPE", "XDG_RUNTIME_DIR",
};

bool is_watched_env(const char *name, size_t len) {
    for (const char *w : kWatchedEnv)
        if (strlen(w) == len && !strncmp(name, w, len))
            return true;
    return false;
}

// Only the game, not the dozen shell utilities the launcher runs on the way.
bool exe_is_game(const char *exe) {
    const char *slash = strrchr(exe, '/');
    return slash && !strcmp(slash + 1, "X4");
}

void log_startup_env() {
    for (const char *w : kWatchedEnv) {
        const char *v = getenv(w);
        X4VR_LOG("env: %s=%s", w, v ? v : "(unset)");
    }
    // Every X4VR_* knob, in one copy-pasteable line.
    //
    // Runs have been referred to by take number for thirty-three takes and
    // the command that produced each one was never written down, so
    // reproducing an earlier configuration meant reconstructing it from what
    // the log happened to reveal. Since every knob is an environment
    // variable, listing them *is* the command. X4VR_TAKE is an ordinary
    // variable with no meaning to the code -- it exists only to be printed
    // here, so a run can carry its own name.
    //
    // **The value is QUOTED when it needs to be, and that is the point of the
    // line.** Take 258's command was written from this log line and contained
    //
    //     X4VR_WATCH_WANT=ui dump: burst frames
    //
    // which no shell reads as one assignment: `X4VR_WATCH_WANT=ui` is the
    // assignment and `dump:` is the command. Patola had to repair it by hand
    // before the take could start. The claim "copy-pasteable" was in this
    // comment while the code could not honour it for any value with a space --
    // and the whole documented method for building the next run is to diff
    // this line against the previous take's.
    // **Not only X4VR_*.** XR_RUNTIME_JSON is what SELECTS the runtime, and
    // filtering to our own prefix dropped it: every SteamVR take from 298
    // onward logged an `env: run =` line that, replayed verbatim, runs under
    // WiVRn instead. The line the whole method depends on was silently
    // recording a different experiment than the one that ran, and score_run
    // already had to warn separately that the variable never arrived.
    //
    // Listed explicitly rather than by prefix: XR_* also covers loader debug
    // vars that do not change what is measured, and a line that grows noise
    // stops being diffable against the previous take.
    static const char *const kAlsoRecord[] = {"XR_RUNTIME_JSON"};
    std::string run;
    for (const char *name : kAlsoRecord)
        if (const char *v = getenv(name)) {
            std::string assign = std::string(name) + "=" + v;
            run += ' ';
            run += x4vr::sh_quote_assignment(assign.c_str());
        }
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "X4VR_", 5)) {
            run += ' ';
            run += x4vr::sh_quote_assignment(*e);
        }
    X4VR_LOG("env: run =%s ./launch/x4vr-launch.sh", run.c_str());

    // **The controller configuration, read from the KERNEL rather than from
    // the handover text.** Take 308 was specified as "X52 Pro connected" and
    // ran with the stick powered but never enumerated: it lit up on bus power
    // while the kernel never saw it on the data lines, so dmesg's only X52
    // entry landed 310 s AFTER the run ended. Nothing in the log said so, and
    // the pad census was correctly silent -- which is indistinguishable from
    // "a device was there and X4 ignored it" without this.
    //
    // /proc/bus/input/devices, not SDL: no library, no open, no grab, and
    // nothing here can perturb the device under test. Entries whose Handlers
    // list a `js` node are the ones a game could use.
    if (FILE *f = fopen("/proc/bus/input/devices", "re")) {
        char line[512], name[256] = "?";
        int found = 0;
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "N: Name=", 8)) {
                snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, line + 8);
                char *nl = strpbrk(name, "\r\n");
                if (nl)
                    *nl = '\0';
            } else if (!strncmp(line, "H: Handlers=", 12) && strstr(line, "js")) {
                char *nl = strpbrk(line, "\r\n");
                if (nl)
                    *nl = '\0';
                X4VR_LOG("pad present: %s [%s]", name, line + 12);
                found++;
            }
        }
        fclose(f);
        if (!found)
            X4VR_LOG("pad present: NONE — the kernel lists no joystick device. "
                     "A stick that is plugged in and lit but absent here is "
                     "powered and not enumerated; re-seat it and check dmesg "
                     "before spending the take.");
    }
    // argv, for the command-line switches that pick a backend
    // (-prefer-wayland and friends): the launcher does not add them, but the
    // Steam launch options are outside its view entirely.
    std::string cmd = x4vr::read_file("/proc/self/cmdline");
    for (auto &c : cmd)
        if (c == '\0')
            c = ' ';
    X4VR_LOG("env: argv = %s", cmd.c_str());
}

template <typename T>
T real(const char *name) {
    static_assert(sizeof(T) == sizeof(void *), "fn ptr size");
    void *p = dlsym(RTLD_NEXT, name);
    T fn;
    memcpy(&fn, &p, sizeof(fn));
    return fn;
}

} // namespace

extern "C" {

static void csmfit_patch_at_start(); // defined with csmfit_tick, below
static void foot_patch_at_start();   // X4VR_HEADLOOK_FOOT, with foot_tick

__attribute__((constructor)) static void x4vr_inject_init() {
    char exe[256] = "?";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
        exe[n] = 0;
    X4VR_LOG("injector loaded into pid %d (%s)", getpid(), exe);
    if (exe_is_game(exe)) {
        log_startup_env();
        // #70's fix, X4VR_CSM_COVER: X4's code is patched here, before any of
        // it has run, so no thread can be inside the four instructions.
        csmfit_patch_at_start();
        // B (walking): the same rule -- before X4 runs a single frame.
        foot_patch_at_start();
    }
    // Task #68: the parsed clamp, once X4 has had time to parse it. A delay
    // rather than a hook, because the parse has no seam we can name -- and if
    // it turns out to have run too early, the log says "NOTHING found" and
    // that is explicitly not evidence of absence.
    // X4VR_PARAMSCAN_ANY exists so the scanner can be exercised outside the
    // game: the first validation run planted the values in a test binary and
    // found nothing, because the gate below is on the exe being X4 and the
    // test was not. A scanner that has only ever reported "nothing" in a
    // process it never ran in proves nothing about X4.
    if (paramscan_on() && (exe_is_game(exe) || getenv("X4VR_PARAMSCAN_ANY"))) {
        static const int delay = [] {
            const char *e = getenv("X4VR_PARAMSCAN_DELAY");
            return e && *e ? atoi(e) : 45;
        }();
        std::thread([] {
            struct timespec ts = {delay, 0};
            nanosleep(&ts, nullptr);
            X4VR_LOG("paramscan: scanning at t+%ds", delay);
            paramscan_run();
        }).detach();
    }
}

// X4 sets SDL_VIDEO_DRIVER itself -- the string "x11,wayland" is in the
// binary, next to SDL_VIDEO_DRIVER and SDL_GetCurrentVideoDriver. Whether it
// arrives through the environment or through SDL_SetHint decides whether the
// launcher can win by exporting a value at all, so watch the environment path
// here; the SDL path is read from the layer via SDL_GetHint.
//
// Interposed rather than merely observed for one reason: nothing else can see
// a write that happens after our constructor ran.
// Where X4 gets its render size.
//
// Take thirty-nine closed the last alternative: the layer reported a real
// 1408x1408 currentExtent and X4 asked for 2816x1408 anyway, so X4 does not
// consult the Vulkan surface at all. Take thirty-eight had already shown it
// follows its window. The window is therefore one number serving two purposes
// that must differ -- input wants the display width, the render wants one eye
// -- and the only way to separate them is to change what X4 *believes* the
// window is while leaving the real one alone.
//
// SDL_GetWindowSize and SDL_GetWindowSizeInPixels are the plausible sources.
// Interposing SDL was avoided until now because SDL2 and SDL3 disagree about
// SDL_CreateWindow's signature and gamescope (SDL2) shares this process tree
// with X4 (SDL3). These two are safe where that one was not: both SDL
// generations declare identical argument lists, and the return value is
// forwarded opaquely rather than interpreted.
//
// Logging is unconditional; the halving is behind X4VR_HALVE_WINDOW, so one
// run answers both "does X4 ask this" and "does answering differently work".
bool halve_window() {
    static const bool on = [] {
        const char *e = getenv("X4VR_HALVE_WINDOW");
        return e && *e && *e != '0';
    }();
    return on;
}

bool this_is_the_game() {
    static const bool yes = [] {
        char exe[256] = "?";
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0)
            exe[n] = 0;
        return exe_is_game(exe);
    }();
    return yes;
}

// Reports at most a handful of lines: X4 asks every frame, and the answer is
// only interesting when it changes.
void note_window_size(const char *fn, int w, int h, bool halved) {
    static int last_w = -1, last_h = -1;
    if (w == last_w && h == last_h)
        return;
    last_w = w;
    last_h = h;
    X4VR_LOG("sdl: %s -> %dx%d%s (pid %d)", fn, w, h,
             halved ? " (HALVED from the real window)" : "", getpid());
}

// Which display server X4 is actually talking to.
//
// Take thirty-six: X4 creates a Wayland surface (handle 0x3edd2040) and an xcb
// surface (0x3befb820), never asks whether it can present on the xcb one, and
// builds its swapchain on the Wayland one -- while SDL's video driver is x11.
// Every explanation for that turns on which socket the Wayland connection
// reached, and the two candidates sit side by side in $XDG_RUNTIME_DIR:
// `wayland-0` is the host Plasma session, `gamescope-0` is gamescope's own.
// The launcher's `env -u WAYLAND_DISPLAY` strips the variable that would have
// named the second, so an unqualified wl_display_connect(NULL) falls back to
// the first.
//
// That is an argument. This is the measurement: the connect() path itself.
// Only display-server sockets are logged -- X4 opens plenty of others.
// The input channel, watched before anything writes to it.
//
// Take forty separated the two purposes the window was thought to serve. X4's
// first SDL_GetWindowSize call lands 33ms *after* it has already created the
// swapchain, so the render size does not come from there -- and halving it
// broke menu clicks and map hover while leaving the render at 2816. So
// SDL_GetWindowSize is X4's **input space**, and the render size arrives by
// some other route entirely. They are different channels, which is what makes
// a shim that owns input possible without touching the render at all.
//
// SDL_PollEvent has identical arguments in SDL2 and SDL3; only the SDL_Event
// layout differs, and that is read solely when the process is X4 (SDL3). The
// offsets below are SDL3's: type/reserved/timestamp/windowID/which fill the
// first 24 bytes of both the motion and button events, putting x and y at 28
// and 32 in each.
struct Sdl3MouseEvent {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint32_t state_or_buttons;
    float x, y;
    float xrel, yrel;
};
enum { SDL_EV_MOUSE_MOTION = 0x400, SDL_EV_MOUSE_DOWN = 0x401,
       SDL_EV_MOUSE_UP = 0x402 };

// SDL3's SDL_JoyAxisEvent and SDL_GamepadAxisEvent, which share this layout
// exactly -- checked against the real headers in tests/sdl_event_layout.cpp
// like the two above.
//
// **Take 307 is why this exists.** The pad census counted SDL_GetJoystickAxis
// and reported "OPENED, NEVER POLLED -- no binding could have acted" while
// Patola was steering the ship with the pad perfectly well. X4 does not import
// SDL_GetGamepadAxis at all, and called SDL_GetJoysticks and
// SDL_GetJoystickAxis zero times, yet SDL_OpenGamepad(2) succeeded: the axis
// values arrive as EVENTS, through the drain this file has hooked since #19.
// The census watched a polling API for an event-driven channel.
//
// The per-axis range is the point, not the count. X4's steering is bound to
// INPUT_JOYAXIS_RX/RY with strafe on X/Y, so "which axis index does this stick
// actually move" is the whole question for the X52 -- and it is a number the
// device reports, not one anybody has to guess.
struct Sdl3AxisEvent {
    uint32_t type;
    uint32_t reserved;
    uint64_t timestamp;
    uint32_t which; // SDL_JoystickID
    uint8_t axis;
    uint8_t padding1, padding2, padding3;
    int16_t value;
    uint16_t padding4;
};
enum {
    SDL_EV_JOY_AXIS = 0x600,
    SDL_EV_JOY_BUTTON_DOWN = 0x603,
    SDL_EV_JOY_ADDED = 0x605,
    SDL_EV_PAD_AXIS = 0x650,
    SDL_EV_PAD_BUTTON_DOWN = 0x651,
    SDL_EV_PAD_ADDED = 0x653,
};

// SDL3 SDL_KeyboardEvent, laid out positionally for the same reason as
// Sdl3MouseEvent above; tests/sdl_event_layout.cpp asserts both against the
// real headers.
//
// **Take 118 is why this exists.** X4 *imports* SDL_GetKeyboardState -- `nm -D`
// says so -- and never calls it: the run produced zero `headlook: armed` lines
// from a one-shot log inside the hook, while every other SDL interposer in this
// file fired normally. Imports are necessary, not sufficient, and the recorded
// error class this project already carries ("hooking a symbol X4 never
// imports") has a subtler sibling: hooking one it imports but does not call.
// X4 reads the keyboard the same way it reads the mouse, from the event queue.
struct Sdl3KeyEvent {
    uint32_t type;      //  0
    uint32_t reserved;  //  4
    uint64_t timestamp; //  8
    uint32_t windowID;  // 16
    uint32_t which;     // 20
    uint32_t scancode;  // 24
    uint32_t key;       // 28
    uint16_t mod;       // 32
    uint16_t raw;       // 34
    bool down;          // 36
    bool repeat;        // 37
};
enum { SDL_EV_KEY_DOWN = 0x300, SDL_EV_KEY_UP = 0x301 };
// "X4VR" little-endian, stamped into the unused `reserved` field of every event
// we synthesise so the log can separate ours from the player's.
constexpr uint32_t kOurEventMagic = 0x52565834u;

// A genuine motion event's windowID/which, copied so ours are indistinguishable
// from X4's own. Guessing zero would be a plausible-looking value X4 may filter
// on, and the failure would present as "head-look does nothing".
std::atomic<uint32_t> g_motion_window{0};
std::atomic<uint32_t> g_motion_which{0};
std::atomic<uint32_t> g_motion_seen{0};
// Set on the first SDL_PumpEvents from X4; see the interposer.
std::atomic<bool> g_pump_seen{false};

// **Task #75: where the head poses go.**
//
// The layer publishes one pose per submitted frame -- 89.6 Hz measured over
// take 283's 208 s -- and `inputrate` counts ~55 ticks/s. Something drops about
// 39% of them, and X4 is not involved in that step: both ends are ours.
//
// `inputrate` cannot say which step, because it counts ticks AFTER two early
// returns. A slow hook, a pose with valid==0, and several publishes collapsing
// into one poll all produce the same 55 Hz there, and they want different
// fixes. So each is counted separately, at its own site:
//
//   pump/peep/wait  how often X4 calls each event entry point. The tick runs
//                   from SDL_PumpEvents -- the SDL_PeepEvents call site is
//                   gated on !g_pump_seen, and g_pump_seen is set in every take
//                   since 118, so that path has been dead code throughout. The
//                   "~18 kHz" in headlook_tick's comment was measured on
//                   SDL_PeepEvents at take 119 and says nothing about the
//                   driver; peep is counted here only to retire that figure
//                   with a measurement instead of reasoning.
//   entered         headlook_tick past `armed` and this_is_the_game.
//   nopose          head_share_read returned false: pose invalid or torn.
//   fresh/skipped   past, or short of, the `updates != last_updates` gate.
//   published       delta of the layer's own `updates` counter over the window.
//
// `published` is the number that makes this a measurement rather than a
// comparison across two logs: it is the layer's publish count read from shared
// memory, printed on the SAME line as the fresh count it should equal. No
// cross-run arithmetic, and no matching of windows between two instruments.
//
// Per-window deltas, not cumulative totals. The last cumulative `inputrate`
// line of take 283 reported the camera acting on 33.4% when per-window
// differencing showed 73% -- a figure diluted by ~100 s of startup, which I
// then printed as a verdict.
struct PumpRate {
    std::atomic<uint64_t> pump{0}, peep{0}, wait{0};
    uint64_t entered = 0, nopose = 0, fresh = 0, skipped = 0;
    // `mark` is the counter as of the previous emit, NOT as of the first read of
    // this window: the poses published between the last read of one window and
    // the first read of the next belong to a window, and dropping them would
    // bias `published` down by exactly the loss being measured.
    uint64_t updates_mark = 0, updates_last = 0;
    // Ticks the freshness gate would have refused; 0 unless X4VR_HEADLOOK_UNGATED.
    uint64_t ungated_ticks = 0;
    // Every tick that reached the servo, whatever let it through.
    uint64_t acted = 0;
    bool have_updates = false;
    // Cumulative values as of the last emitted line, so each line reports its
    // own window.
    uint64_t p_pump = 0, p_peep = 0, p_wait = 0;
    double t_window = 0.0;
} g_pumprate;

// **Task #75: what does X4 actually RECEIVE?**
//
// Two explanations for take 285b's oscillation are dead: the servo arithmetic
// (refuted offline across burst x lead x amplitude) and a stale off-thread
// camera read (refuted by take 286, worst window 1.83 ms). What is left is the
// COUPLING rather than the arithmetic -- SDL_PushEvent called from a foreign
// thread while X4 drains -- and nothing in this project has ever measured it.
//
// So this counts both ends of the one channel that carries our commands into X4,
// and is deliberately a census rather than a hypothesis: how many events we
// push, how many X4 takes, how long each takes to cross, and how many arrive
// together. A third guessed mechanism would be the third in a row.
//
// The crossing time rides in the event's own `timestamp`, which is the field
// that means exactly that. SDL3 fills it only when it is zero, so ours survives,
// and X4 sees a monotonic-ns value from the same clock SDL would have used.
//
// `pushed - got` is the number that decides whether commands are being DROPPED
// or merely delayed -- and those want opposite fixes, which is why the two are
// counted at their own sites rather than inferred from one another.
struct EvFlow {
    std::atomic<uint64_t> pushed_motion{0}, pushed_key{0};
    std::atomic<uint64_t> got_motion{0}, got_key{0}, got_foreign{0};
    std::atomic<uint64_t> lat_sum_us{0}, lat_n{0}, lat_max_us{0};
    std::atomic<uint64_t> drains{0};
    // Ours per single SDL_PeepEvents(GET): index 8 is "8 or more". A servo that
    // commands once per tick should see 1; anything else is the queue holding
    // our commands back and handing X4 several at once.
    std::atomic<uint64_t> burst[9] = {};
    uint64_t p_pm = 0, p_pk = 0, p_gm = 0, p_gk = 0, p_fo = 0;
    uint64_t p_ls = 0, p_ln = 0, p_dr = 0, p_burst[9] = {};
    double t_window = 0.0;
} g_evflow;

// **X4's cursor SHAPE is its only hover indication, and it is the signal takes
// 310 and 311 read as absent.** The vanilla control settled that the signal
// exists: unmodified X4 shows an arrow off a target and a cross-in-circle over
// one, equally at the centre and the edges. (The white 4-corner contour is
// SELECTED, not hover — Patola corrected that, and it matters, because it
// leaves the cursor shape as the sole hover observable.)
//
// We hide X4's real cursor and composite our own captured copy, so a shape
// change X4 makes is only visible if we notice the SetCursor and find the
// handle. Neither was counted, which is why two takes could only say "it never
// changed" and not WHICH of three things happened:
//
//   sets=0             X4 never registered a hover; the pointer never landed
//                      on a target, and the question is placement.
//   sets>0, found=0    X4 changed the cursor and we kept drawing the old
//                      image. Ours, invisible, and a stale composite.
//   sets>0, found>0    X4 changed it and we published the right image, so the
//                      shape DID change on screen.
//
// X4 imports only SDL_CreateColorCursor and SDL_SetCursor (`nm -D`), both
// hooked, so `found` should track `sets` — and a gap is a defect rather than a
// missing hook.
struct CursorSets {
    std::atomic<uint64_t> sets{0}, found{0}, missed{0}, null{0}, changed{0};
    std::atomic<const void *> last{nullptr};
    // **Split by whether head-look was STEERING at the moment X4 set it.**
    //
    // Patola's question -- "ok if I spend half a minute flying closer?" --
    // exposed the confound: head-look holds MOUSELOOK, and #71 established
    // that holding MOUSELOOK is how X4 SUSPENDS MOUSE CONTROL. So a run with
    // `changed == 0` could mean nothing more than "X4 was not hit-testing the
    // pointer, because we were holding the key that tells it not to" -- which
    // is #71 restated, not a placement defect, and the two want completely
    // different work.
    //
    // Taking the flag at the moment of the set rather than re-deriving it
    // later: `steer` is the same local the drain reads, and an instrument that
    // recomputes the decision it reports can evaluate a different predicate
    // than the code did.
    std::atomic<uint64_t> changed_steering{0}, changed_free{0};
    // ...and by view, which is the axis take 312 could not name.
    std::atomic<uint64_t> changed_cockpit{0}, changed_elsewhere{0};
    // Mouse buttons X4 was handed, so a cursor change can be attributed.
    // Clicking changes the shape too (his map report: arrow on an element,
    // cross off one), so without this a `changed` count cannot distinguish a
    // hover from a click -- and asking him to avoid clicking would be asking
    // him to be careful instead of measuring.
    std::atomic<uint64_t> buttons{0};
} g_cursor_sets;

// Written by whichever thread runs the tick, read by the off-thread camera probe
// below, so the probe compares its own reading against one taken through X4's
// normal call path. See camprobe_start() for what the probe is for.
struct CamProbe {
    std::atomic<uint64_t> calls{0}, nonzero{0}, absurd{0};
    std::atomic<float> last_yaw{0.0f}, last_pitch{0.0f};
    std::atomic<float> drain_yaw{0.0f};
    std::atomic<uint32_t> drain_seen{0};
} g_camprobe;

// **Task #75: is the off-thread camera read STALE, or is the camera just fast?**
//
// Take 285b's two reads of the same camera on the same frame -- ours from the
// tick thread, the layer's at the first render pass -- disagreed by up to
// 59.588 deg during the oscillation and by 1.5 deg outside it. Staleness (our
// read returns a value X4 has moved past) and genuine ~1000 deg/s burst motion
// (the two threads sample either side of a burst) BOTH predict a disagreement
// the size of the jump, so that comparison cannot choose between them -- and
// they have opposite fixes.
//
// What separates them is a value matched against a TIME SERIES rather than
// against a single other sample. X4's own thread writes every reading it takes
// into this ring; a reader then asks "how old is the reading I just got?" by
// finding the ring entry closest in value and reporting the age of that entry.
// Fast motion makes the ring entries far apart in value and the match sharp;
// it does not make the match OLD. Only staleness does.
//
// Range, stated rather than assumed: N entries at X4's poll rate is about
// 128/90 = 1.4 s of history, so a lag beyond that reads as "the oldest entry"
// and must not be quoted as a measurement. `spread` below is the second gate:
// with a static camera every entry matches equally and the lag is noise, so a
// window whose values span less than `kMinSpread` is refused outright.
x4vr::CamRing g_camring;
// True while head-look is actually driving X4's camera; read by the event
// drain to decide whether the player's own mouse motion should reach the view.
std::atomic<bool> g_headlook_steering{false};
// Which VIEW X4 is in, stamped by headlook_tick where it already asks
// x4_view(), and read by SDL_SetCursor.
//
// **Take 312 needed this and did not have it.** The cursor-set census split
// changes by whether head-look was steering, and `steering = armed &&
// may_steer` is false BOTH when MOUSELOOK is released in the cockpit AND when
// the map is open -- so 50 changes landed in one "while free" bin that mixed
// the two, while Patola reported the shape never changed in the cockpit at
// all. The counter could not contradict him because it never named the axis.
//
// Stamped rather than queried here: x4_view() is an X4 FFI call and
// SDL_SetCursor runs on whatever thread X4 sets a cursor from, at a moment
// this file has never established is safe for one.
std::atomic<bool> g_in_cockpit{false};
// Whether g_in_cockpit has EVER been stamped. headlook_tick returns before the
// stamp when X4VR_HEADLOOK=0 -- which is precisely the configuration needed to
// test hover with MOUSELOOK released -- so the flag would sit at its `false`
// initialiser and the census would bin every change as "elsewhere", printing
// NOT ONE CHANGE IN THE COCKPIT for a run spent entirely in the cockpit. A
// default that is indistinguishable from a measurement is the same defect this
// file keeps paying for; the census refuses instead.
std::atomic<bool> g_view_known{false};
// Take 126 measured raw=48 for the apostrophe: on Linux this is the X11
// keycode, which is the evdev code plus 8. Settable so a run can test whether
// our key activates free-look with no real press to seed it -- the one thing
// take 126 could not separate, because the same press both activated mouselook
// and donated the field.
std::atomic<uint32_t> g_key_raw{[] {
    const char *e = getenv("X4VR_HEADLOOK_RAW");
    return e && *e ? (uint32_t)atoi(e) : 0u;
}()};
std::atomic<bool> g_key_raw_seen{getenv("X4VR_HEADLOOK_RAW") != nullptr};


// Observation only. Enough samples to establish the coordinate space and
// whether motion is absolute or relative, and then quiet: the question is what
// space X4 is told about, not how often.
//
// These hook SDL_WaitEvent and SDL_PeepEvents because that is what X4 actually
// calls. SDL_PollEvent was interposed here from take 40 until take 87 and fired
// exactly zero times across every run, including one with heavy map and menu
// use -- because X4 never imported it:
//
//     $ nm -D --undefined-only X4 | grep -c SDL_PollEvent
//     0
//
// One command, available from the first day, would have shown that the
// instrument could not work. Check the symbol is imported before drawing any
// conclusion from a silent hook.
// ---------------------------------------------------------------------------
// The input fold (task #19).
//
// X4 renders one eye; the compositor draws that frame twice, side by side, and
// gamescope centres X4's W-wide surface in the 2W-wide display. So an element
// X4 draws at `x_x4` appears on screen at `x_x4` and `x_x4 + W`, while the
// pointer -- confined to the surface -- reaches X4 as `x_sdl = x_screen - W/2`,
// measured at take 89 as exactly `0…1407` for W = 1408.
//
// The consequence is that pointing *at* an element never selects it: the input
// box straddles the seam, covering the right half of one copy and the left half
// of the other. To hover the station drawn at 251 you must put the pointer at
// 955.
//
//     x_x4 = (x_sdl + W/2) mod W
//
// undoes it. Coverage is complete rather than merely better: folding the full
// `0…W-1` yields `W/2…W-1` together with `0…W/2-1`, which is X4's whole frame,
// each element reachable by pointing at whichever of its two copies is nearer.
//
// **The fold is its own inverse**, since applying it twice adds W and W ≡ 0.
// That is what makes `SDL_WarpMouseInWindow` safe to route through the same
// function: X4 warps to a logical position, the shim folds it to the real one,
// and the position X4 reads back folds to what it asked for. A separate inverse
// would be a second thing to keep in step with this one.
//
// Off by default. This changes where every click lands, so it gets proven
// against a run with it off before it becomes the default.
bool input_fold() {
    static const bool on = [] {
        const char *e = getenv("X4VR_INPUT_FOLD");
        return e && *e && *e != '0';
    }();
    return on;
}

// X4's own window width, captured from the calls it makes rather than assumed.
// gamescope asks for the same information about its own 2816-wide surface, so
// this is recorded only when the caller is the game.
std::atomic<int> g_x4_win_w{0};

// Mouse-look. X4 switches the pointer to relative mode when it takes the
// camera, and in that mode positions are not what it steers by -- it integrates
// xrel/yrel, which the fold must never touch. Folding a position X4 is ignoring
// would be harmless; folding one it is not would not be, and the flag costs a
// hook X4 already calls.
std::atomic<bool> g_relative_mouse{false};

// The channel itself. One instance per process; the layer resolves the accessor
// by name. Function-local static so it is constructed on first use rather than
// racing other translation units' initialisers.
extern "C" x4vr::Shared *x4vr_shared_state() {
    static x4vr::Shared state;
    return &state;
}

// The other direction, #33: the layer writes the head pose here and the SDL
// hooks below read it. Owned by the injector for the reason x4vr_share.hpp
// gives -- only the LD_PRELOADed side has globally visible symbols.
extern "C" x4vr::HeadShared *x4vr_head_state() {
    static x4vr::HeadShared state;
    return &state;
}

namespace {

// True only while a located head pose is arriving and head-look is armed.
std::atomic<bool> g_headlook_active{false};

// Which of X4's eight numpad free-look keys are held right now, as a bitmask
// over kPanKeys below. Set from the event drain, read by the pan probe.
std::atomic<unsigned> g_pan_keys{0};
struct PanKey {
    int scancode;
    const char *name;
};
// SDL scancodes for the keypad. These are the shipped bindings of
// INPUT_STATE_CAMERA_LEFT/RIGHT/UP/DOWN and the four diagonals -- see the
// probe's comment for why the measurement uses X4's factory bindings and real
// fingers rather than a rebind and a synthetic press.
constexpr PanKey kPanKeys[] = {
    {92, "LEFT"},      {94, "RIGHT"},      {96, "UP"},        {90, "DOWN"},
    {95, "UP_LEFT"},   {97, "UP_RIGHT"},   {89, "DOWN_LEFT"}, {91, "DOWN_RIGHT"},
};
constexpr unsigned kNumPanKeys = sizeof(kPanKeys) / sizeof(kPanKeys[0]);
// The integrator. Single-threaded by construction: only the SDL hooks touch
// it, and X4 polls input from one thread.
x4vr::HeadLook g_headlook;

bool headlook_key_held() {
    // Only while the head is actually driving. Holding X4's free-look modifier
    // when nothing is publishing a pose would leave the game in mouselook with
    // no one steering it.
    return g_headlook_active.load(std::memory_order_relaxed);
}

// The scancode our rebound free-look key sits on. Must agree with
// x4vr::headlook_code(); SDL_SCANCODE_F13 is 104 and X4's INPUT_KEYCODE_F13 is
// the same physical key. Declared here rather than included, because this file
// deliberately has no SDL headers -- SDL2 and SDL3 share this process tree.
int headlook_scancode() {
    static const int sc = [] {
        const char *e = getenv("X4VR_HEADLOOK_SCANCODE");
        return e && *e ? atoi(e) : 104; // SDL_SCANCODE_F13
    }();
    return sc;
}

} // namespace

// Publish the pointer. Called from the position channel X4 actually reads, so
// what the layer draws and what X4 hit-tests are the same number by
// construction rather than by two paths agreeing.
//
// **Publishes the value X4 receives, fold included.** The first version here
// published the raw SDL position instead, reasoning that the fold is a
// flatscreen ergonomic and the channel should not carry a workaround it
// replaces. That had it backwards. The field means "where in X4's frame does X4
// believe the pointer is", and when the fold is on, that *is* the folded value
// -- X4 never sees the other one. Publishing the raw position would put the
// drawn cursor 704 px from the button it activates, with both features working
// exactly as designed.
//
// So the two compose instead of conflicting, and the invariant is structural:
// whatever number leaves this function for X4 is the number the layer draws at.
void publish_cursor(float x, float y, bool visible) {
    x4vr::Shared *s = x4vr_shared_state();
    const uint32_t start = s->seq.load(std::memory_order_relaxed);
    s->seq.store(start + 1, std::memory_order_relaxed); // odd: writing
    // The fence is the whole correctness of this. A *release store* on seq
    // would only stop earlier writes sinking below it -- the payload writes
    // come after, and nothing would stop them being hoisted above the odd
    // store, which lets a reader see an even seq either side of a half-written
    // pair. The first version here did exactly that.
    //
    // **Not observed, and not observable on this machine.** x86 is
    // store-ordered, so the wrong version does not actually tear here; a
    // negative control that reintroduced it still passed. The fix is for
    // correctness on weakly-ordered hardware, not for a bug that was measured.
    // Said plainly because a run of this project's mistakes have come from
    // recording a fix as "caught by a test" when the test could not have caught
    // it -- a tear that did show up in testing turned out to be the test's own
    // seeding, not the writer.
    std::atomic_thread_fence(std::memory_order_release);
    s->cursor_x.store(x, std::memory_order_relaxed);
    s->cursor_y.store(y, std::memory_order_relaxed);
    s->cursor_visible.store(visible ? 1u : 0u, std::memory_order_relaxed);
    s->window_w = (uint32_t)g_x4_win_w.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->seq.store(start + 2, std::memory_order_release); // even: settled
}

// SDL3's SDL_Surface, read positionally for the same reason SDL_Event is: the
// injector has no SDL headers and wants none. Layout, from SDL 3's public
// struct, on LP64:
//
//     0  SDL_SurfaceFlags flags   (Uint32)
//     4  SDL_PixelFormat  format  (enum, 4 bytes)
//     8  int w
//    12  int h
//    16  int pitch
//    20  (4 bytes of padding, so the pointer is 8-aligned)
//    24  void *pixels
//
// Every field read from it is validated below rather than trusted. If SDL ever
// moves one, the numbers stop being plausible and the capture refuses -- which
// is a great deal better than compositing whatever happened to be at offset 24.
struct Sdl3Surface {
    uint32_t flags;
    uint32_t format;
    int32_t w, h, pitch;
    uint32_t pad;
    const void *pixels;
};

// Cursors X4 has built, so SDL_SetCursor can publish the one it selects. X4
// swaps between a handful (the reticle and the arrow at least), and the handle
// is the only thing SDL_SetCursor gives us.
struct CapturedCursor {
    const void *handle = nullptr;
    uint32_t w = 0, h = 0, format = 0, pitch = 0;
    int32_t hot_x = 0, hot_y = 0;
    std::vector<uint8_t> px;
};
std::mutex g_cursor_mu;
std::vector<CapturedCursor> &g_cursors = x4vr::immortal<std::vector<CapturedCursor>>();

void publish_cursor_image(const CapturedCursor &c) {
    x4vr::Shared *s = x4vr_shared_state();
    const uint32_t start = s->cursor_img_seq.load(std::memory_order_relaxed);
    s->cursor_img_seq.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s->cursor_w = c.w;
    s->cursor_h = c.h;
    s->cursor_hot_x = c.hot_x;
    s->cursor_hot_y = c.hot_y;
    s->cursor_format = c.format;
    s->cursor_pitch = c.w * 4u; // repacked tightly by the capture below
    const size_t n = c.px.size() < sizeof(s->cursor_pixels) ? c.px.size()
                                                            : sizeof(s->cursor_pixels);
    for (size_t i = 0; i < n; i++)
        s->cursor_pixels[i] = c.px[i];
    std::atomic_thread_fence(std::memory_order_release);
    s->cursor_img_seq.store(start + 2, std::memory_order_release);
}

// Task #17, step 3. Once the layer draws the pointer into the eye image, the
// one the compositor draws is a second cursor -- in a different coordinate
// system, at a different place, in only one of the two halves.
//
// **`SDL_SetCursor` is the only lever X4 gives us.** `nm -D` on X4 says it
// imports exactly six mouse entry points -- CreateColorCursor, GetMouseState,
// SetCursor, SetWindowMouseGrab, SetWindowRelativeMouseMode, WarpMouseInWindow
// -- and neither SDL_ShowCursor nor SDL_HideCursor is among them. That was
// checked before this was written, because four instruments in this project
// have been hooked to symbols X4 never calls and each cost a run to discover.
//
// It also means nothing will undo this: X4 cannot re-show a cursor through a
// function it never calls. We resolve SDL_HideCursor ourselves -- it is exported
// by the libSDL3 X4 ships (3.2.28), whether or not X4 references it -- and SDL
// then tells the compositor there is no cursor to draw.
//
// Patola's "it stops being drawn if I hold still" is gamescope's own
// --hide-cursor-delay, which is more evidence that the pointer on screen is
// gamescope's and not something in X4's frame.
// **On by default, and it stands down only on declared intent.**
//
// The tempting design was to have the layer report "I am drawing" and hide only
// then. That is a *fallback*, and it is the wrong thing while the mod is still
// being debugged: if the overlay fails to build, gamescope's pointer quietly
// returns, the game looks fine, and the defect is invisible. This project's
// expensive mistakes are all that shape -- an aggregate that reported no defect
// on a 2.4x shading error, a predicate that masked its own suspect, a metric
// that could not tell correct stereo from broken.
//
// So the rule is: **gate on intent, never on outcome.**
//
//   * intent -- did this run ask for a drawn cursor? -- is configuration, and
//     standing down for it conceals nothing, because a run that never asked for
//     one has no defect to conceal;
//   * outcome -- did the draw succeed? -- is a fallback, and it converts a
//     defect into a working-looking system.
//
// The case that stays loud is exactly the one worth being loud: layer on, SBS
// on, cursor on, overlay failed to build -> no pointer at all, with the reason
// on the line above it in the log. That is escapable (the keyboard still works)
// and diagnosable (every failure path names itself), which is what makes
// failing early a fair trade here rather than a trap.
//
// **Decided 2026-08-08, for the development phase.** It should flip to the
// outcome-gate once anyone other than Patola runs this: then a silent fallback
// to the compositor's pointer protects a user instead of hiding a bug. See
// docs/known-good-runs.md.
bool hide_cursor_wanted() {
    const char *why = nullptr;
    if (!x4vr::env_on("X4VR_HIDE_CURSOR", true))
        why = "X4VR_HIDE_CURSOR=0";
    else if (x4vr::env_on("X4VR_NO_LAYER", false))
        why = "X4VR_NO_LAYER=1 — nothing is loaded that could draw one";
    else if (!x4vr::env_on("X4VR_SBS", false))
        why = "no side-by-side composite (X4VR_SBS is off)";
    else if (!x4vr::env_on("X4VR_SBS_SPLIT", true))
        why = "X4VR_SBS_SPLIT=0 — X4 renders full width, so there is no eye "
              "image to draw into";
    else if (!x4vr::env_on("X4VR_CURSOR", true))
        why = "X4VR_CURSOR=0 — the overlay is off";
    if (!why)
        return true;
    // Every one of these reads a variable the *layer* reads too, through the
    // same env_on so the two cannot disagree about what "on" means.
    X4VR_LOG("sdl: leaving the compositor's pointer alone — %s", why);
    return false;
}

void hide_sdl_cursor() {
    static const bool want = hide_cursor_wanted();
    if (!want)
        return;
    static bool done = false;
    if (done)
        return;
    auto fn = real<bool (*)(void)>("SDL_HideCursor");
    if (!fn) {
        done = true; // asking again every frame would not make it appear
        X4VR_LOG("sdl: SDL_HideCursor not found — cannot suppress the "
                 "compositor's pointer, expect two cursors");
        return;
    }
    done = true;
    const bool ok = fn();
    X4VR_LOG("sdl: SDL_HideCursor() -> %d — the compositor's pointer is "
             "suppressed; the one you see is drawn by the layer",
             (int)ok);
}

// The origin term is the display x at which X4's surface starts, because the
// fold is really `x_x4 = (x_sdl + origin) mod W` -- undoing "where on the
// display is this pointer" back to "where in X4's frame".
//
// It defaults to W/2, and that default encodes a geometry rather than a law:
// a 2W-wide side-by-side composite with X4's W-wide surface centred in it,
// which is what this project runs. **1408x1408 is this machine's convenience,
// not the mod's.** An eye is whatever size the headset asks for, it need not
// be square, and it will differ per user -- so W is read from the
// SDL_GetWindowSize calls X4 makes, and the origin is overridable rather than
// computed from an assumed layout.
//
// X4VR_INPUT_FOLD_ORIGIN sets it explicitly for any composite that is not
// centred-2W. Only x is folded: a side-by-side layout duplicates horizontally,
// so y is 1:1. An over-under layout would need the same treatment on y, and
// would be a change here rather than a different value.
int fold_origin(int w) {
    static const int forced = [] {
        const char *e = getenv("X4VR_INPUT_FOLD_ORIGIN");
        return e && *e ? atoi(e) : -1;
    }();
    return forced >= 0 ? forced : w / 2;
}

float fold_x(float x) {
    const int w = g_x4_win_w.load(std::memory_order_relaxed);
    if (!input_fold() || w <= 1 || g_relative_mouse.load(std::memory_order_relaxed))
        return x;
    const int origin = fold_origin(w);
    float f = fmodf(x + (float)origin, (float)w);
    if (f < 0.f)
        f += (float)w;
    static bool said = false;
    if (!said) {
        said = true;
        X4VR_LOG("sdl: input fold ON — x_x4 = (x_sdl + %d) mod %d "
                 "(W from X4's own window; origin %s)",
                 origin, w,
                 getenv("X4VR_INPUT_FOLD_ORIGIN") ? "forced" : "assumed W/2");
    }
    return f;
}

// The question is the *range* X4 is handed, so measure the range.
//
// Take 88 sampled the first twelve changed positions instead, and every one of
// them landed in a 0.2s burst on the first mouse twitch, 97 seconds into a
// 123-second run: largest value 17, with the deliberate sweep to the screen
// edge happening long after the budget was gone. An instrument for "how far
// does it go" must not spend itself on "where did it start".
//
// Logging on a *new extreme* is self-limiting -- the range only ever widens, so
// this settles to silence on its own -- and it captures the edges, which is the
// only part of the range that discriminates 0..1408 from 0..2816.
// **Take 307 found two defects here, and both flattered the instrument.**
//
// The accumulator and the cap were function-level statics, so the two callers
// -- polled GetMouseState positions and motion-event positions -- SHARED one
// range and one budget. The printed "GetMouseState extent" was therefore
// widened by motion events and vice versa, and take 307 spent exactly 64
// lines (39 + 25) between them and went silent while the real range kept
// growing: its last line read x=0..1738 y=0..1806 where the cursor census
// measured 1898/2024. A truncated range that stops announcing itself is worse
// than no range, because it still looks like an answer.
//
// State is per caller now, keyed on the literal `what` pointer -- every call
// site passes a string literal, so the pointer is a stable identity and no
// allocation or comparison is needed. A caller that passed a computed string
// would get its own slot per call and log once each, which is loud rather
// than silent, and that is the right way round.
struct ExtentState {
    const char *what = nullptr;
    float lo_x = 1e9f, hi_x = -1e9f, lo_y = 1e9f, hi_y = -1e9f;
    int logged = 0;
};
ExtentState g_extents[4];

void note_extent(const char *what, float x, float y) {
    ExtentState *s = nullptr;
    for (auto &c : g_extents) {
        if (c.what == what) { s = &c; break; }
        if (!c.what) { c.what = what; s = &c; break; }
    }
    if (!s)
        return; // more callers than slots; silent rather than wrong
    const bool wider = x < s->lo_x || x > s->hi_x || y < s->lo_y || y > s->hi_y;
    // The cap bounds LINES, not the range: the bounds keep widening after it
    // is reached so a later reader -- or the final report -- still gets the
    // true extent, which is the half take 307 lost.
    s->lo_x = x < s->lo_x ? x : s->lo_x;
    s->hi_x = x > s->hi_x ? x : s->hi_x;
    s->lo_y = y < s->lo_y ? y : s->lo_y;
    s->hi_y = y > s->hi_y ? y : s->hi_y;
    if (!wider)
        return;
    if (s->logged == 64) {
        s->logged++;
        X4VR_LOG("sdl: %s extent — 64 lines, going quiet; the range keeps "
                 "widening and the final value is reported at teardown", what);
        return;
    }
    if (s->logged > 64)
        return;
    s->logged++;
    X4VR_LOG("sdl: %s extent x=%.0f..%.0f y=%.0f..%.0f (now %.0f,%.0f)", what,
             s->lo_x, s->hi_x, s->lo_y, s->hi_y, x, y);
}

// The true extent per caller, so a capped run still reports the range it
// actually saw. Rides the periodic cadence and reprints only when a bound has
// moved, which makes it self-limiting the way the per-line logging was meant
// to be.
void note_extent_report() {
    for (auto &c : g_extents) {
        if (!c.what || c.hi_x < c.lo_x)
            continue;
        static float seen[4][4];
        const size_t i = (size_t)(&c - g_extents);
        if (seen[i][0] == c.lo_x && seen[i][1] == c.hi_x &&
            seen[i][2] == c.lo_y && seen[i][3] == c.hi_y)
            continue;
        seen[i][0] = c.lo_x; seen[i][1] = c.hi_x;
        seen[i][2] = c.lo_y; seen[i][3] = c.hi_y;
        X4VR_LOG("sdl: %s extent FINAL x=%.0f..%.0f y=%.0f..%.0f (%d line(s) "
                 "logged)", c.what, c.lo_x, c.hi_x, c.lo_y, c.hi_y, c.logged);
    }
}

// Every key event X4 drains, capped so the log stays readable. Take 121 left
// two possibilities standing that no amount of reasoning could separate: our
// synthetic event never reaching X4's queue, or reaching it and not matching a
// binding. A real press logged in the same format next to ours settles it by
// comparison rather than by argument -- which is what should have happened
// before the keycode was guessed at, and again before the scancode was.
void note_key_event(const Sdl3KeyEvent *k, const char *via, int action) {
    // Separate budgets. Take 122's cap was spent entirely on our own key
    // repeating at ~2 Hz, so not one of Patola's real presses was logged and
    // the comparison this exists for could not be made. An instrument that
    // crowds out its own subject is the take-115 range ceiling again: a few of
    // ours is proof it arrives, and the rest of the budget belongs to the keys
    // being compared against.
    // Ours and a REAL press of the same key are indistinguishable by
    // scancode, so the budget is split by timestamp instead: SDL stamps a
    // genuine event, and ours goes in as zero unless SDL fills it.
    const bool ours = k->reserved == kOurEventMagic;
    // `raw` is the platform scancode -- the evdev code on Linux -- and ours is
    // zero because we have no way to derive it. It is the last field that still
    // differs from a real press, so a real one of OUR key donates it, exactly
    // as a real motion event donates windowID.
    if (!ours && (int)k->scancode == headlook_scancode() && k->raw &&
        !g_key_raw_seen.exchange(true, std::memory_order_relaxed)) {
        g_key_raw.store(k->raw, std::memory_order_relaxed);
        X4VR_LOG("headlook: sampled raw=%u from a real press of scancode %d; "
                 "synthetic keys will carry it",
                 (unsigned)k->raw, headlook_scancode());
    }
    // Track the free-look keypad BEFORE the budget below, and log every
    // transition uncapped. The budget exists to stop one repeating key from
    // crowding out the comparison; these are the subject of the measurement and
    // there are only a handful of them, so they must not be rationed. Take
    // 148's scorer took its window from the keys instead of from X4's own
    // reported state and got the verdict wrong, so these stamps segment the
    // sample stream and nothing more -- the angles are the measurement.
    for (unsigned i = 0; i < kNumPanKeys; i++) {
        if ((int)k->scancode != kPanKeys[i].scancode)
            continue;
        const unsigned bit = 1u << i;
        const unsigned was = g_pan_keys.load(std::memory_order_relaxed);
        const unsigned now = k->down ? (was | bit) : (was & ~bit);
        if (now != was) {
            g_pan_keys.store(now, std::memory_order_relaxed);
            X4VR_LOG("panprobe: key %s %s (scancode %u, %s)", kPanKeys[i].name,
                     k->down ? "DOWN" : "UP", k->scancode,
                     ours ? "ours" : "real");
        }
        break;
    }

    // **An auto-repeat carries no information, and it crowded out the subject
    // AGAIN.** Take 122's fix split this budget by ours/theirs so our own
    // repeating key could not spend it all. Take 269 shows that was half the
    // problem: 27 of the 40 events were `repeat=1` from two REAL held keys (x
    // and z), the budget ran out 243 s before the log ended, and every M and
    // Esc press Patola made fell in that blind window. score_run then reported
    // "map NOT EXERCISED" for a run whose map was exercised twice — an
    // instrument's silence read as the subject's absence.
    //
    // A held key's first DOWN (repeat=0) and its UP are still logged, so
    // nothing that identifies a press is lost.
    if (!ours && k->repeat)
        return;
    static int seen_ours = 0, seen_other = 0;
    if (ours) {
        if (seen_ours >= 3)
            return;
        seen_ours++;
    } else {
        // 40 was sized for the two-minute takes of #33's bring-up. Sessions
        // now run six minutes through several menus; the cap exists to stop a
        // flood, not to ration a whole run.
        if (seen_other >= 200)
            return;
        seen_other++;
    }
    // timestamp and action are the two fields take 123 could not rule out.
    // A zero timestamp is the obvious way a synthetic event differs from a real
    // one even when every other field matches, and X4 may treat it as stale;
    // action distinguishes a peek (1) from a consuming get (2), and a binding
    // that only fires on the latter would look exactly like this.
    X4VR_LOG("key[%s%s] type=0x%x scancode=%u keycode=0x%x mod=0x%x which=%u "
             "window=%u down=%d repeat=%d raw=%u ts=%llu action=%d",
             via, ours ? ":OURS" : "", k->type, k->scancode, k->key, k->mod, k->which, k->windowID,
             (int)k->down, (int)k->repeat, (unsigned)k->raw,
             (unsigned long long)k->timestamp,
             action);
    (void)ours;
}

void note_mouse_event(const Sdl3MouseEvent *e) {
    // #33: the first real motion event donates the two fields our synthetic
    // ones must carry. Done here because every mouse event already flows
    // through this function.
    if (e->type == SDL_EV_MOUSE_MOTION &&
        !g_motion_seen.load(std::memory_order_relaxed)) {
        g_motion_window.store(e->windowID, std::memory_order_relaxed);
        g_motion_which.store(e->which, std::memory_order_relaxed);
        g_motion_seen.store(1, std::memory_order_relaxed);
        X4VR_LOG("headlook: sampled a real motion event — window %u, mouse %u; "
                 "synthetic events will carry the same",
                 e->windowID, e->which);
    }
    static int motions = 0, buttons = 0;
    const bool is_motion = e->type == SDL_EV_MOUSE_MOTION;
    // ABOVE the sample caps, deliberately. Putting it below would let the
    // eight-motion budget switch off the instrument that measures the range --
    // the same shape as the bindless counter that sat under the gate removing
    // its own population, and the reason take 88 measured a maximum of 17.
    note_extent(is_motion ? "motion" : "button", e->x, e->y);

    // Is the pointer *confined* to X4's surface, or does it simply leave and
    // stop being reported? Task #31 assumed the former and never tested it, and
    // the answer decides what that task is: defeating a confinement, or moving
    // a surface.
    //
    // The two differ observably. If something pins the pointer to the surface,
    // pushing past the edge keeps delivering motion with non-zero xrel while x
    // stays at the wall -- you are pushing against something. If instead the
    // pointer is free and Wayland just stops sending events once it is off the
    // surface, motion ceases entirely and x freezes because nothing updates it.
    //
    // Raw SDL values: the fold is applied by the callers *after* this runs.
    if (is_motion) {
        const int w = g_x4_win_w.load(std::memory_order_relaxed);
        if (w > 1) {
            // Per-wall budgets. Take 91 used one counter for both and spent it
            // entirely on the left wall before the right wall was ever reached,
            // so the right wall's silence was a budget artifact and not a
            // measurement -- the same mistake as take 88's sampler, made again
            // in the probe written to explain it. A shared budget between two
            // things being compared cannot compare them.
            const bool left = e->x <= 0.5f;
            const bool right = e->x >= (float)(w - 1) - 0.5f;
            static int pins_l = 0, pins_r = 0, frees = 0;
            int &pins = left ? pins_l : pins_r;
            if ((left || right) && fabsf(e->xrel) > 0.5f && pins < 6) {
                pins++;
                X4VR_LOG("sdl: wall push %s#%d — x=%.0f pinned, xrel=%.1f still "
                         "arriving: this wall CONFINES",
                         left ? "LEFT " : "RIGHT ", pins, e->x, e->xrel);
            } else if (!left && !right && frees < 1) {
                frees++;
                X4VR_LOG("sdl: motion away from the walls (x=%.0f) — the probe "
                         "is live, so a later absence of wall pushes is real",
                         e->x);
            }
        }
    }
    if (is_motion && motions >= 8)
        return;
    if (!is_motion && buttons >= 6)
        return;
    (is_motion ? motions : buttons)++;
    X4VR_LOG("sdl: mouse %s x=%.1f y=%.1f xrel=%.1f yrel=%.1f win=%u",
             is_motion ? "motion" : (e->type == SDL_EV_MOUSE_DOWN ? "down" : "up"),
             e->x, e->y, is_motion ? e->xrel : 0.f, is_motion ? e->yrel : 0.f,
             e->windowID);
}

// SDL_Window is opaque here on purpose -- the injector has no SDL headers and
// needs none; the handle is only ever passed straight through.
// ------------------------------------------------------------------ Lua
//
// Reconnaissance for #33's open loop: see injector/x4vr_lua.hpp for why this is
// the route and why it is the only one that keeps saves unmodified.
//
// **`static`, not an anonymous namespace.** The first version of this block
// opened one and closed it in the wrong place, which silently moved every real
// interposer -- fopen, open, the whole SDL family -- inside it, where internal
// linkage meant they interposed nothing at all. The injector then did nothing
// but log Lua for a whole run. Nothing in this file needs a namespace it does
// not already have; `static` gives internal linkage without a brace to get
// wrong.
static bool lua_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_LUA");
        return e && *e && *e != '0';
    }();
    return on;
}

// Two lessons from #33 are built in rather than learned again. **An import is
// not a call** -- X4 imports SDL_GetKeyboardState and never calls it -- so the
// first firing of each hook is logged before anything is built on it. And an
// instrument must not flood its own log, which cost takes 119 and 122: chunk
// names are capped, and the keyword scan reports the chunk rather than the
// match.
// Where to write chunks that hit a probe, or empty for none.
//
// **/tmp only, and never committed.** X4's Lua is Egosoft's copyrighted source,
// exactly like the shader modules this project already dumps and has always
// kept out of the tree. The knob takes a directory so the path is the caller's
// choice and nothing defaults into the repo.
static const char *lua_dump_dir() {
    static const char *d = [] {
        const char *e = getenv("X4VR_LUA_DUMP");
        return (e && *e) ? e : "";
    }();
    return d;
}

// Chunk names are paths; flatten them so one directory holds the lot.
static void lua_dump_chunk(const char *name, const char *buff, size_t sz) {
    const char *dir = lua_dump_dir();
    if (!*dir || !buff || !sz)
        return;
    std::string flat(name ? name : "anon");
    for (char &c : flat)
        if (c == '/' || c == '\\' || c == '@' || c == '=')
            c = '_';
    // Make the directory ourselves. Take 133 asked Patola to mkdir it first and
    // he had no reason to remember, so 24 chunks hit and nothing was written --
    // the same shape as gating head-look arming on a mouse nudge. A tool should
    // not put a precondition on the person running it when it can satisfy it.
    static bool made = false;
    if (!made) {
        made = true;
        if (::mkdir(dir, 0755) != 0 && errno != EEXIST)
            X4VR_LOG("lua: WARNING cannot create %s (%s) — nothing will be "
                     "dumped",
                     dir, strerror(errno));
    }

    const std::string path = std::string(dir) + "/" + flat;
    // write_file() goes through open()/write() rather than fopen(), which
    // matters here for the same reason it does for the config profile: fopen is
    // interposed by this very file.
    if (x4vr::write_file(path.c_str(), std::string(buff, sz)))
        X4VR_LOG("lua: dumped %s (%zu bytes)", path.c_str(), sz);
    else
        // Reported, because take 133 failed here in total silence: 24 hits, 0
        // dumps, and a log that read as if nothing had gone wrong.
        X4VR_LOG("lua: WARNING could not write %s (%s)", path.c_str(),
                 strerror(errno));
}

// For the Lua link (take 328 on): WHICH Lua state each chunk loads into --
// the crosshair code and the right-click menu may live in different ones, and
// an add-on reaches only "menus" -- and whether X4 hands over source or LuaJIT
// bytecode ("\x1bLJ"), which decides whether a chunk can be patched as text.
static void note_lua_chunk(void *L, const char *buff, size_t sz,
                           const char *name, const char *via) {
    static int chunks = 0, hits = 0;
    const char *n = x4vr::lua_chunk_name(name);
    const char *kind = !buff ? "by path"
                       : (sz >= 3 && buff[0] == '\x1b' && buff[1] == 'L' &&
                          buff[2] == 'J')
                           ? "bytecode"
                           : "source";
    if (chunks < 2000) {
        chunks++;
        X4VR_LOG("lua[%s] %s (%zu bytes, %s, L=%p)", via, n, sz, kind, L);
    } else if (chunks == 2000) {
        chunks++;
        X4VR_LOG("lua: 2000 chunk names logged, capping — the keyword scan "
                 "below keeps running");
    }
    if (!buff || hits >= 60)
        return;
    size_t np = 0;
    const x4vr::LuaProbe *probes = x4vr::lua_probes(&np);
    for (size_t i = 0; i < np && hits < 60; i++) {
        if (!x4vr::lua_chunk_mentions(buff, sz, probes[i].needle))
            continue;
        hits++;
        X4VR_LOG("lua HIT %-14s in %s  (%s)", probes[i].needle, n, probes[i].why);
        static std::string &last_dumped = x4vr::immortal<std::string>();
        if (last_dumped != n) { // one dump per chunk, not one per probe
            last_dumped = n;
            lua_dump_chunk(n, buff, sz);
        }
    }
}

// LuaJIT is Lua 5.1: int luaL_loadbuffer(lua_State*, const char*, size_t,
// const char*). lua_State is opaque here, as SDL_Window is elsewhere in this
// file -- the injector has no Lua headers and does not need them.
extern "C" int luaL_loadbuffer(void *L, const char *buff, size_t sz,
                               const char *name) {
    static auto real_fn =
        real<int (*)(void *, const char *, size_t, const char *)>(
            "luaL_loadbuffer");
    if (lua_enabled() && this_is_the_game()) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("lua: luaL_loadbuffer IS called — X4's Lua is reachable "
                     "from here");
        }
        note_lua_chunk(L, buff, sz, name, "buf");
    }
    return real_fn(L, buff, sz, name);
}

extern "C" int luaL_loadfile(void *L, const char *filename) {
    static auto real_fn = real<int (*)(void *, const char *)>("luaL_loadfile");
    if (lua_enabled() && this_is_the_game()) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("lua: luaL_loadfile IS called — chunks also arrive by path");
        }
        note_lua_chunk(L, nullptr, 0, filename, "file");
    }
    return real_fn(L, filename);
}

// ---- X4's own camera, read back -------------------------------------------
//
// `Rotation GetCameraRotation();` is EXPORTED from the X4 binary (nm -D, a T
// symbol at 0xa41610), and the Lua bytecode carries its FFI declaration:
//
//     typedef struct { float yaw; float pitch; float roll; } Rotation;
//
// Field order taken from the declaration itself, not from a sorted listing --
// getting that backwards is exactly the positional error this project keeps
// making.
//
// **This is what #33 has been missing.** Every remaining head-look defect is one
// defect: we drive open-loop and cannot ask X4 where its camera points. The
// unmeasured gain, the real windup at X4's own clamp, and the undetected
// recentre on a seat change or save load are all that same blindness, and a
// readback makes all three self-correcting at once.
//
// The Lua reconnaissance found this and is not needed to use it: the function is
// a plain exported symbol, so no chunk is injected and no game file is touched.
// There is no SetCameraRotation -- the exported setters switch view MODE
// (cockpit, external, target), not orientation -- so this is a readback and not
// direct control.
//
// **Logged before it is trusted.** Which frame it reports in, degrees or
// radians, and whether it follows free-look at all are unknown; nothing here
// assumes an answer. Gated behind X4VR_CAMREAD so calling an unknown game
// function every frame is opt-in and reversible.
struct X4Rotation {
    float yaw, pitch, roll;
};

// X4 reports radians; every angle on our side is degrees.
static constexpr float kRad2Deg = 57.2957795130823f;

// Whether the readback DRIVES the loop or is merely reported. Separate from
// X4VR_CAMREAD so a run can observe without changing behaviour, which is how
// take 135 established the units without risking anything.
static const bool g_camloop = [] {
    const char *e = getenv("X4VR_CAMLOOP");
    return e && *e && *e != '0';
}();

// **Ask X4 which view it is in, instead of inferring it from motion.**
//
// Both of these are exported (nm -D) and declared in the Lua cdefs as returning
// bool. Inferring context from "did the camera respond" cost takes 139 and 140:
// first it could not tell a converged servo from a dead one, then it became a
// trap door. A direct question does not have either failure mode.
//
// Absent symbols mean "assume cockpit", which is the pre-existing behaviour.
// These three do **not** cover the map -- take 146 measured that directly -- so
// x4_hud_active() below carries that case, and the reasoning for splitting it
// out rather than adding it here is written there.
// **Which of the three said "not the cockpit".** Take 329: the ESC menu
// PAUSES the game (take 146 had already found that), and the one log line
// "view is now external/floating" said otherwise for 13 takes -- so
// X4VR_HEADLOOK_MENU_CENTRE, gated on the cockpit, never saw the ESC menu as a
// menu in the cockpit, which it is. Named apart so a consumer can say which.
enum class X4View { Cockpit, External, Floating, Paused };
static X4View x4_view() {
    using Fn = bool (*)();
    static Fn ext = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "IsExternalViewActive");
        X4VR_LOG("camread: IsExternalViewActive %s",
                 f ? "resolved" : "NOT FOUND");
        return f;
    }();
    static Fn flt = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "IsFloatingViewActive");
        X4VR_LOG("camread: IsFloatingViewActive %s",
                 f ? "resolved" : "NOT FOUND");
        return f;
    }();
    // **Only functions whose FFI declaration we have actually read.**
    //
    // IsFullscreenMenuDisplayed was called here on the strength of its name in
    // `nm -D` alone, with a bool() signature I invented, and it segfaulted X4 --
    // the core dump lands on it, one frame inside x4_cockpit_view (now x4_view). The other
    // three are declared verbatim in X4's own Lua bytecode:
    //
    //     bool IsExternalViewActive();
    //     bool IsFloatingViewActive(void);
    //     bool IsGamePaused(void);
    //
    // A symbol table gives a name and an address. It does not give arity,
    // argument types, or whether the thing is safe to call with no context, and
    // calling into a game with a guessed prototype corrupts the stack. The rule
    // is now explicit: if the cdef has not been read, the function is not
    // called.
    static Fn paused = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "IsGamePaused");
        X4VR_LOG("camread: IsGamePaused %s", f ? "resolved" : "NOT FOUND");
        return f;
    }();
    if (ext && ext())
        return X4View::External;
    if (flt && flt())
        return X4View::Floating;
    if (paused && paused())
        return X4View::Paused;
    return X4View::Cockpit;
}

// **Is the cockpit HUD up?** The question the other three cannot answer.
//
// Take 146 settled it as a measurement rather than an assumption. The map was
// open for 9.2 seconds -- M down at 25223.677, up at 25232.843 -- and
// the view predicate (now x4_view()) never once said anything but "the cockpit", so we kept
// steering and the map flew away. X4's map is not an external view, is not
// floating, and -- the part that makes those three useless here -- does **not**
// pause the game; it runs live. The two "view is now" transitions in that log
// are the ESC pause menu, which IsGamePaused does catch. Counting transitions
// reported "covered" about an event that was not the map at all.
//
// `bool IsHUDActive();` is declared verbatim in X4's own Lua cdefs and exported
// at 0xa431d0, 192 bytes from IsExternalViewActive -- the same view-state
// module. It reads three live bytes off a global, so it is a real query and not
// one of the VR stubs.
//
// **It must not be called early.** Its disassembly loads that global and
// dereferences it with no null check at all:
//
//     mov    0x3373c71(%rip),%rdx
//     movzbl 0x418(%rdx),%eax        <- no test/je, unlike its neighbours
//
// IsExternalViewActive tests its pointer before using it; this one does not.
// (This comment used to say IsInPanelMode tests its pointer too. It tests the
// SECOND-level pointer and dereferences the global unchecked -- take 316's
// exit crash, the same class as this one. See x4_panel_mode.) Calling it before X4 has built the object is precisely
// the crash we just removed, arriving by a different door. So the caller asks
// only once X4 is in relative mouse mode -- a state X4 enters for mouselook and
// cannot reach before the HUD exists. Being declared is necessary and it is not
// sufficient; the disassembly is the other half.
//
// **And it must not be called LATE either.** X4 NULLs that same global during
// shutdown and keeps pumping events, so the latch in headlook_tick ("once seen
// alive it stays alive") held all through play and failed at exit: both
// surviving cores fault at IsHUDActive+7 with rdx=0, called from here. That was
// the "X4 segfaults on exit in every run" the docs carried as harmless. So the
// pointer is read first, from the address decoded out of IsHUDActive's own
// first instruction (x4vr_riprel.hpp), and a NULL is not dereferenced. If the
// prologue ever stops decoding, that is said once and the call goes ahead as
// before -- the old behaviour, not a guess.
//
// **A NULL answers "no HUD", not "HUD up".** This guard first answered true,
// "like the not-yet-built case" -- but the caller's not-yet-built `true` is a
// BOOTSTRAP (head-look must press the key once to reach relative mouse mode at
// all), never a statement that a HUD exists, and this function is only called
// after that latch. Take 318 showed the cost: quitting through the menu, the
// global went NULL and in the same millisecond the log said "HUD is now up --
// steering", so head-look held MOUSELOOK and pushed mouse deltas into a
// quitting X4 for five seconds, and the pose went identity with head-look
// claiming to be live -- the scorer's one FAIL. A load that NULLs it would be
// worse: the key pressed during loading is take 126's lost edge. With no HUD
// object there is no HUD; head-look idles until the global comes back.
// X4GUARD: IsHUDActive
using X4HudFn = bool (*)();
static X4HudFn x4_hud_fn() {
    static X4HudFn hud = [] {
        X4HudFn f = (X4HudFn)dlsym(RTLD_DEFAULT, "IsHUDActive");
        X4VR_LOG("camread: IsHUDActive %s", f ? "resolved" : "NOT FOUND");
        return f;
    }();
    return hud;
}
// The global IsHUDActive dereferences, decoded from its first instruction, or
// nullptr when the prologue does not decode -- in which case nothing guards a
// call made before X4 built the object. Write-mode head-look asks this before
// calling at all, because it never enters relative mouse mode and so never
// sets the latch the MOUSELOOK path relies on.
static const void *const *x4_hud_slot() {
    static const void *const *slot = [] {
        const X4HudFn hud = x4_hud_fn();
        const void *const *s = x4vr::riprel_rdx_load_slot((const void *)hud);
        if (hud)
            X4VR_LOG("camread: IsHUDActive reads its global at %p — %s", s,
                     s ? "guarded: a NULL global is not dereferenced"
                       : "prologue did NOT decode as mov disp32(%rip),%rdx, "
                         "so the teardown guard is OFF and exit may crash");
        return s;
    }();
    return slot;
}
static bool x4_hud_active() {
    const X4HudFn hud = x4_hud_fn();
    const void *const *slot = x4_hud_slot();
    if (!hud)
        return true;
    if (slot && !*slot) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("camread: X4's HUD global is NULL — X4 is tearing the "
                     "HUD down (exit or load); IsHUDActive is not called "
                     "while it stays NULL, and the HUD counts as DOWN");
        }
        return false;
    }
    return hud();
}

// Reported, never acted on. `bool IsInPanelMode(void);` is declared and null-
// safe, and sits beside IsFloatingViewActive, but what X4 counts as "panel
// mode" is a guess until a log says. If IsHUDActive turns out not to cover the
// map, this is the next candidate and the run that shows it will already have
// the evidence -- rather than costing another take to go and look.
//
// X4GUARD: IsInPanelMode. **Take 316 crashed here on exit**, one call over from
// the IsHUDActive guard that had just held: `mov 0x3369551(%rip),%rax;
// mov 0x8(%rax),%rdx` with rax=0. It null-checks the SECOND pointer, not the
// global. Same rule as IsHUDActive, so the same guard: read the global its
// first instruction loads (`mov disp32(%rip),%rax`) and do not call while it
// is NULL. tests/run-x4-call-guards.sh disassembles every X4 function this
// file calls and fails if one dereferences its global unchecked without an
// X4GUARD tag -- a backtrace names a RULE, not a location.
static bool x4_panel_mode() {
    using Fn = bool (*)();
    static Fn panel = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "IsInPanelMode");
        X4VR_LOG("camread: IsInPanelMode %s", f ? "resolved" : "NOT FOUND");
        return f;
    }();
    static const void *const *slot = [] {
        const void *const *s = x4vr::riprel_mov_load_slot((const void *)panel, 0);
        if (panel)
            X4VR_LOG("camread: IsInPanelMode reads its global at %p — %s", s,
                     s ? "guarded: a NULL global is not dereferenced"
                       : "prologue did NOT decode as mov disp32(%rip),%rax, "
                         "so the teardown guard is OFF and exit may crash");
        return s;
    }();
    if (!panel)
        return false;
    if (slot && !*slot)
        return false; // X4 has torn its UI state down (exit or load)
    return panel();
}

// **X4VR_CAMSCAN=1 — where does X4 keep its free-look angle? Read-only.**
//
// Head-look holds MOUSELOOK because X4 exposes no camera SETTER, and holding
// MOUSELOOK is what kills cockpit hover (takes 312/313). Patola asked for the
// view to be moved "by our own means". The survey that closed the door looked
// for setter functions; it never looked at the state. This finds the state:
// see x4vr_camscan.hpp for the matching, and the reasoning.
//
// The objects scanned are the ones GetCameraRotation itself reads: its root
// global (decoded from its own `mov disp32(%rip),%rax` at +0xd, refusing any
// other form) and the camera node at root+0x3e8, plus every pointer-looking
// slot in the first 1 KB of each, one level deep. Any of those can be freed or
// moved under us, so NOTHING here dereferences X4's heap directly:
// process_vm_readv on our own pid answers an unmapped address with EFAULT
// instead of a fault, and a failed read is simply a region missing that sample.
static bool camscan_enabled() {
    static const bool on = x4vr::env_on("X4VR_CAMSCAN", false);
    return on;
}

static bool self_read(uintptr_t addr, void *out, size_t n) {
    if (!addr)
        return false;
    struct iovec local {out, n};
    struct iovec remote {(void *)addr, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

namespace {
struct CamScanState {
    x4vr::camscan::Scanner scan;
    // Region paths, fixed at the first sample so a candidate's index keeps its
    // meaning: 0 = root, 1 = cam, then children as (parent index, offset).
    struct Child {
        int parent;
        uint32_t off;
    };
    std::vector<Child> children;
    bool mapped = false;
    uint64_t reports = 0;
    size_t last_survivors = (size_t)-1;
    size_t bytes_last = 0;
    int regions_last = 0;
};
CamScanState &g_camscan = x4vr::immortal<CamScanState>();
constexpr size_t kScanRootBytes = 0x2000, kScanChildBytes = 0x800;
constexpr uint32_t kScanPtrWindow = 0x400;
constexpr size_t kScanMaxChildren = 48;
} // namespace

static void camscan_tick() {
    if (!camscan_enabled())
        return;
    // ~10 Hz: the answer needs many DISTINCT angles, not many samples.
    static timespec last{};
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double dt = (double)(now.tv_sec - last.tv_sec) +
                      (double)(now.tv_nsec - last.tv_nsec) * 1e-9;
    if (dt < 0.1)
        return;
    last = now;

    using Fn = X4Rotation (*)();
    static Fn get = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
    static const void *const *root_slot = [] {
        const void *const *s =
            get ? x4vr::riprel_mov_load_slot((const uint8_t *)(void *)get + 0xd,
                                             0)
                : nullptr;
        X4VR_LOG("camscan: GetCameraRotation %s, root global %p — %s",
                 get ? "resolved" : "NOT FOUND", (const void *)s,
                 s ? "scanning the objects it reads (read-only, EFAULT-safe)"
                   : "its +0xd did NOT decode as mov disp32(%rip),%rax, so "
                     "the scan is OFF rather than aimed at a guess");
        return s;
    }();
    if (!get || !root_slot)
        return;

    uintptr_t root = 0, cam = 0;
    if (!self_read((uintptr_t)root_slot, &root, 8) || !root)
        return; // X4 has not built it yet, or has torn it down
    if (!self_read(root + 0x3e8, &cam, 8) || !cam)
        return;
    // Same order as GetCameraRotation: the angle, then the memory, both on
    // X4's thread between frames, so they describe the same instant.
    const X4Rotation r = get();

    CamScanState &st = g_camscan;
    const uintptr_t bases[2] = {root, cam};
    if (!st.mapped) {
        st.mapped = true;
        for (int parent = 0; parent < 2; parent++) {
            uint8_t win[kScanPtrWindow];
            if (!self_read(bases[parent], win, sizeof win))
                continue;
            for (uint32_t off = 0; off + 8 <= kScanPtrWindow; off += 8) {
                uintptr_t v;
                memcpy(&v, win + off, 8);
                // Pointer-looking: aligned, user-space, not a tiny integer,
                // not the two roots, and actually readable right now.
                if ((v & 7) || v < 0x10000 || v >= 0x800000000000ull ||
                    v == root || v == cam)
                    continue;
                uint8_t probe[16];
                if (!self_read(v, probe, sizeof probe))
                    continue;
                st.children.push_back({parent, off});
                if (st.children.size() >= kScanMaxChildren)
                    break;
            }
            if (st.children.size() >= kScanMaxChildren)
                break;
        }
        X4VR_LOG("camscan: %zu child object(s) one pointer deep from root and "
                 "cam (first %u bytes of each); scanning root and cam %zu "
                 "bytes, children %zu bytes each. The state is found only if "
                 "it lives inside this range",
                 st.children.size(), kScanPtrWindow, kScanRootBytes,
                 kScanChildBytes);
    }

    std::vector<x4vr::camscan::Region> regions(2 + st.children.size());
    size_t bytes = 0;
    int readable = 0;
    auto grab = [&](x4vr::camscan::Region &rg, uintptr_t base, size_t n) {
        rg.bytes.assign(n, 0);
        if (base && self_read(base, rg.bytes.data(), n)) {
            bytes += n;
            readable++;
        } else {
            rg.bytes.clear();
        }
    };
    regions[0].name = "root";
    regions[1].name = "cam";
    grab(regions[0], root, kScanRootBytes);
    grab(regions[1], cam, kScanRootBytes);
    for (size_t i = 0; i < st.children.size(); i++) {
        const auto &c = st.children[i];
        char nm[48];
        snprintf(nm, sizeof nm, "%s->[0x%x]", c.parent ? "cam" : "root", c.off);
        regions[2 + i].name = nm;
        uintptr_t child = 0;
        self_read(bases[c.parent] + c.off, &child, 8);
        grab(regions[2 + i], child, kScanChildBytes);
    }
    st.bytes_last = bytes;
    st.regions_last = readable;
    st.scan.observe(regions, r.yaw, r.pitch);
}

// With the 2 s report block. A summary always; candidates when there are any,
// and only when their number changed or every 10th report, so a long run
// does not bury the log.
static void camscan_report() {
    if (!camscan_enabled())
        return;
    CamScanState &st = g_camscan;
    const x4vr::camscan::Scanner &sc = st.scan;
    const auto sv = sc.survivors();
    X4VR_LOG("camscan: %llu sample(s), camera yaw span %.1f deg, pitch span "
             "%.1f deg | %d region(s) %zu bytes readable | discoveries %llu, "
             "alive %zu, eliminated %llu%s | SURVIVORS %zu",
             (unsigned long long)sc.samples(), sc.yaw_span_deg(),
             sc.pitch_span_deg(), st.regions_last, st.bytes_last,
             (unsigned long long)sc.discoveries(), sc.alive(),
             (unsigned long long)sc.eliminated(),
             sc.capped() ? ", CAPPED at the candidate limit" : "", sv.size());
    const bool say = sv.size() != st.last_survivors || (st.reports % 10) == 0;
    st.reports++;
    st.last_survivors = sv.size();
    if (!say)
        return;
    for (size_t i = 0; i < sv.size() && i < 8; i++)
        X4VR_LOG("camscan: CANDIDATE %s", sc.describe(sv[i]).c_str());
}

// **X4VR_CAMWATCH=1 — WHO writes the camera orientation GetCameraRotation
// reads? Read-only.** See x4vr_camwatch.hpp for the mechanism and why the
// value scan was not enough.
//
// Split in two on purpose. X4's thread (SDL_PumpEvents) works out WHERE to
// watch -- the key-0 entry's first rotation row in each per-frame buffer -- by
// reading X4's own structures through the decoded anchors, once a second so a
// new camera object (seat change, load) re-aims it. OUR thread arms one
// hardware write-watchpoint per X4 thread and target, drains the rings and
// logs. Nothing of ours runs inside X4's threads; each store costs X4 a debug
// trap the kernel absorbs.
// X4VR_CAMWATCH=1 is hop 1 (the snapshot entries, take 316), =2 is hop 2 (the
// camera controller and its chain of child controllers), =3 hop 2b (the
// settings object, take 318), =4 hop 3 (the camera's own transform). Each is
// kept as it was so its take stays reproducible.
static int camwatch_mode() {
    static const int m = [] {
        const char *e = getenv("X4VR_CAMWATCH");
        return e && *e ? atoi(e) : 0;
    }();
    return m;
}
static bool camwatch_enabled() { return camwatch_mode() > 0; }
static const char *camwatch_hop_name() {
    switch (camwatch_mode()) {
    case 1: return "hop 1";
    case 2: return "hop 2";
    case 3: return "hop 2b";
    case 4: return "hop 3";
    default: return "hop ?";
    }
}

namespace {
struct CamWatchShared {
    std::mutex mu;
    std::vector<x4vr::camwatch::Target> targets;
    uint64_t generation = 0;
    std::atomic<bool> started{false};
    // Camera sweep seen on X4's thread, for the "enough" milestone.
    std::atomic<float> yaw_lo{1e9f}, yaw_hi{-1e9f};
};
CamWatchShared &g_camwatch = x4vr::immortal<CamWatchShared>();
} // namespace

static bool self_read(uintptr_t addr, void *out, size_t n);

// X4's executable text, for telling return addresses from stack noise.
static void x4_text_ranges(std::vector<std::pair<uintptr_t, uintptr_t>> *out) {
    out->clear();
    if (FILE *f = fopen("/proc/self/maps", "re")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long lo, hi;
            char perms[8] = {};
            if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 &&
                perms[2] == 'x' && strstr(line, "X4 Foundations/X4\n"))
                out->push_back({(uintptr_t)lo, (uintptr_t)hi});
        }
        fclose(f);
    }
}

static void camwatch_monitor() {
    using namespace x4vr::camwatch;
    WriteWatch w;
    uint64_t gen_armed = ~0ull;
    std::vector<Target> tg;
    struct Agg {
        uint64_t n = 0;
        pid_t tid = 0;
        uint32_t buf = 0;
    };
    std::map<uint64_t, Agg> writers;
    std::map<int, uint64_t> arm_err;
    uint64_t sampled = 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> text;
    x4_text_ranges(&text);
    const pid_t self = (pid_t)syscall(SYS_gettid);
    timespec t_scan{}, t_report{};
    bool said_enough = false, said_capped = false;
    constexpr size_t kMaxEvents = 400;
    auto secs_since = [](const timespec &t) {
        timespec n{};
        clock_gettime(CLOCK_MONOTONIC, &n);
        return (double)(n.tv_sec - t.tv_sec) + (n.tv_nsec - t.tv_nsec) * 1e-9;
    };
    auto arm_threads = [&]() {
        for (pid_t tid : threads_of_self()) {
            if (tid == self || w.armed_on(tid))
                continue;
            for (const Target &t : tg) {
                if (w.events() >= kMaxEvents) {
                    if (!said_capped) {
                        said_capped = true;
                        X4VR_LOG("camwatch: event cap %zu reached — later "
                                 "threads are NOT watched", kMaxEvents);
                    }
                    return;
                }
                if (int e = w.arm(tid, t, false)) {
                    if (arm_err[e]++ == 0)
                        X4VR_LOG("camwatch: arming thread %d failed: %s "
                                 "(counted from here on)", (int)tid,
                                 strerror(e));
                }
            }
        }
    };
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_camwatch.mu);
            if (g_camwatch.generation != gen_armed) {
                gen_armed = g_camwatch.generation;
                tg = g_camwatch.targets;
                w.disarm_all();
                arm_threads();
                char b[256];
                int k = 0;
                for (const Target &t : tg)
                    k += snprintf(b + k, sizeof b - (size_t)k, "%sbuf %u%s%s%s @%p",
                                  k ? ", " : "", t.buf, t.label[0] ? " [" : "",
                                  t.label, t.label[0] ? "]" : "",
                                  (void *)t.addr);
                X4VR_LOG("camwatch: aimed at %s — %zu event(s) armed",
                         k ? b : "(nothing)", w.events());
                clock_gettime(CLOCK_MONOTONIC, &t_scan);
            }
        }
        if (secs_since(t_scan) >= 1.0) {
            arm_threads(); // threads X4 started since
            // Threads whose counter moved WRITE the camera: give them a ring.
            // Capped, because a ring is locked memory (x4vr_camwatch.hpp).
            for (const auto &p : w.writers_to_upgrade()) {
                if (w.sampling_events() >= 16)
                    break;
                if (int e = w.upgrade(p.first, p.second)) {
                    if (arm_err[e]++ == 0)
                        X4VR_LOG("camwatch: upgrading writer thread %d to "
                                 "sampling failed: %s", (int)p.first,
                                 strerror(e));
                } else {
                    X4VR_LOG("camwatch: thread %d WRITES buf %u — now "
                             "sampling it", (int)p.first, p.second.buf);
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t_scan);
        }
        w.poll([&](const Sample &s, const Target &t) {
            sampled++;
            Agg &a = writers[s.ip];
            if (a.n++ == 0) {
                a.tid = (pid_t)s.tid;
                a.buf = t.buf;
                char comm[32] = "?";
                char path[64];
                snprintf(path, sizeof path, "/proc/self/task/%u/comm", s.tid);
                if (FILE *f = fopen(path, "re")) {
                    if (fgets(comm, sizeof comm, f))
                        comm[strcspn(comm, "\n")] = 0;
                    fclose(f);
                }
                char regs[640];
                int k = 0;
                for (int i = 0; i < kNumRegs; i++)
                    k += snprintf(regs + k, sizeof regs - (size_t)k,
                                  "%s%s=%llx", i ? " " : "", reg_name(i),
                                  (unsigned long long)s.regs[i]);
                char rets[400];
                int m = 0, found = 0;
                for (uint32_t o = 0; o + 8 <= s.stack_len && found < 12; o += 8) {
                    uint64_t v;
                    memcpy(&v, s.stack + o, 8);
                    for (const auto &r : text)
                        if (v >= r.first && v < r.second) {
                            m += snprintf(rets + m, sizeof rets - (size_t)m,
                                          " +0x%x:%llx", o,
                                          (unsigned long long)v);
                            found++;
                            break;
                        }
                }
                if (!m)
                    snprintf(rets, sizeof rets, " (none in %u bytes)",
                             s.stack_len);
                X4VR_LOG("camwatch: WRITER #%zu ip=%llx (the store is the "
                         "instruction BEFORE this address) thread %u '%s' "
                         "buf %u%s%s%s | %s | stack->X4 text:%s",
                         writers.size(), (unsigned long long)s.ip, s.tid,
                         comm, t.buf, t.label[0] ? " [" : "", t.label,
                         t.label[0] ? "]" : "", regs, rets);
            }
        });
        if (secs_since(t_report) >= 2.0) {
            clock_gettime(CLOCK_MONOTONIC, &t_report);
            const uint64_t stores = w.stores();
            char top[512];
            int k = 0;
            std::vector<std::pair<uint64_t, uint64_t>> byn;
            for (const auto &kv : writers)
                byn.push_back({kv.second.n, kv.first});
            std::sort(byn.rbegin(), byn.rend());
            for (size_t i = 0; i < byn.size() && i < 6; i++)
                k += snprintf(top + k, sizeof top - (size_t)k,
                              "%s%llx x%llu", i ? ", " : "",
                              (unsigned long long)byn[i].second,
                              (unsigned long long)byn[i].first);
            uint64_t errs = 0;
            for (const auto &kv : arm_err)
                errs += kv.second;
            const float ys = g_camwatch.yaw_hi.load() - g_camwatch.yaw_lo.load();
            X4VR_LOG("camwatch: %zu event(s), %zu sampling, %llu arm "
                     "failure(s) | %llu store(s) counted, %llu sampled, %llu "
                     "LOST | %zu distinct writer ip(s)%s%s | camera yaw span "
                     "%.0f deg",
                     w.events(), w.sampling_events(), (unsigned long long)errs,
                     (unsigned long long)stores, (unsigned long long)sampled,
                     (unsigned long long)w.lost(), writers.size(),
                     k ? ": " : "", k ? top : "", ys > 0 ? ys : 0.0f);
            // Hop 1 only: hop 2's "enough" is the co-move census, which needs
            // far longer than 300 samples (they arrive in seconds).
            if (camwatch_mode() == 1 && !said_enough && sampled >= 300 &&
                ys >= 60.0f) {
                said_enough = true;
                X4VR_LOG("milestone: the camera probe has what it needs "
                         "(%llu writes sampled from %zu writer(s)). You can "
                         "quit X4 now — through the menu, please.",
                         (unsigned long long)sampled, writers.size());
            }
        }
        usleep(20000);
    }
}

// Publish a new target set to the monitor thread (both hops).
static void camwatch_publish(const std::vector<x4vr::camwatch::Target> &t,
                             const char *why) {
    std::lock_guard<std::mutex> lock(g_camwatch.mu);
    bool same = t.size() == g_camwatch.targets.size();
    for (size_t i = 0; same && i < t.size(); i++)
        same = t[i].addr == g_camwatch.targets[i].addr;
    if (same)
        return;
    g_camwatch.targets = t;
    g_camwatch.generation++;
    if (why)
        X4VR_LOG("camwatch: re-aimed (%s)", why);
    if (!t.empty() && !g_camwatch.started.exchange(true))
        std::thread(camwatch_monitor).detach();
}

// Hop 3: the camera object's own transform (+0x5a0 translation, +0x5b0..
// rotation), the last thing 0x1929f70 composes. Its sites are verified once;
// it does not need the controller's class -- the controller is only the
// reference, added when its class is verified. The transform and the mode
// (+0x880) are logged whenever the camera object changes, so the census has
// its starting values beside it.
static void camwatch_aim_hop3(uintptr_t cam, const x4vr::camwatch::Chain &c) {
    using namespace x4vr::camwatch;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static int sites = -1;
    if (sites < 0) {
        uintptr_t bad = 0;
        sites = verify_hop3_sites(rd, &bad) ? 1 : 0;
        if (sites)
            X4VR_LOG("camwatch hop3: all %zu camera-transform sites match X4 "
                     "9.00", sizeof kHop3Sites / sizeof kHop3Sites[0]);
        else
            X4VR_LOG("camwatch hop3: REFUSED — code at %p differs from the "
                     "analysed build", (void *)bad);
    }
    if (!sites)
        return;
    static uintptr_t last_cam = 0;
    if (cam != last_cam) {
        last_cam = cam;
        float t[16] = {};
        int32_t mode = -1;
        self_read(cam + kCamPos, t, sizeof t);
        self_read(cam + kCamMode, &mode, sizeof mode);
        X4VR_LOG("camwatch hop3: camera %p mode %d — translation (%.3f, %.3f, "
                 "%.3f) rows (%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f)",
                 (void *)cam, (int)mode, t[0], t[1], t[2], t[4], t[5], t[6],
                 t[8], t[9], t[10], t[12], t[13], t[14]);
    }
    camwatch_publish(camera_local_targets(cam, c), "hop 3 camera transform");
}

// Hop 2: verify the analysed code sites once, then read the controller and
// its chain every second, log it whenever it changes (class identity is in
// the vtables, which are build constants), and aim at the stored matrices.
static void camwatch_aim_hop2(uintptr_t cam) {
    using namespace x4vr::camwatch;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static int sites = -1;
    if (sites < 0) {
        uintptr_t bad = 0;
        sites = verify_hop2_sites(rd, &bad) ? 1 : 0;
        if (sites)
            X4VR_LOG("camwatch hop2: all %zu analysed code sites match X4 9.00",
                     sizeof kHop2Sites / sizeof kHop2Sites[0]);
        else
            X4VR_LOG("camwatch hop2: REFUSED — code at %p differs from the "
                     "analysed build", (void *)bad);
    }
    if (!sites)
        return;
    const Chain c = read_chain(cam, rd);
    char desc[1024];
    int k = snprintf(desc, sizeof desc, "controller %p vtable %p local %p | %zu "
                     "child(ren):", (void *)c.controller, (void *)c.vtable,
                     (void *)c.local_fn, c.children.size());
    for (size_t i = 0; i < c.children.size() && k < (int)sizeof desc - 96; i++)
        k += snprintf(desc + k, sizeof desc - (size_t)k,
                      " [%zu] %p vt %p compose %p local %p", i,
                      (void *)c.children[i].obj, (void *)c.children[i].vtable,
                      (void *)c.children[i].compose, (void *)c.children[i].local);
    static std::string &last = x4vr::immortal<std::string>();
    if (last != desc) {
        last = desc;
        X4VR_LOG("camwatch hop2: %s — %s", c.ok ? "chain" : "REFUSED", c.why);
        X4VR_LOG("camwatch hop2: %s", desc);
    }
    if (camwatch_mode() == 4) {
        camwatch_aim_hop3(cam, c);
        return;
    }
    if (!c.ok)
        return;
    if (camwatch_mode() != 3) {
        camwatch_publish(chain_targets(c), "hop 2 chain");
        return;
    }
    // Hop 2b: the settings object's transform, with the controller's sway
    // matrix as the reference. Its own sites verified once.
    static int sites_b = -1;
    if (sites_b < 0) {
        uintptr_t bad = 0;
        sites_b = verify_hop2b_sites(rd, &bad) ? 1 : 0;
        if (sites_b)
            X4VR_LOG("camwatch hop2b: settings-object sites match X4 9.00; the "
                     "global is %p", (void *)settings_global());
        else
            X4VR_LOG("camwatch hop2b: REFUSED — code at %p differs from the "
                     "analysed build", (void *)bad);
    }
    if (!sites_b)
        return;
    uintptr_t obj = 0;
    if (!self_read(settings_global(), &obj, 8) || !obj)
        return;
    static uintptr_t last_obj = 0;
    if (obj != last_obj) {
        last_obj = obj;
        float t[16] = {};
        self_read(obj + kSettingsPos, t, sizeof t);
        X4VR_LOG("camwatch hop2b: settings object %p — translation (%.3f, %.3f, "
                 "%.3f) rows (%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f)",
                 (void *)obj, t[0], t[1], t[2], t[4], t[5], t[6], t[8], t[9],
                 t[10], t[12], t[13], t[14]);
    }
    camwatch_publish(settings_targets(obj, c), "hop 2b settings");
}

// Hop 2's co-movement census, on X4's thread at ~30 Hz: for each watched
// stored matrix, does it change on the ticks the CAMERA changes? The free-look
// matrix moves with the head and with nothing else; a static one never moves;
// shake or sway moves on its own. Counts, not impressions.
namespace {
struct CoMove {
    uint64_t ticks = 0, cam = 0, with = 0, alone = 0;
    float prev[12] = {};
    bool have = false;
    // How FAR it moves, not only when: take 317's census called the sway
    // matrix "moves with the camera" because the camera moves on most ticks
    // under head-look, while its rotation never passed ~9 deg against the
    // camera's 131. A rotation's angle is acos((trace-1)/2); a translation's
    // is its distance from the first sample.
    bool is_pos = false;
    float max_rot_deg = 0.f, max_disp = 0.f, first_t[3] = {};
    float lo[9], hi[9];
    char label[24] = {};
    uintptr_t addr = 0;
    // Reads that were not a live transform (take 318: the controller read
    // after X4 freed it, all zeros, 120 deg). Counted and kept out of every
    // statistic below, so one teardown read cannot become the verdict.
    uint64_t bad = 0;
    // Hop 3: does its angle EQUAL the camera's? Over the ticks the camera is
    // at least 10 deg off centre ("far"), |angle(matrix) - angle(camera)| in
    // 1-deg bins (the last bin holds everything >= 60). How far was take
    // 317's missing question; "the same angle" is the sharper one.
    uint64_t far = 0, hist[61] = {};
};
std::vector<CoMove> &g_comove = x4vr::immortal<std::vector<CoMove>>();
} // namespace

static void camwatch_comove_tick() {
    if (camwatch_mode() < 2)
        return;
    static timespec last{}, last_rep{};
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    auto since = [&](const timespec &t) {
        return (double)(now.tv_sec - t.tv_sec) + (now.tv_nsec - t.tv_nsec) * 1e-9;
    };
    if (since(last) < 1.0 / 30.0)
        return;
    last = now;
    using Fn = X4Rotation (*)();
    static Fn get = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
    if (!get)
        return;
    std::vector<x4vr::camwatch::Target> tg;
    {
        std::lock_guard<std::mutex> lock(g_camwatch.mu);
        tg = g_camwatch.targets;
    }
    // A re-aim resets the census: the rows it compared belong to other objects.
    bool same = tg.size() == g_comove.size();
    for (size_t i = 0; same && i < tg.size(); i++)
        same = g_comove[i].addr == tg[i].addr;
    if (!same) {
        g_comove.assign(tg.size(), CoMove{});
        for (size_t i = 0; i < tg.size(); i++) {
            g_comove[i].addr = tg[i].addr;
            snprintf(g_comove[i].label, sizeof g_comove[i].label, "%s",
                     tg[i].label[0] ? tg[i].label : "?");
            g_comove[i].is_pos = strstr(g_comove[i].label, ".pos") != nullptr;
            for (int j = 0; j < 9; j++) {
                g_comove[i].lo[j] = 1e9f;
                g_comove[i].hi[j] = -1e9f;
            }
        }
    }
    static float pyaw = 0, ppitch = 0;
    static bool phave = false;
    const X4Rotation r = get();
    const float yaw = r.yaw * kRad2Deg, pitch = r.pitch * kRad2Deg;
    // 0.01 degree, not a "visible" threshold: a slow head drift must still
    // count as the camera moving, or the free-look matrix -- the one being
    // looked for -- would be scored as moving ALONE on every slow tick.
    const bool cam_moved =
        phave && (std::fabs(yaw - pyaw) + std::fabs(pitch - ppitch)) > 0.01f;
    pyaw = yaw;
    ppitch = pitch;
    phave = true;
    const float cam_angle = x4vr::camwatch::yaw_pitch_angle_deg(r.yaw, r.pitch);
    static timespec last_sample{};
    const bool sample_now = camwatch_mode() == 4 && cam_angle >= 30.f &&
                            since(last_sample) >= 5.0;
    if (sample_now)
        last_sample = now;
    for (CoMove &m : g_comove) {
        float rows[12];
        if (!self_read(m.addr, rows, sizeof rows))
            continue; // rows at +0x20,+0x30,+0x40 -> x,y,z,w each
        if (!x4vr::camwatch::live_transform(rows, m.is_pos)) {
            m.bad++;
            continue;
        }
        if (!m.is_pos && cam_angle >= 10.f) {
            const float dth = std::fabs(
                x4vr::camwatch::rotation_angle_deg(rows) - cam_angle);
            m.far++;
            m.hist[dth >= 60.f ? 60 : (int)dth]++;
        }
        // The raw pair, a few times a run, so the CONVENTION (which rows,
        // which signs) can be read off offline -- the write test needs it.
        if (sample_now && !m.is_pos)
            X4VR_LOG("camwatch hop3 sample [%s]: camera yaw %.2f pitch %.2f "
                     "roll %.2f deg (angle %.2f) | rows (%.4f %.4f %.4f | %.4f "
                     "%.4f %.4f | %.4f %.4f %.4f) angle %.2f",
                     m.label, r.yaw * kRad2Deg, r.pitch * kRad2Deg,
                     r.roll * kRad2Deg, cam_angle, rows[0], rows[1], rows[2],
                     rows[4], rows[5], rows[6], rows[8], rows[9], rows[10],
                     x4vr::camwatch::rotation_angle_deg(rows));
        // A translation target moves only if its OWN three floats do: the
        // rows read after it are the object's rotation, and counting them made
        // take 319's camera.pos -- zero, never moving -- "a translation that
        // moves with the camera".
        float d = 0.f;
        for (int j = 0; j < (m.is_pos ? 3 : 12); j++)
            d = std::fmax(d, std::fabs(rows[j] - m.prev[j]));
        const bool mat_moved = m.have && d > 1e-5f;
        if (m.have) {
            m.ticks++;
            m.cam += cam_moved;
            m.with += cam_moved && mat_moved;
            m.alone += !cam_moved && mat_moved;
        }
        if (m.is_pos) {
            if (!m.have)
                memcpy(m.first_t, rows, sizeof m.first_t);
            const float dx = rows[0] - m.first_t[0], dy = rows[1] - m.first_t[1],
                        dz = rows[2] - m.first_t[2];
            m.max_disp = std::fmax(m.max_disp, std::sqrt(dx * dx + dy * dy + dz * dz));
        } else {
            m.max_rot_deg = std::fmax(m.max_rot_deg,
                                      x4vr::camwatch::rotation_angle_deg(rows));
        }
        memcpy(m.prev, rows, sizeof rows);
        m.have = true;
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++) {
                const float v = rows[row * 4 + col];
                const int j = row * 3 + col;
                m.lo[j] = std::fmin(m.lo[j], v);
                m.hi[j] = std::fmax(m.hi[j], v);
            }
    }
    if (since(last_rep) < 2.0)
        return;
    last_rep = now;
    // Enough when every watched matrix has 60 s of ticks, with the camera
    // moving in at least 10 s of them.
    // Hop 3 also needs 10 s with the camera at least 10 deg off centre, or
    // the angle match has nothing to compare.
    static bool said_enough = false;
    if (!said_enough && !g_comove.empty()) {
        bool enough = true;
        uint64_t far = 0;
        for (const CoMove &m : g_comove) {
            enough = enough && m.ticks >= 1800 && m.cam >= 300;
            far = std::max(far, m.far);
        }
        if (camwatch_mode() == 4)
            enough = enough && far >= 300;
        if (enough) {
            said_enough = true;
            X4VR_LOG("milestone: the camera probe (%s) has what it needs. "
                     "You can quit X4 now — through the menu, please.",
                     camwatch_hop_name());
        }
    }
    for (const CoMove &m : g_comove) {
        char rng[400];
        int k = 0;
        for (int j = 0; j < 9; j++)
            k += snprintf(rng + k, sizeof rng - (size_t)k, "%s%.3f..%.3f",
                          j ? (j % 3 ? " " : " | ") : "", m.lo[j], m.hi[j]);
        char mag[64];
        if (m.is_pos)
            snprintf(mag, sizeof mag, "max displacement %.4f", m.max_disp);
        else
            snprintf(mag, sizeof mag, "max rotation %.2f deg", m.max_rot_deg);
        // The angle match, as a median and p90 of |dtheta| over the far
        // ticks, read off the 1-deg histogram (bin upper edges).
        char match[160] = "";
        if (!m.is_pos && camwatch_mode() == 4) {
            int med = -1, p90 = -1, within3 = 0;
            uint64_t acc = 0;
            for (int b = 0; b <= 60; b++) {
                acc += m.hist[b];
                if (b < 3)
                    within3 += (int)m.hist[b];
                if (med < 0 && m.far && acc * 2 >= m.far)
                    med = b;
                if (p90 < 0 && m.far && acc * 10 >= m.far * 9)
                    p90 = b;
            }
            snprintf(match, sizeof match,
                     " | match: far %llu, within 3 deg %d, |dtheta| median <%d "
                     "p90 <%d deg",
                     (unsigned long long)m.far, within3, med + 1, p90 + 1);
        }
        X4VR_LOG("camwatch hop2 co-move [%s] @%p: %llu tick(s), camera moved "
                 "in %llu, the matrix moved WITH it in %llu, ALONE in %llu | "
                 "%s | %llu not-a-transform read(s) skipped%s | 3x3 ranges %s",
                 m.label, (void *)m.addr, (unsigned long long)m.ticks,
                 (unsigned long long)m.cam, (unsigned long long)m.with,
                 (unsigned long long)m.alone, mag, (unsigned long long)m.bad,
                 match, rng);
    }
}

static void camwatch_tick() {
    if (!camwatch_enabled())
        return;
    static timespec last{};
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    using Fn = X4Rotation (*)();
    static Fn get = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
    if (!get)
        return;
    // The sweep, every tick: cheap, and it is what "enough" is judged on.
    {
        const X4Rotation r = get();
        const float y = r.yaw * kRad2Deg;
        if (y < g_camwatch.yaw_lo.load())
            g_camwatch.yaw_lo.store(y);
        if (y > g_camwatch.yaw_hi.load())
            g_camwatch.yaw_hi.store(y);
    }
    const double dt = (double)(now.tv_sec - last.tv_sec) +
                      (double)(now.tv_nsec - last.tv_nsec) * 1e-9;
    if (dt < 1.0)
        return;
    last = now;

    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static const x4vr::camwatch::Anchors anc = [&] {
        const x4vr::camwatch::Anchors a =
            x4vr::camwatch::decode_anchors((uintptr_t)(void *)get, rd);
        X4VR_LOG("camwatch: anchors %s — fetch %p, buffer-index globals %p %p "
                 "(%s)",
                 a.ok ? "OK" : "REFUSED", (void *)a.fetch, (void *)a.g0,
                 (void *)a.g1, a.why);
        return a;
    }();
    static const void *const *root_slot =
        x4vr::riprel_mov_load_slot((const uint8_t *)(void *)get + 0xd, 0);
    if (!anc.ok || !root_slot)
        return;
    uintptr_t root = 0, cam = 0;
    if (!self_read((uintptr_t)root_slot, &root, 8) || !root ||
        !self_read(root + 0x3e8, &cam, 8) || !cam)
        return;
    if (camwatch_mode() >= 2) {
        camwatch_aim_hop2(cam);
        return;
    }
    uint32_t n0 = 0, n1 = 0;
    const auto t = x4vr::camwatch::targets(cam, anc, rd, &n0, &n1);
    std::lock_guard<std::mutex> lock(g_camwatch.mu);
    bool same = t.size() == g_camwatch.targets.size();
    for (size_t i = 0; same && i < t.size(); i++)
        same = t[i].addr == g_camwatch.targets[i].addr;
    if (!same) {
        g_camwatch.targets = t;
        g_camwatch.generation++;
        X4VR_LOG("camwatch: camera %p, %zu valid buffer(s) with a key-0 "
                 "entry; the index globals name buffers %u and %u right now",
                 (void *)cam, t.size(), n0, n1);
    }
    if (!t.empty() && !g_camwatch.started.exchange(true))
        std::thread(camwatch_monitor).detach();
}

// **X4VR_HOVER_VIEW=1 — which way was X4's camera pointing when the cursor
// changed? And tell the player, in the headset, when a part is done.**
//
// Take 315 asks whether hover survives a camera turned WITHOUT MOUSELOOK, and
// the numpad is the only way to turn it so (a spring: it snaps to the stop in
// under 2 s and back on release). Patola cannot keep time in a headset -- *"my
// view is completely obstructed and I can't check a chronometer"* -- so the
// protocol is paced by the log instead of a clock: each cursor change is
// binned by the camera angle X4 reported at that frame, and when a part has
// enough of them a `milestone:` line is written, which the launcher's watcher
// relays as a desktop notification (the channel that reaches him in VR).
//
// The angle is read on X4's thread from SDL_PumpEvents, the same call path as
// the head-look read-back; GetCameraRotation tests its own pointers.
static bool hoverview_enabled() {
    static const bool on = x4vr::env_on("X4VR_HOVER_VIEW", false);
    return on;
}

namespace {
// -1 unknown, 0 straight, 1 yaw-turned, 2 pitch-turned, 3 in transit.
std::atomic<int> g_view_class{-1};
struct HoverByView {
    std::atomic<uint64_t> n[5]; // index = class + 1
} g_hover_by_view;
constexpr float kStraightDeg = 3.0f, kYawTurnedDeg = 30.0f,
                kPitchTurnedDeg = 20.0f;
constexpr uint64_t kPart1Hovers = 30, kPart2HoversPerAxis = 5;
} // namespace

static void hoverview_tick() {
    if (!hoverview_enabled())
        return;
    using Fn = X4Rotation (*)();
    static Fn get = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
    if (!get)
        return;
    const X4Rotation r = get();
    const float y = std::fabs(r.yaw * kRad2Deg), p = std::fabs(r.pitch * kRad2Deg);
    int cls;
    if (y < kStraightDeg && p < kStraightDeg)
        cls = 0;
    else if (y >= kYawTurnedDeg)
        cls = 1;
    else if (p >= kPitchTurnedDeg)
        cls = 2;
    else
        cls = 3;
    g_view_class.store(cls, std::memory_order_relaxed);

    // Milestones, each said once. Worded for the player, because the watcher
    // relays the text verbatim.
    const uint64_t straight = g_hover_by_view.n[1].load(std::memory_order_relaxed);
    const uint64_t yawn = g_hover_by_view.n[2].load(std::memory_order_relaxed);
    const uint64_t pitchn = g_hover_by_view.n[3].load(std::memory_order_relaxed);
    static bool m1 = false, m2y = false, m2p = false, m2 = false;
    if (!m1 && straight >= kPart1Hovers) {
        m1 = true;
        X4VR_LOG("milestone: PART 1 DONE (%llu hovers, view straight). Now "
                 "HOLD a numpad key - 4 or 6 (left/right), 8 or 2 (up/down) - "
                 "and hover objects while it is held.",
                 (unsigned long long)straight);
    }
    if (!m2 && yawn >= kPart2HoversPerAxis && pitchn >= kPart2HoversPerAxis) {
        m2 = true;
        X4VR_LOG("milestone: PART 2 DONE (%llu hovers turned left/right, %llu "
                 "up/down). You can quit X4 now.",
                 (unsigned long long)yawn, (unsigned long long)pitchn);
    } else if (!m2y && !m2 && yawn >= kPart2HoversPerAxis) {
        m2y = true;
        X4VR_LOG("milestone: left/right done (%llu hovers). Now hold 8 or 2 "
                 "(up/down) and hover while it is held.",
                 (unsigned long long)yawn);
    } else if (!m2p && !m2 && pitchn >= kPart2HoversPerAxis) {
        m2p = true;
        X4VR_LOG("milestone: up/down done (%llu hovers). Now hold 4 or 6 "
                 "(left/right) and hover while it is held.",
                 (unsigned long long)pitchn);
    }
}

// Cursor changes binned by view, with the cursor-set report. Printed only when
// a count moved.
static void hoverview_report() {
    if (!hoverview_enabled())
        return;
    uint64_t v[5];
    for (int i = 0; i < 5; i++)
        v[i] = g_hover_by_view.n[i].load(std::memory_order_relaxed);
    static uint64_t prev = UINT64_MAX;
    const uint64_t sum = v[0] + v[1] + v[2] + v[3] + v[4];
    if (sum == prev)
        return;
    prev = sum;
    X4VR_LOG("hover view: cursor changes with the view straight %llu | turned "
             "left/right (|yaw|>=%.0f) %llu | turned up/down (|pitch|>=%.0f) "
             "%llu | in transit %llu | view unknown %llu",
             (unsigned long long)v[1], kYawTurnedDeg, (unsigned long long)v[2],
             kPitchTurnedDeg, (unsigned long long)v[3],
             (unsigned long long)v[4], (unsigned long long)v[0]);
}

static bool camread_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_CAMREAD");
        return e && *e && *e != '0';
    }();
    return on;
}

// False if the symbol is absent, which is not an error: a future X4 could stop
// exporting it and the open-loop integrator still works without it.
static bool x4_camera_rotation(X4Rotation *out) {
    using Fn = X4Rotation (*)();
    static Fn fn = [] () -> Fn {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
        X4VR_LOG("camread: GetCameraRotation %s",
                 f ? "resolved — X4's camera can be read back"
                   : "NOT FOUND — staying open-loop");
        return f;
    }();
    if (!fn)
        return false;
    *out = fn();
    return true;
}

// -------------------------------------------- #33 pan-channel probe
//
// Characterise X4's numpad free-look -- INPUT_STATE_CAMERA_LEFT/RIGHT/UP/DOWN
// -- as a candidate rotation channel. Head-look currently holds
// INPUT_STATE_CAMERA_MOUSELOOK, and holding that is precisely what X4 uses to
// suspend ship control, so it costs the player mouse steering and the aiming
// cursor. The eight directional states never touch MOUSELOOK. If they can carry
// the head, the conflict disappears rather than being tuned around.
//
// **Driven by real fingers, not by us.** The question here is about X4's
// behaviour -- pan rate, whether a held key runs into a clamp, and above all
// what happens on release -- and a recentre on release kills the idea outright,
// at which point our ability to synthesise the key does not matter. Testing the
// thing that can falsify the plan first costs one run instead of two. It also
// removes the synthesis path as a confound: a flat log here means X4 did
// nothing, not that our event was wrong, because the keypad is on its factory
// binding and Patola pressed it himself. Only if the channel survives this does
// it become worth asking whether we can drive it -- and X4's keycode vocabulary
// holds just F13/F14/F15 above the printable keys, of which head-look already
// owns one, so that question has its own cost and belongs after this one.
//
// **What this instrument can and cannot resolve.** It samples at 50 Hz while a
// key is held and for six seconds after the last release, so: a pan rate
// between roughly 0.5 and 500 deg/s is measurable; a clamp is visible if it is
// inside the reachable range and the hold is long enough to reach it; and a
// recentre is resolved as a curve if it takes longer than ~40 ms, or seen only
// as a single-sample step if it is faster than that. A step and an instant snap
// are NOT distinguishable here, and the difference does not matter -- either
// one is fatal to the idea.
static bool panprobe_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_PANPROBE");
        return e && *e && *e != '0';
    }();
    return on;
}

// Task #73's open-loop plant identification. Replaces the servo entirely for
// the length of the run, so this is never something to leave on by accident.
static std::atomic<bool> g_plantid_done{false};

static bool plantid_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_PLANTID");
        return e && *e && *e != '0';
    }();
    return on;
}

// **Task #75: the exogenous dither, for measuring X4's latency band IN PLAY.**
//
// `plantid` takes the servo out of the loop for four minutes and cannot run
// while anyone is playing. This is a few mouse counts the servo does not know it
// is sending -- white, from an LFSR, so uncorrelated with the head, the camera
// and the command. That independence is the whole basis of closed-loop
// identification; correlating the command against the camera instead recovers
// the controller along with the plant.
//
// **A white DELTA, not the difference of a white position.** X4 integrates
// deltas, so `delta cam = P (*) u` convolves against the delta sequence and the
// instrument has to match it. Four estimators died of differencing a position
// sequence to keep its offset bounded -- see tests/dither_id.cpp. The offset does
// not need bounding: the servo is a regulator and suppresses it, and capping the
// walk destroys the whiteness the method depends on (12.6% of draws reflected at
// +-3 inside +-6, and a planted fixed delay came back 2.51 instead of 2.00).
//
// Counts per tick. 2 is what tests/dither_id.cpp measured as sufficient; 0 is off
// and is the default, because this puts a real tremor in the view.
static int dither_counts() {
    static const int n = [] {
        const char *e = getenv("X4VR_DITHER");
        const int v = (e && *e) ? atoi(e) : 0;
        return v < 0 ? 0 : (v > 16 ? 16 : v);
    }();
    return n;
}

// The same LFSR tests/dither_id.cpp validates the recovery against. Taps
// 0/2/3/5 over 16 bits; the sign is the low bit.
static int dither_next(int counts) {
    static uint16_t lfsr = 0xACE1u;
    const uint16_t bit = (uint16_t)(((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^
                                     (lfsr >> 5)) & 1u);
    lfsr = (uint16_t)((lfsr >> 1) | (bit << 15));
    return (lfsr & 1u) ? counts : -counts;
}

// Task #73's per-pose lag samples. On by its own knob, and implied by the A/B
// -- a run that asks for the comparison always produces the measurement that
// judges it, rather than depending on a second flag being remembered.
// Which arm of the kp A/B is live, published for `lagsample` to stamp. -1 when
// the A/B is not running, so the scorer can tell "one arm" from "no A/B".
static std::atomic<int> g_kp_arm{-1};
// Which ratefix arm the code TOOK, so lagsample stamps the branch rather than
// re-deriving it from a timestamp. [[record-the-branch-dont-recompute-it]].
static std::atomic<int> g_ratefix_arm{-1};
static std::atomic<int> g_ungate_arm{-1};

static bool lag_log_enabled() {
    static const bool on = [] {
        const char *e = getenv("X4VR_HEADLOOK_LAGLOG");
        if (e && *e && *e != '0')
            return true;
        const char *ab = getenv("X4VR_HEADLOOK_LEAD_AB");
        if (ab && *ab && atof(ab) > 0.0)
            return true;
        // The kp A/B's whole acceptance check is this stream. Arming one without
        // the other would produce a run that cannot be scored, which is the
        // failure take 293 already paid for.
        const char *kab = getenv("X4VR_HEADLOOK_KP_AB");
        if (kab && *kab && atof(kab) > 0.0)
            return true;
        // Same for the ratefix A/B, and for the same reason.
        const char *rab = getenv("X4VR_HEADLOOK_TAU_AB");
        if (rab && *rab && atof(rab) > 0.0)
            return true;
        const char *uab = getenv("X4VR_HEADLOOK_UNGATED_AB");
        return uab && *uab && atof(uab) > 0.0;
    }();
    return on;
}

static double mono_seconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void panprobe_tick() {
    if (!panprobe_enabled() || !this_is_the_game())
        return;
    X4Rotation r{};
    if (!x4_camera_rotation(&r))
        return; // already logged NOT FOUND once

    const double now = mono_seconds();
    static const double t0 = now;
    const unsigned keys = g_pan_keys.load(std::memory_order_relaxed);

    // Active while a key is held and for six seconds after the last release --
    // the release transient is the measurement that decides this, so it gets
    // the same cadence as the hold rather than a slower tail.
    static double last_down = -1e9;
    if (keys)
        last_down = now;
    const bool active = keys || (now - last_down) < 6.0;
    const double period = active ? 0.02 : 1.0;

    static double last_sample = -1e9;
    if (now - last_sample < period)
        return;
    last_sample = now;

    char held[80];
    size_t off = 0;
    for (unsigned i = 0; i < kNumPanKeys && off + 12 < sizeof(held); i++)
        if (keys & (1u << i))
            off += (size_t)snprintf(held + off, sizeof(held) - off, "%s%s",
                                    off ? "+" : "", kPanKeys[i].name);
    if (!off)
        snprintf(held, sizeof(held), "-");

    X4VR_LOG("panprobe: t=%.3f yaw=%.3f pitch=%.3f deg held=%s%s", now - t0,
             kRad2Deg * -r.yaw, kRad2Deg * r.pitch, held,
             active ? "" : " idle");
}

// ------------------------------------------------------- #33 head-look
//
// X4 is SDL3 and imports SDL_GetKeyboardState, so the held free-look key is a
// *polled state array with one bit forced* -- no event synthesis, no key-up to
// lose, and self-healing by construction, which is exactly what the
// hold-not-toggle decision needs. `nm -D` first, per this project's rule about
// hooking symbols the target never imports; X4 does not import SDL_PollEvent,
// which is what the dead hook described near the top of this file was.
//
// **The mouse path is measured before it is built.** X4 imports BOTH
// SDL_GetMouseState + SDL_WarpMouseInWindow (the warp-to-centre idiom, where a
// delta is the absolute position minus the centre) and
// SDL_SetWindowRelativeMouseMode (where motion arrives as events instead).
// Those need opposite implementations, and the XML gives no way to tell which
// X4 uses for free-look. So this run logs, and the next one implements the one
// that fires. Guessing here would mean writing an SDL_Event by offset against
// a header this file deliberately does not include.
// Defined below with the rest of the mouse half; declared here because the
// tick is what drives it and reads better next to the arithmetic.
void send_mouse_delta(int dx, int dy);
void send_key_down(int scancode);
static void push_our_event(void *ev);
void send_key(int scancode, bool down);

// ------------------------------------------------ X4VR_HEADLOOK_WRITE=1
//
// Head-look WITHOUT MOUSELOOK: the head's yaw/pitch written straight into X4's
// free-look state, the camera's own local matrix (camera+0x5b0..), every tick.
// See injector/x4vr_camwrite.hpp for what 0x1933d00 does with it and why the
// x4vrmod_headlook extension (anglespeed 0) makes a write stay. The first
// WRITE into X4's memory in this project: through process_vm_writev, so a
// freed camera is EFAULT, never a crash, and refused unless every site, the
// camera mode, X4's loaded parameters and the base transform say the matrix
// means what we think it means.
static bool self_write(uintptr_t addr, const void *in, size_t n) {
    if (!addr)
        return false;
    struct iovec local {const_cast<void *>(in), n};
    struct iovec remote {(void *)addr, n};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

namespace {
struct CamWrite {
    // The G-force sway, read live from the camera controller's stored matrix
    // (take 317: +0x10 translation, +0x20.. rotation, written by the physics
    // step 0x13f1970). Max per report window and over the run, so the
    // x4vrmod_nosway extension is checked by what it is meant to stop, not
    // by its parameters.
    float sway_deg = 0.f, sway_m = 0.f, sway_run_deg = 0.f, sway_run_m = 0.f;
    bool sway_seen = false;
    uint64_t writes = 0, failed = 0, refused = 0, mouselook_downs = 0;
    // X4VR_HEADLOOK_MENU_CENTRE: HUD-down periods in the cockpit that were
    // centred, and the writes that did it -- kept apart from `writes`, which
    // the scorer reads as the head's.
    uint64_t centre_periods = 0, centre_writes = 0;
    float yaw = 0.f, pitch = 0.f;
    bool have = false;
    uintptr_t armed_cam = 0;
    char last_refusal[256] = "";
};
CamWrite g_camwrite;
} // namespace

// The player's camera object the way GetCameraRotation reaches it: its root
// global, decoded from its own `mov disp32(%rip),%rax` at +0xd, then +0x3e8.
static uintptr_t x4_player_camera() {
    using Fn = X4Rotation (*)();
    static Fn get = (Fn)dlsym(RTLD_DEFAULT, "GetCameraRotation");
    static const void *const *root_slot =
        get ? x4vr::riprel_mov_load_slot((const uint8_t *)(void *)get + 0xd, 0)
            : nullptr;
    uintptr_t root = 0, cam = 0;
    if (!root_slot || !self_read((uintptr_t)root_slot, &root, 8) || !root ||
        !self_read(root + 0x3e8, &cam, 8))
        return 0;
    return cam;
}

// Said once per distinct reason, counted always.
//
// **When to tell Patola.** Take 321's first version said "still REFUSED after
// 20 s in the cockpit" during the LOAD SCREEN: X4's main menu reports the HUD
// up, so may_steer held while no player camera existed, and the parameters
// read X4's load-time defaults (2*pi, 0) for 12 s more before arming. Neither
// is a fault. So `alarm` is false for the states every session passes through
// (no camera, wrong mode), and the clock restarts whenever the reason changes:
// only the SAME reason, with a cockpit camera present, for 30 s is said.
static void camwrite_refuse(const char *why, bool alarm) {
    g_camwrite.refused++;
    static double since = 0.0;
    const double now = mono_seconds();
    if (strcmp(g_camwrite.last_refusal, why) != 0) {
        snprintf(g_camwrite.last_refusal, sizeof g_camwrite.last_refusal, "%s",
                 why);
        X4VR_LOG("headlook write: REFUSED — %s", why);
        since = now;
    }
    static bool said = false;
    if (!alarm || g_camwrite.armed_cam || said)
        return;
    if (now - since > 30.0) {
        said = true;
        X4VR_LOG("milestone: head-look write still REFUSED after 30 s with the "
                 "cockpit camera up — %.120s. Quit and tell me.", why);
    }
}

// May we write into `cam` now? The code sites once per session; the rest per
// tick, because a camera, its mode or X4's parameters can change under us.
static bool x4_game_time(double *out); // with X4VR_FIELD_AB, below

// x4vr::camwrite::SpeedOwner, applied: X4's follower speed at zero while the
// camera is in mode 0, X4's own value in every other mode (take 351). Runs
// every head-look tick in write mode, before anything checks the speed.
static void headlook_speed_tick(uintptr_t cam) {
    using namespace x4vr;
    static camwrite::SpeedOwner owner;
    if (headlook_speed_extension() || !cam)
        return;
    uintptr_t params = 0;
    float p[4] = {};
    if (!self_read(camwrite::params_global(), &params, 8) || !params ||
        !self_read(params + camwrite::kMaxPitch, p, sizeof p))
        return;
    if (p[3] == 0.f && std::fabs(p[0] - p[1]) < 1e-6f)
        return; // the load's defaults, not X4's library values yet
    if (owner.learn(p[2]))
        X4VR_LOG("headlook speed: X4's own <lookaround><internal> anglespeed "
                 "%.4f (angleacc %.4f) — held at 0 while the camera is in "
                 "mode 0 (cockpit, walking) so a written angle stays, given "
                 "back in every other mode so the external views orbit",
                 p[2], p[3]);
    int32_t mode = -1;
    if (!self_read(cam + camwatch::kCamMode, &mode, sizeof mode))
        return;
    const float want = owner.wanted(mode);
    if (want < 0.f || std::fabs(p[2] - want) < 1e-6f)
        return;
    if (!self_write(params + camwrite::kAngleSpeed, &want, sizeof want))
        return;
    uint64_t &n = want == 0.f ? owner.to_zero : owner.to_x4;
    if (n++ < 40)
        X4VR_LOG("headlook speed: camera mode %d — anglespeed %s (%.4f)",
                 (int)mode, want == 0.f ? "held at 0" : "given back to X4",
                 want);
}

static bool camwrite_ready(uintptr_t cam, float plim_deg, float ylim_deg) {
    using namespace x4vr;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static int sites = -1;
    if (sites < 0) {
        uintptr_t bad = 0;
        sites = camwatch::verify_hop3_sites(rd, &bad) &&
                        camwrite::verify_write_sites(rd, &bad)
                    ? 1
                    : 0;
        if (sites)
            X4VR_LOG("headlook write: all %zu camera and %zu write sites match "
                     "X4 9.00; the parameters object is at global %p",
                     sizeof camwatch::kHop3Sites / sizeof camwatch::kHop3Sites[0],
                     sizeof camwrite::kWriteSites / sizeof camwrite::kWriteSites[0],
                     (void *)camwrite::params_global());
        else {
            X4VR_LOG("headlook write: REFUSED for the session — code at %p "
                     "differs from the analysed build; nothing will be written",
                     (void *)bad);
            X4VR_LOG("milestone: head-look write REFUSED — this X4 is not the "
                     "analysed build. The view will not follow your head; quit "
                     "and tell me.");
        }
    }
    if (!sites) {
        g_camwrite.refused++;
        return false;
    }
    if (!cam) {
        camwrite_refuse("no player camera yet (or X4 tore it down)", false);
        return false;
    }
    int32_t mode = -1;
    if (!self_read(cam + camwatch::kCamMode, &mode, sizeof mode) || mode != 0) {
        char why[128];
        snprintf(why, sizeof why, "the camera is in mode %d, not the cockpit's 0",
                 (int)mode);
        camwrite_refuse(why, false);
        return false;
    }
    uintptr_t params = 0;
    float p[4] = {};
    if (!self_read(camwrite::params_global(), &params, 8) || !params ||
        !self_read(params + camwrite::kMaxPitch, p, sizeof p)) {
        camwrite_refuse("X4's player-camera parameters are not readable", true);
        return false;
    }
    // Take 321's load screen read maxpitch = maxyaw = 2*pi with zero speed and
    // acceleration for 12 s before the real values arrived: X4's defaults, not
    // yet the library's. A state every load passes through, not a fault.
    if (p[3] == 0.f && std::fabs(p[0] - p[1]) < 1e-6f) {
        camwrite_refuse("X4's look-around parameters are not loaded yet "
                        "(load-time defaults)", false);
        return false;
    }
    const camwrite::ParamCheck pc = camwrite::check_params(p, plim_deg, ylim_deg);
    if (!pc.ok) {
        char why[256];
        snprintf(why, sizeof why,
                 "%s (X4 reads maxpitch %.4f maxyaw %.4f anglespeed %.4f "
                 "angleacc %.4f; the servo's limits are %.0f/%.0f)",
                 pc.why, p[0], p[1], p[2], p[3], plim_deg, ylim_deg);
        camwrite_refuse(why, true);
        return false;
    }
    float b[16] = {};
    if (!self_read(cam + camwrite::kBasePos, b, sizeof b) ||
        !camwrite::base_is_identity(b, 1e-3f)) {
        char why[256];
        snprintf(why, sizeof why,
                 "the camera's base transform is not identity (translation "
                 "%.3f %.3f %.3f, row0 %.3f %.3f %.3f) — a written angle would "
                 "be offset by it, and composing it is not built",
                 b[0], b[1], b[2], b[4], b[5], b[6]);
        camwrite_refuse(why, true);
        return false;
    }
    if (cam != g_camwrite.armed_cam) {
        g_camwrite.armed_cam = cam;
        g_camwrite.last_refusal[0] = 0;
        uintptr_t att[2] = {};
        self_read(cam + 0x780, att, sizeof att);
        X4VR_LOG("headlook write: ARMED on camera %p (mode 0) — X4 reads "
                 "maxpitch %.4f maxyaw %.4f (%s), anglespeed %.4f angleacc "
                 "%.4f; base identity; attached %p ref %p. MOUSELOOK will not "
                 "be pressed",
                 (void *)cam, p[0], p[1], pc.radians ? "radians" : "degrees",
                 p[2], p[3], (void *)att[0], (void *)att[1]);
    }
    // The notice waits for play. Arming needs only the camera and X4's loaded
    // look-around parameters, and take 327 met both at ~65% of the save's
    // loading screen: writing then is harmless, announcing it then misleads.
    // Two seconds of running game time, the same gate the A/Bs use.
    static bool said = false;
    static x4vr::fieldab::GameClock clock;
    if (!said) {
        double gt = 0.0;
        if (x4_game_time(&gt) &&
            x4vr::fieldab::game_clock_step(clock, gt, mono_seconds(), 2.0, 1.0)) {
            said = true;
            X4VR_LOG("milestone: head-look write ARMED — the view now follows "
                     "your head without mouse-look.");
        }
    }
    return true;
}

// One write of the head's angle: the rows rebuilt in X4's convention, w kept.
// `counter` is the tally a successful write goes to: the head's by default.
static bool camwrite_apply(uintptr_t cam, float yaw_deg, float pitch_deg,
                           uint64_t *counter = &g_camwrite.writes) {
    float rows[12];
    const uintptr_t at = cam + x4vr::camwatch::kCamRow0;
    if (!self_read(at, rows, sizeof rows)) {
        g_camwrite.failed++;
        return false;
    }
    x4vr::camwrite::rows_for(yaw_deg, pitch_deg, rows);
    if (!self_write(at, rows, sizeof rows)) {
        g_camwrite.failed++;
        return false;
    }
    (*counter)++;
    return true;
}

// One sample of the sway: the controller at camera+0x18, its class verified
// the way hop 2 verifies it (the sites once, then vtable+0x1b0 == 0x191d0f0),
// so a load camera's controller is never read as the cockpit's.
static void camwrite_sway_sample(uintptr_t cam) {
    using namespace x4vr::camwatch;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static int sites = -1;
    if (sites < 0) {
        uintptr_t bad = 0;
        sites = verify_hop2_sites(rd, &bad) ? 1 : 0;
        if (!sites)
            X4VR_LOG("headlook write: sway readout OFF — controller code at %p "
                     "differs from the analysed build", (void *)bad);
    }
    if (!sites || !cam)
        return;
    const Chain c = read_chain(cam, rd);
    if (!c.ok)
        return;
    float m[16];
    if (!self_read(c.controller + 0x10, m, sizeof m))
        return;
    if (!live_transform(m + 4, false))
        return; // freed or mid-teardown: not a sway
    const float deg = rotation_angle_deg(m + 4);
    const float dist = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    g_camwrite.sway_seen = true;
    g_camwrite.sway_deg = std::fmax(g_camwrite.sway_deg, deg);
    g_camwrite.sway_m = std::fmax(g_camwrite.sway_m, dist);
    g_camwrite.sway_run_deg = std::fmax(g_camwrite.sway_run_deg, deg);
    g_camwrite.sway_run_m = std::fmax(g_camwrite.sway_run_m, dist);
}

static void camwrite_report(bool final) {
    static double last = 0.0;
    const double now = mono_seconds();
    if (!final && now - last < 2.0)
        return;
    last = now;
    char sway[160] = " | sway not read (no verified controller yet)";
    if (g_camwrite.sway_seen)
        snprintf(sway, sizeof sway,
                 " | sway max %.2f deg %.3f m this window, %.2f deg %.3f m run",
                 g_camwrite.sway_deg, g_camwrite.sway_m,
                 g_camwrite.sway_run_deg, g_camwrite.sway_run_m);
    X4VR_LOG("headlook write%s: %llu write(s), %llu failed, %llu refused "
             "tick(s) | last written yaw %.2f pitch %.2f | MOUSELOOK pressed "
             "%llu time(s) | menu centring %llu period(s) %llu write(s)%s",
             final ? " (final)" : "", (unsigned long long)g_camwrite.writes,
             (unsigned long long)g_camwrite.failed,
             (unsigned long long)g_camwrite.refused, g_camwrite.yaw,
             g_camwrite.pitch, (unsigned long long)g_camwrite.mouselook_downs,
             (unsigned long long)g_camwrite.centre_periods,
             (unsigned long long)g_camwrite.centre_writes, sway);
    g_camwrite.sway_deg = g_camwrite.sway_m = 0.f;
}

// ------------------------------------------------ X4VR_SHADOWFOV_AB=<s>
//
// #70's suspect switched live: X4's camera keeps a BASE field (camera+0x890,
// the 73.74 deg default) that several systems size by, while the picture uses
// option x base (+0x874). FIX phases widen the base to the rendered field;
// CONTROL phases put X4's default back. Patola watches the cockpit shadows
// across the switches, announced by notification. See x4vr_shadowfov.hpp.
static double shadowfov_period() {
    static const double p = [] {
        const char *e = getenv("X4VR_SHADOWFOV_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    return p;
}

namespace {
struct ShadowFov {
    bool armed = false;
    uintptr_t cam = 0;
    double t0 = 0.0;
    long phase = -1;
    uint64_t base_writes = 0, restores = 0, write_failed = 0;
    char refusal[200] = "";
};
ShadowFov g_sfov;
} // namespace

static void shadowfov_refuse(const char *why) {
    if (strcmp(g_sfov.refusal, why) != 0) {
        snprintf(g_sfov.refusal, sizeof g_sfov.refusal, "%s", why);
        X4VR_LOG("shadowfov: REFUSED — %s", why);
    }
}

static void shadowfov_tick() {
    using namespace x4vr::shadowfov;
    const double period = shadowfov_period();
    if (period <= 0.0)
        return;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static int sites = -1;
    if (sites < 0) {
        uintptr_t bad = 0;
        float def = 0.f;
        const bool code = verify_sites(rd, &bad);
        const bool cst = self_read(kDefaultAddr, &def, sizeof def) &&
                         std::fabs(def - kDefault) < 1e-6f;
        sites = code && cst ? 1 : 0;
        if (sites)
            X4VR_LOG("shadowfov: all %zu field sites match X4 9.00 and X4's "
                     "default field reads %.7f rad (73.74 deg)",
                     sizeof kSites / sizeof kSites[0], def);
        else {
            X4VR_LOG("shadowfov: REFUSED for the session — %s",
                     !code ? "a field site differs from the analysed build"
                           : "X4's default-field constant is not 1.2870022");
            X4VR_LOG("milestone: shadow A/B REFUSED — this X4 is not the "
                     "analysed build. Nothing will switch; quit and tell me.");
        }
    }
    if (!sites)
        return;
    static const float option = [] {
        const char *e = getenv("X4VR_FOV");
        return e && *e ? strtof(e, nullptr) : 0.0f;
    }();
    if (option <= 0.0f) {
        shadowfov_refuse("X4VR_FOV is unset — there is no widened field to "
                         "match the base to");
        return;
    }
    const uintptr_t cam = x4_player_camera();
    Read r{};
    if (!cam || !self_read(cam + kBase, &r.base, sizeof r.base) ||
        !self_read(cam + kEffective, &r.effective, sizeof r.effective))
        return;
    if (cam != g_sfov.cam) {
        // A new camera (load, seat change): its fields must read as the
        // layout says before it is touched.
        if (!layout_ok(r, option)) {
            char why[200];
            snprintf(why, sizeof why,
                     "camera %p reads base %.5f effective %.5f, not X4's "
                     "default %.5f and option %.4f x it",
                     (void *)cam, r.base, r.effective, kDefault, option);
            shadowfov_refuse(why);
            return;
        }
        g_sfov.cam = cam;
        g_sfov.refusal[0] = 0;
        X4VR_LOG("shadowfov: ARMED on camera %p — base %.5f effective %.5f "
                 "(rendered field %.5f rad = %.2f deg); phases of %.0f s, "
                 "CONTROL first",
                 (void *)cam, r.base, r.effective, rendered(option),
                 rendered(option) * 57.2957795f, period);
        if (!g_sfov.armed) {
            g_sfov.armed = true;
            g_sfov.t0 = mono_seconds();
        }
    }
    const double el = mono_seconds() - g_sfov.t0;
    const bool fix = fix_phase(el, period);
    const long phase = (long)(el / period);
    if (phase != g_sfov.phase) {
        g_sfov.phase = phase;
        X4VR_LOG("shadowfov: phase %ld %s", phase,
                 fix ? "FIX — the base field widened to the rendered field"
                     : "CONTROL — X4's default base field");
        X4VR_LOG("milestone: shadows %s (phase %ld). Watch the cockpit "
                 "shadows as you turn your head.",
                 fix ? "FIX" : "CONTROL", phase);
    }
    const Action a = decide(r, option, fix);
    if (a.write_base) {
        if (self_write(cam + kBase, &a.base, sizeof a.base))
            g_sfov.base_writes++;
        else
            g_sfov.write_failed++;
    }
    if (a.write_effective) {
        if (self_write(cam + kEffective, &a.effective, sizeof a.effective)) {
            if (g_sfov.restores++ < 5)
                X4VR_LOG("shadowfov: X4 set the effective field to %.5f (its "
                         "setter recomputing option x base?) — restored to "
                         "%.5f", r.effective, a.effective);
        } else {
            g_sfov.write_failed++;
        }
    }
    static double last = 0.0;
    const double now = mono_seconds();
    if (now - last >= 10.0) {
        last = now;
        X4VR_LOG("shadowfov: phase %ld %s | base %.5f effective %.5f | %llu "
                 "base write(s), %llu effective restore(s), %llu failed",
                 phase, fix ? "FIX" : "CONTROL", r.base, r.effective,
                 (unsigned long long)g_sfov.base_writes,
                 (unsigned long long)g_sfov.restores,
                 (unsigned long long)g_sfov.write_failed);
    }
}

// ------------------------------------------------ X4VR_FIELD_AB=<s>
//
// #70's knee switched live, flatscreen: phase A renders at X4VR_FOV, phase B
// at X4VR_FIELD_B (default 1.24), by writing X4's EFFECTIVE field
// (camera+0x874). Phases start only once game time runs with the cockpit
// camera up; each is announced. See x4vr_fieldab.hpp. The layer's `camuj`
// lines say, per phase, what X4 actually drew with.
static double fieldab_period() {
    static const double p = [] {
        const char *e = getenv("X4VR_FIELD_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    return p;
}

// X4's game clock. `double GetCurrentGameTime(void);` is declared in X4's
// Lua cdefs; its body reads a thread-local clock (0xa41960: a TLS-init check,
// then %fs:-0x2728) and dereferences no global. Called on X4's main thread.
static bool x4_game_time(double *out) {
    using Fn = double (*)();
    static Fn fn = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "GetCurrentGameTime");
        X4VR_LOG("gametime: GetCurrentGameTime %s",
                 f ? "resolved" : "NOT FOUND — nothing can wait for play");
        return f;
    }();
    if (!fn)
        return false;
    *out = fn();
    return true;
}

namespace {
struct FieldAb {
    bool armed = false, started = false;
    uintptr_t cam = 0;
    double t0 = 0.0;
    long phase = -1;
    float last_written = 0.f;
    uint64_t writes = 0, reverted = 0, failed = 0, ticks_a = 0, ticks_b = 0,
             held_a = 0, held_b = 0;
    char refusal[200] = "";
    x4vr::fieldab::GameClock clock;
};
FieldAb g_fab;
} // namespace

static void fieldab_refuse(const char *why) {
    if (strcmp(g_fab.refusal, why) != 0) {
        snprintf(g_fab.refusal, sizeof g_fab.refusal, "%s", why);
        X4VR_LOG("fieldab: REFUSED — %s", why);
    }
}

static void fieldab_tick() {
    using namespace x4vr::fieldab;
    const double period = fieldab_period();
    if (period <= 0.0)
        return;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    static int sites = -1;
    if (sites < 0) {
        uintptr_t bad = 0;
        float def = 0.f;
        const bool code = verify_sites(rd, &bad);
        const bool cst =
            self_read(x4vr::shadowfov::kDefaultAddr, &def, sizeof def) &&
            std::fabs(def - kDefault) < 1e-6f;
        sites = code && cst ? 1 : 0;
        if (sites)
            X4VR_LOG("fieldab: all %zu effective-field sites match X4 9.00; "
                     "X4's default field reads %.7f rad",
                     sizeof kSites / sizeof kSites[0], def);
        else {
            X4VR_LOG("fieldab: REFUSED for the session — %s",
                     !code ? "an effective-field site differs from the "
                             "analysed build"
                           : "X4's default-field constant is not 1.2870022");
            X4VR_LOG("milestone: field A/B REFUSED — this X4 is not the "
                     "analysed build. Nothing will switch; quit and tell me.");
        }
    }
    if (!sites)
        return;
    static const float opt_a = [] {
        const char *e = getenv("X4VR_FOV");
        return e && *e ? strtof(e, nullptr) : 0.0f;
    }();
    static const float opt_b = [] {
        const char *e = getenv("X4VR_FIELD_B");
        return e && *e ? strtof(e, nullptr) : 1.24f;
    }();
    if (opt_a <= 0.0f || opt_b <= 0.0f) {
        fieldab_refuse("X4VR_FOV is unset — phase A has no field");
        return;
    }
    // The game clock, every tick, logged when it starts or stops running.
    double gt = 0.0;
    const double wall = mono_seconds();
    const bool have_gt = x4_game_time(&gt);
    const bool was = g_fab.clock.running;
    const bool running =
        have_gt && game_clock_step(g_fab.clock, gt, wall, 5.0, 1.0);
    if (running != was)
        X4VR_LOG("gametime: %s (game time %.1f s)",
                 running ? "RUNNING — 5 s of play counted" : "stopped",
                 gt);
    static double last_gt_log = 0.0;
    if (wall - last_gt_log >= 10.0) {
        last_gt_log = wall;
        X4VR_LOG("gametime: %.2f s (running %d)", gt, running ? 1 : 0);
    }
    const uintptr_t cam = x4_player_camera();
    float eff = 0.f;
    int32_t mode = -1;
    if (!cam || !self_read(cam + kEffective, &eff, sizeof eff) ||
        !self_read(cam + x4vr::camwatch::kCamMode, &mode, sizeof mode))
        return;
    if (cam != g_fab.cam) {
        // A camera seen for the first time must read X4's own A field.
        if (!layout_ok(eff, opt_a)) {
            char why[200];
            snprintf(why, sizeof why,
                     "camera %p reads effective field %.5f, not option %.4f x "
                     "X4's default (%.5f)",
                     (void *)cam, eff, opt_a, field_of(opt_a));
            fieldab_refuse(why);
            return;
        }
        g_fab.cam = cam;
        g_fab.armed = true;
        g_fab.refusal[0] = 0;
        X4VR_LOG("fieldab: ARMED on camera %p — A %.4f (%.5f rad, %.2f deg, "
                 "sx %.5f), B %.4f (%.5f rad, %.2f deg, sx %.5f); phases of "
                 "%.0f s once game time runs",
                 (void *)cam, opt_a, field_of(opt_a), field_of(opt_a) * 57.2957795f,
                 sx_of(opt_a), opt_b, field_of(opt_b), field_of(opt_b) * 57.2957795f,
                 sx_of(opt_b), period);
    }
    if (!g_fab.started) {
        // The one unverified premise: that X4's clock runs on the thread we
        // ask it from. If the cockpit camera has been up a minute and it never
        // has, say so rather than let him wait for phases that cannot come.
        static double cockpit_since = 0.0;
        static bool said_stuck = false;
        if (mode == 0 && cockpit_since == 0.0)
            cockpit_since = wall;
        if (mode == 0 && !running && !said_stuck && wall - cockpit_since > 60.0) {
            said_stuck = true;
            X4VR_LOG("fieldab: the cockpit camera has been up 60 s and game "
                     "time never ran (it reads %.2f s) — no phase will start",
                     gt);
            X4VR_LOG("milestone: field A/B is waiting for game time that never "
                     "runs. Quit and tell me.");
        }
        if (!(running && mode == 0))
            return; // loading, a menu, or not the cockpit: no phase yet
        g_fab.started = true;
        g_fab.t0 = wall;
        X4VR_LOG("fieldab: STARTED — game time %.1f s running, cockpit camera "
                 "up", gt);
    }
    const double el = wall - g_fab.t0;
    const bool b = phase_b(el, period);
    const long phase = (long)(el / period);
    if (phase != g_fab.phase) {
        g_fab.phase = phase;
        X4VR_LOG("fieldab: phase %ld %s", phase,
                 b ? "B — the NARROW field" : "A — the WIDE field");
        // The "enough" notice rides IN the phase notice, from phase 8 on:
        // take 324 sent it as a second notification in the same instant, it
        // was lost behind the first, and Patola flew to phase 21 wondering
        // whether the count was a bug.
        X4VR_LOG("milestone: field %s (phase %ld) — %s. Watch the cockpit "
                 "shadows while you look around.%s",
                 b ? "B" : "A", phase,
                 b ? "narrow, shadows expected steady"
                   : "wide, shadows expected to fade",
                 phase >= 8 ? " 4 of each are DONE: quit through the menu "
                              "whenever you like (phases go on until you do)."
                            : "");
    }
    // Held? The value X4 has NOW, against this phase's field: a tick that
    // finds it changed since our last write was X4 putting its own back.
    const float want = field_of(b ? opt_b : opt_a);
    const bool held = std::fabs(eff - want) <= 1e-5f;
    (b ? g_fab.ticks_b : g_fab.ticks_a)++;
    if (held)
        (b ? g_fab.held_b : g_fab.held_a)++;
    if (g_fab.writes && !held && std::fabs(eff - g_fab.last_written) > 1e-5f &&
        std::fabs(g_fab.last_written - want) <= 1e-5f)
        g_fab.reverted++;
    const Action a = decide(eff, opt_a, opt_b, b);
    if (a.write) {
        if (self_write(cam + kEffective, &a.value, sizeof a.value)) {
            g_fab.writes++;
            g_fab.last_written = a.value;
        } else {
            g_fab.failed++;
        }
    }
    static double last = 0.0;
    if (wall - last >= 5.0) {
        last = wall;
        X4VR_LOG("fieldab: phase %ld %s | effective %.5f | held A %llu/%llu B "
                 "%llu/%llu tick(s) | %llu write(s), %llu reverted by X4, %llu "
                 "failed",
                 phase, b ? "B" : "A", eff, (unsigned long long)g_fab.held_a,
                 (unsigned long long)g_fab.ticks_a,
                 (unsigned long long)g_fab.held_b,
                 (unsigned long long)g_fab.ticks_b,
                 (unsigned long long)g_fab.writes,
                 (unsigned long long)g_fab.reverted,
                 (unsigned long long)g_fab.failed);
    }
}

// ------------------------------------------------ X4VR_CSM_COVER=<rho>
//
// #70's fix. X4's cascade fit sizes each cascade by 1/tan of the half-field
// where the frustum needs tan, so at 110 deg the cockpit's corners fall out of
// cascade 0 and fade. The four instructions through which the fit reads X4's
// aspect globals are pointed at a private pair (x4vr_csmfit.hpp); this tick
// keeps that pair at X4's own values (CONTROL) or at the divisors that make
// the fit reach `rho` of the slice (FIX). X4VR_CSM_AB=<s> alternates them,
// CONTROL first, once game time runs in the cockpit; unset, it is FIX always.
static double csmfit_cover() {
    static const double r = [] {
        const char *e = getenv("X4VR_CSM_COVER");
        return e && *e ? atof(e) : 0.0;
    }();
    return r;
}

static double csmfit_period() {
    static const double p = [] {
        const char *e = getenv("X4VR_CSM_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    return p;
}

namespace {
struct CsmFit {
    volatile float *slots = nullptr; // [0] x, [1] y: what the fit reads now
    bool patched = false, started = false;
    double t0 = 0.0;
    long phase = -1;
    uint64_t ticks_fix = 0, ticks_ctl = 0;
    x4vr::fieldab::GameClock clock;
};
CsmFit g_csm;
} // namespace

static void csmfit_patch_at_start() {
    using namespace x4vr::csmfit;
    const double rho = csmfit_cover();
    if (rho <= 0.0)
        return;
    auto refuse = [](const char *why) {
        X4VR_LOG("csmfit: REFUSED — %s. X4's cascade fit is untouched.", why);
        X4VR_LOG("milestone: shadow fix REFUSED — %s. Quit and tell me.", why);
    };
    if (!(rho <= 2.0)) {
        refuse("X4VR_CSM_COVER must be in (0, 2]");
        return;
    }
    uintptr_t va[kNumSites];
    for (size_t i = 0; i < kNumSites; i++) {
        const Site &st = kSites[i].site;
        uint8_t b[sizeof st.bytes];
        if (!self_read(st.va, b, st.n) || memcmp(b, st.bytes, st.n) != 0 ||
            rip_target(st.va, st.n, b) !=
                (kSites[i].slot ? kAspectY : kAspectX)) {
            char why[160];
            snprintf(why, sizeof why,
                     "the fit's read at %#lx differs from X4 9.00",
                     (unsigned long)st.va);
            refuse(why);
            return;
        }
        va[i] = st.va;
    }
    float def[2] = {0.f, 0.f};
    if (!self_read(kAspectDefaultAddr, def, sizeof def) || def[0] != 1.0f ||
        def[1] != 1.0f) {
        refuse("X4's default aspect constant is not (1, 1)");
        return;
    }
    void *page = alloc_near(0x3f0000, 0x7c00000, va, kNumSites, 8);
    if (!page) {
        refuse("no free page within rel32 reach of the fit");
        return;
    }
    volatile float *slots = (volatile float *)page;
    slots[0] = def[0]; // X4's own values until the first tick
    slots[1] = def[1];
    Retarget r[kNumSites];
    for (size_t i = 0; i < kNumSites; i++)
        r[i] = {va[i], 8, (uintptr_t)&slots[kSites[i].slot]};
    char err[200] = "";
    if (!retarget(r, kNumSites, err, sizeof err)) {
        refuse(err);
        return;
    }
    g_csm.slots = slots;
    g_csm.patched = true;
    X4VR_LOG("csmfit: PATCHED — the cascade fit's %zu reads of X4's aspect "
             "(0x4533b00/04) now read %p/%p; coverage %.3f%s",
             kNumSites, (void *)&slots[0], (void *)&slots[1], rho,
             csmfit_period() > 0.0 ? ", CONTROL/FIX phases once game time runs"
                                   : ", FIX always");
}

static void csmfit_tick() {
    using namespace x4vr::csmfit;
    if (!g_csm.patched)
        return;
    const double rho = csmfit_cover(), period = csmfit_period();
    float ax = 1.f, ay = 1.f;
    if (!self_read(kAspectX, &ax, sizeof ax) || !(ax > 0.f) || !std::isfinite(ax))
        ax = 1.f;
    if (!self_read(kAspectY, &ay, sizeof ay) || !(ay > 0.f) || !std::isfinite(ay))
        ay = 1.f;
    const uintptr_t cam = x4_player_camera();
    float field = 0.f;
    int32_t mode = -1;
    if (cam) {
        self_read(cam + x4vr::shadowfov::kEffective, &field, sizeof field);
        self_read(cam + x4vr::camwatch::kCamMode, &mode, sizeof mode);
    }
    if (!(field > 0.1f && field < 3.1f))
        field = 0.f;
    const double wall = mono_seconds();
    bool fix = true;
    long phase = 0;
    if (period > 0.0) {
        double gt = 0.0;
        const bool running = x4_game_time(&gt) &&
                             game_clock_step(g_csm.clock, gt, wall, 5.0, 1.0);
        if (!g_csm.started && running && mode == 0 && field > 0.f) {
            g_csm.started = true;
            g_csm.t0 = wall;
            X4VR_LOG("csmfit: STARTED — game time %.1f s running, cockpit "
                     "camera up", gt);
        }
        if (!g_csm.started) {
            fix = false; // X4's own sizing until the phases start
            phase = -1;
        } else {
            const double el = wall - g_csm.t0;
            fix = x4vr::fieldab::phase_b(el, period); // phase 0 = CONTROL
            phase = (long)(el / period);
        }
    }
    float dx = ax, dy = ay;
    if (fix && field > 0.f) {
        dx = (float)divisor(rho, field, ax);
        dy = (float)divisor(rho, field, ay);
    } else {
        fix = false;
    }
    g_csm.slots[0] = dx;
    g_csm.slots[1] = dy;
    (fix ? g_csm.ticks_fix : g_csm.ticks_ctl)++;

    float c0far = 0.f;
    self_read(kCascade0Far, &c0far, sizeof c0far);
    const double f = c0far * kFarScale;
    const double r = field > 0.f && f > kNear
                         ? radius(kNear, f, field, dx, dy) : 0.0;
    if (period > 0.0 && phase != g_csm.phase && phase >= 0) {
        g_csm.phase = phase;
        X4VR_LOG("csmfit: phase %ld %s", phase,
                 fix ? "FIX — cascades sized for the field"
                     : "CONTROL — X4's own cascade sizing");
        // From phase 8 on the notice itself says the A/B is complete (see
        // fieldab_tick: a separate notice in the same instant was lost).
        X4VR_LOG("milestone: shadows %s (phase %ld) — %s. Look around, "
                 "especially the corners and up/down.%s",
                 fix ? "FIX" : "CONTROL", phase,
                 fix ? "expected steady" : "X4's own, expected to fade",
                 phase >= 8 ? " 4 of each are DONE: quit through the menu "
                              "whenever you like (phases go on until you do)."
                            : "");
    }
    static double last = 0.0;
    if (wall - last >= 5.0) {
        last = wall;
        X4VR_LOG("csmfit: phase %ld %s | field %.5f aspect %.4f/%.4f | "
                 "divisors %.5f/%.5f | coverage %.3f | predicted cascade-0 sx "
                 "%.5f (r %.3f m, far %.3f) | ticks FIX %llu CONTROL %llu",
                 phase, fix ? "FIX" : "CONTROL", field, ax, ay, dx, dy,
                 field > 0.f ? coverage(dx, field, ax) : 0.0,
                 r > 0.0 ? 1.0 / r : 0.0, r, f,
                 (unsigned long long)g_csm.ticks_fix,
                 (unsigned long long)g_csm.ticks_ctl);
    }
}

// ------------------------------------------------ X4VR_CURSOR_CANVAS=auto
//
// Take 326's map: the right-click menu closed within seconds and "Start
// Guidance to Position" would not highlight under the cursor. The recording
// shows the drawn cursor ON the row with X4's map-pan cursor shape: X4 was
// hit-testing elsewhere. Take 311 had measured why: a full-screen menu
// hit-tests in X4's UI, which the layer shows scaled by the canvas map, so the
// cursor must be drawn through that map there; take 314 found the opposite in
// flight (X4 picks the world at the raw pointer), and every take since 315
// drew raw everywhere. `auto` draws through the canvas only while X4 reports a
// full-screen menu.
static bool cursor_canvas_auto() {
    static const bool on = [] {
        const char *e = getenv("X4VR_CURSOR_CANVAS");
        return e && !strcmp(e, "auto");
    }();
    return on;
}

// `bool IsFullscreenMenuDisplayed(bool anymenu, const char* menuname);` is
// declared in X4's own Lua cdefs (ego_helptext/helptext.lua). With anymenu
// true it tail-calls 0x117f7c0, which walks X4's static table of open menus
// (header 0x3dfc200, a std::map header: never NULL) and reads each entry's
// flag. The table lives until X4's main() returns, after which no tick runs.
// Called on X4's main thread, from its event drain.
static void fullscreen_menu_tick() {
    if (!cursor_canvas_auto())
        return;
    using Fn = bool (*)(bool, const char *);
    static Fn fn = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "IsFullscreenMenuDisplayed");
        X4VR_LOG("ui: IsFullscreenMenuDisplayed %s",
                 f ? "resolved — X4VR_CURSOR_CANVAS=auto follows it"
                   : "NOT FOUND — the cursor stays at the raw pointer");
        return f;
    }();
    if (!fn)
        return;
    const bool up = fn(true, nullptr);
    static int last = -1;
    if ((int)up != last) {
        last = up;
        X4VR_LOG("ui: full-screen menu %s",
                 up ? "UP — the cursor is drawn through the UI canvas"
                    : "DOWN — the cursor is drawn at the raw pointer");
    }
    x4vr_shared_state()->fullscreen_menu.store(up ? 1u : 0u,
                                               std::memory_order_relaxed);
}

// ------------------------------------------------ X4VR_STEER=b|a
//
// The mouse-steering target, A/B with a live toggle (x4vr_steer.hpp; Patola,
// after take 328). This side decides WHEN: X4's own condition for showing its
// steering line, read rather than called -- ShouldShowMouseSteeringHelperLine
// is the option AND four bytes off the input object IsMouseSteeringActive
// reads with no NULL check -- plus the cockpit view with the HUD up, which is
// the Lua side's crosshairActive. The layer decides WHERE.
//
// The code the predicate is read from is verified byte for byte once; a
// different build refuses, and then X4's own line is NOT hidden either, so a
// refusal leaves vanilla behaviour rather than no line at all.
static x4vr::steer::Mode steer_asked_mode() {
    static const x4vr::steer::Mode m =
        x4vr::steer::mode_from_env(getenv("X4VR_STEER"));
    return m;
}
static bool steer_sites_verified() {
    static const bool ok = [] {
        if (steer_asked_mode() == x4vr::steer::kOff)
            return false;
        auto rd = [](uintptr_t va, void *o, size_t n) {
            return self_read(va, o, n);
        };
        uintptr_t bad = 0;
        const bool v = x4vr::steer::verify_sites(rd, &bad);
        if (v)
            X4VR_LOG("steer: all %zu ShouldShowMouseSteeringHelperLine sites "
                     "match X4 9.00 — the input object is at global %p; X4's "
                     "own line is hidden and the layer draws the target",
                     x4vr::steer::kNumSites,
                     (void *)x4vr::steer::input_global_from_sites());
        else
            X4VR_LOG("steer: REFUSED — X4's code at %p differs from the "
                     "analysed build; X4VR_STEER does nothing and X4's own "
                     "line stays",
                     (void *)bad);
        return v;
    }();
    return ok;
}
static bool steer_hide_x4_line() {
    return steer_asked_mode() != x4vr::steer::kOff && steer_sites_verified();
}

// The mode in force. The apostrophe used to flip it between A and B; B and
// the toggle were removed after take 339 (x4vr_steer.hpp), so the key is
// X4's again -- unbound in all six of Patola's input maps.
static std::atomic<uint32_t> g_steer_mode{0};

// Mouse steering just engaged: put the pointer at the centre of X4's window,
// so the ship starts straight. X4 itself never does -- its one
// SDL_WarpMouseInWindow call (0xcefcd9) nudges the pointer by a delta -- and
// in the headset the pointer sits wherever the load left it: take 331
// engaged at (125,62) of 2816 and the ship turned hard up-left until Patola
// found the centre. Routed through our own SDL_WarpMouseInWindow, so it is
// folded exactly as one of X4's warps would be.
void SDL_WarpMouseInWindow(void *win, float x, float y);
static std::atomic<bool> g_steer_warping{false};
// The pointer to the centre of X4's window, through our own
// SDL_WarpMouseInWindow so it is folded exactly as one of X4's warps would be.
// False when X4's window is not known yet. `*from_*` is where it was (if
// `*have`), `*to_*` where it went.
static bool recentre_pointer(float *from_x, float *from_y, bool *have,
                             float *to_x, float *to_y, float y_frac = 0.5f) {
    static auto from_id = real<void *(*)(uint32_t)>("SDL_GetWindowFromID");
    static auto get_size = real<bool (*)(void *, int *, int *)>("SDL_GetWindowSize");
    const uint32_t id = g_motion_window.load(std::memory_order_relaxed);
    void *win = (from_id && id) ? from_id(id) : nullptr;
    int w = 0, h = 0;
    if (!win || !get_size || !get_size(win, &w, &h) || w < 2 || h < 2)
        return false;
    const int xw = g_x4_win_w.load(std::memory_order_relaxed);
    *to_x = 0.5f * (float)(xw > 1 ? xw : w);
    *to_y = y_frac * (float)h;
    bool vis = false;
    *have = x4vr::share_read(x4vr_shared_state(), from_x, from_y, &vis);
    g_steer_warping.store(true, std::memory_order_relaxed);
    SDL_WarpMouseInWindow(win, *to_x, *to_y);
    g_steer_warping.store(false, std::memory_order_relaxed);
    return true;
}

static void steer_recentre() {
    static unsigned said = 0;
    float px = 0.f, py = 0.f, cx = 0.f, cy = 0.f;
    bool have = false;
    if (!recentre_pointer(&px, &py, &have, &cx, &cy)) {
        if (said++ < 3)
            X4VR_LOG("steer: mouse steering engaged, but X4's window is not "
                     "known yet — pointer NOT recentred");
        return;
    }
    if (said++ < 40)
        X4VR_LOG("steer: mouse steering engaged — pointer recentred from "
                 "%s%.0f,%.0f to %.0f,%.0f so the ship starts straight",
                 have ? "" : "(unknown) ", px, py, cx, cy);
}

static void steer_tick() {
    if (!steer_hide_x4_line())
        return;
    static bool said = false;
    if (!said) {
        said = true;
        g_steer_mode.store(steer_asked_mode(), std::memory_order_relaxed);
        bool was_b = false;
        x4vr::steer::mode_from_env(getenv("X4VR_STEER"), &was_b);
        X4VR_LOG("steer: overlay ON, starting in %s%s",
                 x4vr::steer::mode_name(steer_asked_mode()),
                 was_b ? " — X4VR_STEER=b asked for B, which was removed "
                         "after take 339: A is the only target"
                       : "");
    }
    const bool cockpit = x4_view() == X4View::Cockpit;
    // Not called unguarded: without a decoded slot the HUD counts as down.
    const bool hud = x4_hud_slot() ? x4_hud_active() : false;
    bool shown = false;
    // Steering ENGAGED: the steering bytes alone, in the cockpit with the HUD
    // up. Not the full line predicate -- its other flags are not known to be
    // steady during flight, and a recentre on their flicker would yank the
    // pointer mid-turn.
    bool engaged = false;
    if (cockpit && hud) {
        using namespace x4vr::steer;
        uintptr_t in = 0;
        uint8_t blk[kA010 - kActive + 1];
        if (self_read(input_global_from_sites(), &in, sizeof in) && in &&
            self_read(in + kActive, blk, sizeof blk)) {
            Flags f;
            f.active = blk[0];
            f.active2 = blk[kActive2 - kActive];
            f.f39 = blk[kF39 - kActive];
            f.f42 = blk[kF42 - kActive];
            f.a010 = blk[kA010 - kActive];
            shown = line_shown(f);
            engaged = f.active || f.active2;
        }
    }
    static bool was_engaged = false;
    if (engaged && !was_engaged)
        steer_recentre();
    was_engaged = engaged;
    static int last = -1;
    static unsigned changes = 0;
    if ((int)shown != last && changes < 200) {
        last = shown;
        changes++;
        X4VR_LOG("steer: X4's steering line would be %s",
                 shown ? "ON — the layer draws the target" : "OFF");
    }
    if (x4vr::Shared *sh = x4vr_shared_state()) {
        sh->steer_mode.store(g_steer_mode.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        sh->steer_line.store(shown ? 1u : 0u, std::memory_order_relaxed);
    }
}

// ------------------------------------------------ X4VR_FOOTLOG=1
//
// What X4's player camera does out of the pilot seat -- read only. See
// injector/x4vr_foot.hpp for why the walking view's state is not decidable
// from 0x1933d00 alone. One sample per 50 ms on X4's thread, a transition line
// whenever the camera, its mode, its attached object or the player's ship
// status changes, and a window line each second. The player's own mouse motion
// is summed in the SDL_PeepEvents(GET) walk, where X4 receives it.
static bool footlog_on() {
    static const bool on = [] {
        const char *e = getenv("X4VR_FOOTLOG");
        return e && *e && *e != '0';
    }();
    return on;
}
namespace {
struct FootMouse {
    std::atomic<int64_t> dx_milli{0}, dy_milli{0};
    std::atomic<uint64_t> n{0}, dropped{0}, pitch_zeroed{0};
};
FootMouse g_footmouse;
} // namespace

// `UniverseID GetPlayerControlledShipID(void);` and `UniverseID
// GetPlayerOccupiedShipID(void);` (UniverseID is uint64_t), declared in X4's
// own Lua. Both test their global before reading through it -- `lea
// 0x238(%rdx)` first is arithmetic, then `test %rdx,%rdx` and a cmove to a
// static fallback (tests/run-x4-call-guards.sh, which now tells a lea from a
// read) -- and the helpers they call (0x1699f20, 0x169af50) read only the
// player object and its parents.
static bool x4_player_ship_ids(uint64_t *controlled, uint64_t *occupied) {
    using Fn = uint64_t (*)();
    static Fn ctl = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "GetPlayerControlledShipID");
        X4VR_LOG("foot: GetPlayerControlledShipID %s", f ? "resolved" : "NOT FOUND");
        return f;
    }();
    static Fn occ = [] {
        Fn f = (Fn)dlsym(RTLD_DEFAULT, "GetPlayerOccupiedShipID");
        X4VR_LOG("foot: GetPlayerOccupiedShipID %s", f ? "resolved" : "NOT FOUND");
        return f;
    }();
    if (!ctl || !occ)
        return false;
    *controlled = ctl();
    *occupied = occ();
    return true;
}

// ------------------------------------------------ X4VR_HEADLOOK_FOOT=1
//
// Head-look off the pilot seat (Patola's B: the head looks, the body walks).
// On foot 0x1929f70 drops the camera's own transform, so the write reached
// nothing (takes 334/335); the patch retargets that one jne so walking
// composes the player and then the camera's own rotation -- ours -- and
// nothing else (x4vr_foot.hpp). Patched at load, before X4 has run a frame,
// as X4VR_CSM_COVER is; it lives in memory only. Needs X4VR_HEADLOOK_WRITE:
// without the write nothing owns +0x5b0 on foot, and a stale cockpit angle
// would be composed into the walking view.
static bool headlook_foot_asked() {
    static const bool on = x4vr::env_on("X4VR_HEADLOOK_FOOT", false);
    return on;
}
namespace {
struct FootPatch {
    bool patched = false;
};
FootPatch g_footpatch;
std::atomic<bool> g_on_foot{false};
// Out of the pilot seat at all, HUD up or down: FootMenu forgets a menu the
// HUD interrupted only when he sits down (or X4 takes the pointer back).
std::atomic<bool> g_off_seat{false};
} // namespace

// X4VR_CURSOR_FOOT_AB=1 — alternate the on-foot menu cursor per opening: odd
// openings through the UI canvas (arm A), even ones raw (arm B, take 338's).
static bool cursor_foot_ab() {
    static const bool on = x4vr::env_on("X4VR_CURSOR_FOOT_AB", false);
    return on;
}

// ------------------------------------------------ X4's menus, read
//
// x4vr_menus.hpp: X4's table of open menus and its conversation state, read
// through self_read after every instruction they come from has been checked.
// Patola after take 341: "Go with the no-setting option first" -- for the
// cockpit's right-click menu, which X4 does not call full-screen.
static bool menus_sites_verified() {
    static const bool ok = [] {
        auto rd = [](uintptr_t va, void *o, size_t n) {
            return self_read(va, o, n);
        };
        uintptr_t bad = 0;
        const bool v = x4vr::menus::verify_sites(rd, &bad);
        if (v)
            X4VR_LOG("menus: IsConversationActive and the open-menu lookup "
                     "match X4 9.00 — the table's root at %p and the "
                     "conversation at %p are READ, never called",
                     (void *)x4vr::menus::kMenuRootSlot,
                     (void *)x4vr::menus::kConvGlobal);
        else
            X4VR_LOG("menus: REFUSED — X4's code at %p differs from the "
                     "analysed build; nothing of X4's menus is read",
                     (void *)bad);
        return v;
    }();
    return ok;
}

// A conversation is on (X4's IsConversationActive, read): -1 unknown.
static int x4_conversation() {
    if (!menus_sites_verified())
        return -1;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    int32_t st = 0;
    if (!x4vr::menus::conversation_state(rd, &st))
        return -1;
    return x4vr::menus::conversation_active(st) ? 1 : 0;
}

// X4's table of open menus, every change logged, and **the cockpit's
// right-click menu followed from it**. Take 342 found "InteractMenu" in the
// table 70-100 ms after each right-click and gone when the HUD's
// TopLevelMenu came back; IsFullscreenMenuDisplayed could not say so -- the
// menu is not full-screen, which is all it reports. While it is there the
// cursor is drawn through the UI canvas (share `interact_menu`). The compass
// menus never entered the table and stay raw, where they already line up.
// 30 Hz on X4's main thread, between its own calls into the table; a walk
// that fails leaves the published state as it was.
static void menus_tick() {
    if (!cursor_canvas_auto() || !menus_sites_verified())
        return;
    static double last = 0.0;
    const double now = mono_seconds();
    if (now - last < 0.033)
        return;
    last = now;
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    x4vr::menus::Entry e[24];
    const int n = x4vr::menus::walk(rd, x4vr::menus::kMenuRootSlot, e, 24);
    static char was[512] = "";
    static unsigned changes = 0, failed = 0;
    if (n < 0) {
        if (failed++ < 20)
            X4VR_LOG("ui: X4's open menus could not be read (a read failed "
                     "or the tree was mid-change) — skipped");
        return;
    }
    const bool interact = x4vr::menus::has(e, n < 24 ? n : 24, "InteractMenu");
    static int interact_was = -1;
    if ((int)interact != interact_was) {
        if (interact_was != -1 || interact)
            X4VR_LOG("ui: X4's right-click menu %s",
                     interact ? "OPEN — the cursor is drawn through the UI "
                                "canvas"
                              : "closed");
        interact_was = interact;
    }
    if (x4vr::Shared *sh = x4vr_shared_state())
        sh->interact_menu.store(interact ? 1u : 0u, std::memory_order_relaxed);
    char line[512];
    x4vr::menus::describe(e, n < 24 ? n : 24, line, sizeof line);
    if (strcmp(line, was) != 0 && changes < 2000) {
        changes++;
        snprintf(was, sizeof was, "%s", line);
        X4VR_LOG("ui: X4's open menus: %s%s", line,
                 n > 24 ? " (and more)" : "");
    }
}

// Where a conversation's options are, as a fraction of the window's height:
// take 339-341's clicks on dialogue options landed at y 2040..2105 of 2816
// (x 1333..1530), so 2070/2816. Patola after 341: the centre start "appears
// too high, if it appeared lower it would be faster to get to the dialogue
// menus".
constexpr float kDialogueY = 2070.0f / 2816.0f;

// X4VR_FOOT_MENU_RECENTRE=0 — leave the pointer where X4 freed it when an
// on-foot menu opens (every take before 341). On by default.
static bool foot_menu_recentre() {
    static const bool on = x4vr::env_on("X4VR_FOOT_MENU_RECENTRE", true);
    return on;
}

// X4VR_CURSOR_CENTRED_AB=1 — the same for HUD-down menus off the seat (the
// station teleporters, take 339): odd periods canvas (A), even ones raw (B).
static bool cursor_centred_ab() {
    static const bool on = x4vr::env_on("X4VR_CURSOR_CENTRED_AB", false);
    return on;
}

// **On-foot menus are UI too.** Take 338: after clicking an interactable
// with a submenu, or an NPC, *"the cursor appears off the selected sections
// of the submenu, depending on where I am looking"*. Neither is a full-screen
// menu, so `auto` drew the cursor raw over them -- take 327's cockpit
// right-click menu again: X4 lays its UI out scaled by the canvas map and
// hit-tests it there, so the item under X4's pointer n is drawn at A*n while
// the raw cursor sits at n, off by (1-A)*n -- nothing at the centre, growing
// outward. When an opening counts is x4vr::foot::FootMenu's (tests/foot.cpp).
// Called after headlook_tick on X4's main thread.
static void foot_menu_tick() {
    if (!cursor_canvas_auto())
        return;
    static x4vr::foot::FootMenu fm;
    const auto ev = fm.step(g_off_seat.load(std::memory_order_relaxed),
                            g_on_foot.load(std::memory_order_relaxed),
                            g_relative_mouse.load(std::memory_order_relaxed),
                            mono_seconds(), cursor_foot_ab());
    x4vr::Shared *sh = x4vr_shared_state();
    if (ev == x4vr::foot::FootMenu::kOpened) {
        // Where the head and the pointer were, so a report about one opening
        // can be set against the two things the offset could depend on: the
        // pointer's distance from the centre (the canvas) or the head's angle
        // off the body.
        X4VR_LOG("ui: on-foot menu %llu OPEN — X4 freed the pointer%s; the "
                 "cursor is drawn %s | head-look yaw %.1f pitch %.1f | "
                 "pointer %.0f,%.0f",
                 (unsigned long long)fm.openings,
                 fm.resumed ? " (still free after a HUD-down menu)" : "",
                 fm.canvas ? "through the UI canvas (arm A)"
                           : "at the raw pointer (arm B, take 338's)",
                 g_camwrite.yaw, g_camwrite.pitch,
                 sh ? sh->cursor_x.load(std::memory_order_relaxed) : -1.f,
                 sh ? sh->cursor_y.load(std::memory_order_relaxed) : -1.f);
        if (cursor_foot_ab())
            X4VR_LOG("milestone: on-foot menu %llu — cursor %s",
                     (unsigned long long)fm.openings,
                     fm.canvas ? "A (canvas)" : "B (raw)");
        // **The pointer to the centre on a fresh opening.** Take 340's video:
        // the cursor appeared "far from the menu" at every terminal, NPC and
        // the vendor, because X4 frees the pointer wherever it drifted,
        // unseen, while he walked and turned (opening #1 at 2621,1026). The
        // centre is where the crosshair was when he clicked, and nearer
        // X4's menus (Patola: "I definitely want the cursor closer to the
        // menus"). Not on a resumed opening: there the pointer is where he
        // last saw and moved it.
        // A conversation's options sit below the centre (kDialogueY); a
        // terminal's menu near it. X4's conversation state is read, so
        // which one this is decides where the pointer goes.
        if (!fm.resumed && foot_menu_recentre()) {
            float px = 0.f, py = 0.f, cx = 0.f, cy = 0.f;
            bool have = false;
            const int conv = x4_conversation();
            if (recentre_pointer(&px, &py, &have, &cx, &cy,
                                 conv == 1 ? kDialogueY : 0.5f))
                X4VR_LOG("ui: on-foot menu %llu — pointer recentred from "
                         "%s%.0f,%.0f to %.0f,%.0f (%s)",
                         (unsigned long long)fm.openings,
                         have ? "" : "(unknown) ", px, py, cx, cy,
                         conv == 1 ? "a conversation: where its options are"
                         : conv == 0 ? "no conversation: the centre"
                                     : "conversation unknown: the centre");
            else
                X4VR_LOG("ui: on-foot menu %llu — X4's window is not known "
                         "yet, pointer NOT recentred",
                         (unsigned long long)fm.openings);
        }
    } else if (ev == x4vr::foot::FootMenu::kClosed) {
        X4VR_LOG("ui: on-foot menu %llu closed — the cursor is drawn at "
                 "the raw pointer",
                 (unsigned long long)fm.openings);
    }
    if (sh)
        sh->foot_menu.store(fm.draw_canvas() ? 1u : 0u,
                            std::memory_order_relaxed);
}

static void foot_patch_at_start() {
    using namespace x4vr::foot;
    if (!headlook_foot_asked())
        return;
    auto refuse = [](const char *why) {
        X4VR_LOG("foot: patch REFUSED — %s. Walking stays as it was: a "
                 "window fixed in front.", why);
        X4VR_LOG("milestone: head-look on foot REFUSED — %s. Quit and tell me.",
                 why);
    };
    if (!headlook_write_enabled()) {
        refuse("X4VR_HEADLOOK_FOOT needs X4VR_HEADLOOK_WRITE=1");
        return;
    }
    // Take 336's retarget of the jne at 0x192a2f3 is RETIRED: that branch is
    // the cockpit's (head-look doubled there). This is the second patch,
    // Patola's approval of 2026-10-27: the NULL-controller exit at 0x1929ffd,
    // through a stub that composes the camera's own transform for the
    // player's camera only (x4vr_foot.hpp).
    auto rd = [](uintptr_t va, void *o, size_t n) { return self_read(va, o, n); };
    uintptr_t bad = 0;
    if (!verify_null_exit_sites(rd, &bad)) {
        char why[160];
        snprintf(why, sizeof why,
                 "0x1929f70's code at %#lx differs from X4 9.00",
                 (unsigned long)bad);
        refuse(why);
        return;
    }
    const uintptr_t site = kNullJe;
    void *stub = x4vr::csmfit::alloc_near(0x3f0000, 0x7c00000, &site, 1, 6);
    if (!stub) {
        refuse("no free page within rel32 reach of 0x1929ffd");
        return;
    }
    uint8_t code[kStubLen];
    make_stub(code, kRootGlobal, kLocalTailVa, kReturn.va,
              (uint32_t)kPlayerCamOff);
    memcpy(stub, code, sizeof code);
    const long page = sysconf(_SC_PAGESIZE);
    if (mprotect(stub, (size_t)page, PROT_READ | PROT_EXEC) != 0) {
        refuse("mprotect(rx) of the stub failed");
        return;
    }
    int32_t want = 0;
    if (!x4vr::csmfit::disp_for(kNullJe, 6, (uintptr_t)stub, &want)) {
        refuse("the stub is out of rel32 reach of 0x1929ffd");
        return;
    }
    // Take 337: the rel32 (0x1929fff..0x192a002) crosses a page boundary,
    // and the first version refused on that. patch_code makes every page it
    // touches writable.
    char err[200] = "";
    if (!patch_code(kNullRel32, &want, 4, err, sizeof err)) {
        refuse(err);
        return;
    }
    int32_t got = 0;
    if (!self_read(kNullRel32, &got, 4) || got != want ||
        kNullJeEnd + (intptr_t)got != (uintptr_t)stub) {
        refuse("the read-back of the patched je does not point at the stub");
        return;
    }
    g_footpatch.patched = true;
    X4VR_LOG("foot: PATCHED — 0x1929f70's walking exit (je at %#lx, the camera "
             "with no controller) now goes through a stub at %p: the player's "
             "camera composes its own rotation (0x192a284), any other returns "
             "(0x192a216); head-look writes off the pilot seat, the mouse's "
             "pitch is zeroed there",
             (unsigned long)kNullJe, stub);
}

// The player pilots a ship: GetPlayerControlledShipID != 0. Take 334: 1 in
// the pilot seat (docked included), 0 from the moment he stands up. Absent
// symbols mean "seated", the behaviour before the gate.
static bool x4_seated() {
    uint64_t ctl = 0, occ = 0;
    if (!x4_player_ship_ids(&ctl, &occ))
        return true;
    return ctl != 0;
}

static void foot_tick() {
    if (!footlog_on())
        return;
    using namespace x4vr::foot;
    static double last_sample = 0.0;
    const double now = mono_seconds();
    if (now - last_sample < 0.05)
        return;
    last_sample = now;
    static bool said = false;
    if (!said) {
        said = true;
        X4VR_LOG("foot: census ON — read only; a transition line per camera "
                 "regime, a window line per second; sampled BEFORE head-look's "
                 "write");
    }

    Sample sm;
    sm.cam = x4_player_camera();
    if (!sm.cam)
        return; // the load screen, or X4 tearing down
    self_read(sm.cam + kCamMode, &sm.mode, sizeof sm.mode);
    self_read(sm.cam + kCamController, &sm.controller, sizeof sm.controller);
    uintptr_t att[2] = {};
    if (self_read(sm.cam + kCamAttached, att, sizeof att)) {
        sm.attached = att[0];
        if (att[0])
            self_read(att[0], &sm.attached_vt, sizeof sm.attached_vt);
    }
    uint64_t ctl = 0, occ = 0;
    if (x4_player_ship_ids(&ctl, &occ)) {
        sm.piloting = ctl != 0;
        sm.occupied = occ != 0;
    }
    sm.view = (int)x4_view();
    sm.hud = x4_hud_slot() ? x4_hud_active() : false;
    sm.relative = g_relative_mouse.load(std::memory_order_relaxed);
    X4Rotation r{};
    if (x4_camera_rotation(&r)) {
        sm.rot_ok = true;
        sm.rot_yaw = kRad2Deg * -r.yaw; // head-look's convention, as camread
        sm.rot_pitch = kRad2Deg * r.pitch;
        sm.rot_roll = kRad2Deg * r.roll;
    }
    sm.local_ok = self_read(sm.cam + kCamLocal, sm.local, sizeof sm.local) &&
                  x4vr::camwatch::live_transform(sm.local + 4, false);
    sm.base_ok = self_read(sm.cam + kCamBase, sm.base, sizeof sm.base) &&
                 x4vr::camwatch::live_transform(sm.base + 4, false);
    static x4vr::HeadShared *hs = x4vr_head_state();
    uint64_t updates = 0;
    sm.head_ok = x4vr::head_share_read(hs, &sm.head_yaw, &sm.head_pitch, &updates);

    static Sample prev;
    static bool have_prev = false;
    static Window w;
    static unsigned transitions = 0;
    if (!have_prev || !same_regime(prev, sm)) {
        if (transitions++ < 400) {
            char line[768];
            format_regime(line, sizeof line, sm);
            X4VR_LOG("%s", line);
        }
        if (have_prev && (prev.cam != sm.cam || prev.attached != sm.attached))
            w.forget_matrices();
    }
    prev = sm;
    have_prev = true;
    w.add(now, sm);

    if (now - w.t0 >= 1.0) {
        w.mouse_dx = 1e-3 * (double)g_footmouse.dx_milli.exchange(0);
        w.mouse_dy = 1e-3 * (double)g_footmouse.dy_milli.exchange(0);
        w.mouse_n = g_footmouse.n.exchange(0);
        w.mouse_dropped = g_footmouse.dropped.exchange(0);
        w.mouse_pitch_zeroed = g_footmouse.pitch_zeroed.exchange(0);
        char line[1024];
        format_window(line, sizeof line, w, now - w.t0);
        X4VR_LOG("%s", line);
        // Paced by the log, never by time: the protocol's first step is
        // leaving the seat, and the census has enough once the player has
        // moved the mouse both ways off the seat and turned the head there.
        // "Off the seat" only after the player has piloted: the load screen
        // has a camera and no ship either.
        static double off_seat_s = 0.0, mouse_x = 0.0, mouse_y = 0.0, head = 0.0;
        static bool piloted = false, said_off = false, said_enough = false;
        if (sm.piloting)
            piloted = true;
        if (piloted && !sm.piloting) {
            off_seat_s += now - w.t0;
            mouse_x += std::fabs(w.mouse_dx);
            mouse_y += std::fabs(w.mouse_dy);
            head += w.head.travel_yaw + w.head.travel_pitch;
            if (!said_off) {
                said_off = true;
                X4VR_LOG("milestone: foot census — out of the seat (camera "
                         "mode %d). Go ahead with the protocol.", (int)sm.mode);
            }
        }
        if (said_off && !said_enough && off_seat_s >= 60.0 &&
            mouse_x >= 1500.0 && mouse_y >= 600.0 && head >= 180.0) {
            said_enough = true;
            X4VR_LOG("milestone: foot census has enough (%.0f s off the seat, "
                     "mouse %.0f/%.0f, head %.0f deg). Finish what you are "
                     "doing, sit back down, quit.",
                     off_seat_s, mouse_x, mouse_y, head);
        }
        w.restart(now);
    }
}

// **Task #75's fix: where the tick's cadence comes from.**
//
// 0 = X4's event drain, exactly as every take so far. Anything else is a rate in
// Hz, driven by a thread of ours.
//
// Take 284 measured the drain delivering 46.5 ticks/s against 89.9 poses
// published, and `fresh / min(pump, published)` at 0.79 -- the polls arrive in
// bursts, so publish intervals go unvisited even in windows that polled at
// 422 Hz. The deficit is phase, not capacity, which is why polling harder is not
// the answer and a cadence of our own is.
//
// It MOVES the tick; it never adds a second caller. headlook_tick keeps
// non-atomic state -- `last_updates`, `stale`, the inputrate counters, the whole
// of g_headlook -- so two threads in it would corrupt the servo, not merely
// race a statistic. `tick_guard` below turns that constraint into something that
// reports rather than something I have to remember.
// **X4VR_TICK_HZ IS HARMFUL. Do not set it, and do not reach for it to raise
// X4's camera rate.** It is kept only so the takes that measured the damage can
// be reproduced.
//
// It does exactly what it says -- it moves the servo tick off X4's event drain
// onto a thread of our own at the requested rate -- and that move is the defect.
// `headlook_tick()` is normally called from INSIDE the `SDL_PeepEvents`
// interception below, so the mouse event it pushes is consumed by the very
// `while (SDL_PollEvent(&e))` loop it was pushed into. Measured residency in
// X4's queue, over the windows that actually pushed motion:
//
//   default (servo on the drain)    take 291   0.01 ms mean, 0.01 median
//   X4VR_TICK_HZ=90 (own thread)    take 292  16.79 ms mean, 19.89 median
//
// Roughly 17 ms of pure added latency, and it lands on a loop whose stability
// depends on total delay measured in TICKS -- so raising the rate shortens the
// tick and charges the same milliseconds twice. `tests/servo_rate.cpp` section 9
// drives the real servo from those measured rates and reproduces both outcomes:
// 0.0% slam at the default against a measured 0.0%, and 32.3% at 90 Hz against a
// measured 20.3%.
//
// Every session that ever went wild had this knob set:
//
//   285b-s2  70.7 Hz  WILD (7.1%)      287b  69.7 Hz  WILD (7.5%)
//   292      75.8 Hz  WILD (20.3%)     285b-s1  68.6 Hz  calm -- UNEXPLAINED
//
// s1 is the one session with the knob that stayed calm and nothing measured
// separates it from s2; that is still open (#75). It is not a reason to use the
// knob. The camera-rate win it was added for is real (28.2 -> 54.5 Hz in s1) and
// unusable: a camera slamming between X4's stops is a far worse VR defect than
// the jitter it was meant to reduce, and there are no cosmetic defects in VR.
static int tick_hz() {
    static const int hz = [] {
        const char *e = getenv("X4VR_TICK_HZ");
        const int v = e && *e ? atoi(e) : 0;
        // 400 Hz is far above any display rate and still 45x below take 119's
        // 18 kHz drain. Above that the push rate itself becomes the hazard.
        const int clamped = (v < 0 || v > 400) ? 0 : v;
        // Said once, at the point of use, because a knob documented only in
        // docs/ is a knob whose warning is not read by whoever sets it.
        //
        // **Gated on the variable being SET, not on the value surviving the
        // clamp.** `X4VR_TICK_HZ=500` asks for the harmful behaviour just as
        // plainly as `=90` does; gating on `clamped` would have said nothing at
        // all about the run that asked loudest. Intent, never outcome.
        if (e && *e) {
            X4VR_LOG("tick: X4VR_TICK_HZ=%s — this knob is HARMFUL and is kept "
                     "only to reproduce takes 285b/287b/292. It moves the servo "
                     "off X4's event drain, which measured +17 ms of queue "
                     "latency (0.01 ms -> 19.89 ms median) and made the camera "
                     "slam between X4's stops in 3 of the 4 sessions that used "
                     "it.", e);
            if (clamped)
                X4VR_LOG("tick: driving the servo at %d Hz on our own thread — "
                         "expect this run to be unstable.", clamped);
            else
                X4VR_LOG("tick: %s is outside 1..400 and was IGNORED — the servo "
                         "stays on X4's drain, which is the good path. The run "
                         "is NOT testing what its command line asked for.", e);
        }
        return clamped;
    }();
    return hz;
}

void headlook_tick() {
    static const bool armed = [] {
        const char *e = getenv("X4VR_HEADLOOK");
        return e && *e && *e != '0';
    }();
    if (!armed || !this_is_the_game())
        return;
    // A guard that can actually fail: it compares the thread now inside the tick
    // against the one that was, and names both. Had this merely set a flag and
    // returned, it could not distinguish "moved the tick" from "called it from
    // two places", which is the mistake the driver exists to avoid.
    {
        static std::atomic<unsigned long> owner{0};
        const unsigned long me = (unsigned long)pthread_self();
        unsigned long prev = owner.exchange(me, std::memory_order_relaxed);
        if (prev != 0 && prev != me) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true, std::memory_order_relaxed))
                X4VR_LOG("headlook: TWO THREADS are in headlook_tick (%lu and "
                         "%lu). The servo's state is not atomic, so this "
                         "corrupts the command rather than just the counters. "
                         "X4VR_TICK_HZ is supposed to MOVE the tick, not add a "
                         "caller — the drain's call is not being suppressed.",
                         prev, me);
        }
    }
    g_pumprate.entered++;

    static x4vr::HeadShared *hs = x4vr_head_state();
    static const float gain = [] {
        const char *e = getenv("X4VR_HEADLOOK_GAIN");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    static const bool once = [] {
        X4VR_LOG("headlook: armed, scancode %d, gain %s", headlook_scancode(),
                 getenv("X4VR_HEADLOOK_GAIN")
                     ? getenv("X4VR_HEADLOOK_GAIN")
                     : "UNSET — commands nothing until calibrated");
        return true;
    }();
    (void)once;
    g_headlook.gain_deg_per_count = gain;
    static const float kp = [] {
        const char *e = getenv("X4VR_HEADLOOK_KP");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    if (kp > 0.0f)
        g_headlook.servo_kp = kp;

    // **Task #75: the read-back plausibility guard, ON by default.** See
    // observe_max_jump_deg in x4vr_headlook.hpp for the measurement. The knob
    // exists so an earlier take can be reproduced exactly (0 = unguarded), not
    // as a tuning dial -- the threshold is 3 * max_step, which is the dead time
    // times the step ceiling and not a free parameter.
    // **Task #75 take 295: the rate fix, and its A/B.**
    //
    // X4VR_HEADLOOK_RATEFIX is the TIME CONSTANT in milliseconds. kp_eff is
    // dt/tau and the step ceiling is X4VR_HEADLOOK_MAXRATE * dt, so the loop
    // answers in the same time however often it runs — and the two are
    // INDEPENDENT, which take 295 is the reason for.
    //
    // **Take 295 ran the coupled version and the frustum span.** One knob set
    // both, so asking for tau 40 also took the slew from 600 to 1200 deg/s.
    // ARM_B: steps of 32.77 deg against a ceiling of 12, lag 448-2072 ms
    // against the control arm's 41-153, 39 of the run's 40
    // saturated-with-head-centred commands, and 898 of its 901 read-back
    // refusals.
    //
    // Swept offline in the SATURATING regime (servo_rate.cpp section 12), slew
    // held at 600, against the measured 60 ms dead time:
    //
    //   tau ms   worst |cam| against an 85 limit
    //      200         50.14   clean
    //      140         71.88   clean
    //      110         81.88   clean
    //       80         92.35   SPINS   <- today's equivalent
    //       40         96.49   SPINS
    //
    // So the safe direction is LONGER, not shorter. Do not ask a take for tau
    // under 110 until the dead time itself is smaller.
    //
    // X4VR_HEADLOOK_RATEFIX_AB alternates it against OFF every N seconds, which
    // is the only honest control: the two arms then differ in this and nothing
    // else, including the scene and the player's mood.
    // **RENAMED, and the old name is now fatal on purpose.**
    //
    // X4VR_HEADLOOK_RATEFIX meant "nominal Hz" in take 295 and "tau in ms"
    // after the knob was split. Re-running take 295's command line therefore
    // ran a DIFFERENT experiment under the same name — tau 100 ms and slew 600
    // instead of tau 40 and slew 1200 — and nothing in the log said so. A knob
    // whose meaning changes silently makes every stored command line a trap.
    static const float ratefix = [] {
        if (const char *old_name = getenv("X4VR_HEADLOOK_RATEFIX")) {
            if (*old_name)
                X4VR_LOG("headlook: !! X4VR_HEADLOOK_RATEFIX=%s is the OLD name "
                         "and meant nominal Hz; it now means tau in ms and is "
                         "IGNORED. Use X4VR_HEADLOOK_TAU_MS. This run is NOT "
                         "testing what its command line asked for.", old_name);
        }
        const char *e = getenv("X4VR_HEADLOOK_TAU_MS");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    static const float maxrate = [] {
        const char *e = getenv("X4VR_HEADLOOK_MAXRATE");
        return e && *e ? (float)atof(e) : 600.0f;
    }();
    g_headlook.rate_fix_max_rate_deg_s = maxrate;
    if (ratefix > 0.0f)
        g_headlook.rate_fix_tau_ms = ratefix;
    static const double ratefix_ab = [] {
        const char *e = getenv("X4VR_HEADLOOK_TAU_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    if (ratefix_ab > 0.0 && ratefix > 0.0) {
        static const double t0 = mono_seconds();
        const long phase = (long)((mono_seconds() - t0) / ratefix_ab);
        const bool on = (phase & 1) != 0;
        static long last_phase = -1;
        g_headlook.rate_fix_tau_ms = on ? ratefix : 0.0f;
        g_ratefix_arm.store(on ? 1 : 0, std::memory_order_relaxed);
        if (phase != last_phase) {
            last_phase = phase;
            X4VR_LOG("headlook: ratefix %s (tau %.0f ms, slew %.0f deg/s, "
                     "A/B phase %ld, %.0f s)",
                     on ? "ARM_B" : "control (off)", on ? ratefix : 0.0f,
                     maxrate, phase, ratefix_ab);
        }
    } else if (ratefix > 0.0f) {
        static const bool rf_once = [] {
            X4VR_LOG("headlook: ratefix ON, tau %.0f ms, slew %.0f deg/s — "
                     "no A/B, so this run has no control arm",
                     g_headlook.rate_fix_tau_ms,
                     g_headlook.rate_fix_max_rate_deg_s);
            return true;
        }();
        (void)rf_once;
    }

    static const bool jump_once = [] {
        const char *e = getenv("X4VR_HEADLOOK_OBSERVE_JUMP");
        if (e && *e) {
            g_headlook.observe_max_jump_deg = (float)atof(e);
            X4VR_LOG("headlook: observe guard %s (%.1f deg, %d refusal budget)",
                     g_headlook.observe_max_jump_deg > 0.0f ? "set" : "DISABLED",
                     g_headlook.observe_max_jump_deg,
                     g_headlook.observe_max_refusals);
        } else {
            X4VR_LOG("headlook: observe guard %.1f deg (default), refusal "
                     "budget %d — a read-back further than this from our own "
                     "command did not come from us",
                     g_headlook.observe_max_jump_deg,
                     g_headlook.observe_max_refusals);
        }
        return true;
    }();
    (void)jump_once;

    // **Task #75: the servo overshoots its own target, and kp is the lever the
    // arithmetic picks.** Measured offline against the real head_look_step with
    // the plant unclamped, dead time 3, the limit PAIRED so nothing sits on a
    // stop. Score is the larger of overshoot and lag, because both are the neck
    // and the view disagreeing:
    //
    //                    350 deg/s  over/lag/worst     500 deg/s worst
    //   kp 0.25 (today)     22.00 / 16.70 / 22.00           27.17
    //   kp 0.15             16.59 / 16.70 / 16.70           23.81
    //   max_step 3           4.40 / 36.08 / 36.08           45.05
    //
    // max_step has the best overshoot on that table and the worst worst-case: it
    // moves the error from forward to backward rather than removing it. kp costs
    // nothing while the step is saturated -- a clamped step ignores kp, which is
    // why both rows read the same lag -- and settles faster, 143 ms against 413.
    //
    // **kp also scales the lead**, `lead_scale * (1/kp - 1)`: 3.0x at 0.25
    // becomes 5.67x at 0.15. The arms therefore differ in anticipation as well
    // as gain, which is honest -- that IS what setting kp does -- but it means a
    // difference here does not attribute to gain alone. #73 closed the lead as
    // imperceptible, so this is recorded rather than controlled.
    //
    // Alternating inside one run for the reason take 293 taught: comparing arms
    // across takes let my own protocol instruction change the input distribution
    // and the run could not be read.
    static const double kp_ab = [] {
        const char *e = getenv("X4VR_HEADLOOK_KP_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    static const float kp_b = [] {
        const char *e = getenv("X4VR_HEADLOOK_KP_B");
        return e && *e ? (float)atof(e) : 0.15f;
    }();
    if (kp_ab > 0.0) {
        static const double kab_t0 = mono_seconds();
        static const float kp_a = g_headlook.servo_kp;   // whatever the run set
        const long phase = (long)((mono_seconds() - kab_t0) / kp_ab);
        const bool arm_b = (phase & 1) != 0;   // opens on the CONTROL arm
        g_headlook.servo_kp = arm_b ? kp_b : kp_a;
        // Stamped, not re-derived. `lagsample` carries it so the scorer reads
        // the branch the code TOOK rather than recomputing it from timestamps
        // against the phase lines -- a join that has to get guard bands and
        // clock skew right, and that can evaluate a different predicate than
        // this one did.
        g_kp_arm.store(arm_b ? 1 : 0, std::memory_order_relaxed);
        static long kab_said = -1;
        if (phase != kab_said) {
            kab_said = phase;
            // Intent, never outcome: if kp somehow fails to reach the servo this
            // must still name the arm the run ASKED for, or the scorer would
            // split one population in half and call the null a pass.
            X4VR_LOG("headlook: kp %s (%.3f, lead %.2fx, A/B phase %ld, %.0f s)",
                     arm_b ? "ARM_B" : "control", g_headlook.servo_kp,
                     g_headlook.servo_kp > 0.0f
                         ? (1.0f / g_headlook.servo_kp - 1.0f) : 0.0f,
                     phase, kp_ab);
        }
    }
    static const float gpitch = [] {
        const char *e = getenv("X4VR_HEADLOOK_GAIN_PITCH");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    g_headlook.gain_pitch_deg_per_count = gpitch;
    // Task #68: the servo's own ceiling, so an experiment that raises X4's
    // can be seen at all.
    //
    // X4's free-look clamp is data -- libraries/parameters.xml,
    // `<lookaround><internal maxpitch="35" maxyaw="65">` -- and an extension
    // can override it. But the servo clamps its TARGET to these limits, so
    // with them fixed at 65/35 it would never command past the old stop and
    // the camera could not go there whatever X4's file said. The measurement
    // would have read "no change" and meant nothing: a guard that cannot
    // fail, which is how several takes have already been wasted here.
    //
    // Default is exactly the shipped 65/35, so every existing run is
    // unaffected and omitting these is the control.
    static const float ylim = [] {
        const char *e = getenv("X4VR_HEADLOOK_YAW_LIMIT");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    static const float plim = [] {
        const char *e = getenv("X4VR_HEADLOOK_PITCH_LIMIT");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    static const bool limits_said = [] {
        if (ylim > 0.0f || plim > 0.0f)
            X4VR_LOG("headlook: servo limits yaw=%.2f pitch=%.2f deg (default "
                     "65/35 is X4's own, from parameters.xml). Raising these "
                     "alone changes NOTHING unless X4's file is overridden too "
                     "— X4 clamps its camera independently (task #68)",
                     ylim > 0.0f ? ylim : 65.0f, plim > 0.0f ? plim : 35.0f);
        return true;
    }();
    (void)limits_said;
    if (ylim > 0.0f)
        g_headlook.yaw_limit_deg = ylim;
    if (plim > 0.0f)
        g_headlook.pitch_limit_deg = plim;

    // **Task #75: does the camera go unstable because it is NEAR X4's stop?**
    //
    // Four episodes have now been ordered against enough preceding calm to be
    // trusted, and in every one the oscillation starts FIRST and the clamp
    // follows 0.17-0.66 s later — one of them (292/6) is severe and never
    // reaches the clamp at all, so contact is not necessary. But in three of the
    // four the camera was sitting at **61-67 deg** when the oscillation began,
    // just short of the 65 stop. The head is not the trigger (during episodes it
    // is nearer centre than when calm, and never past 65), so the region just
    // inside the stop is the remaining suspect.
    //
    // X4VR_HEADLOOK_YAW_LIMIT_AB, in seconds, alternates the servo's yaw limit
    // between the shipped 65 and the value in X4VR_HEADLOOK_YAW_LIMIT_B. At 40
    // the servo never asks the camera past 40 deg, so it cannot approach X4's
    // stop — and if episodes stop with it, the near-stop region is the trigger.
    //
    // **Alternated inside one run, not across two takes, and that is not a
    // convenience.** Episodes are rare (~1 per 2 min of steering) and their rate
    // varies between sessions for reasons this project has spent days failing to
    // pin: 285b-s1 and s2 were the same binary and the same knobs, and comparing
    // them is what produced a finding that had to be withdrawn in full. Two takes
    // would repeat that exact mistake.
    static const double ylim_ab = [] {
        const char *e = getenv("X4VR_HEADLOOK_YAW_LIMIT_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    static const float ylim_b = [] {
        const char *e = getenv("X4VR_HEADLOOK_YAW_LIMIT_B");
        return e && *e ? (float)atof(e) : 40.0f;
    }();
    if (ylim_ab > 0.0) {
        static const double ab_t0 = mono_seconds();
        const long phase = (long)((mono_seconds() - ab_t0) / ylim_ab);
        // Opens on the CONTROL arm, like the lead A/B: phase 0 is the shipped
        // limit, so a run that dies early still carries a baseline.
        const bool narrow = (phase & 1) != 0;
        g_headlook.yaw_limit_deg = narrow ? ylim_b : 65.0f;
        static long ab_said = -1;
        if (phase != ab_said) {
            ab_said = phase;
            // Intent, not outcome. If the limit somehow fails to reach the
            // servo this must still say which arm the run ASKED for, or the
            // scorer would split one population in half and report a null as a
            // pass.
            X4VR_LOG("headlook: yawlimit %s (%.1f deg, A/B phase %ld, %.0f s)",
                     narrow ? "NARROW" : "control", g_headlook.yaw_limit_deg,
                     phase, ylim_ab);
        }
    }

    // **Task #73's lead, and the A/B that measures it.**
    //
    // X4VR_HEADLOOK_LEAD is the scale (1.0 = the full (1/kp - 1) anticipation
    // the arithmetic calls for; 0 = exactly the servo as it shipped).
    //
    // X4VR_HEADLOOK_LEAD_AB, in seconds, alternates the lead on and off inside
    // ONE run. That is not a convenience: the lag depends on the frame rate,
    // so two separate takes would differ in frame rate as well as in the lead
    // and could not tell a servo fix from a frame-time change. Alternating
    // within a run makes the frame rate a covariate instead of a confound, and
    // both arms pay the same instrumentation cost.
    static const float lead_scale = [] {
        const char *e = getenv("X4VR_HEADLOOK_LEAD");
        return e && *e ? (float)atof(e) : 0.0f;
    }();
    static const double lead_ab = [] {
        const char *e = getenv("X4VR_HEADLOOK_LEAD_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    bool lead_armed = lead_scale > 0.0f;
    if (lead_ab > 0.0) {
        static const double t0 = mono_seconds();
        const long phase = (long)((mono_seconds() - t0) / lead_ab);
        lead_armed = (phase & 1) != 0; // the run OPENS on the control arm
        static long said_phase = -1;
        if (phase != said_phase) {
            said_phase = phase;
            // Intent, not outcome: this records what the run ASKED for. If the
            // lead turns out not to reach the servo, this line must still say
            // it was armed, or the scorer would silently compare two identical
            // arms and report a null as a pass.
            X4VR_LOG("headlook: lead %s (scale %.2f, A/B phase %ld, %.0f s)",
                     lead_armed ? "ARMED" : "IDLE",
                     lead_armed ? (lead_scale > 0.0f ? lead_scale : 1.0f) : 0.0f,
                     phase, lead_ab);
        }
    }
    g_headlook.lead_scale =
        lead_armed ? (lead_scale > 0.0f ? lead_scale : 1.0f) : 0.0f;

    float yaw = 0.0f, pitch = 0.0f;
    uint64_t updates = 0;
    if (!x4vr::head_share_read(hs, &yaw, &pitch, &updates)) {
        g_pumprate.nopose++;
        if (g_headlook_active.exchange(false, std::memory_order_relaxed))
            send_key(headlook_scancode(), false);
        return;
    }
    // The layer's own publish counter, captured before the freshness gate can
    // return. Its delta over the window is how many poses were published, which
    // is the quantity `fresh` is supposed to equal.
    if (!g_pumprate.have_updates) {
        g_pumprate.have_updates = true;
        g_pumprate.updates_mark = updates;
    }
    g_pumprate.updates_last = updates;
    // **Once per published pose, not once per call.** Take 119 flooded SDL's
    // event queue: SDL_PeepEvents runs far more often than once a frame -- ~18
    // thousand times a second measured -- and send_key_down had no gate, so
    // roughly 3.2 million key events went into the queue over three minutes.
    // X4 toggled relative mouse mode on and off forty times and the camera
    // moved 3.6 degrees in total. The arithmetic was never the problem; the
    // rate was.
    //
    // Gating on the layer's `updates` counter rather than on a timer ties the
    // tick to the thing that actually changed -- one command per new head pose,
    // which is the headset's frame rate and nothing faster.
    //
    // "Held still" and "the layer stopped publishing" look identical in the
    // angles and want opposite responses, which is the second thing `updates`
    // is for.
    static uint64_t last_updates = 0;
    static int stale = 0;
    const bool fresh = updates != last_updates;
    if (fresh)
        g_pumprate.fresh++;
    else
        g_pumprate.skipped++;
    stale = fresh ? 0 : stale + 1;
    last_updates = updates;
    if (stale > 5000) {
        if (g_headlook_active.exchange(false, std::memory_order_relaxed))
            send_key(headlook_scancode(), false);
        return;
    }
    g_headlook_active.store(true, std::memory_order_relaxed);

    // **Task #75: the freshness gate is half the dead time, and it is ours.**
    //
    // `if (!fresh) return;` makes a tick happen only when a drain poll happens
    // to find a new pose. Measured, the servo therefore iterates at 41.9 Hz
    // (take 295) and 52.2 (294) while the drain runs at 84.2 and 103.7 and the
    // layer publishes at ~89.7 — so roughly half the available iteration rate
    // is left on the floor by our own gate, not by anything X4 does. Lag is
    // (1/kp - 1) * tick_period, so that is a factor of two on the complaint.
    //
    // **A TIME floor, not a bare removal.** Take 295 measured the drain between
    // 25 and 325 Hz; ticking the servo at 325 Hz would be take 119's shape
    // again, and it would also leave the period as irregular as it is now. A
    // minimum interval bounds the rate AND regularises it, which is the same
    // wander section 12 charges for the tremble tail.
    //
    // Modelled in tests/servo_rate.cpp section 12, saturating regime, take
    // 295's own rates:
    //
    //                                tick ms   lag ms   worst |cam|
    //   gated (today), ratefix off      23.9    117.0         72.45   clean
    //         UNGATED, ratefix off      11.9     53.2         78.55   clean
    //      UNGATED, ratefix tau110      11.9    106.8         68.08   clean
    //
    // Lag more than halves and the excursion grows but stays inside the limit.
    // The model cannot see one real risk: our own injected events lengthen X4's
    // drain, so doubling the event rate may raise the drain rate that feeds
    // this — take 119's runaway in miniature, and the reason for the floor.
    //
    // 0 (the default) keeps the gate, so every earlier take reproduces.
    static const double ungate_ms = [] {
        const char *e = getenv("X4VR_HEADLOOK_UNGATED");
        return e && *e ? atof(e) : 0.0;
    }();
    static const double ungate_ab = [] {
        const char *e = getenv("X4VR_HEADLOOK_UNGATED_AB");
        return e && *e ? atof(e) : 0.0;
    }();
    bool ungated = ungate_ms > 0.0;
    if (ungated && ungate_ab > 0.0) {
        static const double t0 = mono_seconds();
        const long phase = (long)((mono_seconds() - t0) / ungate_ab);
        ungated = (phase & 1) != 0;
        static long said = -1;
        if (phase != said) {
            said = phase;
            X4VR_LOG("headlook: ungate %s (floor %.1f ms, A/B phase %ld, %.0f s)",
                     ungated ? "ARM_B" : "control (gated)", ungate_ms, phase,
                     ungate_ab);
        }
    }
    g_ungate_arm.store(ungated ? 1 : 0, std::memory_order_relaxed);
    // **The floor is measured from the last tick of ANY kind.** Measuring it
    // from the last UNGATED tick, which is what this did first, does not bound
    // the rate at all: a fresh tick followed 2 ms later by a stale one would
    // both run, and the cap it exists to enforce would be advisory. The whole
    // point of the floor is that take 295 measured X4's drain spiking to
    // 325 Hz.
    static double last_tick_s = 0.0;
    const double now_s = mono_seconds();
    if (!fresh) {
        if (!ungated)
            return; // nothing new to act on; acting anyway is what take 119 did
        // Ungated: act on the LATEST pose even though it has not changed. For a
        // proportional servo that is simply another correction toward the same
        // target, not a double-count — the error shrinks each time.
        if (now_s - last_tick_s < ungate_ms * 0.001)
            return; // the floor
        g_pumprate.ungated_ticks++;
    }
    // **The floor must NOT apply to a FRESH tick** — take 305 measured the
    // ungated arm ticking SLOWER than the control, 14.0 ms against 12.7.
    //
    // The floor exists to stop STALE ticks spamming the servo when X4's drain
    // spikes (take 295 saw 325 Hz). Fresh ticks need no such bound: they are
    // already capped by the layer's publish rate, ~85-90 Hz. Applying the
    // floor to them means that whenever the natural rate is FASTER than the
    // floor — 12.7 ms against an 11 ms floor in take 305 — the ungated arm
    // refuses fresh ticks the control would have run, and the knob meant to
    // raise the iteration rate lowers it instead.
    //
    // Stale ticks keep the floor and keep measuring it from the last tick of
    // ANY kind, so they still only fill genuine gaps and cannot spam.
    //
    // What stood here was:
    //
    //     else if (ungated && now_s - last_tick_s < ungate_ms * 0.001)
    //         return;
    //
    // reasoned as "the cap is on how often the servo may ACT, not on which
    // kind of tick it acts from". That reasoning is sound for a servo whose
    // natural rate exceeds the floor and wrong about which way the knob then
    // points, which is only visible once measured.
    last_tick_s = now_s;
    // **Count what the servo actually DID.** `fresh` counts poses seen, and
    // with ungating on that is no longer the iteration count — the scorer's
    // "the servo ITERATES at fresh Hz" line would be false by a factor of two
    // in exactly the arm being measured.
    g_pumprate.acted++;

    // Observed BEFORE the step, so the servo acts on this frame's reading.
    // It used to run after, which cost a frame of lag for no reason.
    X4Rotation r{};
    const bool have_cam = camread_enabled() && x4_camera_rotation(&r);
    // Not off the seat: there GetCameraRotation is the body times the head,
    // so against our command it is "off" by however far the mouse has turned
    // him. Take 338 logged 9839 refusals and resyncs, every one of them the
    // body; the write then put cmd back to the head, so only the log paid.
    // g_off_seat, not g_on_foot: the latter is false while the HUD is down,
    // and take 349 logged ~60 more against his body (-90 deg) in the second
    // between a teleport and the HUD's return.
    if (have_cam && g_camloop && !g_off_seat.load(std::memory_order_relaxed)) {
        const float obs_y = kRad2Deg * -r.yaw, obs_p = kRad2Deg * r.pitch;
        const float was_y = g_headlook.cmd_yaw_deg;
        const uint64_t was_resyncs = g_headlook.observe_resyncs;
        if (!x4vr::head_look_observe(g_headlook, obs_y, obs_p)) {
            // Rare by construction -- one fault in 395 s in take 294 -- so this
            // logs the EVENT rather than a rate. If a run ever fills the log
            // with these, the guard is wrong and that is the finding.
            X4VR_LOG("headlook: REFUSED read-back %.2f,%.2f — %.2f deg from our "
                     "own command %.2f (limit %.1f), refusal %d of %d",
                     obs_y, obs_p, obs_y - was_y, was_y,
                     g_headlook.observe_max_jump_deg,
                     g_headlook.observe_refused_run,
                     g_headlook.observe_max_refusals);
        } else if (g_headlook.observe_resyncs != was_resyncs) {
            X4VR_LOG("headlook: RESYNC to %.2f,%.2f — the refusal budget ran "
                     "out, so X4 really did move the camera %.2f deg",
                     obs_y, obs_p, obs_y - was_y);
        }
    }

    // **Never keep pushing a camera that does not answer -- but know the
    // difference between "not answering" and "already there".**
    //
    // Opening the map made the view accelerate away: GetCameraRotation reports
    // 0.00,0.00 outside the cockpit, so the servo saw a constant error and
    // commanded forever into whatever the map DOES steer with. A servo with no
    // feedback on the thing it is actually moving is an accelerator.
    //
    // The first attempt at detecting that was worthless: "commanded something
    // and the camera did not move" is exactly what a CONVERGED servo looks like
    // when the head is still. It stood down after 0.66 s of perfect tracking and
    // then thrashed -- 23 stand-downs and 22 resumes in one run -- so the
    // cockpit never worked at all. A predicate that cannot separate success from
    // failure is not a detector.
    //
    // What separates them is the RATIO, accumulated over a window: a stall is
    // having asked for a lot of rotation and received almost none. A converged
    // servo asks for almost nothing, so it never qualifies however long it runs.
    static x4vr::StallWatch g_stall;
    static float last_obs_yaw = 0.0f, last_obs_pitch = 0.0f;
    const float obs_now_yaw = kRad2Deg * -r.yaw;
    const float obs_now_pitch = kRad2Deg * r.pitch;
    const float obs_moved = std::fabs(obs_now_yaw - last_obs_yaw) +
                            std::fabs(obs_now_pitch - last_obs_pitch);
    last_obs_yaw = obs_now_yaw;
    last_obs_pitch = obs_now_pitch;

    // The yaw fade, measured at FULL RATE.
    //
    // The headlook lines below are every 30th call, and adjacent ones can
    // differ by tens of degrees purely because the player moved fast — a
    // doctored 65 deg cliff sat inside take 209's own 62.68 deg level
    // baseline, so no statistic over those samples can separate "the fade
    // failed" from "he turned quickly". Consecutive frames can.
    //
    // Bucketed by pitch because the claim is specifically about steep angles:
    // above 82 deg the weight is under 0.05 and the command must be nearly
    // frozen, whatever the head does.
    static float last_cmd_yaw = 0.0f;
    static bool have_last = false;
    static float worst_level = 0.0f, worst_fade = 0.0f, worst_deep = 0.0f;
    const float pre_cmd = g_headlook.cmd_yaw_deg;
    // The weight RECOVERED from pitch. `{yaw, pitch}` left yaw_weight at its
    // default 1.0, so the fade — and the hard gate before it — never applied
    // in a real run; see yaw_weight_of_pitch_deg().
    x4vr::HeadAngles ha;
    ha.yaw_deg = yaw;
    ha.pitch_deg = pitch;
    ha.yaw_weight = x4vr::yaw_weight_of_pitch_deg(pitch);
    ha.yaw_reliable = ha.yaw_weight > 0.0f;
    // **The real interval between ticks, for the rate fix.** Measured here
    // rather than assumed, because the whole point is that it wanders: take 294
    // read p10 9.0 ms against p90 40.0. Clamped so a pause — alt-tab, a load —
    // cannot present a multi-second dt as one enormous gain.
    static double last_step_s = 0.0;
    const double now_step_s = mono_seconds();
    const float step_dt =
        last_step_s > 0.0
            ? (float)std::min(0.200, std::max(0.001, now_step_s - last_step_s))
            : 0.0f;
    last_step_s = now_step_s;
    // Write mode has no servo: the angle is written, not chased, so nothing
    // here may push a mouse delta. `d` stays zero and every count below that
    // derives from it reads "asked for nothing", which is the truth.
    const bool write_mode = headlook_write_enabled();
    x4vr::Delta d{};
    if (!write_mode)
        d = x4vr::head_look_step(g_headlook, ha, step_dt);

    if (have_last) {
        float step = std::fabs(g_headlook.cmd_yaw_deg - pre_cmd);
        if (step < 180.0f) {                       // not the +-180 wrap itself
            const float ap = std::fabs(pitch);
            float &slot = ap >= 82.0f ? worst_deep
                                      : (ap >= 65.0f ? worst_fade : worst_level);
            if (step > slot)
                slot = step;
        }
    }
    last_cmd_yaw = g_headlook.cmd_yaw_deg;
    have_last = true;
    (void)last_cmd_yaw;
    {
        static uint64_t fn = 0;
        if ((fn++ % 900) == 0)
            X4VR_LOG("fade: worst single-frame yaw command step — level "
                     "%.3f deg, fade band (65-82) %.3f, deep (>=82) %.3f. The "
                     "deep figure is the one the cliff broke: above 82 the "
                     "weight is under 0.05 and the command must barely move",
                     worst_level, worst_fade, worst_deep);
    }

    // Reported whether or not anything is sent yet, because this line is how
    // the calibration run recovers the gain: a known command against the angle
    // camera_rotation.py --integrate measures.
    // Re-asserted every tick. X4 never called SDL_GetKeyboardState in take
    // 118, so the hold is an event, and an event has to be repeated to
    // survive anything that resets X4's own key state.
    // The key only has to keep X4's held state alive, so it is re-asserted on a
    // slow cadence rather than every frame: one every 32 poses is ~3 Hz at
    // headset rate, enough to recover from a save-load reset within a blink and
    // far too slow to be a flood.
    // **Activation needs an edge; sustaining needs a level.** Take 126 measured
    // both halves. Patola's tap was a down and an up three MICROseconds apart,
    // and mouselook then stayed on for 32.7 seconds -- our repeated downs held
    // it. But our downs had been arriving for 100 seconds before that tap
    // without ever switching it on.
    //
    // The reading that fits: our first down lands during loading or a menu,
    // when free-look is not allowed. X4 records the key as held, discards it,
    // and because we never send an up it never sees a fresh down-edge once the
    // cockpit is ready. A level cannot re-arm what a level already set.
    //
    // So while not steering, re-assert as up-then-down to manufacture a fresh
    // edge each cycle; once steering, send only downs, because an up would
    // release the very state we are holding.
    // The direct question first, the heuristic only as a backstop.
    // **One predicate, computed once and used at every site.** Take 147 is why
    // this is a variable rather than a condition spelled out wherever it is
    // needed. The HUD gate went into `steering`, and the re-assert below kept
    // its own copy of the test -- `cockpit` alone, the one query that provably
    // cannot see the map. So we released the key when the map opened and put it
    // straight back on:
    //
    //     map opens 26026.247, released 26026.376, RE-PRESSED 26026.861
    //
    // and X4 spent the next 23 seconds in mouselook with the map up. Patola
    // felt it as the mouse rotating the view instead of moving the cursor, so
    // the map could not be dragged at all. Two sites, one meaning, one of them
    // updated: that is the entire defect, and it was introduced by the commit
    // that fixed the other half.
    const X4View view = x4_view();
    const bool cockpit = view == X4View::Cockpit;
    // Paused (the ESC menu) is still the cockpit's camera: X4 draws that menu
    // through it exactly as it draws the map (take 329).
    const bool cockpit_camera = cockpit || view == X4View::Paused;
    const bool armed_now = g_relative_mouse.load(std::memory_order_relaxed);

    // IsHUDActive dereferences its global with no null check, so it must not be
    // called before X4 has built that object (see x4_hud_active). Relative
    // mouse mode proves the object exists -- but the moment we most need the
    // answer is exactly when we are NOT in relative mouse mode and are deciding
    // whether to press the key again, and that is the hole take 147 fell
    // through.
    //
    // So latch it rather than requiring it to be true right now. X4 calls
    // IsHUDActive itself while the HUD is inactive -- every path in its
    // disassembly, the false ones included, dereferences the same pointer -- so
    // the object is not torn down when the HUD goes away. Once seen alive it
    // stays alive, and the query is safe from then on whatever the mouse mode.
    static bool hud_object_exists = false;
    if (armed_now)
        hud_object_exists = true;
    // Write mode never enters relative mouse mode, so that latch never sets
    // and the map would go unseen. It asks only when the NULL guard decoded:
    // then a not-yet-built HUD reads as "down" and nothing is written before
    // the cockpit exists.
    const bool hud = write_mode
                         ? (x4_hud_slot() ? x4_hud_active() : true)
                         : (hud_object_exists ? x4_hud_active() : true);

    // What every site below actually means by "the cockpit".
    const bool may_steer = cockpit && hud;

    // **In the pilot seat** -- write mode only. Take 334: out of the seat
    // X4's camera stays in mode 0 with the same attached object, so the
    // write armed on foot -- and reached nothing. GetCameraRotation ignored
    // it there (r 0.00 against the head over 29 windows) and Patola saw the
    // view stay put. Writing on regardless made `steering` true, so the
    // layer declared X4's rendered rotation and the picture swung away
    // whenever the arrow keys turned him, and the player's mouse was zeroed
    // (below, before this fix). Off the seat nothing is written; the layer
    // declares identity, as in a menu.
    // Quiet until he has first sat: the load screen has no ship either.
    //
    // X4VR_HEADLOOK_FOOT: with 0x1929f70 patched, off the seat X4 composes
    // the camera's own rotation after the player's, so the write acts there
    // too (`write_here`) -- B, the head looking while the body walks.
    const bool seated = !write_mode || x4_seated();
    const bool write_here = seated || g_footpatch.patched;
    const bool on_foot = write_mode && !seated && g_footpatch.patched;
    static bool was_seated = false, ever_seated = false;
    if (seated != was_seated) {
        was_seated = seated;
        if (seated || ever_seated)
            X4VR_LOG("headlook: %s",
                     seated ? "in the pilot seat — the write may act"
                     : g_footpatch.patched
                         ? "out of the pilot seat — head-look continues on "
                           "foot (X4VR_HEADLOOK_FOOT): the head looks, the "
                           "mouse and arrows turn the body, the mouse's pitch "
                           "is zeroed"
                         : "out of the pilot seat — the write stands down "
                           "(on foot X4 does not render through the "
                           "cockpit's free-look, take 334); the picture is "
                           "a window fixed in front");
        ever_seated = ever_seated || seated;
    }

    // "— idle" / "the cockpit" are what score_run.py keys on; take 329 split
    // the old "external/floating" into the three cases it covered.
    static X4View was_view = X4View::Cockpit;
    if (view != was_view) {
        was_view = view;
        X4VR_LOG("headlook: view is now %s",
                 view == X4View::Cockpit    ? "the cockpit — steering"
                 : view == X4View::External ? "external — idle"
                 : view == X4View::Floating ? "floating — idle"
                                            : "PAUSED (IsGamePaused: the ESC "
                                              "menu) — idle");
    }
    static bool was_hud = true;
    if (hud != was_hud) {
        was_hud = hud;
        X4VR_LOG("headlook: HUD is now %s (IsInPanelMode=%d) — %s",
                 hud ? "up" : "down", (int)x4_panel_mode(),
                 hud ? "steering" : "idle, mouse handed back");
    }

    // **Level, not edge.** Pressing a free-look key when we know free-look is
    // not what is on screen was never right, and releasing it once was not
    // enough: an edge-triggered release cannot correct a re-entry it did not
    // observe, and take 147 re-entered on the very next re-assert with
    // `was_hud` already false, so the edge never came again. Hold the key while
    // we may steer, and keep letting go for as long as we may not.
    static uint64_t poses = 0;
    const bool cycle = (poses++ % 32) == 0;
    if (may_steer && cycle && !write_mode) {
        if (!armed_now)
            send_key(headlook_scancode(), false);
        send_key_down(headlook_scancode());
        g_camwrite.mouselook_downs++;
    } else if (!may_steer && armed_now && cycle) {
        send_key(headlook_scancode(), false); // hand the mouse back, and again
    }
    // Only while X4 is actually in mouselook. Take 126 ran the immersive
    // treatment over load screens and menus too, where it does not belong:
    // there is no camera to follow, so the flat panel tilts with the head and
    // Patola asked for the quad back. Worse, mouse deltas sent outside
    // mouselook are cursor movement -- we would be dragging the pointer around
    // a menu we cannot see.
    //
    // g_relative_mouse is the signal, and it costs nothing: X4 turns relative
    // mode on for mouselook and off for anything with a pointer, and the cursor
    // shim has tracked it since #19.
    // Degrees we actually asked X4 for this frame, against what it did.
    const float gpc = g_headlook.gain_pitch_deg_per_count > 0.0f
                          ? g_headlook.gain_pitch_deg_per_count
                          : g_headlook.gain_deg_per_count;
    const float commanded_deg =
        std::fabs((float)d.dx * g_headlook.gain_deg_per_count) +
        std::fabs((float)d.dy * gpc);
    const int stall_event =
        x4vr::stall_watch_step(g_stall, commanded_deg, obs_moved);
    // **Reported, never acted on.** This watch has now broken head tracking in
    // three consecutive runs and has never once done its job, so it is out of
    // the control path.
    //
    // The last failure is the one that settles it. Before free-look engages --
    // during loading, and for the first moments in the cockpit -- our commands
    // legitimately do nothing, because there is no free-look yet to move
    // anything. The watch reads that as a dead camera and releases the key, so
    // free-look can NEVER engage: take 141 stood down 32 times and X4 read
    // exactly 0.0000 for the whole session. A guard that prevents the thing it
    // guards from starting is worse than the hazard it was added for.
    //
    // The hazard was real -- the map accelerated away -- but x4_view()
    // asks X4 directly, which is the right shape of answer. If that turns out
    // not to cover the map, the fix is to find the query that does, not to
    // re-arm an inference that has failed in three different ways.
    //
    // Take 146 tested that. It did not cover the map, the map accelerated away
    // again, and the fix was the one written above rather than this watch:
    // IsHUDActive, declared in X4's cdefs. The prediction stands as it was.
    if (stall_event == 1)
        X4VR_LOG("headlook: NOTE commanded with little camera response — "
                 "reported only, not acted on");

    // **Write mode: the head's angle into X4's free-look matrix.** Steering
    // means "X4's camera is ours this tick", which in write mode is the write
    // being allowed -- not relative mouse mode, which never comes. A single
    // failed write (EFAULT mid-teardown) does not drop it: the camera is
    // still ours, and dropping it would publish an identity pose.
    bool write_ready = false;
    if (write_mode)
        headlook_speed_tick(x4_player_camera());
    if (write_mode && may_steer && write_here) {
        const uintptr_t cam = x4_player_camera();
        write_ready = camwrite_ready(cam, g_headlook.pitch_limit_deg,
                                     g_headlook.yaw_limit_deg);
        if (write_ready) {
            float ty = 0.f, tp = 0.f;
            x4vr::camwrite::write_target(g_camwrite.have ? g_camwrite.yaw : yaw,
                                         yaw, pitch, ha.yaw_weight,
                                         g_headlook.yaw_limit_deg,
                                         g_headlook.pitch_limit_deg, &ty, &tp);
            camwrite_sway_sample(cam);
            if (camwrite_apply(cam, ty, tp)) {
                g_camwrite.yaw = ty;
                g_camwrite.pitch = tp;
                g_camwrite.have = true;
                // What was asked of X4, where every log line and the layer's
                // declared pose expect the command to be.
                g_headlook.cmd_yaw_deg = ty;
                g_headlook.cmd_pitch_deg = tp;
            }
        }
    }
    // **X4VR_HEADLOOK_MENU_CENTRE: the spring-back X4VR_HEADLOOK_WRITE took
    // away.** With the HUD down in the cockpit the head drives nothing, and
    // X4 would have recentred its look-around here by itself (take 311);
    // anglespeed 0 stops that, so the camera stayed at the head's angle and
    // X4's menus were drawn -- and hit-tested -- through it (take 328B). The
    // same guarded write, the angle 0,0, re-asserted every tick for as long
    // as the HUD stays down; `steering` stays false, so nothing here is
    // declared as head motion.
    static bool centred_period = false;
    // Take 339: a HUD-down menu is X4's UI as much as a full-screen one --
    // the camera is centred precisely so that UI fills the window -- but off
    // the seat X4 calls the teleporters' menus none of the latter, so the
    // cursor stayed raw over them. Each period's arm is chosen as it starts:
    // the canvas, or under X4VR_CURSOR_CENTRED_AB alternately raw for the
    // periods off the seat. In the seat every period so far was full-screen
    // too (333: 19 of 20, the 20th the save loading, before ARMED).
    static bool centred_canvas = false, centred_logged = false;
    static uint64_t centred_off_seat = 0;
    if ((hud || !cockpit_camera) && centred_logged) {
        centred_logged = false;
        X4VR_LOG("ui: menu with the HUD down %llu closed — the cursor is "
                 "drawn at the raw pointer",
                 (unsigned long long)centred_off_seat);
    }
    if (hud || !cockpit_camera)
        centred_period = false;
    if (write_mode && cockpit_camera && !hud && write_here && headlook_menu_centre()) {
        const uintptr_t cam = x4_player_camera();
        if (camwrite_ready(cam, g_headlook.pitch_limit_deg,
                           g_headlook.yaw_limit_deg)) {
            const float was_yaw = g_camwrite.yaw, was_pitch = g_camwrite.pitch;
            if (camwrite_apply(cam, 0.f, 0.f, &g_camwrite.centre_writes)) {
                g_camwrite.yaw = g_camwrite.pitch = 0.f;
                g_camwrite.have = true;
                g_headlook.cmd_yaw_deg = g_headlook.cmd_pitch_deg = 0.f;
                if (!centred_period) {
                    centred_period = true;
                    g_camwrite.centre_periods++;
                    X4VR_LOG("headlook: HUD down %s%s — X4's "
                             "camera CENTRED from yaw %.2f pitch %.2f to 0,0, "
                             "so X4's menus fill the window the pointer is "
                             "confined to (X4VR_HEADLOOK_MENU_CENTRE=0 leaves "
                             "it where the head was, take 328B)",
                             seated ? "in the cockpit" : "off the pilot seat",
                             cockpit ? "" : " (PAUSED)", was_yaw, was_pitch);
                    centred_canvas = true;
                    if (!seated && cursor_canvas_auto()) {
                        centred_off_seat++;
                        centred_logged = true;
                        centred_canvas = !cursor_centred_ab() ||
                                         (centred_off_seat % 2) == 1;
                        X4VR_LOG("ui: menu with the HUD down %llu OPEN off "
                                 "the pilot seat — the cursor is drawn %s",
                                 (unsigned long long)centred_off_seat,
                                 centred_canvas
                                     ? "through the UI canvas (arm A)"
                                     : "at the raw pointer (arm B, take 339's)");
                        if (cursor_centred_ab())
                            X4VR_LOG("milestone: menu with the HUD down %llu "
                                     "— cursor %s",
                                     (unsigned long long)centred_off_seat,
                                     centred_canvas ? "A (canvas)" : "B (raw)");
                    }
                }
            }
        }
    }
    if (x4vr::Shared *sh = x4vr_shared_state())
        sh->centred_menu.store(
            centred_period && centred_canvas && cursor_canvas_auto() ? 1u : 0u,
            std::memory_order_relaxed);
    if (write_mode)
        camwrite_report(false);

    // may_steer, not a second spelling of it. This site having its own copy of
    // the test is what take 147 cost.
    const bool steering =
        write_mode ? write_ready : (armed_now && may_steer);
    g_headlook_steering.store(steering, std::memory_order_relaxed);
    // On foot only while the write is actually ours this tick: that is when
    // the mouse's pitch is zeroed and the layer declares the head part.
    const bool foot_now = on_foot && steering;
    g_on_foot.store(foot_now, std::memory_order_relaxed);
    g_off_seat.store(on_foot, std::memory_order_relaxed);
    if (x4vr::Shared *sh = x4vr_shared_state())
        sh->on_foot.store(foot_now ? 1u : 0u, std::memory_order_relaxed);
    g_in_cockpit.store(cockpit, std::memory_order_relaxed);
    g_view_known.store(true, std::memory_order_relaxed);

    // **Task #73: open-loop plant identification REPLACES the servo.**
    //
    // Everything take 212 established about X4's camera was inferred through a
    // closed loop, which chooses its command from the very error it is trying
    // to measure. Two explanations built that way died in one session. This
    // drives the plant with a fixed, known sequence instead and simply watches.
    //
    // The head is not in the loop: the player can move however they like and
    // not one command changes.
    //
    // **Sited after `steering`, and gated on it.** The sequence must not
    // advance on a tick whose command is discarded -- during a load, in the
    // menu, or in external view the delta never reaches X4, and an element
    // whose SUSTAIN ran while nothing was being sent would read as "the camera
    // delivered nothing", which is a conclusion this run is specifically meant
    // to be able to draw honestly. An earlier revision stepped it above, where
    // `steering` is not yet known.
    if (plantid_enabled()) {
        static x4vr::PlantId pid;
        static bool said = false, waited = false;
        if (!said) {
            said = true;
            X4VR_LOG("plantid: open-loop sequence armed — %d sizes x 2 "
                     "directions x %d pass(es) = %d measured ticks (~%.0f s at "
                     "24 Hz, plus reset). The servo is OFF and the head drives "
                     "NOTHING for the whole run.",
                     x4vr::PlantId::kNumSizes, pid.passes,
                     x4vr::plant_id_total_steps(pid),
                     x4vr::plant_id_total_steps(pid) / 24.0);
        }
        if (!steering) {
            if (!waited) {
                waited = true;
                X4VR_LOG("plantid: holding — not steering yet (cockpit + HUD "
                         "up + relative mouse). The sequence does not advance "
                         "until a command can actually reach X4.");
            }
            d.dx = d.dy = 0;
        } else {
            waited = false;
            const float cam_now = kRad2Deg * -r.yaw;
            const x4vr::PlantStep ps =
                x4vr::plant_id_step(pid, cam_now, have_cam, 0.115f);
            d.dx = ps.dx;
            d.dy = 0;
            d.clamped = false;
            // **Logged only on the ticks where the sequence ADVANCED**, which is
            // once per camera update. The readback is the camera BEFORE this
            // tick's command -- the same pairing lagsample uses, so the two
            // instruments can be read against each other.
            //
            // It used to log every tick, and that is take 289's defect: with 5.7
            // ticks per update the scorer's lag profile was indexed by tick, so
            // one camera update smeared across six lines and the impulse
            // response came back "wider" purely because the tick rate had
            // tripled. One line per observation puts the measurement in the
            // readback's own time base at any tick rate.
            static bool said_ratio = false;
            if (ps.stepped && !said_ratio &&
                pid.phase != x4vr::PlantPhase::Calibrate) {
                said_ratio = true;
                X4VR_LOG("plantid: calibrated %.2f servo ticks per camera "
                         "update (%d change(s) in %d ticks). Phases are counted "
                         "in UPDATES from here, so this run is the same "
                         "experiment as one at any other tick rate.",
                         pid.ticks_per_update, pid.cal_changes, pid.cal_cmp);
            }
            if (ps.stepped)
                X4VR_LOG("plantid: pass=%d elem=%d size=%+d phase=%s k=%d dx=%+d "
                         "cam=%.4f meas=%d camok=%d%s",
                         pid.pass, pid.element, pid.size_counts(),
                         x4vr::plant_phase_name(ps.acted), pid.k, ps.dx, cam_now,
                         ps.measuring ? 1 : 0, have_cam ? 1 : 0,
                         pid.finished ? " FINISHED" : "");
            if (pid.finished && !g_plantid_done.exchange(true))
                X4VR_LOG("plantid: sequence COMPLETE — every element ran. The "
                         "run can be ended now; nothing after this line is "
                         "part of the measurement.");
        }
    }

    // **Task #75: the dither, added AFTER the servo has stepped.** The estimate
    // never contains it, so it is a disturbance rather than a command: the servo
    // sees it only as camera motion it did not ask for and regulates it away.
    //
    // Gated on `steering`, and that is load-bearing rather than tidy. A delta
    // sent while not steering never reaches X4, so recording it as plant input
    // would put a term in the correlation that the plant never received and the
    // recovered response would be wrong by however long the gap lasted.
    if (dither_counts() > 0 && steering) {
        const int dv = dither_next(dither_counts());
        d.dx += dv;
        // `r` was read at the TOP of this tick, before anything was sent, so
        // `cam` is the state the logged command acted on. The scorer differences
        // consecutive lines to get the motion, which is why every tick has to be
        // logged and none may be skipped.
        if (have_cam)
            X4VR_LOG("dither: v=%+d dx=%+d cam=%.4f", dv, d.dx,
                     kRad2Deg * -r.yaw);
    }

    if (steering && !write_mode)
        send_mouse_delta(d.dx, d.dy);

    // Tell the layer where X4 is now looking, so the frame is submitted with
    // the pose it was rendered from rather than with identity. Cleared when not
    // steering, which puts the layer back to the world-locked quad.
    if (x4vr::Shared *sh = x4vr_shared_state()) {
        // **One publish, under the camera seqlock.** These were seven separate
        // relaxed stores, and the layer read them at two different points in its
        // submission block -- so `poselag` could print `x4=` from one tick and
        // `cmd=` from another, and the declared pose could mix a yaw and a pitch
        // that never coexisted. See share_write_camera for the whole account.
        x4vr::CamSnap snap;
        snap.yaw_deg = g_headlook.cmd_yaw_deg;
        snap.pitch_deg = g_headlook.cmd_pitch_deg;
        snap.valid = steering;
        // **Task #75: what X4 actually said, beside what we believe.**
        //
        // `r` was read at the TOP of this tick, before the servo stepped, so it
        // is X4's camera as it stood before this frame's command -- while
        // cam_yaw_deg above already contains that command. The gap between them
        // is exactly the angle the compositor would reproject by that the
        // pixels were never drawn with, and until now nothing carried both
        // numbers on one line.
        //
        // Stamped, because the readback is older than the frame the layer is
        // about to submit and "how much older" must be visible rather than
        // assumed small.
        // Carried forward when this tick had no reading, which is what the
        // seven separate stores did too: `cam_raw_valid` says which it is, and
        // overwriting a real angle with zero would make "no answer" and "centred"
        // the same value again.
        if (have_cam) {
            snap.raw_yaw_deg = kRad2Deg * -r.yaw;
            snap.raw_pitch_deg = kRad2Deg * r.pitch;
        } else {
            snap.raw_yaw_deg =
                sh->cam_raw_yaw_deg.load(std::memory_order_relaxed);
            snap.raw_pitch_deg =
                sh->cam_raw_pitch_deg.load(std::memory_order_relaxed);
        }
        snap.raw_valid = have_cam;
        snap.raw_ns = (uint64_t)(mono_seconds() * 1e9);
        x4vr::share_write_camera(sh, snap);
    }

    // The camera stops being ours when X4 stops listening, so the estimate has
    // to go back to where X4 will have left it rather than keeping a belief it
    // can no longer act on. This is the same reset a seat change needs.
    static bool was_steering = false;
    if (was_steering && !steering)
        x4vr::head_look_recentre(g_headlook);
    was_steering = steering;

    static uint64_t n = 0;
    // Every 30 poses, not 240. Take 129 was a clean controlled excursion and
    // the gain still could not be read off it, because at one sample in 240 the
    // log missed the plateau entirely: cmd appeared to peak at 12.6 deg while
    // the dumps showed 36. A calibration run whose instrument samples slower
    // than the thing it measures is not a calibration run.
    // **Close the loop.** Take 135 established what GetCameraRotation reports:
    // RADIANS, with yaw of the opposite sign to ours, linear to within 0.8% over
    // a 30 degree excursion. So the estimate stops being a belief.
    //
    // With an observation in hand the three defects that outlasted every patch
    // all go at once. The gain only has to be roughly right, because a servo
    // corrects what it gets wrong. The clamp is SEEN rather than modelled, so
    // there is no windup to discharge as a jolt. And a recentre on a seat change
    // or save load fixes itself on the next frame instead of leaving a permanent
    // offset nothing could detect.
    if (have_cam) {
        static uint64_t rn = 0;
        if ((rn++ % 30) == 0)
            X4VR_LOG("camread: X4 %.2f,%.2f deg (raw %.4f,%.4f) | cmd %.2f,%.2f "
                     "| head %.2f,%.2f%s",
                     kRad2Deg * -r.yaw, kRad2Deg * r.pitch, r.yaw, r.pitch,
                     g_headlook.cmd_yaw_deg, g_headlook.cmd_pitch_deg, yaw,
                     pitch, g_camloop ? " CLOSED" : " open-loop");
    }

    // **Task #73's measurement, at FULL rate.**
    //
    // The lag was first measured from `camread`, which prints one sample in 30
    // -- about 350 ms apart at the headset's rate and 1.1 s at 20 fps. That was
    // enough for a SLOPE (error against head rate needs a spread of speeds, not
    // fine sampling) but it cannot resolve a ~40 ms lag directly, and it cannot
    // see which side of an A/B boundary a sample fell on. So this emits every
    // pose while the measurement is running.
    //
    // `view` and `arm` are STAMPED rather than left for the scorer to re-derive
    // from transition lines: an instrument that recomputes the decision it
    // reports can end up reporting a different one than the code took, which is
    // how 18 modules once logged "World" after being dispatched as UI.
    //
    // `cam=` is X4's readback as it stood BEFORE this tick's command, which is
    // the pairing the regression needs: error at the moment of observation.
    if (lag_log_enabled()) {
        X4VR_LOG("lagsample: arm=%d view=%s steer=%d cam=%d head=%.3f,%.3f "
                 "x4=%.3f,%.3f kparm=%d kp=%.3f ylim=%.1f rfarm=%d rftau=%.0f ugarm=%d",
                 lead_armed ? 1 : 0,
                 // "foot": off the seat under X4VR_HEADLOOK_FOOT, where x4=
                 // includes the body -- stamped, never re-derived.
                 !cockpit ? "external"
                 : g_on_foot.load(std::memory_order_relaxed) ? "foot"
                                                             : "cockpit",
                 steering ? 1 : 0, have_cam ? 1 : 0, yaw, pitch,
                 have_cam ? kRad2Deg * -r.yaw : 0.0f,
                 have_cam ? kRad2Deg * r.pitch : 0.0f,
                 g_kp_arm.load(std::memory_order_relaxed), g_headlook.servo_kp,
                 g_headlook.yaw_limit_deg,
         g_ratefix_arm.load(std::memory_order_relaxed),
         g_headlook.rate_fix_tau_ms,
         g_ungate_arm.load(std::memory_order_relaxed));
    }

    // **Task #75: do WE send sparsely, or does X4 ignore what we send?**
    //
    // X4's camera changes on a near-constant ~43% of RENDERED frames across
    // three runs -- 42.9% and 41.7% at 88 fps, 44.6% at 58 fps -- so it is tied
    // to the frame loop rather than to a fixed sim clock. Two hypotheses with
    // opposite fixes, and until now nothing in the log separated them:
    //
    //   ours   the servo emits a delta on only ~43% of ticks, because a count
    //          needs 0.23 deg of error (kp 0.25, gain 0.115, lround) and the
    //          0.15 deg dead zone sits below that. Fixable here: carry the
    //          rounding residue instead of discarding it.
    //   X4's   we send on nearly every tick and X4 samples the mouse at its own
    //          rate. Not fixable from this side, and the staleness has to be
    //          hidden rather than removed.
    //
    // COUNTED, not sampled: the `headlook:` line above fires every 30 ticks AND
    // on every clamp, so reading a rate off it would be reading a biased subset
    // -- which is how the 12.4%-zero-delta figure misled me once already.
    //
    // `cam_moved` is X4's readback changing between ticks, which is the same
    // event `camdiff` counts per frame from the other side of the process. The
    // two are expected to agree; if they do not, one of them is wrong.
    //
    // Two defects of this instrument, both fixed here rather than carried:
    //
    //   the window   it used to print totals since the first tick, so the last
    //                line of a run averaged over everything before it. Take
    //                283's final line said the camera acted on 33.4% and I
    //                printed that as a verdict; differencing consecutive lines
    //                gave 73%, the startup phase being the whole difference.
    //                Per-window now, with the window length on the line.
    //   the denominator
    //                `cam_moved` only incremented where `have_cam` held, but it
    //                was divided by ALL ticks -- so "the camera did not move"
    //                and "the camera could not be read" produced the same
    //                number, and a run with camread off would have reported 0%
    //                and looked like a frozen camera. `reads` is counted and
    //                used as the denominator, and printed so a low read rate
    //                cannot be mistaken for a low move rate.
    //   `sent`       counted `d.dx != 0`, but `d` is computed ~160 lines above
    //                where `steering` becomes known and send_mouse_delta is
    //                gated on it -- so every tick with the HUD down or the view
    //                external counted a delta that was computed and thrown away.
    //                Take 284 read "delta sent on 100.0%" against a camera
    //                moving on 13.1%, and concluded X4 was ignoring us; the
    //                protocol for that run deliberately included a still
    //                segment and a map segment, which is precisely when nothing
    //                was being sent. This is the SAME defect as `cam_moved`
    //                above -- counted at a site that cannot see the gate -- and
    //                I fixed one and left its twin in the same block.
    {
        static uint64_t ticks = 0, sent = 0, steer_ticks = 0, reads = 0,
                        cam_moved = 0, clamps = 0;
        static float last_cam = 0.0f;
        static bool have_last = false;
        static double t0 = 0.0;
        const double now = mono_seconds();
        if (t0 == 0.0)
            t0 = now;
        ticks++;
        // What actually left the process, under the same `steering` the send is
        // gated on -- and the steering ticks beside it, so "we sent nothing"
        // cannot be read as "we had nothing to send".
        if (steering)
            steer_ticks++;
        if (steering && (d.dx != 0 || d.dy != 0))
            sent++;
        if (d.clamped)
            clamps++;
        if (have_cam) {
            const float c = kRad2Deg * -r.yaw;
            // For camprobe to compare against. Whichever thread the tick is on,
            // this is the reading taken through X4's own call path.
            g_camprobe.drain_yaw.store(c, std::memory_order_relaxed);
            g_camprobe.drain_seen.fetch_add(1, std::memory_order_relaxed);
            // Only a read that has a predecessor can show a change. Counting
            // the first one would credit a move that was never observed.
            if (have_last) {
                reads++;
                if (c != last_cam)
                    cam_moved++;
            }
            last_cam = c;
            have_last = true;
        } else {
            // A gap breaks the pair: the next reading would be compared against
            // one from before the gap, and any change across it is unattributed.
            have_last = false;
        }
        const double dt = now - t0;
        if (dt >= 2.0) {
            X4VR_LOG("inputrate: %llu ticks in %.2f s = %.1f Hz | steering on "
                     "%.1f%% of ticks, delta sent on %.1f%% of STEERING ticks "
                     "(=%.1f Hz) | camera read on %.1f%% of ticks, moved "
                     "on %.1f%% of READS (=%.1f Hz) | clamped %.1f%% | deadzone "
                     "%.2f deg, %.3f deg per count, kp %.2f -> a count needs "
                     "%.3f deg of error",
                     (unsigned long long)ticks, dt, ticks / dt,
                     100.0 * (double)steer_ticks / (double)ticks,
                     steer_ticks ? 100.0 * (double)sent / (double)steer_ticks
                                 : 0.0,
                     sent / dt, 100.0 * (double)reads / (double)ticks,
                     reads ? 100.0 * (double)cam_moved / (double)reads : 0.0,
                     cam_moved / dt, 100.0 * (double)clamps / (double)ticks,
                     g_headlook.dead_zone_deg, g_headlook.gain_deg_per_count,
                     g_headlook.servo_kp,
                     0.5f * g_headlook.gain_deg_per_count /
                         (g_headlook.servo_kp > 0.0f ? g_headlook.servo_kp
                                                     : 1.0f));
            ticks = sent = steer_ticks = reads = cam_moved = clamps = 0;
            t0 = now;
        }
    }

    if ((n++ % 30) == 0 || d.clamped)
        X4VR_LOG("headlook: head %.2f,%.2f -> cmd %.2f,%.2f delta %d,%d%s",
                 yaw, pitch, g_headlook.cmd_yaw_deg, g_headlook.cmd_pitch_deg,
                 d.dx, d.dy, d.clamped ? " CLAMPED" : "");
}

// The tick's own cadence. Started lazily from the drain hook, so it cannot exist
// before X4 is up and SDL is running, and never exists in a non-game process.
static void tick_driver_start() {
    if (!tick_hz() || !this_is_the_game())
        return;
    static std::once_flag once;
    std::call_once(once, [] {
        const int hz = tick_hz();
        X4VR_LOG("headlook: tick driver is OURS at %d Hz — X4's event drain no "
                 "longer calls the tick. Take 284 measured the drain delivering "
                 "46.5 ticks/s against 89.9 poses published (0.79 of "
                 "min(pump,published)); this run asks whether a cadence of our "
                 "own reaches the publish rate. Watch `pumprate` for fresh, and "
                 "`inputrate` for whether X4 still acts on ~0.7-1.0 per tick.",
                 hz);
        std::thread([hz] {
            struct timespec next;
            clock_gettime(CLOCK_MONOTONIC, &next);
            const long period_ns = 1000000000L / (hz > 0 ? hz : 1);
            uint64_t late = 0, ticks = 0, resync = 0;
            double t_log = mono_seconds();
            for (;;) {
                next.tv_nsec += period_ns;
                while (next.tv_nsec >= 1000000000L) {
                    next.tv_nsec -= 1000000000L;
                    next.tv_sec++;
                }
                // **Never spin to catch up.** With absolute deadlines, a tick
                // that overruns its period leaves `next` in the past, and
                // clock_nanosleep then returns immediately -- so a slow patch
                // would turn this into a busy loop pushing SDL events as fast as
                // it can, which is precisely take 119's drain livelock. If the
                // deadline is more than two periods stale, give up the missed
                // ticks and re-phase to now. Dropping ticks is the benign
                // failure; bursting is not.
                {
                    struct timespec now_ts;
                    clock_gettime(CLOCK_MONOTONIC, &now_ts);
                    const double behind =
                        (double)(now_ts.tv_sec - next.tv_sec) +
                        (double)(now_ts.tv_nsec - next.tv_nsec) * 1e-9;
                    if (behind > 2.0 * (double)period_ns * 1e-9) {
                        next = now_ts;
                        resync++;
                    }
                }
                // Absolute deadlines, so a slow tick is absorbed instead of
                // accumulating into drift. A relative sleep would make the real
                // cadence (period + work), which is the rate this run is
                // measuring -- the instrument would set its own answer.
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);
                const double slip = (now_ts.tv_sec - next.tv_sec) +
                                    (now_ts.tv_nsec - next.tv_nsec) * 1e-9;
                if (slip > 0.5 * period_ns * 1e-9)
                    late++;
                ticks++;
                headlook_tick();
                const double now = mono_seconds();
                if (now - t_log >= 2.0) {
                    // The cadence ACHIEVED, beside the one asked for. A driver
                    // that reported only its setting could not tell a fix from a
                    // thread that never ran.
                    X4VR_LOG("tickdrv: asked %d Hz, ran %.1f Hz over %.2f s, "
                             "%.1f%% of wakeups late by >half a period, %llu "
                             "re-phase(s) after overrun",
                             hz, ticks / (now - t_log), now - t_log,
                             100.0 * (double)late / (double)(ticks ? ticks : 1),
                             (unsigned long long)resync);
                    ticks = late = resync = 0;
                    t_log = now;
                }
            }
        }).detach();
    });
}

// **Task #75: is GetCameraRotation safe to call off X4's own thread?**
//
// This cannot be answered by reading -- X4 is closed source -- and its failure
// mode is a crash, so it must not be answered inside a VR take. The probe runs
// on its own thread at a set rate and does nothing but call the getter and
// compare it against the last value X4's own thread read, so it can be run
// FLATSCREEN with no headset: the cheapest configuration in which it can fail.
//
// It is deliberately NOT part of the tick. The tick reads the camera only after
// the freshness gate, and with no layer publishing there are no fresh poses, so
// a flatscreen run of the tick would never reach the call being tested. An
// instrument that cannot execute the thing it measures has failed twice in this
// project already.
//
// `disagree` is not itself a defect: the two threads sample at different
// instants, so the values SHOULD differ while the camera moves. What would be a
// defect is a value outside the plausible range, or the process dying.
// (CamProbe itself is declared beside PumpRate, above — the tick writes to it.)

static int camprobe_hz() {
    static const int hz = [] {
        const char *e = getenv("X4VR_CAMPROBE_HZ");
        const int v = e && *e ? atoi(e) : 0;
        return (v < 0 || v > 1000) ? 0 : v;
    }();
    return hz;
}

// Accumulated by both sides; `self` is the control and `ours` the measurement.
struct CamLag {
    std::atomic<uint64_t> self_n{0}, ours_n{0}, refused{0};
    std::atomic<uint64_t> self_us{0}, ours_us{0};   // summed lag, microseconds
    std::atomic<uint64_t> self_max_us{0}, ours_max_us{0};
    std::atomic<uint64_t> ours_resid_milli{0};      // summed match residual, 1e-3 deg
} g_camlag;

static void bump_max(std::atomic<uint64_t> &m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed))
        ;
}

// Called from X4's OWN thread, in the event-drain hook. Pushes the reading into
// the ring, then matches that same reading against the ring MINUS the entry it
// just wrote. That is the control: a value known to be current, run through the
// identical matcher, so the measurement below has a scale to be read against
// rather than a bare number of milliseconds.
static void camring_tick_on_x4_thread() {
    if (!camprobe_hz() || !this_is_the_game())
        return;
    X4Rotation r{};
    if (!x4_camera_rotation(&r))
        return;
    const float y = kRad2Deg * -r.yaw;
    const double now = mono_seconds();
    double lag = 0.0;
    float spread = 0.0f, resid = 0.0f;
    if (g_camring.age_of(y, now, 1, &lag, &spread, &resid)) {
        g_camlag.self_n.fetch_add(1, std::memory_order_relaxed);
        g_camlag.self_us.fetch_add((uint64_t)(lag * 1e6),
                                   std::memory_order_relaxed);
        bump_max(g_camlag.self_max_us, (uint64_t)(lag * 1e6));
    }
    g_camring.push(y, now);
}

static void camprobe_start() {
    if (!camprobe_hz() || !this_is_the_game())
        return;
    static std::once_flag once;
    std::call_once(once, [] {
        const int hz = camprobe_hz();
        X4VR_LOG("camprobe: starting a thread calling GetCameraRotation at %d "
                 "Hz. This answers ONLY whether the call survives off X4's own "
                 "thread — it drives nothing and steers nothing. Run it "
                 "flatscreen first; if X4 dies, the tick driver must not call "
                 "the getter from its thread.",
                 hz);
        std::thread([hz] {
            struct timespec next;
            clock_gettime(CLOCK_MONOTONIC, &next);
            const long period_ns = 1000000000L / (hz > 0 ? hz : 1);
            double t_log = mono_seconds();
            for (;;) {
                next.tv_nsec += period_ns;
                while (next.tv_nsec >= 1000000000L) {
                    next.tv_nsec -= 1000000000L;
                    next.tv_sec++;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
                X4Rotation r{};
                if (!x4_camera_rotation(&r))
                    continue;
                const float y = kRad2Deg * -r.yaw, p = kRad2Deg * r.pitch;
                g_camprobe.calls.fetch_add(1, std::memory_order_relaxed);
                if (y != 0.0f || p != 0.0f)
                    g_camprobe.nonzero.fetch_add(1, std::memory_order_relaxed);
                // A free-look camera cannot be outside these bounds. Anything
                // else means the read raced something, which is the finding.
                if (!(y > -200.0f && y < 200.0f && p > -200.0f && p < 200.0f))
                    g_camprobe.absurd.fetch_add(1, std::memory_order_relaxed);
                g_camprobe.last_yaw.store(y, std::memory_order_relaxed);
                g_camprobe.last_pitch.store(p, std::memory_order_relaxed);
                const double now = mono_seconds();
                // The measurement: how old is the value WE just read, against
                // the series X4's own thread recorded? Fast motion spreads the
                // ring's values apart and makes the match sharper; it cannot
                // make the match old. Only a stale read can.
                {
                    double lag = 0.0;
                    float spread = 0.0f, resid = 0.0f;
                    if (g_camring.age_of(y, now, 0, &lag, &spread, &resid)) {
                        g_camlag.ours_n.fetch_add(1, std::memory_order_relaxed);
                        g_camlag.ours_us.fetch_add((uint64_t)(lag * 1e6),
                                                   std::memory_order_relaxed);
                        bump_max(g_camlag.ours_max_us, (uint64_t)(lag * 1e6));
                        g_camlag.ours_resid_milli.fetch_add(
                            (uint64_t)(resid * 1000.0f),
                            std::memory_order_relaxed);
                    } else {
                        g_camlag.refused.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (now - t_log >= 2.0) {
                    t_log = now;
                    X4VR_LOG("camprobe: %llu call(s) off-thread, %llu non-zero, "
                             "%llu ABSURD | probe %.3f,%.3f | X4-thread %.3f "
                             "(seen %u) | alive",
                             (unsigned long long)g_camprobe.calls.load(
                                 std::memory_order_relaxed),
                             (unsigned long long)g_camprobe.nonzero.load(
                                 std::memory_order_relaxed),
                             (unsigned long long)g_camprobe.absurd.load(
                                 std::memory_order_relaxed),
                             y, p,
                             g_camprobe.drain_yaw.load(std::memory_order_relaxed),
                             g_camprobe.drain_seen.load(
                                 std::memory_order_relaxed));
                    // Control and measurement on ONE line, so the lag can never
                    // be quoted without the number that gives it a scale.
                    const uint64_t sn = g_camlag.self_n.load(
                        std::memory_order_relaxed);
                    const uint64_t on = g_camlag.ours_n.load(
                        std::memory_order_relaxed);
                    X4VR_LOG("camlag: CONTROL (X4's own thread) n=%llu mean "
                             "%.2f ms max %.2f | OURS (off-thread) n=%llu mean "
                             "%.2f ms max %.2f | match residual %.3f deg | "
                             "%llu refused (camera too still, spread < %.1f "
                             "deg) | ring %d entries",
                             (unsigned long long)sn,
                             sn ? g_camlag.self_us.load(
                                      std::memory_order_relaxed) /
                                      (double)sn / 1000.0
                                : 0.0,
                             g_camlag.self_max_us.load(
                                 std::memory_order_relaxed) / 1000.0,
                             (unsigned long long)on,
                             on ? g_camlag.ours_us.load(
                                      std::memory_order_relaxed) /
                                      (double)on / 1000.0
                                : 0.0,
                             g_camlag.ours_max_us.load(
                                 std::memory_order_relaxed) / 1000.0,
                             on ? g_camlag.ours_resid_milli.load(
                                      std::memory_order_relaxed) /
                                      (double)on / 1000.0
                                : 0.0,
                             (unsigned long long)g_camlag.refused.load(
                                 std::memory_order_relaxed),
                             (double)x4vr::CamRing::kMinSpread, x4vr::CamRing::N);
                }
            }
        }).detach();
    });
}

// **Task #75: one line that accounts for every published pose.**
//
// Called from the same site as headlook_tick, under the same condition, so the
// report always describes whichever hook is actually driving. Putting it only in
// SDL_PumpEvents would have left the instrument silent in exactly the
// configuration where SDL_PeepEvents drives.
//
// Fixed 2 s windows. Not "every N ticks": the window length would then be set by
// the very rate being measured, so a slow phase would be averaged over a longer
// span than a fast one and the two could not be compared. Windows also make the
// phases Patola flies visible -- take 283's rate fell from ~88 Hz early to
// ~52-58 Hz in gameplay, and a cumulative counter hid that behind one average.
// #71's controller census, defined with the SDL joystick hooks far below. It
// rides this cadence rather than owning one, so an idle run costs nothing.
void pad_report();
// Called from BOTH drains for every event X4 is handed; see Sdl3AxisEvent for
// why the event channel is what has to be watched.
void pad_note_event(uint32_t type, const Sdl3AxisEvent *a);
// The per-caller pointer ranges, whose 64-line cap take 307 hit.
void note_extent_report();
// #46: how often X4 set a cursor, which is its only hover signal.
void cursor_sets_report();

void pumprate_report() {
    const double now = mono_seconds();
    if (g_pumprate.t_window == 0.0) {
        g_pumprate.t_window = now;
        return;
    }
    const double dt = now - g_pumprate.t_window;
    if (dt < 2.0)
        return;

    const uint64_t cp = g_pumprate.pump.load(std::memory_order_relaxed);
    const uint64_t cq = g_pumprate.peep.load(std::memory_order_relaxed);
    const uint64_t cw = g_pumprate.wait.load(std::memory_order_relaxed);
    const uint64_t pump = cp - g_pumprate.p_pump;
    const uint64_t peep = cq - g_pumprate.p_peep;
    const uint64_t wait = cw - g_pumprate.p_wait;
    // A monotonic counter's delta: how many poses the layer published in this
    // window. `fresh` should equal it, and the gap is the loss being hunted.
    const uint64_t published =
        g_pumprate.have_updates
            ? g_pumprate.updates_last - g_pumprate.updates_mark
            : 0;
    const auto hz = [&](uint64_t c) { return dt > 0.0 ? (double)c / dt : 0.0; };

    char tickdrv[32];
    if (tick_hz())
        snprintf(tickdrv, sizeof(tickdrv), "ours@%dHz", tick_hz());
    else
        snprintf(tickdrv, sizeof(tickdrv), "drain");

    X4VR_LOG("pumprate: %.2f s | pump %.1f Hz peep %.1f Hz wait %.1f Hz | "
             "tick entered %.1f Hz, nopose %llu, fresh %.1f Hz, skipped %.1f Hz "
             "| layer published %.1f Hz | acted %.1f Hz (ungated %llu) | "
             "acted on %.1f%% of published | driver=%s tick=%s",
             dt, hz(pump), hz(peep), hz(wait), hz(g_pumprate.entered),
             (unsigned long long)g_pumprate.nopose, hz(g_pumprate.fresh),
             hz(g_pumprate.skipped), hz(published), hz(g_pumprate.acted),
             (unsigned long long)g_pumprate.ungated_ticks,
             published ? 100.0 * (double)g_pumprate.fresh / (double)published
                       : 0.0,
             g_pump_seen.load(std::memory_order_relaxed) ? "pump" : "peep",
             tickdrv);

    g_pumprate.p_pump = cp;
    g_pumprate.p_peep = cq;
    g_pumprate.p_wait = cw;
    // **The event census, on the same windows as everything else.**
    //
    // `pushed` and `got` are counted at their own sites, never inferred from one
    // another, because "delayed" and "DROPPED" want opposite fixes and a single
    // counter cannot tell them apart.
    {
        const uint64_t pm = g_evflow.pushed_motion.load(std::memory_order_relaxed);
        const uint64_t pk = g_evflow.pushed_key.load(std::memory_order_relaxed);
        const uint64_t gm = g_evflow.got_motion.load(std::memory_order_relaxed);
        const uint64_t gk = g_evflow.got_key.load(std::memory_order_relaxed);
        const uint64_t fo = g_evflow.got_foreign.load(std::memory_order_relaxed);
        const uint64_t ls = g_evflow.lat_sum_us.load(std::memory_order_relaxed);
        const uint64_t ln = g_evflow.lat_n.load(std::memory_order_relaxed);
        const uint64_t dr = g_evflow.drains.load(std::memory_order_relaxed);
        const uint64_t d_pm = pm - g_evflow.p_pm, d_gm = gm - g_evflow.p_gm;
        const uint64_t d_pk = pk - g_evflow.p_pk, d_gk = gk - g_evflow.p_gk;
        const uint64_t d_ln = ln - g_evflow.p_ln;
        const uint64_t d_ls = ls - g_evflow.p_ls;
        char bhist[96];
        int bo = 0;
        for (int i = 0; i < 9 && bo < (int)sizeof(bhist) - 12; i++) {
            const uint64_t v = g_evflow.burst[i].load(std::memory_order_relaxed);
            bo += snprintf(bhist + bo, sizeof(bhist) - bo, "%s%llu",
                           i ? "/" : "", (unsigned long long)(v - g_evflow.p_burst[i]));
            g_evflow.p_burst[i] = v;
        }
        X4VR_LOG("evflow: motion pushed %.1f Hz -> X4 took %.1f Hz (%+lld "
                 "in flight/lost) | key pushed %.1f Hz -> took %.1f Hz | "
                 "queue time mean %.2f ms max %.2f | player's own motion "
                 "%.1f Hz | ours per drain 0..8+: %s | drains %.1f Hz",
                 hz(d_pm), hz(d_gm), (long long)d_pm - (long long)d_gm,
                 hz(d_pk), hz(d_gk),
                 d_ln ? (double)d_ls / (double)d_ln / 1000.0 : 0.0,
                 g_evflow.lat_max_us.load(std::memory_order_relaxed) / 1000.0,
                 hz(fo - g_evflow.p_fo), bhist, hz(dr - g_evflow.p_dr));
        g_evflow.p_pm = pm; g_evflow.p_gm = gm;
        g_evflow.p_pk = pk; g_evflow.p_gk = gk;
        g_evflow.p_fo = fo; g_evflow.p_ls = ls;
        g_evflow.p_ln = ln; g_evflow.p_dr = dr;
    }
    pad_report();
    note_extent_report();
    cursor_sets_report();
    camscan_report();
    hoverview_report();

    g_pumprate.entered = g_pumprate.nopose = 0;
    g_pumprate.fresh = g_pumprate.skipped = 0;
    g_pumprate.acted = g_pumprate.ungated_ticks = 0;
    // The mark carries forward so the next window's baseline is this window's
    // last reading, with no gap between them. `have_updates` stays set unless
    // nothing was ever read.
    g_pumprate.updates_mark = g_pumprate.updates_last;
    g_pumprate.t_window = now;
}

// ---- the mouse half -------------------------------------------------------
//
// Take 117's log already answered which of X4's two mouse idioms free-look
// uses, without a run being spent on it: `relative mouse mode ON` at 962738 and
// off at 962804, a 66-second window that is exactly the clamp walk. In relative
// mode SDL delivers motion as events, and X4 does not import
// SDL_GetRelativeMouseState, so `xrel`/`yrel` on a motion event is the channel.
//
// Sdl3MouseEvent and the SDL_PeepEvents interposer already exist above, from
// the cursor shim -- the layout was established there and is reused rather than
// declared a second time. tests/sdl_event_layout.cpp asserts it against the
// real SDL3 headers, which this file cannot include because SDL2 (gamescope)
// and SDL3 (X4) share this process tree and the hand declarations here would
// collide with the real prototypes.

// Push one synthetic relative motion. Deliberately silent until a real event
// has been seen: sending before then would mean inventing the two fields this
// exists to copy.
// Hold the free-look key by pushing a key-down and never a key-up. X4 tracks
// held state from the event stream, so one down latches it; re-sending is
// harmless (it looks like key repeat) and is what makes the hold self-healing
// across the save-load desync that ruled out toggle="1".
//
// **The `key` field is why take 120 did nothing.** It was left zero, and X4
// works in *keycodes*, not scancodes -- it imports SDL_GetKeyFromScancode and
// SDL_GetKeyName, which is the tell. A zero keycode is a lookup for a binding
// that does not exist, so the event arrived, was well-formed, and meant
// nothing. Relative mouse mode never turned on in take 120 and, on a second
// reading, probably never did in 119 either: forty short on/off pairs look like
// menu activity, not the single 66-second window a real key press produced in
// take 117.
//
// Resolved through SDL rather than hardcoded. SDLK_F13 is 0x40000068 and the
// rule is plainly SDLK_SCANCODE_MASK | scancode, but X4 imports
// SDL_GetKeyFromScancode so it is there to be called, it respects the keyboard
// layout, and a constant here would be a fourth positional assumption to
// maintain.
uint32_t keycode_of(int scancode) {
    static auto from_sc =
        real<uint32_t (*)(uint32_t, uint16_t, bool)>("SDL_GetKeyFromScancode");
    if (from_sc) {
        const uint32_t k = from_sc((uint32_t)scancode, 0, false);
        if (k)
            return k;
    }
    return 0x40000000u | (uint32_t)scancode; // SDLK_SCANCODE_MASK
}

void send_key(int scancode, bool down) {
    if (!g_motion_seen.load(std::memory_order_relaxed))
        return;
    unsigned char ev[128] = {};
    auto *k = (Sdl3KeyEvent *)ev;
    k->type = down ? SDL_EV_KEY_DOWN : SDL_EV_KEY_UP;
    k->windowID = g_motion_window.load(std::memory_order_relaxed);
    k->scancode = (uint32_t)scancode;
    k->key = keycode_of(scancode);
    k->down = down;
    // Our own marker. `reserved` is untouched by SDL and by X4, and take 124
    // showed the alternatives do not work: scancode is shared with a real press
    // of the same key, and SDL fills in a zero timestamp on push, so both
    // discriminators the log has used so far were wrong.
    k->raw = (uint16_t)g_key_raw.load(std::memory_order_relaxed);
    push_our_event(ev);
    static bool said = false;
    if (!said) {
        said = true;
        // SDL_PushEvent returns 1 on success, 0 if an event filter dropped it,
        // negative on error. Ignoring it is how take 121 could not tell "X4
        // received our key and did nothing with it" from "SDL never queued it".
        X4VR_LOG("headlook: pushing scancode %d as keycode 0x%x, window %u, "
                 "raw %u",
                 scancode, k->key, k->windowID, (unsigned)k->raw);
    }
}

void send_key_down(int scancode) { send_key(scancode, true); }

// The ONE way a synthetic event leaves this file.
//
// **Take 139 through 142 were lost to a marker set in one path and not the
// other.** send_key stamped `reserved` with our magic; send_mouse_delta did
// not. The event drain then zeroes xrel/yrel on any motion event that is "not
// ours" -- so it ate our own commands, every one, for four runs. X4's camera
// never moved, and the stall watch dutifully reported exactly that while I
// rewrote the watch three times instead of believing it.
//
// The marker now lives here, where a caller cannot forget it, because the fix
// for "I set the field in one place and not the other" is to have one place.
static void push_our_event(void *ev) {
    // reserved is at a fixed offset in every SDL_Event; both of our event types
    // share the layout, which tests/sdl_event_layout.cpp asserts.
    auto *m = (Sdl3MouseEvent *)ev;
    m->reserved = kOurEventMagic;
    // **Left at zero ON PURPOSE, so SDL stamps it.**
    //
    // Take 287a wrote `mono_seconds() * 1e9` here -- CLOCK_MONOTONIC since BOOT,
    // about 6.4e14 ns -- while SDL fills this field from SDL_GetTicksNS, which
    // counts from SDL_Init and was around 1e10. Four orders of magnitude out, in
    // a field handed straight to X4, and that run ghosted for its whole length
    // in the drain-driven configuration that has never had a problem.
    //
    // tests/sdl_push_timestamp.cpp proved SDL PRESERVES a value we set. It never
    // asked whether X4 READS it, which is the question that mattered: verifying
    // the wrong property is how an instrument comes to perturb the thing it
    // measures. SDL already stamps every event with its push time, so the queue
    // time can be had for free, by reading rather than writing.
    m->timestamp = 0;
    if (m->type == SDL_EV_MOUSE_MOTION)
        g_evflow.pushed_motion.fetch_add(1, std::memory_order_relaxed);
    else
        g_evflow.pushed_key.fetch_add(1, std::memory_order_relaxed);
    static auto push = real<int (*)(void *)>("SDL_PushEvent");
    if (push)
        push(ev);
}

void send_mouse_delta(int dx, int dy) {
    if ((dx == 0 && dy == 0) || !g_motion_seen.load(std::memory_order_relaxed))
        return;
    unsigned char ev[128] = {};
    auto *m = (Sdl3MouseEvent *)ev;
    m->type = SDL_EV_MOUSE_MOTION;
    m->windowID = g_motion_window.load(std::memory_order_relaxed);
    m->which = g_motion_which.load(std::memory_order_relaxed);
    m->xrel = (float)dx;
    m->yrel = (float)dy;
    push_our_event(ev);
}

// X4 polls the keyboard every frame; this is where the free-look key is held.
// The real array is copied rather than written through: SDL owns that memory
// and other consumers in this process tree read it.
const bool *SDL_GetKeyboardState(int *numkeys) {
    static auto real_fn = real<const bool *(*)(int *)>("SDL_GetKeyboardState");
    int n = 0;
    const bool *src = real_fn(&n);
    if (numkeys)
        *numkeys = n;
    if (!src || !headlook_key_held())
        return src;
    const int sc = headlook_scancode();
    if (sc < 0 || sc >= n)
        return src;
    static bool *copy = nullptr;
    static int copy_n = 0;
    if (copy_n != n) {
        delete[] copy;
        copy = new bool[n];
        copy_n = n;
    }
    memcpy(copy, src, (size_t)n * sizeof(bool));
    copy[sc] = true;
    return copy;
}

// #33: the window id, without needing the player to move a mouse.
//
// Take 127 exposed this as a design flaw rather than an inconvenience: every
// synthetic event was gated on having sampled a REAL motion event to donate
// windowID, so a player on a HOTAS or a stick -- which is most of X4's cockpit
// audience -- would never arm head-look at all. X4 hands us its window pointer
// on every SDL_GetWindowSize call, and SDL will convert that to an id for free.
void note_window(void *win) {
    if (!win || !this_is_the_game() ||
        g_motion_seen.load(std::memory_order_relaxed))
        return;
    static auto get_id = real<uint32_t (*)(void *)>("SDL_GetWindowID");
    if (!get_id)
        return;
    const uint32_t id = get_id(win);
    if (!id)
        return;
    g_motion_window.store(id, std::memory_order_relaxed);
    g_motion_seen.store(1, std::memory_order_relaxed);
    X4VR_LOG("headlook: window id %u from SDL_GetWindowID — no mouse nudge "
             "needed to arm",
             id);
}

int SDL_GetWindowSize(void *win, int *w, int *h) {
    note_window(win);
    static auto real_fn = real<int (*)(void *, int *, int *)>("SDL_GetWindowSize");
    int r = real_fn(win, w, h);
    const bool cut = halve_window() && this_is_the_game() && w && *w > 1;
    if (cut)
        *w /= 2;
    if (w && h)
        note_window_size("SDL_GetWindowSize", *w, *h, cut);
    if (w && *w > 1 && this_is_the_game())
        g_x4_win_w.store(*w, std::memory_order_relaxed);
    return r;
}

int SDL_GetWindowSizeInPixels(void *win, int *w, int *h) {
    static auto real_fn =
        real<int (*)(void *, int *, int *)>("SDL_GetWindowSizeInPixels");
    int r = real_fn(win, w, h);
    const bool cut = halve_window() && this_is_the_game() && w && *w > 1;
    if (cut)
        *w /= 2;
    if (w && h)
        note_window_size("SDL_GetWindowSizeInPixels", *w, *h, cut);
    return r;
}

// SDL3 returns `bool` from these, not int. The low byte is what matters either
// way, but declaring it correctly costs nothing.
// X4 imports this; whether it CALLS it is the question take 118 taught me not
// to assume, so the first call is logged and the SDL_PeepEvents path stays as a
// fallback until one arrives. Pumping happens before the drain, so events
// pushed here are already queued when X4 starts reading and the drain
// terminates normally.
void SDL_PumpEvents(void) {
    static auto real_fn = real<void (*)()>("SDL_PumpEvents");
    if (this_is_the_game()) {
        if (!g_pump_seen.exchange(true, std::memory_order_relaxed))
            X4VR_LOG("sdl: SDL_PumpEvents is called — head-look drives from "
                     "here, not from inside the drain");
        g_pumprate.pump.fetch_add(1, std::memory_order_relaxed);
        // Started here rather than in the constructor: this is the first point
        // at which X4 is provably running its own event loop.
        tick_driver_start();
        camprobe_start();
        // X4's OWN thread, which is what makes this the control.
        camring_tick_on_x4_thread();
        // Before head-look's write: the census reads what X4 left in its
        // camera since the last tick, not our own write back (take 334's
        // "local follows the head" was exactly that).
        foot_tick();
        // MOVED, not duplicated — see the two-thread guard in headlook_tick.
        if (!tick_hz())
            headlook_tick();
        panprobe_tick();
        camscan_tick();
        camwatch_tick();
        camwatch_comove_tick();
        hoverview_tick();
        shadowfov_tick();
        fieldab_tick();
        csmfit_tick();
        fullscreen_menu_tick();
        foot_menu_tick();
        menus_tick();
        steer_tick();
        pumprate_report();
    }
    real_fn();
}

bool SDL_WaitEvent(void *event) {
    static auto real_fn = real<bool (*)(void *)>("SDL_WaitEvent");
    const bool r = real_fn(event);
    if (this_is_the_game())
        g_pumprate.wait.fetch_add(1, std::memory_order_relaxed);
    if (r && event && this_is_the_game()) {
        auto *e = (Sdl3MouseEvent *)event;
        if (e->type == SDL_EV_KEY_DOWN || e->type == SDL_EV_KEY_UP)
            note_key_event((const Sdl3KeyEvent *)e, "wait", -1);
        if (e->type >= SDL_EV_MOUSE_MOTION && e->type <= SDL_EV_MOUSE_UP) {
            note_mouse_event(e);
            // Position only. xrel/yrel are deltas and a fold of a delta is
            // meaningless -- it is also what X4 steers the camera by.
            e->x = fold_x(e->x);
        }
        // Counted at BOTH drains. This file has already shipped a fix to one
        // of two symmetric sites and had the defect come back through the
        // other; a census that watched only PeepEvents would read zero for a
        // session X4 happened to drain through WaitEvent.
        pad_note_event(e->type, (const Sdl3AxisEvent *)e);
        if (e->type == SDL_EV_MOUSE_DOWN)
            g_cursor_sets.buttons.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// SDL_Event is a fixed 128 bytes in SDL3 -- it is a union with an explicit
// padding[128] member -- so the array can be walked without the headers.
int SDL_PeepEvents(void *events, int numevents, int action, uint32_t minType,
                   uint32_t maxType) {
    static auto real_fn =
        real<int (*)(void *, int, int, uint32_t, uint32_t)>("SDL_PeepEvents");
    int n = real_fn(events, numevents, action, minType, maxType);
    // SDL_ADDEVENT=0, SDL_PEEKEVENT=1, SDL_GETEVENT=2. Fold only on GET.
    //
    // A peek leaves the event in the queue, so folding there and again when it
    // is finally fetched would apply the transform twice -- and because the
    // fold is its own inverse, that lands exactly back on the unfolded value.
    // The fix would silently do nothing, which is the worst way for it to fail.
    // #33 is driven from here rather than from SDL_GetKeyboardState: take 118
    // showed X4 imports that and never calls it, while this drain runs every
    // input poll. Outside the `n > 0` test on purpose -- head-look must keep
    // running through frames where the queue happens to be empty.
    // Only if X4 never calls SDL_PumpEvents. Pushing an event from inside the
    // drain is what made take 124 lag: X4 loops on SDL_PeepEvents until it
    // returns nothing, and an event pushed during that loop keeps handing it
    // more work, so the queue never empties and input arrives minutes late --
    // which is exactly what "it took some time to activate and kept applying
    // after I released" describes.
    if (this_is_the_game())
        g_pumprate.peep.fetch_add(1, std::memory_order_relaxed);
    if (this_is_the_game() && !g_pump_seen.load(std::memory_order_relaxed)) {
        tick_driver_start();
        camprobe_start();
        // X4's OWN thread, which is what makes this the control.
        camring_tick_on_x4_thread();
        if (!tick_hz())
            headlook_tick();
        panprobe_tick();
        pumprate_report();
    }
    const bool consuming = action == 2;
    // Counted ONLY on GET. A peek leaves the event in the queue and X4 would be
    // credited with receiving it twice -- the same trap the fold below is
    // guarded against, one field over.
    int ours_this_drain = 0;
    if (n > 0 && events && this_is_the_game()) {
        for (int i = 0; i < n; i++) {
            auto *e = (Sdl3MouseEvent *)((unsigned char *)events + 128 * i);
            if (consuming) {
                const bool mine = e->reserved == kOurEventMagic;
                const bool is_motion = e->type == SDL_EV_MOUSE_MOTION;
                if (mine) {
                    ours_this_drain++;
                    if (is_motion)
                        g_evflow.got_motion.fetch_add(
                            1, std::memory_order_relaxed);
                    else
                        g_evflow.got_key.fetch_add(1,
                                                   std::memory_order_relaxed);
                    // How long this command sat in SDL's queue. This is the
                    // quantity the servo is blind to: it commands again each
                    // tick with no knowledge that the last one has not landed.
                    // SDL's own clock at BOTH ends. Our CLOCK_MONOTONIC cannot be
                    // subtracted from a field SDL wrote from SDL_GetTicksNS --
                    // different epochs, which is the whole of take 287a's defect.
                    static auto ticks_ns = real<uint64_t (*)()>("SDL_GetTicksNS");
                    const uint64_t sent_ns = e->timestamp;
                    if (sent_ns && ticks_ns) {
                        const double lat =
                            ((double)ticks_ns() - (double)sent_ns) * 1e-9;
                        if (lat >= 0.0 && lat < 5.0) {
                            const uint64_t us = (uint64_t)(lat * 1e6);
                            g_evflow.lat_sum_us.fetch_add(
                                us, std::memory_order_relaxed);
                            g_evflow.lat_n.fetch_add(
                                1, std::memory_order_relaxed);
                            bump_max(g_evflow.lat_max_us, us);
                        }
                    }
                } else if (is_motion) {
                    // The player's own mouse, competing for the same channel.
                    g_evflow.got_foreign.fetch_add(1,
                                                   std::memory_order_relaxed);
                }
            }
            if (e->type == SDL_EV_KEY_DOWN || e->type == SDL_EV_KEY_UP)
                note_key_event((const Sdl3KeyEvent *)e, "peep", action);
            // Only on GET, like the ours/theirs counters above: a peek leaves
            // the event in the queue and would be counted again when X4
            // finally fetches it, inflating the axis traffic by however often
            // X4 peeks.
            if (consuming) {
                pad_note_event(e->type, (const Sdl3AxisEvent *)e);
                if (e->type == SDL_EV_MOUSE_DOWN)
                    g_cursor_sets.buttons.fetch_add(1,
                                                    std::memory_order_relaxed);
            }
            if (e->type >= SDL_EV_MOUSE_MOTION && e->type <= SDL_EV_MOUSE_UP) {
                note_mouse_event(e);
                if (consuming)
                    e->x = fold_x(e->x);
                // While head-look is steering, X4 is in mouselook and reads
                // every motion event as LOOK. So the player's own mouse fought
                // the head: Patola saw the view jitter toward the mouse and get
                // dragged back by the servo. The pointer must drive the cursor
                // and nothing else.
                //
                // Only the RELATIVE fields are dropped. x/y still carry the
                // pointer position the cursor shim composites, so targeting and
                // menus are untouched -- and our own synthetic motion is spared
                // by its marker, which is what that marker was for.
                //
                // **Take 334: on foot this zeroed every motion** -- the write
                // armed there (X4's camera stays in mode 0), so `steering`
                // was true and the mouse could not turn him. Fixed where it
                // started: off the pilot seat the write stands down and
                // `steering` is false (headlook_tick, `seated`). In the
                // cockpit this stays exactly as it was.
                //
                // On foot under X4VR_HEADLOOK_FOOT the mouse turns the body,
                // so only its PITCH is zeroed: Patola, 2026-10-27, "head
                // alone for pitch on foot".
                bool dropped = false, pitch_zeroed = false;
                if (consuming && e->type == SDL_EV_MOUSE_MOTION &&
                    e->reserved != kOurEventMagic &&
                    g_headlook_steering.load(std::memory_order_relaxed)) {
                    if (g_on_foot.load(std::memory_order_relaxed)) {
                        pitch_zeroed = e->yrel != 0.0f;
                        e->yrel = 0.0f;
                    } else {
                        e->xrel = 0.0f;
                        e->yrel = 0.0f;
                        dropped = true;
                    }
                }
                // X4VR_FOOTLOG: what X4 RECEIVES, after the line above. Take
                // 334 summed it before, so the census credited X4 with motion
                // it never got and called the result "the mouse turns
                // nothing".
                if (consuming && e->type == SDL_EV_MOUSE_MOTION &&
                    e->reserved != kOurEventMagic && footlog_on()) {
                    if (pitch_zeroed)
                        g_footmouse.pitch_zeroed.fetch_add(1, std::memory_order_relaxed);
                    if (dropped) {
                        g_footmouse.dropped.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        g_footmouse.dx_milli.fetch_add(
                            (int64_t)(e->xrel * 1000.f), std::memory_order_relaxed);
                        g_footmouse.dy_milli.fetch_add(
                            (int64_t)(e->yrel * 1000.f), std::memory_order_relaxed);
                        g_footmouse.n.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }
    // A drain that handed X4 more than one of our commands at once is the whole
    // question: the servo emitted them on separate ticks believing each would
    // act alone.
    if (consuming && n > 0 && this_is_the_game()) {
        g_evflow.drains.fetch_add(1, std::memory_order_relaxed);
        const int b = ours_this_drain > 8 ? 8 : ours_this_drain;
        g_evflow.burst[b].fetch_add(1, std::memory_order_relaxed);
    }
    return n;
}

// The other half of the input channel, and the more interesting one: X4 *polls*
// the pointer rather than only reading motion events. Whatever space this
// returns is the space X4 hit-tests in, which is exactly what the shim has to
// rewrite -- so this is where task #19 will act, not on the event stream.
// X4 recentres the pointer for mouse-look. Routed through the same fold, which
// works because the fold is an involution: X4 asks for a logical position, the
// real pointer goes to the place that folds back to it, and the value X4 reads
// next is the one it asked for. Using a separately-derived inverse here would
// be a second expression to keep in step with fold_x, and they would drift.
// X4 builds its cursors here. Captured rather than merely counted, because the
// shim has to draw *this* image -- a cursor of our own would not match, and X4
// varies it by context (Patola: a small blue hollow cross on the map, an arrow
// over a station).
//
// The surface is repacked tightly to w*4 so the consumer never has to carry
// SDL's pitch, and the pixel format is passed through unconverted: one run will
// say what it actually is, which beats a guessed conversion that silently
// mangles the colours.
// X4's own arrow, by build order (take 328's map sweep); see below.
constexpr size_t kStandInCursor = 5;

void *SDL_CreateColorCursor(void *surface, int hot_x, int hot_y) {
    static auto real_fn =
        real<void *(*)(void *, int, int)>("SDL_CreateColorCursor");
    void *cur = real_fn(surface, hot_x, hot_y);
    if (!cur || !surface || !this_is_the_game())
        return cur;
    const auto *sf = (const Sdl3Surface *)surface;
    // Every one of these has to hold for the positional read to be believable.
    const bool sane = sf->w > 0 && sf->h > 0 &&
                      sf->w <= (int32_t)x4vr::Shared::kCursorMax &&
                      sf->h <= (int32_t)x4vr::Shared::kCursorMax &&
                      sf->pitch >= sf->w * 4 && sf->pitch < 1 << 20 && sf->pixels;
    if (!sane) {
        static bool said = false;
        if (!said) {
            said = true;
            X4VR_LOG("sdl: cursor surface refused — w=%d h=%d pitch=%d pixels=%p "
                     "(max %u); either it is larger than the channel or the "
                     "SDL_Surface layout has moved",
                     sf->w, sf->h, sf->pitch, sf->pixels,
                     x4vr::Shared::kCursorMax);
        }
        return cur;
    }
    CapturedCursor c;
    c.handle = cur;
    c.w = (uint32_t)sf->w;
    c.h = (uint32_t)sf->h;
    c.format = sf->format;
    c.pitch = (uint32_t)sf->pitch;
    c.hot_x = hot_x;
    c.hot_y = hot_y;
    c.px.resize((size_t)c.w * c.h * 4u);
    for (uint32_t y = 0; y < c.h; y++)
        for (uint32_t b = 0; b < c.w * 4u; b++)
            c.px[(size_t)y * c.w * 4u + b] =
                ((const uint8_t *)sf->pixels)[(size_t)y * c.pitch + b];
    size_t idx;
    {
        std::lock_guard<std::mutex> lock(g_cursor_mu);
        g_cursors.push_back(c);
        idx = g_cursors.size() - 1;
        // Until X4 sets a cursor of its own it shows SDL's default system
        // arrow, which is never captured, so the layer had nothing to draw:
        // takes 326 and 327 had no cursor on the start screen while hover
        // worked. One of X4's own stands in. Take 328 chose #0 and Patola saw
        // "a blue bracket square with two horizontal arrows"; the same take's
        // map sweep showed X4's own arrow is #5 (hotspot (11,6)): X4 sets it
        // where the map's circled target (#12) ends. Its hotspot is checked,
        // so a build that orders its cursors differently stands nothing in
        // rather than the wrong image.
        if (idx == kStandInCursor && hot_x == 11 && hot_y == 6 &&
            !g_cursor_sets.sets.load(std::memory_order_relaxed)) {
            publish_cursor_image(c);
            X4VR_LOG("sdl: X4 has set no cursor yet — showing its arrow "
                     "(#%zu, %ux%u hot=(%d,%d)) until it picks one",
                     idx, c.w, c.h, hot_x, hot_y);
        } else if (idx == kStandInCursor) {
            X4VR_LOG("sdl: cursor #%zu has hotspot (%d,%d), not X4's arrow's "
                     "(11,6) — no stand-in; the start screen will have no "
                     "cursor",
                     idx, hot_x, hot_y);
        }
    }
    X4VR_LOG("sdl: captured cursor #%zu %p — %ux%u fmt=0x%08x pitch=%u "
             "hot=(%d,%d)",
             idx, cur, c.w, c.h, c.format, c.pitch, hot_x, hot_y);
    return cur;
}

// Which of them is current. This is also the *shape* signal: X4 swapping cursor
// is how the reticle becomes an arrow, so a shim that draws a fixed image would
// be wrong exactly when the game is telling the player something.

void cursor_sets_report() {
    const uint64_t s = g_cursor_sets.sets.load(std::memory_order_relaxed);
    // **sets == 0 is the most interesting outcome of the three, so it must
    // PRINT.** An early return here would make "X4 never set a cursor" look
    // exactly like "this build has no counter" -- which is the defect the
    // cursor census was written to end, reintroduced one file over. The gate
    // is instead "has X4 built any cursor at all": if it has not, there is
    // nothing to set and silence is honest.
    size_t built = 0;
    {
        std::lock_guard<std::mutex> lock(g_cursor_mu);
        built = g_cursors.size();
    }
    if (!built)
        return;
    static uint64_t p_s = UINT64_MAX;
    if (s == p_s)
        return; // only when it moves, so a static session stays quiet
    p_s = s;
    if (!s) {
        X4VR_LOG("cursor sets: 0 set(s) — X4 built %zu cursor(s) and has not "
                 "SET one. Its cursor shape is its only hover indication, so "
                 "this says X4 has not yet seen the pointer over anything "
                 "clickable.",
                 built);
        return;
    }
    X4VR_LOG("cursor sets: %llu set(s) — handle found %llu, NOT found %llu, "
             "null %llu | %llu were a CHANGE (%llu while head-look was "
             "STEERING, %llu while it was NOT; %llu in the COCKPIT, %llu "
             "elsewhere%s) | %llu mouse button press(es)",
             (unsigned long long)s,
             (unsigned long long)g_cursor_sets.found.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.missed.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.null.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.changed.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.changed_steering.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.changed_free.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.changed_cockpit.load(std::memory_order_relaxed),
             (unsigned long long)g_cursor_sets.changed_elsewhere.load(std::memory_order_relaxed),
             g_view_known.load(std::memory_order_relaxed)
                 ? ""
                 : " — VIEW NEVER STAMPED, the split is meaningless",
             (unsigned long long)g_cursor_sets.buttons.load(std::memory_order_relaxed));
}

bool SDL_SetCursor(void *cursor) {
    static auto real_fn = real<bool (*)(void *)>("SDL_SetCursor");
    if (this_is_the_game()) {
        g_cursor_sets.sets.fetch_add(1, std::memory_order_relaxed);
        if (!cursor)
            g_cursor_sets.null.fetch_add(1, std::memory_order_relaxed);
        // A CHANGE is the interesting event: X4 re-asserts the same cursor
        // every frame in some states, and counting those as shape changes
        // would report a hover on every frame of a run that never had one.
        if (g_cursor_sets.last.exchange(cursor, std::memory_order_relaxed) !=
            cursor) {
            g_cursor_sets.changed.fetch_add(1, std::memory_order_relaxed);
            (g_headlook_steering.load(std::memory_order_relaxed)
                 ? g_cursor_sets.changed_steering
                 : g_cursor_sets.changed_free)
                .fetch_add(1, std::memory_order_relaxed);
            (g_in_cockpit.load(std::memory_order_relaxed)
                 ? g_cursor_sets.changed_cockpit
                 : g_cursor_sets.changed_elsewhere)
                .fetch_add(1, std::memory_order_relaxed);
            // The class stamped by the last pump on this same thread, not a
            // fresh read: an instrument must bin by the state the frame had.
            if (hoverview_enabled())
                g_hover_by_view
                    .n[g_view_class.load(std::memory_order_relaxed) + 1]
                    .fetch_add(1, std::memory_order_relaxed);
        }
        bool hit = false;
        long which = -1;
        {
            std::lock_guard<std::mutex> lock(g_cursor_mu);
            for (size_t i = 0; i < g_cursors.size(); i++)
                if (g_cursors[i].handle == cursor) {
                    publish_cursor_image(g_cursors[i]);
                    hit = true;
                    which = (long)i;
                    break;
                }
        }
        // Which cursor, by build order, for the first changes: whether the
        // stand-in above (#0) is the arrow X4 uses in its menus is a fact
        // about X4 this line establishes.
        //
        // With the pointer, while a full-screen menu is up: X4 switches shape
        // exactly where its content starts and ends (the map's circled target
        // becomes an arrow at the map's edge), so a sweep across the four
        // edges measures X4's own layout in window pixels -- the question
        // take 327's "the cursor does not fit the map" left open.
        static long last_which = -2;
        static int said_sets = 0, said_menu = 0;
        if (which != last_which) {
            const x4vr::Shared *sh = x4vr_shared_state();
            const bool menu =
                sh->fullscreen_menu.load(std::memory_order_relaxed) != 0;
            if (said_sets < 40 || (menu && said_menu < 400)) {
                said_sets++;
                if (menu)
                    said_menu++;
                X4VR_LOG("sdl: X4 set cursor #%ld%s at pointer %.0f,%.0f%s",
                         which,
                         which < 0 ? " (not one it built through "
                                     "SDL_CreateColorCursor)"
                                   : "",
                         sh->cursor_x.load(std::memory_order_relaxed),
                         sh->cursor_y.load(std::memory_order_relaxed),
                         menu ? " [full-screen menu]" : "");
            }
            last_which = which;
        }
        (hit ? g_cursor_sets.found : g_cursor_sets.missed)
            .fetch_add(1, std::memory_order_relaxed);
    }
    return real_fn(cursor);
}

void SDL_WarpMouseInWindow(void *win, float x, float y) {
    static auto real_fn =
        real<void (*)(void *, float, float)>("SDL_WarpMouseInWindow");
    // X4's own warps, the first 20: its single call site (0xcefcd9) nudges
    // the pointer by a delta. Ours (steer_recentre) logs itself.
    static std::atomic<unsigned> n{0};
    if (this_is_the_game() && !g_steer_warping.load(std::memory_order_relaxed) &&
        n.fetch_add(1, std::memory_order_relaxed) < 20)
        X4VR_LOG("sdl: X4 warped the pointer to %.0f,%.0f", x, y);
    real_fn(win, this_is_the_game() ? fold_x(x) : x, y);
}

// ------------------------------------------------ the controller census (#71)
//
// X4 has TWO controller paths and we cannot tell from the binaries which one is
// live: it requests `SteamInput006` and `SteamController008` through
// SteamInternal_FindOrCreateUserInterface, AND imports the whole SDL3
// joystick/gamepad API, with SDL_GetGamepadSteamHandle tying the two together.
// Patola's X52 Pro and his sc-controller virtual pad both failed to fly the
// ship, and at least two explanations survive that: his inputmap puts steering
// on INPUT_JOYAXIS_RX/RY with strafe on X/Y, so a stick whose main axes are X/Y
// would strafe rather than turn -- or Steam captured the device and SDL never
// saw it.
//
// Those want opposite fixes, so this counts rather than guesses: what X4
// enumerates, what it opens, and what it names them. A device that never
// appears here was never offered to X4 at all, which is the one thing no amount
// of rebinding could fix.
//
// Every symbol below was checked with `nm -D --undefined-only` before it was
// hooked. This project has four times instrumented a function X4 never calls --
// SDL_PollEvent for 47 takes, SDL_GetKeyboardState, and, as of today,
// SDL_GetMouseState, which is publish_cursor's only caller. An import is not a
// call, and the census exists to say which of the two this is.
struct PadCensus {
    std::atomic<uint64_t> enumerated{0}, opened_joy{0}, opened_pad{0};
    std::atomic<uint64_t> axis_reads{0}, is_gamepad{0}, mappings{0};
    std::atomic<int> last_count{-1};
    std::atomic<bool> said{false};
    // The channel take 307 proved the values actually arrive on. Counted in
    // the drain, per event family, because "X4 polled nothing" and "X4 was
    // handed everything as events" are the two readings that got confused.
    std::atomic<uint64_t> ev_joy_axis{0}, ev_pad_axis{0};
    std::atomic<uint64_t> ev_buttons{0}, ev_added{0};
    // Per-axis travel, which is the number that names a stick. 16 axes is
    // more than an X52 Pro reports and the index is a uint8 anyway, so a
    // device with more simply lands in the last bin rather than corrupting
    // memory -- stated because a positional read with no bound is how this
    // file's own SDL_Surface handling would have failed.
    //
    // **Kept per FAMILY, not pooled.** SDL numbers gamepad axes LEFTX=0,
    // LEFTY=1, RIGHTX=2, RIGHTY=3 (asserted in tests/sdl_event_layout.cpp),
    // which is NOT raw joystick numbering -- so index 3 means different
    // hardware depending on which event carried it. The X52 is likely to
    // arrive as a joystick and the sc-controller pad as a gamepad, and a
    // range pooled across the two would name neither, which is the mistake
    // this census already made once by watching the wrong API.
    static constexpr int kAxes = 16;
    static constexpr int kFamilies = 2; // 0 = joystick, 1 = gamepad
    std::atomic<int32_t> axis_lo[kFamilies][kAxes];
    std::atomic<int32_t> axis_hi[kFamilies][kAxes];
    std::atomic<uint64_t> axis_n[kFamilies][kAxes];
    // Which device instance ids were seen per family, so two sticks in one
    // take cannot be read as one stick with twice the axes.
    std::atomic<uint32_t> seen_ids[kFamilies][4];
    PadCensus() {
        for (int f = 0; f < kFamilies; f++) {
            for (int i = 0; i < kAxes; i++) {
                axis_lo[f][i].store(INT32_MAX, std::memory_order_relaxed);
                axis_hi[f][i].store(INT32_MIN, std::memory_order_relaxed);
                axis_n[f][i].store(0, std::memory_order_relaxed);
            }
            for (int i = 0; i < 4; i++)
                seen_ids[f][i].store(UINT32_MAX, std::memory_order_relaxed);
        }
    }
} g_pads;

void pad_note_axis(const Sdl3AxisEvent *a, bool gamepad);

// Dispatch for the drains. Deliberately a switch on the exact type rather
// than a range test: SDL packs unrelated families adjacently (0x605 is
// JOYSTICK_ADDED, 0x606 REMOVED) and a range would quietly fold an axis
// count into a hotplug count the first time SDL renumbers anything.
void pad_note_event(uint32_t type, const Sdl3AxisEvent *a) {
    switch (type) {
    case SDL_EV_JOY_AXIS:
        pad_note_axis(a, false);
        break;
    case SDL_EV_PAD_AXIS:
        pad_note_axis(a, true);
        break;
    case SDL_EV_JOY_BUTTON_DOWN:
    case SDL_EV_PAD_BUTTON_DOWN:
        g_pads.ev_buttons.fetch_add(1, std::memory_order_relaxed);
        break;
    case SDL_EV_JOY_ADDED:
    case SDL_EV_PAD_ADDED:
        g_pads.ev_added.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

// Called from the drain for every joystick/gamepad axis event X4 is handed.
// `which` is carried so two devices cannot be summed into one stick -- the
// X52 and the sc-controller pad may both be present, and a range pooled
// across them would name neither.
void pad_note_axis(const Sdl3AxisEvent *a, bool gamepad) {
    if (!a)
        return;
    (gamepad ? g_pads.ev_pad_axis : g_pads.ev_joy_axis)
        .fetch_add(1, std::memory_order_relaxed);
    const int f = gamepad ? 1 : 0;
    const int i = a->axis < PadCensus::kAxes ? a->axis : PadCensus::kAxes - 1;
    const int32_t v = a->value;
    // Monotone widening, so a relaxed CAS loop is not needed: a lost race can
    // only fail to widen a bound that a later event will widen again.
    if (v < g_pads.axis_lo[f][i].load(std::memory_order_relaxed))
        g_pads.axis_lo[f][i].store(v, std::memory_order_relaxed);
    if (v > g_pads.axis_hi[f][i].load(std::memory_order_relaxed))
        g_pads.axis_hi[f][i].store(v, std::memory_order_relaxed);
    g_pads.axis_n[f][i].fetch_add(1, std::memory_order_relaxed);
    for (auto &slot : g_pads.seen_ids[f]) {
        const uint32_t cur = slot.load(std::memory_order_relaxed);
        if (cur == a->which)
            break;
        if (cur == UINT32_MAX) {
            slot.store(a->which, std::memory_order_relaxed);
            break;
        }
    }
}

// Names are logged once each, keyed on the string, because X4 re-queries them
// per frame in some menus and a per-call line would bury the log.
void pad_note_name(const char *what, const char *name, uint32_t id) {
    static std::mutex mu;
    static std::vector<std::string> &seen = x4vr::immortal<std::vector<std::string>>();
    if (!name || !*name)
        return;
    std::lock_guard<std::mutex> lock(mu);
    const std::string key = std::string(what) + "\x1f" + name;
    for (const auto &s : seen)
        if (s == key)
            return;
    seen.push_back(key);
    X4VR_LOG("pad: X4 %s id=%u name=\"%s\"", what, id, name);
}

// SDL_JoystickID is Uint32, so the return is uint32_t* -- passed straight
// through either way, but spelled correctly so the next reader is not misled
// about what X4 is handed.
uint32_t *SDL_GetJoysticks(int *count) {
    static auto real_fn = real<uint32_t *(*)(int *)>("SDL_GetJoysticks");
    uint32_t *r = real_fn(count);
    if (this_is_the_game()) {
        g_pads.enumerated.fetch_add(1, std::memory_order_relaxed);
        const int n = count ? *count : -1;
        // Logged on a CHANGE, not per call: X4 polls this. A hotplug is the
        // event worth a line, and the first call is a change from -1.
        if (g_pads.last_count.exchange(n, std::memory_order_relaxed) != n)
            X4VR_LOG("pad: SDL_GetJoysticks -> %d device(s)", n);
    }
    return r;
}

void *SDL_OpenJoystick(uint32_t id) {
    static auto real_fn = real<void *(*)(uint32_t)>("SDL_OpenJoystick");
    void *r = real_fn(id);
    if (this_is_the_game()) {
        g_pads.opened_joy.fetch_add(1, std::memory_order_relaxed);
        static auto name_fn = real<const char *(*)(void *)>("SDL_GetJoystickName");
        X4VR_LOG("pad: SDL_OpenJoystick(%u) -> %s%s", id, r ? "ok" : "FAILED",
                 r ? "" : " (X4 saw the device and could not open it)");
        if (r && name_fn)
            pad_note_name("opened joystick", name_fn(r), id);
    }
    return r;
}

void *SDL_OpenGamepad(uint32_t id) {
    static auto real_fn = real<void *(*)(uint32_t)>("SDL_OpenGamepad");
    void *r = real_fn(id);
    if (this_is_the_game()) {
        g_pads.opened_pad.fetch_add(1, std::memory_order_relaxed);
        static auto name_fn = real<const char *(*)(void *)>("SDL_GetGamepadName");
        X4VR_LOG("pad: SDL_OpenGamepad(%u) -> %s", id, r ? "ok" : "FAILED");
        if (r && name_fn)
            pad_note_name("opened gamepad", name_fn(r), id);
    }
    return r;
}

bool SDL_IsGamepad(uint32_t id) {
    static auto real_fn = real<bool (*)(uint32_t)>("SDL_IsGamepad");
    const bool r = real_fn(id);
    if (this_is_the_game())
        g_pads.is_gamepad.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// The one that says whether the axes are being READ, as opposed to the device
// merely being opened. An X52 that enumerates, opens, and is never polled is a
// different defect from one that is polled and whose values go nowhere.
int16_t SDL_GetJoystickAxis(void *joy, int axis) {
    static auto real_fn = real<int16_t (*)(void *, int)>("SDL_GetJoystickAxis");
    const int16_t v = real_fn(joy, axis);
    if (this_is_the_game())
        g_pads.axis_reads.fetch_add(1, std::memory_order_relaxed);
    return v;
}

int SDL_AddGamepadMappingsFromFile(const char *path) {
    static auto real_fn = real<int (*)(const char *)>("SDL_AddGamepadMappingsFromFile");
    const int r = real_fn(path);
    if (this_is_the_game()) {
        g_pads.mappings.fetch_add(1, std::memory_order_relaxed);
        X4VR_LOG("pad: SDL_AddGamepadMappingsFromFile(\"%s\") -> %d mapping(s)",
                 path ? path : "(null)", r);
    }
    return r;
}

// Called from the same cadence as the other injector reports.
void pad_report() {
    const uint64_t en = g_pads.enumerated.load(std::memory_order_relaxed);
    const uint64_t oj = g_pads.opened_joy.load(std::memory_order_relaxed);
    const uint64_t op = g_pads.opened_pad.load(std::memory_order_relaxed);
    const uint64_t ax = g_pads.axis_reads.load(std::memory_order_relaxed);
    const uint64_t eja = g_pads.ev_joy_axis.load(std::memory_order_relaxed);
    const uint64_t epa = g_pads.ev_pad_axis.load(std::memory_order_relaxed);
    const uint64_t evb = g_pads.ev_buttons.load(std::memory_order_relaxed);
    const uint64_t eva = g_pads.ev_added.load(std::memory_order_relaxed);
    // Silent until X4 touches the API at all, so a keyboard-and-mouse run does
    // not carry a line about hardware nobody plugged in.
    if (!en && !oj && !op && !ax && !eja && !epa && !evb && !eva)
        return;
    // Re-reported while axis traffic is still growing, so a stick moved late
    // in a take is not missed by a one-shot. `said` alone would have printed
    // once, before the player touched anything -- which is exactly how take
    // 307's census came to describe a device nobody had moved yet.
    static uint64_t p_axis_ev = 0;
    const uint64_t axis_ev = eja + epa;
    const bool grew = axis_ev != p_axis_ev;
    p_axis_ev = axis_ev;
    if (g_pads.said.exchange(true, std::memory_order_relaxed) && !grew)
        return;
    X4VR_LOG("pad census: enumerate %llu (last saw %d device(s)) | opened "
             "joystick %llu gamepad %llu | POLLED axis reads %llu | EVENTS "
             "joy-axis %llu pad-axis %llu buttons %llu added %llu | mapping "
             "files %llu",
             (unsigned long long)en, g_pads.last_count.load(),
             (unsigned long long)oj, (unsigned long long)op,
             (unsigned long long)ax, (unsigned long long)eja,
             (unsigned long long)epa, (unsigned long long)evb,
             (unsigned long long)eva,
             (unsigned long long)g_pads.mappings.load(std::memory_order_relaxed));
    // The per-axis travel, which is what names a stick. Reported per FAMILY
    // and WITHOUT inventing a mapping onto X4's INPUT_JOYAXIS_* names: the
    // gamepad family is SDL's own LEFTX/LEFTY/RIGHTX/RIGHTY ordering, the
    // joystick family is whatever the device reports, and which of those X4's
    // "RX" means is a question for the inputmap, not for this line. Stating a
    // raw index and its travel is a measurement; labelling it would be the
    // guess that made the first version of this census wrong.
    static const char *const kPadName[6] = {"LEFTX",  "LEFTY",  "RIGHTX",
                                            "RIGHTY", "LTRIG",  "RTRIG"};
    for (int f = 0; f < PadCensus::kFamilies; f++) {
        char buf[320];
        int n = 0;
        for (int i = 0; i < PadCensus::kAxes && n < (int)sizeof(buf) - 40; i++) {
            const uint64_t cnt =
                g_pads.axis_n[f][i].load(std::memory_order_relaxed);
            if (!cnt)
                continue;
            const int32_t lo = g_pads.axis_lo[f][i].load(std::memory_order_relaxed);
            const int32_t hi = g_pads.axis_hi[f][i].load(std::memory_order_relaxed);
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%d%s%s:%d..%d",
                          n ? " " : "", i,
                          (f == 1 && i < 6) ? "=" : "",
                          (f == 1 && i < 6) ? kPadName[i] : "", lo, hi);
        }
        if (!n)
            continue;
        char ids[64];
        int m = 0;
        for (auto &slot : g_pads.seen_ids[f]) {
            const uint32_t v = slot.load(std::memory_order_relaxed);
            if (v == UINT32_MAX)
                break;
            m += snprintf(ids + m, sizeof(ids) - (size_t)m, "%s%u", m ? "," : "",
                          v);
        }
        X4VR_LOG("pad axes (%s, device id %s): %s", f ? "gamepad" : "joystick",
                 m ? ids : "?", buf);
    }
}

// Tracked so fold_x can stand down during mouse-look. Passed through untouched:
// the shim has no business changing whether X4 grabs the pointer, only where it
// believes the pointer is.
bool SDL_SetWindowRelativeMouseMode(void *win, bool enabled) {
    static auto real_fn =
        real<bool (*)(void *, bool)>("SDL_SetWindowRelativeMouseMode");
    if (this_is_the_game()) {
        g_relative_mouse.store(enabled, std::memory_order_relaxed);
        X4VR_LOG("sdl: relative mouse mode %s (fold %s)",
                 enabled ? "ON" : "off",
                 enabled ? "stands down" : "applies");
    }
    return real_fn(win, enabled);
}

uint32_t SDL_GetMouseState(float *x, float *y) {
    static auto real_fn = real<uint32_t (*)(float *, float *)>("SDL_GetMouseState");
    const uint32_t buttons = real_fn(x, y);
    if (this_is_the_game()) {
        note_extent("GetMouseState", x ? *x : 0.f, y ? *y : 0.f);
        if (x)
            *x = fold_x(*x);
        // After the fold, so what is published is what X4 is about to read.
        publish_cursor(x ? *x : 0.f, y ? *y : 0.f,
                       !g_relative_mouse.load(std::memory_order_relaxed));
        hide_sdl_cursor();
    }
    return buttons;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static auto real_connect =
        real<int (*)(int, const struct sockaddr *, socklen_t)>("connect");
    if (addr && addr->sa_family == AF_UNIX && len > sizeof(sa_family_t)) {
        const char *path = ((const struct sockaddr_un *)addr)->sun_path;
        // Abstract sockets start with NUL; print the name after it so the X11
        // ones (@/tmp/.X11-unix/X2) are not silently dropped as empty.
        const bool abstract = path[0] == '\0';
        const char *shown = abstract ? path + 1 : path;
        if (strstr(shown, "wayland") || strstr(shown, "gamescope") ||
            strstr(shown, "X11-unix"))
            X4VR_LOG("net: connect(%s%s) by pid %d", abstract ? "@" : "", shown,
                     getpid());
    }
    return real_connect(fd, addr, len);
}

// No null guards on the arguments: glibc declares all three nonnull, so a
// check here is dead code the compiler is entitled to delete, and -Wnonnull
// -compare says so. A caller passing null is already in undefined behaviour
// before it reaches us.
int setenv(const char *name, const char *value, int overwrite) {
    static auto real_setenv =
        real<int (*)(const char *, const char *, int)>("setenv");
    if (is_watched_env(name, strlen(name)))
        X4VR_LOG("env: setenv(%s=%s, overwrite=%d) by pid %d", name, value,
                 overwrite, getpid());
    return real_setenv(name, value, overwrite);
}

int unsetenv(const char *name) {
    static auto real_unsetenv = real<int (*)(const char *)>("unsetenv");
    if (is_watched_env(name, strlen(name)))
        X4VR_LOG("env: unsetenv(%s) by pid %d", name, getpid());
    return real_unsetenv(name);
}

// putenv takes "NAME=VALUE" in one string, so the name has to be split off
// before it can be matched.
int putenv(char *s) {
    static auto real_putenv = real<int (*)(char *)>("putenv");
    const char *eq = strchr(s, '=');
    if (eq && is_watched_env(s, (size_t)(eq - s)))
        X4VR_LOG("env: putenv(%s) by pid %d", s, getpid());
    return real_putenv(s);
}

int open(const char *path, int flags, ...) {
    static auto real_open = real<int (*)(const char *, int, mode_t)>("open");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    file_probe("open", path);
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("open(%s, 0x%x)", path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_open(to.c_str(), flags, mode);
    }
    if (extensions_enabled() && path[0] == '/' &&
        (flags & O_ACCMODE) == O_RDONLY && x4vr::is_x4_profile_content(path)) {
        const int fd = content_memfd(path, "open");
        if (fd >= 0)
            return fd;
    }
    {
        const int fd = real_open(path, flags, mode);
        catpatch_track(fd, path);
        return fd;
    }
}

int open64(const char *path, int flags, ...) {
    static auto real_open64 =
        real<int (*)(const char *, int, mode_t)>("open64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    file_probe("open64", path);
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("open64(%s, 0x%x)", path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_open64(to.c_str(), flags, mode);
    }
    if (extensions_enabled() && path[0] == '/' &&
        (flags & O_ACCMODE) == O_RDONLY && x4vr::is_x4_profile_content(path)) {
        const int fd = content_memfd(path, "open64");
        if (fd >= 0)
            return fd;
    }
    {
        const int fd = real_open64(path, flags, mode);
        catpatch_track(fd, path);
        return fd;
    }
}

int openat(int dirfd, const char *path, int flags, ...) {
    static auto real_openat =
        real<int (*)(int, const char *, int, mode_t)>("openat");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    file_probe("openat", path);
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("openat(%d, %s, 0x%x)", dirfd, path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_openat(dirfd, to.c_str(), flags, mode);
    }
    if (extensions_enabled() && path[0] == '/' &&
        (flags & O_ACCMODE) == O_RDONLY && x4vr::is_x4_profile_content(path)) {
        const int fd = content_memfd(path, "openat");
        if (fd >= 0)
            return fd;
    }
    {
        const int fd = real_openat(dirfd, path, flags, mode);
        catpatch_track(fd, path);
        return fd;
    }
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static auto real_openat64 =
        real<int (*)(int, const char *, int, mode_t)>("openat64");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("openat64(%d, %s, 0x%x)", dirfd, path, flags);
    if ((flags & O_ACCMODE) != O_RDONLY) {
        const std::string to = redirect_write(path);
        if (!to.empty())
            return real_openat64(dirfd, to.c_str(), flags, mode);
    }
    if (extensions_enabled() && path[0] == '/' &&
        (flags & O_ACCMODE) == O_RDONLY && x4vr::is_x4_profile_content(path)) {
        const int fd = content_memfd(path, "openat64");
        if (fd >= 0)
            return fd;
    }
    return real_openat64(dirfd, path, flags, mode);
}

// Read-only opens of X4's own config.xml get the profile, patched in memory.
// Anything else — the game saving its settings — is redirected to the
// profile file, so it lands in ours instead of the player's. If either path
// fails we fall through to the real file untouched, which for a write means
// the pre-profile behaviour rather than a lost save.
static bool is_read_only(const char *mode) {
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

FILE *fopen(const char *path, const char *mode) {
    static auto real_fopen = real<FILE *(*)(const char *, const char *)>("fopen");
    file_probe("fopen", path);
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("fopen(%s, %s)", path, mode);
    if (!overrides_disabled() && x4vr::is_x4_config(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_config(path, steer_hide_x4_line()))
                return f;
        } else {
            Profile prof = load_profile(path);
            if (!prof.xml.empty())
                return real_fopen(prof.path.c_str(), mode);
        }
    }
    if (headlook_enabled() && x4vr::is_x4_inputmap(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_inputmap(path))
                return f;
        } else {
            Profile prof = load_inputmap_profile(path);
            if (!prof.xml.empty())
                return real_fopen(prof.path.c_str(), mode);
        }
    }
    if (extensions_enabled() && x4vr::is_x4_profile_content(path) &&
        is_read_only(mode)) {
        const int fd = content_memfd(path, "fopen");
        if (fd >= 0) {
            if (FILE *f = fdopen(fd, "rb"))
                return f;
            ::close(fd);
        }
    }
    {
        FILE *f = real_fopen(path, mode);
        catpatch_track_file(f, path);
        return f;
    }
}

FILE *fopen64(const char *path, const char *mode) {
    static auto real_fopen64 =
        real<FILE *(*)(const char *, const char *)>("fopen64");
    file_probe("fopen64", path);
    if (is_config_xml(path) || is_inputmap_path(path))
        X4VR_LOG("fopen64(%s, %s)", path, mode);
    if (!overrides_disabled() && x4vr::is_x4_config(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_config(path, steer_hide_x4_line()))
                return f;
        } else {
            Profile prof = load_profile(path);
            if (!prof.xml.empty())
                return real_fopen64(prof.path.c_str(), mode);
        }
    }
    if (headlook_enabled() && x4vr::is_x4_inputmap(path)) {
        if (is_read_only(mode)) {
            if (FILE *f = open_patched_inputmap(path))
                return f;
        } else {
            Profile prof = load_inputmap_profile(path);
            if (!prof.xml.empty())
                return real_fopen64(prof.path.c_str(), mode);
        }
    }
    if (extensions_enabled() && x4vr::is_x4_profile_content(path) &&
        is_read_only(mode)) {
        const int fd = content_memfd(path, "fopen64");
        if (fd >= 0) {
            if (FILE *f = fdopen(fd, "rb"))
                return f;
            ::close(fd);
        }
    }
    {
        FILE *f = real_fopen64(path, mode);
        catpatch_track_file(f, path);
        return f;
    }
}

} // extern "C"

// ---- the read family, for X4VR_CATPATCH (task #68) -------------------------
//
// Only fds that named a file in the patch table do any work; every other read
// pays one map lookup behind a static bool, and with the knob off, nothing at
// all. X4 reads its archives constantly, so this stays on the cheap path.
//
// The file offset is recovered AFTER the read rather than before: lseek to the
// current position and subtract what was returned. One extra syscall on
// tracked fds only, and it cannot race with the read it describes the way a
// before-and-after pair could.
extern "C" ssize_t read(int fd, void *buf, size_t count) {
    static auto real_read = real<ssize_t (*)(int, void *, size_t)>("read");
    const ssize_t n = real_read(fd, buf, count);
    if (n > 0 && catpatch_on()) {
        static auto real_lseek = real<off_t (*)(int, off_t, int)>("lseek");
        const off_t here = real_lseek(fd, 0, SEEK_CUR);
        if (here >= n)
            catpatch_apply(fd, buf, n, (long long)here - n);
    }
    return n;
}

extern "C" ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    static auto real_pread =
        real<ssize_t (*)(int, void *, size_t, off_t)>("pread");
    const ssize_t n = real_pread(fd, buf, count, offset);
    if (n > 0)
        catpatch_apply(fd, buf, n, (long long)offset);
    return n;
}

extern "C" ssize_t pread64(int fd, void *buf, size_t count, off64_t offset) {
    static auto real_pread64 =
        real<ssize_t (*)(int, void *, size_t, off64_t)>("pread64");
    const ssize_t n = real_pread64(fd, buf, count, offset);
    if (n > 0)
        catpatch_apply(fd, buf, n, (long long)offset);
    return n;
}

extern "C" int close(int fd) {
    static auto real_close = real<int (*)(int)>("close");
    catpatch_untrack(fd);
    return real_close(fd);
}

// X4 may map the archive instead of reading it. If it does, the substitution
// above never fires and the take would read as "X4 verified our bytes" when in
// truth it never saw them. Logged so that case is distinguishable.
extern "C" void *mmap(void *addr, size_t len, int prot, int flags, int fd,
                      off_t off) {
    static auto real_mmap =
        real<void *(*)(void *, size_t, int, int, int, off_t)>("mmap");
    void *p = real_mmap(addr, len, prot, flags, fd, off);
    if (catpatch_on() && fd >= 0 && p != MAP_FAILED) {
        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(catfd_mu());
            tracked = catfds().count(fd) != 0;
        }
        if (tracked) {
            static bool said = false;
            if (!said) {
                said = true;
                X4VR_LOG("catpatch: X4 MMAPPED a patched archive (len %zu off "
                         "%lld) — read() substitution does not cover mmap, so "
                         "this run cannot say whether the bytes were seen",
                         len, (long long)off);
            }
        }
    }
    return p;
}

// ---- the stdio read family, which is the one X4 uses -----------------------
//
// Position is taken AFTER the call with ftello and the byte count subtracted,
// so a short read is described correctly and there is no before/after pair to
// race. fread returns ITEMS, not bytes.
extern "C" size_t fread(void *buf, size_t size, size_t n, FILE *f) {
    static auto real_fread =
        real<size_t (*)(void *, size_t, size_t, FILE *)>("fread");
    const size_t got = real_fread(buf, size, n, f);
    if (got && size && catpatch_on()) {
        const long long bytes = (long long)got * (long long)size;
        const off_t here = ftello(f);
        if (here >= bytes)
            catpatch_apply_file(f, buf, (ssize_t)bytes,
                                (long long)here - bytes);
    }
    return got;
}

extern "C" size_t fread_unlocked(void *buf, size_t size, size_t n, FILE *f) {
    static auto real_fru =
        real<size_t (*)(void *, size_t, size_t, FILE *)>("fread_unlocked");
    const size_t got = real_fru(buf, size, n, f);
    if (got && size && catpatch_on()) {
        const long long bytes = (long long)got * (long long)size;
        const off_t here = ftello(f);
        if (here >= bytes)
            catpatch_apply_file(f, buf, (ssize_t)bytes,
                                (long long)here - bytes);
    }
    return got;
}

extern "C" int fclose(FILE *f) {
    static auto real_fclose = real<int (*)(FILE *)>("fclose");
    if (catpatch_on() && f) {
        std::lock_guard<std::mutex> lock(catfd_mu());
        catfiles().erase(f);
    }
    return real_fclose(f);
}
