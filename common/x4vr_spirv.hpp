// SPDX-License-Identifier: GPL-3.0-or-later WITH x4vrmod-linking-exception
//
// x4vr_spirv.hpp — the two shader edits the stereo mechanism needs.
//
// `patch_vertex_clip` shears clip space per eye; `patch_fragment_view_layer`
// makes a sampled texture read follow the view index. The first makes the two
// eyes different, the second stops them collapsing back together the moment
// something samples the result. See the second function for why sampling is a
// separate problem at all.
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

// For x4vr::OffAxis. The two vertex patches emit `apply_off_axis` verbatim, so
// they take the struct that function takes rather than four loose floats --
// reading a list of coefficients positionally is a mistake this project has
// made before, and a named field cannot be swapped by accident. x4vr_view.hpp
// includes nothing from here, so the dependency is one-way.
#include "x4vr_view.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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
    OpTypeImage = 25,
    OpTypeSampler = 26,
    OpTypeSampledImage = 27,
    OpTypeArray = 28,
    OpTypeRuntimeArray = 29,
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
    OpSampledImage = 86,
    OpImageSampleImplicitLod = 87,
    OpImageSampleExplicitLod = 88,
    OpImageFetch = 95,
    OpImageGather = 96,
    OpImage = 100,
    OpCompositeExtract = 81,
    OpCompositeInsert = 82,
    OpConvertSToF = 111,
    OpBitcast = 124,
    OpFNegate = 127,
    OpIAdd = 128,
    OpFAdd = 129,
    OpFSub = 131,
    OpIMul = 132,
    OpFMul = 133,
    OpVectorTimesScalar = 142,
    OpMatrixTimesVector = 145,
    OpReturn = 253,

    // For patch_vertex_eye_offset_mvp. OpSelect with a *scalar* condition and
    // a scalar result is core SPIR-V 1.0 -- it is the vector-result form that
    // only relaxed in 1.4, which is what patch_vertex_clip's note is about.
    OpExtInstImport = 11,
    OpExtInst = 12,
    OpTypeBool = 20,
    OpSelect = 169,
    OpFDiv = 136,
    OpFOrdGreaterThan = 186,
    GLSLstd450Sqrt = 31,

    // For the off-axis affine (task #35). Core SPIR-V 1.0, like everything
    // else emitted here.
    OpLogicalAnd = 167,

    ExecutionModelVertex = 0,
    ExecutionModelFragment = 4,
    ExecutionModelGLCompute = 5,
    DecorationBuiltIn = 11,
    // An integer Input in a fragment shader must be Flat -- interpolating one
    // is meaningless and Vulkan forbids it (VUID-StandaloneSpirv-Flat-04744).
    // gl_ViewIndex is no exception, builtin or not.
    DecorationFlat = 14,
    BuiltInPosition = 0,
    StorageClassUniformConstant = 0,
    StorageClassInput = 1,
    StorageClassUniform = 2,
    StorageClassOutput = 3,
    Dim2D = 1,

    // Multiview. The builtin is what makes one draw produce two different
    // eyes: it is the only thing inside a shader that differs between the
    // two views of a masked pass.
    CapabilityMultiView = 4439,
    BuiltInViewIndex = 4440,
};

enum : uint32_t { DecorationBinding = 33, DecorationDescriptorSet = 34 };

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
// World = geometry positioned by a per-object matrix (set 3), or -- under
// `wide_camera` -- by the camera block. NonWorld = everything else.
//
// NonWorld is named for what it is rather than for its most visible member. It
// does contain the UI and HUD shaders, but also every fullscreen triangle and
// every procedural vertex shader in the frame -- on X4, 54 modules of 350, most
// of which no player would call UI. Naming the set after the HUD invites the
// reading that its matrix moves the HUD, which is not what it does.
enum class Kind { NotVertex, World, NonWorld };

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

// The same, for an operand count only known at run time -- OpTypeImage carries
// an optional trailing access qualifier, and a relocated OpVariable an optional
// initialiser, so neither has a fixed length to copy.
inline void emit_n(std::vector<uint32_t> &dst, uint32_t op,
                   const std::vector<uint32_t> &operands) {
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
/// `wide_camera` additionally counts geometry positioned by the **camera**
/// rather than by a per-object matrix: a module with no set-3 block at all,
/// whose *vertex stage* reads `M_view`, `M_projection`, `M_viewprojection` or
/// `M_viewinverse` from the camera block at set 1, binding 0.
///
/// That is what X4's instanced deferred light volumes are (`mod-0207`:
/// `IO_center`, `IO_radius`, `IO_lightcolor`, six instance locations, no set 3).
/// Without it they draw unsheared while the geometry they light is sheared, and
/// the light lands on the wrong pixels in view 1 — see task #22 and P70.
///
/// **The camera check is restricted to the vertex entry point's function, and
/// that restriction is load-bearing.** 247 of X4's 409 modules read
/// `M_invprojection` in their *fragment* stage, and X4 ships combined modules,
/// so a whole-module scan would classify almost everything World. The set-3
/// check below is deliberately left scanning the whole module: it has always
/// done so, and 0 of 409 modules differ between the two readings, so narrowing
/// it now would be an unmeasured change riding along with a measured one.
///
/// Measured over take 61's 397 dumps: +18 modules, of which 6 are in the
/// lighting passes, 0 are fullscreen, and 0 are bound to a present pass. Two
/// (203, 225) are bound to shadow passes and are safe only because the
/// pass-level MONO gate substitutes the unsheared twin for depth-only passes.
inline Kind classify(const std::vector<uint32_t> &code,
                     bool wide_camera = false) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return Kind::NotVertex;

    constexpr uint32_t kDecorationBinding = 33u;
    bool vertex = false;
    uint32_t vert_fn = 0;
    std::unordered_map<uint32_t, uint32_t> const_val; // id -> literal value
    std::vector<uint32_t> set3_vars;
    std::unordered_set<uint32_t> set1_vars, bind0_vars;
    // Pass 1: entry stage, integer constants, and set-3 variables.
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        switch (in.op) {
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelVertex) {
                vertex = true;
                vert_fn = w[2];
            }
            break;
        case OpConstant:
            if (in.len >= 4)
                const_val[w[2]] = w[3];
            break;
        case OpDecorate:
            if (in.len >= 4 && w[2] == DecorationDescriptorSet && w[3] == 3)
                set3_vars.push_back(w[1]);
            if (in.len >= 4 && w[2] == DecorationDescriptorSet && w[3] == 1)
                set1_vars.insert(w[1]);
            if (in.len >= 4 && w[2] == kDecorationBinding && w[3] == 0)
                bind0_vars.insert(w[1]);
            break;
        default:
            break;
        }
    }
    if (!vertex)
        return Kind::NotVertex;
    if (set3_vars.empty()) {
        if (!wide_camera)
            return Kind::NonWorld;
        // Every variable at (set 1, binding 0), not the first: X4 declares the
        // camera block once per stage and aliases two variables onto the one
        // binding. First-match here would read the fragment stage's variable
        // and miss the vertex stage's entirely -- take forty-eight's bug, which
        // has now cost this project three separate times.
        std::unordered_set<uint32_t> cam_vars;
        for (uint32_t v : set1_vars)
            if (bind0_vars.count(v))
                cam_vars.insert(v);
        if (cam_vars.empty())
            return Kind::NonWorld;
        bool in_vert = false;
        for (const Inst &in : insts) {
            const uint32_t *w = &code[in.start];
            if (in.op == OpFunction && in.len >= 3)
                in_vert = (w[2] == vert_fn);
            else if (in.op == OpFunctionEnd)
                in_vert = false;
            else if (in_vert && in.op == OpAccessChain && in.len >= 5 &&
                     cam_vars.count(w[3])) {
                auto it = const_val.find(w[4]); // first index = struct member
                if (it != const_val.end() &&
                    (it->second == 0 || it->second == 1 || it->second == 7 ||
                     it->second == 8))
                    return Kind::World;
            }
        }
        return Kind::NonWorld;
    }

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
    return Kind::NonWorld;
}

/// Does this module's vertex stage generate its own positions, with no vertex
/// data at all? Task #40.
///
/// A fullscreen post pass has no vertex attributes: it emits a triangle or
/// quad straight from `gl_VertexIndex` and covers the screen exactly. Any
/// transform on that quad leaves unrendered borders and samples texels that do
/// not correspond to the pixels it is writing, so it must be left alone even
/// when it is drawn in the same pass as the UI. `mod-0180` — the sun light
/// with cascaded shadows — is the canonical one.
///
/// **This is the discriminator #30 was reaching for and did not have.** Its
/// canvas keys on `World`, and got the right answer because every World module
/// has attributes, but "World" is a statement about a per-object matrix and
/// has nothing to do with whether a quad is procedural. The two disagree on
/// exactly five modules — `mod-0000`, `0228`, `0229`, `0397`, `0398`, all
/// NonWorld with real attributes — and those five are UI that #30 could not
/// move. Measured over all 409 dumps: 348 World-with-attributes, 41
/// NonWorld-procedural, 5 NonWorld-with-attributes, 0 World-procedural.
///
/// Attribution is by the **vertex entry point's interface list**, not by
/// storage class alone: X4 ships combined vertex+fragment modules, and a
/// fragment stage's inputs are Input-storage too. Reading those would call
/// every combined module attribute-driven.
inline bool is_procedural_fullscreen(const std::vector<uint32_t> &code) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    uint32_t vert_fn = 0;
    std::unordered_set<uint32_t> iface, builtins, input_vars;
    bool vertex = false;
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        switch (in.op) {
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelVertex && !vertex) {
                vertex = true;
                vert_fn = w[2];
                // words 3.. are the name (packed), then the interface ids.
                // The name's end is the word containing a zero byte; ids
                // follow. Rather than parse the string, collect every id-sized
                // word after it -- an over-collection here can only make the
                // module look MORE attribute-driven, which is the safe way to
                // be wrong: it leaves a procedural pass out of the canvas.
                uint32_t j = 3;
                while (j < in.len) {
                    const uint32_t v = w[j++];
                    if (((v >> 24) & 0xff) == 0 || ((v >> 16) & 0xff) == 0 ||
                        ((v >> 8) & 0xff) == 0 || (v & 0xff) == 0) {
                        break; // the word that terminates the name
                    }
                }
                for (; j < in.len; j++)
                    iface.insert(w[j]);
            }
            break;
        case OpDecorate:
            if (in.len >= 4 && w[2] == DecorationBuiltIn)
                builtins.insert(w[1]);
            break;
        case OpVariable:
            if (in.len >= 4 && w[3] == StorageClassInput)
                input_vars.insert(w[2]);
            break;
        default:
            break;
        }
    }
    if (!vertex || !vert_fn)
        return false;
    for (uint32_t v : input_vars)
        if (iface.count(v) && !builtins.count(v))
            return false; // real vertex data positions this draw
    return true;
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

// ---------------------------------------------------------------- task #35
//
// The ids `emit_off_axis` needs. Everything here is either a type or a value
// already materialised by the caller: the four coefficients arrive as ids
// rather than floats because in stereo each one is a per-view expression, not
// a constant.
struct OffAxisIds {
    uint32_t ax_num = 0, bx = 0, ay_num = 0, by = 0;
    uint32_t zero_f = 0, one_f = 0;
    uint32_t t_float = 0, t_v4 = 0, t_bool = 0;
};

/// `base + float(gl_ViewIndex)·diff`, or `base` alone when `diff` is 0 — the id
/// 0, meaning "the caller made no right-eye constant", not the value zero.
///
/// The same arithmetic selection patch_vertex_clip uses, for the same reasons:
/// it appends cleanly before each OpReturn with no basic-block surgery, it
/// avoids OpSelect's SPIR-V-1.4 rules for a vector result, and `float(view)` is
/// exact for 0 and 1 so this is a selection and not a blend.
///
/// `vf` is required to be a real id whenever `diff` is one; that holds by
/// construction in both callers, where a per-view constant is only ever built
/// under the same condition that builds `vf`.
inline uint32_t select_view(std::vector<uint32_t> &out, uint32_t &bound,
                           uint32_t t_float, uint32_t vf, uint32_t base,
                           uint32_t diff) {
    if (!diff)
        return base;
    const uint32_t scaled = bound++;
    emit(out, OpFMul, {t_float, scaled, vf, diff});
    const uint32_t sum = bound++;
    emit(out, OpFAdd, {t_float, sum, base, scaled});
    return sum;
}

