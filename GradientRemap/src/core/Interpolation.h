#pragma once

#include "GradientData.h"

namespace GradientRemap {

struct RGBAf {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

// Small scalar helpers shared across the plugin (GradientRemap_Main.cpp,
// GradientRemap_UI.cpp, GradientRemap_Arb.cpp) and the test harness, so each has exactly
// one definition instead of independent copies that can drift apart.
float ClampUnit(float v);
float Lerp(float a, float b, float t);
float SmoothStep(float t);

// Working-space <-> linear-light transfer function. `gamma <= 0` (the default) uses
// the precise sRGB piecewise EOTF/OETF -- this is the Phase 1 behaviour and what every
// existing GradientRemapCoreTests case exercises. `gamma > 0` uses a plain power-law
// curve (c^gamma / c^(1/gamma)) instead, driven by Phase 2's AEGP_ColorSettingsSuite6
// query of AE's actual project working space (see docs/DESIGN.md).
//
// This is a parameter, not mutable global state: SmartFX declares
// PF_OutFlag2_SUPPORTS_THREADED_RENDERING, so multiple frames (potentially from
// different comps with different working spaces) can render concurrently on different
// threads -- a shared "current transform" global would be a race condition.
struct WorkingSpaceTransform {
    static float ToLinear(float c, float gamma = 0.0f);
    static float ToWorkingSpace(float c, float gamma = 0.0f);
};

struct OKLab {
    float L = 0.0f, a = 0.0f, b = 0.0f;
};

struct OKLCH {
    float L = 0.0f, C = 0.0f, h = 0.0f; // h in radians
};

// Conversions operate on LINEAR-light RGB with Rec.709/sRGB primaries.
// Known v1 simplification: these matrices are calibrated for Rec.709 primaries; a
// working space with wider primaries (Rec.2020/ACEScg) will produce subtly incorrect
// hue results until the matrices are re-derived for that primary set (see open
// questions in docs/DESIGN.md).
OKLab LinearSRGBToOKLab(float r, float g, float b);
void OKLabToLinearSRGB(const OKLab& lab, float& r, float& g, float& b);
OKLCH OKLabToOKLCH(const OKLab& lab);
OKLab OKLCHToOKLab(const OKLCH& lch);

// Evaluate the gradient at parametric position t. t outside [knots.front().position,
// knots.back().position] is clamped to the nearest end knot's colour (no extrapolation
// at the core level -- that policy belongs to the AE integration layer in Phase 2).
// `g` must satisfy g.IsValid(). `working_space_gamma` is forwarded to
// WorkingSpaceTransform for the LinearLight/OKLCH modes (ignored by NaiveLerp, which
// never converts); see WorkingSpaceTransform's doc comment above.
RGBAf EvaluateGradient(const GradientData& g, float t, float working_space_gamma = 0.0f);

} // namespace GradientRemap
