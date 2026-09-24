// GradientRemap_Main.cpp -- Phase 2 (docs/DESIGN.md): a minimal SmartFX AE plugin
// wrapping the validated GradientRemapCore behind a throwaway fixed 4-knot stock-param
// UI, driven through real PF_Cmd_SMART_RENDER calls at all three bit depths. This is
// NOT the final custom multi-knot gradient-bar UI (that's Phase 3) -- its only job is
// to prove the core renders correctly inside real After Effects.
//
// Known Phase 2 simplifications (see docs/DESIGN.md open questions):
//  - WorkingSpaceTransform still uses the Phase 1 hardcoded sRGB EOTF stand-in; the
//    real AEGP_ColorSettingsSuite6 query (AEGP_GetNewWorkingSpaceColorProfile /
//    AEGP_GetColorProfileApproximateGamma, or the OCIO path via
//    AEGP_IsOCIOColorManagementUsed) is not yet wired in.
//  - Stock PF_ADD_COLOR knots have no per-knot alpha (AE's colour swatch has no alpha
//    channel), so knot alpha is always 1.0 here; real per-knot alpha arrives with the
//    Phase 3 arbitrary-data knot list.
//  - No dithering yet (8bpc-only feature from the original brief); deferred to keep
//    this validation pass bounded.

#include "GradientRemap.h"

#include "../core/GradientData.h"
#include "../core/Interpolation.h"

using GradientRemap::GradientData;
using GradientRemap::GradientKnot;
using GradientRemap::InterpMode;
using GradientRemap::RGBAf;

