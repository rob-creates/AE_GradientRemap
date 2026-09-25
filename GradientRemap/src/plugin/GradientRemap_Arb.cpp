// GradientRemap_Arb.cpp -- PF_ADD_ARBITRARY2 callback set for the multi-knot gradient
// parameter (GRADREMAP_GRADIENT), modelled directly on the AE SDK's own ColorGrid
// sample (Examples/UI/ColorGrid/ColorGrid_Arb_Handler.cpp).
//
// Design choice: the *live* PF_ArbitraryH's bytes ARE
// GradientRemap::GradientData::Flatten()'s output directly -- there is no separate POD
// "live" struct distinct from the serialized form. ColorGrid's own CG_ArbData is a
// fixed-size POD array, so its FLATTEN/UNFLATTEN are trivial memcpys; our knot list is
// variable-length (up to 256 knots), so instead of inventing a parallel fixed-array
// mirror struct, we let the already-tested, already-versioned GradientData::Flatten()/
// Unflatten() format (magic+version+interp_mode+path+knot_count+knots) be the handle's
// content at all times. This means FLAT_SIZE/FLATTEN are just "how big is the handle" /
// "memcpy the handle" -- no re-serialization step -- and only UNFLATTEN/INTERP/COMPARE
// actually need to interpret the bytes, via the exact same GradientData the core render
// path and GradientRemapCoreTests already exercise.

#include "GradientRemap.h"

#include <cstring>

using GradientRemap::GradientData;
using GradientRemap::GradientKnot;

namespace {

// Allocate a new PF_Handle whose bytes are exactly `g`'s flattened form.
PF_Handle NewHandleFromGradient(PF_InData* in_data, const GradientData& g) {
    std::vector<uint8_t> flat = g.Flatten();
    PF_Handle h = PF_NEW_HANDLE(static_cast<A_long>(flat.size()));
    if (h) {
        void* p = PF_LOCK_HANDLE(h);
        if (p) {
            memcpy(p, flat.data(), flat.size());
        }
        PF_UNLOCK_HANDLE(h);
    }
    return h;
}

// Best-effort unflatten of a raw handle: falls back to a validated/default gradient
// rather than ever handing back an unusable (e.g. <2 knot) result, matching
// GradientData::Unflatten()'s own defensive posture toward corrupt project data.
GradientData UnflattenHandleOrDefault(PF_InData* in_data, PF_Handle h) {
    if (h) {
        A_u_long size = static_cast<A_u_long>(PF_GET_HANDLE_SIZE(h));
        const void* p = PF_LOCK_HANDLE(h);
        if (p && size > 0) {
            auto parsed = GradientData::Unflatten(reinterpret_cast<const uint8_t*>(p), size);
            PF_UNLOCK_HANDLE(h);
            if (parsed) {
                return *parsed;
            }
        } else {
            PF_UNLOCK_HANDLE(h);
        }
    }
    return GradientData::Default();
}

} // namespace

GradientData GradientRemap_UnflattenArbHandle(PF_InData* in_data, PF_Handle arbH) {
    return UnflattenHandleOrDefault(in_data, arbH);
}

PF_Err GradientRemap_ReflattenIntoHandle(PF_InData* in_data, const GradientData& g, PF_Handle* arbHP) {
    if (!arbHP) return PF_Err_BAD_CALLBACK_PARAM;
    std::vector<uint8_t> flat = g.Flatten();
    PF_Err err = PF_RESIZE_HANDLE(static_cast<A_long>(flat.size()), arbHP);
    if (!err && *arbHP) {
        void* p = PF_LOCK_HANDLE(*arbHP);
        if (p) {
            memcpy(p, flat.data(), flat.size());
        }
        PF_UNLOCK_HANDLE(*arbHP);
    }
    return err;
}

PF_Err GradientRemap_CreateDefaultArbHandle(PF_InData* in_data, PF_Handle* arbHP) {
    if (!arbHP) return PF_Err_BAD_CALLBACK_PARAM;
    *arbHP = NewHandleFromGradient(in_data, GradientData::Default());
    return *arbHP ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
}

