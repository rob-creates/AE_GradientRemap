// Standalone, AE-independent test/inspection harness for GradientRemapCore.
//
// Usage:
//   GradientRemapCoreTests test              -- run assertions, exit 0 on pass
//   GradientRemapCoreTests dump <output_dir> -- write PPM strips + CSV per mode for visual inspection

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../core/GradientCSV.h"
#include "../core/GradientData.h"
#include "../core/Interpolation.h"

using namespace GradientRemap;

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& what) {
    if (!cond) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    } else {
        std::cout << "ok:   " << what << "\n";
    }
}

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

const char* ModeName(InterpMode mode) {
    switch (mode) {
        case InterpMode::NaiveLerp: return "Naive";
        case InterpMode::LinearLight: return "LinearLight";
        case InterpMode::OKLCH: return "OKLCH";
    }
    return "Unknown";
}

void TestFlattenRoundTrip() {
    GradientData g;
    g.interpolation_mode = InterpMode::OKLCH;
    g.path = InterpPath::Ease;
    g.knots = {
        {0.00f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.25f, 0.8f, 0.1f, 0.1f, 1.0f},
        {0.75f, 0.1f, 0.3f, 0.8f, 0.9f},
        {1.00f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    Check(g.IsValid(), "flatten round-trip: source gradient is valid");

    auto blob = g.Flatten();
    auto restored = GradientData::Unflatten(blob.data(), blob.size());
    Check(restored.has_value(), "flatten round-trip: unflatten succeeded");
    if (!restored) return;

    Check(restored->interpolation_mode == g.interpolation_mode, "flatten round-trip: interp mode preserved");
    Check(restored->path == g.path, "flatten round-trip: path preserved");
    Check(restored->knots.size() == g.knots.size(), "flatten round-trip: knot count preserved");
    for (size_t i = 0; i < g.knots.size() && i < restored->knots.size(); ++i) {
        const auto& a = g.knots[i];
        const auto& b = restored->knots[i];
        Check(Near(a.position, b.position) && Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a),
              "flatten round-trip: knot " + std::to_string(i) + " values preserved");
    }

    // Corrupt magic -> must fail gracefully, not crash.
    auto corrupt = blob;
    corrupt[0] ^= 0xFF;
    auto bad = GradientData::Unflatten(corrupt.data(), corrupt.size());
    Check(!bad.has_value(), "flatten round-trip: corrupted magic rejected");

    // Truncated buffer -> must fail gracefully, not read out of bounds / crash.
    auto truncated = GradientData::Unflatten(blob.data(), blob.size() - 4);
    Check(!truncated.has_value(), "flatten round-trip: truncated buffer rejected");
}

void TestWysiwygEndpoints() {
    GradientData g;
    g.path = InterpPath::Linear; // isolate interpolation_mode behaviour from path shaping
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    for (InterpMode mode : {InterpMode::NaiveLerp, InterpMode::LinearLight, InterpMode::OKLCH}) {
        g.interpolation_mode = mode;
        RGBAf at0 = EvaluateGradient(g, 0.0f);
        RGBAf at1 = EvaluateGradient(g, 1.0f);
        std::string modeName = ModeName(mode);
        Check(Near(at0.r, 0.0f) && Near(at0.g, 0.0f) && Near(at0.b, 0.0f),
              modeName + ": t=0 exactly matches authored knot colour (WYSIWYG)");
        Check(Near(at1.r, 1.0f) && Near(at1.g, 1.0f) && Near(at1.b, 1.0f),
              modeName + ": t=1 exactly matches authored knot colour (WYSIWYG)");

        // Out-of-range t clamps to end knots rather than extrapolating (core-level policy).
        RGBAf below = EvaluateGradient(g, -0.5f);
        RGBAf above = EvaluateGradient(g, 1.5f);
        Check(Near(below.r, 0.0f) && Near(below.g, 0.0f) && Near(below.b, 0.0f), modeName + ": t<0 clamps to first knot");
        Check(Near(above.r, 1.0f) && Near(above.g, 1.0f) && Near(above.b, 1.0f), modeName + ": t>1 clamps to last knot");
    }
}

void TestWorkingSpaceGammaParameter() {
    // Phase 2: a nonzero working_space_gamma must still preserve WYSIWYG endpoints
    // exactly (any power-law curve maps 0->0 and 1->1), but must change the *interior*
    // blend relative to the default precise-sRGB curve -- otherwise the parameter isn't
    // actually doing anything.
    GradientData g;
    g.interpolation_mode = InterpMode::LinearLight;
    g.path = InterpPath::Linear;
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    constexpr float kTestGamma = 2.2f;

    RGBAf at0 = EvaluateGradient(g, 0.0f, kTestGamma);
    RGBAf at1 = EvaluateGradient(g, 1.0f, kTestGamma);
    Check(Near(at0.r, 0.0f) && Near(at1.r, 1.0f), "LinearLight+gamma: WYSIWYG endpoints preserved under a nonzero gamma");

    RGBAf midDefault = EvaluateGradient(g, 0.5f); // gamma=0 -> precise sRGB curve
    RGBAf midGamma = EvaluateGradient(g, 0.5f, kTestGamma);
    Check(!Near(midDefault.r, midGamma.r, 1e-4f),
          "LinearLight+gamma: nonzero gamma actually changes the interior blend vs. the sRGB default");

    g.interpolation_mode = InterpMode::OKLCH;
    RGBAf oklchAt0 = EvaluateGradient(g, 0.0f, kTestGamma);
    RGBAf oklchAt1 = EvaluateGradient(g, 1.0f, kTestGamma);
    Check(Near(oklchAt0.r, 0.0f) && Near(oklchAt1.r, 1.0f), "OKLCH+gamma: WYSIWYG endpoints preserved under a nonzero gamma");
}

void TestNaiveReferenceValues() {
    GradientData g;
    g.interpolation_mode = InterpMode::NaiveLerp;
    g.path = InterpPath::Linear;
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    RGBAf mid = EvaluateGradient(g, 0.5f);
    Check(Near(mid.r, 0.5f) && Near(mid.g, 0.5f) && Near(mid.b, 0.5f) && Near(mid.a, 1.0f),
          "Naive: black->white midpoint is exactly mid-grey (0.5,0.5,0.5)");
}

void TestOklchAvoidsLuminanceDip() {
    // Classic RGB-lerp artifact: red->green naive lerp passes through a dark, muddy
    // olive at the midpoint because perceptual lightness isn't linear in raw RGB.
    // OKLCH lerps perceptual lightness (L) directly, so its midpoint L should sit
    // close to the arithmetic mean of the endpoints' L, while naive/linear-light's
    // midpoint L should measurably dip below that mean. This test demonstrates and
    // guards the exact property the user asked for ("not unnecessarily flattened").
    GradientData g;
    g.path = InterpPath::Linear;
    g.knots = {
        {0.0f, 1.0f, 0.0f, 0.0f, 1.0f}, // pure red
        {1.0f, 0.0f, 1.0f, 0.0f, 1.0f}, // pure green
    };

    auto perceptualL = [](const RGBAf& c) {
        float lr = WorkingSpaceTransform::ToLinear(c.r);
        float lg = WorkingSpaceTransform::ToLinear(c.g);
        float lb = WorkingSpaceTransform::ToLinear(c.b);
        return LinearSRGBToOKLab(lr, lg, lb).L;
    };

    float lRed = perceptualL(RGBAf{1, 0, 0, 1});
    float lGreen = perceptualL(RGBAf{0, 1, 0, 1});
    float expectedMeanL = (lRed + lGreen) * 0.5f;

    g.interpolation_mode = InterpMode::NaiveLerp;
    float lNaiveMid = perceptualL(EvaluateGradient(g, 0.5f));

    g.interpolation_mode = InterpMode::OKLCH;
    float lOklchMid = perceptualL(EvaluateGradient(g, 0.5f));

    std::cout << "info: red->green midpoint perceptual L -- naive=" << lNaiveMid << " oklch=" << lOklchMid
               << " endpoint-mean=" << expectedMeanL << "\n";

    Check(lOklchMid > lNaiveMid, "OKLCH: red->green midpoint is perceptually lighter than naive lerp's midpoint");
    Check(Near(lOklchMid, expectedMeanL, 0.02f), "OKLCH: red->green midpoint L closely tracks the endpoint mean (no dip)");
    Check(lNaiveMid < expectedMeanL - 0.02f, "Naive: red->green midpoint L measurably dips below the endpoint mean");
}

void TestPathCubicDegeneratesToLinearFor2Knots() {
    // A 2-knot gradient has only one segment -- there's nothing for Cubic to smooth
    // relative to Linear, and the reflected-phantom-point maths should make them
    // produce bit-identical (to float epsilon) results. This locks in that guarantee
    // across all 3 interpolation modes so a future change to the reflection logic
    // can't silently break it.
    GradientData g;
    g.knots = {
        {0.0f, 0.1f, 0.8f, 0.3f, 1.0f},
        {1.0f, 0.9f, 0.2f, 0.6f, 0.4f},
    };
    for (InterpMode mode : {InterpMode::NaiveLerp, InterpMode::LinearLight, InterpMode::OKLCH}) {
        g.interpolation_mode = mode;
        std::string modeName = ModeName(mode);
        for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            g.path = InterpPath::Linear;
            RGBAf viaLinear = EvaluateGradient(g, t);
            g.path = InterpPath::Cubic;
            RGBAf viaCubic = EvaluateGradient(g, t);
            Check(Near(viaLinear.r, viaCubic.r, 1e-4f) && Near(viaLinear.g, viaCubic.g, 1e-4f) &&
                      Near(viaLinear.b, viaCubic.b, 1e-4f) && Near(viaLinear.a, viaCubic.a, 1e-4f),
                  modeName + ": Cubic == Linear for a 2-knot gradient at t=" + std::to_string(t));
        }
    }
}