namespace {

float ComputeLuma(float r, float g, float b, A_long formula) {
    switch (formula) {
        case LumaFormula_REC709:
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        case LumaFormula_REC601:
            return 0.299f * r + 0.587f * g + 0.114f * b;
        case LumaFormula_AVERAGE:
        default:
            return (r + g + b) / 3.0f;
    }
}

// In-range blending goes through the tested GradientRemapCore dispatch (respects the
// selected interpolation mode). Outside the authored knot range, we linearly extend
// from the boundary pair -- a plugin-layer policy (not a GradientRemapCore concern; see
// Interpolation.h) so 32bpc HDR/negative luma values can extrapolate past the chosen
// stop colours instead of flattening, per the original brief's clamp-toggle intent.
RGBAf EvaluateGradientExtrapolated(const GradientData& g, float t) {
    const auto& knots = g.knots;
    if (t < knots.front().position) {
        const GradientKnot& a = knots[0];
        const GradientKnot& b = knots[1];
        float span = b.position - a.position;
        float slope = (span > 0.0f) ? (t - a.position) / span : 0.0f;
        return RGBAf{a.r + (b.r - a.r) * slope, a.g + (b.g - a.g) * slope, a.b + (b.b - a.b) * slope,
                     a.a + (b.a - a.a) * slope};
    }
    if (t > knots.back().position) {
        const GradientKnot& a = knots[knots.size() - 2];
        const GradientKnot& b = knots.back();
        float span = b.position - a.position;
        float slope = (span > 0.0f) ? (t - a.position) / span : 1.0f;
        return RGBAf{a.r + (b.r - a.r) * slope, a.g + (b.g - a.g) * slope, a.b + (b.b - a.b) * slope,
                     a.a + (b.a - a.a) * slope};
    }
    return GradientRemap::EvaluateGradient(g, t);
}

struct RemapRefcon {
    GradientData gradient;
    A_long luma_formula;
    PF_Boolean clamp_input;
    PF_Boolean remap_alpha;
};

float ClampUnit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

PF_Err RemapPixel8(void* refcon, A_long /*x*/, A_long /*y*/, PF_Pixel8* inP, PF_Pixel8* outP) {
    auto* rc = static_cast<RemapRefcon*>(refcon);

    float r = inP->red / static_cast<float>(PF_MAX_CHAN8);
    float g = inP->green / static_cast<float>(PF_MAX_CHAN8);
    float b = inP->blue / static_cast<float>(PF_MAX_CHAN8);

    // 8bpc channels are already inherently within [0,1], so luma is too -- the clamp
    // checkbox is a no-op here by construction (matches the original brief: "only
    // relevant for 32bpc since integer formats are inherently clamped").
    float t = ComputeLuma(r, g, b, rc->luma_formula);
    RGBAf out = EvaluateGradientExtrapolated(rc->gradient, t);

    outP->red = static_cast<A_u_char>(ClampUnit(out.r) * PF_MAX_CHAN8 + 0.5f);
    outP->green = static_cast<A_u_char>(ClampUnit(out.g) * PF_MAX_CHAN8 + 0.5f);
    outP->blue = static_cast<A_u_char>(ClampUnit(out.b) * PF_MAX_CHAN8 + 0.5f);
    outP->alpha = rc->remap_alpha ? static_cast<A_u_char>(ClampUnit(out.a) * PF_MAX_CHAN8 + 0.5f) : inP->alpha;

    return PF_Err_NONE;
}

PF_Err RemapPixel16(void* refcon, A_long /*x*/, A_long /*y*/, PF_Pixel16* inP, PF_Pixel16* outP) {
    auto* rc = static_cast<RemapRefcon*>(refcon);

    float r = inP->red / static_cast<float>(PF_MAX_CHAN16);
    float g = inP->green / static_cast<float>(PF_MAX_CHAN16);
    float b = inP->blue / static_cast<float>(PF_MAX_CHAN16);

    float t = ComputeLuma(r, g, b, rc->luma_formula);
    RGBAf out = EvaluateGradientExtrapolated(rc->gradient, t);

    outP->red = static_cast<A_u_short>(ClampUnit(out.r) * PF_MAX_CHAN16 + 0.5f);
    outP->green = static_cast<A_u_short>(ClampUnit(out.g) * PF_MAX_CHAN16 + 0.5f);
    outP->blue = static_cast<A_u_short>(ClampUnit(out.b) * PF_MAX_CHAN16 + 0.5f);
    outP->alpha = rc->remap_alpha ? static_cast<A_u_short>(ClampUnit(out.a) * PF_MAX_CHAN16 + 0.5f) : inP->alpha;

    return PF_Err_NONE;
}

PF_Err RemapPixelFloat(void* refcon, A_long /*x*/, A_long /*y*/, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    auto* rc = static_cast<RemapRefcon*>(refcon);

    float t = ComputeLuma(static_cast<float>(inP->red), static_cast<float>(inP->green), static_cast<float>(inP->blue),
                           rc->luma_formula);
    if (rc->clamp_input) {
        t = ClampUnit(t);
    }
    RGBAf out = EvaluateGradientExtrapolated(rc->gradient, t);

    // 32bpc float is not range-limited: HDR/negative values from extrapolation are
    // preserved as-is (no clamp), matching the original brief's clamp-toggle intent.
    outP->red = out.r;
    outP->green = out.g;
    outP->blue = out.b;
    outP->alpha = rc->remap_alpha ? out.a : inP->alpha;

    return PF_Err_NONE;
}

PF_Err BuildGradientFromParams(PF_InData* in_data, GradientData* gradient) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_ParamDef interp_mode_param, knot_color[4], knot_pos[4];
    AEFX_CLR_STRUCT(interp_mode_param);
    for (int i = 0; i < 4; ++i) {
        AEFX_CLR_STRUCT(knot_color[i]);
        AEFX_CLR_STRUCT(knot_pos[i]);
    }

    ERR(PF_CHECKOUT_PARAM(
        in_data, GRADREMAP_INTERP_MODE, in_data->current_time, in_data->time_step, in_data->time_scale, &interp_mode_param));

    const int color_ids[4] = {GRADREMAP_KNOT0_COLOR, GRADREMAP_KNOT1_COLOR, GRADREMAP_KNOT2_COLOR, GRADREMAP_KNOT3_COLOR};
    const int pos_ids[4] = {GRADREMAP_KNOT0_POS, GRADREMAP_KNOT1_POS, GRADREMAP_KNOT2_POS, GRADREMAP_KNOT3_POS};

    for (int i = 0; i < 4 && !err; ++i) {
        ERR(PF_CHECKOUT_PARAM(in_data, color_ids[i], in_data->current_time, in_data->time_step, in_data->time_scale,
                               &knot_color[i]));
    }
    for (int i = 0; i < 4 && !err; ++i) {
        ERR(PF_CHECKOUT_PARAM(in_data, pos_ids[i], in_data->current_time, in_data->time_step, in_data->time_scale,
                               &knot_pos[i]));
    }

