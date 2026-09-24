#pragma once

#include "GradientData.h"

namespace GradientRemap {

struct RGBAf {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

// Stand-in working-space <-> linear-light transfer function. Phase 1 hardcodes the
// sRGB EOTF/OETF; Phase 2 replaces the body of these two functions with a transform
// derived from AE's actual queried project working space, without touching any caller.
struct WorkingSpaceTransform {
    static float ToLinear(float c);
    static float ToWorkingSpace(float c);
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
// `g` must satisfy g.IsValid().
RGBAf EvaluateGradient(const GradientData& g, float t);

} // namespace GradientRemap
