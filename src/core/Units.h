// SPDX-License-Identifier: GPL-3.0-or-later
// GNUSketch - length units: formatting and parsing of user-typed values.
#pragma once

#include <string>

namespace gs {

enum class LengthUnit { Millimeters, Centimeters, Meters, Inches, Feet };

struct Units {
    LengthUnit unit = LengthUnit::Millimeters;
    int precision = 0; // decimals shown (feet and inches: of the inches)

    // Length snapping while drawing: lengths become whole steps.
    enum class Snap { Off, Precision, Step };
    Snap snap = Snap::Off;
    double snapStep = 0.01; // meters (Snap::Step)
    // The step in meters (0 when snapping is off). Precision: one unit of the
    // last decimal shown (0.1mm at one decimal, 1/100" ...).
    double snapLength() const;
    // A length rounded to the nearest step (unchanged when off).
    double snapped(double meters) const;
    // Decimals that make sense for a unit when it is chosen.
    static int defaultPrecision(LengthUnit u);

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
