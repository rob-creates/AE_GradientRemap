#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace GradientRemap {

// A single gradient stop. Colour is the literal working-space value as authored/
// displayed (WYSIWYG) -- never converted for storage. Alpha is straight (un-premultiplied).
struct GradientKnot {
    float position; // gradient-relative position; knots are kept sorted ascending by this
    float r, g, b, a;
};

enum class InterpMode : uint8_t {
    NaiveLerp = 0,   // direct working-space lerp, no conversion (matches the original 2-stop brief)
    LinearLight = 1, // linearize -> lerp -> re-encode
    OKLCH = 2,       // linearize -> OKLab/OKLCH -> lerp -> re-encode
};

// The parametric PATH taken between knots, orthogonal to InterpMode (which only governs
// what colour SPACE the blend happens in). Named after Cinema 4D's gradient path options
// (a subset -- Blend and Cubic Bias are not implemented):
enum class InterpPath : uint8_t {
    Linear = 0, // straight lerp within each segment (the only behaviour before this existed)
    Step = 1,   // no blending: hold the segment's starting knot colour across the whole segment
    Ease = 2,   // ease in/out within each segment (smoothstep on the local t), matching the same
                // idea as After Effects' own "Easy Ease" keyframe assistant -- still has a
                // zero-derivative "plateau" at every knot, since each segment is eased
                // independently of its neighbours. Renamed from "Smooth" (2026-09-25): it isn't
                // smoother than Cubic overall and shouldn't read as if it were.
    Cubic = 3,  // Catmull-Rom spline through all knots -- continuous (non-zero) derivative
                // across knot boundaries, avoiding the "ridge"/Mach-banding artifact that
                // piecewise-linear (or independently-eased) segments show at each knot
};

// Serializable knot-list gradient. This is the AE-agnostic core representation that
// GradientRemap_Main.cpp's PF_ADD_ARBITRARY2 callbacks flatten/unflatten into/out of.
struct GradientData {
    static constexpr uint32_t kMagic = 0x4D525247u; // ASCII "GRRM" read as bytes G,R,R,M
    static constexpr uint32_t kVersion = 2; // v2 adds the `path` byte (see Flatten)
    static constexpr size_t kMinKnots = 2;
    static constexpr size_t kMaxKnots = 256;

    InterpMode interpolation_mode = InterpMode::NaiveLerp;
    // Cubic is the default per user preference (2026-09-25): "gives the smoothest path
    // through multiple knots, less plateau at each knot" -- matches the Cinema 4D
    // convention this option set is modelled on.
    InterpPath path = InterpPath::Cubic;
    std::vector<GradientKnot> knots; // invariant: sorted ascending by position, size in [kMinKnots, kMaxKnots]

    // Default gradient: 2-knot black at 0 -> white at 1, matching the original 2-stop brief.
    static GradientData Default();

    void SortKnots();
    bool IsValid() const;

    // Explicit little-endian binary layout (independent of host endianness), no pointers:
    //   [u32 magic][u32 version][u8 interp_mode][u8 path][u8 knot_count][u8 reserved]
    //   knot_count * [f32 position][f32 r][f32 g][f32 b][f32 a]
    std::vector<uint8_t> Flatten() const;
    static std::optional<GradientData> Unflatten(const uint8_t* data, size_t size);
};

} // namespace GradientRemap