/// Emits `apply_off_axis` (common/x4vr_view.hpp) into `out`, mapping X4's
/// symmetric frustum onto the runtime's canted one.
///
///     x' = (ax_num/sx)·x + bx·w        y' = (ay_num/sy)·y + by·w
///
/// `x_in` is clip x **after** the eye shear and `loaded` the position as X4
/// wrote it — the shear touches component 0 only, so y and w come from
/// `loaded` directly. That composition order is not a choice: the shear leaves
/// `x_c = sx·(x − d)`, so dividing by `sx` recovers the offset eye's own ray
/// and no eye-offset term belongs in the affine. See x4vr_view.hpp:363.
///
/// Returns the id of the corrected vec4.
///
/// **Shared, where the three scans below are deliberately duplicated.** That
/// duplication protects code already proven in the field from a refactor; this
/// is new in both callers at once, and the failure it guards against is the
/// opposite one — a sign fixed in one copy and left wrong in the other. This
/// project has already shipped exactly that defect once, fixing an
/// aggregate-across-cameras bug for sx and adding it for sy in the same commit.
///
/// The degenerate case follows the host function rather than improving on it:
/// a zero sx or sy leaves the position untouched instead of dividing by it.
/// X4's constant blocks read back all zeroes before the first real frame — the
/// layer's own camera dump skips them for that reason — and an inf in
/// gl_Position takes the whole primitive with it.
inline uint32_t emit_off_axis(std::vector<uint32_t> &out, uint32_t &bound,
                              const OffAxisIds &t, uint32_t sx, uint32_t sy,
                              uint32_t loaded, uint32_t x_in) {
    auto nid = [&bound] { return bound++; };
    const uint32_t y_in = nid();
    emit(out, OpCompositeExtract, {t.t_float, y_in, loaded, 1});
    const uint32_t w = nid();
    emit(out, OpCompositeExtract, {t.t_float, w, loaded, 3});

    // v·v > 0 rather than |v| > 0: OpFAbs is an OpExtInst, and requiring the
    // GLSL.std.450 import here would make this refuse modules that do not
    // import it. The squared form is also exactly how
    // patch_vertex_eye_offset_mvp already guards its own divide.
    auto nonzero = [&](uint32_t v) {
        const uint32_t sq = nid();
        emit(out, OpFMul, {t.t_float, sq, v, v});
        const uint32_t ok = nid();
        emit(out, OpFOrdGreaterThan, {t.t_bool, ok, sq, t.zero_f});
        return ok;
    };
    const uint32_t okx = nonzero(sx);
    const uint32_t oky = nonzero(sy);
    const uint32_t ok = nid();
    emit(out, OpLogicalAnd, {t.t_bool, ok, okx, oky});
    // Guarded denominators as well as a guarded result: the OpSelect below
    // discards the quotient, but computing inf and throwing it away relies on
    // the divide being well behaved, and this costs two instructions.
    const uint32_t denx = nid();
    emit(out, OpSelect, {t.t_float, denx, okx, sx, t.one_f});
    const uint32_t deny = nid();
    emit(out, OpSelect, {t.t_float, deny, oky, sy, t.one_f});

    auto axis = [&](uint32_t num, uint32_t den, uint32_t v, uint32_t b) {
        const uint32_t a = nid();
        emit(out, OpFDiv, {t.t_float, a, num, den});
        const uint32_t scaled = nid();
        emit(out, OpFMul, {t.t_float, scaled, a, v});
        const uint32_t shift = nid();
        emit(out, OpFMul, {t.t_float, shift, b, w});
        const uint32_t sum = nid();
        emit(out, OpFAdd, {t.t_float, sum, scaled, shift});
        return sum;
    };
    const uint32_t cx = axis(t.ax_num, denx, x_in, t.bx);
    const uint32_t cy = axis(t.ay_num, deny, y_in, t.by);
    const uint32_t nx = nid();
    emit(out, OpSelect, {t.t_float, nx, ok, cx, x_in});
    const uint32_t ny = nid();
    emit(out, OpSelect, {t.t_float, ny, ok, cy, y_in});

    const uint32_t p1 = nid();
    emit(out, OpCompositeInsert, {t.t_v4, p1, nx, loaded, 0});
    const uint32_t p2 = nid();
    emit(out, OpCompositeInsert, {t.t_v4, p2, ny, p1, 1});
    return p2;
}

/// Emits `compose_inv_off_axis` (common/x4vr_view.hpp): `M -> M·A⁻¹` on a
/// clip→view matrix, so a deferred pass reconstructing from `gl_FragCoord`
/// lands back in the frame X4 rendered rather than the one the affine moved
/// it to. Task #39.
///
/// `raw` is the matrix as X4 uploaded it. The live scales come out of that
/// same matrix — X4's projection has a diagonal top-left 2x2 in every dumped
/// take, so `minv[0][0] = 1/sx` and `minv[1][1] = 1/sy` — which spares a
/// second access chain and, more to the point, makes it impossible for the
/// scale used here to disagree with the matrix being corrected.
///
/// Every input is uniform over the draw, so the whole chain hoists into scalar
/// registers; the cost is a handful of SGPR ops per invocation, not per pixel.
///
/// Indices are `(column, component)`, SPIR-V's own matrix indexing and the
/// same convention `compose_inv_off_axis` is written in. Returns the id of the
/// corrected matrix; on a degenerate camera block the reciprocals come out as
/// 1 and the shifts as 0, so the result is `raw` unchanged — the host
/// function's all-or-nothing refusal, expressed without a branch.
inline uint32_t emit_inv_off_axis(std::vector<uint32_t> &out, uint32_t &bound,
                                  const OffAxisIds &t, uint32_t t_mat4,
                                  uint32_t raw) {
    auto nid = [&bound] { return bound++; };
    const uint32_t minv00 = nid();
    emit(out, OpCompositeExtract, {t.t_float, minv00, raw, 0, 0});
    const uint32_t minv11 = nid();
    emit(out, OpCompositeExtract, {t.t_float, minv11, raw, 1, 1});
    const uint32_t ax = nid();
    emit(out, OpFMul, {t.t_float, ax, t.ax_num, minv00});
    const uint32_t ay = nid();
    emit(out, OpFMul, {t.t_float, ay, t.ay_num, minv11});

    // v·v > 0, for the reason emit_off_axis gives: OpFAbs is an OpExtInst and
    // requiring GLSL.std.450 here would make the patch refuse modules that do
    // not import it.
    auto nonzero = [&](uint32_t v) {
        const uint32_t sq = nid();
        emit(out, OpFMul, {t.t_float, sq, v, v});
        const uint32_t okv = nid();
        emit(out, OpFOrdGreaterThan, {t.t_bool, okv, sq, t.zero_f});
        return okv;
    };
    const uint32_t ok = nid();
    emit(out, OpLogicalAnd, {t.t_bool, ok, nonzero(ax), nonzero(ay)});

    const uint32_t denx = nid();
    emit(out, OpSelect, {t.t_float, denx, ok, ax, t.one_f});
    const uint32_t deny = nid();
    emit(out, OpSelect, {t.t_float, deny, ok, ay, t.one_f});
    const uint32_t iax = nid();
    emit(out, OpFDiv, {t.t_float, iax, t.one_f, denx});
    const uint32_t iay = nid();
    emit(out, OpFDiv, {t.t_float, iay, t.one_f, deny});
    // The shifts have to vanish with the scales or a refusal would leave a
    // half-applied map, which is worse than either state.
    const uint32_t gbx = nid();
    emit(out, OpSelect, {t.t_float, gbx, ok, t.bx, t.zero_f});
    const uint32_t gby = nid();
    emit(out, OpSelect, {t.t_float, gby, ok, t.by, t.zero_f});

    // col0 /= A_x, col1 /= A_y, col3 -= B_x·col0' + B_y·col1'. The scaled
    // columns are what col3 takes its multiple of -- B/A, written as B times
    // the already-divided column.
    uint32_t acc = raw;
    for (uint32_t r = 0; r < 4; r++) {
        const uint32_t c0 = nid();
        emit(out, OpCompositeExtract, {t.t_float, c0, raw, 0, r});
        const uint32_t c1 = nid();
        emit(out, OpCompositeExtract, {t.t_float, c1, raw, 1, r});
        const uint32_t c3 = nid();
        emit(out, OpCompositeExtract, {t.t_float, c3, raw, 3, r});
        const uint32_t n0 = nid();
        emit(out, OpFMul, {t.t_float, n0, c0, iax});
        const uint32_t n1 = nid();
        emit(out, OpFMul, {t.t_float, n1, c1, iay});
        const uint32_t shx = nid();
        emit(out, OpFMul, {t.t_float, shx, gbx, n0});
        const uint32_t shy = nid();
        emit(out, OpFMul, {t.t_float, shy, gby, n1});
        const uint32_t sum = nid();
        emit(out, OpFAdd, {t.t_float, sum, shx, shy});
        const uint32_t n3 = nid();
        emit(out, OpFSub, {t.t_float, n3, c3, sum});
        const uint32_t a1 = nid();
        emit(out, OpCompositeInsert, {t_mat4, a1, n0, acc, 0, r});
        const uint32_t a2 = nid();
        emit(out, OpCompositeInsert, {t_mat4, a2, n1, a1, 1, r});
        const uint32_t a3 = nid();
        emit(out, OpCompositeInsert, {t_mat4, a3, n3, a2, 3, r});
        acc = a3;
    }
    return acc;
}

