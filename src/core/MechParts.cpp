// SPDX-License-Identifier: GPL-3.0-or-later
#include "MechParts.h"

#include "ScrewThread.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gs {

// clang-format off
const std::vector<MetricSize>& metricSizes() {
    //                 d     P     fine   nutS  nutM  headK  capDk capKey cskDk  cskKey sqS   sqM   wD1   wD2  wH
    static const std::vector<MetricSize> t = {
        {"M1",   1.0,  0.25, 0.2,  2.5,  0.8,  0.7,   2.0,  0.9,   2.24,  0.7,   2.5,  0.8,  1.1,  3.0, 0.3},
        {"M1.2", 1.2,  0.25, 0.2,  3.0,  1.0,  0.8,   2.3,  1.0,   2.69,  0.9,   3.0,  1.0,  1.3,  3.5, 0.3},
        {"M1.6", 1.6,  0.35, 0.2,  3.2,  1.3,  1.1,   3.0,  1.5,   3.58,  0.9,   3.2,  1.3,  1.7,  4.0, 0.3},
        {"M2",   2.0,  0.4,  0.25, 4.0,  1.6,  1.4,   3.8,  1.5,   4.48,  1.3,   4.0,  1.6,  2.2,  5.0, 0.3},
        {"M2.5", 2.5,  0.45, 0.35, 5.0,  2.0,  1.7,   4.5,  2.0,   5.6,   1.5,   5.0,  2.0,  2.7,  6.0, 0.5},
        {"M3",   3.0,  0.5,  0.35, 5.5,  2.4,  2.0,   5.5,  2.5,   6.72,  2.0,   5.5,  2.4,  3.2,  7.0, 0.5},
        {"M4",   4.0,  0.7,  0.5,  7.0,  3.2,  2.8,   7.0,  3.0,   8.96,  2.5,   7.0,  3.2,  4.3,  9.0, 0.8},
        {"M5",   5.0,  0.8,  0.5,  8.0,  4.7,  3.5,   8.5,  4.0,  11.2,   3.0,   8.0,  4.0,  5.3, 10.0, 1.0},
        {"M6",   6.0,  1.0,  0.75, 10.0, 5.2,  4.0,  10.0,  5.0,  13.44,  4.0,  10.0,  5.0,  6.4, 12.0, 1.6},
        {"M8",   8.0,  1.25, 1.0,  13.0, 6.8,  5.3,  13.0,  6.0,  17.92,  5.0,  13.0,  6.5,  8.4, 16.0, 1.6},
        {"M10", 10.0,  1.5,  1.25, 16.0, 8.4,  6.4,  16.0,  8.0,  22.4,   6.0,  17.0,  8.0, 10.5, 20.0, 2.0},
        {"M12", 12.0,  1.75, 1.25, 18.0, 10.8, 7.5,  18.0, 10.0,  26.88,  8.0,  19.0, 10.0, 13.0, 24.0, 2.5},
        {"M14", 14.0,  2.0,  1.5,  21.0, 12.8, 8.8,  21.0, 12.0,  30.8,  10.0,  22.0, 11.0, 15.0, 28.0, 2.5},
        {"M16", 16.0,  2.0,  1.5,  24.0, 14.8, 10.0, 24.0, 14.0,  33.6,  10.0,  24.0, 13.0, 17.0, 30.0, 3.0},
        {"M20", 20.0,  2.5,  1.5,  30.0, 18.0, 12.5, 30.0, 17.0,  40.32, 12.0,  30.0, 16.0, 21.0, 37.0, 3.0},
        {"M24", 24.0,  3.0,  2.0,  36.0, 21.5, 15.0, 36.0, 19.0,  48.0,  14.0,  36.0, 19.0, 25.0, 44.0, 4.0},
        {"M30", 30.0,  3.5,  2.0,  46.0, 25.6, 18.7, 45.0, 22.0,  60.0,  17.0,  46.0, 24.0, 31.0, 56.0, 4.0},
        {"M36", 36.0,  4.0,  3.0,  55.0, 31.0, 22.5, 54.0, 27.0,  72.0,  19.0,  55.0, 29.0, 37.0, 66.0, 5.0},
    };
    return t;
}
// clang-format on

