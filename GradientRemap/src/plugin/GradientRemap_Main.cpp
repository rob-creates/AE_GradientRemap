// GradientRemap_Main.cpp -- Phase 2 (docs/DESIGN.md): a minimal SmartFX AE plugin
// wrapping the validated GradientRemapCore behind a throwaway fixed 4-knot stock-param
// UI, driven through real PF_Cmd_SMART_RENDER calls at all three bit depths. This is
// NOT the final custom multi-knot gradient-bar UI (that's Phase 3) -- its only job is
// to prove the core renders correctly inside real After Effects.
//
// Known Phase 2 simplifications (see docs/DESIGN.md):
//  - Stock PF_ADD_COLOR knots have no per-knot alpha (AE's colour swatch has no alpha
//    channel), so knot alpha is always 1.0 here; real per-knot alpha arrives with the
//    Phase 3 arbitrary-data knot list, alongside its own independent alpha-knot gradient
//    (see the enum comment in GradientRemap.h) -- source alpha passes through unchanged
//    here in the meantime.

#include "GradientRemap.h"
#include "GradientRemap_ColorSpace.h"

#include <algorithm>
#include <cmath>

#include "../core/GradientData.h"
#include "../core/Interpolation.h"

using GradientRemap::GradientData;
using GradientRemap::GradientKnot;
using GradientRemap::InterpMode;
using GradientRemap::InterpPath;
using GradientRemap::RGBAf;