    if (!err) {
        switch (interp_mode_param.u.pd.value) {
            case InterpModePopup_LINEAR_LIGHT:
                gradient->interpolation_mode = InterpMode::LinearLight;
                break;
            case InterpModePopup_OKLCH:
                gradient->interpolation_mode = InterpMode::OKLCH;
                break;
            case InterpModePopup_NAIVE:
            default:
                gradient->interpolation_mode = InterpMode::NaiveLerp;
                break;
        }

        gradient->knots.clear();
        for (int i = 0; i < 4; ++i) {
            GradientKnot k{};
            k.position = static_cast<float>(knot_pos[i].u.fs_d.value);
            k.r = knot_color[i].u.cd.value.red / 255.0f;
            k.g = knot_color[i].u.cd.value.green / 255.0f;
            k.b = knot_color[i].u.cd.value.blue / 255.0f;
            k.a = 1.0f; // stock PF_ADD_COLOR has no alpha channel -- see file header note
            gradient->knots.push_back(k);
        }
        gradient->SortKnots();
    }

    ERR2(PF_CHECKIN_PARAM(in_data, &interp_mode_param));
    for (int i = 0; i < 4; ++i) {
        ERR2(PF_CHECKIN_PARAM(in_data, &knot_color[i]));
        ERR2(PF_CHECKIN_PARAM(in_data, &knot_pos[i]));
    }

    return err;
}

PF_Err ActuallyRender(PF_InData* in_data, PF_OutData* out_data, PF_EffectWorld* input, PF_EffectWorld* output,
                       RemapRefcon* refcon) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_WorldSuite2* wsP = nullptr;
    PF_PixelFormat format = PF_PixelFormat_INVALID;

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, "Couldn't load suite.", (void**)&wsP));

    if (!err) {
        ERR(wsP->PF_GetPixelFormat(input, &format));
    }

    if (!err) {
        PF_Point origin;
        origin.h = in_data->output_origin_x;
        origin.v = in_data->output_origin_y;

        // area = NULL means "all pixels" (see PF_Iterate*Suite2 docs) -- in_data->extent_hint
        // is an old PF_Cmd_RENDER-era field and isn't reliably populated for SmartFX's
        // PF_Cmd_SMART_RENDER, so passing it here silently iterated zero pixels and left
        // the output buffer at whatever checkout_output handed back (reads as black).
        switch (format) {
            case PF_PixelFormat_ARGB128:
                ERR(suites.IterateFloatSuite2()->iterate_origin(in_data, 0, output->height, input, nullptr, &origin,
                                                                 (void*)refcon, RemapPixelFloat, output));
                break;
            case PF_PixelFormat_ARGB64:
                ERR(suites.Iterate16Suite2()->iterate_origin(in_data, 0, output->height, input, nullptr, &origin,
                                                              (void*)refcon, RemapPixel16, output));
                break;
            case PF_PixelFormat_ARGB32:
                ERR(suites.Iterate8Suite2()->iterate_origin(in_data, 0, output->height, input, nullptr, &origin,
                                                             (void*)refcon, RemapPixel8, output));
                break;
            default:
                err = PF_Err_BAD_CALLBACK_PARAM;
                break;
        }
    }

    ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, "Couldn't release suite."));
    return err;
}

} // namespace

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d\r%s", NAME, MAJOR_VERSION, MINOR_VERSION, DESCRIPTION);
    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
    out_data->out_flags |= PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_USE_OUTPUT_EXTENT;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER |
                            PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Luma Formula", 3, LumaFormula_REC709, "Rec.709|Rec.601|Average", LUMA_FORMULA_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Interpolation", 3, InterpModePopup_NAIVE, "Naive|Linear Light|OKLCH", INTERP_MODE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Knot 0 Colour", 0, 0, 0, KNOT0_COLOR_DISK_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Knot 0 Position", 0.0, 1.0, 0.0, 1.0, 0.0, 3, 0, 0, KNOT0_POS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Knot 1 Colour", 191, 26, 26, KNOT1_COLOR_DISK_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Knot 1 Position", 0.0, 1.0, 0.0, 1.0, 0.33f, 3, 0, 0, KNOT1_POS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Knot 2 Colour", 242, 204, 26, KNOT2_COLOR_DISK_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Knot 2 Position", 0.0, 1.0, 0.0, 1.0, 0.67f, 3, 0, 0, KNOT2_POS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR("Knot 3 Colour", 255, 255, 255, KNOT3_COLOR_DISK_ID);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Knot 3 Position", 0.0, 1.0, 0.0, 1.0, 1.0, 3, 0, 0, KNOT3_POS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Clamp Input to [0,1]", FALSE, 0, CLAMP_INPUT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Remap Alpha", FALSE, 0, REMAP_ALPHA_DISK_ID);

    out_data->num_params = GRADREMAP_NUM_PARAMS;
    return err;
}

static PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    req.preserve_rgb_of_zero_alpha = TRUE; // AE's PF_EffectWorld buffers are straight
                                            // alpha (see docs/DESIGN.md open question 3);
                                            // keep transparent-pixel RGB intact so a
                                            // gradient-remapped colour is still visible
                                            // there when alpha remap is off.

    ERR(extra->cb->checkout_layer(in_data->effect_ref, GRADREMAP_INPUT, GRADREMAP_INPUT, &req, in_data->current_time,
                                   in_data->time_step, in_data->time_scale, &in_result));

    UnionLRect(&in_result.result_rect, &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

    return err;
}

static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld *input_worldP = nullptr, *output_worldP = nullptr;
    PF_ParamDef clamp_param, remap_alpha_param;
    AEFX_CLR_STRUCT(clamp_param);
    AEFX_CLR_STRUCT(remap_alpha_param);

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, GRADREMAP_INPUT, &input_worldP));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_worldP));

    ERR(PF_CHECKOUT_PARAM(in_data, GRADREMAP_CLAMP_INPUT, in_data->current_time, in_data->time_step,
                           in_data->time_scale, &clamp_param));
    ERR(PF_CHECKOUT_PARAM(in_data, GRADREMAP_REMAP_ALPHA, in_data->current_time, in_data->time_step,
                           in_data->time_scale, &remap_alpha_param));

    RemapRefcon refcon;
    PF_ParamDef luma_formula_param;
    AEFX_CLR_STRUCT(luma_formula_param);
    ERR(PF_CHECKOUT_PARAM(in_data, GRADREMAP_LUMA_FORMULA, in_data->current_time, in_data->time_step,
                           in_data->time_scale, &luma_formula_param));

    if (!err) {
        refcon.luma_formula = luma_formula_param.u.pd.value;
        refcon.clamp_input = clamp_param.u.bd.value;
        refcon.remap_alpha = remap_alpha_param.u.bd.value;
        ERR(BuildGradientFromParams(in_data, &refcon.gradient));
    }

    if (!err && refcon.gradient.IsValid()) {
        ERR(ActuallyRender(in_data, out_data, input_worldP, output_worldP, &refcon));
    } else if (!err) {
        // Defensive fallback: if the gradient ever fails validation, pass the input
        // through unchanged rather than leaving the output at whatever checkout_output
        // handed back (which reads as solid black and is much harder to diagnose).
        ERR(PF_COPY(input_worldP, output_worldP, NULL, NULL));
    }

    ERR2(PF_CHECKIN_PARAM(in_data, &luma_formula_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &clamp_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &remap_alpha_param));

    return err;
}

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                      SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
                                                      const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, NAME, "ADBE Gradient Remap", "Sample Plug-ins",
                                      AE_RESERVED_INFO, "EffectMain", "https://www.adobe.com");
    return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output,
                   void* extra) {
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                err = About(in_data, out_data, params, output);
                break;
            case PF_Cmd_GLOBAL_SETUP:
                err = GlobalSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_PARAMS_SETUP:
                err = ParamsSetup(in_data, out_data, params, output);
                break;
            case PF_Cmd_SMART_PRE_RENDER:
                err = PreRender(in_data, out_data, reinterpret_cast<PF_PreRenderExtra*>(extra));
                break;
            case PF_Cmd_SMART_RENDER:
                err = SmartRender(in_data, out_data, reinterpret_cast<PF_SmartRenderExtra*>(extra));
                break;
        }
    } catch (PF_Err& thrown_err) {
        err = thrown_err;
    }
    return err;
}