void TestPathStepHoldsSegmentStart() {
    GradientData g;
    g.interpolation_mode = InterpMode::NaiveLerp;
    g.path = InterpPath::Step;
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    };
    RGBAf midFirstSegment = EvaluateGradient(g, 0.25f);
    Check(Near(midFirstSegment.r, 0.0f) && Near(midFirstSegment.g, 0.0f),
          "Step: mid-first-segment holds knot 0's colour exactly, no blend");
    RGBAf midSecondSegment = EvaluateGradient(g, 0.75f);
    Check(Near(midSecondSegment.r, 1.0f) && Near(midSecondSegment.g, 0.0f),
          "Step: mid-second-segment holds knot 1's colour exactly, no blend");
}

void TestPathCubicReducesRidgeAtInteriorKnot() {
    // The user's reported symptom: piecewise-linear (or independently-eased) segments
    // meeting at a knot with a different slope on each side create a visible "ridge"
    // (Mach banding) at the knot, because the DERIVATIVE is discontinuous there even
    // though the VALUE is continuous. Build a 3-knot gradient with deliberately
    // different segment slopes (0->0.1 over the first half, 0.1->1.0 over the second)
    // and confirm Cubic's one-sided derivatives either side of the middle knot are much
    // closer to each other than Linear's are.
    GradientData g;
    g.interpolation_mode = InterpMode::NaiveLerp;
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.1f, 0.1f, 0.1f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    constexpr float kEps = 0.01f;

    g.path = InterpPath::Linear;
    float linearLeftSlope = (EvaluateGradient(g, 0.5f).r - EvaluateGradient(g, 0.5f - kEps).r) / kEps;
    float linearRightSlope = (EvaluateGradient(g, 0.5f + kEps).r - EvaluateGradient(g, 0.5f).r) / kEps;
    float linearKink = std::fabs(linearRightSlope - linearLeftSlope);

    g.path = InterpPath::Cubic;
    float cubicLeftSlope = (EvaluateGradient(g, 0.5f).r - EvaluateGradient(g, 0.5f - kEps).r) / kEps;
    float cubicRightSlope = (EvaluateGradient(g, 0.5f + kEps).r - EvaluateGradient(g, 0.5f).r) / kEps;
    float cubicKink = std::fabs(cubicRightSlope - cubicLeftSlope);

    std::cout << "info: derivative discontinuity at interior knot -- linear=" << linearKink << " cubic=" << cubicKink
               << "\n";
    Check(cubicKink < linearKink * 0.1f,
          "Cubic: derivative discontinuity at an interior knot is far smaller than Linear's (fixes the reported ridge)");
}

