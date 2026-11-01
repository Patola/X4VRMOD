// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// A float32 interpreter for the straight-line SPIR-V of one entry point, so a
// vertex patch can be checked by what it COMPUTES rather than by its shape.
//
// Written for take 397: the mvp route (patch_vertex_eye_offset_mvp) had no
// roll while the camera-block route did, and nothing in the suite compared
// where the two put the same vertex. spirv-val passes a module that computes
// the wrong number; this runs it.
//
// Deliberately small. One basic block, no calls, no branches, and only the
// instructions X4-shaped vertex entries and our patches emit. Anything else is
// an error naming the opcode -- an interpreter that skipped what it did not
// know would report agreement for code it never ran.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace x4vr::interp {

namespace op {
constexpr uint32_t ExtInst = 12, TypeVoid = 19, TypeBool = 20, TypeInt = 21,
                   TypeFloat = 22, TypeVector = 23, TypeMatrix = 24,
                   TypeArray = 28, TypeStruct = 30, TypePointer = 32,
                   TypeFunction = 33, ConstantTrue = 41, ConstantFalse = 42,
                   Constant = 43, ConstantComposite = 44, Function = 54,
                   FunctionEnd = 56, Variable = 59, Load = 61, Store = 62,
                   AccessChain = 65, Decorate = 71, MemberDecorate = 72,
                   VectorShuffle = 79, CompositeConstruct = 80,
                   CompositeExtract = 81, CompositeInsert = 82,
                   ConvertSToF = 111, FNegate = 127, FAdd = 129, FSub = 131,
                   FMul = 133, FDiv = 136, VectorTimesScalar = 142,
                   MatrixTimesVector = 145, Select = 169,
                   FOrdGreaterThan = 186, Label = 248, Return = 253,
                   EntryPoint = 15;
} // namespace op

constexpr uint32_t kDecBuiltIn = 11, kDecOffset = 35, kDecMatrixStride = 7,
                   kDecArrayStride = 6, kDecSet = 34, kDecBinding = 33,
                   kDecLocation = 30, kDecRowMajor = 4;
constexpr uint32_t kBuiltInPosition = 0, kBuiltInViewIndex = 4440;
constexpr uint32_t kSCInput = 1, kSCUniform = 2, kSCOutput = 3,
                   kSCFunction = 7, kSCPrivate = 6;
constexpr uint32_t kGlslSqrt = 31;

struct Type {
    uint32_t op = 0;
    uint32_t elem = 0;  // vector component / matrix column / array element
    uint32_t count = 0; // vector size / matrix columns / array length
    std::vector<uint32_t> members;
    uint32_t storage = 0, pointee = 0; // pointers
};

// A pointer: into a uniform buffer (byte offset) or into a variable's
// flattened float storage (scalar offset).
struct Ptr {
    bool uniform = false;
    uint32_t var = 0;
    size_t off = 0;
    uint32_t type = 0;
};

struct Machine {
    std::string error;
    // Inputs the caller sets before run().
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> buffers; // (set,binding)
    std::map<uint32_t, std::vector<float>> input_location; // location -> value
    int view_index = 0;
    // The result: gl_Position for a vertex entry, and every output by its
    // location for either stage.
    float position[4] = {0, 0, 0, 0};
    std::map<uint32_t, std::vector<float>> outputs;

