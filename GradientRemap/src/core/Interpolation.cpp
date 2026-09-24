#include "Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GradientRemap {

namespace {

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

float ClampUnit(float v) { return std::min(1.0f, std::max(0.0f, v)); }

// Björn Ottosson's sRGB <-> OKLab matrices (D65-referenced), operating on linear-light
// Rec.709/sRGB primaries. See docs/DESIGN.md for the coefficient derivation reference.

float Cbrt(float x) {
    // std::cbrt handles negatives correctly (real cube root), unlike pow(x, 1/3).
    return std::cbrt(x);
}

} // namespace

float WorkingSpaceTransform::ToLinear(float c) {
    // Standard sRGB EOTF (piecewise). Negative/HDR values outside [0,1] are extended
    // via the linear-segment slope / power curve respectively, so 32bpc float values
    // that exceed [0,1] still round-trip sensibly rather than clamping silently.
    float sign = c < 0.0f ? -1.0f : 1.0f;
    float ac = std::fabs(c);
    float lin = (ac <= 0.04045f) ? (ac / 12.92f) : std::pow((ac + 0.055f) / 1.055f, 2.4f);
    return sign * lin;
}

float WorkingSpaceTransform::ToWorkingSpace(float c) {
    float sign = c < 0.0f ? -1.0f : 1.0f;
    float ac = std::fabs(c);
    float enc = (ac <= 0.0031308f) ? (ac * 12.92f) : (1.055f * std::pow(ac, 1.0f / 2.4f) - 0.055f);
    return sign * enc;
}

OKLab LinearSRGBToOKLab(float r, float g, float b) {
    float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    float l_ = Cbrt(l);
    float m_ = Cbrt(m);
    float s_ = Cbrt(s);

    OKLab out;
    out.L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    out.a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    out.b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
    return out;
}

void OKLabToLinearSRGB(const OKLab& lab, float& r, float& g, float& b) {
    float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    r = +4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

OKLCH OKLabToOKLCH(const OKLab& lab) {
    OKLCH out;
    out.L = lab.L;
    out.C = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    out.h = std::atan2(lab.b, lab.a);
    return out;
}

OKLab OKLCHToOKLab(const OKLCH& lch) {
    OKLab out;
    out.L = lch.L;
    out.a = lch.C * std::cos(lch.h);
    out.b = lch.C * std::sin(lch.h);
    return out;
}

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPi = 3.14159265358979323846f;

// Shortest-path hue lerp: wrap the delta into (-pi, pi] before interpolating so hue
// always takes the shorter way around the circle, then wrap the result back.
float LerpHue(float h0, float h1, float t) {
    float delta = h1 - h0;
    while (delta > kPi) delta -= kTwoPi;
    while (delta < -kPi) delta += kTwoPi;
    float h = h0 + delta * t;
    while (h > kPi) h -= kTwoPi;
    while (h < -kPi) h += kTwoPi;
    return h;
}

RGBAf BlendNaive(const GradientKnot& a, const GradientKnot& b, float t) {
    return RGBAf{Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

RGBAf BlendLinearLight(const GradientKnot& a, const GradientKnot& b, float t) {
    float ar = WorkingSpaceTransform::ToLinear(a.r);
    float ag = WorkingSpaceTransform::ToLinear(a.g);
    float ab = WorkingSpaceTransform::ToLinear(a.b);
    float br = WorkingSpaceTransform::ToLinear(b.r);
    float bg = WorkingSpaceTransform::ToLinear(b.g);
    float bb = WorkingSpaceTransform::ToLinear(b.b);

    float lr = Lerp(ar, br, t);
    float lg = Lerp(ag, bg, t);
    float lb = Lerp(ab, bb, t);

    return RGBAf{WorkingSpaceTransform::ToWorkingSpace(lr), WorkingSpaceTransform::ToWorkingSpace(lg),
                 WorkingSpaceTransform::ToWorkingSpace(lb), Lerp(a.a, b.a, t)};
}

RGBAf BlendOKLCH(const GradientKnot& a, const GradientKnot& b, float t) {
    float ar = WorkingSpaceTransform::ToLinear(a.r);
    float ag = WorkingSpaceTransform::ToLinear(a.g);
    float ab = WorkingSpaceTransform::ToLinear(a.b);
    float br = WorkingSpaceTransform::ToLinear(b.r);
    float bg = WorkingSpaceTransform::ToLinear(b.g);
    float bb = WorkingSpaceTransform::ToLinear(b.b);

    OKLCH lchA = OKLabToOKLCH(LinearSRGBToOKLab(ar, ag, ab));
    OKLCH lchB = OKLabToOKLCH(LinearSRGBToOKLab(br, bg, bb));

    OKLCH lchOut;
    lchOut.L = Lerp(lchA.L, lchB.L, t);
    lchOut.C = Lerp(lchA.C, lchB.C, t);
    // Degenerate hue (near-zero chroma, e.g. greys/black/white) makes atan2's angle
    // meaningless noise; if either side is essentially achromatic, just take the
    // other side's hue instead of lerping toward/away from an arbitrary angle.
    constexpr float kChromaEpsilon = 1e-4f;
    if (lchA.C < kChromaEpsilon && lchB.C < kChromaEpsilon) {
        lchOut.h = 0.0f;
    } else if (lchA.C < kChromaEpsilon) {
        lchOut.h = lchB.h;
    } else if (lchB.C < kChromaEpsilon) {
        lchOut.h = lchA.h;
    } else {
        lchOut.h = LerpHue(lchA.h, lchB.h, t);
    }

    float lr, lg, lb;
    OKLabToLinearSRGB(OKLCHToOKLab(lchOut), lr, lg, lb);

    return RGBAf{WorkingSpaceTransform::ToWorkingSpace(lr), WorkingSpaceTransform::ToWorkingSpace(lg),
                 WorkingSpaceTransform::ToWorkingSpace(lb), Lerp(a.a, b.a, t)};
}

} // namespace

RGBAf EvaluateGradient(const GradientData& g, float t) {
    const auto& knots = g.knots;
    // g.IsValid() guarantees knots.size() >= kMinKnots (2), sorted ascending.

    if (t <= knots.front().position) {
        const auto& k = knots.front();
        return RGBAf{k.r, k.g, k.b, k.a};
    }
    if (t >= knots.back().position) {
        const auto& k = knots.back();
        return RGBAf{k.r, k.g, k.b, k.a};
    }

    // Linear scan is fine up to kMaxKnots (256); replace with binary search only if
    // profiling shows this loop matters (it's precomputed once per render, not per pixel).
    size_t i = 0;
    while (i + 1 < knots.size() && knots[i + 1].position < t) {
        ++i;
    }
    const GradientKnot& a = knots[i];
    const GradientKnot& b = knots[i + 1];

    float span = b.position - a.position;
    float localT = (span > 0.0f) ? ClampUnit((t - a.position) / span) : 0.0f;

    switch (g.interpolation_mode) {
        case InterpMode::NaiveLerp:
            return BlendNaive(a, b, localT);
        case InterpMode::LinearLight:
            return BlendLinearLight(a, b, localT);
        case InterpMode::OKLCH:
            return BlendOKLCH(a, b, localT);
    }
    return BlendNaive(a, b, localT);
}

} // namespace GradientRemap