int metricSizeIndex(const std::string& name) {
    const auto& t = metricSizes();
    for (size_t i = 0; i < t.size(); ++i)
        if (name == t[i].name) return int(i);
    return -1;
}

const std::vector<double>& standardScrewLengths() {
    static const std::vector<double> t = {2,  2.5, 3,  4,  5,  6,  8,  10, 12,  16,  20,  25,  30,  35,  40,  45,  50,
                                          55, 60,  65, 70, 75, 80, 90, 100, 110, 120, 130, 140, 150, 160, 180, 200};
    return t;
}

double metricThreadDepth(double pitch) { return 0.625 * std::sqrt(3.0) / 2 * pitch; } // 5H/8, H = √3/2 P

const char* fastenerKindName(FastenerKind k) {
    switch (k) {
    case FastenerKind::HexBolt: return "Hex Bolt";
    case FastenerKind::SocketCap: return "Socket Cap Screw";
    case FastenerKind::Countersunk: return "Countersunk Screw";
    case FastenerKind::ThreadedRod: return "Threaded Rod";
    case FastenerKind::HexNut: return "Hex Nut";
    case FastenerKind::SquareNut: return "Square Nut";
    case FastenerKind::Washer: return "Washer";
    }
    return "Fastener";
}

const char* fastenerStandard(FastenerKind k) {
    switch (k) {
    case FastenerKind::HexBolt: return "ISO 4017";
    case FastenerKind::SocketCap: return "ISO 4762";
    case FastenerKind::Countersunk: return "ISO 10642";
    case FastenerKind::ThreadedRod: return "DIN 976";
    case FastenerKind::HexNut: return "ISO 4032";
    case FastenerKind::SquareNut: return "DIN 557";
    case FastenerKind::Washer: return "ISO 7089";
    }
    return "";
}

bool fastenerHasLength(FastenerKind k) { return k <= FastenerKind::ThreadedRod; }
bool fastenerHasThread(FastenerKind k) { return k != FastenerKind::Washer; }

namespace {

bool fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

// A closed outline in the XY plane, counter-clockwise, with a flag per point:
// smooth points get soft side edges (round surfaces), the rest drawn ones.
struct Loop2 {
    std::vector<Vec2> p;
    std::vector<char> smooth;
    bool curve = false; // a circle: its edges share a curve id
};

double signedArea(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) a += p[i].cross(p[(i + 1) % p.size()]);
    return a / 2;
}

Loop2 makeLoop(std::vector<Vec2> p, std::vector<char> smooth, bool curve = false) {
    if (smooth.size() != p.size()) smooth.assign(p.size(), 0);
    if (signedArea(p) < 0) {
        std::reverse(p.begin(), p.end());
        std::reverse(smooth.begin(), smooth.end());
    }
    return {std::move(p), std::move(smooth), curve};
}

Loop2 circle(double r, int n, double phase = 0) {
    std::vector<Vec2> p;
    for (int i = 0; i < n; ++i) {
        const double a = phase + 2 * kPi * i / n;
        p.push_back({r * std::cos(a), r * std::sin(a)});
    }
    return makeLoop(std::move(p), std::vector<char>(n, 1), true);
}

// A regular polygon by its across-flats width (flats square to X for an even count).
Loop2 polygonAcrossFlats(int n, double acrossFlats) {
    const double rc = acrossFlats / 2 / std::cos(kPi / n);
    std::vector<Vec2> p;
    for (int i = 0; i < n; ++i) {
        const double a = kPi / n + 2 * kPi * i / n;
        p.push_back({rc * std::cos(a), rc * std::sin(a)});
    }
    return makeLoop(std::move(p), {});
}

// Puts faces into a mesh from outlines at heights; vertices at the same place
// are shared, so pieces built one after the other join into one solid.
class Builder {
public:
    explicit Builder(Mesh& m) : m_(m) {}

    std::vector<Id> ring(const Loop2& L, double z) {
        std::vector<Id> ids;
        for (const Vec2& q : L.p) ids.push_back(m_.addVertex(Vec3(q.x, q.y, z)));
        return ids;
    }