void TestPathCubicOKLCHNoHueOvershoot() {
    // Regression for a reported bug (2026-09-25): a magenta->green->yellow gradient
    // under OKLCH+Cubic showed a "cyan blip" on the green->yellow segment. Root cause:
    // green's hue sits at a local extremum in the 3-knot hue sequence (hue increases
    // sharply coming from magenta, then must decrease going to yellow), so the plain
    // Catmull-Rom tangent at green (the average of both neighbours' secants) pointed the
    // wrong way -- toward higher/cyan-ward hue -- causing the curve to overshoot past
    // green before curving back down to yellow. MonotoneTangent fixes this by flattening
    // the tangent at exactly this kind of local extremum.
    GradientData g;
    g.interpolation_mode = InterpMode::OKLCH;
    g.path = InterpPath::Cubic;
    g.knots = {
        {0.0f, 0.72f, 0.11f, 0.68f, 1.0f}, // magenta-ish
        {0.5f, 0.10f, 0.55f, 0.12f, 1.0f}, // green
        {1.0f, 0.85f, 0.80f, 0.10f, 1.0f}, // yellow
    };

    auto hueOf = [](const RGBAf& c) {
        float lr = WorkingSpaceTransform::ToLinear(c.r);
        float lg = WorkingSpaceTransform::ToLinear(c.g);
        float lb = WorkingSpaceTransform::ToLinear(c.b);
        return OKLabToOKLCH(LinearSRGBToOKLab(lr, lg, lb)).h;
    };

    float hGreen = hueOf(EvaluateGradient(g, 0.5f));
    float hYellow = hueOf(EvaluateGradient(g, 1.0f));
    // Neither reference hue is near the +/-pi wrap boundary for this colour choice, so a
    // plain (non-circular) min/max comparison is safe here.
    float lo = std::min(hGreen, hYellow) - 0.003f; // tight tolerance for float/atan2 noise only
    float hi = std::max(hGreen, hYellow) + 0.003f;

    bool overshootFound = false;
    float worst = 0.0f;
    for (int i = 1; i < 20; ++i) {
        float t = 0.5f + 0.5f * (static_cast<float>(i) / 20.0f);
        float h = hueOf(EvaluateGradient(g, t));
        if (h < lo || h > hi) {
            overshootFound = true;
            worst = (h < lo) ? (lo - h) : (h - hi);
        }
    }
    std::cout << "info: green->yellow hue range [" << lo << "," << hi << "], overshoot=" << (overshootFound ? worst : 0.0f)
               << "\n";
    Check(!overshootFound,
          "OKLCH+Cubic: hue does not overshoot past either endpoint on the green->yellow segment (no 'cyan blip')");
}

