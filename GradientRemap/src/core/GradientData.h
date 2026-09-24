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

// Serializable knot-list gradient. This is the AE-agnostic core representation that
// GradientRemap_Main.cpp's PF_ADD_ARBITRARY2 callbacks flatten/unflatten into/out of.
struct GradientData {
    static constexpr uint32_t kMagic = 0x4D525247u; // ASCII "GRRM" read as bytes G,R,R,M
    static constexpr uint32_t kVersion = 1;
    static constexpr size_t kMinKnots = 2;
    static constexpr size_t kMaxKnots = 256;

    InterpMode interpolation_mode = InterpMode::NaiveLerp;
    std::vector<GradientKnot> knots; // invariant: sorted ascending by position, size in [kMinKnots, kMaxKnots]

    // Default gradient: 2-knot black at 0 -> white at 1, matching the original 2-stop brief.
    static GradientData Default();

    void SortKnots();
    bool IsValid() const;

    // Explicit little-endian binary layout (independent of host endianness), no pointers:
    //   [u32 magic][u32 version][u8 interp_mode][u8 knot_count][u16 reserved]
    //   knot_count * [f32 position][f32 r][f32 g][f32 b][f32 a]
    std::vector<uint8_t> Flatten() const;
    static std::optional<GradientData> Unflatten(const uint8_t* data, size_t size);
};

} // namespace GradientRemap