    // A flat face at height z facing up or down, with holes.
    Id cap(const Loop2& outer, const std::vector<const Loop2*>& holes, double z, bool up) {
        std::vector<std::vector<Id>> hs;
        const std::vector<Id> o = ring(outer, z);
        markCurve(outer, o);
        for (const Loop2* h : holes) {
            hs.push_back(ring(*h, z));
            markCurve(*h, hs.back());
        }
        return m_.addFaceRaw(o, hs, Vec3(0, 0, up ? 1 : -1));
    }

    // The side between loop a at za and loop b at zb (same point count):
    // facing away from the loops' inside (or into it, for a hole's wall).
    std::vector<Id> wall(const Loop2& a, double za, const Loop2& b, double zb, bool outward) {
        const std::vector<Id> A = ring(a, za), B = ring(b, zb);
        std::vector<Id> faces;
        const size_t n = A.size();
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const Vec2 t = a.p[j] - a.p[i];
            const Vec3 hint = Vec3(t.y, -t.x, 0) * (outward ? 1.0 : -1.0);
            std::vector<Id> loop = {A[i], A[j], B[j], B[i]};
            loop.erase(std::unique(loop.begin(), loop.end()), loop.end());
            const Id f = m_.addFaceRaw(loop, {}, hint);
            if (f != kNone) faces.push_back(f);
            if (a.smooth[i]) soft_.push_back({A[i], B[i]});
        }
        return faces;
    }

    // Soft edges and circle curves, once all faces are in.
    void finish() {
        for (const auto& [a, b] : soft_) {
            const Id e = m_.findEdge(a, b);
            if (e != kNone) m_.edges[e].soft = m_.edges[e].smooth = true;
        }
        m_.touch();
    }

private:
    void markCurve(const Loop2& L, const std::vector<Id>& ids) {
        if (!L.curve) return;
        const Id c = m_.newCurveId();
        for (size_t i = 0; i < ids.size(); ++i) {
            const Id e = m_.addEdgeRaw(ids[i], ids[(i + 1) % ids.size()]);
            if (e != kNone) m_.edges[e].curve = c;
        }
    }

    Mesh& m_;
    std::vector<std::pair<Id, Id>> soft_;
};

std::string fmtMM(double mm) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", std::round(mm * 100) / 100);
    return buf;
}

} // namespace