void TestCSVRoundTrip() {
    GradientData g;
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.3f, 0.8f, 0.1f, 0.2f, 0.5f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };

    std::string csv = GradientKnotsToCSV(g);
    Check(csv.rfind("position,r,g,b,a\n", 0) == 0, "CSV: starts with the expected header row");

    auto parsed = ParseGradientKnotsCSV(csv);
    Check(parsed.has_value(), "CSV: round-tripped output parses back successfully");
    if (parsed) {
        Check(parsed->size() == g.knots.size(), "CSV: round trip preserves knot count");
        for (size_t i = 0; i < g.knots.size() && i < parsed->size(); ++i) {
            Check(Near((*parsed)[i].position, g.knots[i].position) && Near((*parsed)[i].r, g.knots[i].r) &&
                      Near((*parsed)[i].g, g.knots[i].g) && Near((*parsed)[i].b, g.knots[i].b) &&
                      Near((*parsed)[i].a, g.knots[i].a),
                  "CSV: round trip preserves knot " + std::to_string(i) + "'s values");
        }
    }

    // TouchDesigner Table-DAT compatibility: a headerless numeric-first-row file should
    // still parse (the header-detection probe falls through to data parsing).
    auto headerless = ParseGradientKnotsCSV("0.0,0.0,0.0,0.0,1.0\n1.0,1.0,1.0,1.0,1.0\n");
    Check(headerless.has_value() && headerless->size() == 2, "CSV: tolerates a missing header row");

    Check(!ParseGradientKnotsCSV("position,r,g,b,a\n0.0,0.0,0.0\n").has_value(),
          "CSV: rejects a row with the wrong column count");
    Check(!ParseGradientKnotsCSV("position,r,g,b,a\nnot_a_number,0.0,0.0,0.0,1.0\n").has_value(),
          "CSV: rejects a non-numeric field");
    Check(!ParseGradientKnotsCSV("position,r,g,b,a\n0.0,0.0,0.0,0.0,1.0\n").has_value(),
          "CSV: rejects a single-knot file (below GradientData::kMinKnots)");
}

