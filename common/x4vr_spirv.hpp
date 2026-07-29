// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_spirv.hpp — inject a clip-space matrix into a vertex shader.
//
// Phase 3b / the Phase-4 stereo mechanism. Because X4 renders
// camera-relative (M_view == identity, see docs/frame-analysis.md), an eye
// offset d is a pure view-space translation and therefore collapses to a
// single constant clip-space matrix:
//
//     clip_eye = K * clip        K = P * T(-d) * P^-1
//
// So instead of touching the ~1300 per-object constant blocks every frame,
// we append one matrix multiply to the end of each scene vertex shader:
//
//     gl_Position = K * gl_Position;
//
// K is baked in as constants at vkCreateShaderModule time, which keeps the
// per-frame cost at exactly zero.
//
// The patch is deliberately conservative: it only touches modules that have
// a Vertex entry point which writes BuiltIn Position, it never reorders or
// deletes existing instructions, and it bails out (returning false, leaving
// the module untouched) on anything it does not fully understand.
#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace x4vr {
namespace spv {

// --- the small slice of the SPIR-V spec we need -------------------------
enum : uint32_t {
    kMagic = 0x07230203u,

    OpExtension = 10,
    OpEntryPoint = 15,
    OpCapability = 17,
    OpTypeInt = 21,
    OpTypeFloat = 22,
    OpTypeVector = 23,
    OpTypeMatrix = 24,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpFunction = 54,
    OpFunctionEnd = 56,
    OpVariable = 59,
    OpLoad = 61,
    OpStore = 62,
    OpAccessChain = 65,
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpCompositeConstruct = 80,
    OpConvertSToF = 111,
    OpFAdd = 129,
    OpVectorTimesScalar = 142,
    OpMatrixTimesVector = 145,
    OpReturn = 253,

    ExecutionModelVertex = 0,
    DecorationBuiltIn = 11,
    BuiltInPosition = 0,
    StorageClassInput = 1,
    StorageClassOutput = 3,

    // Multiview. The builtin is what makes one draw produce two different
    // eyes: it is the only thing inside a shader that differs between the
    // two views of a masked pass.
    CapabilityMultiView = 4439,
    BuiltInViewIndex = 4440,
};

enum : uint32_t { DecorationDescriptorSet = 34 };

struct Inst {
    uint32_t op = 0;
    uint32_t len = 0;
    size_t start = 0; // word index of the instruction header
};

// What kind of geometry a vertex module draws, decided statically from the
// descriptor sets it declares (see docs/frame-analysis.md for the binding
// slots). World geometry is positioned by the per-object
// BLOCK_BUFFER_BINDING_SLOT_WORLD block at set 3; UI/HUD draws are not.
// This lets each class get its own baked clip-space matrix: the world gets
// the eye offset, the UI gets its own (identity = screen depth, or a chosen
// depth plane).
enum class Kind { NotVertex, World, UI };

inline bool iterate(const std::vector<uint32_t> &code,
                    std::vector<Inst> &out) {
    if (code.size() < 5 || code[0] != kMagic)
        return false;
    size_t i = 5;
    while (i < code.size()) {
        const uint32_t len = code[i] >> 16;
        const uint32_t op = code[i] & 0xffffu;
        if (len == 0 || i + len > code.size())
            return false;
        out.push_back({op, len, i});
        i += len;
    }
    return true;
}

// Appends one instruction to `dst`.
inline void emit(std::vector<uint32_t> &dst, uint32_t op,
                 std::initializer_list<uint32_t> operands) {
    dst.push_back(((uint32_t)(operands.size() + 1) << 16) | op);
    for (uint32_t w : operands)
        dst.push_back(w);
}

/// Classifies a module without modifying it.
///
/// A module counts as World only if its vertex stage actually *reads member
/// 0* (`M_worldviewprojection`) of the set-3 per-object block. Merely
/// declaring the block is not enough — X4's UI shaders declare it too, and
/// shadow-pass shaders declare it but transform through `M_shadowCSM*`
/// (members 3..7) instead. Since the per-eye matrix is derived from the main
/// camera's projection, it is only valid for draws that go through
/// `M_worldviewprojection`; applying it to a light-space shadow pass would be
/// simply wrong (there, clip z is not the constant near plane the derivation
/// assumes).
inline Kind classify(const std::vector<uint32_t> &code) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return Kind::NotVertex;