namespace {

// Rec.709 relative luminance, hardcoded (not user-facing -- see GradientRemap.h). This
// matches sRGB/Rec.709 primaries, the overwhelming common case, and "just works" without
// requiring the user to understand or choose a luma model.
float ComputeLuma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// In-range blending goes through the tested GradientRemapCore dispatch (respects the
// selected interpolation mode). Outside the authored knot range, we linearly extend
// from the boundary pair -- a plugin-layer policy (not a GradientRemapCore concern; see
// Interpolation.h) so 32bpc HDR/negative luma values can extrapolate past the chosen
// stop colours instead of flattening, per the original brief's clamp-toggle intent.
RGBAf EvaluateGradientExtrapolated(const GradientData& g, float t, float working_space_gamma) {
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
    return GradientRemap::EvaluateGradient(g, t, working_space_gamma);
}

struct RemapRefcon {
    GradientData gradient;
    PF_Boolean clamp_input;
    PF_Boolean dither;
    float deband_threshold; // see RemapPixel8's threshold-gated blur
    float working_space_gamma; // 0.0f = precise sRGB curve (see GradientRemap_ColorSpace.h)
    const PF_EffectWorld* input_world; // only used by the 8bpc anti-banding blur (RemapPixel8)
};

float ClampUnit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float SmoothStep01(float t) { return t * t * (3.0f - 2.0f * t); }

// Standard 8x8 Bayer ordered-dither matrix (all 64 values 0-63, unique). Stateless,
// derived purely from (x,y).
constexpr int kBayer8x8[8][8] = {
    {0, 48, 12, 60, 3, 51, 15, 63},   {32, 16, 44, 28, 35, 19, 47, 31}, {8, 56, 4, 52, 11, 59, 7, 55},
    {40, 24, 36, 20, 43, 27, 39, 23}, {2, 50, 14, 62, 1, 49, 13, 61},   {34, 18, 46, 30, 33, 17, 45, 29},
    {10, 58, 6, 54, 9, 57, 5, 53},    {42, 26, 38, 22, 41, 25, 37, 21}};

float BayerDitherOffset8(A_long x, A_long y) {
    int bx = static_cast<int>(x & 7);
    int by = static_cast<int>(y & 7);
    float normalized = (kBayer8x8[by][bx] + 0.5f) / 64.0f; // (0,1)
    return (normalized - 0.5f) / static_cast<float>(PF_MAX_CHAN8);
}

// Real anti-banding fix (2026-09-25): pure per-pixel dithering (jittering t by up to
// +/-N LSBs before the lookup, tried first at N=0.5 then N=3) never adequately broke up
// banding, and the reason turned out to be upstream of our maths entirely -- the user
// confirmed the RAW, un-remapped source gradient (AE's own Shape Layer "Gradient Fill",
// no effect applied at all) shows the *same* hard banding at 8bpc. AE's own gradient
// rendering is coarser than the theoretical 8-bit limit here. No amount of dithering
// around a single sample can fix that: dithering can only jitter the ONE value a pixel
// already has, it cannot discover that a real neighbouring pixel holds different
// information. A real spatial blur can, because it reads actual neighbouring pixels --
// exactly what the user's manual Photoshop blur test demonstrated works.
//
// So: average real neighbouring source pixels' luma (a small box blur, radius
// kBandingBlurRadius) before the gradient lookup, then add a small residual Bayer
// jitter on top to smooth out the blur's own step-like radius. This does soften real
// high-frequency detail in non-gradient source content by the same radius -- an
// inherent tradeoff of any spatial anti-banding filter, which is why it's opt-in via
// the same checkbox rather than always-on.
constexpr A_long kBandingBlurRadius = 2; // 5x5 = 25 taps

struct LumaAlphaSample {
    float luma;
    float alpha;
};

LumaAlphaSample SampleLumaAlpha8Clamped(const PF_EffectWorld* world, A_long x, A_long y) {
    A_long cx = x < 0 ? 0 : (x >= world->width ? world->width - 1 : x);
    A_long cy = y < 0 ? 0 : (y >= world->height ? world->height - 1 : y);
    const char* row = reinterpret_cast<const char*>(world->data) + static_cast<size_t>(cy) * static_cast<size_t>(world->rowbytes);
    const PF_Pixel8* pixel = reinterpret_cast<const PF_Pixel8*>(row) + cx;
    float luma = ComputeLuma(pixel->red / static_cast<float>(PF_MAX_CHAN8), pixel->green / static_cast<float>(PF_MAX_CHAN8),
                              pixel->blue / static_cast<float>(PF_MAX_CHAN8));
    return {luma, pixel->alpha / static_cast<float>(PF_MAX_CHAN8)};
}

struct BlurredLumaResult {
    float blurredLuma;
    float lumaMin;
    float lumaMax;
};

// Alpha-weighted: a neighbour's contribution is scaled by its OWN alpha, so blurring
// near a shape's edge doesn't pull in colour from the transparent area outside it (which
// would otherwise read as a softened/fringed edge, even though alpha itself is never
// touched -- see docs/DESIGN.md, user feedback 2026-09-25 "hold colour/alpha values at
// edges"). Falls back to the unblurred centre luma if the whole neighbourhood is
// transparent (nothing sensible to blur toward). Also reports the (alpha-gated) luma
// range actually sampled, so the caller can tell whether this neighbourhood spans a
// genuine colour transition rather than sub-LSB quantization noise -- see the threshold
// gate in RemapPixel8.
BlurredLumaResult BlurredLuma8(const PF_EffectWorld* world, A_long x, A_long y, float centerLuma) {
    float weightedSum = 0.0f;
    float weightTotal = 0.0f;
    float lumaMin = centerLuma;
    float lumaMax = centerLuma;
    constexpr float kOpaqueEnough = 1.0f / 255.0f;
    for (A_long dy = -kBandingBlurRadius; dy <= kBandingBlurRadius; ++dy) {
        for (A_long dx = -kBandingBlurRadius; dx <= kBandingBlurRadius; ++dx) {
            LumaAlphaSample s = SampleLumaAlpha8Clamped(world, x + dx, y + dy);
            weightedSum += s.luma * s.alpha;
            weightTotal += s.alpha;
            if (s.alpha >= kOpaqueEnough) {
                lumaMin = std::min(lumaMin, s.luma);
                lumaMax = std::max(lumaMax, s.luma);
            }
        }
    }
    constexpr float kMinWeight = 1.0f / 255.0f; // ~1 fully-opaque-equivalent sample's worth
    float blurred = (weightTotal < kMinWeight) ? centerLuma : (weightedSum / weightTotal);
    return {blurred, lumaMin, lumaMax};
}

PF_Err RemapPixel8(void* refcon, A_long x, A_long y, PF_Pixel8* inP, PF_Pixel8* outP) {
    auto* rc = static_cast<RemapRefcon*>(refcon);

    float t;
    if (rc->dither) {
        float centerLuma = ComputeLuma(inP->red / static_cast<float>(PF_MAX_CHAN8), inP->green / static_cast<float>(PF_MAX_CHAN8),
                                        inP->blue / static_cast<float>(PF_MAX_CHAN8));
        BlurredLumaResult blur = BlurredLuma8(rc->input_world, x, y, centerLuma);

        // Threshold gate: if the gradient's OUTPUT colour differs a lot between this
        // neighbourhood's darkest and lightest sampled luma, blurring would be averaging
        // across a genuine, intentional transition (Step's hard edges, or two knots
        // placed close together) rather than smoothing sub-LSB source quantization noise.
        // Ramp the blur weight down to 0 as that jump approaches the threshold, rather
        // than a hard on/off cut, so debanding doesn't leave a visible seam of its own.
        RGBAf atMin = EvaluateGradientExtrapolated(rc->gradient, blur.lumaMin, rc->working_space_gamma);
        RGBAf atMax = EvaluateGradientExtrapolated(rc->gradient, blur.lumaMax, rc->working_space_gamma);
        float colorJump = std::max({std::fabs(atMax.r - atMin.r), std::fabs(atMax.g - atMin.g), std::fabs(atMax.b - atMin.b)});
        float jumpFraction = (rc->deband_threshold > 0.0f) ? ClampUnit(colorJump / rc->deband_threshold) : 1.0f;
        float blurWeight = 1.0f - SmoothStep01(jumpFraction);

        float lumaForLookup = centerLuma + (blur.blurredLuma - centerLuma) * blurWeight;
        t = lumaForLookup + BayerDitherOffset8(x, y);
    } else {
        float r = inP->red / static_cast<float>(PF_MAX_CHAN8);
        float g = inP->green / static_cast<float>(PF_MAX_CHAN8);
        float b = inP->blue / static_cast<float>(PF_MAX_CHAN8);
        t = ComputeLuma(r, g, b);
    }
    RGBAf out = EvaluateGradientExtrapolated(rc->gradient, t, rc->working_space_gamma);

    outP->red = static_cast<A_u_char>(ClampUnit(out.r) * PF_MAX_CHAN8 + 0.5f);
    outP->green = static_cast<A_u_char>(ClampUnit(out.g) * PF_MAX_CHAN8 + 0.5f);
    outP->blue = static_cast<A_u_char>(ClampUnit(out.b) * PF_MAX_CHAN8 + 0.5f);
    outP->alpha = inP->alpha; // alpha remap: not yet implemented as its own knot gradient (Phase 3)

    return PF_Err_NONE;
}

PF_Err RemapPixel16(void* refcon, A_long /*x*/, A_long /*y*/, PF_Pixel16* inP, PF_Pixel16* outP) {
    auto* rc = static_cast<RemapRefcon*>(refcon);

    float r = inP->red / static_cast<float>(PF_MAX_CHAN16);
    float g = inP->green / static_cast<float>(PF_MAX_CHAN16);
    float b = inP->blue / static_cast<float>(PF_MAX_CHAN16);

    float t = ComputeLuma(r, g, b);
    RGBAf out = EvaluateGradientExtrapolated(rc->gradient, t, rc->working_space_gamma);

    outP->red = static_cast<A_u_short>(ClampUnit(out.r) * PF_MAX_CHAN16 + 0.5f);
    outP->green = static_cast<A_u_short>(ClampUnit(out.g) * PF_MAX_CHAN16 + 0.5f);
    outP->blue = static_cast<A_u_short>(ClampUnit(out.b) * PF_MAX_CHAN16 + 0.5f);
    outP->alpha = inP->alpha; // alpha remap: not yet implemented as its own knot gradient (Phase 3)

    return PF_Err_NONE;
}

PF_Err RemapPixelFloat(void* refcon, A_long /*x*/, A_long /*y*/, PF_PixelFloat* inP, PF_PixelFloat* outP) {
    auto* rc = static_cast<RemapRefcon*>(refcon);

    float t = ComputeLuma(static_cast<float>(inP->red), static_cast<float>(inP->green), static_cast<float>(inP->blue));
    if (rc->clamp_input) {
        t = ClampUnit(t);
    }
    RGBAf out = EvaluateGradientExtrapolated(rc->gradient, t, rc->working_space_gamma);

    // 32bpc float is not range-limited: HDR/negative values from extrapolation are
    // preserved as-is (no clamp), matching the original brief's clamp-toggle intent.
    outP->red = out.r;
    outP->green = out.g;
    outP->blue = out.b;
    outP->alpha = inP->alpha; // alpha remap: not yet implemented as its own knot gradient (Phase 3)

    return PF_Err_NONE;
}

PF_Err BuildGradientFromParams(PF_InData* in_data, GradientData* gradient) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_ParamDef interp_mode_param, path_param, knot_color[4], knot_pos[4];
    AEFX_CLR_STRUCT(interp_mode_param);
    AEFX_CLR_STRUCT(path_param);
    for (int i = 0; i < 4; ++i) {
        AEFX_CLR_STRUCT(knot_color[i]);
        AEFX_CLR_STRUCT(knot_pos[i]);
    }