void TestOffsetLoop() {
    const LoopMode modes[] = {LoopMode::Cycle, LoopMode::Bounce};
    for (LoopMode m : modes) {
        bool identity = true;
        for (int i = 0; i <= 20; ++i) {
            float t = i / 20.0f;
            identity = identity && Near(ApplyOffsetLoop(t, 0.0f, m), t);
        }
        Check(identity, std::string("offset loop: zero offset is an exact identity on [0,1] (") +
                            (m == LoopMode::Cycle ? "Cycle" : "Bounce") + ")");
    }
    Check(Near(ApplyOffsetLoop(0.0f, 0.0f, LoopMode::Sine), 0.0f) && Near(ApplyOffsetLoop(1.0f, 0.0f, LoopMode::Sine), 1.0f),
          "offset loop: Sine maps 0->0 and 1->1 at zero offset");

    Check(Near(ApplyOffsetLoop(0.9f, 0.25f, LoopMode::Cycle), 0.15f), "offset loop: Cycle wraps past 1");
    Check(Near(ApplyOffsetLoop(0.1f, -0.25f, LoopMode::Cycle), 0.85f), "offset loop: Cycle wraps below 0");
    Check(Near(ApplyOffsetLoop(0.9f, 0.1f, LoopMode::Bounce), 0.9f), "offset loop: Bounce reflects off 1 (0.9 + 0.2 -> 0.9)");
    Check(Near(ApplyOffsetLoop(0.0f, 0.5f, LoopMode::Bounce), 1.0f), "offset loop: Bounce half revolution reverses the ramp");

    // One full revolution returns every mode to where it started (seamless animation loop).
    const LoopMode all[] = {LoopMode::Cycle, LoopMode::Sine, LoopMode::Bounce};
    bool periodic = true;
    for (LoopMode m : all) {
        for (int i = 0; i <= 20; ++i) {
            float t = i / 20.0f;
            periodic = periodic && Near(ApplyOffsetLoop(t, 1.0f, m), ApplyOffsetLoop(t, 0.0f, m), 1e-4f);
        }
    }
    Check(periodic, "offset loop: one full revolution is a full period in every mode");
}

