#include "GradientData.h"

#include <algorithm>
#include <cstring>

namespace GradientRemap {

namespace {

void WriteU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void WriteF32LE(std::vector<uint8_t>& out, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    WriteU32LE(out, bits);
}

bool ReadU32LE(const uint8_t* data, size_t size, size_t offset, uint32_t& out) {
    if (offset + 4 > size) return false;
    out = static_cast<uint32_t>(data[offset]) |
          (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

bool ReadF32LE(const uint8_t* data, size_t size, size_t offset, float& out) {
    uint32_t bits;
    if (!ReadU32LE(data, size, offset, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

} // namespace

GradientData GradientData::Default() {
    GradientData g;
    g.interpolation_mode = InterpMode::NaiveLerp;
    g.knots = {
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    return g;
}

void GradientData::SortKnots() {
    std::stable_sort(knots.begin(), knots.end(),
                      [](const GradientKnot& a, const GradientKnot& b) { return a.position < b.position; });
}

bool GradientData::IsValid() const {
    if (knots.size() < kMinKnots || knots.size() > kMaxKnots) return false;
    for (size_t i = 1; i < knots.size(); ++i) {
        if (knots[i].position < knots[i - 1].position) return false;
    }
    return true;
}

std::vector<uint8_t> GradientData::Flatten() const {
    std::vector<uint8_t> out;
    out.reserve(12 + knots.size() * 20);

    WriteU32LE(out, kMagic);
    WriteU32LE(out, kVersion);
    out.push_back(static_cast<uint8_t>(interpolation_mode));
    out.push_back(static_cast<uint8_t>(path));
    out.push_back(static_cast<uint8_t>(knots.size()));
    out.push_back(0); // reserved

    for (const auto& k : knots) {
        WriteF32LE(out, k.position);
        WriteF32LE(out, k.r);
        WriteF32LE(out, k.g);
        WriteF32LE(out, k.b);
        WriteF32LE(out, k.a);
    }
    return out;
}

std::optional<GradientData> GradientData::Unflatten(const uint8_t* data, size_t size) {
    if (data == nullptr) return std::nullopt;

    size_t offset = 0;
    uint32_t magic = 0, version = 0;
    if (!ReadU32LE(data, size, offset, magic)) return std::nullopt;
    offset += 4;
    if (magic != kMagic) return std::nullopt;

    if (!ReadU32LE(data, size, offset, version)) return std::nullopt;
    offset += 4;
    if (version != kVersion) return std::nullopt; // no older versions to migrate from yet

    if (offset + 4 > size) return std::nullopt; // interp_mode(1) + path(1) + knot_count(1) + reserved(1)
    uint8_t interp_mode_raw = data[offset++];
    uint8_t path_raw = data[offset++];
    uint8_t knot_count = data[offset++];
    ++offset; // reserved

    if (interp_mode_raw > static_cast<uint8_t>(InterpMode::OKLCH)) return std::nullopt;
    if (path_raw > static_cast<uint8_t>(InterpPath::Cubic)) return std::nullopt;
    if (knot_count < kMinKnots) return std::nullopt;

    GradientData g;
    g.interpolation_mode = static_cast<InterpMode>(interp_mode_raw);
    g.path = static_cast<InterpPath>(path_raw);
    g.knots.reserve(knot_count);

    for (uint8_t i = 0; i < knot_count; ++i) {
        GradientKnot k{};
        if (!ReadF32LE(data, size, offset, k.position)) return std::nullopt;
        offset += 4;
        if (!ReadF32LE(data, size, offset, k.r)) return std::nullopt;
        offset += 4;
        if (!ReadF32LE(data, size, offset, k.g)) return std::nullopt;
        offset += 4;
        if (!ReadF32LE(data, size, offset, k.b)) return std::nullopt;
        offset += 4;
        if (!ReadF32LE(data, size, offset, k.a)) return std::nullopt;
        offset += 4;
        g.knots.push_back(k);
    }

    if (!g.IsValid()) {
        // Defensive: a hand-edited/corrupted blob might have unsorted positions;
        // repair by sorting rather than rejecting outright, then re-validate.
        g.SortKnots();
        if (!g.IsValid()) return std::nullopt;
    }
    return g;
}

} // namespace GradientRemap