/// Rewrites `code` so every Vertex entry point ends with
///
///     gl_Position.x -= M_projection[0][0] * d
///
/// reading `sx` out of X4's camera uniform block at (`set`, `binding`),
/// member `member`, instead of baking it in.
///
/// **Why this replaces the baked matrix.** Take fifty-three measured X4's
/// projection through a session of ordinary play: `sx` ranged from 1.15 to
/// 37.75 as the player zoomed — a 33x spread, and at full zoom a baked
/// constant is 28x too small. There is no value that can be baked, because
/// the shear is baked at vkCreateShaderModule and X4 has no camera for
/// another twenty-six seconds.
///
/// **Why it is only one scalar.** X4's projection has `m[10] = 0`, so clip z
/// is the constant near plane for every vertex and the shear collapses:
///
///     x_c' = x_c + (-sx*d/near)*z_c = x_c - sx*d      (z_c == near)
///
/// `near` cancels. So this is one load, one multiply and one subtract, where
/// patch_vertex_clip does a full mat4 multiply — the correct version is
/// cheaper than the one it replaces. tests/view_math.cpp asserts the two
/// forms agree, against clip positions that came through P rather than
/// arbitrary vectors (on an arbitrary vector they legitimately differ).
///
/// `d` is the eye offset in metres, our own choice, so it stays a constant.
/// With `d_right` non-null the module reads `gl_ViewIndex` and selects
/// arithmetically, exactly as patch_vertex_clip does and for the same reason:
/// no basic-block surgery, and every op is core SPIR-V 1.0.
///
/// Returns false and leaves `code` untouched when the module has no camera
/// block at (set, binding) — 18 of X4's 341 world modules do not — so the
/// caller can fall back to the baked matrix rather than losing the shear.
///
/// The scan below deliberately repeats patch_vertex_clip's rather than
/// sharing it. That function is proven in the field and this one is new; a
/// refactor that broke both at once is the expensive mistake here, and the
/// duplication is the cheap one.
///
/// **Task #35.** With `oa_left` non-null the shear is followed by the off-axis
/// affine, which needs `sy` as well and so reads `M_projection[1][1]` beside
/// the `[0][0]` it already reads. `oa_right` makes those coefficients per-view
/// exactly as `d_right` makes the offset per-view, and the two are independent:
/// the runtime's frusta can differ between eyes for reasons that have nothing
/// to do with the IPD.
///
/// With both null the emitted words are **byte-identical** to what this
/// function produced before #35 existed, which is what lets every tagged state
/// before it be reproduced by leaving the knob unset. tests/run-multiview-
/// render.sh asserts that identity rather than trusting it.
///
/// Refuses a non-`ok` OffAxis rather than quietly skipping the affine. A
/// degenerate frustum reaching here is a caller bug, and a module that renders
/// symmetric while the layer believes it is canted is the kind of failure that
/// looks like a headset problem for a week.
inline bool patch_vertex_eye_offset(std::vector<uint32_t> &code, uint32_t set,
                                    uint32_t binding, uint32_t member,
                                    float d_left,
                                    const float *d_right = nullptr,
                                    const OffAxis *oa_left = nullptr,
                                    const OffAxis *oa_right = nullptr) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    uint32_t bound = code[3];
    auto new_id = [&bound] { return bound++; };

    uint32_t entry_fn = 0;
    std::unordered_map<uint32_t, uint32_t> struct_pos_member;
    std::vector<uint32_t> var_pos_direct;
    struct PtrType { uint32_t storage, pointee; };
    std::unordered_map<uint32_t, PtrType> ptr_types;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vars;
    std::unordered_map<uint32_t, std::vector<uint32_t>> struct_members;
    std::unordered_map<uint32_t, uint32_t> var_set, var_binding;
    uint32_t t_float = 0, t_v4 = 0, t_mat4 = 0, t_int = 0, t_bool = 0;
    uint32_t ptr_out_v4 = 0, ptr_uniform_float = 0;
    size_t first_fn = 0;
    bool have_first_fn = false;
    size_t last_annotation_end = 0;
    size_t first_global = 0;
    bool have_first_global = false;
    bool has_multiview_cap = false;
    bool has_multiview_ext = false;
    size_t caps_end = 0, exts_end = 0;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
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
            if (in.len >= 4 && w[2] == DecorationDescriptorSet)
                var_set[w[1]] = w[3];
            if (in.len >= 4 && w[2] == DecorationBinding)
                var_binding[w[1]] = w[3];
            break;
        case OpTypeFloat:
            if (in.len >= 3 && w[2] == 32 && !t_float)
                t_float = w[1];
            break;
        case OpTypeInt:
            if (in.len >= 4 && w[2] == 32 && w[3] == 1 && !t_int)
                t_int = w[1];
            break;
        case OpTypeBool:
            if (in.len >= 2 && !t_bool)
                t_bool = w[1];
            break;
        case OpTypeVector:
            if (in.len >= 4 && w[2] == t_float && w[3] == 4 && !t_v4)
                t_v4 = w[1];
            break;
        case OpTypeMatrix:
            if (in.len >= 4 && w[2] == t_v4 && w[3] == 4 && !t_mat4)
                t_mat4 = w[1];
            break;
        case OpTypeStruct:
            if (in.len >= 2) {
                std::vector<uint32_t> m;
                for (uint32_t j = 2; j < in.len; j++)
                    m.push_back(w[j]);
                struct_members[w[1]] = std::move(m);
            }
            break;
        case OpTypePointer:
            if (in.len >= 4) {
                ptr_types[w[1]] = {w[2], w[3]};
                if (w[2] == StorageClassOutput && w[3] == t_v4 && !ptr_out_v4)
                    ptr_out_v4 = w[1];
                if (w[2] == StorageClassUniform && w[3] == t_float &&
                    !ptr_uniform_float)
                    ptr_uniform_float = w[1];
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

    if (!entry_fn || !t_float || !t_v4 || !t_mat4 || !have_first_fn)
        return false;
    // Task #35. Checked before anything is emitted, so a refusal leaves the
    // module untouched the way every other refusal here does.
    if ((oa_left && !oa_left->ok) || (oa_right && !oa_right->ok))
        return false;
    if (oa_right && !oa_left)
        return false; // a right-eye map with no left is a caller error

    // --- locate the camera block --------------------------------------
    // Matched by (set, binding) and then *verified* by shape: the variable
    // must be a Uniform pointing at a struct whose member `member` is a mat4.
    // Matching on the decoration alone would happily patch whatever else a
    // future X4 build put at set 1 binding 0, and produce a shader that
    // validates while reading nonsense.
    uint32_t cam_var = 0;
    for (auto &[id, ts] : vars) {
        if (ts.second != StorageClassUniform)
            continue;
        auto s = var_set.find(id), b = var_binding.find(id);
        if (s == var_set.end() || b == var_binding.end())
            continue;
        if (s->second != set || b->second != binding)
            continue;
        auto pt = ptr_types.find(ts.first);
        if (pt == ptr_types.end())
            continue;
        auto sm = struct_members.find(pt->second.pointee);
        if (sm == struct_members.end() || member >= sm->second.size())
            continue;
        if (sm->second[member] != t_mat4)
            continue;
        cam_var = id;
        break;
    }
    if (!cam_var)
        return false; // caller falls back to the baked matrix

    // --- locate gl_Position -------------------------------------------
    uint32_t pos_var = 0, pos_member = 0;
    bool via_struct = false;
    for (auto &[id, ts] : vars) {
        if (ts.second != StorageClassOutput)
            continue;
        auto pt = ptr_types.find(ts.first);
        if (pt == ptr_types.end())
            continue;
        auto sm = struct_pos_member.find(pt->second.pointee);
        if (sm != struct_pos_member.end()) {
            pos_var = id;
            pos_member = sm->second;
            via_struct = true;
            break;
        }
    }
    if (!pos_var)
        for (uint32_t v : var_pos_direct)
            if (vars.count(v) && vars[v].second == StorageClassOutput) {
                pos_var = v;
                via_struct = false;
                break;
            }
    if (!pos_var)
        return false;

    // --- declarations --------------------------------------------------
    std::vector<uint32_t> decls;
    if (!ptr_out_v4) {
        ptr_out_v4 = new_id();
        emit(decls, OpTypePointer, {ptr_out_v4, StorageClassOutput, t_v4});
    }
    if (!ptr_uniform_float) {
        ptr_uniform_float = new_id();
        emit(decls, OpTypePointer,
             {ptr_uniform_float, StorageClassUniform, t_float});
    }
    if (!t_int) {
        t_int = new_id();
        emit(decls, OpTypeInt, {t_int, 32, 1});
    }
    auto iconst = [&](uint32_t v) {
        const uint32_t id = new_id();
        emit(decls, OpConstant, {t_int, id, v});
        return id;
    };
    auto fconst = [&](float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        const uint32_t id = new_id();
        emit(decls, OpConstant, {t_float, id, bits});
        return id;
    };
    const uint32_t c_member = iconst(member);
    const uint32_t c_zero = iconst(0);
    const uint32_t const_member_idx = via_struct ? iconst(pos_member) : 0;
    const uint32_t c_dl = fconst(d_left);

    const bool stereo = d_right != nullptr;
    // gl_ViewIndex is needed by whichever of the two is per-view, and the two
    // are independent: a runtime can hand back canted frusta with the eye
    // offset unchanged, or the reverse.
    const bool oa_on = oa_left != nullptr;
    const bool oa_stereo = oa_right != nullptr;
    const bool need_view = stereo || oa_stereo;
    uint32_t c_ddiff = 0, view_var = 0;
    if (stereo)
        c_ddiff = fconst(*d_right - d_left);
    if (need_view) {
        const uint32_t ptr_in_int = new_id();
        emit(decls, OpTypePointer, {ptr_in_int, StorageClassInput, t_int});
        view_var = new_id();
        emit(decls, OpVariable, {ptr_in_int, view_var, StorageClassInput});
    }
    // Nothing below runs unless #35 is asked for, which is what keeps the
    // emitted words identical to the pre-#35 ones when it is not.
    OffAxisIds oa{};
    uint32_t c_one = 0, c_oa[4] = {}, c_oad[4] = {};
    if (oa_on) {
        if (!t_bool) {
            t_bool = new_id();
            emit(decls, OpTypeBool, {t_bool});
        }
        oa.t_float = t_float;
        oa.t_v4 = t_v4;
        oa.t_bool = t_bool;
        oa.zero_f = fconst(0.0f);
        oa.one_f = fconst(1.0f);
        c_one = iconst(1);
        const float l[4] = {oa_left->ax_num, oa_left->bx, oa_left->ay_num,
                            oa_left->by};
        for (int i = 0; i < 4; i++)
            c_oa[i] = fconst(l[i]);
        if (oa_stereo) {
            const float r[4] = {oa_right->ax_num, oa_right->bx,
                                oa_right->ay_num, oa_right->by};
            for (int i = 0; i < 4; i++)
                c_oad[i] = fconst(r[i] - l[i]);
        }
    }

    const size_t anno_at = last_annotation_end
                               ? last_annotation_end
                               : (have_first_global ? first_global : first_fn);

    std::vector<uint32_t> out;
    out.reserve(code.size() + decls.size() + 64);
    out.insert(out.end(), code.begin(), code.begin() + 5);

    const size_t cap_at = caps_end ? caps_end : 5;
    const size_t ext_at = exts_end ? exts_end : cap_at;

    bool in_entry = false;
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (need_view && !has_multiview_cap && in.start == cap_at)
            emit(out, OpCapability, {CapabilityMultiView});
        if (need_view && !has_multiview_ext && code[1] < 0x00010300u &&
            in.start == ext_at) {
            static const uint32_t kExt[] = {0x5f565053u, 0x5f52484bu,
                                            0x746c756du, 0x65697669u,
                                            0x00000077u};
            out.push_back((uint32_t)((1 + 5) << 16) | OpExtension);
            for (uint32_t x : kExt)
                out.push_back(x);
        }
        if (need_view && in.start == anno_at)
            emit(out, OpDecorate,
                 {view_var, DecorationBuiltIn, BuiltInViewIndex});
        if (in.start == first_fn)
            out.insert(out.end(), decls.begin(), decls.end());

        if (in.op == OpFunction && in.len >= 3 && w[2] == entry_fn)
            in_entry = true;

        if (in_entry && in.op == OpReturn) {
            // sx = M_projection[0][0]
            //
            // The access chain indexes the *logical* matrix, so column 0
            // component 0 is P[0][0] whether the block is ColMajor or
            // RowMajor -- the decoration describes memory, not indexing. X4
            // declares ColMajor, which is also what the host-side read in
            // x4vr_view.hpp detects, so the two agree; this note is here
            // because that agreement is worth not having to re-derive.
            const uint32_t p_sx = new_id();
            emit(out, OpAccessChain,
                 {ptr_uniform_float, p_sx, cam_var, c_member, c_zero, c_zero});
            const uint32_t sx = new_id();
            emit(out, OpLoad, {t_float, sx, p_sx});

            // float(gl_ViewIndex), loaded once: the eye offset and the #35
            // coefficients both select on it, and emitting it twice would
            // leave two selections free to disagree.
            uint32_t vf = 0;
            if (need_view) {
                const uint32_t vi = new_id();
                emit(out, OpLoad, {t_int, vi, view_var});
                vf = new_id();
                emit(out, OpConvertSToF, {t_float, vf, vi});
            }

            // d = d_left + float(gl_ViewIndex) * (d_right - d_left)
            uint32_t d = c_dl;
            if (stereo) {
                const uint32_t scaled = new_id();
                emit(out, OpFMul, {t_float, scaled, vf, c_ddiff});
                d = new_id();
                emit(out, OpFAdd, {t_float, d, c_dl, scaled});
            }

            const uint32_t delta = new_id();
            emit(out, OpFMul, {t_float, delta, sx, d});

            const uint32_t ptr = via_struct ? new_id() : pos_var;
            if (via_struct)
                emit(out, OpAccessChain,
                     {ptr_out_v4, ptr, pos_var, const_member_idx});
            const uint32_t loaded = new_id();
            emit(out, OpLoad, {t_v4, loaded, ptr});
            const uint32_t x = new_id();
            emit(out, OpCompositeExtract, {t_float, x, loaded, 0});
            const uint32_t nx = new_id();
            emit(out, OpFSub, {t_float, nx, x, delta});

            uint32_t np;
            if (oa_on) {
                OffAxisIds t = oa;
                uint32_t *dst[4] = {&t.ax_num, &t.bx, &t.ay_num, &t.by};
                for (int i = 0; i < 4; i++)
                    *dst[i] = select_view(out, bound, t_float, vf, c_oa[i],
                                          c_oad[i]);
                // sy = M_projection[1][1], the vertical counterpart of the
                // read above and indexed the same logical way.
                const uint32_t p_sy = new_id();
                emit(out, OpAccessChain, {ptr_uniform_float, p_sy, cam_var,
                                          c_member, c_one, c_one});
                const uint32_t sy = new_id();
                emit(out, OpLoad, {t_float, sy, p_sy});
                np = emit_off_axis(out, bound, t, sx, sy, loaded, nx);
            } else {
                np = new_id();
                emit(out, OpCompositeInsert, {t_v4, np, nx, loaded, 0});
            }
            emit(out, OpStore, {ptr, np});
        }

        // gl_ViewIndex is an Input and belongs in the interface list. The
        // camera block deliberately does *not*: every X4 module is SPIR-V 1.0
        // or 1.2, and before 1.4 the interface is limited to Input and Output
        // storage classes, so listing a Uniform there is the violation rather
        // than omitting it.
        if (need_view && in.op == OpEntryPoint && in.len >= 3 &&
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

    out[3] = bound;
    code.swap(out);
    return true;
}

/// The eye offset for the modules that cannot see the camera — task #23.
///
/// Twelve of X4's World vertex modules declare exactly one descriptor, the
/// set-3 per-object block, and no camera block in either stage. Four of them
/// (mod 26, 134, 146 and 158 in take 74's dump) draw into `rp #13`, `#16` and
/// `#23`, all STEREO world passes; two draw only into the depth-only shadow
/// cascades the layer excludes anyway, and six are the variant X4 compiles and
/// never binds. So `patch_vertex_eye_offset` refuses them, they fall back to
/// `patch_vertex_clip` with a baked `sx`, and under zoom that constant is wrong
/// by up to the full range the scene camera covers — 0.75405 to 29.18689.
///
/// **sx is not stored where they can see it, and is recoverable anyway.** With
/// X4's projection (`row0(P) = [sx 0 0 0]`, `row3(P) = [0 0 1 0]`, `m[10] = 0`),
/// for `MVP = P·V·W`:
///
///     row0(MVP) = sx * row0(VW)          row3(MVP) = row2(VW)
///
/// A rigid view and an object matrix that is rigid times a **uniform** scale
/// `s` give both rows a linear part of magnitude `s`, so
///
///     sx = |row0(MVP).xyz| / |row3(MVP).xyz| = (sx*s)/s
///
/// exactly, with the object's own scale cancelling. `tests/view_math.cpp`
/// checks this against every `sx` this project has measured, both an identity
/// and a rigid non-identity view, and object scales from 0.01 to 3000: worst
/// relative error 2.4e-07.
///
/// **The assumption is non-uniform scale**, and the test pins its cost rather
/// than calling it rare: a 2x scale on x alone reports exactly `2*sx`. Nothing
/// in the module can detect that cheaply, so it is accepted and recorded.
///
/// **Why not a push constant.** It would be the textbook way to hand `sx` in,
/// and it would require adding a range to a `VkPipelineLayout` the layer does
/// not own. Layout compatibility is defined by the push-constant ranges as well
/// as the set layouts, so a range added here can invalidate descriptor bindings
/// for pipelines sharing that layout. Reading a block the module already binds
/// costs one divide and one square root and changes no interface at all.
///
/// Refuses — returns false, `code` untouched — when there is no block of the
/// right shape at (set, binding), when its `member` is not a mat4, or when the
/// module does not already import GLSL.std.450. The caller then falls back to
/// the baked matrix exactly as before, which is the behaviour this replaces
/// rather than a new failure mode.
///
/// The scan repeats `patch_vertex_eye_offset`'s rather than sharing it, for the
/// same reason that one repeats `patch_vertex_clip`'s: those two are proven in
/// the field, this one is new, and a refactor that broke all three at once is
/// the expensive mistake here.
///
/// **Task #35, and the one place it is not simply the same edit.** The affine
/// needs `sy`, and the recovery above yields a *magnitude*: `row1(MVP) =
/// sy·row1(VW)` gives `|sy| = |row1(MVP).xyz| / |row3(MVP).xyz|`, and no
/// quantity in the block carries the sign. It is supplied, as `sy = −|sy|`,
/// from measurement: across every log this project has kept, 35,783 sampled
/// camera blocks report a negative `sy` and not one reports a positive — the
/// Y-flip that Vulkan's downward NDC y requires.
///
/// That is the *same* convention `make_off_axis`'s negative `ay_num` is derived
/// from (x4vr_view.hpp:305). So this does not add a failure mode of its own: if
/// X4 ever flipped the convention, the camera-block path would be wrong in
/// exactly the same way, and every module would flip together rather than these
/// twelve alone. It is named here because an assumption only one path spells
/// out is the one that gets forgotten. The layer logs a signed `sy` on every
/// `proj CAMERA` line and warns once if it ever sees a non-negative one, which
/// is what makes this checkable rather than merely stated.
///
/// Without #35 requested, the emitted words are byte-identical to the pre-#35
/// ones, as in `patch_vertex_eye_offset`.
inline bool patch_vertex_eye_offset_mvp(std::vector<uint32_t> &code,
                                        uint32_t set, uint32_t binding,
                                        uint32_t member, float d_left,
                                        const float *d_right = nullptr,
                                        const OffAxis *oa_left = nullptr,
                                        const OffAxis *oa_right = nullptr) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    uint32_t bound = code[3];
    auto new_id = [&bound] { return bound++; };

    uint32_t entry_fn = 0, glsl_ext = 0;
    std::unordered_map<uint32_t, uint32_t> struct_pos_member;
    std::vector<uint32_t> var_pos_direct;
    struct PtrType { uint32_t storage, pointee; };
    std::unordered_map<uint32_t, PtrType> ptr_types;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vars;
    std::unordered_map<uint32_t, std::vector<uint32_t>> struct_members;
    std::unordered_map<uint32_t, uint32_t> var_set, var_binding;
    uint32_t t_float = 0, t_v4 = 0, t_mat4 = 0, t_int = 0, t_bool = 0;
    uint32_t ptr_out_v4 = 0, ptr_uniform_float = 0;
    size_t first_fn = 0;
    bool have_first_fn = false;
    size_t last_annotation_end = 0;
    size_t first_global = 0;
    bool have_first_global = false;
    bool has_multiview_cap = false;
    bool has_multiview_ext = false;
    size_t caps_end = 0, exts_end = 0;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
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
        case OpExtInstImport:
            // "GLSL.std.450", packed the same way the SPV_KHR_multiview check
            // above packs its name. Matched rather than assumed: emitting an
            // OpExtInst against the wrong set is a module that validates and
            // computes something else.
            if (in.len >= 6 && w[2] == 0x4c534c47u && w[3] == 0x6474732eu &&
                w[4] == 0x3035342eu && w[5] == 0x00000000u)
                glsl_ext = w[1];
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
            if (in.len >= 4 && w[2] == DecorationDescriptorSet)
                var_set[w[1]] = w[3];
            if (in.len >= 4 && w[2] == DecorationBinding)
                var_binding[w[1]] = w[3];
            break;
        case OpTypeFloat:
            if (in.len >= 3 && w[2] == 32 && !t_float)
                t_float = w[1];
            break;
        case OpTypeInt:
            if (in.len >= 4 && w[2] == 32 && w[3] == 1 && !t_int)
                t_int = w[1];
            break;
        case OpTypeBool:
            if (in.len >= 2 && !t_bool)
                t_bool = w[1];
            break;
        case OpTypeVector:
            if (in.len >= 4 && w[2] == t_float && w[3] == 4 && !t_v4)
                t_v4 = w[1];
            break;
        case OpTypeMatrix:
            if (in.len >= 4 && w[2] == t_v4 && w[3] == 4 && !t_mat4)
                t_mat4 = w[1];
            break;
        case OpTypeStruct:
            if (in.len >= 2) {
                std::vector<uint32_t> m;
                for (uint32_t j = 2; j < in.len; j++)
                    m.push_back(w[j]);
                struct_members[w[1]] = std::move(m);
            }
            break;
        case OpTypePointer:
            if (in.len >= 4) {
                ptr_types[w[1]] = {w[2], w[3]};
                if (w[2] == StorageClassOutput && w[3] == t_v4 && !ptr_out_v4)
                    ptr_out_v4 = w[1];
                if (w[2] == StorageClassUniform && w[3] == t_float &&
                    !ptr_uniform_float)
                    ptr_uniform_float = w[1];
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

    if (!entry_fn || !t_float || !t_v4 || !t_mat4 || !have_first_fn)
        return false;
    if (!glsl_ext)
        return false; // no sqrt available without adding an import; refuse
    // Task #35, checked before anything is emitted -- see the twin above.
    if ((oa_left && !oa_left->ok) || (oa_right && !oa_right->ok))
        return false;
    if (oa_right && !oa_left)
        return false; // a right-eye map with no left is a caller error

    // --- locate the per-object block, verified by shape ------------------
    uint32_t obj_var = 0;
    for (auto &[id, ts] : vars) {
        if (ts.second != StorageClassUniform)
            continue;
        auto s = var_set.find(id), b = var_binding.find(id);
        if (s == var_set.end() || b == var_binding.end())
            continue;
        if (s->second != set || b->second != binding)
            continue;
        auto pt = ptr_types.find(ts.first);
        if (pt == ptr_types.end())
            continue;
        auto sm = struct_members.find(pt->second.pointee);
        if (sm == struct_members.end() || member >= sm->second.size())
            continue;
        if (sm->second[member] != t_mat4)
            continue;
        obj_var = id;
        break;
    }
    if (!obj_var)
        return false;

    // --- locate gl_Position ---------------------------------------------
    uint32_t pos_var = 0, pos_member = 0;
    bool via_struct = false;
    for (auto &[id, ts] : vars) {
        if (ts.second != StorageClassOutput)
            continue;
        auto pt = ptr_types.find(ts.first);
        if (pt == ptr_types.end())
            continue;
        auto sm = struct_pos_member.find(pt->second.pointee);
        if (sm != struct_pos_member.end()) {
            pos_var = id;
            pos_member = sm->second;
            via_struct = true;
            break;
        }
    }
    if (!pos_var)
        for (uint32_t v : var_pos_direct)
            if (vars.count(v) && vars[v].second == StorageClassOutput) {
                pos_var = v;
                via_struct = false;
                break;
            }
    if (!pos_var)
        return false;

    // --- declarations ----------------------------------------------------
    std::vector<uint32_t> decls;
    if (!ptr_out_v4) {
        ptr_out_v4 = new_id();
        emit(decls, OpTypePointer, {ptr_out_v4, StorageClassOutput, t_v4});
    }
    if (!ptr_uniform_float) {
        ptr_uniform_float = new_id();
        emit(decls, OpTypePointer,
             {ptr_uniform_float, StorageClassUniform, t_float});
    }
    if (!t_int) {
        t_int = new_id();
        emit(decls, OpTypeInt, {t_int, 32, 1});
    }
    if (!t_bool) {
        t_bool = new_id();
        emit(decls, OpTypeBool, {t_bool});
    }
    auto iconst = [&](uint32_t v) {
        const uint32_t id = new_id();
        emit(decls, OpConstant, {t_int, id, v});
        return id;
    };
    auto fconst = [&](float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        const uint32_t id = new_id();
        emit(decls, OpConstant, {t_float, id, bits});
        return id;
    };
    const uint32_t c_member = iconst(member);
    const uint32_t c_col[3] = {iconst(0), iconst(1), iconst(2)};
    const uint32_t c_row0 = c_col[0];
    const uint32_t c_row3 = iconst(3);
    const uint32_t const_member_idx = via_struct ? iconst(pos_member) : 0;
    const uint32_t c_zero_f = fconst(0.0f);
    const uint32_t c_one_f = fconst(1.0f);
    const uint32_t c_dl = fconst(d_left);

    const bool stereo = d_right != nullptr;
    const bool oa_on = oa_left != nullptr;
    const bool oa_stereo = oa_right != nullptr;
    const bool need_view = stereo || oa_stereo;
    uint32_t c_ddiff = 0, view_var = 0;
    if (stereo)
        c_ddiff = fconst(*d_right - d_left);
    if (need_view) {
        const uint32_t ptr_in_int = new_id();
        emit(decls, OpTypePointer, {ptr_in_int, StorageClassInput, t_int});
        view_var = new_id();
        emit(decls, OpVariable, {ptr_in_int, view_var, StorageClassInput});
    }
    // As in the twin: nothing here runs unless #35 is asked for. c_zero_f and
    // c_one_f already exist above -- the sqrt guard needs the same two -- so
    // the affine reuses them rather than declaring a second pair.
    OffAxisIds oa{};
    uint32_t c_oa[4] = {}, c_oad[4] = {};
    if (oa_on) {
        oa.t_float = t_float;
        oa.t_v4 = t_v4;
        oa.t_bool = t_bool;
        oa.zero_f = c_zero_f;
        oa.one_f = c_one_f;
        const float l[4] = {oa_left->ax_num, oa_left->bx, oa_left->ay_num,
                            oa_left->by};
        for (int i = 0; i < 4; i++)
            c_oa[i] = fconst(l[i]);
        if (oa_stereo) {
            const float r[4] = {oa_right->ax_num, oa_right->bx,
                                oa_right->ay_num, oa_right->by};
            for (int i = 0; i < 4; i++)
                c_oad[i] = fconst(r[i] - l[i]);
        }
    }

    const size_t anno_at = last_annotation_end
                               ? last_annotation_end
                               : (have_first_global ? first_global : first_fn);

    std::vector<uint32_t> out;
    out.reserve(code.size() + decls.size() + 96);
    out.insert(out.end(), code.begin(), code.begin() + 5);

    const size_t cap_at = caps_end ? caps_end : 5;
    const size_t ext_at = exts_end ? exts_end : cap_at;

    bool in_entry = false;
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (need_view && !has_multiview_cap && in.start == cap_at)
            emit(out, OpCapability, {CapabilityMultiView});
        if (need_view && !has_multiview_ext && code[1] < 0x00010300u &&
            in.start == ext_at) {
            static const uint32_t kExt[] = {0x5f565053u, 0x5f52484bu,
                                            0x746c756du, 0x65697669u,
                                            0x00000077u};
            out.push_back((uint32_t)((1 + 5) << 16) | OpExtension);
            for (uint32_t x : kExt)
                out.push_back(x);
        }
        if (need_view && in.start == anno_at)
            emit(out, OpDecorate,
                 {view_var, DecorationBuiltIn, BuiltInViewIndex});
        if (in.start == first_fn)
            out.insert(out.end(), decls.begin(), decls.end());

        // The vertex entry only. These are combined vertex+fragment modules --
        // that is most of why they exist as a separate case at all -- so
        // keying on the function id rather than on "inside any function" is
        // load-bearing, not tidiness.
        if (in.op == OpFunction && in.len >= 3 && w[2] == entry_fn)
            in_entry = true;

        if (in_entry && in.op == OpReturn) {
            // n0 = |row0(MVP).xyz|^2, n3 = |row3(MVP).xyz|^2.
            //
            // The access chain indexes the logical matrix: {member, c, r} is
            // element (row r, column c), whichever way the block is decorated,
            // because ColMajor/RowMajor describe memory and not indexing. X4
            // declares ColMajor here, matching the host-side read.
            auto elem = [&](uint32_t col, uint32_t row) {
                const uint32_t p = new_id();
                emit(out, OpAccessChain,
                     {ptr_uniform_float, p, obj_var, c_member, col, row});
                const uint32_t v = new_id();
                emit(out, OpLoad, {t_float, v, p});
                return v;
            };
            auto sq_len3 = [&](uint32_t row) {
                uint32_t acc = 0;
                for (int i = 0; i < 3; i++) {
                    const uint32_t e = elem(c_col[i], row);
                    const uint32_t s = new_id();
                    emit(out, OpFMul, {t_float, s, e, e});
                    if (!acc) {
                        acc = s;
                    } else {
                        const uint32_t a = new_id();
                        emit(out, OpFAdd, {t_float, a, acc, s});
                        acc = a;
                    }
                }
                return acc;
            };
            const uint32_t n0 = sq_len3(c_row0);
            const uint32_t n3 = sq_len3(c_row3);
            // row 1 for |sy|, by the same argument that gives |sx| from row 0.
            // Emitted after n3 so that with #35 unrequested the instruction
            // stream is exactly what it was before.
            const uint32_t n1 = oa_on ? sq_len3(c_col[1]) : 0;

            // sx = n3 > 0 ? sqrt(n0/n3) : 0, written exactly as the host-side
            // `recover` in tests/view_math.cpp so the two cannot drift. The
            // guarded denominator is what keeps an unpopulated block -- all
            // zeroes, which is what X4's blocks read as before the first real
            // frame -- from putting a NaN into gl_Position.x and taking the
            // whole vertex with it.
            const uint32_t ok = new_id();
            emit(out, OpFOrdGreaterThan, {t_bool, ok, n3, c_zero_f});
            const uint32_t den = new_id();
            emit(out, OpSelect, {t_float, den, ok, n3, c_one_f});
            const uint32_t q = new_id();
            emit(out, OpFDiv, {t_float, q, n0, den});
            const uint32_t root = new_id();
            emit(out, OpExtInst,
                 {t_float, root, glsl_ext, GLSLstd450Sqrt, q});
            const uint32_t sx = new_id();
            emit(out, OpSelect, {t_float, sx, ok, root, c_zero_f});

            // sy = -sqrt(n1/n3), guarded by the same `ok`. The magnitude comes
            // out of the block; the sign does not exist there and is X4's
            // measured Y-flip -- see this function's doc comment, which is
            // where the 35,783 samples behind that minus are recorded.
            uint32_t sy = 0;
            if (oa_on) {
                const uint32_t qy = new_id();
                emit(out, OpFDiv, {t_float, qy, n1, den});
                const uint32_t rooty = new_id();
                emit(out, OpExtInst,
                     {t_float, rooty, glsl_ext, GLSLstd450Sqrt, qy});
                const uint32_t negy = new_id();
                emit(out, OpFNegate, {t_float, negy, rooty});
                sy = new_id();
                emit(out, OpSelect, {t_float, sy, ok, negy, c_zero_f});
            }

            // float(gl_ViewIndex), loaded once and shared -- see the twin.
            uint32_t vf = 0;
            if (need_view) {
                const uint32_t vi = new_id();
                emit(out, OpLoad, {t_int, vi, view_var});
                vf = new_id();
                emit(out, OpConvertSToF, {t_float, vf, vi});
            }

            // d = d_left + float(gl_ViewIndex) * (d_right - d_left)
            uint32_t d = c_dl;
            if (stereo) {
                const uint32_t scaled = new_id();
                emit(out, OpFMul, {t_float, scaled, vf, c_ddiff});
                d = new_id();
                emit(out, OpFAdd, {t_float, d, c_dl, scaled});
            }

            const uint32_t delta = new_id();
            emit(out, OpFMul, {t_float, delta, sx, d});

            const uint32_t ptr = via_struct ? new_id() : pos_var;
            if (via_struct)
                emit(out, OpAccessChain,
                     {ptr_out_v4, ptr, pos_var, const_member_idx});
            const uint32_t loaded = new_id();
            emit(out, OpLoad, {t_v4, loaded, ptr});
            const uint32_t x = new_id();
            emit(out, OpCompositeExtract, {t_float, x, loaded, 0});
            const uint32_t nx = new_id();
            emit(out, OpFSub, {t_float, nx, x, delta});

            uint32_t np;
            if (oa_on) {
                OffAxisIds t = oa;
                uint32_t *dst[4] = {&t.ax_num, &t.bx, &t.ay_num, &t.by};
                for (int i = 0; i < 4; i++)
                    *dst[i] = select_view(out, bound, t_float, vf, c_oa[i],
                                          c_oad[i]);
                np = emit_off_axis(out, bound, t, sx, sy, loaded, nx);
            } else {
                np = new_id();
                emit(out, OpCompositeInsert, {t_v4, np, nx, loaded, 0});
            }
            emit(out, OpStore, {ptr, np});
        }

        if (need_view && in.op == OpEntryPoint && in.len >= 3 &&
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

    out[3] = bound;
    code.swap(out);
    return true;
}

/// Corrects `M_invprojection` per eye, so deferred passes reconstruct position
/// in the **centre** frame rather than the eye's.
///
/// **The bug this fixes.** The eye shear moves `gl_Position` only, so geometry
/// rasterizes where the offset eye would see it — correct. But the deferred
/// passes then read the depth buffer at a screen pixel and reconstruct view
/// position with the centre-frame `M_invprojection`, which recovers the
/// position *in that eye's frame*, and light it with shadow matrices and light
/// positions that are still centre-frame. Surface and shadow disagree by the
/// eye offset: 64 mm between the two eyes at a 64 mm IPD, which on cockpit
/// geometry is a plainly visible shadow displacement.
///
/// Shadows are view-independent — a shadow edge stays on the same surface
/// point in both eyes — so this is a defect and not parallax. Task #22 was
/// closed once on the grounds that the difference scaled with the IPD; so does
/// this, which is why that evidence never discriminated.
///
/// **The correction.** `clip_sheared = K·clip_centre` with `K = P·T(−d)·P⁻¹`,
/// so recovering the centre frame needs `T(d)·M_invprojection`. For a
/// column-major matrix that is one row-combine:
///
///     result[0][c] = M[0][c] + d · M[3][c]        c = 0..3
///
/// applied where the matrix is *loaded*. Locating "the reconstructed position"
/// in arbitrary shader code is not tractable; locating a load of member
/// `member` is.
///
/// SSA is preserved by giving the load a fresh id and letting the final
/// `OpCompositeInsert` take over the original result id, so every existing use
/// downstream sees the corrected matrix with no rewriting of uses.
///
/// **Fragment only.** 16 of the 19 modules that reconstruct this way are
/// fragment; the other 3 are compute, where `gl_ViewIndex` does not exist at
/// all. That is the project's already-recorded compute gap, not something this
/// patch can close, and it refuses those modules rather than pretending.
///
/// **Task #39 — the off-axis affine, undone.** Once #35 maps X4's symmetric
/// frustum onto the runtime's canted ones, `gl_Position` no longer lands where
/// `M_invprojection` expects, and the error reverses sign between the eyes
/// because `bx` does. Passing `oa_left`/`oa_right` composes `A⁻¹` onto the
/// right of the matrix, giving `T(d)·M_invprojection·A⁻¹` — the shear undone
/// last, the affine undone first. Both are constant compositions on a matrix
/// that is already being rewritten, so this adds no load and no descriptor.
///
/// Left null, not one word changes: the emitted module is byte-identical to
/// the pre-#39 one, which is what keeps take 163's behaviour available.
///
/// **The forward direction deliberately stays unpatched.** Ten fragment and
/// two compute modules also read `M_projection`. Eight of them project a
/// reconstructed view position back to NDC to index a **froxel volume**
/// (`mod-0365`..`0370`, `0167`, `0168` — unproject with member 2, reproject
/// with member 1, sample a `sampler3D`). That volume is filled by compute
/// (`mod-0177`, `mod-0364`), which has no `gl_ViewIndex` and so is
/// parametrised by X4's SYMMETRIC frustum for ever. The lookup must therefore
/// use the symmetric projection with a *correct* view position — exactly what
/// this patch now produces. Composing the affine onto member 1 as well would
/// send those samples off by the full map. The remaining two (`mod-0111`,
/// `mod-0112`) read `M_projection[1][1]` as a scalar, to fade an effect by
/// zoom; the whole-matrix load this patch keys on does not match them, which
/// is the right answer for a value that is not a transform.
inline bool patch_fragment_invproj_eye(std::vector<uint32_t> &code, uint32_t set,
                                       uint32_t binding, uint32_t member,
                                       float d_left, float d_right,
                                       const OffAxis *oa_left = nullptr,
                                       const OffAxis *oa_right = nullptr) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    uint32_t bound = code[3];
    auto new_id = [&bound] { return bound++; };

    uint32_t frag_fn = 0, compute_fn = 0, existing_view_var = 0;
    struct PtrType { uint32_t storage, pointee; };
    std::unordered_map<uint32_t, PtrType> ptr_types;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> vars;
    std::unordered_map<uint32_t, std::vector<uint32_t>> struct_members;
    std::unordered_map<uint32_t, uint32_t> var_set, var_binding;
    std::unordered_map<uint32_t, uint32_t> int_consts; // id -> value
    uint32_t t_float = 0, t_v4 = 0, t_mat4 = 0, t_int = 0, t_bool = 0;
    size_t first_fn = 0, last_annotation_end = 0, first_global = 0;
    size_t caps_end = 0, exts_end = 0;
    bool have_first_fn = false, have_first_global = false;
    bool has_multiview_cap = false, has_multiview_ext = false;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
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
            if (in.len >= 3 && w[1] == ExecutionModelFragment && !frag_fn)
                frag_fn = w[2];
            if (in.len >= 3 && w[1] == ExecutionModelGLCompute && !compute_fn)
                compute_fn = w[2];
            break;
        case OpMemberDecorate:
            last_annotation_end = in.start + in.len;
            break;
        case OpDecorate:
            last_annotation_end = in.start + in.len;
            if (in.len >= 4 && w[2] == DecorationBuiltIn &&
                w[3] == BuiltInViewIndex)
                existing_view_var = w[1];
            if (in.len >= 4 && w[2] == DecorationDescriptorSet)
                var_set[w[1]] = w[3];
            if (in.len >= 4 && w[2] == DecorationBinding)
                var_binding[w[1]] = w[3];
            break;
        case OpTypeFloat:
            if (in.len >= 3 && w[2] == 32 && !t_float)
                t_float = w[1];
            break;
        case OpTypeInt:
            if (in.len >= 4 && w[2] == 32 && w[3] == 1 && !t_int)
                t_int = w[1];
            break;
        case OpTypeBool:
            if (in.len >= 2 && !t_bool)
                t_bool = w[1];
            break;
        case OpTypeVector:
            if (in.len >= 4 && w[2] == t_float && w[3] == 4 && !t_v4)
                t_v4 = w[1];
            break;
        case OpTypeMatrix:
            if (in.len >= 4 && w[2] == t_v4 && w[3] == 4 && !t_mat4)
                t_mat4 = w[1];
            break;
        case OpConstant:
            if (in.len >= 4 && w[1] == t_int)
                int_consts[w[2]] = w[3];
            break;
        case OpTypeStruct:
            if (in.len >= 2) {
                std::vector<uint32_t> m;
                for (uint32_t j = 2; j < in.len; j++)
                    m.push_back(w[j]);
                struct_members[w[1]] = std::move(m);
            }
            break;
        case OpTypePointer:
            if (in.len >= 4)
                ptr_types[w[1]] = {w[2], w[3]};
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

    // No fragment stage, or a compute module: nothing this patch can do. A
    // compute dispatch has no view index to select `d` with, so "correcting"
    // it would mean picking one eye and being wrong in the other.
    if (!frag_fn || !t_float || !t_v4 || !t_mat4 || !have_first_fn)
        return false;
    // Task #39, refused on exactly the terms the vertex patches use. The two
    // refusals HAVE to agree: a frustum the vertex side declines while this
    // side accepts would leave the affine applied nowhere and undone
    // everywhere, which is worse than either state alone.
    if ((oa_left && !oa_left->ok) || (oa_right && !oa_right->ok))
        return false;
    if (oa_right && !oa_left)
        return false; // a right-eye map with no left is a caller error

    // --- the camera block(s), verified by shape ---------------------------
    //
    // EVERY variable at (set, binding), not the first one. X4's modules are
    // combined vertex+fragment and declare the camera block *once per stage*:
    // mod-0100 has %__1 and %__3, both pointing at
    // BLOCK_BUFFER_BINDING_SLOT_CAMERA, both decorated set 1 binding 0. They
    // address the same buffer, so for a patch that only *reads* the block
    // either handle does; but this patch rewrites existing **loads**, and
    // those name whichever variable their own stage declared. Taking the first
    // and breaking silently skipped every module whose fragment stage used the
    // other one.
    //
    // This is take forty-eight's bug in a new place: first-match on an aliased
    // binding, which is legal, which X4 does, and which reads as correct until
    // something downstream comes back wrong. Recorded here because it is now
    // the second time.
    std::vector<uint32_t> cam_vars;
    for (auto &[id, ts] : vars) {
        if (ts.second != StorageClassUniform)
            continue;
        auto s = var_set.find(id), b = var_binding.find(id);
        if (s == var_set.end() || b == var_binding.end())
            continue;
        if (s->second != set || b->second != binding)
            continue;
        auto pt = ptr_types.find(ts.first);
        if (pt == ptr_types.end())
            continue;
        auto sm = struct_members.find(pt->second.pointee);
        if (sm == struct_members.end() || member >= sm->second.size())
            continue;
        if (sm->second[member] != t_mat4)
            continue;
        cam_vars.push_back(id);
    }
    if (cam_vars.empty())
        return false;

    // --- access chains that name exactly (cam_var, member) --------------
    // Exactly: `OpAccessChain %ptr %r %cam %idx` and nothing further. A chain
    // that indexes deeper yields a column or a scalar rather than the matrix,
    // and correcting those would need a different edit. None of X4's do, and a
    // wrong assumption here would silently mangle one.
    std::unordered_map<uint32_t, bool> chain_ids;
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op != OpAccessChain || in.len != 5)
            continue;
        if (std::find(cam_vars.begin(), cam_vars.end(), w[3]) == cam_vars.end())
            continue;
        auto c = int_consts.find(w[4]);
        if (c == int_consts.end() || c->second != member)
            continue;
        chain_ids[w[2]] = true;
    }
    if (chain_ids.empty())
        return false;

    // --- loads of those pointers inside the fragment entry function ------
    std::unordered_map<size_t, uint32_t> patch_loads; // inst start -> result id
    {
        uint32_t cur_fn = 0;
        for (const Inst &in : insts) {
            const uint32_t *w = &code[in.start];
            if (in.op == OpFunction && in.len >= 3)
                cur_fn = w[2];
            if (in.op == OpFunctionEnd)
                cur_fn = 0;
            if (in.op != OpLoad || in.len < 4 || cur_fn != frag_fn)
                continue;
            if (w[1] != t_mat4 || !chain_ids.count(w[3]))
                continue;
            patch_loads[in.start] = w[2];
        }
    }
    if (patch_loads.empty())
        return false;

    // --- declarations ----------------------------------------------------
    std::vector<uint32_t> decls;
    if (!t_int) {
        t_int = new_id();
        emit(decls, OpTypeInt, {t_int, 32, 1});
    }
    auto fconst = [&](float f) {
        uint32_t bits;
        memcpy(&bits, &f, 4);
        const uint32_t id = new_id();
        emit(decls, OpConstant, {t_float, id, bits});
        return id;
    };
    const uint32_t c_dl = fconst(d_left);
    const uint32_t c_ddiff = fconst(d_right - d_left);
    // Nothing below runs unless #39 is asked for, which is what keeps the
    // emitted words identical to the pre-#39 ones when it is not.
    const bool oa_on = oa_left != nullptr;
    const bool oa_stereo = oa_right != nullptr;
    OffAxisIds oa{};
    uint32_t c_oa[4] = {}, c_oad[4] = {};
    if (oa_on) {
        if (!t_bool) {
            t_bool = new_id();
            emit(decls, OpTypeBool, {t_bool});
        }
        oa.t_float = t_float;
        oa.t_v4 = t_v4;
        oa.t_bool = t_bool;
        oa.zero_f = fconst(0.0f);
        oa.one_f = fconst(1.0f);
        const float l[4] = {oa_left->ax_num, oa_left->bx, oa_left->ay_num,
                            oa_left->by};
        for (int i = 0; i < 4; i++)
            c_oa[i] = fconst(l[i]);
        if (oa_stereo) {
            const float r[4] = {oa_right->ax_num, oa_right->bx,
                                oa_right->ay_num, oa_right->by};
            for (int i = 0; i < 4; i++)
                c_oad[i] = fconst(r[i] - l[i]);
        }
    }
    const uint32_t view_var = existing_view_var ? existing_view_var : new_id();
    if (!existing_view_var) {
        const uint32_t ptr_in_int = new_id();
        emit(decls, OpTypePointer, {ptr_in_int, StorageClassInput, t_int});
        emit(decls, OpVariable, {ptr_in_int, view_var, StorageClassInput});
    }

    const size_t anno_at = last_annotation_end
                               ? last_annotation_end
                               : (have_first_global ? first_global : first_fn);

    std::vector<uint32_t> out;
    out.reserve(code.size() + decls.size() + patch_loads.size() * 32);
    out.insert(out.end(), code.begin(), code.begin() + 5);

    const size_t cap_at = caps_end ? caps_end : 5;
    const size_t ext_at = exts_end ? exts_end : cap_at;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (!has_multiview_cap && in.start == cap_at)
            emit(out, OpCapability, {CapabilityMultiView});
        if (!has_multiview_ext && code[1] < 0x00010300u && in.start == ext_at) {
            static const uint32_t kExt[] = {0x5f565053u, 0x5f52484bu,
                                            0x746c756du, 0x65697669u,
                                            0x00000077u};
            out.push_back((uint32_t)((1 + 5) << 16) | OpExtension);
            for (uint32_t x : kExt)
                out.push_back(x);
        }
        if (in.start == anno_at && !existing_view_var) {
            emit(out, OpDecorate,
                 {view_var, DecorationBuiltIn, BuiltInViewIndex});
            // An integer Input in a fragment shader must be Flat --
            // interpolating one is meaningless and Vulkan forbids it.
            emit(out, OpDecorate, {view_var, DecorationFlat});
        }
        if (in.start == first_fn)
            out.insert(out.end(), decls.begin(), decls.end());

        auto pl = patch_loads.find(in.start);
        if (pl != patch_loads.end()) {
            const uint32_t orig = pl->second;
            // Fresh id for the raw load; the original id is redefined below by
            // the last OpCompositeInsert, so every downstream use picks up the
            // corrected matrix without touching a single use site.
            const uint32_t raw = new_id();
            emit(out, OpLoad, {t_mat4, raw, w[3]});

            // d = d_left + float(gl_ViewIndex) * (d_right - d_left)
            const uint32_t vi = new_id();
            emit(out, OpLoad, {t_int, vi, view_var});
            const uint32_t vf = new_id();
            emit(out, OpConvertSToF, {t_float, vf, vi});
            const uint32_t scaled = new_id();
            emit(out, OpFMul, {t_float, scaled, vf, c_ddiff});
            const uint32_t d = new_id();
            emit(out, OpFAdd, {t_float, d, c_dl, scaled});

            // Task #39: M·A⁻¹ first, then T(d) on the result, which is the
            // order the composition is written in -- A is undone first
            // because it was applied last.
            uint32_t src = raw;
            if (oa_on) {
                OffAxisIds t = oa;
                t.ax_num = select_view(out, bound, t_float, vf, c_oa[0],
                                       c_oad[0]);
                t.bx = select_view(out, bound, t_float, vf, c_oa[1], c_oad[1]);
                t.ay_num = select_view(out, bound, t_float, vf, c_oa[2],
                                       c_oad[2]);
                t.by = select_view(out, bound, t_float, vf, c_oa[3], c_oad[3]);
                src = emit_inv_off_axis(out, bound, t, t_mat4, raw);
            }

            // row 0 += d * row 3, one column at a time
            uint32_t acc = src;
            for (uint32_t c = 0; c < 4; c++) {
                const uint32_t x = new_id();
                emit(out, OpCompositeExtract, {t_float, x, src, c, 0});
                const uint32_t wv = new_id();
                emit(out, OpCompositeExtract, {t_float, wv, src, c, 3});
                const uint32_t dw = new_id();
                emit(out, OpFMul, {t_float, dw, d, wv});
                const uint32_t nx = new_id();
                emit(out, OpFAdd, {t_float, nx, x, dw});
                const uint32_t next = (c == 3) ? orig : new_id();
                emit(out, OpCompositeInsert, {t_mat4, next, nx, acc, c, 0});
                acc = next;
            }
            continue; // the original OpLoad is replaced, not kept
        }

        // gl_ViewIndex is an Input and must be in the fragment entry point's
        // interface list.
        if (in.op == OpEntryPoint && in.len >= 3 &&
            w[1] == ExecutionModelFragment && !existing_view_var) {
            out.push_back((uint32_t)((in.len + 1) << 16) | OpEntryPoint);
            for (uint32_t j = 1; j < in.len; j++)
                out.push_back(w[j]);
            out.push_back(view_var);
            continue;
        }

        out.insert(out.end(), code.begin() + in.start,
                   code.begin() + in.start + in.len);
    }

    out[3] = bound;
    code.swap(out);
    return true;
}