    ERR(PF_CHECKOUT_PARAM(
        in_data, GRADREMAP_INTERP_MODE, in_data->current_time, in_data->time_step, in_data->time_scale, &interp_mode_param));
    ERR(PF_CHECKOUT_PARAM(
        in_data, GRADREMAP_PATH, in_data->current_time, in_data->time_step, in_data->time_scale, &path_param));

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

        switch (path_param.u.pd.value) {
            case InterpPathPopup_LINEAR:
                gradient->path = InterpPath::Linear;
                break;
            case InterpPathPopup_STEP:
                gradient->path = InterpPath::Step;
                break;
            case InterpPathPopup_EASE:
                gradient->path = InterpPath::Ease;
                break;
            case InterpPathPopup_CUBIC:
            default:
                gradient->path = InterpPath::Cubic;
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
    ERR2(PF_CHECKIN_PARAM(in_data, &path_param));
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
    GradientRemap_RegisterWithAEGP(in_data); // enables the real working-space query in SmartRender
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* /*params*/[], PF_LayerDef* /*output*/) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Interpolation", 3, InterpModePopup_NAIVE, "Naive|Linear Light|OKLCH", INTERP_MODE_DISK_ID);

    // Subset of Cinema 4D's gradient path options (see GradientRemap.h) -- orthogonal to
    // "Interpolation" above, which only controls the colour SPACE the blend happens in.
    // Cubic first/default per user preference: smoothest path through multiple knots.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Path", 4, InterpPathPopup_CUBIC, "Cubic|Ease|Linear|Step", PATH_DISK_ID);

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
    PF_ADD_CHECKBOXX("Clamp Range (32bpc)", FALSE, 0, CLAMP_INPUT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Reduce Banding (8bpc)", FALSE, 0, DITHER_DISK_ID);

    // Gates the anti-banding blur by how large a colour jump it would smooth over (see
    // BlurredLuma8/RemapPixel8): below this, blend fully; at/above it, don't blend at
    // all (with a smooth ramp in between) so a genuine hard transition -- Step, or two
    // knots placed close together with contrasting colours -- doesn't get softened.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Debanding Threshold", 0.0, 1.0, 0.0, 1.0, 0.5f, 2, 0, 0, DEBAND_THRESHOLD_DISK_ID);

    out_data->num_params = GRADREMAP_NUM_PARAMS;
    return err;
}

static PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_result;