void RunTests() {
    TestFlattenRoundTrip();
    TestWysiwygEndpoints();
    TestNaiveReferenceValues();
    TestOklchAvoidsLuminanceDip();
    TestWorkingSpaceGammaParameter();
    TestPathCubicDegeneratesToLinearFor2Knots();
    TestPathStepHoldsSegmentStart();
    TestPathCubicReducesRidgeAtInteriorKnot();
    TestPathCubicOKLCHNoHueOvershoot();
    TestCSVRoundTrip();
    TestOffsetLoop();

    std::cout << "\n" << (g_failures == 0 ? "ALL TESTS PASSED" : std::to_string(g_failures) + " TEST(S) FAILED") << "\n";
}

void WritePPMStrip(const std::filesystem::path& path, const GradientData& g, int width, int height) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << width << " " << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float t = static_cast<float>(x) / static_cast<float>(width - 1);
            RGBAf c = EvaluateGradient(g, t);
            auto to8 = [](float v) { return static_cast<unsigned char>(std::round(ClampUnit(v) * 255.0f)); };
            unsigned char rgb[3] = {to8(c.r), to8(c.g), to8(c.b)};
            f.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
}

void WriteCSVStrip(const std::filesystem::path& path, const GradientData& g, int samples) {
    std::ofstream f(path);
    f << "t,r,g,b,a\n";
    for (int i = 0; i < samples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(samples - 1);
        RGBAf c = EvaluateGradient(g, t);
        f << t << "," << c.r << "," << c.g << "," << c.b << "," << c.a << "\n";
    }
}

int DumpStrips(const std::string& outDir) {
    std::filesystem::create_directories(outDir);

    struct Case {
        std::string name;
        GradientData gradient;
    };

    GradientData blackWhite = GradientData::Default();

    GradientData redGreen;
    redGreen.knots = {{0.0f, 1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 0.0f, 1.0f}};

    GradientData multiKnot;
    multiKnot.knots = {
        {0.00f, 0.05f, 0.02f, 0.20f, 1.0f},
        {0.35f, 0.90f, 0.10f, 0.10f, 1.0f},
        {0.65f, 0.95f, 0.80f, 0.10f, 1.0f},
        {1.00f, 0.10f, 0.90f, 0.95f, 1.0f},
    };

    std::vector<Case> cases = {{"black_white", blackWhite}, {"red_green", redGreen}, {"multi_knot", multiKnot}};
    std::vector<std::pair<std::string, InterpMode>> modes = {
        {"naive", InterpMode::NaiveLerp}, {"linear_light", InterpMode::LinearLight}, {"oklch", InterpMode::OKLCH}};

    for (auto& c : cases) {
        for (auto& [modeName, mode] : modes) {
            GradientData g = c.gradient;
            g.interpolation_mode = mode;
            g.path = InterpPath::Linear; // keep pre-existing strips' meaning unchanged
            std::string base = c.name + "_" + modeName;
            WritePPMStrip(std::filesystem::path(outDir) / (base + ".ppm"), g, 512, 48);
            WriteCSVStrip(std::filesystem::path(outDir) / (base + ".csv"), g, 64);
            std::cout << "wrote " << base << ".ppm / .csv\n";
        }
    }
    // Path comparison: OKLCH+Linear vs OKLCH+Cubic on the multi-knot gradient, to
    // visually confirm the "ridge"/Mach-banding fix at each interior knot.
    for (auto& [pathName, path] : std::vector<std::pair<std::string, InterpPath>>{
             {"linear", InterpPath::Linear}, {"step", InterpPath::Step}, {"smooth", InterpPath::Ease},
             {"cubic", InterpPath::Cubic}}) {
        GradientData g = multiKnot;
        g.interpolation_mode = InterpMode::OKLCH;
        g.path = path;
        std::string base = "multi_knot_oklch_path_" + pathName;
        WritePPMStrip(std::filesystem::path(outDir) / (base + ".ppm"), g, 512, 48);
        std::cout << "wrote " << base << ".ppm\n";
    }

    // Regression visual: magenta->green->yellow under OKLCH+Cubic, the reported "cyan
    // blip" case (see TestPathCubicOKLCHNoHueOvershoot).
    GradientData magentaGreenYellow;
    magentaGreenYellow.interpolation_mode = InterpMode::OKLCH;
    magentaGreenYellow.path = InterpPath::Cubic;
    magentaGreenYellow.knots = {
        {0.0f, 0.72f, 0.11f, 0.68f, 1.0f},
        {0.5f, 0.10f, 0.55f, 0.12f, 1.0f},
        {1.0f, 0.85f, 0.80f, 0.10f, 1.0f},
    };
    WritePPMStrip(std::filesystem::path(outDir) / "magenta_green_yellow_oklch_cubic.ppm", magentaGreenYellow, 512, 48);
    std::cout << "wrote magenta_green_yellow_oklch_cubic.ppm\n";

    std::cout << "\nStrips written to " << std::filesystem::absolute(outDir).string()
               << " -- open the .ppm files (e.g. macOS Preview) to eyeball smoothness.\n";
    return 0;
}

} // namespace

