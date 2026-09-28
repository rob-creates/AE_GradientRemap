#include "Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GradientRemap {

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

float ClampUnit(float v) { return std::min(1.0f, std::max(0.0f, v)); }

float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

float ApplyOffsetLoop(float t, float offset_revolutions, LoopMode mode) {
    constexpr float kPi = 3.14159265358979323846f;
    if (mode == LoopMode::Cycle) {
        // Reduce to [0,1) first so every whole revolution is an exact identity -- otherwise
        // offset 1.0 would push black (t=0) to exactly 1.0 and render it as white.
        float u = t + (offset_revolutions - std::floor(offset_revolutions));
        return (u >= 0.0f && u <= 1.0f) ? u : u - std::floor(u);
    }
    // Bounce/Sine have period 2 in t, so one revolution = 2 units of t.
    float u = t + 2.0f * offset_revolutions;
    if (mode == LoopMode::Sine) return 0.5f - 0.5f * std::cos(kPi * u);
    float m = u - 2.0f * std::floor(u * 0.5f); // [0,2)
    return m <= 1.0f ? m : 2.0f - m;
}

namespace {

// Point-slope reflection: the value at `far` reflected through `near` (2*near - far).
// Used for Catmull-Rom phantom boundary points (ReflectKnot) and the equivalent per-
// channel/per-quantity constructions in the LinearLight/OKLCH cubic blends below.
float Reflect(float near, float far) { return 2.0f * near - far; }

// Cubic Hermite basis: given values p1/p2 at t=0/t=1 and explicit tangents m1/m2 there.
// Plain Catmull-Rom is the special case m1=0.5*(p2-p0), m2=0.5*(p3-p1) -- passing those
// exact tangents here reproduces it precisely. MonotoneCubicScalar below instead clamps
// those tangents at local extrema, which is the fix for the overshoot bug described in
// docs/DESIGN.md ("cyan blip" on an OKLCH+Cubic hue path).
float CubicHermiteScalar(float p1, float m1, float p2, float m2, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;
    return h00 * p1 + h10 * m1 + h01 * p2 + h11 * m2;
}

// Fritsch-Carlson-style monotone tangent: given the two secants adjacent to a knot, if
// they disagree in sign (or either is zero), this knot is a local extremum in the
// sequence of values -- the plain Catmull-Rom tangent (their average) would point the
// curve past the extremum before curving back, an overshoot. Flattening the tangent to
// zero there removes that overshoot; well-behaved (monotonic) runs of knots are
// completely unaffected, since same-signed secants pass straight through to the
// ordinary Catmull-Rom average.
float MonotoneTangent(float secantPrev, float secantNext) {
    if (secantPrev * secantNext <= 0.0f) return 0.0f;
    return 0.5f * (secantPrev + secantNext);
}

// Drop-in replacement for a plain Catmull-Rom evaluation (same p0..p3, t signature) that
// clamps tangents at local extrema instead of letting them overshoot. Identical to plain
// Catmull-Rom wherever p0,p1,p2,p3 is monotonic; only changes behaviour exactly at a
// local min/max, which is precisely where plain Catmull-Rom is known to misbehave.
float MonotoneCubicScalar(float p0, float p1, float p2, float p3, float t) {
    float m1 = MonotoneTangent(p1 - p0, p2 - p1);
    float m2 = MonotoneTangent(p2 - p1, p3 - p2);
    return CubicHermiteScalar(p1, m1, p2, m2, t);
}

// Björn Ottosson's sRGB <-> OKLab matrices (D65-referenced), operating on linear-light
// Rec.709/sRGB primaries. See docs/DESIGN.md for the coefficient derivation reference.

float Cbrt(float x) {
    // std::cbrt handles negatives correctly (real cube root), unlike pow(x, 1/3).
    return std::cbrt(x);
}

} // namespace

float WorkingSpaceTransform::ToLinear(float c, float gamma) {
    float sign = c < 0.0f ? -1.0f : 1.0f;
    float ac = std::fabs(c);

    if (gamma > 0.0f) {
        // Phase 2: a plain power-law curve driven by AE's queried working-space
        // approximate gamma (AEGP_GetColorProfileApproximateGamma) -- an approximation
        // of the real ICC profile, not a full LUT/matrix transform (see docs/DESIGN.md).
        return sign * std::pow(ac, gamma);
    }

    // Default (gamma <= 0): precise sRGB EOTF (piecewise). Negative/HDR values outside
    // [0,1] are extended via the linear-segment slope / power curve respectively, so
    // 32bpc float values that exceed [0,1] still round-trip sensibly rather than
    // clamping silently.
    float lin = (ac <= 0.04045f) ? (ac / 12.92f) : std::pow((ac + 0.055f) / 1.055f, 2.4f);
    return sign * lin;
}