    bool run(const std::vector<uint32_t> &code, uint32_t model) {
        if (code.size() < 5 || code[0] != 0x07230203u)
            return fail("not SPIR-V");
        // ---- pass 1: types, constants, decorations, variables, entry ----
        uint32_t entry = 0;
        for (size_t i = 5; i < code.size();) {
            const uint32_t len = code[i] >> 16, o = code[i] & 0xffff;
            if (!len || i + len > code.size())
                return fail("bad instruction length");
            const uint32_t *w = &code[i];
            switch (o) {
            case op::EntryPoint:
                if (w[1] == model && !entry)
                    entry = w[2];
                break;
            case op::TypeVoid: case op::TypeBool: case op::TypeFunction:
                types_[w[1]].op = o;
                break;
            case op::TypeInt: case op::TypeFloat:
                types_[w[1]].op = o;
                break;
            case op::TypeVector: case op::TypeMatrix:
                types_[w[1]] = Type{o, w[2], w[3], {}, 0, 0};
                break;
            case op::TypeArray: {
                Type t{o, w[2], 0, {}, 0, 0};
                auto c = consts_.find(w[3]);
                if (c == consts_.end())
                    return fail("array length not a constant");
                t.count = (uint32_t)c->second[0];
                types_[w[1]] = t;
                break;
            }
            case op::TypeStruct: {
                Type t{o, 0, len - 2, {}, 0, 0};
                for (uint32_t j = 2; j < len; j++)
                    t.members.push_back(w[j]);
                types_[w[1]] = t;
                break;
            }
            case op::TypePointer:
                types_[w[1]] = Type{o, 0, 0, {}, w[2], w[3]};
                break;
            case op::Constant: {
                const Type &t = types_[w[1]];
                float v;
                if (t.op == op::TypeFloat)
                    memcpy(&v, &w[3], 4);
                else
                    v = (float)(int32_t)w[3];
                consts_[w[2]] = {v};
                ctype_[w[2]] = w[1];
                break;
            }
            case op::ConstantTrue: case op::ConstantFalse:
                consts_[w[2]] = {o == op::ConstantTrue ? 1.f : 0.f};
                ctype_[w[2]] = w[1];
                break;
            case op::ConstantComposite: {
                std::vector<float> v;
                for (uint32_t j = 3; j < len; j++)
                    for (float x : consts_[w[j]])
                        v.push_back(x);
                consts_[w[2]] = v;
                ctype_[w[2]] = w[1];
                break;
            }
            case op::Decorate:
                if (w[2] == kDecSet) set_[w[1]] = w[3];
                if (w[2] == kDecBinding) binding_[w[1]] = w[3];
                if (w[2] == kDecLocation) location_[w[1]] = w[3];
                if (w[2] == kDecBuiltIn) builtin_[w[1]] = w[3];
                if (w[2] == kDecArrayStride) array_stride_[w[1]] = w[3];
                break;
            case op::MemberDecorate:
                if (w[3] == kDecOffset) member_off_[{w[1], w[2]}] = w[4];
                if (w[3] == kDecMatrixStride) member_mstride_[{w[1], w[2]}] = w[4];
                if (w[3] == kDecRowMajor) member_rowmajor_[{w[1], w[2]}] = true;
                if (w[3] == kDecBuiltIn) member_builtin_[{w[1], w[2]}] = w[4];
                break;
            case op::Variable:
                var_type_[w[2]] = w[1];
                var_storage_[w[2]] = w[3];
                break;
            default:
                break;
            }
            i += len;
        }
        if (!entry)
            return fail("no entry point for the model");
        // Storage for every non-uniform variable, inputs filled.
        for (auto &[id, pt] : var_type_) {
            const uint32_t sc = var_storage_[id];
            if (sc == kSCUniform)
                continue;
            const uint32_t pointee = types_[pt].pointee;
            std::vector<float> s(flat_size(pointee), 0.f);
            if (sc == kSCInput) {
                auto b = builtin_.find(id);
                if (b != builtin_.end() && b->second == kBuiltInViewIndex)
                    s.assign(1, (float)view_index);
                auto l = location_.find(id);
                if (l != location_.end()) {
                    auto in = input_location.find(l->second);
                    if (in == input_location.end())
                        return fail("input location not provided");
                    for (size_t k = 0; k < s.size() && k < in->second.size(); k++)
                        s[k] = in->second[k];
                }
            }
            store_[id] = s;
        }
        // ---- pass 2: run the entry function ----
        bool in_fn = false;
        for (size_t i = 5; i < code.size();) {
            const uint32_t len = code[i] >> 16, o = code[i] & 0xffff;
            const uint32_t *w = &code[i];
            i += len;
            if (o == op::Function) {
                in_fn = w[2] == entry;
                continue;
            }
            if (!in_fn)
                continue;
            if (o == op::Return)
                break;
            if (!step(o, w, len))
                return false;
        }
        for (auto &[id, l] : location_)
            if (var_storage_[id] == kSCOutput)
                outputs[l] = store_[id];
        return model != 0 || read_position(); // 0 = Vertex
    }

private:
    std::unordered_map<uint32_t, Type> types_;
    std::unordered_map<uint32_t, std::vector<float>> consts_, vals_, store_;
    std::unordered_map<uint32_t, uint32_t> ctype_, vtype_;
    std::unordered_map<uint32_t, uint32_t> set_, binding_, location_, builtin_,
        array_stride_, var_type_, var_storage_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> member_off_,
        member_mstride_, member_builtin_;
    std::map<std::pair<uint32_t, uint32_t>, bool> member_rowmajor_;
    std::unordered_map<uint32_t, Ptr> ptrs_;