// Throughput benchmark: `GradientRemapCoreTests bench`. Sweeps t across [0,1] (a
// deterministic stand-in for a frame's worth of luma values) and reports millions of
// gradient evaluations per second for every colour space x interpolation path.
constexpr int kBenchSamples = 4'000'000;

GradientData BenchGradient(InterpMode mode, InterpPath path) {
    GradientData g;
    g.interpolation_mode = mode;
    g.path = path;
    g.knots = {
        {0.00f, 0.02f, 0.01f, 0.10f, 1.0f}, {0.20f, 0.80f, 0.05f, 0.40f, 1.0f}, {0.45f, 0.10f, 0.70f, 0.20f, 1.0f},
        {0.70f, 0.95f, 0.85f, 0.10f, 1.0f}, {1.00f, 1.00f, 1.00f, 1.00f, 1.0f},
    };
    return g;
}

template <typename Fn>
double BenchMevalsPerSec(Fn&& eval) {
    volatile float sink = 0.0f;
    float acc = 0.0f;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kBenchSamples; ++i) {
        RGBAf c = eval(static_cast<float>(i) / static_cast<float>(kBenchSamples - 1));
        acc += c.r + c.g + c.b;
    }
    auto end = std::chrono::steady_clock::now();
    sink = acc;
    (void)sink;
    double seconds = std::chrono::duration<double>(end - start).count();
    return kBenchSamples / seconds / 1e6;
}

int RunBench() {
    const InterpMode modes[] = {InterpMode::NaiveLerp, InterpMode::LinearLight, InterpMode::OKLCH};
    const InterpPath paths[] = {InterpPath::Cubic, InterpPath::Ease, InterpPath::Linear, InterpPath::Step};
    const char* pathNames[] = {"Cubic", "Ease", "Linear", "Step"};
    std::printf("%-12s %-7s %12s\n", "mode", "path", "direct Mev/s");
    for (InterpMode m : modes) {
        for (int p = 0; p < 4; ++p) {
            GradientData g = BenchGradient(m, paths[p]);
            double direct = BenchMevalsPerSec([&](float t) { return EvaluateGradient(g, t); });
            std::printf("%-12s %-7s %12.1f\n", ModeName(m), pathNames[p], direct);
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "bench") == 0) {
        return RunBench();
    }
    if (argc >= 2 && std::strcmp(argv[1], "dump") == 0) {
        std::string outDir = argc >= 3 ? argv[2] : "test_output";
        return DumpStrips(outDir);
    }

    RunTests();
    return g_failures == 0 ? 0 : 1;
}