/// One sampled texture a fragment module declares.
struct SampledTexture {
    uint32_t set, binding;
    uint32_t count; // descriptor-array length; 1 for a plain texture
    bool arrayed;   // already a 2D array -- nothing for the patch to do
    bool depth;     // a shadow sampler, which the patch refuses
};

/// Lists the 2D textures a fragment module samples, without modifying it.
///
/// This exists to be pointed at X4's shaders before anything is patched, and
/// the first time it was, it reported "samples nothing" about a shader that
/// samples. X4 is **bindless**: the variable's type is not an image but an
/// `OpTypeArray` of 53306 images, and looking only for the image type walked
/// straight past it. Seeing through the array is the whole reason `count`
/// exists — it is the number that decides whether the per-view mechanism can be
/// an index offset instead of a type change.
///
/// `arrayed` and `depth` are reported because they are the two shapes
/// `patch_fragment_view_layer` refuses, and it is better to learn that from a
/// log line than from a live run where nothing changed.
inline std::vector<SampledTexture>
list_sampled_textures(const std::vector<uint32_t> &code) {
    std::vector<SampledTexture> out;
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return out;

    bool fragment = false;
    std::unordered_map<uint32_t, uint32_t> dec_set, dec_binding;
    std::unordered_map<uint32_t, std::vector<uint32_t>> img_ops;
    std::unordered_map<uint32_t, uint32_t> si_img;
    std::unordered_map<uint32_t, uint32_t> ptr_pointee;
    std::unordered_map<uint32_t, uint32_t> const_val;  // id -> literal
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> arr_of;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        switch (in.op) {
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelFragment)
                fragment = true;
            break;
        case OpConstant:
            if (in.len >= 4)
                const_val[w[2]] = w[3];
            break;
        case OpTypeArray:
            // element type and the length's constant id
            if (in.len >= 4)
                arr_of[w[1]] = {w[2], w[3]};
            break;
        case OpTypeRuntimeArray:
            if (in.len >= 3)
                arr_of[w[1]] = {w[2], 0};
            break;
        case OpDecorate:
            if (in.len >= 4 && w[2] == DecorationDescriptorSet)
                dec_set[w[1]] = w[3];
            if (in.len >= 4 && w[2] == DecorationBinding)
                dec_binding[w[1]] = w[3];
            break;
        case OpTypeImage:
            if (in.len >= 9)
                img_ops[w[1]] = std::vector<uint32_t>(w + 2, w + in.len);
            break;
        case OpTypeSampledImage:
            if (in.len >= 3)
                si_img[w[1]] = w[2];
            break;
        case OpTypePointer:
            if (in.len >= 4 && w[2] == StorageClassUniformConstant)
                ptr_pointee[w[1]] = w[3];
            break;
        case OpVariable: {
            if (in.len < 4 || w[3] != StorageClassUniformConstant)
                break;
            auto p = ptr_pointee.find(w[1]);
            if (p == ptr_pointee.end())
                break;
            // Bindless: the pointee is an array of images (or of sampled
            // images), not an image. X4 declares 53306 of them.
            uint32_t pointee = p->second, count = 1;
            auto a = arr_of.find(pointee);
            if (a != arr_of.end()) {
                pointee = a->second.first;
                auto cv = const_val.find(a->second.second);
                count = cv != const_val.end() ? cv->second : 0; // 0 = runtime
            }
            auto s = si_img.find(pointee);
            const uint32_t img = s != si_img.end() ? s->second : pointee;
            auto io = img_ops.find(img);
            if (io == img_ops.end() || io->second.size() < 7)
                break;
            // Dim 2D, sampled (not a storage image), not multisampled.
            if (io->second[1] != Dim2D || io->second[4] != 0 ||
                io->second[5] != 1)
                break;
            auto ds = dec_set.find(w[2]);
            auto db = dec_binding.find(w[2]);
            out.push_back({ds != dec_set.end() ? ds->second : UINT32_MAX,
                           db != dec_binding.end() ? db->second : UINT32_MAX,
                           count, io->second[3] != 0, io->second[2] == 1});
            break;
        }
        default:
            break;
        }
    }
    if (!fragment)
        out.clear();
    return out;
}

