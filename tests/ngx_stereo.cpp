// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// DLSS in both eyes (injector/x4vr_ngx.hpp), against a fake NGX: there's no NVIDIA card here.
// What's checked is what the stubs ask of NGX. Whether NGX then draws both eyes is only for a
// real card to say. The patch sites are checked against X4 by x4vr_test_launcher.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../injector/x4vr_ngx.hpp"

using namespace x4vr::ngx;

static int g_fail = 0;
static void check(bool ok, const std::string &what) {
    printf("%s  %s\n", ok ? "ok   " : "FAIL ", what.c_str());
    if (!ok)
        g_fail++;
}

namespace {
constexpr uint32_t kBad = 0xbad00005u;
struct Eval {
    const void *feature;
    uint64_t view[4];
    uint32_t layer[4];
    uint32_t count[4];
};
struct Fake {
    std::map<std::string, void *> params;
    std::vector<Eval> evals;
    std::vector<void *> created, released;
    int next = 0x100;
    int fail_create_at = -1, creates = 0;
    int fail_eval_at = -1, evaluated = 0;
    bool no_right = false, input_shared = false;
} f;

uint32_t fake_create(void *, int, void *, void **out) {
    if (f.creates++ == f.fail_create_at)
        return kBad;
    *out = reinterpret_cast<void *>((uintptr_t)f.next);
    f.next += 0x10;
    f.created.push_back(*out);
    return 1;
}
uint32_t fake_evaluate(void *, const void *feature, const void *, void *) {
    if (f.evaluated++ == f.fail_eval_at)
        return kBad;
    Eval e{};
    e.feature = feature;
    for (int i = 0; i < 4; i++) {
        const ResourceVk *r = static_cast<const ResourceVk *>(f.params[kImages[i]]);
        e.view[i] = r->view;
        e.layer[i] = r->base_layer;
        e.count[i] = r->layer_count;
    }
    f.evals.push_back(e);
    return 1;
}
uint32_t fake_release(void *feature) {
    f.released.push_back(feature);
    return 1;
}
uint32_t fake_get(void *, const char *name, void **out) {
    const auto it = f.params.find(name);
    if (it == f.params.end())
        return kBad;
    *out = it->second;
    return 1;
}
void fake_set(void *, const char *name, void *value) { f.params[name] = value; }
uint64_t fake_right_view(void *, uint64_t view) {
    if (f.no_right)
        return view;
    if (f.input_shared && view == 3000)
        return view;
    return view + 1;
}

ResourceVk res(uint64_t view) {
    ResourceVk r{};
    r.view = view;
    r.image = view + 500;
    r.aspect = 1;
    r.mip_count = 1;
    r.layer_count = 1;
    r.format = 97;
    r.width = 1877;
    r.height = 1877;
    r.read_write = true;
    return r;
}
void *const kCmd = reinterpret_cast<void *>(0xc0), *const kParams = reinterpret_cast<void *>(0xd0);
} // namespace