    bool vertex = false;
    std::unordered_map<uint32_t, uint32_t> const_val; // id -> literal value
    std::vector<uint32_t> set3_vars;
    // Pass 1: entry stage, integer constants, and set-3 variables.
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        switch (in.op) {
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelVertex)
                vertex = true;
            break;
        case OpConstant:
            if (in.len >= 4)
                const_val[w[2]] = w[3];
            break;
        case OpDecorate:
            if (in.len >= 4 && w[2] == DecorationDescriptorSet && w[3] == 3)
                set3_vars.push_back(w[1]);
            break;
        default:
            break;
        }
    }
    if (!vertex)
        return Kind::NotVertex;
    if (set3_vars.empty())
        return Kind::UI;

    // Pass 2: does anything index member 0 of a set-3 block?
    for (const Inst &in : insts) {
        if (in.op != OpAccessChain || in.len < 5)
            continue;
        const uint32_t *w = &code[in.start];
        const uint32_t base = w[3];
        bool is_set3 = false;
        for (uint32_t v : set3_vars)
            if (v == base)
                is_set3 = true;
        if (!is_set3)
            continue;
        auto it = const_val.find(w[4]); // first index = struct member
        if (it != const_val.end() && it->second == 0)
            return Kind::World;
    }
    return Kind::UI;
}