/// What `list_sampled_textures` cannot see, counted honestly.
///
/// That function is fragment-only and 2D-only, by design and by its name. The
/// mistake was using it as the layer's answer to "does this module declare a
/// table the mirror covers?" -- a question that has nothing to do with either
/// filter. X4's skybox is a **compute** shader sampling the same 53306-entry
/// heap as a **cube** array, so it failed both tests, and a refusal counter
/// built on the lister reported `0 refused` about a module it could not see.
///
/// The heap is one bindless region holding mixed image types; the mirror writes
/// twins for every image descriptor at those bindings regardless of dim or
/// stage. So coverage has to be measured the same way.
struct TableSurvey {
    bool fragment = false; ///< has a fragment entry point
    bool compute = false;  ///< has a GLCompute entry point
    uint32_t large = 0;    ///< image arrays declared with count > `min_count`
};

inline TableSurvey survey_image_tables(const std::vector<uint32_t> &code,
                                       uint32_t min_count) {
    TableSurvey s;
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return s;

    std::unordered_map<uint32_t, uint32_t> const_val, si_img, ptr_pointee;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> arr_of;
    std::unordered_set<uint32_t> img_types;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        switch (in.op) {
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelFragment)
                s.fragment = true;
            if (in.len >= 3 && w[1] == ExecutionModelGLCompute)
                s.compute = true;
            break;
        case OpConstant:
            if (in.len >= 4)
                const_val[w[2]] = w[3];
            break;
        case OpTypeArray:
            if (in.len >= 4)
                arr_of[w[1]] = {w[2], w[3]};
            break;
        case OpTypeImage:
            if (in.len >= 9)
                img_types.insert(w[1]);
            break;
        case OpTypeSampledImage:
            if (in.len >= 3)
                si_img[w[1]] = w[2];
            break;
        case OpTypePointer:
            if (in.len >= 4 && w[2] == StorageClassUniformConstant)
                ptr_pointee[w[1]] = w[3];
            break;
        case OpVariable: {
            if (in.len < 4 || w[3] != StorageClassUniformConstant)
                break;
            auto p = ptr_pointee.find(w[1]);
            if (p == ptr_pointee.end())
                break;
            auto a = arr_of.find(p->second);
            if (a == arr_of.end())
                break;
            auto cv = const_val.find(a->second.second);
            if (cv == const_val.end() || cv->second <= min_count)
                break;
            uint32_t el = a->second.first;
            auto si = si_img.find(el);
            if (si != si_img.end())
                el = si->second;
            if (img_types.count(el))
                s.large++;
            break;
        }
        default:
            break;
        }
    }
    return s;
}