namespace {

PF_Err ArbNew(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    if (extra->u.new_func_params.refconPV != GRADIENT_ARB_REFCON) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    *extra->u.new_func_params.arbPH = NewHandleFromGradient(in_data, GradientData::Default());
    return *extra->u.new_func_params.arbPH ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
}

PF_Err ArbDispose(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    if (extra->u.dispose_func_params.refconPV != GRADIENT_ARB_REFCON) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    PF_DISPOSE_HANDLE(extra->u.dispose_func_params.arbH);
    return PF_Err_NONE;
}

PF_Err ArbCopy(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    if (extra->u.copy_func_params.refconPV != GRADIENT_ARB_REFCON) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    PF_Handle src = extra->u.copy_func_params.src_arbH;
    if (!src) {
        *extra->u.copy_func_params.dst_arbPH = NewHandleFromGradient(in_data, GradientData::Default());
        return PF_Err_NONE;
    }
    A_long size = static_cast<A_long>(PF_GET_HANDLE_SIZE(src));
    PF_Handle dst = PF_NEW_HANDLE(size);
    if (dst) {
        void* srcP = PF_LOCK_HANDLE(src);
        void* dstP = PF_LOCK_HANDLE(dst);
        if (srcP && dstP) {
            memcpy(dstP, srcP, static_cast<size_t>(size));
        }
        PF_UNLOCK_HANDLE(dst);
        PF_UNLOCK_HANDLE(src);
    }
    *extra->u.copy_func_params.dst_arbPH = dst;
    return dst ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
}

PF_Err ArbFlatSize(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    *extra->u.flat_size_func_params.flat_data_sizePLu =
        static_cast<A_u_long>(PF_GET_HANDLE_SIZE(extra->u.flat_size_func_params.arbH));
    return PF_Err_NONE;
}

PF_Err ArbFlatten(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    PF_Handle h = extra->u.flatten_func_params.arbH;
    A_u_long handleSize = static_cast<A_u_long>(PF_GET_HANDLE_SIZE(h));
    A_u_long bufSize = extra->u.flatten_func_params.buf_sizeLu;
    void* src = PF_LOCK_HANDLE(h);
    if (src && bufSize >= handleSize) {
        memcpy(extra->u.flatten_func_params.flat_dataPV, src, handleSize);
    }
    PF_UNLOCK_HANDLE(h);
    return PF_Err_NONE;
}

PF_Err ArbUnflatten(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    // Validate through GradientData::Unflatten() (defensive against a hand-edited or
    // corrupted project file) and re-flatten the canonical result, rather than trusting
    // the incoming bytes verbatim.
    GradientData g = GradientData::Default();
    auto parsed = GradientData::Unflatten(reinterpret_cast<const uint8_t*>(extra->u.unflatten_func_params.flat_dataPV),
                                           extra->u.unflatten_func_params.buf_sizeLu);
    if (parsed) {
        g = *parsed;
    }
    PF_Handle h = NewHandleFromGradient(in_data, g);
    *extra->u.unflatten_func_params.arbPH = h;
    return h ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
}

float LerpF(float a, float b, float t) { return a + (b - a) * static_cast<float>(t); }

PF_Err ArbInterp(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    if (extra->u.interp_func_params.refconPV != GRADIENT_ARB_REFCON) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    GradientData left = UnflattenHandleOrDefault(in_data, extra->u.interp_func_params.left_arbH);
    GradientData right = UnflattenHandleOrDefault(in_data, extra->u.interp_func_params.right_arbH);
    double t = extra->u.interp_func_params.tF;

    GradientData result = left; // interpolation_mode/path are overridden by stock params anyway
    if (left.knots.size() == right.knots.size()) {
        for (size_t i = 0; i < result.knots.size(); ++i) {
            GradientKnot& k = result.knots[i];
            const GradientKnot& l = left.knots[i];
            const GradientKnot& r = right.knots[i];
            k.position = LerpF(l.position, r.position, static_cast<float>(t));
            k.r = LerpF(l.r, r.r, static_cast<float>(t));
            k.g = LerpF(l.g, r.g, static_cast<float>(t));
            k.b = LerpF(l.b, r.b, static_cast<float>(t));
            k.a = LerpF(l.a, r.a, static_cast<float>(t));
        }
        result.SortKnots();
    }
    // Structural mismatch (knot count differs between keyframes): snap to the left
    // keyframe rather than attempting to morph topologically-different knot lists.

    PF_Handle h = NewHandleFromGradient(in_data, result);
    *extra->u.interp_func_params.interpPH = h;
    return h ? PF_Err_NONE : PF_Err_OUT_OF_MEMORY;
}

PF_Err ArbCompare(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    PF_Handle a = extra->u.compare_func_params.a_arbH;
    PF_Handle b = extra->u.compare_func_params.b_arbH;
    A_u_long sizeA = static_cast<A_u_long>(PF_GET_HANDLE_SIZE(a));
    A_u_long sizeB = static_cast<A_u_long>(PF_GET_HANDLE_SIZE(b));

    PF_ArbCompareResult result = PF_ArbCompare_EQUAL;
    if (sizeA != sizeB) {
        result = PF_ArbCompare_NOT_EQUAL;
    } else {
        void* pa = PF_LOCK_HANDLE(a);
        void* pb = PF_LOCK_HANDLE(b);
        if (!pa || !pb || memcmp(pa, pb, sizeA) != 0) {
            result = PF_ArbCompare_NOT_EQUAL;
        }
        PF_UNLOCK_HANDLE(a);
        PF_UNLOCK_HANDLE(b);
    }
    *extra->u.compare_func_params.compareP = result;
    return PF_Err_NONE;
}

constexpr A_u_long kMaxPrintSize = 256;

PF_Err ArbPrintSize(PF_ArbParamsExtra* extra) {
    *extra->u.print_size_func_params.print_sizePLu = kMaxPrintSize;
    return PF_Err_NONE;
}

PF_Err ArbPrint(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    GradientData g = UnflattenHandleOrDefault(in_data, extra->u.print_func_params.arbH);
    if (extra->u.print_func_params.print_sizeLu > 0 && extra->u.print_func_params.print_bufferPC) {
        PF_SPRINTF(extra->u.print_func_params.print_bufferPC, "Gradient (%d knots)",
                   static_cast<int>(g.knots.size()));
    }
    return PF_Err_NONE;
}

// SCAN (text -> arb data, for "paste"/text-based project search-replace) is left
// unimplemented, matching the vendored SDK's own ColorGrid sample -- Adobe ships that
// selector as dead/commented-out code even in their reference plugin. Low value versus
// the actual gradient-editor feature; AE tolerates a callback that reports success
// without producing new data here.
PF_Err ArbScan(PF_InData* in_data, PF_ArbParamsExtra* extra) {
    *extra->u.scan_func_params.arbPH = NewHandleFromGradient(in_data, GradientData::Default());
    return PF_Err_NONE;
}

} // namespace

