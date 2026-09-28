#pragma once

#include "GradientData.h"

#include <cstdint>
#include <vector>

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

// "Offset" dial + "Cycles" + "Loop" popup (Colorama-style phase shift, plus a
// TouchDesigner-Ramp-style repeat count). Scales the lookup position `t` by `cycles`
// (how many times the input range repeats across [0,1]: 1 = unscaled, 2 = the gradient
// plays twice), shifts it by `offset_revolutions` (dial angle / 360), then folds the
// result back into [0,1]:
//  - Cycle:  wraps (modulo 1). Exact positive whole numbers land on 1, not 0, so with
//            zero offset and whole-number cycles, t=0 -> 0 and t=1 -> 1 (black and white
//            still map to the first/last knot).
//  - Bounce: ping-pong/triangle wave, period 2 -- identity on [0,1] at offset 0, cycles 1.
//  - Sine:   smooth (raised-cosine) version of Bounce, same period; eases into each end.
// One full revolution always advances exactly one full period of the chosen mode, so
// animating the dial 0->360 degrees loops seamlessly in every mode.
enum class LoopMode { Cycle, Sine, Bounce };
float ApplyOffsetLoop(float t, float offset_revolutions, float cycles, LoopMode mode);

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
// knots.back().position] is clamped to the nearest end knot's colour (no extrapolation).
// `g` must satisfy g.IsValid(). `working_space_gamma` is forwarded to
// WorkingSpaceTransform for the LinearLight/OKLCH modes (ignored by NaiveLerp, which
// never converts); see WorkingSpaceTransform's doc comment above.
RGBAf EvaluateGradient(const GradientData& g, float t, float working_space_gamma = 0.0f);

// Per-frame lookup table over t in [0,1], built once from EvaluateGradient so the
// expensive colour-space maths (pow/cbrt/atan2/sin/cos, cubic tangents) runs `size`
// times per frame instead of once per pixel. Sample() linearly interpolates between
// entries, so t is never quantised and no banding is introduced.
//
// Exactness guarantees:
//  - t=0 and t=1 return exactly EvaluateGradient(0)/(1) (end entries are evaluated there).
//  - Any cell containing a knot falls back to exact EvaluateGradient. Knots are where
//    the curve can jump (Step, coincident knots) or kink (Linear), which linear sampling
//    between entries would smear or round off.
//  - Any other cell whose midpoint lerp misses by more than kMaxLerpError is also
//    evaluated exactly (see Build). See the LUT accuracy test in tests/main.cpp.
class GradientLUT {
public:
    static constexpr int kDefaultSize = 4096;
    // Cells whose midpoint lerp misses the exact value by more than this are evaluated
    // exactly instead (half a 16-bit code value).
    static constexpr float kMaxLerpError = 0.5f / 65535.0f;

    void Build(const GradientData& g, float working_space_gamma, int size = kDefaultSize);
    RGBAf Sample(float t) const;
    // Fraction of cells that fall back to exact evaluation (diagnostics/benchmarking).
    float ExactCellFraction() const;

private:
    GradientData gradient_;
    float working_space_gamma_ = 0.0f;
    std::vector<RGBAf> entries_;
    std::vector<uint8_t> exact_cell_; // 1 = cell [i, i+1] contains a knot
};

} // namespace GradientRemap
