// SPDX-License-Identifier: GPL-3.0-or-later
// Forma - standard engineering parts: ISO metric fasteners (screws, bolts,
// threaded rods, nuts, washers) and involute gears (spur gears, racks).
//
// The sizes come from the standards' nominal dimensions (tables below), not
// from stored models: each part is generated from its size. Fasteners are
// built as a closed blank (a head and a round shank, or a nut with a round
// hole at the thread's minor diameter) and then threaded by the Screw Thread
// algorithm with the ISO 60° profile, so a screw and a nut of the same size
// and pitch mate (both at the basic profile, no clearance).
//
// Every part is a closed solid standing on the ground (lowest point z = 0),
// centred on the Z axis; screws stand on their tip with the head on top,
// gears and racks lie flat with their teeth in the XY plane.
#pragma once

#include "Mesh.h"

#include <string>
#include <vector>

namespace forma {

// One ISO metric thread size and the parts made for it. Millimetres.
struct MetricSize {
    const char* name;        // "M6"
    double d;                // nominal (major) diameter
    double pitch;            // coarse pitch (ISO 261/262)
    double finePitch;        // the usual fine pitch
    double nutS, nutM;       // hex nut ISO 4032 (M1, M1.2: DIN 934): across flats, height
    double headK;            // hex head screw ISO 4017: head height (across flats = nutS)
    double capDk, capKey;    // socket head cap screw ISO 4762: head diameter, hex key (head height = d)
    double cskDk, cskKey;    // countersunk socket screw ISO 10642: head diameter (90°), hex key
    double sqS, sqM;         // square nut DIN 557 (sizes outside M5–M16: approximate)
    double washD1, washD2, washH; // plain washer ISO 7089 (M1, M1.2: approximate)
};
const std::vector<MetricSize>& metricSizes();
int metricSizeIndex(const std::string& name); // -1 if unknown

// Lengths screws are sold in (ISO 888 preferred lengths), millimetres.
const std::vector<double>& standardScrewLengths();

// ISO 60° basic thread: depth of the thread from crest to root (5H/8).
double metricThreadDepth(double pitch);

enum class FastenerKind {
    HexBolt,      // ISO 4017 hex head screw (fully threaded)
    SocketCap,    // ISO 4762 socket head cap screw
    Countersunk,  // ISO 10642 countersunk socket screw
    ThreadedRod,  // DIN 976 stud
    HexNut,       // ISO 4032
    SquareNut,    // DIN 557
    Washer,       // ISO 7089
};
constexpr int kFastenerKindCount = 7;
const char* fastenerKindName(FastenerKind k);
const char* fastenerStandard(FastenerKind k);
bool fastenerHasLength(FastenerKind k);
bool fastenerHasThread(FastenerKind k);

struct FastenerParams {
    FastenerKind kind = FastenerKind::HexBolt;
    int size = 0;            // index into metricSizes()
    double length = 0.02;    // screws, rods: under the head (countersunk: overall), meters
    bool finePitch = false;
    bool thread = true;      // model the thread (else a plain shank or hole, much lighter)
    bool leftHand = false;
    int segments = 36;       // pieces around round surfaces
};

// Builds the part into `out` (which should be empty). `name` gets a
// descriptive name such as "Hex Bolt M6×30". Returns false (and explains in
// `err`) for bad parameters.
bool buildFastener(const FastenerParams& p, Mesh& out, std::string* name = nullptr, std::string* err = nullptr);

// ---- gears --------------------------------------------------------------------

// ISO 54 module series (millimetres), first choice and second choice mixed.
const std::vector<double>& standardModules();

enum class GearKind { Spur, Rack, Bevel };

struct GearParams {
    GearKind kind = GearKind::Spur;
    double module = 0.001;      // meters: pitch diameter = module × teeth
    int teeth = 20;
    double pressureAngle = 20;  // degrees (ISO 53: 20)
    double faceWidth = 0.008;   // thickness of the gear, meters
    double bore = 0.005;        // spur: diameter of the shaft hole (0 = none), meters
    double rackHeight = 0;      // rack: bottom to pitch line (0 = 3 × module), meters
    double backlash = 0;        // meters, taken off each tooth's thickness at the pitch line
    int flankSteps = 6;         // pieces of each involute flank
    int boreSegments = 32;
    // Bevel: the teeth of the gear it meshes with (shafts at 90°; the same
    // count makes a 45° miter pair). faceWidth is then measured along the
    // cone, 0 = a third of the cone distance.
    int mateTeeth = 0;
};

struct GearInfo {
    double pitchDiameter = 0, outsideDiameter = 0, rootDiameter = 0, baseDiameter = 0; // meters (spur)
    double pitch = 0;           // circular pitch π·m, meters
    double length = 0;          // rack: length, meters
    bool undercut = false;      // spur: too few teeth for the pressure angle (real hobbed gears get undercut)
    double coneAngle = 0;       // bevel: pitch cone half-angle, degrees
    double coneDistance = 0;    // bevel: apex to the large end along the pitch cone, meters
    double faceWidth = 0;       // bevel: tooth length along the cone, meters
};

// A bevel gear's cones (meters, radians), in its own coordinates: axis Z, the
// large end on the ground, the apex up at apexHeight.
struct BevelGeometry {
    double coneAngle = 0, pitchRadius = 0, coneDistance = 0, faceWidth = 0, apexHeight = 0;
};
BevelGeometry bevelGeometry(const GearParams& p);

// The outline of a spur gear (CCW, centred on the origin, in meters) with a
// flag per point for whether the outline turns smoothly there.
std::vector<Vec2> spurGearOutline(const GearParams& p, std::vector<char>* smooth = nullptr, GearInfo* info = nullptr,
                                  std::string* err = nullptr);

bool buildGear(const GearParams& p, Mesh& out, std::string* name = nullptr, GearInfo* info = nullptr,
               std::string* err = nullptr);

// Sizing by eye: gears of one module and pressure angle all mesh, so a gear
// set keeps those and only the size changes, the tooth count following it.
// Teeth of a spur gear whose pitch diameter comes closest to `diameter`
// (at least 6), and of a rack closest to `length` long (at least 1).
int gearTeethForDiameter(double module, double diameter);
int rackTeethForLength(double module, double length);
// Distance between the centres of two meshing spur gears (meters).
double gearCentreDistance(double module, int teethA, int teethB);

// Where `b` goes to mesh with `a`, in a's own coordinates (as buildGear
// makes them): a second spur gear to the right (+X) of a spur gear, turned so
// a tooth of one sits in a gap of the other; a rack under a spur gear; a spur
// gear on top of a rack, over the tooth nearest its middle; a bevel gear's mate
// on a shaft at right angles (along +X), the cones' apexes together. False
// when the two cannot mesh (different module or pressure angle, two racks, a
// bevel gear with anything but the mate it was made for).
bool gearMeshPlacement(const GearParams& a, const GearParams& b, Mat4& out, std::string* err = nullptr);

} // namespace forma
