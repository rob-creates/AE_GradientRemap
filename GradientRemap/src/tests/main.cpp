// Standalone, AE-independent test/inspection harness for GradientRemapCore.
//
// Usage:
//   GradientRemapCoreTests test              -- run assertions, exit 0 on pass
//   GradientRemapCoreTests dump <output_dir> -- write PPM strips + CSV per mode for visual inspection

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

void TestFlattenRoundTrip() {
    GradientData g;
    g.interpolation_mode = InterpMode::OKLCH;
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
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    for (InterpMode mode : {InterpMode::NaiveLerp, InterpMode::LinearLight, InterpMode::OKLCH}) {
        g.interpolation_mode = mode;
        RGBAf at0 = EvaluateGradient(g, 0.0f);
        RGBAf at1 = EvaluateGradient(g, 1.0f);
        std::string modeName = mode == InterpMode::NaiveLerp ? "Naive" : mode == InterpMode::LinearLight ? "LinearLight" : "OKLCH";
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

void TestNaiveReferenceValues() {
    GradientData g;
    g.interpolation_mode = InterpMode::NaiveLerp;
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

void RunTests() {
    TestFlattenRoundTrip();
    TestWysiwygEndpoints();
    TestNaiveReferenceValues();
    TestOklchAvoidsLuminanceDip();

    std::cout << "\n" << (g_failures == 0 ? "ALL TESTS PASSED" : std::to_string(g_failures) + " TEST(S) FAILED") << "\n";
}

void WritePPMStrip(const std::filesystem::path& path, const GradientData& g, int width, int height) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << width << " " << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float t = static_cast<float>(x) / static_cast<float>(width - 1);
            RGBAf c = EvaluateGradient(g, t);
            auto to8 = [](float v) { return static_cast<unsigned char>(std::round(std::min(1.0f, std::max(0.0f, v)) * 255.0f)); };
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
            std::string base = c.name + "_" + modeName;
            WritePPMStrip(std::filesystem::path(outDir) / (base + ".ppm"), g, 512, 48);
            WriteCSVStrip(std::filesystem::path(outDir) / (base + ".csv"), g, 64);
            std::cout << "wrote " << base << ".ppm / .csv\n";
        }
    }
    std::cout << "\nStrips written to " << std::filesystem::absolute(outDir).string()
               << " -- open the .ppm files (e.g. macOS Preview) to eyeball smoothness.\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "dump") == 0) {
        std::string outDir = argc >= 3 ? argv[2] : "test_output";
        return DumpStrips(outDir);
    }

    RunTests();
    return g_failures == 0 ? 0 : 1;
}