bool buildFastener(const FastenerParams& p, Mesh& out, std::string* name, std::string* err) {
    const auto& table = metricSizes();
    if (p.size < 0 || p.size >= (int)table.size()) return fail(err, "Unknown thread size.");
    if (p.segments < 8 || p.segments > 256) return fail(err, "Use 8 to 256 segments.");
    const MetricSize& s = table[p.size];
    const double mm = 0.001;
    const double d = s.d * mm;
    const double P = (p.finePitch ? s.finePitch : s.pitch) * mm;
    const double depth = metricThreadDepth(P);
    const int N = p.segments;
    const double L = p.length;

    std::string label = std::string(fastenerKindName(p.kind)) + " " + s.name;
    if (p.finePitch && fastenerHasThread(p.kind)) label += "×" + fmtMM(s.finePitch);
    if (fastenerHasLength(p.kind)) {
        if (!(L > 0)) return fail(err, "The length must be above zero.");
        if (L < 2 * P) return fail(err, "The length must be at least two turns of the thread (" + fmtMM(2 * P / mm) + " mm).");
        if (L > 2.0) return fail(err, "The length is limited to 2 m.");
        label += " × " + fmtMM(L / mm);
    }

    Builder b(out);
    std::vector<Id> threadFaces; // the round surface to thread
    const Loop2 shank = circle(d / 2, N);
    // The hex socket of socket screws: a pocket in the top of the head.
    auto socket = [&](const Loop2& headTop, double top, double key, double depthT) {
        const Loop2 hex = polygonAcrossFlats(6, key);
        b.cap(headTop, {&hex}, top, true);
        b.wall(hex, top - depthT, hex, top, false);
        b.cap(hex, {}, top - depthT, true);
    };

    switch (p.kind) {
    case FastenerKind::HexBolt: {
        const Loop2 head = polygonAcrossFlats(6, s.nutS * mm);
        const double k = s.headK * mm;
        b.cap(shank, {}, 0, false);
        threadFaces = b.wall(shank, 0, shank, L, true);
        b.cap(head, {&shank}, L, false);
        b.wall(head, L, head, L + k, true);
        b.cap(head, {}, L + k, true);
        break;
    }
    case FastenerKind::SocketCap: {
        const Loop2 head = circle(s.capDk * mm / 2, N);
        const double k = d;
        b.cap(shank, {}, 0, false);
        threadFaces = b.wall(shank, 0, shank, L, true);
        b.cap(head, {&shank}, L, false);
        b.wall(head, L, head, L + k, true);
        socket(head, L + k, s.capKey * mm, 0.5 * k);
        break;
    }
    case FastenerKind::Countersunk: {
        // A 90° cone from (just outside) the shank to the head's diameter; the
        // thin flat ring where it starts lets the thread end on a flat face.
        const double r0 = d / 2 + std::max(0.1 * P, 3 * kTol), r1 = s.cskDk * mm / 2;
        const double k = r1 - r0;
        const double Ls = L - k; // the length is the screw's overall length
        if (Ls < 2 * P)
            return fail(err, "A countersunk screw's length includes its head: at least " + fmtMM((k + 2 * P) / mm) + " mm.");
        const Loop2 lip = circle(r0, N), head = circle(r1, N);
        b.cap(shank, {}, 0, false);
        threadFaces = b.wall(shank, 0, shank, Ls, true);
        b.cap(lip, {&shank}, Ls, false);
        b.wall(lip, Ls, head, Ls + k, true);
        socket(head, Ls + k, s.cskKey * mm, 0.6 * k);
        break;
    }
    case FastenerKind::ThreadedRod:
        b.cap(shank, {}, 0, false);
        threadFaces = b.wall(shank, 0, shank, L, true);
        b.cap(shank, {}, L, true);
        break;
    case FastenerKind::HexNut:
    case FastenerKind::SquareNut: {
        const bool hex = p.kind == FastenerKind::HexNut;
        const Loop2 body = hex ? polygonAcrossFlats(6, s.nutS * mm) : polygonAcrossFlats(4, s.sqS * mm);
        const double h = (hex ? s.nutM : s.sqM) * mm;
        // The hole at the minor diameter; carving the thread widens it to d.
        const Loop2 hole = circle(p.thread ? d / 2 - depth : d / 2, N);
        b.cap(body, {&hole}, 0, false);
        b.wall(body, 0, body, h, true);
        b.cap(body, {&hole}, h, true);
        threadFaces = b.wall(hole, 0, hole, h, false);
        break;
    }
    case FastenerKind::Washer: {
        const Loop2 o = circle(s.washD2 * mm / 2, N), i = circle(s.washD1 * mm / 2, N);
        const double h = s.washH * mm;
        b.cap(o, {&i}, 0, false);
        b.wall(o, 0, o, h, true);
        b.cap(o, {&i}, h, true);
        b.wall(i, 0, i, h, false);
        break;
    }
    }
    b.finish();
    if (out.aliveFaceCount() == 0) return fail(err, "Nothing to build.");

    if (p.thread && fastenerHasThread(p.kind)) {
        ScrewThreadParams tp;
        tp.profile = ScrewThreadParams::Metric;
        tp.pitch = P;
        tp.depth = depth;
        tp.inward = true; // the crest stays on the blank: d on a screw, the minor diameter in a nut
        tp.leftHand = p.leftHand;
        tp.segments = N;
        std::string terr;
        const std::set<Id> fs(threadFaces.begin(), threadFaces.end());
        if (!screwThread(out, fs, tp, nullptr, &terr)) {
            out = Mesh();
            return fail(err, "Could not cut the thread: " + terr);
        }
        if (p.leftHand) label += " LH";
    }
    if (name) *name = label;
    return true;
}

// ---- gears ----------------------------------------------------------------------

const std::vector<double>& standardModules() {
    static const std::vector<double> t = {0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1,  1.125, 1.25, 1.375, 1.5, 1.75, 2, 2.25, 2.5,
                                          2.75, 3, 3.5, 4, 4.5, 5, 5.5, 6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 25};
    return t;
}

namespace {

double involute(double a) { return std::tan(a) - a; }

} // namespace

