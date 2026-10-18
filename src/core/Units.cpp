// SPDX-License-Identifier: GPL-3.0-or-later
#include "Units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gs {

const char* Units::suffix(LengthUnit u) {
    switch (u) {
    case LengthUnit::Millimeters: return "mm";
    case LengthUnit::Centimeters: return "cm";
    case LengthUnit::Meters: return "m";
    case LengthUnit::Inches: return "\"";
    case LengthUnit::Feet: return "'";
    }
    return "";
}

double Units::toMeters(LengthUnit u) {
    switch (u) {
    case LengthUnit::Millimeters: return 0.001;
    case LengthUnit::Centimeters: return 0.01;
    case LengthUnit::Meters: return 1.0;
    case LengthUnit::Inches: return 0.0254;
    case LengthUnit::Feet: return 0.3048;
    }
    return 1.0;
}

static std::string fmtNum(double v, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    std::string s = buf;
    if (s == "-0" || s.rfind("-0.", 0) == 0) {
        bool allZero = true;
        for (char c : s)
            if (c >= '1' && c <= '9') allZero = false;
        if (allZero) s = s.substr(1);
    }
    return s;
}

std::string Units::format(double meters) const {
    if (unit == LengthUnit::Feet) {
        // Architectural style: 5' 6"
        double inches = meters / 0.0254;
        bool neg = inches < 0;
        inches = std::fabs(inches);
        int ft = (int)std::floor(inches / 12.0 + 1e-9);
        double in = inches - ft * 12.0;
        if (in > 11.995) { ft += 1; in = 0; }
        std::string s = (neg ? "-" : "") + std::to_string(ft) + "' " + fmtNum(in, std::max(precision, 0)) + "\"";
        return s;
    }
    int prec = precision;
    if (unit == LengthUnit::Meters && prec < 2) prec = 2;
    if (unit == LengthUnit::Centimeters && prec < 1) prec = 1;
    return fmtNum(meters / toMeters(unit), prec) + suffix(unit);
}

std::string Units::formatArea(double sq) const {
    double f = toMeters(unit == LengthUnit::Feet ? LengthUnit::Feet : unit);
    const char* sfx = unit == LengthUnit::Millimeters ? "mm²"
                    : unit == LengthUnit::Centimeters ? "cm²"
                    : unit == LengthUnit::Meters ? "m²"
                    : unit == LengthUnit::Inches ? "in²" : "ft²";
    int prec = unit == LengthUnit::Meters ? 2 : (unit == LengthUnit::Millimeters ? 0 : 2);
    return fmtNum(sq / (f * f), prec) + " " + sfx;
}

bool parseNumber(const std::string& textIn, double& out) {
    std::string t;
    for (char c : textIn) {
        if (c == ',') t += '.';
        else if (!std::isspace((unsigned char)c)) t += c;
    }
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == 0;
}

bool Units::parseLength(const std::string& textIn, double& meters) const {
    std::string t;
    for (char c : textIn)
        if (!std::isspace((unsigned char)c)) t += (char)std::tolower((unsigned char)c);
    if (t.empty()) return false;

    // Feet and inches: 5'6" / 5' / 6"
    size_t ap = t.find('\'');
    if (ap != std::string::npos) {
        double ft = 0, in = 0;
        if (!parseNumber(t.substr(0, ap), ft)) return false;
        std::string rest = t.substr(ap + 1);
        if (!rest.empty()) {
            if (rest.back() == '"') rest.pop_back();
            if (!rest.empty() && !parseNumber(rest, in)) return false;
        }
        double sign = ft < 0 || t[0] == '-' ? -1 : 1;
        meters = (std::fabs(ft) * 12 + in) * 0.0254 * sign;
        return true;
    }
    struct Sfx { const char* s; double f; };
    static const Sfx sfx[] = {{"mm", 0.001}, {"cm", 0.01}, {"km", 1000.0}, {"m", 1.0},
                              {"\"", 0.0254}, {"in", 0.0254}, {"ft", 0.3048}, {"yd", 0.9144}};
    for (const Sfx& s : sfx) {
        size_t n = std::char_traits<char>::length(s.s);
        if (t.size() > n && t.compare(t.size() - n, n, s.s) == 0) {
            double v;
            if (!parseNumber(t.substr(0, t.size() - n), v)) return false;
            meters = v * s.f;
            return true;
        }
    }
    double v;
    if (!parseNumber(t, v)) return false;
    meters = v * toMeters(unit);
    return true;
}

} // namespace gs