/// Rewrites a fragment module so the texture at (`set`, `binding`) is read as a
/// 2D **array** texture whose layer is `gl_ViewIndex`:
///
///     uniform sampler2D src        ->  uniform sampler2DArray src
///     texture(src, uv)             ->  texture(src, vec3(uv, gl_ViewIndex))
///     texelFetch(src, xy, 0)       ->  texelFetch(src, ivec3(xy, gl_ViewIndex), 0)
///
/// This is the other half of the stereo mechanism, and it exists because of
/// one asymmetry in multiview: a view-masked pass view-indexes its *subpass
/// inputs* automatically, but it never view-indexes a *sampler*. A descriptor
/// set is bound once for the whole pass and has no per-view form, so both views
/// read array layer 0 and draw the same picture. That is exactly what X4's
/// tonemap does — `#103` replicates into both layers and the contents are
/// identical (docs/frame-analysis.md, "Take nineteen"). Nothing about the
/// masking is wrong; the read is.
///
/// Two decisions are worth stating, because the cheap version of each is wrong:
///
/// **The image type is rebuilt, not edited.** Flipping `Arrayed` on the
/// existing `OpTypeImage` is one word, but a module's `sampler2D`s all share
/// one type id — so that word would promote every other texture in the shader
/// to an array as well, and none of those were doubled. A fresh type reachable
/// only from the variable we were asked about cannot touch them.
///
/// **Anything not understood is a bail-out.** The retyped value is followed
/// through the function bodies; an instruction that consumes it and is not on
/// the short accepted list leaves the module untouched. Depth-compare (`Dref`)
/// reads are refused outright — shadow maps are what killed the previous X4 VR
/// attempt, and they are not among the doubled images.
///
/// The variable is *relocated* to the end of the globals section rather than
/// retyped in place. Real modules declare it early: in `tests/sample.frag.spv`
/// glslc emits `%src = OpVariable` before `%int` and `%v2int` exist, so
/// declarations parked in front of it would reference types that are not
/// defined yet.
inline bool patch_fragment_view_layer(std::vector<uint32_t> &code,
                                      uint32_t want_set,
                                      uint32_t want_binding) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    uint32_t bound = code[3];
    auto new_id = [&bound] { return bound++; };

    // --- pass 1: learn the module -------------------------------------
    uint32_t entry_fn = 0;
    std::unordered_map<uint32_t, uint32_t> dec_set, dec_binding;
    std::unordered_map<uint32_t, std::vector<uint32_t>> img_ops; // img -> operands
    std::unordered_map<uint32_t, uint32_t> si_img;   // sampled-image -> image
    struct PtrType { uint32_t storage, pointee; };
    std::unordered_map<uint32_t, PtrType> ptr_types;
    std::unordered_set<uint32_t> f32_types;
    std::unordered_map<uint32_t, uint32_t> vec3_of;  // component type -> vec3
    uint32_t t_int = 0;
    size_t first_fn = 0, last_annotation_end = 0, first_global = 0;
    bool have_first_fn = false, have_first_global = false;
    size_t caps_end = 0, exts_end = 0;
    bool has_multiview_cap = false, has_multiview_ext = false;
    size_t target_start = 0;   // the OpVariable we relocate
    uint32_t target_var = 0, target_ptr = 0;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (!have_first_global)
            switch (in.op) {
            case OpTypeInt:
            case OpTypeFloat:
            case OpTypeVector:
            case OpTypeMatrix:
            case OpTypeImage:
            case OpTypeSampler:
            case OpTypeSampledImage:
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
            if (in.len >= 3 && w[1] == ExecutionModelFragment && !entry_fn)
                entry_fn = w[2];
            break;
        case OpMemberDecorate:
            last_annotation_end = in.start + in.len;
            break;
        case OpDecorate:
            last_annotation_end = in.start + in.len;
            if (in.len >= 4 && w[2] == DecorationDescriptorSet)
                dec_set[w[1]] = w[3];
            if (in.len >= 4 && w[2] == DecorationBinding)
                dec_binding[w[1]] = w[3];
            break;
        case OpTypeFloat:
            if (in.len >= 3 && w[2] == 32)
                f32_types.insert(w[1]);
            break;
        case OpTypeInt:
            if (in.len >= 4 && w[2] == 32 && w[3] == 1 && !t_int)
                t_int = w[1];
            break;
        case OpTypeVector:
            if (in.len >= 4 && w[3] == 3 && !vec3_of.count(w[2]))
                vec3_of[w[2]] = w[1];
            break;
        case OpTypeImage:
            if (in.len >= 9)
                img_ops[w[1]] =
                    std::vector<uint32_t>(w + 2, w + in.len);
            break;
        case OpTypeSampledImage:
            if (in.len >= 3)
                si_img[w[1]] = w[2];
            break;
        case OpTypePointer:
            if (in.len >= 4)
                ptr_types[w[1]] = {w[2], w[3]};
            break;
        case OpVariable:
            if (in.len >= 4 && w[3] == StorageClassUniformConstant &&
                !target_var) {
                auto s = dec_set.find(w[2]);
                auto b = dec_binding.find(w[2]);
                if (s != dec_set.end() && b != dec_binding.end() &&
                    s->second == want_set && b->second == want_binding) {
                    target_var = w[2];
                    target_ptr = w[1];
                    target_start = in.start;
                }
            }
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

    if (!entry_fn || !target_var || !have_first_fn)
        return false;

    // --- the type we are replacing ------------------------------------
    auto pt = ptr_types.find(target_ptr);
    if (pt == ptr_types.end() || pt->second.storage != StorageClassUniformConstant)
        return false;
    const uint32_t old_pointee = pt->second.pointee;
    // Either a combined `sampler2D` (pointee is the sampled-image type) or a
    // separate `texture2D` bound alongside its own sampler.
    const bool combined = si_img.count(old_pointee) != 0;
    const uint32_t old_img = combined ? si_img[old_pointee] : old_pointee;
    auto io = img_ops.find(old_img);
    if (io == img_ops.end())
        return false;
    std::vector<uint32_t> ops = io->second; // sampled_type Dim Depth Arrayed MS Sampled Format [Access]
    if (ops.size() < 7)
        return false;
    const uint32_t t_float = ops[0];
    // Only the shape this is written for: a plain, non-multisampled, sampled
    // 2D colour texture. Depth==1 is a shadow sampler and is refused above the
    // Dref check as well, so it cannot arrive here by another route.
    if (!f32_types.count(t_float) || ops[1] != Dim2D || ops[2] != 0 ||
        ops[3] != 0 || ops[4] != 0 || ops[5] != 1)
        return false;

    const uint32_t new_img = new_id();
    const uint32_t new_si = combined ? new_id() : 0;
    const uint32_t new_pointee = combined ? new_si : new_img;
    const uint32_t new_ptr = new_id();
    std::unordered_map<uint32_t, uint32_t> retype_map{{old_img, new_img}};
    if (combined)
        retype_map[old_pointee] = new_si;

    // --- pass 2: follow the retyped value, and refuse surprises --------
    // `tainted` is every id now carrying an arrayed type, starting with the
    // variable itself. Anything inside a function that mentions one of them
    // and is not handled below means this module does something with the
    // texture we have not accounted for, and the patch is abandoned.
    std::unordered_set<uint32_t> tainted{target_var};
    std::unordered_map<size_t, uint32_t> retype;    // inst start -> new result type
    std::unordered_map<size_t, bool> coord_fix;     // inst start -> integer coords
    bool need_v3f = false, need_v3i = false;
    bool in_function = false;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op == OpFunction)
            in_function = true;
        if (in.op == OpFunctionEnd) {
            in_function = false;
            continue;
        }
        if (!in_function)
            continue;

        bool touches = false;
        for (uint32_t j = 1; j < in.len; j++)
            if (tainted.count(w[j]))
                touches = true;
        if (!touches)
            continue;

        switch (in.op) {
        case OpLoad:
            // The pointer is our variable; the loaded value takes the new type.
            if (in.len < 4 || !tainted.count(w[3]) || w[1] != old_pointee)
                return false;
            retype[in.start] = new_pointee;
            tainted.insert(w[2]);
            break;
        case OpSampledImage:
        case OpImage: {
            // Image <-> sampled-image conversions, both of which just carry the
            // arrayed-ness across to a new id.
            if (in.len < 4 || !tainted.count(w[3]))
                return false;
            auto r = retype_map.find(w[1]);
            if (r == retype_map.end())
                return false;
            retype[in.start] = r->second;
            tainted.insert(w[2]);
            break;
        }
        case OpImageSampleImplicitLod:
        case OpImageSampleExplicitLod:
        case OpImageGather:
            if (in.len < 5 || !tainted.count(w[3]))
                return false;
            coord_fix[in.start] = false;
            need_v3f = true;
            break;
        case OpImageFetch:
            if (in.len < 5 || !tainted.count(w[3]))
                return false;
            coord_fix[in.start] = true;
            need_v3i = true;
            break;
        default:
            // Dref sampling, image queries, storage reads, passing the sampler
            // to a function: all real SPIR-V, none of it handled here.
            return false;
        }
    }
    if (coord_fix.empty())
        return false; // the texture is declared but never read

    // --- build the declarations ---------------------------------------
    std::vector<uint32_t> decls;
    uint32_t t_v3f = 0, t_v3i = 0;
    if (need_v3f) {
        auto v = vec3_of.find(t_float);
        if (v != vec3_of.end()) {
            t_v3f = v->second;
        } else {
            t_v3f = new_id();
            emit(decls, OpTypeVector, {t_v3f, t_float, 3});
        }
    }
    if (need_v3i || !t_int) {
        if (!t_int) {
            t_int = new_id();
            emit(decls, OpTypeInt, {t_int, 32, 1});
        }
    }
    if (need_v3i) {
        auto v = vec3_of.find(t_int);
        if (v != vec3_of.end()) {
            t_v3i = v->second;
        } else {
            t_v3i = new_id();
            emit(decls, OpTypeVector, {t_v3i, t_int, 3});
        }
    }

    ops[3] = 1; // Arrayed
    std::vector<uint32_t> img_decl{new_img};
    img_decl.insert(img_decl.end(), ops.begin(), ops.end());
    emit_n(decls, OpTypeImage, img_decl);
    if (combined)
        emit(decls, OpTypeSampledImage, {new_si, new_img});
    emit(decls, OpTypePointer,
         {new_ptr, StorageClassUniformConstant, new_pointee});
    // The relocated variable, carrying over any initialiser it had.
    {
        const uint32_t *w = &code[target_start];
        const uint32_t len = code[target_start] >> 16;
        std::vector<uint32_t> v{new_ptr};
        for (uint32_t j = 2; j < len; j++)
            v.push_back(w[j]);
        emit_n(decls, OpVariable, v);
    }
    const uint32_t ptr_in_int = new_id();
    emit(decls, OpTypePointer, {ptr_in_int, StorageClassInput, t_int});
    const uint32_t view_var = new_id();
    emit(decls, OpVariable, {ptr_in_int, view_var, StorageClassInput});

    const size_t anno_at = last_annotation_end
                               ? last_annotation_end
                               : (have_first_global ? first_global : first_fn);

    // --- pass 3: rebuild -----------------------------------------------
    std::vector<uint32_t> out;
    out.reserve(code.size() + decls.size() + 64);
    out.insert(out.end(), code.begin(), code.begin() + 5);
    const size_t cap_at = caps_end ? caps_end : 5;
    const size_t ext_at = exts_end ? exts_end : cap_at;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (!has_multiview_cap && in.start == cap_at)
            emit(out, OpCapability, {CapabilityMultiView});
        if (!has_multiview_ext && code[1] < 0x00010300u && in.start == ext_at) {
            static const uint32_t kExt[] = {0x5f565053u, 0x5f52484bu,
                                            0x746c756du, 0x65697669u,
                                            0x00000077u};
            out.push_back((uint32_t)((1 + 5) << 16) | OpExtension);
            for (uint32_t x : kExt)
                out.push_back(x);
        }
        if (in.start == anno_at) {
            emit(out, OpDecorate,
                 {view_var, DecorationBuiltIn, BuiltInViewIndex});
            emit(out, OpDecorate, {view_var, DecorationFlat});
        }
        if (in.start == first_fn)
            out.insert(out.end(), decls.begin(), decls.end());

        // The original declaration of the variable: dropped, since `decls`
        // re-emitted it with the arrayed type.
        if (in.start == target_start)
            continue;

        // The ViewIndex input joins the entry point's interface list, and so
        // does the relocated variable -- it is the same id, so the existing
        // entry stays valid and only the new one has to be added.
        if (in.op == OpEntryPoint && in.len >= 3 && w[2] == entry_fn) {
            out.push_back((uint32_t)((in.len + 1) << 16) | OpEntryPoint);
            for (uint32_t j = 1; j < in.len; j++)
                out.push_back(w[j]);
            out.push_back(view_var);
            continue;
        }

        auto cf = coord_fix.find(in.start);
        if (cf != coord_fix.end()) {
            const bool ints = cf->second;
            const uint32_t vi = new_id();
            emit(out, OpLoad, {t_int, vi, view_var});
            uint32_t comp = vi;
            if (!ints) {
                comp = new_id();
                emit(out, OpConvertSToF, {t_float, comp, vi});
            }
            const uint32_t c3 = new_id();
            emit(out, OpCompositeConstruct,
                 {ints ? t_v3i : t_v3f, c3, w[4], comp});
            out.push_back(w[0]);
            for (uint32_t j = 1; j < in.len; j++)
                out.push_back(j == 4 ? c3 : w[j]);
            continue;
        }

        auto rt = retype.find(in.start);
        if (rt != retype.end()) {
            out.push_back(w[0]);
            out.push_back(rt->second);
            for (uint32_t j = 2; j < in.len; j++)
                out.push_back(w[j]);
            continue;
        }

        out.insert(out.end(), code.begin() + in.start,
                   code.begin() + in.start + in.len);
    }

    out[3] = bound;
    code.swap(out);
    return true;
}