std::vector<Vec2> spurGearOutline(const GearParams& p, std::vector<char>* smooth, GearInfo* info, std::string* err) {
    std::vector<Vec2> pts;
    std::vector<char> sm;
    const double m = p.module;
    const int z = p.teeth;
    if (!(m > 0)) return fail(err, "The module must be above zero."), pts;
    if (z < 6 || z > 1000) return fail(err, "Use 6 to 1000 teeth."), pts;
    if (!(p.pressureAngle >= 10 && p.pressureAngle <= 35)) return fail(err, "Use a pressure angle of 10° to 35°."), pts;
    const double alpha = p.pressureAngle * kPi / 180;
    const double r = m * z / 2, rb = r * std::cos(alpha), ra = r + m, rf = r - 1.25 * m;
    if (!(rf > 0)) return fail(err, "Too few teeth for this module."), pts;
    // Half the tooth's angular thickness at the pitch circle, less backlash.
    const double psi = (kPi * m / 2 - std::max(0.0, p.backlash)) / (2 * r);
    if (!(psi > 0)) return fail(err, "The backlash is wider than a tooth."), pts;
    // Half-angle of the tooth at radius rho on its involute flank.
    auto half = [&](double rho) { return psi + involute(alpha) - involute(std::acos(std::min(1.0, rb / rho))); };
    if (half(ra) <= 0.02 * kPi / z) return fail(err, "The teeth come to a point: more teeth or a smaller pressure angle."), pts;
    const double rs = std::max(rb, rf); // where the involute starts
    const double gapAtRoot = 2 * kPi / z - 2 * half(rs);
    if (gapAtRoot <= 0) return fail(err, "The teeth run into each other at the root."), pts;
    const int K = std::clamp(p.flankSteps, 1, 64);
    const int R = 3; // pieces of the root arc
    const int T = 2; // pieces of the tip arc
    auto polar = [](double rho, double a) { return Vec2(rho * std::cos(a), rho * std::sin(a)); };
    // Involute points from rs to ra, evenly in the roll angle.
    const double t0 = std::sqrt(std::max(0.0, (rs / rb) * (rs / rb) - 1)), t1 = std::sqrt((ra / rb) * (ra / rb) - 1);
    std::vector<double> rhos;
    for (int k = 0; k <= K; ++k) {
        const double t = t0 + (t1 - t0) * k / K;
        rhos.push_back(rb * std::sqrt(1 + t * t));
    }
    for (int i = 0; i < z; ++i) {
        const double c = 2 * kPi * i / z;
        // Rising flank (the side towards smaller angles), root to tip.
        if (rf < rb) {
            pts.push_back(polar(rf, c - half(rb))); // radial line under the base circle
            sm.push_back(0);
        }
        for (int k = 0; k <= K; ++k) {
            pts.push_back(polar(rhos[k], c - half(rhos[k])));
            // Corners: where the flank leaves the root (unless a radial line
            // leads into it, which meets the involute tangentially) and the tip.
            sm.push_back(k == K ? 0 : k == 0 ? (rf < rb ? 1 : 0) : 1);
        }
        for (int k = 1; k < T; ++k) {
            pts.push_back(polar(ra, c - half(ra) + 2 * half(ra) * k / T));
            sm.push_back(1);
        }
        for (int k = K; k >= 0; --k) {
            pts.push_back(polar(rhos[k], c + half(rhos[k])));
            sm.push_back(k == K ? 0 : k == 0 ? (rf < rb ? 1 : 0) : 1);
        }
        if (rf < rb) {
            pts.push_back(polar(rf, c + half(rb)));
            sm.push_back(0);
        }
        // The root arc to the next tooth.
        const double a0 = c + half(rs), a1 = c + 2 * kPi / z - half(rs);
        for (int k = 1; k < R; ++k) {
            pts.push_back(polar(rf, a0 + (a1 - a0) * k / R));
            sm.push_back(1);
        }
    }
    if (info) {
        info->pitchDiameter = 2 * r;
        info->outsideDiameter = 2 * ra;
        info->rootDiameter = 2 * rf;
        info->baseDiameter = 2 * rb;
        info->pitch = kPi * m;
        // Teeth start being undercut below 2 / sin²α (17 at 20°).
        info->undercut = z < 2 / (std::sin(alpha) * std::sin(alpha));
    }
    if (smooth) *smooth = std::move(sm);
    return pts;
}

