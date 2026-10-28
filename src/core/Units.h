// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - length units: formatting and parsing of user-typed values.
#pragma once

#include <string>

namespace gs {

enum class LengthUnit { Millimeters, Centimeters, Meters, Inches, Feet };

struct Units {
    LengthUnit unit = LengthUnit::Millimeters;
    int precision = 0; // decimals shown

    std::string format(double meters) const;
    std::string formatArea(double sqMeters) const;
    std::string formatVolume(double cubicMeters) const;
    // Parses "1500", "1.5m", "150cm", "12'", "5'6\"", "3in" ... A bare number is
    // interpreted in the current unit. Returns false on syntax error.
    bool parseLength(const std::string& text, double& meters) const;
    static const char* suffix(LengthUnit u);
    static double toMeters(LengthUnit u);
};

// Parses a plain number (locale independent, accepts ',' as decimal comma).
bool parseNumber(const std::string& text, double& out);

} // namespace gs
