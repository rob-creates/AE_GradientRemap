#include "GradientCSV.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace GradientRemap {

std::string GradientKnotsToCSV(const GradientData& g) {
    std::ostringstream out;
    out << "position,r,g,b,a\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& k : g.knots) {
        out << k.position << ',' << k.r << ',' << k.g << ',' << k.b << ',' << k.a << '\n';
    }
    return out.str();
}

namespace {

std::vector<std::string> SplitCSVLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            fields.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    fields.push_back(cur);
    return fields;
}

bool ParseFloatStrict(const std::string& s, float* out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        float v = std::stof(s, &pos);
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) return false;
        *out = v;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::optional<std::vector<GradientKnot>> ParseGradientKnotsCSV(const std::string& csv_text) {
    std::istringstream stream(csv_text);
    std::string line;
    std::vector<GradientKnot> knots;
    bool checked_header = false;

    while (std::getline(stream, line)) {
        if (line.empty()) continue; // tolerate blank/trailing lines

        std::vector<std::string> fields = SplitCSVLine(line);

        if (!checked_header) {
            checked_header = true;
            float probe;
            if (!ParseFloatStrict(fields.empty() ? "" : fields[0], &probe)) {
                continue; // a genuine header row (e.g. "position,r,g,b,a") -- skip it
            }
            // First field parses as a number: this file has no header, fall through
            // and parse this line as data below.
        }

        if (fields.size() != 5) return std::nullopt;

        GradientKnot k{};
        if (!ParseFloatStrict(fields[0], &k.position)) return std::nullopt;
        if (!ParseFloatStrict(fields[1], &k.r)) return std::nullopt;
        if (!ParseFloatStrict(fields[2], &k.g)) return std::nullopt;
        if (!ParseFloatStrict(fields[3], &k.b)) return std::nullopt;
        if (!ParseFloatStrict(fields[4], &k.a)) return std::nullopt;
        knots.push_back(k);
    }

    if (knots.size() < GradientData::kMinKnots || knots.size() > GradientData::kMaxKnots) return std::nullopt;
    return knots;
}

} // namespace GradientRemap