bool buildGear(const GearParams& p, Mesh& out, std::string* name, GearInfo* info, std::string* err) {
    if (!(p.faceWidth > 0)) return fail(err, "The face width must be above zero.");
    GearInfo gi;
    const double m = p.module;
    Builder b(out);
    const double w = p.faceWidth;
    std::string label;
    char buf[96];
    if (p.kind == GearKind::Spur) {
        std::vector<char> sm;
        std::string e;
        std::vector<Vec2> pts = spurGearOutline(p, &sm, &gi, &e);
        if (pts.empty()) return fail(err, e);
        const Loop2 outline = makeLoop(std::move(pts), std::move(sm));
        if (p.bore < 0) return fail(err, "The bore cannot be negative.");
        if (p.bore > 0 && p.bore / 2 > gi.rootDiameter / 2 - 0.5 * m)
            return fail(err, "The bore is too large for this gear: under " + fmtMM((gi.rootDiameter - m) * 1000) + " mm.");
        if (p.boreSegments < 8 || p.boreSegments > 256) return fail(err, "Use 8 to 256 bore segments.");
        if (p.bore > 0) {
            const Loop2 bore = circle(p.bore / 2, p.boreSegments);
            b.cap(outline, {&bore}, 0, false);
            b.cap(outline, {&bore}, w, true);
            b.wall(bore, 0, bore, w, false);
        } else {
            b.cap(outline, {}, 0, false);
            b.cap(outline, {}, w, true);
        }
        b.wall(outline, 0, outline, w, true);
        std::snprintf(buf, sizeof buf, "Spur Gear m%g z%d", m * 1000, p.teeth);
        label = buf;
    } else {
        if (!(m > 0)) return fail(err, "The module must be above zero.");
        if (p.teeth < 1 || p.teeth > 2000) return fail(err, "Use 1 to 2000 teeth.");
        if (!(p.pressureAngle >= 10 && p.pressureAngle <= 35)) return fail(err, "Use a pressure angle of 10° to 35°.");
        const double alpha = p.pressureAngle * kPi / 180, ta = std::tan(alpha);
        const double pitch = kPi * m;
        const double hp = p.rackHeight > 0 ? p.rackHeight : 3 * m; // bottom to pitch line
        if (hp <= 1.25 * m + 1e-9) return fail(err, "The rack must reach more than 1.25 × module below the pitch line.");
        const double half = (pitch / 2 - std::max(0.0, p.backlash)) / 2; // tooth half-thickness at the pitch line
        const double tip = half - m * ta, root = half + 1.25 * m * ta;
        if (tip <= 0.02 * m) return fail(err, "The teeth come to a point: a smaller pressure angle.");
        if (root >= pitch / 2) return fail(err, "The teeth run into each other at the root: a smaller pressure angle.");
        const int n = p.teeth;
        const double len = n * pitch, x0 = -len / 2;
        // Pitch line at y = 0: teeth up to +m, root at -1.25 m, bottom at -hp.
        std::vector<Vec2> top; // left to right along the toothed side
        top.push_back({x0, -1.25 * m});
        for (int i = 0; i < n; ++i) {
            const double c = x0 + (i + 0.5) * pitch;
            top.push_back({c - root, -1.25 * m});
            top.push_back({c - tip, m});
            top.push_back({c + tip, m});
            top.push_back({c + root, -1.25 * m});
        }
        top.push_back({x0 + len, -1.25 * m});
        std::vector<Vec2> pts = {{x0, -hp}, {x0 + len, -hp}};
        for (auto it = top.rbegin(); it != top.rend(); ++it) pts.push_back(*it);
        // Shift so the rack's middle sits on the origin.
        const double yMid = (m - hp) / 2;
        for (Vec2& q : pts) q.y -= yMid;
        const Loop2 outline = makeLoop(std::move(pts), {});
        b.cap(outline, {}, 0, false);
        b.cap(outline, {}, w, true);
        b.wall(outline, 0, outline, w, true);
        gi.pitch = pitch;
        gi.length = len;
        std::snprintf(buf, sizeof buf, "Gear Rack m%g × %d", m * 1000, n);
        label = buf;
    }
    b.finish();
    out.removeDegenerateFaces();
    if (out.aliveFaceCount() == 0) return fail(err, "Nothing to build.");
    if (info) *info = gi;
    if (name) *name = label;
    return true;
}

} // namespace gs