int main() {
    ResourceVk color = res(1000), output = res(2000), depth = res(3000), mv = res(4000);
    auto reset = [&] {
        f = Fake{};
        f.params = {{"Color", &color}, {"Output", &output}, {"Depth", &depth}, {"MotionVectors", &mv}};
    };
    auto fresh = [] {
        auto *s = new Stereo; // Stereo holds a mutex and isn't assignable.
        s->api = Api{fake_create, fake_evaluate, fake_release, fake_get, fake_set, fake_right_view};
        return s;
    };

    // A feature for each eye.
    reset();
    Stereo *s = fresh();
    void *handle = nullptr;
    check(!failed(s->create(kCmd, kSuperSampling, kParams, &handle)) && handle, "X4 gets its feature");
    check(f.created.size() == 2 && f.created[0] == handle && f.created[1] != handle,
          "a second one is made, and X4 keeps the first");
    check(s->stats.features == 1, "...counted");

    // An evaluation for each eye.
    check(!failed(s->evaluate(kCmd, handle, kParams, nullptr)), "X4's evaluation succeeds");
    check(f.evals.size() == 2, "it runs twice");
    check(f.evals[0].feature == handle && f.evals[1].feature == f.created[1],
          "the left with X4's feature, the right with its own: each keeps its own history");
    bool left_as_asked = true, right_layer1 = true;
    const uint64_t views[4] = {1000, 2000, 3000, 4000};
    for (int i = 0; i < 4; i++) {
        left_as_asked = left_as_asked && f.evals[0].view[i] == views[i] && f.evals[0].layer[i] == 0;
        right_layer1 = right_layer1 && f.evals[1].view[i] == views[i] + 1 &&
                       f.evals[1].layer[i] == 1 && f.evals[1].count[i] == 1;
    }
    check(left_as_asked, "the left eye sees X4's four images, untouched");
    check(right_layer1, "the right eye sees layer 1 of all four, and says so in the range");
    check(f.params["Color"] == &color && f.params["Output"] == &output && f.params["Depth"] == &depth &&
              f.params["MotionVectors"] == &mv,
          "X4's parameters are given back as they were");
    check(color.view == 1000 && color.base_layer == 0, "X4's own resource isn't written to");
    check(s->stats.left == 1 && s->stats.right == 1, "both are counted");

    // Giving them back.
    void *right = f.created[1];
    check(!failed(s->release(handle)), "X4's release succeeds");
    check(f.released.size() == 2 && f.released[0] == right && f.released[1] == handle,
          "both features go");
    f.evals.clear();
    s->evaluate(kCmd, handle, kParams, nullptr);
    check(f.evals.size() == 1 && s->stats.no_feature == 1,
          "after a release X4's stale handle has no partner: left only, counted");
    delete s;

    // The right eye's feature refused: X4 carries on, and the count says so.
    reset();
    f.fail_create_at = 1;
    s = fresh();
    check(!failed(s->create(kCmd, kSuperSampling, kParams, &handle)) && handle && s->stats.right_refused == 1 &&
              s->stats.last_error == kBad,
          "the right eye's feature refused: X4's stands, and it's counted");
    s->evaluate(kCmd, handle, kParams, nullptr);
    check(f.evals.size() == 1 && s->stats.no_feature == 1, "...so one evaluation");
    delete s;

    // X4's own refused: nothing more is tried.
    reset();
    f.fail_create_at = 0;
    s = fresh();
    handle = nullptr;
    check(failed(s->create(kCmd, kSuperSampling, kParams, &handle)) && f.creates == 1,
          "X4's feature refused: X4 is told, and no second is tried");
    delete s;

    // Only DLSS's feature is doubled.
    reset();
    s = fresh();
    s->create(kCmd, 11, kParams, &handle);
    check(f.created.size() == 1, "another feature isn't doubled");
    delete s;

    // Left fails, right isn't tried.
    reset();
    s = fresh();
    s->create(kCmd, kSuperSampling, kParams, &handle);
    f.fail_eval_at = 0;
    check(failed(s->evaluate(kCmd, handle, kParams, nullptr)) && f.evaluated == 1 && s->stats.right == 0,
          "X4's evaluation fails: X4 is told, and the right isn't tried");
    delete s;

    // Right fails: X4 isn't told, the parameters are restored.
    reset();
    s = fresh();
    s->create(kCmd, kSuperSampling, kParams, &handle);
    f.fail_eval_at = 1;
    check(!failed(s->evaluate(kCmd, handle, kParams, nullptr)) && s->stats.right_failed == 1 &&
              s->stats.last_error == kBad,
          "the right's evaluation fails: X4 goes on, and it's counted");
    check(f.params["Output"] == &output, "...with its parameters given back");
    delete s;

    // No layer 1 to write to: writing anyway would overwrite the left eye's picture.
    reset();
    s = fresh();
    s->create(kCmd, kSuperSampling, kParams, &handle);
    f.no_right = true;
    s->evaluate(kCmd, handle, kParams, nullptr);
    check(f.evals.size() == 1 && s->stats.no_view == 1 && f.params["Output"] == &output,
          "no right-eye output: the left is left alone, and it's counted");
    delete s;

    // No layer yet to ask.
    reset();
    s = fresh();
    s->api.right_view = nullptr;
    s->create(kCmd, kSuperSampling, kParams, &handle);
    s->evaluate(kCmd, handle, kParams, nullptr);
    check(f.evals.size() == 1 && s->stats.no_view == 1, "the layer hasn't answered yet: left only");
    delete s;

    // An input X4 draws for both eyes is shared; the rest are still per eye.
    reset();
    s = fresh();
    s->create(kCmd, kSuperSampling, kParams, &handle);
    f.input_shared = true;
    s->evaluate(kCmd, handle, kParams, nullptr);
    check(f.evals.size() == 2 && f.evals[1].view[2] == 3000 && f.evals[1].layer[2] == 0 &&
              f.evals[1].view[1] == 2001 && f.evals[1].layer[1] == 1,
          "a shared input stays on layer 0, and the output still goes to layer 1");
    delete s;

    // A parameter X4 didn't set: the left has run, and nothing of the right's is half done.
    reset();
    s = fresh();
    s->create(kCmd, kSuperSampling, kParams, &handle);
    {
        ResourceVk *mv_kept = static_cast<ResourceVk *>(f.params["MotionVectors"]);
        // The fake reads all four, so the left runs with them and the lookup then loses one.
        const auto missing = [](void *, const char *name, void **out) -> uint32_t {
            if (!strcmp(name, "MotionVectors"))
                return kBad;
            return fake_get(nullptr, name, out);
        };
        s->api.get_pointer = missing;
        s->evaluate(kCmd, handle, kParams, nullptr);
        check(f.evals.size() == 1 && s->stats.no_view == 1 && f.params["Color"] == &color &&
                  f.params["MotionVectors"] == mv_kept,
              "an image NGX can't name: left only, nothing changed, counted");
    }
    delete s;

    check(sizeof(ResourceVk) == 56 && offsetof(ResourceVk, base_layer) == 28 &&
              offsetof(ResourceVk, format) == 36 && offsetof(ResourceVk, type) == 48 &&
              offsetof(ResourceVk, read_write) == 52,
          "the resource is laid out as X4 fills it at 0x20e2f40");

    printf("\n%s\n", g_fail ? "SOME CHECKS FAILED" : "all checks passed");
    return g_fail ? 1 : 0;
}