/// Rewrites `code` so every Vertex entry point ends with
/// `gl_Position = K * gl_Position`. `k` is 16 floats, column-major (4
/// consecutive floats per column) matching X4's storage order.
/// Returns false and leaves `code` untouched if the module is not a
/// patchable vertex shader.
/// Bakes `gl_Position = K * gl_Position` into a vertex module.
///
/// With `k_right` non-null the module becomes **stereo**: it reads
/// `gl_ViewIndex` and uses `k` for view 0 and `k_right` for view 1, so a
/// single multiview draw produces two different eyes.
///
/// The selection is arithmetic rather than a branch —
/// `col = colL + float(view) * (colR - colL)` — for two reasons. It needs no
/// basic-block surgery, so the instructions still append cleanly before each
/// `OpReturn` the way the mono path does; and it avoids `OpSelect`, whose
/// rules for a scalar condition with a vector result only relaxed in SPIR-V
/// 1.4. Every op used here is core SPIR-V 1.0.
///
/// `float(view)` is exact for 0 and 1, so this is a selection and not a blend:
/// the difference matrix is added once or not at all, with no rounding in
/// between.
inline bool patch_vertex_clip(std::vector<uint32_t> &code, const float k[16],
                              const float *k_right = nullptr) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    uint32_t bound = code[3];
    auto new_id = [&bound] { return bound++; };

    // --- pass 1: learn the module -------------------------------------
    uint32_t entry_fn = 0;
    std::unordered_map<uint32_t, uint32_t> struct_pos_member; // struct -> idx
    std::vector<uint32_t> var_pos_direct;                     // vars w/ BuiltIn Position
    struct PtrType { uint32_t storage, pointee; };
    std::unordered_map<uint32_t, PtrType> ptr_types;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vars; // id -> (type, storage)
    uint32_t t_float = 0, t_v4 = 0, t_mat4 = 0, t_int = 0;
    uint32_t ptr_out_v4 = 0;
    size_t first_fn = 0;
    bool have_first_fn = false;
    // Where a new OpDecorate may legally go. Annotations form their own
    // section, ahead of every type and constant, so the ViewIndex decoration
    // cannot ride along with the declarations appended before the first
    // function -- that lands in the types section and the module stops
    // validating.
    size_t last_annotation_end = 0;
    size_t first_global = 0;
    bool have_first_global = false;
    bool has_multiview_cap = false;
    bool has_multiview_ext = false;
    size_t caps_end = 0;    // after the last OpCapability
    size_t exts_end = 0;    // after the last OpExtension

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        // Fallback anchor for the annotation insert, for the (unlikely)
        // module that decorates nothing: the types section starts here.
        if (!have_first_global)
            switch (in.op) {
            case OpTypeInt:
            case OpTypeFloat:
            case OpTypeVector:
            case OpTypeMatrix:
            case OpTypeStruct:
            case OpTypePointer:
            case OpConstant:
            case OpConstantComposite:
            case OpVariable:
                first_global = in.start;
                have_first_global = true;
                break;
            default:
                break;
            }
        switch (in.op) {
        case OpCapability:
            caps_end = in.start + in.len;
            if (in.len >= 2 && w[1] == CapabilityMultiView)
                has_multiview_cap = true;
            break;
        case OpExtension:
            exts_end = in.start + in.len;
            if (in.len >= 6 && w[1] == 0x5f565053u && w[2] == 0x5f52484bu &&
                w[3] == 0x746c756du && w[4] == 0x65697669u &&
                w[5] == 0x00000077u)
                has_multiview_ext = true;
            break;
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelVertex && !entry_fn)
                entry_fn = w[2];
            break;
        case OpMemberDecorate:
            last_annotation_end = in.start + in.len;
            if (in.len >= 5 && w[3] == DecorationBuiltIn &&
                w[4] == BuiltInPosition)
                struct_pos_member[w[1]] = w[2];
            break;
        case OpDecorate:
            last_annotation_end = in.start + in.len;
            if (in.len >= 4 && w[2] == DecorationBuiltIn &&
                w[3] == BuiltInPosition)
                var_pos_direct.push_back(w[1]);
            break;
        case OpTypeFloat:
            if (in.len >= 3 && w[2] == 32 && !t_float)
                t_float = w[1];
            break;
        case OpTypeInt:
            if (in.len >= 4 && w[2] == 32 && w[3] == 1 && !t_int)
                t_int = w[1];
            break;
        case OpTypeVector:
            if (in.len >= 4 && w[2] == t_float && w[3] == 4 && !t_v4)
                t_v4 = w[1];
            break;
        case OpTypeMatrix:
            if (in.len >= 4 && w[2] == t_v4 && w[3] == 4 && !t_mat4)
                t_mat4 = w[1];
            break;
        case OpTypePointer:
            if (in.len >= 4) {
                ptr_types[w[1]] = {w[2], w[3]};
                if (w[2] == StorageClassOutput && w[3] == t_v4 && !ptr_out_v4)
                    ptr_out_v4 = w[1];
            }
            break;
        case OpVariable:
            if (in.len >= 4)
                vars[w[2]] = {w[1], w[3]};
            break;
        case OpFunction:
            if (!have_first_fn) {
                first_fn = in.start;
                have_first_fn = true;
            }
            break;
        default:
            break;
        }
    }

    if (!entry_fn || !t_float || !t_v4 || !have_first_fn)
        return false; // not a vertex shader we understand

    // --- locate the gl_Position output --------------------------------
    // Either a variable directly decorated BuiltIn Position, or (the usual
    // case) member N of a gl_PerVertex struct in an Output variable.
    uint32_t pos_var = 0, pos_member = 0;
    bool via_struct = false;
    for (auto &[id, ts] : vars) {
        if (ts.second != StorageClassOutput)
            continue;
        auto pt = ptr_types.find(ts.first);
        if (pt == ptr_types.end())
            continue;
        const uint32_t pointee = pt->second.pointee;
        auto sm = struct_pos_member.find(pointee);
        if (sm != struct_pos_member.end()) {
            pos_var = id;
            pos_member = sm->second;
            via_struct = true;
            break;
        }
    }
    if (!pos_var) {
        for (uint32_t v : var_pos_direct)
            if (vars.count(v) && vars[v].second == StorageClassOutput) {
                pos_var = v;
                via_struct = false;
                break;
            }
    }
    if (!pos_var)
        return false;

    // --- build the declarations we need -------------------------------
    std::vector<uint32_t> decls;
    if (!t_mat4) {
        t_mat4 = new_id();
        emit(decls, OpTypeMatrix, {t_mat4, t_v4, 4});
    }
    if (!ptr_out_v4) {
        ptr_out_v4 = new_id();
        emit(decls, OpTypePointer, {ptr_out_v4, StorageClassOutput, t_v4});
    }
    uint32_t const_member_idx = 0;
    if (via_struct) {
        if (!t_int) {
            t_int = new_id();
            emit(decls, OpTypeInt, {t_int, 32, 1});
        }
        const_member_idx = new_id();
        emit(decls, OpConstant, {t_int, const_member_idx, pos_member});
    }
    // A float constant, reusing nothing -- duplicate OpConstants of the same
    // type and value are legal but a validator may fold them; either way the
    // module stays correct.
    auto fconst = [&](float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        const uint32_t id = new_id();
        emit(decls, OpConstant, {t_float, id, bits});
        return id;
    };
    auto mat_const = [&](const float m[16], uint32_t col_out[4]) {
        for (int c = 0; c < 4; c++) {
            uint32_t comp[4];
            for (int r = 0; r < 4; r++)
                comp[r] = fconst(m[c * 4 + r]);
            col_out[c] = new_id();
            emit(decls, OpConstantComposite,
                 {t_v4, col_out[c], comp[0], comp[1], comp[2], comp[3]});
        }
        const uint32_t id = new_id();
        emit(decls, OpConstantComposite,
             {t_mat4, id, col_out[0], col_out[1], col_out[2], col_out[3]});
        return id;
    };

    // K's 16 scalars -> 4 column vectors -> the matrix constant
    uint32_t col[4];
    const uint32_t const_k = mat_const(k, col);

    // --- stereo: the difference matrix and the ViewIndex input ----------
    uint32_t diff_col[4] = {0, 0, 0, 0};
    uint32_t view_var = 0;
    const bool stereo = k_right != nullptr;
    if (stereo) {
        float diff[16];
        for (int i = 0; i < 16; i++)
            diff[i] = k_right[i] - k[i];
        // Only the columns are used -- the assembled matrix constant is a
        // by-product of reusing mat_const and costs nothing at runtime.
        (void)mat_const(diff, diff_col);

        if (!t_int) {
            t_int = new_id();
            emit(decls, OpTypeInt, {t_int, 32, 1});
        }
        const uint32_t ptr_in_int = new_id();
        emit(decls, OpTypePointer, {ptr_in_int, StorageClassInput, t_int});
        view_var = new_id();
        emit(decls, OpVariable, {ptr_in_int, view_var, StorageClassInput});
    }

    // Where the ViewIndex decoration goes: after the last annotation, or --
    // for a module that decorates nothing -- immediately before the types.
    const size_t anno_at = last_annotation_end ? last_annotation_end
                                               : (have_first_global ? first_global
                                                                    : first_fn);

    // --- rebuild the module -------------------------------------------
    std::vector<uint32_t> out;
    out.reserve(code.size() + decls.size() + 64);
    out.insert(out.end(), code.begin(), code.begin() + 5); // header

    // A module with no OpCapability at all is not a thing the loader would
    // have accepted, but anchor on the header rather than 0 so a malformed
    // input cannot make us write before the instruction stream.
    const size_t cap_at = caps_end ? caps_end : 5;
    const size_t ext_at = exts_end ? exts_end : cap_at;

    bool in_entry = false;
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        // Capabilities come first in a module, extensions immediately after,
        // so both are emitted at the end of their own runs rather than
        // wherever the declarations happen to land.
        if (stereo && !has_multiview_cap && in.start == cap_at)
            emit(out, OpCapability, {CapabilityMultiView});
        // Multiview is core from SPIR-V 1.3 (Vulkan 1.1), where naming the
        // extension is redundant. Only older modules get it -- X4 ships a
        // mix, so this is decided per module rather than once.
        if (stereo && !has_multiview_ext && code[1] < 0x00010300u &&
            in.start == ext_at) {
            // "SPV_KHR_multiview", NUL-terminated and word-padded: 17 chars
            // + NUL = 18 bytes -> 5 words, little-endian.
            static const uint32_t kExt[] = {0x5f565053u, 0x5f52484bu,
                                            0x746c756du, 0x65697669u,
                                            0x00000077u};
            out.push_back((uint32_t)((1 + 5) << 16) | OpExtension);
            for (uint32_t x : kExt)
                out.push_back(x);
        }
        if (stereo && in.start == anno_at)
            emit(out, OpDecorate,
                 {view_var, DecorationBuiltIn, BuiltInViewIndex});
        // declarations go immediately before the first function
        if (in.start == first_fn)
            out.insert(out.end(), decls.begin(), decls.end());

        if (in.op == OpFunction && in.len >= 3 && w[2] == entry_fn)
            in_entry = true;

        // append `gl_Position = K * gl_Position` before every return of the
        // entry point
        if (in_entry && in.op == OpReturn) {
            const uint32_t ptr = via_struct ? new_id() : pos_var;
            if (via_struct)
                emit(out, OpAccessChain,
                     {ptr_out_v4, ptr, pos_var, const_member_idx});
            const uint32_t loaded = new_id();
            emit(out, OpLoad, {t_v4, loaded, ptr});
            uint32_t use_k = const_k;
            if (stereo) {
                // K = K_left + float(gl_ViewIndex) * (K_right - K_left)
                const uint32_t vi = new_id();
                emit(out, OpLoad, {t_int, vi, view_var});
                const uint32_t vf = new_id();
                emit(out, OpConvertSToF, {t_float, vf, vi});
                uint32_t mixed[4];
                for (int c = 0; c < 4; c++) {
                    const uint32_t scaled = new_id();
                    emit(out, OpVectorTimesScalar,
                         {t_v4, scaled, diff_col[c], vf});
                    mixed[c] = new_id();
                    emit(out, OpFAdd, {t_v4, mixed[c], col[c], scaled});
                }
                use_k = new_id();
                emit(out, OpCompositeConstruct,
                     {t_mat4, use_k, mixed[0], mixed[1], mixed[2], mixed[3]});
            }
            const uint32_t mul = new_id();
            emit(out, OpMatrixTimesVector, {t_v4, mul, use_k, loaded});
            emit(out, OpStore, {ptr, mul});
        }

        // The ViewIndex variable is an Input, so it belongs in the entry
        // point's interface list. Required from SPIR-V 1.4 on, and harmless
        // before it; omitting it is the kind of thing that validates on one
        // driver and not the next.
        if (stereo && in.op == OpEntryPoint && in.len >= 3 &&
            w[2] == entry_fn) {
            out.push_back((uint32_t)((in.len + 1) << 16) | OpEntryPoint);
            for (uint32_t j = 1; j < in.len; j++)
                out.push_back(w[j]);
            out.push_back(view_var);
            continue;
        }

        out.insert(out.end(), code.begin() + in.start,
                   code.begin() + in.start + in.len);

        if (in.op == OpFunctionEnd)
            in_entry = false;
    }

    out[3] = bound; // updated id bound
    code.swap(out);
    return true;
}

} // namespace spv
} // namespace x4vr
