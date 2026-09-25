#pragma once

#include <optional>
#include <string>
#include <vector>

#include "GradientData.h"

namespace GradientRemap {

// Plain CSV knot-list format, TouchDesigner Table-DAT compatible: a header row
// ("position,r,g,b,a") followed by one row per knot, values as decimal floats. Colours
// are the literal working-space values as authored/displayed (WYSIWYG) -- same
// convention as everywhere else in this plugin, no gamma conversion on the way in/out.
// This intentionally does NOT round-trip interpolation_mode/path -- those live in their
// own stock AE popups, orthogonal to the knot list itself (see GradientRemap.h).
std::string GradientKnotsToCSV(const GradientData& g);

// Parses the same format back into a knot list. Tolerates a missing header row (if the
// first line's first field parses as a number, it's treated as data instead of being
// skipped) so hand-edited or non-TouchDesigner-authored files still work. Returns
// std::nullopt on any structural problem: a row without exactly 5 fields, a
// non-numeric field, or a total knot count outside [GradientData::kMinKnots,
// GradientData::kMaxKnots] -- callers decide how to surface that (e.g. a native alert);
// this stays host-agnostic like the rest of GradientRemapCore.
std::optional<std::vector<GradientKnot>> ParseGradientKnotsCSV(const std::string& csv_text);

} // namespace GradientRemap