    bool fail(const std::string &e) {
        error = e;
        return false;
    }
    size_t flat_size(uint32_t t) {
        const Type &ty = types_[t];
        switch (ty.op) {
        case op::TypeVector: case op::TypeMatrix: case op::TypeArray:
            return ty.count * flat_size(ty.elem);
        case op::TypeStruct: {
            size_t n = 0;
            for (uint32_t m : ty.members)
                n += flat_size(m);
            return n;
        }
        default:
            return 1;
        }
    }
    const std::vector<float> *val(uint32_t id) {
        auto v = vals_.find(id);
        if (v != vals_.end())
            return &v->second;
        auto c = consts_.find(id);
        return c == consts_.end() ? nullptr : &c->second;
    }
    uint32_t type_of(uint32_t id) {
        auto v = vtype_.find(id);
        if (v != vtype_.end())
            return v->second;
        auto c = ctype_.find(id);
        return c == ctype_.end() ? 0 : c->second;
    }
    // Reads a value of type t from a uniform buffer at byte offset `off`.
    // Matrices are column-major with the stride the enclosing member declared.
    bool read_uniform(const std::vector<uint8_t> &buf, size_t off, uint32_t t,
                      uint32_t mstride, std::vector<float> &out) {
        const Type &ty = types_[t];
        switch (ty.op) {
        case op::TypeFloat: {
            if (off + 4 > buf.size())
                return fail("uniform read past the buffer");
            float f;
            memcpy(&f, &buf[off], 4);
            out.push_back(f);
            return true;
        }
        case op::TypeVector:
            for (uint32_t k = 0; k < ty.count; k++)
                if (!read_uniform(buf, off + 4 * k, ty.elem, 0, out))
                    return false;
            return true;
        case op::TypeMatrix:
            if (!mstride)
                return fail("matrix without a MatrixStride");
            for (uint32_t c = 0; c < ty.count; c++)
                if (!read_uniform(buf, off + (size_t)c * mstride, ty.elem, 0, out))
                    return false;
            return true;
        case op::TypeStruct:
            for (uint32_t m = 0; m < ty.members.size(); m++) {
                auto mo = member_off_.find({t, m});
                if (mo == member_off_.end())
                    return fail("struct member without an Offset");
                auto ms = member_mstride_.find({t, m});
                if (!read_uniform(buf, off + mo->second, ty.members[m],
                                  ms == member_mstride_.end() ? 0 : ms->second,
                                  out))
                    return false;
            }
            return true;
        default:
            return fail("unsupported uniform type");
        }
    }
    bool step(uint32_t o, const uint32_t *w, uint32_t len) {
        auto put = [&](uint32_t rt, uint32_t id, std::vector<float> v) {
            vtype_[id] = rt;
            vals_[id] = std::move(v);
        };
        auto in = [&](uint32_t id) -> const std::vector<float> & {
            static const std::vector<float> empty;
            const std::vector<float> *v = val(id);
            return v ? *v : empty;
        };
        switch (o) {
        case op::Label:
            return true;
        case op::Variable: // Function-scope: storage already allocated? no
            store_[w[2]].assign(flat_size(types_[w[1]].pointee), 0.f);
            var_type_[w[2]] = w[1];
            var_storage_[w[2]] = kSCFunction;
            return true;
        case op::AccessChain: {
            Ptr p;
            auto base = ptrs_.find(w[3]);
            uint32_t mstride = 0;
            if (base != ptrs_.end()) {
                p = base->second;
            } else {
                p.var = w[3];
                p.type = types_[var_type_[w[3]]].pointee;
                p.uniform = var_storage_[w[3]] == kSCUniform;
            }
            for (uint32_t j = 4; j < len; j++) {
                auto c = consts_.find(w[j]);
                if (c == consts_.end())
                    return fail("dynamic access chain index");
                const uint32_t k = (uint32_t)c->second[0];
                const Type &ty = types_[p.type];
                if (ty.op == op::TypeStruct) {
                    if (p.uniform) {
                        auto mo = member_off_.find({p.type, k});
                        if (mo == member_off_.end())
                            return fail("uniform member without an Offset");
                        p.off += mo->second;
                        auto ms = member_mstride_.find({p.type, k});
                        mstride = ms == member_mstride_.end() ? 0 : ms->second;
                        if (member_rowmajor_.count({p.type, k}))
                            return fail("RowMajor is not interpreted");
                    } else {
                        for (uint32_t m = 0; m < k; m++)
                            p.off += flat_size(ty.members[m]);
                    }
                    p.type = ty.members[k];
                } else if (ty.op == op::TypeMatrix) {
                    p.off += p.uniform ? (size_t)k * mstride
                                       : k * flat_size(ty.elem);
                    p.type = ty.elem;
                } else if (ty.op == op::TypeVector) {
                    p.off += p.uniform ? 4 * k : k;
                    p.type = ty.elem;
                } else if (ty.op == op::TypeArray) {
                    auto as = array_stride_.find(p.type);
                    p.off += p.uniform ? (size_t)k * (as == array_stride_.end() ? 0 : as->second)
                                       : k * flat_size(ty.elem);
                    p.type = ty.elem;
                } else {
                    return fail("access chain into a scalar");
                }
            }
            // A uniform matrix keeps its stride for the load.
            p.var = p.var ? p.var : w[3];
            ptrs_[w[2]] = p;
            mstride_of_[w[2]] = mstride;
            return true;
        }
        case op::Load: {
            Ptr p;
            auto pp = ptrs_.find(w[3]);
            uint32_t mstride = 0;
            if (pp != ptrs_.end()) {
                p = pp->second;
                mstride = mstride_of_[w[3]];
            } else {
                p.var = w[3];
                p.type = types_[var_type_[w[3]]].pointee;
                p.uniform = var_storage_[w[3]] == kSCUniform;
            }
            std::vector<float> v;
            if (p.uniform) {
                const auto key = std::make_pair(set_[p.var], binding_[p.var]);
                auto b = buffers.find(key);
                if (b == buffers.end())
                    return fail("no buffer at set " + std::to_string(key.first) +
                                " binding " + std::to_string(key.second));
                if (!read_uniform(b->second, p.off, p.type, mstride, v))
                    return false;
            } else {
                const std::vector<float> &s = store_[p.var];
                const size_t n = flat_size(p.type);
                if (p.off + n > s.size())
                    return fail("load past a variable");
                v.assign(s.begin() + p.off, s.begin() + p.off + n);
            }
            put(w[1], w[2], v);
            return true;
        }
        case op::Store: {
            Ptr p;
            auto pp = ptrs_.find(w[1]);
            if (pp != ptrs_.end()) {
                p = pp->second;
            } else {
                p.var = w[1];
                p.type = types_[var_type_[w[1]]].pointee;
            }
            if (p.uniform)
                return fail("store to a uniform");
            std::vector<float> &s = store_[p.var];
            const std::vector<float> &v = in(w[2]);
            if (p.off + v.size() > s.size())
                return fail("store past a variable");
            std::copy(v.begin(), v.end(), s.begin() + p.off);
            return true;
        }
        case op::CompositeConstruct: {
            std::vector<float> v;
            for (uint32_t j = 3; j < len; j++)
                for (float x : in(w[j]))
                    v.push_back(x);
            put(w[1], w[2], v);
            return true;
        }
        case op::CompositeExtract: {
            uint32_t t = type_of(w[3]);
            size_t off = 0;
            for (uint32_t j = 4; j < len; j++) {
                const Type &ty = types_[t];
                if (ty.op == op::TypeStruct) {
                    for (uint32_t m = 0; m < w[j]; m++)
                        off += flat_size(ty.members[m]);
                    t = ty.members[w[j]];
                } else {
                    off += w[j] * flat_size(ty.elem);
                    t = ty.elem;
                }
            }
            const std::vector<float> &src = in(w[3]);
            const size_t n = flat_size(t);
            if (off + n > src.size())
                return fail("extract past a composite");
            put(w[1], w[2], std::vector<float>(src.begin() + off, src.begin() + off + n));
            return true;
        }
        case op::CompositeInsert: {
            uint32_t t = w[1];
            size_t off = 0;
            for (uint32_t j = 5; j < len; j++) {
                const Type &ty = types_[t];
                off += w[j] * flat_size(ty.elem);
                t = ty.elem;
            }
            std::vector<float> v = in(w[4]);
            const std::vector<float> &obj = in(w[3]);
            if (off + obj.size() > v.size())
                return fail("insert past a composite");
            std::copy(obj.begin(), obj.end(), v.begin() + off);
            put(w[1], w[2], v);
            return true;
        }
        case op::VectorShuffle: {
            std::vector<float> both = in(w[3]);
            for (float x : in(w[4]))
                both.push_back(x);
            std::vector<float> v;
            for (uint32_t j = 5; j < len; j++)
                v.push_back(w[j] < both.size() ? both[w[j]] : 0.f);
            put(w[1], w[2], v);
            return true;
        }
        case op::MatrixTimesVector: {
            const std::vector<float> &m = in(w[3]), &x = in(w[4]);
            const size_t cols = x.size(), rows = cols ? m.size() / cols : 0;
            std::vector<float> v(rows, 0.f);
            for (size_t r = 0; r < rows; r++) {
                float acc = 0.f;
                for (size_t c = 0; c < cols; c++)
                    acc = acc + m[c * rows + r] * x[c];
                v[r] = acc;
            }
            put(w[1], w[2], v);
            return true;
        }
        case op::VectorTimesScalar: {
            std::vector<float> v = in(w[3]);
            const float s = in(w[4])[0];
            for (float &x : v)
                x = x * s;
            put(w[1], w[2], v);
            return true;
        }
        case op::FAdd: case op::FSub: case op::FMul: case op::FDiv: {
            const std::vector<float> &a = in(w[3]), &b = in(w[4]);
            std::vector<float> v(a.size());
            for (size_t k = 0; k < a.size(); k++)
                v[k] = o == op::FAdd ? a[k] + b[k]
                     : o == op::FSub ? a[k] - b[k]
                     : o == op::FMul ? a[k] * b[k]
                                     : a[k] / b[k];
            put(w[1], w[2], v);
            return true;
        }
        case op::FNegate: {
            std::vector<float> v = in(w[3]);
            for (float &x : v)
                x = -x;
            put(w[1], w[2], v);
            return true;
        }
        case op::ConvertSToF:
            put(w[1], w[2], in(w[3]));
            return true;
        case op::FOrdGreaterThan:
            put(w[1], w[2], {in(w[3])[0] > in(w[4])[0] ? 1.f : 0.f});
            return true;
        case op::Select:
            put(w[1], w[2], in(w[3])[0] != 0.f ? in(w[4]) : in(w[5]));
            return true;
        case op::ExtInst:
            if (w[4] != kGlslSqrt)
                return fail("ExtInst other than Sqrt");
            put(w[1], w[2], {std::sqrt(in(w[5])[0])});
            return true;
        default:
            return fail("unsupported opcode " + std::to_string(o));
        }
    }
    std::unordered_map<uint32_t, uint32_t> mstride_of_;

    bool read_position() {
        for (auto &[id, pt] : var_type_) {
            if (var_storage_[id] != kSCOutput)
                continue;
            const uint32_t t = types_[pt].pointee;
            auto b = builtin_.find(id);
            if (b != builtin_.end() && b->second == kBuiltInPosition) {
                memcpy(position, store_[id].data(), 4 * sizeof(float));
                return true;
            }
            const Type &ty = types_[t];
            if (ty.op != op::TypeStruct)
                continue;
            size_t off = 0;
            for (uint32_t m = 0; m < ty.members.size(); m++) {
                auto mb = member_builtin_.find({t, m});
                if (mb != member_builtin_.end() && mb->second == kBuiltInPosition) {
                    memcpy(position, store_[id].data() + off, 4 * sizeof(float));
                    return true;
                }
                off += flat_size(ty.members[m]);
            }
        }
        return fail("no gl_Position output");
    }
};

} // namespace x4vr::interp