/// Per-view sampling for a bindless table: `element = index + ViewIndex*offset`.
///
/// This is the mechanism, where `patch_fragment_view_layer` above was only a
/// proof that a sample can follow `gl_ViewIndex` at all. X4 keeps every texture
/// in one 53,306-entry array indexed by an integer in a uniform block, so
/// per-view sampling is integer arithmetic on that index and **no type
/// changes**: `sampler2D` stays `sampler2D`, no view has to be rebuilt to match,
/// and the shader/view pairing problem the array approach died of never arises.
///
/// The layer mirrors every descriptor write into `slot + offset`, so the twin
/// element already holds either a layer-1 view of a doubled image or an
/// identical copy of an ordinary one. That is what makes this safe to apply
/// blindly: a texture that is not per-eye reads the same in both views, so the
/// patch needs no per-shader targeting and never has to know which slot holds
/// which image -- and a shadow map's twin *is* the same shadow map, which
/// defuses the hazard that wrecked the earlier attempt instead of dodging it.
///
/// Refuses, leaving `code` untouched, if:
///   - there is no fragment entry point, or no such variable at (set, binding);
///   - the variable is not an array of images or sampled images (a plain
///     texture has no index to offset);
///   - the index's type cannot be established, or is not a 32-bit integer;
///   - the table is indexed from any function other than the fragment entry
///     point's own. A helper function would need the same treatment and its
///     reachability is not analysed here, so the whole module is declined
///     rather than half-patched.
inline bool patch_fragment_index_offset(std::vector<uint32_t> &code,
                                        uint32_t want_set,
                                        uint32_t want_binding,
                                        uint32_t offset) {
    if (!offset)
        return false;
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    // --- pass 1: learn the module ---------------------------------------
    uint32_t entry_fn = 0;
    size_t caps_end = 0, exts_end = 0, last_annotation_end = 0;
    size_t first_global = 0, first_fn = 0;
    bool have_first_global = false, has_multiview_cap = false;
    bool has_multiview_ext = false;
    uint32_t bound = code[3], existing_view_var = 0;
    std::unordered_map<uint32_t, uint32_t> set_of, bind_of; // id -> value
    std::unordered_map<uint32_t, uint32_t> ptr_to;          // ptr type -> pointee
    std::unordered_map<uint32_t, uint32_t> arr_elem;        // array -> element
    std::unordered_map<uint32_t, uint32_t> res_type;        // id -> its type id
    std::unordered_set<uint32_t> img_types, si_types;
    // int type id -> width, and separately the signed 32-bit one if present.
    std::unordered_map<uint32_t, uint32_t> int_width;
    uint32_t t_int_signed = 0;
    struct KnownConst {
        uint32_t type, value;
    };
    std::unordered_map<uint32_t, KnownConst> consts;
    std::vector<size_t> var_starts;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        switch (in.op) {
        case OpCapability:
            caps_end = in.start + in.len;
            if (in.len >= 2 && w[1] == CapabilityMultiView)
                has_multiview_cap = true;
            break;
        case OpExtension:
            exts_end = in.start + in.len;
            // All five words, not just "SPV_". X4's bindless modules declare
            // SPV_EXT_descriptor_indexing, and a first-word-only test read that
            // as "multiview is already declared", so the capability went in
            // without the extension that permits it.
            if (in.len >= 6 && w[1] == 0x5f565053u && w[2] == 0x5f52484bu &&
                w[3] == 0x746c756du && w[4] == 0x65697669u &&
                w[5] == 0x00000077u)
                has_multiview_ext = true;
            break;
        case OpEntryPoint:
            if (in.len >= 3 && w[1] == ExecutionModelFragment)
                entry_fn = w[2];
            break;
        case OpDecorate:
            last_annotation_end = in.start + in.len;
            if (in.len >= 4 && w[2] == DecorationDescriptorSet)
                set_of[w[1]] = w[3];
            if (in.len >= 4 && w[2] == DecorationBinding)
                bind_of[w[1]] = w[3];
            // X4's modules declare two and three tables, so this runs more than
            // once per module. Two variables decorated BuiltIn ViewIndex in one
            // entry point is invalid SPIR-V, so a second call reuses the first
            // call's input instead of declaring another.
            if (in.len >= 4 && w[2] == DecorationBuiltIn &&
                w[3] == BuiltInViewIndex)
                existing_view_var = w[1];
            break;
        case OpMemberDecorate:
            last_annotation_end = in.start + in.len;
            break;
        case OpTypeInt:
            if (in.len >= 4) {
                int_width[w[1]] = w[2];
                if (w[2] == 32 && w[3] == 1)
                    t_int_signed = w[1];
            }
            break;
        case OpTypeImage:
            if (in.len >= 2)
                img_types.insert(w[1]);
            break;
        case OpTypeSampledImage:
            if (in.len >= 3)
                si_types.insert(w[1]);
            break;
        case OpTypeArray:
        case OpTypeRuntimeArray:
            if (in.len >= 3)
                arr_elem[w[1]] = w[2];
            break;
        case OpTypePointer:
            if (in.len >= 4)
                ptr_to[w[1]] = w[3];
            break;
        case OpConstant:
            if (in.len >= 4)
                consts[w[2]] = {w[1], w[3]};
            break;
        case OpVariable:
            var_starts.push_back(in.start);
            break;
        case OpFunction:
            if (!first_fn)
                first_fn = in.start;
            break;
        default:
            break;
        }
        // The globals section begins at the first type/constant declaration.
        if (!have_first_global && in.op >= OpTypeInt && in.op <= OpTypePointer) {
            have_first_global = true;
            first_global = in.start;
        }
        // Result-type map, for the opcodes an index can plausibly come from.
        // Every one of these puts the result type in word 1 and the result id in
        // word 2. An index produced by anything else makes the module refuse
        // rather than guess a type.
        switch (in.op) {
        case OpLoad:
        case OpAccessChain:
        case OpIAdd:
        case OpIMul:
        case OpBitcast:
        case 66:  // OpInBoundsAccessChain
        case 81:  // OpCompositeExtract
        case 113: // OpUConvert
        case 114: // OpSConvert
        case 169: // OpSelect
        case 194: // OpShiftRightLogical
        case 199: // OpBitwiseAnd
            if (in.len >= 3)
                res_type[w[2]] = w[1];
            break;
        default:
            break;
        }
    }
    if (!entry_fn || !first_fn)
        return false;

    // The table: a variable at (set, binding) whose pointee is an *array* of
    // images or sampled images. A plain texture is refused because there is no
    // index to offset -- and because that is the shape the abandoned approach
    // handled, so silently accepting it here would resurrect it.
    // EVERY variable at (set, binding), not the first one.
    //
    // Take forty-eight: this used to `break` at the first match, and X4's
    // present shader (module #12, the pass that composites into the swapchain)
    // declares *two* variables on set 0 binding 5 -- `S_sampler2D_AUTOMS`,
    // indexed by a literal, and `S_sampler2D`, indexed from the dynamic block.
    // Aliasing one binding with two variables is legal and X4 does it in 228 of
    // 409 dumped modules.
    //
    // The caller iterates tables, so it called this twice with the *same*
    // (set, binding). With the break, both calls targeted the same variable:
    // the first was offset twice -- index + 53306, past the end of a
    // 53306-element array, which reads as undefined and comes back black -- and
    // the second was never patched at all, so its view 1 stayed on view 0's
    // slot. That is the black right eye, and it is why the passes declaring one
    // variable per binding (rp #33 on binding 7) were visibly stereo in the
    // same run while the present pass was not.
    std::vector<uint32_t> target_vars;
    for (size_t s : var_starts) {
        const uint32_t *w = &code[s];
        const uint32_t id = w[2];
        auto si = set_of.find(id), bi = bind_of.find(id);
        if (si == set_of.end() || bi == bind_of.end())
            continue;
        if (si->second != want_set || bi->second != want_binding)
            continue;
        auto p = ptr_to.find(w[1]);
        if (p == ptr_to.end())
            continue;
        auto a = arr_elem.find(p->second);
        if (a == arr_elem.end())
            continue; // not an array: nothing to index
        if (!img_types.count(a->second) && !si_types.count(a->second))
            continue;
        target_vars.push_back(id);
    }
    if (target_vars.empty())
        return false;

    // Every access chain into the table, each with the type of its own index.
    //
    // The types genuinely differ within one module: X4 indexes the table with a
    // `uint` loaded from the dynamic block in one place and a literal `%int_21`
    // in another. Requiring one type across the module refused twelve otherwise
    // perfectly patchable shaders, which is why this is measured against the
    // dumped corpus rather than reasoned about.
    std::vector<std::pair<size_t, uint32_t>> chains;
    uint32_t cur_fn = 0;
    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op == OpFunction && in.len >= 3)
            cur_fn = w[2];
        if ((in.op != OpAccessChain && in.op != 66) || in.len < 5)
            continue;
        if (std::find(target_vars.begin(), target_vars.end(), w[3]) ==
            target_vars.end())
            continue;
        if (cur_fn != entry_fn)
            return false; // indexed from a helper: decline the whole module
        const uint32_t index = w[4];
        uint32_t t = 0;
        auto r = res_type.find(index);
        if (r != res_type.end())
            t = r->second;
        else {
            auto c = consts.find(index);
            if (c != consts.end())
                t = c->second.type;
        }
        if (!t || !int_width.count(t) || int_width[t] != 32)
            return false; // unknown or non-32-bit index type
        chains.push_back({in.start, t});
    }
    if (chains.empty())
        return false;

    // --- pass 2: the declarations to add ---------------------------------
    auto new_id = [&]() { return bound++; };
    std::vector<uint32_t> decls;
    // ViewIndex must be a 32-bit integer; signed is what glslc emits and what
    // every validator certainly accepts, so it is declared signed and bitcast to
    // the index's type when they differ. IAdd/IMul only require equal width, but
    // a free bitcast costs nothing and removes the argument.
    uint32_t t_int = t_int_signed;
    if (!t_int) {
        t_int = new_id();
        emit(decls, OpTypeInt, {t_int, 32, 1});
    }
    uint32_t ptr_in_int = 0;
    for (const auto &e : ptr_to)
        if (e.second == t_int) {
            // Only an Input pointer will do; re-check the storage class.
            for (const Inst &in : insts)
                if (in.op == OpTypePointer && in.len >= 4 &&
                    code[in.start + 1] == e.first &&
                    code[in.start + 2] == StorageClassInput)
                    ptr_in_int = e.first;
        }
    if (!ptr_in_int) {
        ptr_in_int = new_id();
        emit(decls, OpTypePointer, {ptr_in_int, StorageClassInput, t_int});
    }
    const uint32_t view_var = existing_view_var ? existing_view_var : new_id();
    if (!existing_view_var)
        emit(decls, OpVariable, {ptr_in_int, view_var, StorageClassInput});

    // One offset constant per index type in play, reusing an existing constant
    // where the module already has one of that type and value.
    std::unordered_map<uint32_t, uint32_t> off_of;
    for (const auto &ch : chains) {
        if (off_of.count(ch.second))
            continue;
        uint32_t k = 0;
        for (const auto &c : consts)
            if (c.second.type == ch.second && c.second.value == offset)
                k = c.first;
        if (!k) {
            k = new_id();
            emit(decls, OpConstant, {ch.second, k, offset});
        }
        off_of[ch.second] = k;
    }

    const size_t anno_at = last_annotation_end
                               ? last_annotation_end
                               : (have_first_global ? first_global : first_fn);
    std::unordered_map<size_t, uint32_t> chain_type(chains.begin(),
                                                    chains.end());

    // --- pass 3: rebuild --------------------------------------------------
    std::vector<uint32_t> out;
    out.reserve(code.size() + decls.size() + chains.size() * 16 + 32);
    out.insert(out.end(), code.begin(), code.begin() + 5);
    const size_t cap_at = caps_end ? caps_end : 5;
    const size_t ext_at = exts_end ? exts_end : cap_at;

    for (const Inst &in : insts) {
        const uint32_t *w = &code[in.start];
        if (!has_multiview_cap && in.start == cap_at)
            emit(out, OpCapability, {CapabilityMultiView});
        if (!has_multiview_ext && code[1] < 0x00010300u && in.start == ext_at) {
            static const uint32_t kExt[] = {0x5f565053u, 0x5f52484bu,
                                            0x746c756du, 0x65697669u,
                                            0x00000077u};
            out.push_back((uint32_t)((1 + 5) << 16) | OpExtension);
            for (uint32_t x : kExt)
                out.push_back(x);
        }
        if (in.start == anno_at && !existing_view_var) {
            emit(out, OpDecorate,
                 {view_var, DecorationBuiltIn, BuiltInViewIndex});
            emit(out, OpDecorate, {view_var, DecorationFlat});
        }
        if (in.start == first_fn)
            out.insert(out.end(), decls.begin(), decls.end());

        // ViewIndex joins the *fragment* entry point's interface only. This
        // module also carries a vertex entry point -- X4 ships both in one --
        // and giving it an input it never reads would be wrong.
        if (in.op == OpEntryPoint && in.len >= 3 &&
            w[1] == ExecutionModelFragment && !existing_view_var) {
            out.push_back((uint32_t)((in.len + 1) << 16) | OpEntryPoint);
            for (uint32_t j = 1; j < in.len; j++)
                out.push_back(w[j]);
            out.push_back(view_var);
            continue;
        }

        auto ct = chain_type.find(in.start);
        if (ct != chain_type.end()) {
            const uint32_t idx_type = ct->second;
            const uint32_t vi = new_id();
            emit(out, OpLoad, {t_int, vi, view_var});
            uint32_t v = vi;
            if (idx_type != t_int) {
                v = new_id();
                emit(out, OpBitcast, {idx_type, v, vi});
            }
            const uint32_t scaled = new_id();
            emit(out, OpIMul, {idx_type, scaled, v, off_of[idx_type]});
            const uint32_t sum = new_id();
            emit(out, OpIAdd, {idx_type, sum, w[4], scaled});
            out.push_back(w[0]);
            for (uint32_t j = 1; j < in.len; j++)
                out.push_back(j == 4 ? sum : w[j]);
            continue;
        }

        out.insert(out.end(), code.begin() + in.start,
                   code.begin() + in.start + in.len);
    }

    out[3] = bound;
    code.swap(out);
    return true;
}