PF_Err GradientRemap_HandleArbitrary(PF_InData* in_data, PF_OutData* /*out_data*/, PF_ParamDef* /*params*/[],
                                      PF_LayerDef* /*output*/, PF_ArbParamsExtra* extra) {
    switch (extra->which_function) {
        case PF_Arbitrary_NEW_FUNC:
            return ArbNew(in_data, extra);
        case PF_Arbitrary_DISPOSE_FUNC:
            return ArbDispose(in_data, extra);
        case PF_Arbitrary_COPY_FUNC:
            return ArbCopy(in_data, extra);
        case PF_Arbitrary_FLAT_SIZE_FUNC:
            return ArbFlatSize(in_data, extra);
        case PF_Arbitrary_FLATTEN_FUNC:
            return ArbFlatten(in_data, extra);
        case PF_Arbitrary_UNFLATTEN_FUNC:
            return ArbUnflatten(in_data, extra);
        case PF_Arbitrary_INTERP_FUNC:
            return ArbInterp(in_data, extra);
        case PF_Arbitrary_COMPARE_FUNC:
            return ArbCompare(in_data, extra);
        case PF_Arbitrary_PRINT_SIZE_FUNC:
            return ArbPrintSize(extra);
        case PF_Arbitrary_PRINT_FUNC:
            return ArbPrint(in_data, extra);
        case PF_Arbitrary_SCAN_FUNC:
            return ArbScan(in_data, extra);
        default:
            return PF_Err_NONE;
    }
}