    req.preserve_rgb_of_zero_alpha = TRUE; // AE's PF_EffectWorld buffers are straight
                                            // alpha (see docs/DESIGN.md open question 3);
                                            // keep transparent-pixel RGB intact so the
                                            // gradient-remapped colour is still visible
                                            // there (source alpha always passes through
                                            // unchanged in this Phase 2 UI).

    ERR(extra->cb->checkout_layer(in_data->effect_ref, GRADREMAP_INPUT, GRADREMAP_INPUT, &req, in_data->current_time,
                                   in_data->time_step, in_data->time_scale, &in_result));

    UnionLRect(&in_result.result_rect, &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

    return err;
}

static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld *input_worldP = nullptr, *output_worldP = nullptr;
    PF_ParamDef clamp_param, dither_param, deband_threshold_param;
    AEFX_CLR_STRUCT(clamp_param);
    AEFX_CLR_STRUCT(dither_param);
    AEFX_CLR_STRUCT(deband_threshold_param);

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, GRADREMAP_INPUT, &input_worldP));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &output_worldP));

    ERR(PF_CHECKOUT_PARAM(in_data, GRADREMAP_CLAMP_INPUT, in_data->current_time, in_data->time_step,
                           in_data->time_scale, &clamp_param));
    ERR(PF_CHECKOUT_PARAM(in_data, GRADREMAP_DITHER, in_data->current_time, in_data->time_step, in_data->time_scale,
                           &dither_param));
    ERR(PF_CHECKOUT_PARAM(in_data, GRADREMAP_DEBAND_THRESHOLD, in_data->current_time, in_data->time_step,
                           in_data->time_scale, &deband_threshold_param));

    RemapRefcon refcon;
    if (!err) {
        refcon.clamp_input = clamp_param.u.bd.value;
        refcon.dither = dither_param.u.bd.value;
        refcon.deband_threshold = static_cast<float>(deband_threshold_param.u.fs_d.value);
        refcon.working_space_gamma = GradientRemap_QueryWorkingSpaceGamma(in_data);
        refcon.input_world = input_worldP;
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

    ERR2(PF_CHECKIN_PARAM(in_data, &clamp_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &dither_param));
    ERR2(PF_CHECKIN_PARAM(in_data, &deband_threshold_param));

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