/// Turns X4's volumetric fog composite into a passthrough. **Diagnostic only.**
///
/// **KNOWN TOO COARSE — take 63. Do not reuse without tightening it first.**
/// The signature below (declares a SubpassData image *and* samples a 3D one)
/// matches `mod-0182`, which take 63's own join shows bound to *nine* passes:
/// the main geometry passes rp #13/#16/#23 and all five depth-only shadow
/// passes. It is a general-purpose shader whose 3D sample is not the fog
/// volume, and zeroing it turned every 3D scene black, leaving only the HUD.
///
/// The measurement still succeeded — `#57`'s layer difference came back
/// bit-stable, which was the question — but the run was visually broken and
/// that was avoidable. Being bound to the fog pass is not the same as being
/// bound *only* to it, and I read the first as the second.
///
/// To tighten: require the 3D sample's result to feed the composite's shape —
/// component 3 into an OpVectorTimesScalar and components 0..2 into the OpFAdd
/// that consumes it. That is the fog composite specifically, rather than
/// "samples a 3D texture and happens to also have a subpass input".
///
/// The fog pass computes `OUT = scene * fog.a + fog.rgb`, where `fog` is a
/// froxel volume built by compute. Forcing that one sample to vec4(0,0,0,1)
/// leaves `OUT = scene * 1 + 0` -- the pass still runs, still reads its subpass
/// input, still writes its attachment, and the frame graph is untouched. Only
/// the term under test disappears.
///
/// It answers one question and no others: is the difference between the two
/// eyes in the HDR buffer *produced* by the fog composite, or merely *carried*
/// through it? Take 62 established that the froxel coordinate is not the
/// differentiator -- toggling the matrix that feeds it moved the lift by under
/// 1% -- which is what makes "carrier" a live possibility and this test worth
/// a run. If the eyes still differ with the fog term gone, the source is
/// upstream, among the other five passes that write the same image.
///
/// Refuses unless the module both declares a SubpassData image and samples a
/// 3D one. That pair is the structural signature of X4's fog family: 8 of the
/// 409 dumped modules match, and every module bound to the fog pass is among
/// them. Identifying by structure rather than by serial is deliberate --
/// module serials are per-run, so a hardcoded list would silently rot.
///
/// A refusal leaves the module byte-identical, which the offline suite asserts.
inline bool patch_fragment_disable_fog(std::vector<uint32_t> &code) {
    std::vector<Inst> insts;
    if (!iterate(code, insts))
        return false;

    // Dim: 2 = 3D, 6 = SubpassData. Both are read from OpTypeImage word 3.
    std::unordered_set<uint32_t> img3d;
    bool has_subpass = false;
    uint32_t t_float = 0, t_v4float = 0;
    for (const auto &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op == OpTypeImage && in.len >= 4) {
            if (w[3] == 2u)
                img3d.insert(w[1]);
            else if (w[3] == 6u)
                has_subpass = true;
        } else if (in.op == OpTypeFloat && in.len >= 3 && w[2] == 32u &&
                   !t_float) {
            t_float = w[1];
        }
    }
    if (!has_subpass || img3d.empty() || !t_float)
        return false;
    for (const auto &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op == OpTypeVector && in.len >= 4 && w[2] == t_float &&
            w[3] == 4u) {
            t_v4float = w[1];
            break;
        }
    }
    if (!t_v4float)
        return false;

    // Follow the chain from the 3D image type to the sample that uses it:
    // OpTypeImage -> OpLoad (result type is the image) -> OpSampledImage ->
    // OpImageSample*. Walking it is what keeps this off the module's *other*
    // samples -- mod-0368 has two OpImageSampleExplicitLod and only one of
    // them is the fog lookup. Rewriting both would have disabled something
    // unrelated and made the measurement unreadable.
    std::unordered_set<uint32_t> load3d, si3d;
    for (const auto &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op == OpLoad && in.len >= 4 && img3d.count(w[1]))
            load3d.insert(w[2]);
    }
    for (const auto &in : insts) {
        const uint32_t *w = &code[in.start];
        if (in.op == OpSampledImage && in.len >= 4 && load3d.count(w[3]))
            si3d.insert(w[2]);
    }
    std::unordered_set<size_t> targets;
    for (size_t i = 0; i < insts.size(); i++) {
        const auto &in = insts[i];
        const uint32_t *w = &code[in.start];
        if ((in.op == OpImageSampleExplicitLod ||
             in.op == OpImageSampleImplicitLod) &&
            in.len >= 5 && si3d.count(w[3]) && w[1] == t_v4float)
            targets.insert(i);
    }
    if (targets.empty())
        return false;

    // Two float constants, appended to the declarations rather than reused.
    // Duplicate OpConstants of the same value are legal, and searching for an
    // existing one would have to match the bit pattern rather than the name --
    // the same trap that made the "19 modules" count wrong.
    uint32_t bound = code[3];
    const uint32_t c_zero = bound++, c_one = bound++;
    std::vector<uint32_t> consts;
    const float f0 = 0.0f, f1 = 1.0f;
    uint32_t b0, b1;
    memcpy(&b0, &f0, 4);
    memcpy(&b1, &f1, 4);
    emit(consts, OpConstant, {t_float, c_zero, b0});
    emit(consts, OpConstant, {t_float, c_one, b1});

    // Rebuild: constants go in before the first function, each fog sample
    // becomes an OpCompositeConstruct keeping its original result id, so every
    // downstream use stays valid without touching a single other instruction.
    std::vector<uint32_t> out(code.begin(), code.begin() + 5);
    bool placed = false;
    for (size_t i = 0; i < insts.size(); i++) {
        const auto &in = insts[i];
        const uint32_t *w = &code[in.start];
        if (!placed && in.op == OpFunction) {
            out.insert(out.end(), consts.begin(), consts.end());
            placed = true;
        }
        if (targets.count(i))
            emit(out, OpCompositeConstruct,
                 {t_v4float, w[2], c_zero, c_zero, c_zero, c_one});
        else
            out.insert(out.end(), code.begin() + in.start,
                       code.begin() + in.start + in.len);
    }
    if (!placed)
        return false; // no function body: nothing to disable, change nothing
    out[3] = bound;
    code.swap(out);
    return true;
}

} // namespace spv
} // namespace x4vr