float WorkingSpaceTransform::ToWorkingSpace(float c, float gamma) {
    float sign = c < 0.0f ? -1.0f : 1.0f;
    float ac = std::fabs(c);

    if (gamma > 0.0f) {
        return sign * std::pow(ac, 1.0f / gamma);
    }

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

// Below this chroma, atan2's hue angle is meaningless noise (e.g. greys/black/white) --
// shared by both the pairwise (BlendOKLCH) and cubic (BlendOKLCHCubic) OKLCH blends so
// the "what counts as achromatic" threshold can't drift between them.
constexpr float kChromaEpsilon = 1e-4f;

bool IsAchromatic(float chroma) { return chroma < kChromaEpsilon; }

// Wrap `h` to lie within (ref-pi, ref+pi] -- i.e. the representative of h's angle class
// closest to `ref`. With ref=0 this is the usual "wrap to principal range" (LerpHue and
// BlendOKLCHCubic's final hue both use it this way); with ref set to a running reference
// hue, this instead *unwraps* h to stay continuous with that reference (BlendOKLCHCubic's
// hueOrRef, so the spline's tangent maths never sees a fake 2*pi jump).
float WrapRelativeTo(float h, float ref) {
    while (h - ref > kPi) h -= kTwoPi;
    while (h - ref < -kPi) h += kTwoPi;
    return h;
}

// Shortest-path hue lerp: wrap the delta into (-pi, pi] before interpolating so hue
// always takes the shorter way around the circle, then wrap the result back.
float LerpHue(float h0, float h1, float t) {
    float delta = WrapRelativeTo(h1 - h0, 0.0f);
    return WrapRelativeTo(h0 + delta * t, 0.0f);
}

// Phantom control point for Catmull-Rom at a gradient boundary: reflect the far knot
// through the near one (2*near - far). Only the colour channels matter -- phantom
// points never get looked up by position, only fed into the cubic's channel maths --
// and this reflection is what makes a 2-knot gradient's Cubic path degenerate exactly
// to Linear (matching intuition: nothing to smooth with only one segment), unlike the
// more common "duplicate the endpoint" boundary rule, which would introduce a
// half-magnitude tangent and visibly differ from Linear even for a plain 2-knot case.
GradientKnot ReflectKnot(const GradientKnot& near, const GradientKnot& far) {
    GradientKnot k{};
    k.position = near.position;
    k.r = Reflect(near.r, far.r);
    k.g = Reflect(near.g, far.g);
    k.b = Reflect(near.b, far.b);
    k.a = Reflect(near.a, far.a);
    return k;
}

RGBAf BlendNaive(const GradientKnot& a, const GradientKnot& b, float t) {
    return RGBAf{Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

RGBAf BlendLinearLight(const GradientKnot& a, const GradientKnot& b, float t, float gamma) {
    float ar = WorkingSpaceTransform::ToLinear(a.r, gamma);
    float ag = WorkingSpaceTransform::ToLinear(a.g, gamma);
    float ab = WorkingSpaceTransform::ToLinear(a.b, gamma);
    float br = WorkingSpaceTransform::ToLinear(b.r, gamma);
    float bg = WorkingSpaceTransform::ToLinear(b.g, gamma);
    float bb = WorkingSpaceTransform::ToLinear(b.b, gamma);

    float lr = Lerp(ar, br, t);
    float lg = Lerp(ag, bg, t);
    float lb = Lerp(ab, bb, t);

    return RGBAf{WorkingSpaceTransform::ToWorkingSpace(lr, gamma), WorkingSpaceTransform::ToWorkingSpace(lg, gamma),
                 WorkingSpaceTransform::ToWorkingSpace(lb, gamma), Lerp(a.a, b.a, t)};
}

RGBAf BlendOKLCH(const GradientKnot& a, const GradientKnot& b, float t, float gamma) {
    float ar = WorkingSpaceTransform::ToLinear(a.r, gamma);
    float ag = WorkingSpaceTransform::ToLinear(a.g, gamma);
    float ab = WorkingSpaceTransform::ToLinear(a.b, gamma);
    float br = WorkingSpaceTransform::ToLinear(b.r, gamma);
    float bg = WorkingSpaceTransform::ToLinear(b.g, gamma);
    float bb = WorkingSpaceTransform::ToLinear(b.b, gamma);

    OKLCH lchA = OKLabToOKLCH(LinearSRGBToOKLab(ar, ag, ab));
    OKLCH lchB = OKLabToOKLCH(LinearSRGBToOKLab(br, bg, bb));

    OKLCH lchOut;
    lchOut.L = Lerp(lchA.L, lchB.L, t);
    lchOut.C = Lerp(lchA.C, lchB.C, t);
    // Degenerate hue (near-zero chroma, e.g. greys/black/white) makes atan2's angle
    // meaningless noise; if either side is essentially achromatic, just take the
    // other side's hue instead of lerping toward/away from an arbitrary angle.
    if (IsAchromatic(lchA.C) && IsAchromatic(lchB.C)) {
        lchOut.h = 0.0f;
    } else if (IsAchromatic(lchA.C)) {
        lchOut.h = lchB.h;
    } else if (IsAchromatic(lchB.C)) {
        lchOut.h = lchA.h;
    } else {
        lchOut.h = LerpHue(lchA.h, lchB.h, t);
    }

    float lr, lg, lb;
    OKLabToLinearSRGB(OKLCHToOKLab(lchOut), lr, lg, lb);

    return RGBAf{WorkingSpaceTransform::ToWorkingSpace(lr, gamma), WorkingSpaceTransform::ToWorkingSpace(lg, gamma),
                 WorkingSpaceTransform::ToWorkingSpace(lb, gamma), Lerp(a.a, b.a, t)};
}

// InterpPath::Cubic variants: a Catmull-Rom spline through p1(t=0)..p2(t=1), using p0/p3
// as tangent context (see ReflectKnot for boundary handling). Unlike Linear/Smooth,
// these need all 4 knots, not just the bracketing pair -- Smooth reshapes t within one
// segment (still zero-derivative at each knot, still shows the "ridge" the ordinary
// piecewise-linear/eased segments do); Cubic's continuous derivative across knots is
// what actually removes it.

// p0/p3 are the real neighbouring knots when they exist, or nullptr at a gradient
// boundary. Reflection for a missing boundary point MUST happen in the same space the
// blend interpolates in (raw RGB / linear RGB / OKLCH), not in raw RGB and then
// converted -- reflecting in the wrong space breaks the degenerate case (a plain 2-knot
// gradient must produce bit-identical results to the non-cubic path; see
// GradientRemapCoreTests' path-cubic tests) and generally distorts the curve near
// gradient endpoints. Each function below converts first, then reflects.

RGBAf BlendNaiveCubic(const GradientKnot* p0, const GradientKnot& a, const GradientKnot& b, const GradientKnot* p3,
                       float t) {
    GradientKnot rp0 = p0 ? *p0 : ReflectKnot(a, b);
    GradientKnot rp3 = p3 ? *p3 : ReflectKnot(b, a);
    return RGBAf{MonotoneCubicScalar(rp0.r, a.r, b.r, rp3.r, t), MonotoneCubicScalar(rp0.g, a.g, b.g, rp3.g, t),
                 MonotoneCubicScalar(rp0.b, a.b, b.b, rp3.b, t), MonotoneCubicScalar(rp0.a, a.a, b.a, rp3.a, t)};
}

RGBAf BlendLinearLightCubic(const GradientKnot* p0, const GradientKnot& a, const GradientKnot& b,
                             const GradientKnot* p3, float t, float gamma) {
    auto toLin = [gamma](const GradientKnot& k) {
        return RGBAf{WorkingSpaceTransform::ToLinear(k.r, gamma), WorkingSpaceTransform::ToLinear(k.g, gamma),
                     WorkingSpaceTransform::ToLinear(k.b, gamma), k.a};
    };
    RGBAf linA = toLin(a), linB = toLin(b);
    RGBAf linP0 = p0 ? toLin(*p0)
                     : RGBAf{Reflect(linA.r, linB.r), Reflect(linA.g, linB.g), Reflect(linA.b, linB.b),
                             Reflect(a.a, b.a)};
    RGBAf linP3 = p3 ? toLin(*p3)
                     : RGBAf{Reflect(linB.r, linA.r), Reflect(linB.g, linA.g), Reflect(linB.b, linA.b),
                             Reflect(b.a, a.a)};

    float lr = MonotoneCubicScalar(linP0.r, linA.r, linB.r, linP3.r, t);
    float lg = MonotoneCubicScalar(linP0.g, linA.g, linB.g, linP3.g, t);
    float lb = MonotoneCubicScalar(linP0.b, linA.b, linB.b, linP3.b, t);
    float alpha = MonotoneCubicScalar(linP0.a, a.a, b.a, linP3.a, t); // alpha never gamma-encoded
    return RGBAf{WorkingSpaceTransform::ToWorkingSpace(lr, gamma), WorkingSpaceTransform::ToWorkingSpace(lg, gamma),
                 WorkingSpaceTransform::ToWorkingSpace(lb, gamma), alpha};
}

RGBAf BlendOKLCHCubic(const GradientKnot* p0, const GradientKnot& a, const GradientKnot& b, const GradientKnot* p3,
                       float t, float gamma) {
    auto toLCH = [gamma](const GradientKnot& k) {
        float lr = WorkingSpaceTransform::ToLinear(k.r, gamma);
        float lg = WorkingSpaceTransform::ToLinear(k.g, gamma);
        float lb = WorkingSpaceTransform::ToLinear(k.b, gamma);
        return OKLabToOKLCH(LinearSRGBToOKLab(lr, lg, lb));
    };
    OKLCH lchA = toLCH(a), lchB = toLCH(b);

    // Reference hue for unwrapping/achromatic substitution: prefer the real segment
    // endpoints, same spirit as BlendOKLCH's pairwise handling.
    float refH = IsAchromatic(lchA.C) ? (IsAchromatic(lchB.C) ? 0.0f : lchB.h) : lchA.h;
    auto hueOrRef = [&](const OKLCH& c) { return IsAchromatic(c.C) ? refH : WrapRelativeTo(c.h, refH); };
    float hA = hueOrRef(lchA), hB = hueOrRef(lchB);

    OKLCH lchP0, lchP3;
    float hP0, hP3;
    if (p0) {
        lchP0 = toLCH(*p0);
        hP0 = hueOrRef(lchP0);
    } else {
        // Reflect in OKLCH space (L, C, and the already-unwrapped hue angle).
        lchP0 = OKLCH{Reflect(lchA.L, lchB.L), Reflect(lchA.C, lchB.C), 0.0f};
        hP0 = Reflect(hA, hB);
    }
    if (p3) {
        lchP3 = toLCH(*p3);
        hP3 = hueOrRef(lchP3);
    } else {
        lchP3 = OKLCH{Reflect(lchB.L, lchA.L), Reflect(lchB.C, lchA.C), 0.0f};
        hP3 = Reflect(hB, hA);
    }

    OKLCH out;
    out.L = MonotoneCubicScalar(lchP0.L, lchA.L, lchB.L, lchP3.L, t);
    out.C = std::max(0.0f, MonotoneCubicScalar(lchP0.C, lchA.C, lchB.C, lchP3.C, t)); // cubic can overshoot below 0
    out.h = WrapRelativeTo(MonotoneCubicScalar(hP0, hA, hB, hP3, t), 0.0f);

    float lr, lg, lb;
    OKLabToLinearSRGB(OKLCHToOKLab(out), lr, lg, lb);
    float alphaP0 = p0 ? p0->a : Reflect(a.a, b.a);
    float alphaP3 = p3 ? p3->a : Reflect(b.a, a.a);
    float alpha = MonotoneCubicScalar(alphaP0, a.a, b.a, alphaP3, t);
    return RGBAf{WorkingSpaceTransform::ToWorkingSpace(lr, gamma), WorkingSpaceTransform::ToWorkingSpace(lg, gamma),
                 WorkingSpaceTransform::ToWorkingSpace(lb, gamma), alpha};
}

} // namespace

RGBAf EvaluateGradient(const GradientData& g, float t, float working_space_gamma) {
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

    if (g.path == InterpPath::Step) {
        return RGBAf{a.r, a.g, a.b, a.a}; // hold the segment's starting knot, no blend at all
    }

    if (g.path == InterpPath::Cubic) {
        const GradientKnot* p0 = (i > 0) ? &knots[i - 1] : nullptr;
        const GradientKnot* p3 = (i + 2 < knots.size()) ? &knots[i + 2] : nullptr;
        switch (g.interpolation_mode) {
            case InterpMode::NaiveLerp:
                return BlendNaiveCubic(p0, a, b, p3, localT);
            case InterpMode::LinearLight:
                return BlendLinearLightCubic(p0, a, b, p3, localT, working_space_gamma);
            case InterpMode::OKLCH:
                return BlendOKLCHCubic(p0, a, b, p3, localT, working_space_gamma);
        }
        return BlendNaiveCubic(p0, a, b, p3, localT);
    }

    // Linear: localT unchanged. Smooth: ease within this segment only -- still has a
    // zero-derivative plateau at every knot, since neighbouring segments aren't
    // consulted (that's what distinguishes it from Cubic; see docs/DESIGN.md).
    float shapedT = (g.path == InterpPath::Ease) ? SmoothStep(localT) : localT;

    switch (g.interpolation_mode) {
        case InterpMode::NaiveLerp:
            return BlendNaive(a, b, shapedT);
        case InterpMode::LinearLight:
            return BlendLinearLight(a, b, shapedT, working_space_gamma);
        case InterpMode::OKLCH:
            return BlendOKLCH(a, b, shapedT, working_space_gamma);
    }
    return BlendNaive(a, b, shapedT);
}

} // namespace GradientRemap
